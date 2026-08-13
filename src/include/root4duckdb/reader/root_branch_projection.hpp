#pragma once

#include <cstdint>
#include <string>
#include <vector>

class TBranch;
class TTree;

namespace duckdb::rootlake {

/// Outcome of a physical branch projection attempt.
struct BranchProjectionResult {
    bool applied = false;
    std::string reason;
    std::vector<std::string> branches;
};

/// Enables proven ancestors; callers validate once against object reading.
BranchProjectionResult ApplyBranchProjection(TTree* tree, const std::vector<TBranch*>& branches, uint64_t cache_bytes);

/// Restores all branches and configures the optional tree cache.
void EnableAllBranches(TTree* tree, uint64_t cache_bytes = 0);

} // namespace duckdb::rootlake
