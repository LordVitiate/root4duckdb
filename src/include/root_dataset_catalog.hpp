#pragma once

#include "root_semantic_reader.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb::rootlake {

struct CatalogSources {
    std::string files;
    std::string schemas;
    std::string access;
    std::string baskets;
    std::string snapshots;
    std::string commits;
    std::string catalog_prefix;
    std::string snapshot_id;
    bool sql_tables = false;
};

struct SchemaBinding {
    std::string schema_id;
    std::string column_id;
    std::string root_class;
    std::string root_type;
    std::string access_plan_id;
    std::string index_signature;
    std::vector<PathLevel> expected_levels;
};

struct DatasetSchemaSet {
    std::vector<SchemaBinding> schemas;
    std::unordered_map<std::string, idx_t> lookup;
    std::vector<std::string> index_names;
    LogicalType value_type;
};

CatalogSources ResolveCatalogSources(const std::string &catalog_path,
                                     const named_parameter_map_t &parameters);
void ResolveCommittedSnapshot(ClientContext &context, CatalogSources &sources);
std::string CatalogRelationSQL(const std::string &source, bool sql_tables);
DatasetSchemaSet LoadDatasetSchemas(ClientContext &context,
                                    const CatalogSources &sources,
                                    const std::string &logical_path);
std::vector<SchemaBinding> LoadDatasetPathSchemas(
    ClientContext &context, const CatalogSources &sources,
    const std::string &logical_path, LogicalType &value_type,
    std::unordered_map<std::string, idx_t> &lookup);

} // namespace duckdb::rootlake
