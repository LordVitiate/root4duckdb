#pragma once

#include <cstdint>
#include <vector>

namespace duckdb {

const uint32_t ROOT_META_MAGIC = 0x54454D52; // "RMET"
const uint32_t ROOT_META_VERSION = 1;

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
// Фиксированная часть метаданных корзины для записи/чтения
struct BinaryBasketHeader {
    uint32_t basket_number;     // Номер корзины
    uint16_t column_index;      // Индекс колонки / Ветки TBranch
    uint64_t start_row;         // Начальная строка в TTree
    uint32_t num_rows;          // Количество строк в этой корзине
    uint64_t physical_offset;   // Физический Офсет в .root файле <-- Наш pread
    uint32_t compressed_size;   // Размер сжатой корзины в байтах
    MetaCompression compression;// Алгоритм сжатия (UINT8)
    
    // Статистика Zonemap
    double min_value;           // Min double/int64
    double max_value;           // Max double/int64
    
    uint32_t bloom_size;        // Размер Блум-фильтра в байтах (UINT32)
};
#pragma pack(pop)

// Полноценная структура корзины в памяти DuckDB
struct BinaryBasketMeta {
    BinaryBasketHeader header;
    std::vector<uint8_t> bloom_filter; // Сырой битовый массив
};

} // namespace duckdb
