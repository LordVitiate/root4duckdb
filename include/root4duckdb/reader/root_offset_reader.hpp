#ifndef ROOT4DUCKDB_READER_ROOT_OFFSET_READER_HPP
#define ROOT4DUCKDB_READER_ROOT_OFFSET_READER_HPP

#include "root4duckdb/reader/root_semantic_types.hpp"

namespace duckdb::rootlake
{

/// Resolves a logical path through ROOT streamer metadata.
class PathResolver
{
  public:
    /// Resolves a path or throws on incompatible streamer metadata.
    static std::vector<PathLevel> Resolve(TClass* root_class, const std::vector<std::string>& fields);
    /// Resolves a path without crossing an exception boundary.
    static std::vector<PathLevel> TryResolve(TClass* root_class, const std::vector<std::string>& fields) noexcept;
};

/// Derives flattened index names and signatures from a path.
/// @{
void AppendLevelIndexNames(const PathLevel& level, std::vector<std::string>& names);
std::string IndexSignature(const std::vector<PathLevel>& levels);
idx_t IndexDepth(const std::vector<PathLevel>& levels);
/// @}

/// Describes exactly one logical level below a semantic path.
bool DescribeSemanticPath(TClass* root_class, const ParsedPath& path, const std::string& raw_path,
                          std::vector<SemanticPathChild>& children);

/// Selects immediate primitive/string children for a relation prefix.
bool SelectSemanticPath(TClass* root_class, const ParsedPath& path, const std::string& raw_path,
                        SemanticPathSelection& selection);

/// Resumable bounded traversal of one materialized ROOT entry.
/// The caller owns the object and resolved levels; neither may change until Reset().
/// All cursor state is worker-local (CERT EXP54-CPP, CTR50-CPP, CON52-CPP).
class OffsetValueCursor final
{
  public:
    void Open(void* root_object, const std::vector<PathLevel>& levels, int64_t entry_id);
    /// Returns true when the page contains rows. Empty means traversal exhausted.
    bool ReadPage(idx_t maximum_rows, ReadResult& result);
    [[nodiscard]] bool HasMore() const noexcept;
    void Reset() noexcept;

  private:
    void* root_object_ = nullptr; // Non-owning; parent ROOT reader outlives the cursor.
    const std::vector<PathLevel>* levels_ = nullptr;
    std::vector<int32_t> resume_indices_;
    int64_t entry_id_ = 0;
    bool previous_emitted_ = false;
    bool complete_ = true;
};

/// Traverses materialized ROOT objects using resolved offsets.
class OffsetValueReader
{
  public:
    /// Collects exact primitive values and flat indices without creating per-value rows.
    static void CollectTypedFlat(void* root_object, const std::vector<PathLevel>& levels, idx_t index_depth,
                                 int64_t max_values, std::vector<RootPrimitiveValue>& values,
                                 std::vector<int32_t>& flat_indices);
    /// Collects the mixed direct-scan row representation.
    static void CollectDirect(void* root_object, const std::vector<PathLevel>& levels, int64_t max_values,
                              int64_t entry_id, ReadResult& out);
};

} // namespace duckdb::rootlake

#endif
