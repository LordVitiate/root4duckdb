#pragma once

#include "root4duckdb/reader/root_semantic_types.hpp"

namespace duckdb::rootlake {

/// Resolves a logical path through ROOT streamer metadata.
class PathResolver {
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

/// Selects terminal primitive children for a semantic prefix.
bool SelectSemanticPath(TClass* root_class, const ParsedPath& path, const std::string& raw_path,
                        SemanticPathSelection& selection);

/// Traverses materialized ROOT objects using resolved offsets.
class OffsetValueReader {
  public:
    /// Collects numeric values without index columns.
    static void CollectValues(void* root_object, const std::vector<PathLevel>& levels, std::vector<double>& out);
    /// Collects numeric values and flattened indices.
    static void CollectFlat(void* root_object, const std::vector<PathLevel>& levels, idx_t index_depth,
                            std::vector<double>& values, std::vector<int32_t>& flat_indices);
    /// Collects the mixed direct-scan row representation.
    static void CollectDirect(void* root_object, const std::vector<PathLevel>& levels, int64_t max_values,
                              int64_t event_id, ReadResult& out);
};

} // namespace duckdb::rootlake
