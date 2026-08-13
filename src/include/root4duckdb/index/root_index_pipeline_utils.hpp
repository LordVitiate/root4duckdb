#pragma once

#include "root4duckdb/index/root_index_pipeline.hpp"

#include <sys/stat.h>

namespace duckdb::rootlake {

/// Reads local file metadata without following remote URIs.
bool LocalFileStat(const std::string& path, struct stat& status);
/// Returns a local file size or zero for remote and unavailable inputs.
uint64_t LocalFileSize(const std::string& path);
/// Returns a local nanosecond mtime or zero when unavailable.
int64_t LocalMtimeNS(const std::string& path);
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
