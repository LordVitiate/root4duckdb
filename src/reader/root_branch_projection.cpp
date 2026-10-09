#include "root4duckdb/core/root_headers.hpp"

#include "root4duckdb/reader/root_branch_projection.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace duckdb::rootlake
{

BranchProjectionResult ApplyBranchProjection(TTree* tree, const std::vector<TBranch*>& input, uint64_t cache_bytes)
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
    if (cache_bytes)
    {
        tree->SetCacheSize(static_cast<Long64_t>(
            std::min<uint64_t>(cache_bytes, static_cast<uint64_t>(std::numeric_limits<Long64_t>::max()))));
    }
    for (auto* branch : branches)
    {
        tree->SetBranchStatus(branch->GetName(), 1);
        tree->AddBranchToCache(branch, true);
        result.branches.emplace_back(branch->GetName());
    }
    // The requested branches are already known. End ROOT's learning phase so
    // TTreeCache can begin prefetching without sampling the first entries.
    if (cache_bytes)
    {
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
    if (cache_bytes)
    {
        tree->SetCacheSize(static_cast<Long64_t>(
            std::min<uint64_t>(cache_bytes, static_cast<uint64_t>(std::numeric_limits<Long64_t>::max()))));
    }
    // In object fallback the actual branch-use pattern is not known here.
    // Keep ROOT's adaptive learning instead of eagerly prefetching the tree.
    tree->AddBranchToCache("*", true);
}

} // namespace duckdb::rootlake
