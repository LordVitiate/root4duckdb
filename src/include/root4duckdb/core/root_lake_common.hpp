#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake {

constexpr uint32_t ROOT_LAKE_INDEX_VERSION = 12;

/// Computes the index format's stable FNV-1a hash.
uint64_t FNV1a64(const void* data, size_t size, uint64_t seed = 14695981039346656037ULL);
uint64_t FNV1a64(const std::string& value, uint64_t seed = 14695981039346656037ULL);
/// Formats an identifier as fixed-width lowercase hexadecimal.
std::string Hex64(uint64_t value);
/// Formats a double without losing round-trip precision.
std::string DoubleText(double value);
/// Escapes a single CSV field.
std::string CsvEscape(const std::string& value);
/// Joins values with the supplied delimiter.
std::string JoinStrings(const std::vector<std::string>& values, const std::string& delimiter);
/// Quotes a string as a SQL literal.
std::string SqlLiteral(const std::string& value);
/// Derives stable physical-file lineage metadata.
std::string FileId(const std::string& uri, uint64_t size, int64_t mtime);
/// Derives a logical column identifier within one schema.
std::string ColumnId(const std::string& schema_id, const std::string& logical_path);
/// Converts an unsuccessful materialized query into a typed error.
void EnsureQueryOK(MaterializedQueryResult& result, const std::string& label);

} // namespace duckdb::rootlake
