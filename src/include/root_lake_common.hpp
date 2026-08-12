#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake {

constexpr uint32_t ROOT_LAKE_INDEX_VERSION = 12;

uint64_t FNV1a64(const void *data, size_t size,
                 uint64_t seed = 14695981039346656037ULL);
uint64_t FNV1a64(const std::string &value,
                 uint64_t seed = 14695981039346656037ULL);
std::string Hex64(uint64_t value);
std::string DoubleText(double value);
std::string CsvEscape(const std::string &value);
std::string JoinStrings(const std::vector<std::string> &values,
                        const std::string &delimiter);
std::string SqlLiteral(const std::string &value);
std::string FileId(const std::string &uri, uint64_t size, int64_t mtime);
std::string ColumnId(const std::string &schema_id,
                     const std::string &logical_path);
void EnsureQueryOK(MaterializedQueryResult &result,
                   const std::string &label);

} // namespace duckdb::rootlake
