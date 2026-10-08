#include "root4duckdb/serialized/root_serialized_codec.hpp"

#include "root4duckdb/serialized/root_serialized_codec_utils.hpp"
#include "root4duckdb/serialized/root_serialized_reader.hpp"
#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/core/root_debug.hpp"

#include <algorithm>
#include <limits>

namespace duckdb::rootlake
{

namespace
{

bool ValidateLayout(const SerializedEntryLayout& layout, SerializedPrimitiveKind& kind, std::string& reason)
{
    kind = layout.primitive_kind == SerializedPrimitiveKind::UNKNOWN ? ClassifySerializedPrimitive(layout.value_type)
                                                                     : layout.primitive_kind;
    if (!layout.value_bytes || kind == SerializedPrimitiveKind::UNKNOWN ||
        serialized_codec::PrimitiveWidth(kind) != layout.value_bytes)
    {
        reason = "serialized primitive type and width are inconsistent";
        return false;
    }
    if (layout.fixed_array_length != 1 || !layout.array_dimensions.empty() || layout.index_depth != 2)
    {
        reason = "nested serialized vector requires scalar primitives and two indices";
        return false;
    }
    return true;
}

template <class VALUE>
void Fail(std::vector<VALUE>& values, std::vector<int32_t>& indices, std::string& reason, const char* message)
{
    values.clear();
    indices.clear();
    reason = message;
}

} // namespace

template <class VALUE>
bool DecodeNestedPrimitiveColumn(const uint8_t* bytes, size_t entry_size, size_t column_offset, uint64_t outer_count,
                                 const SerializedEntryLayout& layout, uint64_t max_values_per_entry,
                                 std::vector<VALUE>& values, std::vector<int32_t>& flat_indices,
                                 std::string& failure_reason, bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    SerializedPrimitiveKind kind = SerializedPrimitiveKind::UNKNOWN;
    if (!ValidateLayout(layout, kind, failure_reason))
    {
        return false;
    }
    if (!bytes || column_offset > entry_size)
    {
        failure_reason = "serialized nested vector column is shorter than its ROOT header";
        return false;
    }
    if (outer_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || outer_count > max_values_per_entry)
    {
        failure_reason = "serialized outer vector count exceeds configured safety limit";
        return false;
    }
    serialized_codec::CheckedByteCursor entry_cursor(bytes + column_offset, entry_size - column_offset);
    uint32_t byte_count = 0;
    if (!entry_cursor.ReadBE32(byte_count))
    {
        failure_reason = "serialized nested vector column is shorter than its ROOT header";
        return false;
    }
    if ((byte_count & serialized_codec::ROOT_BYTE_COUNT_MASK) == 0)
    {
        failure_reason = "serialized nested vector column has no ROOT byte-count marker";
        return false;
    }
    const uint64_t declared_size = (byte_count & serialized_codec::ROOT_BYTE_COUNT_VALUE_MASK) + 4ULL;
    if (declared_size < 6)
    {
        failure_reason = "serialized nested vector column byte-count is shorter than its ROOT header";
        return false;
    }
    if (declared_size > entry_size - column_offset)
    {
        failure_reason = "serialized nested vector column byte-count exceeds its entry";
        return false;
    }
    serialized_codec::CheckedByteCursor column_cursor(bytes + column_offset + 4,
                                                      static_cast<size_t>(declared_size - 4));
    uint16_t version = 0;
    if (!column_cursor.ReadBE16(version))
    {
        failure_reason = "serialized nested vector column version is truncated";
        return false;
    }
    if (version == 0)
    {
        if (!column_cursor.Skip(4))
        {
            failure_reason = "serialized nested vector column checksum is truncated";
            return false;
        }
    }
    uint64_t total_values = 0;
    for (uint64_t outer = 0; outer < outer_count; ++outer)
    {
        uint32_t encoded_count = 0;
        if (!column_cursor.ReadBE32(encoded_count))
        {
            Fail(values, flat_indices, failure_reason, "serialized nested vector length is truncated");
            return false;
        }
        const int32_t signed_count = static_cast<int32_t>(encoded_count);
        if (signed_count < 0)
        {
            Fail(values, flat_indices, failure_reason, "serialized nested vector has a negative element count");
            return false;
        }
        const uint64_t inner_count = static_cast<uint32_t>(signed_count);
        if (inner_count > max_values_per_entry - total_values ||
            inner_count > std::numeric_limits<uint64_t>::max() / layout.value_bytes ||
            total_values + inner_count > std::numeric_limits<size_t>::max())
        {
            Fail(values, flat_indices, failure_reason,
                 "serialized nested vector value count exceeds configured safety limit");
            return false;
        }
        const uint64_t payload_bytes = inner_count * layout.value_bytes;
        if (payload_bytes > std::numeric_limits<size_t>::max())
        {
            Fail(values, flat_indices, failure_reason, "serialized nested vector payload size overflows size_t");
            return false;
        }
        const uint8_t* payload = nullptr;
        if (!column_cursor.Take(static_cast<size_t>(payload_bytes), payload))
        {
            Fail(values, flat_indices, failure_reason, "serialized nested vector payload is truncated");
            return false;
        }
        if (collect_indices && inner_count > (std::numeric_limits<size_t>::max() - flat_indices.size()) / 2)
        {
            Fail(values, flat_indices, failure_reason, "serialized nested vector index size arithmetic overflow");
            return false;
        }
        values.reserve(static_cast<size_t>(total_values + inner_count));
        if (collect_indices)
        {
            flat_indices.reserve(flat_indices.size() + inner_count * 2);
        }
        for (uint64_t inner = 0; inner < inner_count; ++inner)
        {
            VALUE value{};
            const bool decoded = serialized_codec::DecodePrimitive(payload + inner * layout.value_bytes, kind, value);
            if (!decoded)
            {
                Fail(values, flat_indices, failure_reason,
                     "unsupported primitive decoder for nested serialized vector");
                return false;
            }
            values.push_back(value);
            if (collect_indices)
            {
                flat_indices.push_back(static_cast<int32_t>(outer));
                flat_indices.push_back(static_cast<int32_t>(inner));
            }
        }
        total_values += inner_count;
    }
    if (column_cursor.Remaining() != 0)
    {
        Fail(values, flat_indices, failure_reason,
             "serialized nested vector column byte-count does not match decoded values");
        return false;
    }
    return true;
}

bool DecodeSerializedNestedPrimitiveVectorColumn(const uint8_t* bytes, size_t entry_size, size_t column_offset,
                                                 uint64_t outer_count, const SerializedEntryLayout& layout,
                                                 uint64_t max_values_per_entry, std::vector<double>& values,
                                                 std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                                 bool collect_indices)
{
    return DecodeNestedPrimitiveColumn(bytes, entry_size, column_offset, outer_count, layout, max_values_per_entry,
                                       values, flat_indices, failure_reason, collect_indices);
}

bool DecodeSerializedNestedPrimitiveVectorColumn(const uint8_t* bytes, size_t entry_size, size_t column_offset,
                                                 uint64_t outer_count, const SerializedEntryLayout& layout,
                                                 uint64_t max_values_per_entry, std::vector<RootPrimitiveValue>& values,
                                                 std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                                 bool collect_indices)
{
    return DecodeNestedPrimitiveColumn(bytes, entry_size, column_offset, outer_count, layout, max_values_per_entry,
                                       values, flat_indices, failure_reason, collect_indices);
}


bool DecodeSerializedNestedFixedColumn(TBufferFile& buffer, const SerializedReadPlan& plan, size_t entry_size,
                                       uint64_t outer_count, uint64_t max_values_per_entry,
                                       std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                       std::string& failure_reason, bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    if (!plan.Is(SerializedProjectionKind::NESTED_FIXED_MEMBER) || !plan.nested_container_class ||
        !plan.nested_element_class || !plan.value_bytes || !plan.fixed_array_length ||
        outer_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    {
        failure_reason = "nested offset projection has invalid class or primitive metadata";
        return false;
    }
    const auto primitive_kind = ClassifySerializedPrimitive(plan.value_type);
    if (primitive_kind == SerializedPrimitiveKind::UNKNOWN)
    {
        failure_reason = "nested offset projection uses an unsupported primitive";
        return false;
    }
    int32_t cached_version = -1;
    SerializedEntryLayout inner_layout;
    for (uint64_t outer = 0; outer < outer_count; ++outer)
    {
        const Int_t current_position = buffer.Length();
        if (current_position < 0 || static_cast<uint64_t>(current_position) > entry_size ||
            entry_size - static_cast<uint64_t>(current_position) < 6)
        {
            failure_reason = "nested STL header exceeds the serialized entry";
            values.clear();
            flat_indices.clear();
            return false;
        }
        const auto* raw = reinterpret_cast<const uint8_t*>(buffer.Buffer()) + current_position;
        const auto raw_byte_count = serialized_codec::ReadBE32(raw);
        const auto raw_version = serialized_codec::ReadBE16(raw + 4);
        UInt_t start = 0;
        UInt_t byte_count = 0;
        const Version_t version = buffer.ReadVersion(&start, &byte_count, plan.nested_container_class);
        const Int_t decoded_position = buffer.Length();
        const uint64_t nested_end = static_cast<uint64_t>(start) + byte_count + 4;
        if (outer == 0)
        {
            RootDebug("SERIALIZED.NESTED_FIXED_HEADER",
                      "path=" + plan.logical_path +
                          " cursor=" + std::to_string(current_position) +
                          " raw_byte_count=" + std::to_string(raw_byte_count) +
                          " raw_version=" + std::to_string(raw_version) +
                          " version=" + std::to_string(version) +
                          " start=" + std::to_string(start) +
                          " byte_count=" + std::to_string(byte_count) +
                          " header_end=" + std::to_string(decoded_position) +
                          " frame_end=" + std::to_string(nested_end) +
                          " entry_size=" + std::to_string(entry_size) +
                          " outer_count=" + std::to_string(outer_count));
        }
        const bool memberwise = (version & TBufferFile::kStreamedMemberWise) != 0;
        const auto stl_version = static_cast<uint16_t>(version & ~TBufferFile::kStreamedMemberWise);
        if (!memberwise)
        {
            failure_reason = "nested STL uses object-wise encoding (version=" + std::to_string(stl_version) + ")";
        }
        else if (stl_version < 8)
        {
            failure_reason = "nested STL uses legacy member-wise encoding (version=" +
                             std::to_string(stl_version) + ")";
        }
        else if (!byte_count || decoded_position < 0 || nested_end > entry_size ||
                 nested_end < static_cast<uint64_t>(decoded_position))
        {
            failure_reason = "nested STL has inconsistent framed byte count (version=" +
                             std::to_string(stl_version) + ", frame_end=" +
                             std::to_string(nested_end) + ", entry_size=" +
                             std::to_string(entry_size) + ")";
        }
        if (!failure_reason.empty())
        {
            values.clear();
            flat_indices.clear();
            return false;
        }
        const Version_t element_version = buffer.ReadVersionForMemberWise(plan.nested_element_class);
        if (cached_version != element_version)
        {
            if (!ResolveSerializedNestedFixedLayout(plan, element_version, inner_layout, failure_reason))
            {
                values.clear();
                flat_indices.clear();
                return false;
            }
            cached_version = element_version;
        }
        Int_t signed_count = -1;
        buffer.ReadInt(signed_count);
        if (signed_count < 0)
        {
            failure_reason = "nested member-wise STL has a negative value count";
            values.clear();
            flat_indices.clear();
            return false;
        }
        const uint64_t count = static_cast<uint32_t>(signed_count);
        if (count > max_values_per_entry / plan.fixed_array_length ||
            count * plan.fixed_array_length > max_values_per_entry - values.size() ||
            (inner_layout.bytes_before_value_per_element > 0 &&
             count > std::numeric_limits<uint64_t>::max() / inner_layout.bytes_before_value_per_element))
        {
            failure_reason = "nested fixed value count or prefix exceeds safety limits";
            values.clear();
            flat_indices.clear();
            return false;
        }
        const uint64_t value_count = count * plan.fixed_array_length;
        const uint64_t prefix_bytes = count * inner_layout.bytes_before_value_per_element;
        if (value_count > std::numeric_limits<uint64_t>::max() / inner_layout.value_bytes)
        {
            failure_reason = "nested member value range overflows";
            values.clear();
            flat_indices.clear();
            return false;
        }
        const uint64_t value_bytes = value_count * inner_layout.value_bytes;
        const uint64_t cursor = static_cast<uint64_t>(buffer.Length());
        if (cursor > nested_end || prefix_bytes > nested_end - cursor ||
            value_bytes > nested_end - cursor - prefix_bytes)
        {
            failure_reason = "nested fixed member lies outside its ROOT STL frame";
            values.clear();
            flat_indices.clear();
            return false;
        }
        const auto* payload = reinterpret_cast<const uint8_t*>(buffer.Buffer()) + cursor + prefix_bytes;
        for (uint64_t inner = 0; inner < count; ++inner)
        {
            for (uint64_t index = 0; index < plan.fixed_array_length; ++index)
            {
                RootPrimitiveValue value;
                if (!serialized_codec::DecodePrimitive(payload + (inner * plan.fixed_array_length + index) *
                                                                  inner_layout.value_bytes, primitive_kind, value))
                {
                    failure_reason = "nested fixed member has unsupported primitive encoding";
                    values.clear();
                    flat_indices.clear();
                    return false;
                }
                values.push_back(value);
                if (collect_indices)
                {
                    flat_indices.push_back(static_cast<int32_t>(outer));
                    flat_indices.push_back(static_cast<int32_t>(inner));
                    if (plan.fixed_array_length > 1)
                    {
                        uint64_t remainder = index;
                        const auto offset = flat_indices.size();
                        const auto rank = std::max<size_t>(1, plan.array_dimensions.size());
                        flat_indices.resize(offset + rank);
                        if (plan.array_dimensions.empty())
                        {
                            flat_indices[offset] = static_cast<int32_t>(index);
                        }
                        else
                        {
                            for (size_t reverse = rank; reverse > 0; --reverse)
                            {
                                const size_t dim = reverse - 1;
                                flat_indices[offset + dim] =
                                    static_cast<int32_t>(remainder % plan.array_dimensions[dim]);
                                remainder /= plan.array_dimensions[dim];
                            }
                        }
                    }
                }
            }
        }
        buffer.SetBufferOffset(static_cast<Int_t>(nested_end));
    }
    if (collect_indices && flat_indices.size() != values.size() * plan.index_depth)
    {
        failure_reason = "nested offset projection produced inconsistent collection indices";
        values.clear();
        flat_indices.clear();
        return false;
    }
    return true;
}

} // namespace duckdb::rootlake
