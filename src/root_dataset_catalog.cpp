#include "root_dataset_catalog.hpp"

#include "root_iceberg_catalog.hpp"
#include "root_lake_common.hpp"

#include "duckdb/main/connection.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

namespace duckdb::rootlake {
namespace {

namespace fs = std::filesystem;

bool IsSafeQualifiedName(const std::string &name) {
    if (name.empty()) return false;
    for (const char character : name) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) ||
              character == '_' || character == '.')) {
            return false;
        }
    }
    return true;
}

bool IsInternalIcebergRelation(const std::string &source) {
    if (source.rfind("read_parquet([", 0) == 0) {
        return source.size() >= 2 && source.back() == ')';
    }
    return source.rfind("(SELECT ", 0) == 0 &&
           source.find(" FROM read_parquet([") != std::string::npos &&
           source.size() >= 2 && source.back() == ')';
}

std::vector<std::string> SplitIndexNames(const std::string &signature) {
    std::vector<std::string> result;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty()) result.push_back(name);
    }
    return result;
}

std::vector<PathLevel> LoadAccessPlan(Connection &connection,
                                      const std::string &access_relation,
                                      const std::string &access_plan_id,
                                      const std::string &label) {
    auto result = connection.Query(
        "SELECT DISTINCT field_name, root_type, offset_in_parent, cumulative_offset, "
        "is_pointer, is_container, is_primitive, is_string, is_fixed_array, "
        "array_length, COALESCE(array_dimensions, ''), element_size FROM " +
        access_relation + " WHERE access_plan_id=" + SqlLiteral(access_plan_id) +
        " ORDER BY level_no");
    EnsureQueryOK(*result, label);

    std::vector<PathLevel> levels;
    levels.reserve(result->RowCount());
    for (idx_t row = 0; row < result->RowCount(); ++row) {
        PathLevel level;
        level.name = result->GetValue(0, row).ToString();
        level.type = result->GetValue(1, row).ToString();
        level.offset_in_parent = result->GetValue(2, row).GetValue<int64_t>();
        level.cumulative_offset = result->GetValue(3, row).GetValue<int64_t>();
        level.is_pointer = result->GetValue(4, row).GetValue<bool>();
        level.is_container = result->GetValue(5, row).GetValue<bool>();
        level.is_primitive = result->GetValue(6, row).GetValue<bool>();
        level.is_string = result->GetValue(7, row).GetValue<bool>();
        level.is_fixed_array = result->GetValue(8, row).GetValue<bool>();
        level.fixed_array_length = result->GetValue(9, row).GetValue<uint64_t>();
        const auto dimensions = result->GetValue(10, row).ToString();
        if (!dimensions.empty()) {
            std::stringstream dimension_stream(dimensions);
            std::string dimension;
            while (std::getline(dimension_stream, dimension, 'x')) {
                if (!dimension.empty()) {
                    level.array_dimensions.push_back(
                        static_cast<uint32_t>(std::stoul(dimension)));
                }
            }
        }
        level.element_size = result->GetValue(11, row).GetValue<uint32_t>();
        levels.push_back(std::move(level));
    }
    return levels;
}

std::vector<SchemaBinding> LoadSchemas(
    ClientContext &context, const CatalogSources &sources,
    const std::string &logical_path, const std::string &schema_label,
    const std::string &access_label) {
    Connection connection(*context.db);
    const auto schema_relation =
        CatalogRelationSQL(sources.schemas, sources.sql_tables);
    const auto access_relation =
        CatalogRelationSQL(sources.access, sources.sql_tables);
    auto result = connection.Query(
        "SELECT DISTINCT schema_id, column_id, root_class, root_type, "
        "access_plan_id, COALESCE(index_signature, '') AS index_signature FROM " +
        schema_relation + " WHERE logical_path=" + SqlLiteral(logical_path) +
        " ORDER BY schema_id");
    EnsureQueryOK(*result, schema_label);

    std::vector<SchemaBinding> schemas;
    schemas.reserve(result->RowCount());
    for (idx_t row = 0; row < result->RowCount(); ++row) {
        SchemaBinding schema;
        schema.schema_id = result->GetValue(0, row).ToString();
        schema.column_id = result->GetValue(1, row).ToString();
        schema.root_class = result->GetValue(2, row).ToString();
        schema.root_type = result->GetValue(3, row).ToString();
        schema.access_plan_id = result->GetValue(4, row).ToString();
        schema.index_signature = result->GetValue(5, row).ToString();
        schema.expected_levels = LoadAccessPlan(
            connection, access_relation, schema.access_plan_id, access_label);
        schemas.push_back(std::move(schema));
    }
    return schemas;
}

} // namespace

