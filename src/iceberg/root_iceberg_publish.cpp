#include "root_iceberg_internal.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"

#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/type.h"
#include "iceberg/update/fast_append.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb::rootlake {
namespace fs = std::filesystem;

namespace {

using iceberg::DataFile;
using iceberg::FileFormatType;
using iceberg::Namespace;
using iceberg::PartitionSpec;
using iceberg::Schema;
using iceberg::SchemaField;
using iceberg::SortOrder;
using iceberg::Table;
using iceberg::TableIdentifier;
using iceberg::Type;
using iceberg_internal::AbsolutePath;
using iceberg_internal::Ensure;
using iceberg_internal::EnsureDirectory;
using iceberg_internal::FileUri;
using iceberg_internal::OpenCatalog;
using iceberg_internal::SqlCatalog;
using iceberg_internal::SqlLiteral;
using iceberg_internal::Take;

std::shared_ptr<Type> DuckTypeToIceberg(std::string type) {
    StringUtil::Trim(type);
    type = StringUtil::Upper(type);
    if (type == "BOOLEAN") return std::make_shared<iceberg::BooleanType>();
    if (type == "TINYINT" || type == "SMALLINT" || type == "INTEGER") {
        return std::make_shared<iceberg::IntType>();
    }
    if (type == "UTINYINT" || type == "USMALLINT" || type == "UINTEGER" ||
        type == "BIGINT" || type == "UBIGINT") {
        return std::make_shared<iceberg::LongType>();
    }
    if (type == "FLOAT" || type == "REAL") return std::make_shared<iceberg::FloatType>();
    if (type == "DOUBLE") return std::make_shared<iceberg::DoubleType>();
    if (StringUtil::StartsWith(type, "DECIMAL(")) {
        const auto open = type.find('(');
        const auto comma = type.find(',', open + 1);
        const auto close = type.find(')', comma + 1);
        if (comma == std::string::npos || close == std::string::npos) {
            throw InvalidInputException("Invalid DuckDB decimal type: " + type);
        }
        const auto precision = std::stoi(type.substr(open + 1, comma - open - 1));
        const auto scale = std::stoi(type.substr(comma + 1, close - comma - 1));
        return std::make_shared<iceberg::DecimalType>(precision, scale);
    }
    if (type == "DATE") return std::make_shared<iceberg::DateType>();
    if (type == "TIME" || StringUtil::StartsWith(type, "TIME(")) {
        return std::make_shared<iceberg::TimeType>();
    }
    if (type == "TIMESTAMP" || type == "TIMESTAMP_S" || type == "TIMESTAMP_MS" ||
        type == "TIMESTAMP_NS") {
        return std::make_shared<iceberg::TimestampType>();
    }
    if (type == "TIMESTAMPTZ" || type == "TIMESTAMP WITH TIME ZONE") {
        return std::make_shared<iceberg::TimestampTzType>();
    }
    if (type == "BLOB") return std::make_shared<iceberg::BinaryType>();
    if (type == "VARCHAR" || StringUtil::StartsWith(type, "VARCHAR(")) {
        return std::make_shared<iceberg::StringType>();
    }
    if (type == "UUID") return std::make_shared<iceberg::UuidType>();
    throw NotImplementedException("Cannot publish DuckDB type to Iceberg yet: " + type);
}

struct InferredSchema {
    std::shared_ptr<Schema> schema;
    std::string name_mapping_json;
};

InferredSchema InferParquetSchema(ClientContext &context, const std::string &parquet_path) {
    Connection connection(*context.db);
    auto result = connection.Query(
        "DESCRIBE SELECT * FROM read_parquet(" + SqlLiteral(parquet_path) + ")");
    if (result->HasError()) {
        throw IOException("infer Parquet schema: " + result->GetError());
    }

    std::vector<SchemaField> fields;
    nlohmann::json mapping = nlohmann::json::array();
    fields.reserve(result->RowCount());
    for (idx_t row = 0; row < result->RowCount(); ++row) {
        const auto name = result->GetValue(0, row).ToString();
        const auto type = result->GetValue(1, row).ToString();
        const int32_t field_id = static_cast<int32_t>(row + 1);
        fields.push_back(SchemaField::MakeOptional(field_id, name, DuckTypeToIceberg(type)));
        mapping.push_back({{"field-id", field_id},
                           {"names", nlohmann::json::array({name})}});
    }
    if (fields.empty()) {
        throw IOException("Parquet metadata part has no columns: " + parquet_path);
    }

    auto schema_unique = Take(
        Schema::Make(std::move(fields), Schema::kInitialSchemaId, std::vector<int32_t> {}),
        "build Iceberg schema from Parquet");
    return {std::shared_ptr<Schema>(std::move(schema_unique)), mapping.dump()};
}

uint64_t ParquetRowCount(ClientContext &context, const std::string &parquet_path) {
    Connection connection(*context.db);
    auto result = connection.Query(
        "SELECT count(*) FROM read_parquet(" + SqlLiteral(parquet_path) + ")");
    if (result->HasError()) {
        throw IOException("count Parquet metadata part: " + result->GetError());
    }
    return result->GetValue(0, 0).GetValue<uint64_t>();
}

std::shared_ptr<Table> GetOrCreateTable(const std::shared_ptr<SqlCatalog> &catalog,
                                        const TableIdentifier &identifier,
                                        const InferredSchema &inferred,
                                        const std::string &table_location) {
    const auto exists = Take(catalog->TableExists(identifier),
                             "check Iceberg table " + identifier.ToString());
    if (exists) {
        auto table = Take(catalog->LoadTable(identifier),
                          "load Iceberg table " + identifier.ToString());
        auto current_schema = Take(table->schema(),
                                   "load schema for " + identifier.ToString());
        if (!current_schema->SameSchema(*inferred.schema)) {
            throw IOException("Iceberg schema mismatch for " + identifier.ToString() +
                              "; automatic unsafe schema replacement is forbidden");
        }
        return table;
    }

    std::unordered_map<std::string, std::string> properties {
        {"write.format.default", "parquet"},
        {"schema.name-mapping.default", inferred.name_mapping_json},
        {"root4duckdb.catalog-format", "iceberg-sqlite-v1"},
    };
    return Take(
        catalog->CreateTable(identifier, inferred.schema, PartitionSpec::Unpartitioned(),
                             SortOrder::Unsorted(), table_location, properties),
        "create Iceberg table " + identifier.ToString());
}

RootIcebergTableCommit AppendParquet(ClientContext &context,
                                     const std::shared_ptr<SqlCatalog> &catalog,
                                     const std::string &catalog_root,
                                     const std::string &root_snapshot_id,
                                     const std::string &table_name,
                                     const std::string &source_parquet) {
    if (!fs::is_regular_file(source_parquet)) {
        throw IOException("Missing Parquet metadata part for Iceberg table " + table_name +
                          ": " + source_parquet);
    }

    const auto inferred = InferParquetSchema(context, source_parquet);
    const TableIdentifier identifier {Namespace {{"root_index"}}, table_name};
    const auto table_path = fs::path(AbsolutePath(catalog_root)) /
                            "warehouse" / "root_index" / table_name;
    EnsureDirectory(table_path / "metadata");
    const auto data_path = table_path / "data";
    EnsureDirectory(data_path);
    const auto destination = data_path /
        (root_snapshot_id + "-" + fs::path(source_parquet).filename().string());

    std::error_code error;
    fs::copy_file(source_parquet, destination,
                  fs::copy_options::overwrite_existing, error);
    if (error) throw IOException("Copy Iceberg data file: " + error.message());

    auto table = GetOrCreateTable(catalog, identifier, inferred, FileUri(table_path.string()));
    auto append = Take(table->NewFastAppend(),
                       "create FastAppend for " + identifier.ToString());

    auto data_file = std::make_shared<DataFile>();
    data_file->file_path = FileUri(destination.string());
    data_file->file_format = FileFormatType::kParquet;
    data_file->record_count = static_cast<int64_t>(
        ParquetRowCount(context, destination.string()));
    data_file->file_size_in_bytes = static_cast<int64_t>(fs::file_size(destination));
    data_file->partition_spec_id = PartitionSpec::kInitialSpecId;
    data_file->sort_order_id = SortOrder::kUnsortedOrderId;
    append->AppendFile(data_file);
    Ensure(append->CheckErrors(),
           "append Iceberg data file for " + identifier.ToString());
    Ensure(append->Commit(), "commit FastAppend for " + identifier.ToString());
    Ensure(table->Refresh(), "refresh Iceberg table " + identifier.ToString());

    auto snapshot = Take(table->current_snapshot(),
                         "load current snapshot for " + identifier.ToString());
    RootIcebergTableCommit out;
    out.table_name = table_name;
    out.iceberg_snapshot_id = snapshot->snapshot_id;
    out.metadata_location = std::string(table->metadata_file_location());
    out.manifest_list = snapshot->manifest_list;
    out.record_count = static_cast<uint64_t>(data_file->record_count);
    return out;
}

void WriteSnapshotsParquet(ClientContext &context,
                           const std::string &path,
                           const std::string &root_snapshot_id,
                           const std::vector<RootIcebergTableCommit> &tables) {
    Connection connection(*context.db);
    std::ostringstream values;
    for (idx_t i = 0; i < tables.size(); ++i) {
        if (i) values << ',';
        const auto &table = tables[i];
        values << '(' << SqlLiteral(root_snapshot_id) << ','
               << SqlLiteral(table.table_name) << ',' << table.iceberg_snapshot_id << ','
               << SqlLiteral(table.metadata_location) << ','
               << SqlLiteral(table.manifest_list) << ',' << table.record_count << ')';
    }
    const auto sql =
        "COPY (SELECT col0::VARCHAR AS root_snapshot_id, col1::VARCHAR AS table_name, "
        "col2::BIGINT AS iceberg_snapshot_id, col3::VARCHAR AS metadata_location, "
        "col4::VARCHAR AS manifest_list, col5::UBIGINT AS record_count "
        "FROM (VALUES " + values.str() + ")) TO " + SqlLiteral(path) +
        " (FORMAT PARQUET, COMPRESSION ZSTD)";
    auto result = connection.Query(sql);
    if (result->HasError()) {
        throw IOException("write root_index.snapshots part: " + result->GetError());
    }
}

void WriteCommitParquet(ClientContext &context,
                        const std::string &path,
                        const std::string &root_snapshot_id,
                        const std::string &manifest_fingerprint,
                        const std::string &dictionary_fingerprint) {
    Connection connection(*context.db);
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto sql =
        "COPY (SELECT " + SqlLiteral(root_snapshot_id) + "::VARCHAR AS root_snapshot_id, "
        "'COMMITTED'::VARCHAR AS state, " + SqlLiteral(manifest_fingerprint) +
        "::VARCHAR AS manifest_fingerprint, " + SqlLiteral(dictionary_fingerprint) +
        "::VARCHAR AS dictionary_fingerprint, " + std::to_string(now_ns) +
        "::BIGINT AS committed_at_ns) TO " + SqlLiteral(path) +
        " (FORMAT PARQUET, COMPRESSION ZSTD)";
    auto result = connection.Query(sql);
    if (result->HasError()) {
        throw IOException("write root_index.commits part: " + result->GetError());
    }
}

} // namespace

