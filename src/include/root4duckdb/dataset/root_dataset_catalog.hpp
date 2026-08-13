#pragma once

#include "root4duckdb/reader/root_semantic_reader.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb::rootlake {

/// Physical relations backing one committed dataset catalog.
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

/// Resolved schema and access plan for one logical path.
struct SchemaBinding {
    std::string schema_id;
    std::string column_id;
    std::string root_class;
    std::string root_type;
    std::string access_plan_id;
    std::string index_signature;
    std::vector<PathLevel> expected_levels;
};

/// Compatible schema variants selected for a dataset scan.
struct DatasetSchemaSet {
    std::vector<SchemaBinding> schemas;
    std::unordered_map<std::string, idx_t> lookup;
    std::vector<std::string> index_names;
    LogicalType value_type;
};

/// Builds a table or Parquet relation from a catalog source.
std::string CatalogRelationSQL(const std::string& source, bool sql_tables);

/// Resolves catalog visibility and typed access plans.
class RootDatasetCatalog final {
  public:
    RootDatasetCatalog(ClientContext& context, const std::string& catalog_path,
                       const named_parameter_map_t& parameters);
    RootDatasetCatalog(ClientContext& context, CatalogSources sources);

    /// Returns the committed physical catalog sources.
    const CatalogSources& Sources() const;
    /// Loads all compatible variants of the requested logical path.
    DatasetSchemaSet LoadSchemas(const std::string& logical_path) const;
    /// Loads path schemas while returning their common value type.
    std::vector<SchemaBinding> LoadPathSchemas(const std::string& logical_path, LogicalType& value_type,
                                               std::unordered_map<std::string, idx_t>& lookup) const;

  private:
    CatalogSources ResolveSources(const std::string& catalog_path, const named_parameter_map_t& parameters) const;
    void ResolveCommittedSnapshot();

    ClientContext& context;
    CatalogSources sources;
};

} // namespace duckdb::rootlake