std::string CatalogRelationSQL(const std::string &source, bool sql_tables) {
    if (sql_tables) {
        if (IsInternalIcebergRelation(source)) return source;
        if (!IsSafeQualifiedName(source)) {
            throw InvalidInputException("Unsafe metadata table name: " + source);
        }
        return source;
    }
    return "read_parquet(" + SqlLiteral(source) + ")";
}

CatalogSources ResolveCatalogSources(
    const std::string &catalog_path,
    const named_parameter_map_t &parameters) {
    CatalogSources sources;
    if (IsRootIcebergCatalog(catalog_path)) {
        sources.files = RootIcebergRelation(catalog_path, "files");
        sources.schemas = RootIcebergRelation(catalog_path, "schemas");
        sources.access = RootIcebergRelation(catalog_path, "access");
        sources.baskets = RootIcebergRelation(catalog_path, "baskets");
        sources.snapshots = RootIcebergCommittedSnapshotsRelation(
            catalog_path, ROOT_LAKE_INDEX_VERSION);
        sources.commits = RootIcebergRelation(catalog_path, "commits");
        sources.sql_tables = true;
        return sources;
    }

    auto named = [&](const char *name) -> std::string {
        const auto entry = parameters.find(name);
        return entry == parameters.end() ? std::string() : entry->second.ToString();
    };
    sources.catalog_prefix = named("catalog_prefix");
    sources.files = named("files_table");
    sources.schemas = named("schemas_table");
    sources.access = named("access_table");
    sources.baskets = named("baskets_table");
    sources.snapshots = named("snapshots_table");
    sources.commits = named("commits_table");
    sources.snapshot_id = named("snapshot_id");

    if (sources.catalog_prefix.empty() && !catalog_path.empty() &&
        IsSafeQualifiedName(catalog_path)) {
        std::error_code error;
        if (!fs::exists(catalog_path, error)) sources.catalog_prefix = catalog_path;
    }
    if (!sources.catalog_prefix.empty()) {
        if (!IsSafeQualifiedName(sources.catalog_prefix)) {
            throw InvalidInputException(
                "Unsafe ROOT metadata catalog prefix: " + sources.catalog_prefix);
        }
        sources.files = sources.catalog_prefix + "_files";
        sources.schemas = sources.catalog_prefix + "_schemas";
        sources.access = sources.catalog_prefix + "_access";
        sources.baskets = sources.catalog_prefix + "_baskets";
        sources.snapshots = sources.catalog_prefix + "_snapshots";
        sources.commits = sources.catalog_prefix + "_commits";
        sources.sql_tables = true;
        return sources;
    }

    const bool any_override = !sources.files.empty() || !sources.schemas.empty() ||
                              !sources.access.empty() || !sources.baskets.empty() ||
                              !sources.snapshots.empty();
    if (any_override) {
        if (sources.files.empty() || sources.schemas.empty() ||
            sources.access.empty() || sources.baskets.empty()) {
            throw InvalidInputException(
                "files_table, schemas_table, access_table and baskets_table must be supplied together");
        }
        sources.sql_tables = true;
        return sources;
    }

    fs::path manifest = catalog_path;
    if (fs::is_directory(manifest)) manifest /= "current.json";
    std::ifstream input(manifest, std::ios::binary);
    if (!input) {
        throw IOException("Cannot open ROOT lake catalog manifest: " +
                          manifest.string());
    }
    nlohmann::json json;
    input >> json;
    if (json.value("format", "") != "root4duckdb-lakehouse") {
        throw InvalidInputException(
            "Unsupported ROOT lake catalog format in " + manifest.string());
    }
    const auto version = json.value("index_version", 0U);
    if (version != ROOT_LAKE_INDEX_VERSION) {
        throw InvalidInputException(
            "ROOT lake index version " + std::to_string(version) +
            " is incompatible with reader version " +
            std::to_string(ROOT_LAKE_INDEX_VERSION) + "; rebuild the index");
    }
    sources.snapshot_id = json.value("snapshot_id", "");
    auto table_path = [&](const char *name) {
        fs::path path(json["tables"][name].get<std::string>());
        if (path.is_relative()) path = manifest.parent_path() / path;
        return path.lexically_normal().string();
    };
    sources.files = table_path("files");
    sources.schemas = table_path("schemas");
    sources.access = table_path("access_levels");
    sources.baskets = table_path("baskets");
    return sources;
}

