#pragma once

#include <cstdint>
#include <vector>

namespace duckdb {

constexpr uint32_t ROOT_META_MAGIC = 0x54454D52;
constexpr uint32_t ROOT_META_VERSION = 1;

enum class MetaColumnType : uint8_t {
    INT32 = 1,
    FLOAT = 2,
    DOUBLE = 3,
    INT64 = 4,
    UNKNOWN = 0
};

enum class MetaCompression : uint8_t {
    NONE = 0,
    ZSTD = 1,
    LZMA = 2,
    LZ4  = 3
};

#pragma pack(push, 1)
struct BinaryBasketHeader {
    uint32_t basket_number;
    uint16_t column_index;
    uint64_t start_row;
    uint32_t num_rows;
    uint64_t physical_offset;
    uint32_t compressed_size;
    MetaCompression compression;
    double min_value;
    double max_value;
    uint32_t bloom_size;
};
#pragma pack(pop)

struct BinaryBasketMeta {
    BinaryBasketHeader header;
    std::vector<uint8_t> bloom_filter;
};

} // namespace duckdb
