#include "include/root_lake_common.hpp"

#include <iomanip>
#include <limits>
#include <sstream>

namespace duckdb::rootlake {

uint64_t FNV1a64(const void *data, size_t size, uint64_t seed) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint64_t hash = seed;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t FNV1a64(const std::string &value, uint64_t seed) {
    return FNV1a64(value.data(), value.size(), seed);
}

std::string Hex64(uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::string DoubleText(double value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value;
    return stream.str();
}

std::string CsvEscape(const std::string &value) {
    const bool quote = value.empty() ||
                       value.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote) return value;

    std::string result = "\"";
    for (const char character : value) {
        result += character == '\"' ? "\"\"" : std::string(1, character);
    }
    result += '\"';
    return result;
}

std::string JoinStrings(const std::vector<std::string> &values,
                        const std::string &delimiter) {
    std::string result;
    for (idx_t index = 0; index < values.size(); ++index) {
        if (index) result += delimiter;
        result += values[index];
    }
    return result;
}

std::string SqlLiteral(const std::string &value) {
    std::string result = "'";
    for (const char character : value) {
        result += character == '\'' ? "''" : std::string(1, character);
    }
    result += '\'';
    return result;
}

std::string FileId(const std::string &uri, uint64_t size, int64_t mtime) {
    uint64_t hash = FNV1a64(uri);
    hash = FNV1a64(&size, sizeof(size), hash);
    hash = FNV1a64(&mtime, sizeof(mtime), hash);
    return Hex64(hash);
}

std::string ColumnId(const std::string &schema_id,
                     const std::string &logical_path) {
    return Hex64(FNV1a64(logical_path, FNV1a64(schema_id)));
}

void EnsureQueryOK(MaterializedQueryResult &result,
                   const std::string &label) {
    if (result.HasError()) {
        throw IOException(label + ": " + result.GetError());
    }
}

} // namespace duckdb::rootlake
