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


namespace
{

bool AppendNestedFixedValues(const uint8_t* bytes, size_t frame_end, uint64_t payload_offset,
                             uint64_t outer, uint64_t inner, const SerializedReadPlan& plan,
                             const SerializedEntryLayout& layout, SerializedPrimitiveKind primitive_kind,
                             uint64_t max_values_per_entry, std::vector<RootPrimitiveValue>& values,
                             std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices)
{
    if (!layout.value_bytes || !plan.fixed_array_length ||
        plan.fixed_array_length > max_values_per_entry - values.size() ||
        plan.fixed_array_length > std::numeric_limits<uint64_t>::max() / layout.value_bytes ||
        payload_offset > frame_end ||
        plan.fixed_array_length * layout.value_bytes > frame_end - payload_offset ||
        outer > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        inner > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    {
        failure_reason = "nested fixed member exceeds framed object bounds or value limits";
        return false;
    }
    for (uint64_t element = 0; element < plan.fixed_array_length; ++element)
    {
        RootPrimitiveValue value;
        const auto* pointer = bytes + payload_offset + element * layout.value_bytes;
        if (!serialized_codec::DecodePrimitive(pointer, primitive_kind, value))
        {
            failure_reason = "nested fixed member has unsupported primitive encoding";
            return false;
        }
        values.push_back(value);
        if (!collect_indices)
        {
            continue;
        }
        flat_indices.push_back(static_cast<int32_t>(outer));
        flat_indices.push_back(static_cast<int32_t>(inner));
        if (plan.fixed_array_length == 1)
        {
            continue;
        }
        const size_t rank = plan.array_dimensions.size();
        if (!rank)
        {
            flat_indices.push_back(static_cast<int32_t>(element));
            continue;
        }
        const size_t offset = flat_indices.size();
        flat_indices.resize(offset + rank);
        uint64_t remainder = element;
        for (size_t index = rank; index > 0; --index)
        {
            const size_t dimension = index - 1;
            const auto extent = plan.array_dimensions[dimension];
            if (!extent)
            {
                failure_reason = "nested fixed member has a zero array dimension";
                return false;
            }
            flat_indices[offset + dimension] = static_cast<int32_t>(remainder % extent);
            remainder /= extent;
        }
    }
    return true;
}

bool DecodeNestedObjectWise(TBufferFile& buffer, const SerializedReadPlan& plan, uint64_t frame_end,
                            uint64_t outer, uint64_t max_values_per_entry,
                            SerializedPrimitiveKind primitive_kind, std::vector<RootPrimitiveValue>& values,
                            std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices)
{
    const Int_t count_position = buffer.Length();
    if (count_position < 0 || static_cast<uint64_t>(count_position) > frame_end ||
        frame_end - static_cast<uint64_t>(count_position) < 4)
    {
        failure_reason = "object-wise nested STL count is outside its frame";
        return false;
    }
    Int_t signed_count = -1;
    buffer.ReadInt(signed_count);
    if (signed_count < 0 || static_cast<uint64_t>(signed_count) > max_values_per_entry / plan.fixed_array_length ||
        static_cast<uint64_t>(signed_count) * plan.fixed_array_length > max_values_per_entry - values.size())
    {
        failure_reason = "object-wise nested STL count exceeds configured value limits";
        return false;
    }
    const uint64_t count = static_cast<uint32_t>(signed_count);
    int32_t cached_version = -1;
    SerializedEntryLayout object_layout;
    for (uint64_t inner = 0; inner < count; ++inner)
    {
        const Int_t position = buffer.Length();
        if (position < 0 || static_cast<uint64_t>(position) > frame_end ||
            frame_end - static_cast<uint64_t>(position) < 6)
        {
            failure_reason = "object-wise nested element has no bounded class header";
            return false;
        }
        UInt_t object_start = 0;
        UInt_t object_byte_count = 0;
        const Version_t object_version = buffer.ReadVersion(&object_start, &object_byte_count,
                                                             plan.nested_element_class);
        const Int_t value_start = buffer.Length();
        const uint64_t object_end = static_cast<uint64_t>(object_start) + object_byte_count + 4;
        if (!object_byte_count || object_version < 0 || value_start < 0 ||
            object_end > frame_end || object_end < static_cast<uint64_t>(value_start))
        {
            failure_reason = "object-wise nested element has an invalid class byte count or version";
            return false;
        }
        if (cached_version != object_version)
        {
            if (!ResolveSerializedNestedFixedLayout(plan, object_version, object_layout, failure_reason))
            {
                return false;
            }
            cached_version = object_version;
        }
        const uint64_t prefix = object_layout.bytes_before_value_per_element;
        if (prefix > object_end - static_cast<uint64_t>(value_start))
        {
            failure_reason = "object-wise nested member prefix exceeds the object frame";
            return false;
        }
        if (!AppendNestedFixedValues(reinterpret_cast<const uint8_t*>(buffer.Buffer()), object_end,
                                     static_cast<uint64_t>(value_start) + prefix, outer, inner, plan,
                                     object_layout, primitive_kind, max_values_per_entry,
                                     values, flat_indices, failure_reason, collect_indices))
        {
            return false;
        }
        buffer.SetBufferOffset(static_cast<Int_t>(object_end));
    }
    if (static_cast<uint64_t>(buffer.Length()) != frame_end)
    {
        failure_reason = "object-wise nested STL elements do not consume the declared container frame";
        return false;
    }
    return true;
}

bool DecodeNestedMemberWise(TBufferFile& buffer, const SerializedReadPlan& plan, uint64_t frame_end,
                            uint64_t outer, uint64_t max_values_per_entry,
                            SerializedPrimitiveKind primitive_kind, std::vector<RootPrimitiveValue>& values,
                            std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices)
{
    const Version_t element_version = buffer.ReadVersionForMemberWise(plan.nested_element_class);
    SerializedEntryLayout layout;
    if (!ResolveSerializedNestedFixedLayout(plan, element_version, layout, failure_reason))
    {
        return false;
    }
    const Int_t count_position = buffer.Length();
    if (count_position < 0 || static_cast<uint64_t>(count_position) > frame_end ||
        frame_end - static_cast<uint64_t>(count_position) < 4)
    {
        failure_reason = "member-wise nested STL count is outside its frame";
        return false;
    }
    Int_t signed_count = -1;
    buffer.ReadInt(signed_count);
    if (signed_count < 0 || static_cast<uint64_t>(signed_count) > max_values_per_entry / plan.fixed_array_length ||
        static_cast<uint64_t>(signed_count) * plan.fixed_array_length > max_values_per_entry - values.size())
    {
        failure_reason = "member-wise nested STL count exceeds configured value limits";
        return false;
    }
    const uint64_t count = static_cast<uint32_t>(signed_count);
    const uint64_t prefix_width = layout.bytes_before_value_per_element;
    if ((prefix_width && count > std::numeric_limits<uint64_t>::max() / prefix_width) ||
        !layout.value_bytes || count > std::numeric_limits<uint64_t>::max() / plan.fixed_array_length)
    {
        failure_reason = "member-wise nested column width overflow";
        return false;
    }
    const uint64_t column_values = count * plan.fixed_array_length;
    if (column_values > std::numeric_limits<uint64_t>::max() / layout.value_bytes)
    {
        failure_reason = "member-wise nested value width overflow";
        return false;
    }
    const uint64_t prefix_bytes = count * prefix_width;
    const uint64_t value_bytes = column_values * layout.value_bytes;
    const Int_t cursor = buffer.Length();
    if (cursor < 0 || static_cast<uint64_t>(cursor) > frame_end ||
        prefix_bytes > frame_end - static_cast<uint64_t>(cursor) ||
        value_bytes > frame_end - static_cast<uint64_t>(cursor) - prefix_bytes)
    {
        failure_reason = "member-wise nested values exceed their container frame";
        return false;
    }
    const uint64_t payload = static_cast<uint64_t>(cursor) + prefix_bytes;
    const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.Buffer());
    for (uint64_t inner = 0; inner < count; ++inner)
    {
        if (!AppendNestedFixedValues(bytes, frame_end,
                                     payload + inner * plan.fixed_array_length * layout.value_bytes,
                                     outer, inner, plan, layout, primitive_kind, max_values_per_entry,
                                     values, flat_indices, failure_reason, collect_indices))
        {
            return false;
        }
    }
    buffer.SetBufferOffset(static_cast<Int_t>(frame_end));
    return true;
}

} // namespace

