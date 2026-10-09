#include "root4duckdb/serialized/root_serialized_plan_layout.hpp"
#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <limits>

namespace duckdb::rootlake
{

uint64_t FixedArrayLength(TStreamerElement* element, std::vector<uint32_t>* dimensions)
{
    if (!element)
    {
        return 0;
    }
    const int rank = element->GetArrayDim();
    if (rank <= 0)
    {
        return 1;
    }
    uint64_t length = 1;
    for (int dim = 0; dim < rank; ++dim)
    {
        const int extent = element->GetMaxIndex(dim);
        if (extent <= 0 || length > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(extent))
        {
            return 0;
        }
        if (dimensions)
        {
            dimensions->push_back(static_cast<uint32_t>(extent));
        }
        length *= static_cast<uint64_t>(extent);
    }
    return length;
}

bool FixedPrimitiveWidth(TStreamerElement* element, uint32_t& width, std::string& reason)
{
    if (!element)
    {
        reason = "null streamer element";
        return false;
    }
    if (element->IsaPointer())
    {
        reason = "pointer before projected member";
        return false;
    }
    int type_code = element->GetType();
    while (type_code >= 20 && type_code < 60)
    {
        type_code -= 20;
    }
    const std::string raw_type = element->GetTypeName() ? element->GetTypeName() : "";
    const std::string type =
        StreamerPrimitiveType(element->GetType(), element->GetTypeName() ? element->GetTypeName() : "");
    if (type_code == 9 || type_code == 19 || raw_type == "Float16_t" || raw_type == "Double32_t")
    {
        reason = "compressed floating streamer element before projected member";
        return false;
    }
    const auto primitive = PrimitiveTypeSize(type);
    const auto length = FixedArrayLength(element);
    if (!primitive || !length || length > std::numeric_limits<uint32_t>::max() / primitive)
    {
        reason = "variable-width or unsupported streamer element before projected member: " +
                 std::string(element->GetName());
        return false;
    }
    width = primitive * static_cast<uint32_t>(length);
    return true;
}

bool ResolveSerializedNestedFixedLayout(const SerializedReadPlan& plan, int32_t element_version,
                                        SerializedEntryLayout& resolved_layout, std::string& failure_reason)
{
    if (!plan.Is(SerializedProjectionKind::NESTED_FIXED_MEMBER) || !plan.nested_element_class ||
        plan.nested_member_name.empty())
    {
        failure_reason = "nested fixed projection has no persistent element metadata";
        return false;
    }
    SerializedReadPlan nested_plan;
    nested_plan.Select(SerializedProjectionKind::FIXED_MEMBER);
    nested_plan.logical_path = plan.logical_path;
    nested_plan.outer_element_class = plan.nested_element_class;
    nested_plan.projected_member_name = plan.nested_member_name;
    nested_plan.value_type = plan.value_type;
    nested_plan.value_bytes = plan.value_bytes;
    nested_plan.fixed_array_length = plan.fixed_array_length;
    nested_plan.array_dimensions = plan.array_dimensions;
    nested_plan.streamer_version =
        static_cast<uint32_t>(std::max(0, static_cast<int>(plan.nested_element_class->GetClassVersion())));
    nested_plan.index_depth = 1 + (plan.fixed_array_length > 1 ?
                                   std::max<idx_t>(1, plan.array_dimensions.size()) : 0);
    return ResolveSerializedFixedLayout(nested_plan, element_version, resolved_layout, failure_reason);
}

bool ResolveSerializedNestedMemberWiseStride(const SerializedReadPlan& plan, int32_t element_version,
                                              uint64_t& bytes_per_element, std::string& failure_reason)
{
    bytes_per_element = 0;
    if (!plan.Is(SerializedProjectionKind::NESTED_FIXED_MEMBER) || !plan.nested_element_class ||
        element_version < 0)
    {
        failure_reason = "nested member-wise stride requires a versioned element class";
        return false;
    }
    auto* streamer = plan.nested_element_class->GetStreamerInfo(element_version);
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements)
    {
        failure_reason = "nested member-wise element has no matching on-file streamer";
        return false;
    }
    bool target_seen = false;
    uint64_t stride = 0;
    for (int i = 0; i < elements->GetEntries(); ++i)
    {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element)
        {
            failure_reason = "nested member-wise element contains invalid streamer metadata";
            return false;
        }
        uint64_t width = 0;
        if (element->IsBase())
        {
            std::string base_type = TrimType(element->GetTypeName() ? element->GetTypeName() : "");
            if (base_type.empty() || base_type == "BASE")
            {
                base_type = TrimType(element->GetName() ? element->GetName() : "");
            }
            if (base_type != "TObject")
            {
                failure_reason = "nested member-wise element has unsupported base: " + base_type;
                return false;
            }
            width = 10;
        }
        else
        {
            uint32_t primitive_width = 0;
            std::string width_reason;
            if (!FixedPrimitiveWidth(element, primitive_width, width_reason))
            {
                failure_reason = "nested member-wise stride is not fixed: " + width_reason;
                return false;
            }
            width = primitive_width;
            if (plan.nested_member_name == element->GetName())
            {
                target_seen = true;
            }
        }
        if (stride > std::numeric_limits<uint64_t>::max() - width)
        {
            failure_reason = "nested member-wise element stride overflow";
            return false;
        }
        stride += width;
    }
    if (!target_seen || !stride)
    {
        failure_reason = "nested fixed member missing from versioned member-wise element";
        return false;
    }
    bytes_per_element = stride;
    return true;
}

