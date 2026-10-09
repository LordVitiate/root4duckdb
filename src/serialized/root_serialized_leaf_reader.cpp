#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <limits>

namespace duckdb::rootlake
{

namespace
{

void AppendFixedCoordinates(uint64_t flat, const std::vector<uint32_t>& dimensions, std::vector<int32_t>& indices)
{
    if (dimensions.empty())
    {
        indices.push_back(static_cast<int32_t>(flat));
        return;
    }
    const auto begin = indices.size();
    indices.resize(begin + dimensions.size());
    for (size_t reverse = dimensions.size(); reverse > 0; --reverse)
    {
        const auto dimension = reverse - 1;
        indices[begin + dimension] = static_cast<int32_t>(flat % dimensions[dimension]);
        flat /= dimensions[dimension];
    }
}

} // namespace

bool SerializedBasketReader::DecodeLeafBranchEntry(uint64_t entry, std::vector<RootPrimitiveValue>& values,
                                                   std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                                   bool collect_indices)
{
    values.clear();
    flat_indices.clear();
    if (!branch || !leaf)
    {
        failure_reason = "serialized primitive branch has no unique ROOT leaf";
        return false;
    }
    // Read count branches before split data leaves to preserve the flattened shape.
    auto* branch_element = dynamic_cast<TBranchElement*>(branch);
    std::vector<TBranch*> count_branches;
    auto add_count_branch = [&](TBranch* count_branch)
    {
        if (count_branch && count_branch != branch &&
            std::find(count_branches.begin(), count_branches.end(), count_branch) == count_branches.end())
        {
            count_branches.push_back(count_branch);
        }
    };
    // C arrays use GetLeafCount; split STL members use branch count metadata.
    auto* count_leaf = leaf->GetLeafCount();
    add_count_branch(count_leaf ? count_leaf->GetBranch() : nullptr);
    if (branch_element)
    {
        add_count_branch(branch_element->GetBranchCount());
        add_count_branch(branch_element->GetBranchCount2());
    }
    if (branch_element && branch_element->GetBranchCount2())
    {
        failure_reason = "split variable-length primitive member has a second count branch";
        return false;
    }
    const auto root_entry = static_cast<Long64_t>(entry);
    // Detach old child scratch before collection masters read their counts.
    branch->ResetAddress();
    struct CountRead
    {
        TBranch* source = nullptr;
        double value = 0.0;
        int ndata = 0;
    };
    std::vector<CountRead> count_reads;
    count_reads.reserve(count_branches.size());
    for (auto* count_branch : count_branches)
    {
        // Count branches write only Int_t into private MakeClass scratch.
        std::max_align_t count_scratch{};
        auto* count_element = dynamic_cast<TBranchElement*>(count_branch);
        if (count_element && !count_element->SetMakeClass(true))
        {
            failure_reason = "ROOT cannot decompose a primitive branch collection count";
            return false;
        }
        count_branch->ResetAddress();
        count_branch->SetAddress(&count_scratch);
        const auto count_bytes = count_branch->GetEntry(root_entry, 1);
        if (count_bytes <= 0)
        {
            count_branch->ResetAddress();
            if (count_element)
            {
                count_element->SetMakeClass(false);
            }
            failure_reason = "ROOT failed to read a primitive branch collection count";
            return false;
        }
        auto* count_leaves = count_branch->GetListOfLeaves();
        auto* count_value_leaf =
            count_leaves && count_leaves->GetEntries() == 1 ? dynamic_cast<TLeaf*>(count_leaves->At(0)) : nullptr;
        if (!count_element && !count_value_leaf)
        {
            count_branch->ResetAddress();
            failure_reason = "ROOT count branch has neither a branch-element count nor a unique leaf";
            return false;
        }
        const double count_value =
            count_value_leaf ? count_value_leaf->GetValue(0) : static_cast<double>(count_element->GetNdata());
        const int count_ndata = count_element ? count_element->GetNdata() : count_value_leaf->GetNdata();
        count_reads.push_back(CountRead{count_branch, count_value, count_ndata});
        count_branch->ResetAddress();
        if (count_element)
        {
            // Restore the collection master before re-enabling child decomposed mode.
            count_element->SetMakeClass(false);
        }
    }
    if (branch_element)
    {
        leaf_make_class = branch_element->SetMakeClass(true);
        if (!leaf_make_class)
        {
            failure_reason = "ROOT cannot decompose the primitive TBranchElement into private scratch";
            return false;
        }
    }
    const auto find_count = [&](TBranch* target) -> const CountRead*
    {
        const auto found = std::find_if(count_reads.begin(), count_reads.end(),
                                        [&](const CountRead& read)
                                        {
                                            return read.source == target;
                                        });
        return found == count_reads.end() ? nullptr : &*found;
    };
    int object_count = 1;
    if (branch_element && branch_element->GetBranchCount())
    {
        auto* count_branch = branch_element->GetBranchCount();
        const auto* count_read = find_count(count_branch);
        if (!count_read)
        {
            failure_reason = "primitive branch collection count was not captured";
            return false;
        }
        if (branch_element->GetType() == TBranchElement::kSTLMemberNode ||
            branch_element->GetType() == TBranchElement::kClonesMemberNode)
        {
            // Reading the collection master sets the member branch cardinality.
            object_count = count_read->ndata;
        }
        else
        {
            // Counted primitive members instead point at a scalar counter;
            // its GetNdata() is one, while GetValue() is the actual length.
            const auto counted = count_read->value;
            if (counted < 0.0 || counted > static_cast<double>(std::numeric_limits<int32_t>::max()))
            {
                failure_reason = "primitive branch collection count is outside the supported range";
                return false;
            }
            object_count = static_cast<int>(counted);
        }
    }
    else if (count_leaf)
    {
        const auto* count_read = find_count(count_leaf->GetBranch());
        if (!count_read)
        {
            failure_reason = "primitive array count was not captured";
            return false;
        }
        const auto counted = count_read->value;
        if (counted < 0.0 || counted > static_cast<double>(std::numeric_limits<int32_t>::max()))
        {
            failure_reason = "primitive branch collection count is outside the supported range";
            return false;
        }
        object_count = static_cast<int>(counted);
    }
    const auto logical_type = RootTypeToScanLogicalType(plan.value_type, false, true);
    uint64_t fixed_length = 1;
    std::vector<uint32_t> fixed_dimensions;
    size_t container_depth = 0;
    for (const auto& level : plan.projection_levels)
    {
        container_depth += level.is_container ? 1U : 0U;
        if (level.is_fixed_array)
        {
            fixed_length = level.fixed_array_length;
            fixed_dimensions = level.array_dimensions;
        }
    }
    if (container_depth > 1)
    {
        failure_reason = "split primitive branch lost nested container boundaries";
        return false;
    }
    if (!fixed_length || object_count < 0 || static_cast<uint64_t>(object_count) > max_values_per_entry / fixed_length)
    {
        failure_reason = "primitive physical branch fixed-array shape is inconsistent";
        return false;
    }
    const auto count = static_cast<uint64_t>(object_count) * fixed_length;
    if (!count)
    {
        return true;
    }
    if (count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || !EnsureLeafScratch(count, failure_reason))
    {
        if (failure_reason.empty())
        {
            failure_reason = "primitive physical branch value count exceeds supported range";
        }
        return false;
    }
    const auto bytes = branch->GetEntry(root_entry, 1);
    // Detach branch addresses before scratch inspection or fallback reuse.
    branch->ResetAddress();
    if (bytes <= 0)
    {
        failure_reason = "ROOT failed to read the primitive physical branch";
        return false;
    }
    values.reserve(static_cast<size_t>(count));
    if (collect_indices)
    {
        flat_indices.reserve(static_cast<size_t>(count * plan.index_depth));
    }
    const auto* scratch = leaf_scratch.data();
    for (uint64_t index = 0; index < count; ++index)
    {
        switch (logical_type.id())
        {
        case LogicalTypeId::BOOLEAN:
            values.push_back(RootPrimitiveValue::Unsigned(reinterpret_cast<const bool*>(scratch)[index] ? 1U : 0U));
            break;
        case LogicalTypeId::UTINYINT:
            values.push_back(RootPrimitiveValue::Unsigned(reinterpret_cast<const uint8_t*>(scratch)[index]));
            break;
        case LogicalTypeId::USMALLINT:
            values.push_back(RootPrimitiveValue::Unsigned(reinterpret_cast<const uint16_t*>(scratch)[index]));
            break;
        case LogicalTypeId::UINTEGER:
            values.push_back(RootPrimitiveValue::Unsigned(reinterpret_cast<const uint32_t*>(scratch)[index]));
            break;
        case LogicalTypeId::UBIGINT:
            values.push_back(RootPrimitiveValue::Unsigned(reinterpret_cast<const uint64_t*>(scratch)[index]));
            break;
        case LogicalTypeId::TINYINT:
            values.push_back(RootPrimitiveValue::Signed(reinterpret_cast<const int8_t*>(scratch)[index]));
            break;
        case LogicalTypeId::SMALLINT:
            values.push_back(RootPrimitiveValue::Signed(reinterpret_cast<const int16_t*>(scratch)[index]));
            break;
        case LogicalTypeId::INTEGER:
            values.push_back(RootPrimitiveValue::Signed(reinterpret_cast<const int32_t*>(scratch)[index]));
            break;
        case LogicalTypeId::BIGINT:
            values.push_back(RootPrimitiveValue::Signed(reinterpret_cast<const int64_t*>(scratch)[index]));
            break;
        case LogicalTypeId::FLOAT:
            values.push_back(
                RootPrimitiveValue::Floating(static_cast<double>(reinterpret_cast<const float*>(scratch)[index])));
            break;
        case LogicalTypeId::DOUBLE:
            values.push_back(RootPrimitiveValue::Floating(reinterpret_cast<const double*>(scratch)[index]));
            break;
        default:
            failure_reason = "primitive physical branch has an unsupported logical type";
            values.clear();
            flat_indices.clear();
            return false;
        }
        if (!collect_indices)
        {
            continue;
        }
        if (container_depth)
        {
            flat_indices.push_back(static_cast<int32_t>(index / fixed_length));
        }
        if (fixed_length > 1)
        {
            AppendFixedCoordinates(index % fixed_length, fixed_dimensions, flat_indices);
        }
    }
    counters.serialized_bytes += static_cast<uint64_t>(bytes);
    return true;
}

} // namespace duckdb::rootlake
