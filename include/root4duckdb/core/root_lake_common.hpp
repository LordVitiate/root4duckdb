#ifndef ROOT4DUCKDB_CORE_ROOT_LAKE_COMMON_HPP
#define ROOT4DUCKDB_CORE_ROOT_LAKE_COMMON_HPP

#include "duckdb.hpp"
#include "root4duckdb/core/root_text.hpp"
#include "duckdb/common/exception.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake
{

constexpr uint32_t ROOT_LAKE_INDEX_VERSION = 12;

/// Computes the index format's stable FNV-1a hash.
uint64_t FNV1a64(const void* data, size_t size, uint64_t seed = 14695981039346656037ULL);
uint64_t FNV1a64(const std::string& value, uint64_t seed = 14695981039346656037ULL);
/// Derives stable physical-file lineage metadata.
std::string FileId(const std::string& uri, uint64_t size, int64_t mtime);
/// Derives a logical column identifier within one schema.
std::string ColumnId(const std::string& schema_id, const std::string& logical_path);
/// Converts an unsuccessful query into a typed error.
void EnsureQueryOK(QueryResult& result, const std::string& label);

} // namespace duckdb::rootlake

#endif