bool DecodeSerializedNestedFixedColumn(TBufferFile& buffer, const SerializedReadPlan& plan, size_t entry_size,
                                       uint64_t outer_count, uint64_t max_values_per_entry,
                                       std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                       std::string& failure_reason, bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    if (!plan.Is(SerializedProjectionKind::NESTED_FIXED_MEMBER) || !plan.nested_container_class ||
        !plan.nested_element_class || !plan.value_bytes || !plan.fixed_array_length ||
        outer_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    {
        failure_reason = "nested offset projection has invalid class or primitive metadata";
        return false;
    }
    const auto primitive_kind = ClassifySerializedPrimitive(plan.value_type);
    if (primitive_kind == SerializedPrimitiveKind::UNKNOWN ||
        serialized_codec::PrimitiveWidth(primitive_kind) != plan.value_bytes)
    {
        failure_reason = "nested offset projection uses an unsupported primitive";
        return false;
    }
    for (uint64_t outer = 0; outer < outer_count; ++outer)
    {
        const Int_t current_position = buffer.Length();
        if (current_position < 0 || static_cast<uint64_t>(current_position) > entry_size ||
            entry_size - static_cast<uint64_t>(current_position) < 6)
        {
            failure_reason = "nested STL header exceeds the serialized entry";
            break;
        }
        UInt_t start = 0;
        UInt_t byte_count = 0;
        const Version_t version = buffer.ReadVersion(&start, &byte_count, plan.nested_container_class);
        const Int_t decoded_position = buffer.Length();
        const uint64_t frame_end = static_cast<uint64_t>(start) + byte_count + 4;
        const bool memberwise = (version & TBufferFile::kStreamedMemberWise) != 0;
        const auto stl_version = static_cast<uint16_t>(version & ~TBufferFile::kStreamedMemberWise);
        if (outer == 0)
        {
            RootDebug("SERIALIZED.NESTED_FIXED_HEADER",
                      "path=" + plan.logical_path +
                          " cursor=" + std::to_string(current_position) +
                          " version=" + std::to_string(version) +
                          " streamed_memberwise=" + std::to_string(memberwise) +
                          " frame_end=" + std::to_string(frame_end) +
                          " entry_size=" + std::to_string(entry_size) +
                          " outer_count=" + std::to_string(outer_count));
        }
        if (!byte_count || decoded_position < 0 || frame_end > entry_size ||
            frame_end < static_cast<uint64_t>(decoded_position))
        {
            failure_reason = "nested STL has inconsistent framed byte count";
            break;
        }
        bool decoded = false;
        if (memberwise && stl_version >= 8)
        {
            decoded = DecodeNestedMemberWise(buffer, plan, frame_end, outer, max_values_per_entry,
                                             primitive_kind, values, flat_indices, failure_reason, collect_indices);
        }
        else if (!memberwise && stl_version == 9)
        {
            decoded = DecodeNestedObjectWise(buffer, plan, frame_end, outer, max_values_per_entry,
                                             primitive_kind, values, flat_indices, failure_reason, collect_indices);
        }
        else
        {
            failure_reason = "unsupported nested STL encoding version " + std::to_string(stl_version) +
                             (memberwise ? " (member-wise)" : " (object-wise)");
        }
        if (!decoded)
        {
            break;
        }
    }
    if (failure_reason.empty() && collect_indices && flat_indices.size() != values.size() * plan.index_depth)
    {
        failure_reason = "nested offset projection produced inconsistent collection indices";
    }
    if (!failure_reason.empty())
    {
        values.clear();
        flat_indices.clear();
        return false;
    }
    return true;
}

} // namespace duckdb::rootlake
