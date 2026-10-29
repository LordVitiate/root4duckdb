#include "root4duckdb/serialized/root_serialized_codec.hpp"

#include "root4duckdb/serialized/root_serialized_codec_utils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duckdb::rootlake
{

namespace
{

bool ValidatePrimitiveLayout(const SerializedEntryLayout& layout, size_t expected_index_depth,
                             SerializedPrimitiveKind& primitive_kind, std::string& failure_reason)
{
    if (!layout.value_bytes || !layout.fixed_array_length || !layout.index_depth)
    {
        failure_reason = "serialized entry layout is incomplete";
        return false;
    }
    primitive_kind = layout.primitive_kind == SerializedPrimitiveKind::UNKNOWN
                         ? ClassifySerializedPrimitive(layout.value_type)
                         : layout.primitive_kind;
    if (primitive_kind == SerializedPrimitiveKind::UNKNOWN ||
        serialized_codec::PrimitiveWidth(primitive_kind) != layout.value_bytes)
    {
        failure_reason = "serialized primitive type and width are inconsistent";
        return false;
    }
    if (layout.index_depth != expected_index_depth)
    {
        failure_reason = "serialized entry index shape is inconsistent";
        return false;
    }
    return true;
}

void PushArrayCoordinates(uint64_t flat, const std::vector<uint32_t>& dimensions, std::vector<int32_t>& indices)
{
    if (dimensions.empty())
    {
        indices.push_back(static_cast<int32_t>(flat));
        return;
    }
    const auto old_size = indices.size();
    indices.resize(old_size + dimensions.size());
    for (size_t reverse = dimensions.size(); reverse > 0; --reverse)
    {
        const size_t dim = reverse - 1;
        indices[old_size + dim] = static_cast<int32_t>(flat % dimensions[dim]);
        flat /= dimensions[dim];
    }
}

} // namespace

SerializedPrimitiveKind ClassifySerializedPrimitive(const std::string& raw_type)
{
    const auto type = serialized_codec::NormalizePrimitiveType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O")
    {
        return SerializedPrimitiveKind::BOOL;
    }
    if (type == "Char_t" || type == "char" || type == "b")
    {
        return SerializedPrimitiveKind::INT8;
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B")
    {
        return SerializedPrimitiveKind::UINT8;
    }
    if (type == "Short_t" || type == "short" || type == "S")
    {
        return SerializedPrimitiveKind::INT16;
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s")
    {
        return SerializedPrimitiveKind::UINT16;
    }
    if (type == "Int_t" || type == "int" || type == "I")
    {
        return SerializedPrimitiveKind::INT32;
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i")
    {
        return SerializedPrimitiveKind::UINT32;
    }
    if (type == "Long64_t" || type == "long long" || type == "L")
    {
        return SerializedPrimitiveKind::INT64;
    }
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l")
    {
        return SerializedPrimitiveKind::UINT64;
    }
    if (type == "Float_t" || type == "float" || type == "F")
    {
        return SerializedPrimitiveKind::FLOAT32;
    }
    if (type == "Double_t" || type == "double" || type == "D")
    {
        return SerializedPrimitiveKind::FLOAT64;
    }
    return SerializedPrimitiveKind::UNKNOWN;
}

template <class VALUE>
bool DecodeVectorPayload(const uint8_t* bytes, size_t entry_size, size_t payload_offset, uint64_t count,
                         const SerializedEntryLayout& layout, uint64_t max_values_per_entry, std::vector<VALUE>& values,
                         std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    if (!bytes || payload_offset > entry_size)
    {
        failure_reason = "serialized vector payload starts outside its entry";
        return false;
    }
    const size_t expected_index_depth =
        1 +
        (layout.fixed_array_length > 1 ? (layout.array_dimensions.empty() ? 1 : layout.array_dimensions.size()) : 0);
    SerializedPrimitiveKind primitive_kind = SerializedPrimitiveKind::UNKNOWN;
    if (!ValidatePrimitiveLayout(layout, expected_index_depth, primitive_kind, failure_reason))
    {
        return false;
    }
    uint64_t dimensions_product = 1;
    for (const auto dimension : layout.array_dimensions)
    {
        if (!dimension || dimension > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            dimensions_product > std::numeric_limits<uint64_t>::max() / dimension)
        {
            failure_reason = "serialized fixed-array dimensions are invalid";
            return false;
        }
        dimensions_product *= dimension;
    }
    if (!layout.array_dimensions.empty() && dimensions_product != layout.fixed_array_length)
    {
        failure_reason = "serialized fixed-array dimensions do not match its length";
        return false;
    }
    if (count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || count > max_values_per_entry ||
        count > max_values_per_entry / layout.fixed_array_length)
    {
        failure_reason = "serialized vector element count exceeds configured safety limit";
        return false;
    }
    if ((layout.bytes_before_value_per_element &&
         count > std::numeric_limits<uint64_t>::max() / layout.bytes_before_value_per_element) ||
        count > std::numeric_limits<uint64_t>::max() / layout.fixed_array_length)
    {
        failure_reason = "serialized vector size arithmetic overflow";
        return false;
    }
    const uint64_t prefix_bytes = count * layout.bytes_before_value_per_element;
    const uint64_t value_count = count * layout.fixed_array_length;
    if (value_count > std::numeric_limits<uint64_t>::max() / layout.value_bytes ||
        value_count > std::numeric_limits<size_t>::max() / layout.index_depth)
    {
        failure_reason = "serialized projected member size arithmetic overflow";
        return false;
    }
    const uint64_t projected_bytes = value_count * layout.value_bytes;
    if (prefix_bytes > entry_size - payload_offset || projected_bytes > entry_size - payload_offset - prefix_bytes)
    {
        failure_reason = "projected member range exceeds serialized entry";
        return false;
    }
    const uint8_t* projected = bytes + payload_offset + prefix_bytes;
    values.reserve(static_cast<size_t>(value_count));
    if (collect_indices)
    {
        flat_indices.reserve(static_cast<size_t>(value_count * layout.index_depth));
    }
    for (uint64_t element = 0; element < count; ++element)
    {
        for (uint64_t array_index = 0; array_index < layout.fixed_array_length; ++array_index)
        {
            const uint64_t flat = element * layout.fixed_array_length + array_index;
            VALUE value{};
            const bool decoded =
                serialized_codec::DecodePrimitive(projected + flat * layout.value_bytes, primitive_kind, value);
            if (!decoded)
            {
                failure_reason = "unsupported primitive decoder for " + layout.value_type;
                values.clear();
                flat_indices.clear();
                return false;
            }
            values.push_back(value);
            if (collect_indices)
            {
                flat_indices.push_back(static_cast<int32_t>(element));
                if (layout.fixed_array_length > 1)
                {
                    PushArrayCoordinates(array_index, layout.array_dimensions, flat_indices);
                }
            }
        }
    }
    return true;
}

bool SerializedVectorPageCursor::Open(const uint8_t* bytes, size_t entry_size, size_t payload_offset, uint64_t count,
                                      const SerializedEntryLayout& source_layout, std::string& reason)
{
    Reset();
    reason.clear();
    if (!bytes || payload_offset > entry_size || !source_layout.fixed_array_length || !source_layout.value_bytes)
    {
        reason = "serialized cursor has an invalid entry or primitive width";
        return false;
    }
    const auto rank = source_layout.fixed_array_length > 1
                          ? (source_layout.array_dimensions.empty() ? 1 : source_layout.array_dimensions.size())
                          : 0;
    SerializedPrimitiveKind checked_kind;
    if (!ValidatePrimitiveLayout(source_layout, 1 + rank, checked_kind, reason))
    {
        return false;
    }
    uint64_t product = 1;
    for (const auto dimension : source_layout.array_dimensions)
    {
        if (!dimension || dimension > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            product > std::numeric_limits<uint64_t>::max() / dimension)
        {
            reason = "serialized cursor has invalid array dimensions";
            return false;
        }
        product *= dimension;
    }
    if ((!source_layout.array_dimensions.empty() && product != source_layout.fixed_array_length) ||
        (source_layout.array_dimensions.empty() &&
         source_layout.fixed_array_length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())))
    {
        reason = "serialized cursor array dimensions exceed supported indices";
        return false;
    }
    if (count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        count > std::numeric_limits<uint64_t>::max() / source_layout.fixed_array_length ||
        (source_layout.bytes_before_value_per_element &&
         count > std::numeric_limits<uint64_t>::max() / source_layout.bytes_before_value_per_element))
    {
        reason = "serialized cursor element count or prefix overflows";
        return false;
    }
    const auto total = count * source_layout.fixed_array_length;
    const auto prefix = count * source_layout.bytes_before_value_per_element;
    if (total > std::numeric_limits<uint64_t>::max() / source_layout.value_bytes ||
        prefix > entry_size - payload_offset)
    {
        reason = "serialized cursor projected byte count overflows";
        return false;
    }
    if (total * source_layout.value_bytes > entry_size - payload_offset - prefix)
    {
        reason = "serialized cursor payload exceeds its entry frame";
        return false;
    }
    projected = bytes + payload_offset + static_cast<size_t>(prefix);
    layout = source_layout;
    kind = checked_kind;
    value_count = total;
    return true;
}

bool SerializedVectorPageCursor::ReadPage(size_t maximum_rows, std::vector<RootPrimitiveValue>& values,
                                          std::vector<int32_t>& flat_indices, std::string& reason, bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    reason.clear();
    if (!HasMore() || !maximum_rows)
    {
        return false;
    }
    // STANDARD_VECTOR_SIZE = 2048; reject accidental entry-sized page requests.
    const auto rows =
        static_cast<size_t>(std::min<uint64_t>(value_count - next_value, std::min<size_t>(maximum_rows, 2048)));
    if (collect_indices && rows > flat_indices.max_size() / layout.index_depth)
    {
        reason = "serialized cursor index page exceeds vector capacity";
        return false;
    }
    values.reserve(rows);
    if (collect_indices)
    {
        flat_indices.reserve(rows * layout.index_depth);
    }
    for (size_t i = 0; i < rows; ++i)
    {
        const auto flat = next_value + i;
        RootPrimitiveValue value;
        if (!serialized_codec::DecodePrimitive(projected + flat * layout.value_bytes, kind, value))
        {
            values.clear();
            flat_indices.clear();
            reason = "serialized cursor primitive decode failed";
            return false;
        }
        values.push_back(value);
        if (collect_indices)
        {
            flat_indices.push_back(static_cast<int32_t>(flat / layout.fixed_array_length));
            if (layout.fixed_array_length > 1)
            {
                PushArrayCoordinates(flat % layout.fixed_array_length, layout.array_dimensions, flat_indices);
            }
        }
    }
    next_value += rows;
    return true;
}

bool SerializedVectorPageCursor::HasMore() const noexcept
{
    return projected && next_value < value_count;
}

void SerializedVectorPageCursor::Reset() noexcept
{
    projected = nullptr;
    layout = {};
    kind = SerializedPrimitiveKind::UNKNOWN;
    value_count = 0;
    next_value = 0;
}

template <class VALUE>
bool DecodeVectorEntry(const uint8_t* bytes, size_t entry_size, const SerializedEntryLayout& layout,
                       uint64_t max_values_per_entry, uint32_t& observed_memberwise_header, std::vector<VALUE>& values,
                       std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    serialized_codec::CheckedByteCursor cursor(bytes, entry_size);
    uint32_t byte_count = 0;
    uint32_t memberwise_header = 0;
    uint32_t count = 0;
    if (!cursor.ReadBE32(byte_count) || !cursor.ReadBE32(memberwise_header) || !cursor.ReadBE32(count))
    {
        failure_reason = "serialized vector entry is shorter than its header";
        return false;
    }
    if ((byte_count & serialized_codec::ROOT_BYTE_COUNT_MASK) == 0)
    {
        failure_reason = "serialized vector entry has no ROOT byte-count marker";
        return false;
    }
    const uint64_t declared_size = static_cast<uint64_t>(byte_count & serialized_codec::ROOT_BYTE_COUNT_VALUE_MASK) + 4;
    if (declared_size != entry_size)
    {
        failure_reason = "serialized vector byte-count does not match entry offsets";
        return false;
    }
    if (!memberwise_header)
    {
        failure_reason = "serialized vector has an empty member-wise header";
        return false;
    }
    if (!observed_memberwise_header)
    {
        observed_memberwise_header = memberwise_header;
    }
    if (memberwise_header != observed_memberwise_header)
    {
        failure_reason = "member-wise streamer header changed inside one physical branch";
        return false;
    }
    return DecodeVectorPayload(bytes, entry_size, cursor.Offset(), count, layout, max_values_per_entry, values,
                               flat_indices, failure_reason, collect_indices);
}

bool DecodeSerializedVectorEntry(const uint8_t* bytes, size_t entry_size, const SerializedEntryLayout& layout,
                                 uint64_t max_values_per_entry, uint32_t& observed_memberwise_header,
                                 std::vector<double>& values, std::vector<int32_t>& flat_indices,
                                 std::string& failure_reason, bool collect_indices)
{
    return DecodeVectorEntry(bytes, entry_size, layout, max_values_per_entry, observed_memberwise_header, values,
                             flat_indices, failure_reason, collect_indices);
}

bool DecodeSerializedVectorPayload(const uint8_t* bytes, size_t entry_size, size_t payload_offset, uint64_t count,
                                   const SerializedEntryLayout& layout, uint64_t max_values_per_entry,
                                   std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                   std::string& failure_reason, bool collect_indices)
{
    return DecodeVectorPayload(bytes, entry_size, payload_offset, count, layout, max_values_per_entry, values,
                               flat_indices, failure_reason, collect_indices);
}

bool DecodeSerializedVectorEntry(const uint8_t* bytes, size_t entry_size, const SerializedEntryLayout& layout,
                                 uint64_t max_values_per_entry, uint32_t& observed_memberwise_header,
                                 std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                 std::string& failure_reason, bool collect_indices)
{
    return DecodeVectorEntry(bytes, entry_size, layout, max_values_per_entry, observed_memberwise_header, values,
                             flat_indices, failure_reason, collect_indices);
}

bool EqualDecodedValues(const std::vector<double>& left, const std::vector<int32_t>& left_indices,
                        const std::vector<double>& right, const std::vector<int32_t>& right_indices)
{
    if (left_indices != right_indices || left.size() != right.size())
    {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (left[i] == right[i])
        {
            continue;
        }
        if (std::isnan(left[i]) && std::isnan(right[i]))
        {
            continue;
        }
        return false;
    }
    return true;
}

bool EqualDecodedValues(const std::vector<RootPrimitiveValue>& left, const std::vector<int32_t>& left_indices,
                        const std::vector<RootPrimitiveValue>& right, const std::vector<int32_t>& right_indices)
{
    if (left_indices != right_indices || left.size() != right.size())
    {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (left[i].kind != right[i].kind)
        {
            return false;
        }
        switch (left[i].kind)
        {
        case RootPrimitiveKind::SIGNED:
            if (left[i].signed_value != right[i].signed_value)
            {
                return false;
            }
            break;
        case RootPrimitiveKind::UNSIGNED:
            if (left[i].unsigned_value != right[i].unsigned_value)
            {
                return false;
            }
            break;
        case RootPrimitiveKind::FLOATING:
            if (left[i].floating_value != right[i].floating_value &&
                !(std::isnan(left[i].floating_value) && std::isnan(right[i].floating_value)))
            {
                return false;
            }
            break;
        }
    }
    return true;
}

} // namespace duckdb::rootlake
