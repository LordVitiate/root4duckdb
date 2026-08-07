#pragma once

#include <cstdint>
#include <string>
#include <vector>

class TBranch;
class TTree;

namespace duckdb::rootlake {

struct BranchProjectionResult {
    bool applied = false;
    std::string reason;
    std::vector<std::string> branches;
};

// Enable only proven physical ancestor branches. Callers must validate the
// resulting values once against the universal object reader before trusting
// the projection for a file.
BranchProjectionResult ApplyBranchProjection(TTree *tree,
                                             const std::vector<TBranch *> &branches,
                                             uint64_t cache_bytes);

void EnableAllBranches(TTree *tree, uint64_t cache_bytes = 0);

} // namespace duckdb::rootlake
