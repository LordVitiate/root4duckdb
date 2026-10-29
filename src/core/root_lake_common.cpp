#include "root4duckdb/core/root_lake_common.hpp"
#include "root4duckdb/core/root_scan_contract.hpp"

namespace duckdb::rootlake
{

void ConfigureRootRelationScan(TableFunction& function)
{
    function.filter_pushdown = true;
    function.filter_prune = true;
    function.projection_pushdown = true;
    function.order_preservation_type = OrderPreservationType::NO_ORDER;
}

uint64_t FNV1a64(const void* data, size_t size, uint64_t seed)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t FNV1a64(const std::string& value, uint64_t seed)
{
    return FNV1a64(value.data(), value.size(), seed);
}

std::string FileId(const std::string& uri, uint64_t size, int64_t mtime)
{
    uint64_t hash = FNV1a64(uri);
    hash = FNV1a64(&size, sizeof(size), hash);
    hash = FNV1a64(&mtime, sizeof(mtime), hash);
    return Hex64(hash);
}

std::string ColumnId(const std::string& schema_id, const std::string& logical_path)
{
    return Hex64(FNV1a64(logical_path, FNV1a64(schema_id)));
}

void EnsureQueryOK(QueryResult& result, const std::string& label)
{
    if (result.HasError())
    {
        throw IOException(label + ": " + result.GetError());
    }
}

} // namespace duckdb::rootlake
