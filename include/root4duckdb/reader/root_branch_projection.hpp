#ifndef ROOT4DUCKDB_READER_ROOT_BRANCH_PROJECTION_HPP
#define ROOT4DUCKDB_READER_ROOT_BRANCH_PROJECTION_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

class TBranch;
class TTree;

namespace duckdb::rootlake
{

/// Outcome of a physical branch projection attempt.
struct BranchProjectionResult
{
    bool applied = false;
    std::string reason;
    std::vector<std::string> branches;
};

BranchProjectionResult ApplyBranchProjection(TTree* tree, const std::vector<TBranch*>& input, uint64_t cache_bytes,
                                             uint64_t entry_begin = 0,
                                             uint64_t entry_end = std::numeric_limits<uint64_t>::max());

/// Restores all branches and configures the optional tree cache.
void EnableAllBranches(TTree* tree, uint64_t cache_bytes = 0);

} // namespace duckdb::rootlake

#endif