RootIcebergCommitResult PublishRootIndexStagingToIceberg(
    ClientContext &context,
    const std::string &catalog_root,
    const std::string &root_snapshot_id,
    const std::string &staging_dir,
    const std::string &manifest_fingerprint,
    const std::string &dictionary_fingerprint) {
    if (catalog_root.empty()) throw InvalidInputException("Iceberg catalog folder is required");
    if (root_snapshot_id.empty()) throw InvalidInputException("ROOT snapshot id is required");

    const auto root = fs::path(AbsolutePath(catalog_root));
    EnsureDirectory(root / "warehouse");
    const auto staging = fs::path(AbsolutePath(staging_dir));
    auto catalog = OpenCatalog(root.string());

    const Namespace ns {{"root_index"}};
    const auto ns_exists = Take(catalog->NamespaceExists(ns), "check root_index namespace");
    if (!ns_exists) {
        Ensure(catalog->CreateNamespace(ns, {}), "create root_index namespace");
    }

    RootIcebergCommitResult result;
    result.catalog_root = root.string();
    result.root_snapshot_id = root_snapshot_id;

    const std::vector<std::pair<std::string, std::string>> base_parts {
        {"files", (staging / "root_files.parquet").string()},
        {"schemas", (staging / "root_schemas.parquet").string()},
        {"access", (staging / "root_access_levels.parquet").string()},
        {"baskets", (staging / "root_baskets.parquet").string()},
    };
    for (const auto &[table_name, parquet] : base_parts) {
        result.tables.push_back(AppendParquet(context, catalog, root.string(),
                                              root_snapshot_id, table_name, parquet));
    }

    const auto snapshots_part = (staging / "root_iceberg_snapshots.parquet").string();
    WriteSnapshotsParquet(context, snapshots_part, root_snapshot_id, result.tables);
    result.tables.push_back(AppendParquet(context, catalog, root.string(),
                                          root_snapshot_id, "snapshots", snapshots_part));

    // COMMITTED is deliberately last: it is the reader visibility boundary.
    const auto commits_part = (staging / "root_iceberg_commits.parquet").string();
    WriteCommitParquet(context, commits_part, root_snapshot_id,
                       manifest_fingerprint, dictionary_fingerprint);
    result.tables.push_back(AppendParquet(context, catalog, root.string(),
                                          root_snapshot_id, "commits", commits_part));
    return result;
}

} // namespace duckdb::rootlake
