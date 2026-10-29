#include "root4duckdb/dataset/root_dataset_catalog.hpp"

#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"
#include "root4duckdb/core/root_lake_common.hpp"

#include "duckdb/main/connection.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace duckdb::rootlake
{
namespace
{

namespace fs = std::filesystem;

bool IsSafeQualifiedName(const std::string& name)
{
    if (name.empty())
    {
        return false;
    }
    for (const char character : name)
    {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '.'))
        {
            return false;
        }
    }
    return true;
}

bool IsInternalIcebergRelation(const std::string& source)
{
    if (source.rfind("read_parquet([", 0) == 0)
    {
        return source.size() >= 2 && source.back() == ')';
    }
    return source.rfind("(SELECT ", 0) == 0 && source.find(" FROM read_parquet([") != std::string::npos &&
           source.size() >= 2 && source.back() == ')';
}

bool IsIndexedNumericType(const LogicalType& type)
{
    switch (type.id())
    {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        return true;
    default:
        return false;
    }
}

std::vector<std::string> SplitIndexNames(const std::string& signature)
{
    std::vector<std::string> result;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        if (!name.empty())
        {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<PathLevel> LoadAccessPlan(Connection& connection, const std::string& access_relation,
                                      const std::string& access_plan_id, const std::string& label)
{
    auto result = connection.Query("SELECT DISTINCT field_name, root_type, offset_in_parent, cumulative_offset, "
                                   "is_pointer, is_container, is_primitive, is_string, is_fixed_array, "
                                   "array_length, COALESCE(array_dimensions, ''), element_size FROM " +
                                   access_relation + " WHERE access_plan_id=" + SqlLiteral(access_plan_id) +
                                   " ORDER BY level_no");
    EnsureQueryOK(*result, label);
    std::vector<PathLevel> levels;
    levels.reserve(result->RowCount());
    for (const auto& row : *result)
    {
        PathLevel level;
        level.name = row.GetBaseValue(0).ToString();
        level.type = row.GetBaseValue(1).ToString();
        level.offset_in_parent = row.GetBaseValue(2).GetValue<int64_t>();
        level.cumulative_offset = row.GetBaseValue(3).GetValue<int64_t>();
        level.is_pointer = row.GetBaseValue(4).GetValue<bool>();
        level.is_container = row.GetBaseValue(5).GetValue<bool>();
        level.is_primitive = row.GetBaseValue(6).GetValue<bool>();
        level.is_string = row.GetBaseValue(7).GetValue<bool>();
        level.is_fixed_array = row.GetBaseValue(8).GetValue<bool>();
        level.fixed_array_length = row.GetBaseValue(9).GetValue<uint64_t>();
        const auto dimensions = row.GetBaseValue(10).ToString();
        if (!dimensions.empty())
        {
            std::stringstream dimension_stream(dimensions);
            std::string dimension;
            while (std::getline(dimension_stream, dimension, 'x'))
            {
                if (!dimension.empty())
                {
                    level.array_dimensions.push_back(static_cast<uint32_t>(std::stoul(dimension)));
                }
            }
        }
        level.element_size = row.GetBaseValue(11).GetValue<uint32_t>();
        levels.push_back(std::move(level));
    }
    return levels;
}

std::vector<SchemaBinding> LoadSchemaBindings(ClientContext& context, const CatalogSources& sources,
                                              const std::string& logical_path, const std::string& schema_label,
                                              const std::string& access_label, const std::string& physical_snapshot)
{
    Connection connection(*context.db);
    const auto schema_relation = CatalogRelationSQL(sources.schemas, sources.sql_tables);
    const auto access_relation = CatalogRelationSQL(sources.access, sources.sql_tables);
    std::string filter = " WHERE logical_path=" + SqlLiteral(logical_path);
    if (!sources.path_sources.empty() && !physical_snapshot.empty())
    {
        filter += " AND column_id IN (SELECT DISTINCT column_id FROM " +
                  CatalogRelationSQL(sources.files, sources.sql_tables) +
                  " WHERE snapshot_id=" + SqlLiteral(physical_snapshot) + ")";
    }
    auto result = connection.Query("SELECT DISTINCT schema_id, column_id, root_class, root_type, "
                                   "access_plan_id, COALESCE(index_signature, '') AS index_signature FROM " +
                                   schema_relation + filter + " ORDER BY schema_id");
    EnsureQueryOK(*result, schema_label);
    std::vector<SchemaBinding> schemas;
    schemas.reserve(result->RowCount());
    for (const auto& row : *result)
    {
        SchemaBinding schema;
        schema.schema_id = row.GetBaseValue(0).ToString();
        schema.column_id = row.GetBaseValue(1).ToString();
        schema.root_class = row.GetBaseValue(2).ToString();
        schema.root_type = row.GetBaseValue(3).ToString();
        schema.access_plan_id = row.GetBaseValue(4).ToString();
        schema.index_signature = row.GetBaseValue(5).ToString();
        schema.expected_levels = LoadAccessPlan(connection, access_relation, schema.access_plan_id, access_label);
        schemas.push_back(std::move(schema));
    }
    return schemas;
}

} // namespace

std::string CatalogRelationSQL(const std::string& source, bool sql_tables)
{
    if (sql_tables)
    {
        if (IsInternalIcebergRelation(source))
        {
            return source;
        }
        if (!IsSafeQualifiedName(source))
        {
            throw InvalidInputException("Unsafe metadata table name: " + source);
        }
        return source;
    }
    return "read_parquet(" + SqlLiteral(source) + ")";
}

CatalogSources RootDatasetCatalog::ResolveSources(const std::string& catalog_path,
                                                  const named_argument_map_t& parameters) const
{
    CatalogSources resolved_sources;
    if (IsRootIcebergCatalog(catalog_path))
    {
        Connection connection(*context.db);
        const auto commits = RootIcebergRelation(catalog_path, "commits");
        const auto pinned = parameters.find("snapshot_id");
        std::string root_snapshot;
        if (pinned != parameters.end())
        {
            root_snapshot = pinned->second.ToString();
        }
        auto latest = connection.Query(
            "SELECT root_snapshot_id FROM " + commits + " WHERE state='COMMITTED'" +
            (root_snapshot.empty() ? std::string() : " AND root_snapshot_id=" + SqlLiteral(root_snapshot)) +
            " ORDER BY committed_at_ns DESC LIMIT 1");
        EnsureQueryOK(*latest, "resolve committed Iceberg ROOT index");
        if (latest->RowCount() != 1)
        {
            throw IOException("Requested ROOT Iceberg index generation is not committed");
        }
        resolved_sources.snapshot_id = latest->Fetch()->GetValue(0, 0).ToString();
        const auto snapshots = RootIcebergRelation(catalog_path, "snapshots");
        auto manifest = connection.Query("SELECT table_name, iceberg_snapshot_id FROM " + snapshots +
                                         " WHERE root_snapshot_id=" + SqlLiteral(resolved_sources.snapshot_id));
        EnsureQueryOK(*manifest, "resolve native Iceberg snapshot mapping");
        std::unordered_map<std::string, int64_t> versions;
        for (const auto& row : *manifest)
        {
            const auto name = row.GetBaseValue(0).ToString();
            const auto version = row.GetBaseValue(1).GetValue<int64_t>();
            if (version <= 0 || !versions.emplace(name, version).second)
            {
                throw IOException("Invalid Iceberg metadata snapshot mapping: " + name);
            }
        }
        for (const auto& required : {"files", "schemas", "access", "baskets"})
        {
            if (!versions.count(required))
            {
                throw IOException("Committed ROOT index has incomplete Iceberg table snapshots");
            }
        }
        resolved_sources.files = RootIcebergRelation(catalog_path, "files", versions.at("files"));
        resolved_sources.schemas = RootIcebergRelation(catalog_path, "schemas", versions.at("schemas"));
        resolved_sources.access = RootIcebergRelation(catalog_path, "access", versions.at("access"));
        resolved_sources.baskets = RootIcebergRelation(catalog_path, "baskets", versions.at("baskets"));
        if (versions.count("path_sources"))
        {
            resolved_sources.path_sources =
                RootIcebergRelation(catalog_path, "path_sources", versions.at("path_sources"));
        }
        resolved_sources.sql_tables = true;
        return resolved_sources;
    }
    auto named = [&](const char* name) -> std::string
    {
        const auto entry = parameters.find(name);
        return entry == parameters.end() ? std::string() : entry->second.ToString();
    };
    resolved_sources.catalog_prefix = named("catalog_prefix");
    resolved_sources.files = named("files_table");
    resolved_sources.schemas = named("schemas_table");
    resolved_sources.access = named("access_table");
    resolved_sources.baskets = named("baskets_table");
    resolved_sources.snapshots = named("snapshots_table");
    resolved_sources.commits = named("commits_table");
    resolved_sources.snapshot_id = named("snapshot_id");
    if (resolved_sources.catalog_prefix.empty() && !catalog_path.empty() && IsSafeQualifiedName(catalog_path))
    {
        std::error_code error;
        if (!fs::exists(catalog_path, error))
        {
            resolved_sources.catalog_prefix = catalog_path;
        }
    }
    if (!resolved_sources.catalog_prefix.empty())
    {
        if (!IsSafeQualifiedName(resolved_sources.catalog_prefix))
        {
            throw InvalidInputException("Unsafe ROOT metadata catalog prefix: " + resolved_sources.catalog_prefix);
        }
        resolved_sources.files = resolved_sources.catalog_prefix + "_files";
        resolved_sources.schemas = resolved_sources.catalog_prefix + "_schemas";
        resolved_sources.access = resolved_sources.catalog_prefix + "_access";
        resolved_sources.baskets = resolved_sources.catalog_prefix + "_baskets";
        // SQL table publication uses _snapshots as its commit record.
        // Only require a separate commit ledger when explicitly configured.
        resolved_sources.snapshots = resolved_sources.catalog_prefix + "_snapshots";
        resolved_sources.sql_tables = true;
        return resolved_sources;
    }
    const bool any_override = !resolved_sources.files.empty() || !resolved_sources.schemas.empty() ||
                              !resolved_sources.access.empty() || !resolved_sources.baskets.empty() ||
                              !resolved_sources.snapshots.empty();
    if (any_override)
    {
        if (resolved_sources.files.empty() || resolved_sources.schemas.empty() || resolved_sources.access.empty() ||
            resolved_sources.baskets.empty())
        {
            throw InvalidInputException(
                "files_table, schemas_table, access_table and baskets_table must be supplied together");
        }
        resolved_sources.sql_tables = true;
        return resolved_sources;
    }
    fs::path manifest = catalog_path;
    if (fs::is_directory(manifest))
    {
        manifest /= "current.json";
    }
    std::ifstream input(manifest, std::ios::binary);
    if (!input)
    {
        throw IOException("Cannot open ROOT lake catalog manifest: " + manifest.string());
    }
    nlohmann::json json;
    input >> json;
    if (json.value("format", "") != "root4duckdb-lakehouse")
    {
        throw InvalidInputException("Unsupported ROOT lake catalog format in " + manifest.string());
    }
    const auto version = json.value("index_version", 0U);
    if (version != ROOT_LAKE_INDEX_VERSION)
    {
        throw InvalidInputException("ROOT lake index version " + std::to_string(version) +
                                    " is incompatible with reader version " + std::to_string(ROOT_LAKE_INDEX_VERSION) +
                                    "; rebuild the index");
    }
    resolved_sources.snapshot_id = json.value("snapshot_id", "");
    auto table_path = [&](const char* name)
    {
        fs::path path(json["tables"][name].get<std::string>());
        if (path.is_relative())
        {
            path = manifest.parent_path() / path;
        }
        return path.lexically_normal().string();
    };
    resolved_sources.files = table_path("files");
    resolved_sources.schemas = table_path("schemas");
    resolved_sources.access = table_path("access_levels");
    resolved_sources.baskets = table_path("baskets");
    return resolved_sources;
}

RootDatasetCatalog::RootDatasetCatalog(ClientContext& context_p, const std::string& catalog_path,
                                       const named_argument_map_t& parameters)
    : context(context_p)
{
    sources = ResolveSources(catalog_path, parameters);
    ResolveCommittedSnapshot();
}

RootDatasetCatalog::RootDatasetCatalog(ClientContext& context_p, CatalogSources sources_p)
    : context(context_p), sources(std::move(sources_p))
{
    ResolveCommittedSnapshot();
}

const CatalogSources& RootDatasetCatalog::Sources() const
{
    return sources;
}

std::string RootDatasetCatalog::PathSnapshot(const std::string& logical_path) const
{
    if (sources.path_sources.empty() || sources.snapshot_id.empty())
    {
        return sources.snapshot_id;
    }
    Connection connection(*context.db);
    auto result = connection.Query("SELECT DISTINCT physical_snapshot_id FROM " + sources.path_sources +
                                   " WHERE root_snapshot_id=" + SqlLiteral(sources.snapshot_id) +
                                   " AND logical_path=" + SqlLiteral(logical_path));
    EnsureQueryOK(*result, "resolve indexed ROOT path origin");
    if (result->RowCount() == 1)
    {
        return result->Fetch()->GetValue(0, 0).ToString();
    }
    if (result->RowCount() > 1)
    {
        throw IOException("Conflicting ROOT path origins: " + logical_path);
    }
    auto generation = connection.Query("SELECT count(*) FROM " + sources.path_sources +
                                       " WHERE root_snapshot_id=" + SqlLiteral(sources.snapshot_id));
    EnsureQueryOK(*generation, "validate indexed ROOT generation");
    const auto rows = generation->Fetch();
    if (!rows || rows->size() != 1)
    {
        throw IOException("ROOT path generation lookup returned no result");
    }
    if (rows->GetValue(0, 0).GetValue<uint64_t>() != 0)
    {
        throw InvalidInputException("ROOT path is absent from selected generation: " + logical_path);
    }
    return sources.snapshot_id;
}

void RootDatasetCatalog::ResolveCommittedSnapshot()
{
    if (!sources.sql_tables || sources.snapshots.empty() || !sources.snapshot_id.empty())
    {
        return;
    }
    Connection connection(*context.db);
    if (!sources.commits.empty())
    {
        auto result =
            connection.Query("SELECT s.snapshot_id FROM " + sources.snapshots + " s JOIN " + sources.commits +
                             " c ON c.snapshot_id=s.snapshot_id WHERE s.state='COMMITTED' "
                             "AND c.state='COMMITTED' AND s.index_version=" +
                             std::to_string(ROOT_LAKE_INDEX_VERSION) + " ORDER BY c.committed_at_ns DESC LIMIT 1");
        EnsureQueryOK(*result, "resolve committed ROOT dataset generation");
        if (!result->RowCount())
        {
            throw InvalidInputException("No fully committed ROOT dataset generation in " + sources.commits);
        }
        sources.snapshot_id = result->Fetch()->GetValue(0, 0).ToString();
        return;
    }
    auto result =
        connection.Query("SELECT snapshot_id FROM " + sources.snapshots +
                         " WHERE state='COMMITTED' AND index_version=" + std::to_string(ROOT_LAKE_INDEX_VERSION) +
                         " ORDER BY created_at_ns DESC LIMIT 1");
    EnsureQueryOK(*result, "resolve committed ROOT dataset snapshot");
    if (!result->RowCount())
    {
        throw InvalidInputException("No committed ROOT dataset snapshot in " + sources.snapshots);
    }
    sources.snapshot_id = result->Fetch()->GetValue(0, 0).ToString();
}

DatasetSchemaSet RootDatasetCatalog::LoadSchemas(const std::string& logical_path) const
{
    auto schemas = LoadSchemaBindings(context, sources, logical_path, "load ROOT schema metadata",
                                      "load ROOT access plan", PathSnapshot(logical_path));
    if (schemas.empty())
    {
        throw InvalidInputException("Logical path is absent from catalog: " + logical_path);
    }
    std::optional<std::string> common_type;
    std::optional<std::string> common_signature;
    DatasetSchemaSet result;
    for (auto& schema : schemas)
    {
        if (schema.expected_levels.empty())
        {
            throw InvalidInputException("Empty access plan for schema " + schema.schema_id);
        }
        const auto canonical_signature = IndexSignature(schema.expected_levels);
        if (!schema.index_signature.empty() && schema.index_signature != canonical_signature)
        {
            throw InvalidInputException("Catalog index signature disagrees with access levels for schema " +
                                        schema.schema_id);
        }
        schema.index_signature = canonical_signature;
        if (!common_type)
        {
            common_type = schema.root_type;
        }
        if (!common_signature)
        {
            common_signature = schema.index_signature;
        }
        if (*common_type != schema.root_type)
        {
            throw InvalidInputException("Catalog contains incompatible ROOT leaf types for " + logical_path);
        }
        if (*common_signature != schema.index_signature)
        {
            throw InvalidInputException("Catalog contains incompatible vector nesting for " + logical_path);
        }
        result.lookup[schema.schema_id] = result.schemas.size();
        result.schemas.push_back(std::move(schema));
    }
    result.index_names = SplitIndexNames(*common_signature);
    result.value_type = RootTypeToLogicalType(*common_type);
    if (!IsIndexedNumericType(result.value_type))
    {
        throw NotImplementedException("The indexed lakehouse path requires a primitive numeric leaf; "
                                      "unsupported leaf type: " +
                                      *common_type);
    }
    return result;
}

std::vector<SchemaBinding> RootDatasetCatalog::LoadPathSchemas(const std::string& logical_path, LogicalType& value_type,
                                                               std::unordered_map<std::string, idx_t>& lookup) const
{
    auto schemas = LoadSchemaBindings(context, sources, logical_path, "load predicate ROOT schema metadata",
                                      "load predicate ROOT access plan", PathSnapshot(logical_path));
    if (schemas.empty())
    {
        throw InvalidInputException("Predicate path is absent from the catalog snapshot: " + logical_path);
    }
    std::optional<std::string> common_type;
    for (idx_t i = 0; i < schemas.size(); ++i)
    {
        const auto& schema = schemas[i];
        if (!common_type)
        {
            common_type = schema.root_type;
        }
        if (*common_type != schema.root_type)
        {
            throw InvalidInputException("Predicate path has incompatible ROOT leaf types: " + logical_path);
        }
        lookup[schema.schema_id] = i;
    }
    if (!common_type)
    {
        throw NotImplementedException("Path predicates require a numeric indexed leaf: " + logical_path);
    }
    value_type = RootTypeToLogicalType(*common_type);
    if (!IsIndexedNumericType(value_type))
    {
        throw NotImplementedException("Path predicates require a primitive numeric indexed leaf: " + logical_path);
    }
    return schemas;
}

} // namespace duckdb::rootlake
