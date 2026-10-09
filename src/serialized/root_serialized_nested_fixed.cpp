#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include "root4duckdb/serialized/root_serialized_codec_utils.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace duckdb::rootlake
{

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

bool DecodeNestedMemberWise(TBufferFile& buffer, const SerializedReadPlan& plan, uint64_t frame_end,
                            uint64_t outer_count, uint64_t max_values_per_entry,
                            SerializedPrimitiveKind primitive_kind, SerializedNestedFixedLayoutCache& layout_cache,
                            std::vector<RootPrimitiveValue>& values,
                            std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices)
{
    // One framed STL header precedes all outer elements. Each element has a
    // count followed by column-oriented member-wise payloads.
    const Version_t element_version = buffer.ReadVersionForMemberWise(plan.nested_element_class);
    if (element_version < 0)
    {
        failure_reason = "nested member-wise STL has an invalid element version";
        return false;
    }
    if (layout_cache.element_version != element_version)
    {
        SerializedEntryLayout resolved_layout;
        uint64_t resolved_stride = 0;
        if (!ResolveSerializedNestedFixedLayout(plan, element_version, resolved_layout, failure_reason) ||
            !ResolveSerializedNestedMemberWiseStride(plan, element_version, resolved_stride, failure_reason))
        {
            return false;
        }
        // Commit both values together: a failed schema change must never leave
        // the reader with a half-updated layout.
        layout_cache.layout = std::move(resolved_layout);
        layout_cache.element_stride = resolved_stride;
        layout_cache.element_version = element_version;
    }
    const auto& layout = layout_cache.layout;
    const uint64_t element_stride = layout_cache.element_stride;
    if (!layout.value_bytes ||
        plan.fixed_array_length > std::numeric_limits<uint64_t>::max() / layout.value_bytes)
    {
        failure_reason = "nested member-wise projected array size overflows";
        return false;
    }
    const uint64_t projected_width = plan.fixed_array_length * layout.value_bytes;
    if (layout.bytes_before_value_per_element > element_stride ||
        projected_width > element_stride - layout.bytes_before_value_per_element)
    {
        failure_reason = "nested member-wise projected field lies outside the complete element layout";
        return false;
    }
    if (outer_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    {
        failure_reason = "nested member-wise outer count exceeds index limits";
        return false;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.Buffer());
    for (uint64_t outer = 0; outer < outer_count; ++outer)
    {
        const Int_t count_position = buffer.Length();
        if (count_position < 0 || static_cast<uint64_t>(count_position) > frame_end ||
            frame_end - static_cast<uint64_t>(count_position) < 4)
        {
            failure_reason = "nested member-wise STL count is outside its frame";
            return false;
        }
        Int_t signed_count = -1;
        buffer.ReadInt(signed_count);
        if (signed_count < 0 || static_cast<uint64_t>(signed_count) > max_values_per_entry / plan.fixed_array_length ||
            static_cast<uint64_t>(signed_count) * plan.fixed_array_length > max_values_per_entry - values.size())
        {
            failure_reason = "nested member-wise STL count exceeds configured limits";
            return false;
        }
        const uint64_t count = static_cast<uint32_t>(signed_count);
        const Int_t data_position = buffer.Length();
        if (data_position < 0 || static_cast<uint64_t>(data_position) > frame_end ||
            (element_stride && count > (frame_end - static_cast<uint64_t>(data_position)) / element_stride))
        {
            failure_reason = "nested member-wise STL columns exceed their container frame";
            return false;
        }
        const uint64_t body_start = static_cast<uint64_t>(data_position);
        const uint64_t body_end = body_start + count * element_stride;
        const uint64_t payload_start = body_start + count * layout.bytes_before_value_per_element;
        if (count > 0 && (projected_width > (body_end - payload_start) / count))
        {
            failure_reason = "nested member-wise projected column exceeds its container body";
            return false;
        }
        for (uint64_t inner = 0; inner < count; ++inner)
        {
            if (!AppendNestedFixedValues(bytes, body_end,
                                         payload_start + inner * projected_width, outer, inner, plan, layout,
                                         primitive_kind, max_values_per_entry, values, flat_indices,
                                         failure_reason, collect_indices))
            {
                return false;
            }
        }
        buffer.SetBufferOffset(static_cast<Int_t>(body_end));
    }
    if (static_cast<uint64_t>(buffer.Length()) != frame_end)
    {
        failure_reason = "nested member-wise array does not consume its declared STL frame";
        return false;
    }
    return true;
}

} // namespace

bool DecodeSerializedNestedFixedColumn(TBufferFile& buffer, const SerializedReadPlan& plan, size_t entry_size,
                                       uint64_t outer_count, uint64_t max_values_per_entry,
                                       SerializedNestedFixedLayoutCache& layout_cache,
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

    const Int_t position = buffer.Length();
    if (position < 0 || static_cast<uint64_t>(position) > entry_size ||
        entry_size - static_cast<uint64_t>(position) < 6)
    {
        failure_reason = "nested STL header exceeds the serialized entry";
        return false;
    }
    const auto* header = reinterpret_cast<const uint8_t*>(buffer.Buffer()) + position;
    constexpr uint16_t memberwise_mask = static_cast<uint16_t>(TBufferFile::kStreamedMemberWise);
    const uint16_t stored_version = serialized_codec::ReadBE16(header + 4);
    const bool memberwise = (stored_version & memberwise_mask) != 0;
    const uint16_t stl_version = static_cast<uint16_t>(stored_version & ~memberwise_mask);
    if (!memberwise || stl_version < 8)
    {
        failure_reason = "nested STL requires supported member-wise encoding (version " +
                         std::to_string(stl_version) + ")";
        return false;
    }

    UInt_t start = 0;
    UInt_t byte_count = 0;
    (void)buffer.ReadVersion(&start, &byte_count, plan.nested_container_class);
    const Int_t payload_position = buffer.Length();
    const uint64_t frame_end = static_cast<uint64_t>(start) + byte_count + 4;
    if (!byte_count || payload_position < 0 || frame_end > entry_size ||
        frame_end < static_cast<uint64_t>(payload_position))
    {
        failure_reason = "nested STL has inconsistent framed byte count";
        return false;
    }
    if (!DecodeNestedMemberWise(buffer, plan, frame_end, outer_count, max_values_per_entry,
                                primitive_kind, layout_cache, values, flat_indices, failure_reason, collect_indices))
    {
        values.clear();
        flat_indices.clear();
        return false;
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
