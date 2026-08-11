#pragma once
#include "root_headers.hpp"

#include "root_semantic_reader.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb::rootlake {

static constexpr uint32_t ROOT_LAKE_INDEX_VERSION = 12;

inline uint64_t FNV1a64(const void *ptr, size_t size, uint64_t seed = 14695981039346656037ULL) {
    auto bytes = static_cast<const uint8_t *>(ptr);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t FNV1a64(const std::string &value, uint64_t seed = 14695981039346656037ULL) {
    return FNV1a64(value.data(), value.size(), seed);
}

inline std::string Hex64(uint64_t value) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << value;
    return ss.str();
}

inline std::string DoubleText(double value) {
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return ss.str();
}

inline std::string CsvEscape(const std::string &value) {
    bool quote = value.empty() || value.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote) {
        return value;
    }
    std::string out = "\"";
    for (char c : value) {
        if (c == '\"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

inline std::string JoinStrings(const std::vector<std::string> &values, const std::string &delimiter) {
    std::string result;
    for (idx_t i = 0; i < values.size(); ++i) {
        if (i) result += delimiter;
        result += values[i];
    }
    return result;
}

inline std::string SqlLiteral(const std::string &value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}


inline std::string FileId(const std::string &uri, uint64_t size, int64_t mtime) {
    uint64_t hash = FNV1a64(uri);
    hash = FNV1a64(&size, sizeof(size), hash);
    hash = FNV1a64(&mtime, sizeof(mtime), hash);
    return Hex64(hash);
}

inline std::string ColumnId(const std::string &schema_id, const std::string &logical_path) {
    return Hex64(FNV1a64(logical_path, FNV1a64(schema_id)));
}

inline void EnsureQueryOK(MaterializedQueryResult &result, const std::string &label) {
    if (result.HasError()) {
        throw IOException(label + ": " + result.GetError());
    }
}

} // namespace duckdb::rootlake
