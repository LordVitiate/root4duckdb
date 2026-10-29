#ifndef ROOT4DUCKDB_INDEX_ROOT_INDEX_PIPELINE_UTILS_HPP
#define ROOT4DUCKDB_INDEX_ROOT_INDEX_PIPELINE_UTILS_HPP

#include "root4duckdb/index/root_index_pipeline.hpp"

#include "root4duckdb/core/root_file_metadata.hpp"

namespace duckdb::rootlake
{

/// Creates a sortable timestamp identifier.
std::string TimestampId();
/// Parses one or more requested logical paths.
std::vector<std::string> ParseLogicalPaths(const std::string& raw);
/// Serializes logical paths deterministically.
std::string LogicalPathsJSON(const std::vector<std::string>& paths);
/// Computes the dictionary or manifest file fingerprint.
std::string FileContentFingerprint(const std::string& path);
/// Computes the stable identity of the indexed input set.
std::string ManifestFingerprint(const std::vector<std::string>& files);
/// Writes the fail-closed file-level build report.
void WriteFailureReport(const fs::path& output_dir, const std::string& snapshot_id,
                        const std::vector<RootIndexBuildStatus>& statuses);
/// Accepts only fixed metadata table identifiers.
bool IsSafePublishTableName(const std::string& name);

} // namespace duckdb::rootlake

#endif
