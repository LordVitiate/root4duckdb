#include "root_iceberg_internal.hpp"

#include "iceberg/arrow/arrow_register.h"
#include "iceberg/avro/avro_register.h"
#include "iceberg/catalog/sql/sql_catalog.h"
#include "iceberg/file_io_registry.h"
#include "iceberg/parquet/parquet_register.h"

#include <mutex>
#include <system_error>

namespace duckdb::rootlake::iceberg_internal {

namespace fs = std::filesystem;

void Ensure(iceberg::Status status, const std::string &operation) {
    if (!status) {
        throw IOException(operation + ": " + status.error().message);
    }
}

std::string SqlLiteral(const std::string &value) {
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string AbsolutePath(const std::string &path) {
    std::error_code error;
    auto absolute = fs::absolute(fs::path(path), error);
    if (error) {
        throw IOException("Cannot resolve absolute path: " + path + ": " + error.message());
    }
    return absolute.lexically_normal().string();
}

std::string FileUri(const std::string &path) {
    return "file://" + fs::path(AbsolutePath(path)).generic_string();
}

void EnsureDirectory(const fs::path &path) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error) {
        throw IOException("Cannot create directory " + path.string() + ": " + error.message());
    }
}

static void RegisterBackends() {
    static std::once_flag once;
    std::call_once(once, [] {
        iceberg::arrow::RegisterAll();
        iceberg::avro::RegisterAll();
        iceberg::parquet::RegisterAll();
    });
}

std::shared_ptr<iceberg::FileIO> MakeLocalFileIO() {
    RegisterBackends();
    auto unique_io = Take(
        iceberg::FileIORegistry::Load(
            std::string(iceberg::FileIORegistry::kArrowLocalFileIO), {}),
        "load Apache Iceberg local FileIO");
    return std::shared_ptr<iceberg::FileIO>(std::move(unique_io));
}

std::shared_ptr<SqlCatalog> OpenCatalog(const std::string &catalog_root) {
    const auto root = fs::path(AbsolutePath(catalog_root));
    EnsureDirectory(root);
    EnsureDirectory(root / "warehouse");

    iceberg::sql::SqlCatalogConfig config;
    config.name = "root4duckdb";
    config.uri = (root / "catalog.sqlite").string();
    config.warehouse_location = FileUri((root / "warehouse").string());
    config.max_connections = 1;

    return Take(SqlCatalog::MakeSqliteCatalog(config, MakeLocalFileIO()),
                "open Apache Iceberg SQLite SqlCatalog");
}

} // namespace duckdb::rootlake::iceberg_internal
