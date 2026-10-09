#include "root4duckdb/core/root_headers.hpp"

#include "root4duckdb/reader/root_branch_projection.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace duckdb::rootlake
{

BranchProjectionResult ApplyBranchProjection(TTree* tree, const std::vector<TBranch*>& input, uint64_t cache_bytes,
                                             uint64_t entry_begin, uint64_t entry_end)
{
    BranchProjectionResult result;
    if (!tree)
    {
        result.reason = "missing TTree";
        return result;
    }
    std::vector<TBranch*> branches;
    std::set<TBranch*> seen;
    for (auto* branch : input)
    {
        if (!branch || !seen.insert(branch).second)
        {
            continue;
        }
        if (branch->GetTree() != tree)
        {
            result.reason = "physical branches belong to different TTrees";
            return result;
        }
        // A selected ancestor with persistent children is not a self-contained
        // byte stream. Keep the universal branch set in that case.
        if (branch->GetListOfBranches() && branch->GetListOfBranches()->GetEntries() > 0)
        {
            result.reason = "physical ancestor still has persistent child branches";
            return result;
        }
        branches.push_back(branch);
    }
    if (branches.empty())
    {
        result.reason = "no self-contained physical branches";
        return result;
    }
    tree->SetBranchStatus("*", 0);
    const auto cache_size = static_cast<Long64_t>(
        std::min<uint64_t>(cache_bytes, static_cast<uint64_t>(std::numeric_limits<Long64_t>::max())));
    tree->SetCacheSize(cache_size);
    for (auto* branch : branches)
    {
        tree->SetBranchStatus(branch->GetName(), 1);
        if (cache_bytes)
        {
            tree->AddBranchToCache(branch, true);
        }
        result.branches.emplace_back(branch->GetName());
    }
    // StopCacheLearningPhase may immediately prefetch selected baskets.
    // Restrict that first fill to the scheduled event interval instead of
    // reading the whole TTree to fill a large configured cache.
    if (cache_bytes)
    {
        const auto total_entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));
        const auto begin = std::min(entry_begin, total_entries);
        const auto end = std::min(entry_end, total_entries);
        if (begin < end)
        {
            tree->SetCacheEntryRange(static_cast<Long64_t>(begin), static_cast<Long64_t>(end));
        }
        tree->StopCacheLearningPhase();
    }
    result.applied = true;
    return result;
}

void EnableAllBranches(TTree* tree, uint64_t cache_bytes)
{
    if (!tree)
    {
        return;
    }
    tree->SetBranchStatus("*", 1);
    const auto cache_size = static_cast<Long64_t>(
        std::min<uint64_t>(cache_bytes, static_cast<uint64_t>(std::numeric_limits<Long64_t>::max())));
    // This may follow a narrow serialized projection whose cache has already
    // stopped learning. Recreate that cache to learn the object reader's real
    // branch accesses rather than retaining the stale narrow branch list.
    tree->SetCacheSize(0);
    if (cache_size)
    {
        tree->SetCacheSize(cache_size);
    }
    // AddBranchToCache("*") would disable learning and include every branch.

}

} // namespace duckdb::rootlake