bool ResolveSerializedFixedLayout(const SerializedReadPlan& plan, int32_t element_version,
                                  SerializedEntryLayout& resolved_layout, std::string& failure_reason)
{
    if (!plan.Is(SerializedProjectionKind::FIXED_MEMBER) || !plan.outer_element_class ||
        plan.projected_member_name.empty() || element_version < 0)
    {
        failure_reason = "serialized fixed projection has no versioned member metadata";
        return false;
    }
    auto* streamer = plan.outer_element_class->GetStreamerInfo(element_version);
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements)
    {
        failure_reason =
            "ROOT has no TStreamerInfo for serialized fixed element version " + std::to_string(element_version);
        return false;
    }
    uint64_t prefix_width = 0;
    TStreamerElement* target = nullptr;
    for (int i = 0; i < elements->GetEntries(); ++i)
    {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element)
        {
            failure_reason = "serialized fixed element version has invalid streamer metadata";
            return false;
        }
        if (!element->IsBase() && plan.projected_member_name == element->GetName())
        {
            target = element;
            break;
        }
        if (element->IsBase())
        {
            std::string base_type = TrimType(element->GetTypeName() ? element->GetTypeName() : "");
            if (base_type.empty() || base_type == "BASE")
            {
                base_type = TrimType(element->GetName() ? element->GetName() : "");
            }
            if (base_type != "TObject")
            {
                failure_reason = "unsupported base class before versioned fixed member: " + base_type;
                return false;
            }
            if (prefix_width > std::numeric_limits<uint32_t>::max() - 10U)
            {
                failure_reason = "versioned fixed member prefix exceeds 32-bit offset";
                return false;
            }
            prefix_width += 10;
            continue;
        }
        uint32_t element_width = 0;
        std::string width_reason;
        if (!FixedPrimitiveWidth(element, element_width, width_reason))
        {
            failure_reason = "versioned fixed member prefix is not arithmetic: " + width_reason;
            return false;
        }
        prefix_width += element_width;
        if (prefix_width > std::numeric_limits<uint32_t>::max())
        {
            failure_reason = "versioned fixed member prefix exceeds 32-bit offset";
            return false;
        }
    }
    if (!target)
    {
        failure_reason =
            "projected fixed member is absent from serialized element version " + std::to_string(element_version);
        return false;
    }
    uint32_t target_width = 0;
    std::string target_reason;
    if (!FixedPrimitiveWidth(target, target_width, target_reason))
    {
        failure_reason = "versioned fixed target is not arithmetic: " + target_reason;
        return false;
    }
    std::vector<uint32_t> dimensions;
    const auto array_length = FixedArrayLength(target, &dimensions);
    const auto target_type =
        StreamerPrimitiveType(target->GetType(), target->GetTypeName() ? target->GetTypeName() : "");
    const auto scalar_width = PrimitiveTypeSize(target_type);
    const auto target_kind = ClassifySerializedPrimitive(PrimitiveBaseType(target_type));
    const auto planned_kind = ClassifySerializedPrimitive(plan.value_type);
    const bool identical_primitive = target_kind == planned_kind && scalar_width == plan.value_bytes;
    const bool widen_float = target_kind == SerializedPrimitiveKind::FLOAT32 &&
                             planned_kind == SerializedPrimitiveKind::FLOAT64 && scalar_width == 4 &&
                             plan.value_bytes == 8;
    if (!array_length || !scalar_width || target_width != array_length * scalar_width ||
        target_kind == SerializedPrimitiveKind::UNKNOWN || (!identical_primitive && !widen_float) ||
        array_length != plan.fixed_array_length || dimensions != plan.array_dimensions)
    {
        failure_reason = "projected fixed member changed persistent type or shape in serialized element version " +
                         std::to_string(element_version) + " (field=" + plan.projected_member_name +
                         ", on_file_type=" + target_type + ", dictionary_type=" + plan.value_type +
                         ", on_file_scalar_bytes=" + std::to_string(scalar_width) +
                         ", dictionary_scalar_bytes=" + std::to_string(plan.value_bytes) +
                         ", on_file_array_length=" + std::to_string(array_length) +
                         ", dictionary_array_length=" + std::to_string(plan.fixed_array_length) +
                         ", on_file_rank=" + std::to_string(dimensions.size()) +
                         ", dictionary_rank=" + std::to_string(plan.array_dimensions.size()) + ")";
        return false;
    }
    resolved_layout = {};
    resolved_layout.value_type = PrimitiveBaseType(target_type);
    resolved_layout.primitive_kind = target_kind;
    resolved_layout.bytes_before_value_per_element = static_cast<uint32_t>(prefix_width);
    resolved_layout.value_bytes = scalar_width;
    resolved_layout.fixed_array_length = plan.fixed_array_length;
    resolved_layout.array_dimensions = plan.array_dimensions;
    resolved_layout.index_depth = static_cast<size_t>(plan.index_depth);
    RootDebug("SERIALIZED.FIXED_SCHEMA", "path=" + plan.logical_path +
                                             " file_version=" + std::to_string(element_version) +
                                             " dictionary_version=" + std::to_string(plan.streamer_version) +
                                             " prefix_bytes=" + std::to_string(prefix_width));
    return true;
}

bool ResolveSerializedNestedVersion(const SerializedReadPlan& plan, int32_t element_version,
                                    int32_t& resolved_element_version, std::vector<int>& prefix_element_ids,
                                    std::string& failure_reason)
{
    if (element_version < 0)
    {
        failure_reason = "serialized outer element version is invalid";
        return false;
    }
    if (resolved_element_version == element_version)
    {
        return true;
    }
    if (!plan.outer_element_class || plan.projected_member_name.empty())
    {
        failure_reason = "serialized nested vector plan has no projected member metadata";
        return false;
    }
    if (static_cast<uint32_t>(element_version) == plan.streamer_version)
    {
        prefix_element_ids = plan.prefix_element_ids;
    }
    else if (!ResolveSerializedVersionedMember(plan, element_version, prefix_element_ids, failure_reason))
    {
        return false;
    }
    resolved_element_version = element_version;
    RootDebug("SERIALIZED.NESTED_SCHEMA", "path=" + plan.logical_path +
                                              " file_version=" + std::to_string(element_version) +
                                              " dictionary_version=" + std::to_string(plan.streamer_version) +
                                              " prefix_elements=" + std::to_string(prefix_element_ids.size()));
    return true;
}

} // namespace duckdb::rootlake