void ResolveCommittedSnapshot(ClientContext &context, CatalogSources &sources) {
    if (!sources.sql_tables || sources.snapshots.empty() ||
        !sources.snapshot_id.empty()) {
        return;
    }
    Connection connection(*context.db);
    if (!sources.commits.empty()) {
        try {
            auto result = connection.Query(
                "SELECT s.snapshot_id FROM " + sources.snapshots + " s JOIN " +
                sources.commits +
                " c ON c.snapshot_id=s.snapshot_id WHERE s.state='COMMITTED' "
                "AND c.state='COMMITTED' AND s.index_version=" +
                std::to_string(ROOT_LAKE_INDEX_VERSION) +
                " ORDER BY c.committed_at_ns DESC LIMIT 1");
            EnsureQueryOK(*result, "resolve committed ROOT dataset generation");
            if (result->RowCount()) {
                sources.snapshot_id = result->GetValue(0, 0).ToString();
                return;
            }
            throw InvalidInputException(
                "No fully committed ROOT dataset generation in " + sources.commits);
        } catch (const InvalidInputException &) {
            throw;
        } catch (...) {
        }
    }
    auto result = connection.Query(
        "SELECT snapshot_id FROM " + sources.snapshots +
        " WHERE state='COMMITTED' AND index_version=" +
        std::to_string(ROOT_LAKE_INDEX_VERSION) +
        " ORDER BY created_at_ns DESC LIMIT 1");
    EnsureQueryOK(*result, "resolve committed ROOT dataset snapshot");
    if (!result->RowCount()) {
        throw InvalidInputException(
            "No committed ROOT dataset snapshot in " + sources.snapshots);
    }
    sources.snapshot_id = result->GetValue(0, 0).ToString();
}

DatasetSchemaSet LoadDatasetSchemas(ClientContext &context,
                                    const CatalogSources &sources,
                                    const std::string &logical_path) {
    auto schemas = LoadSchemas(context, sources, logical_path,
                               "load ROOT schema metadata",
                               "load ROOT access plan");
    if (schemas.empty()) {
        throw InvalidInputException(
            "Logical path is absent from catalog: " + logical_path);
    }

    std::optional<std::string> common_type;
    std::optional<std::string> common_signature;
    DatasetSchemaSet result;
    for (auto &schema : schemas) {
        if (schema.expected_levels.empty()) {
            throw InvalidInputException(
                "Empty access plan for schema " + schema.schema_id);
        }
        const auto canonical_signature = IndexSignature(schema.expected_levels);
        if (!schema.index_signature.empty() &&
            schema.index_signature != canonical_signature) {
            throw InvalidInputException(
                "Catalog index signature disagrees with access levels for schema " +
                schema.schema_id);
        }
        schema.index_signature = canonical_signature;
        if (!common_type) common_type = schema.root_type;
        if (!common_signature) common_signature = schema.index_signature;
        if (*common_type != schema.root_type) {
            throw InvalidInputException(
                "Catalog contains incompatible ROOT leaf types for " + logical_path);
        }
        if (*common_signature != schema.index_signature) {
            throw InvalidInputException(
                "Catalog contains incompatible vector nesting for " + logical_path);
        }
        result.lookup[schema.schema_id] = result.schemas.size();
        result.schemas.push_back(std::move(schema));
    }
    result.index_names = SplitIndexNames(*common_signature);
    if (!IsLosslessDoubleBackedType(*common_type)) {
        throw NotImplementedException(
            "The indexed lakehouse path currently supports numeric leaves up to 32-bit integers plus FLOAT/DOUBLE; "
            "unsupported leaf type: " + *common_type);
    }
    result.value_type = RootTypeToLogicalType(*common_type);
    return result;
}

std::vector<SchemaBinding> LoadDatasetPathSchemas(
    ClientContext &context, const CatalogSources &sources,
    const std::string &logical_path, LogicalType &value_type,
    std::unordered_map<std::string, idx_t> &lookup) {
    auto schemas = LoadSchemas(context, sources, logical_path,
                               "load predicate ROOT schema metadata",
                               "load predicate ROOT access plan");
    if (schemas.empty()) {
        throw InvalidInputException(
            "Predicate path is absent from the catalog snapshot: " + logical_path);
    }
    std::optional<std::string> common_type;
    for (idx_t i = 0; i < schemas.size(); ++i) {
        const auto &schema = schemas[i];
        if (!common_type) common_type = schema.root_type;
        if (*common_type != schema.root_type) {
            throw InvalidInputException(
                "Predicate path has incompatible ROOT leaf types: " + logical_path);
        }
        lookup[schema.schema_id] = i;
    }
    if (!common_type || !IsLosslessDoubleBackedType(*common_type)) {
        throw NotImplementedException(
            "Path predicates currently require numeric indexed leaves: " +
            logical_path);
    }
    value_type = RootTypeToLogicalType(*common_type);
    return schemas;
}

} // namespace duckdb::rootlake
