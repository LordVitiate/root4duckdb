#pragma once

#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"

#include "duckdb/common/exception.hpp"

#include "iceberg/catalog/sql/sql_catalog.h"
#include "iceberg/file_io.h"
#include "iceberg/result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace duckdb::rootlake::iceberg_internal {

using SqlCatalog = iceberg::sql::SqlCatalog;

/// Unwraps an Iceberg result as a DuckDB I/O failure.
template <class T> T Take(iceberg::Result<T> result, const std::string& operation) {
    if (!result) {
        throw IOException(operation + ": " + result.error().message);
    }
    return std::move(result).value();
}

/// Converts an unsuccessful Iceberg status into a DuckDB I/O failure.
void Ensure(iceberg::Status status, const std::string& operation);

/// Quotes a value for Iceberg SQL catalog statements.
std::string SqlLiteral(const std::string& value);
/// Returns an absolute normalized filesystem path.
std::string AbsolutePath(const std::string& path);
/// Converts a local path into a file URI.
std::string FileUri(const std::string& path);
/// Creates a directory or reports a typed I/O failure.
void EnsureDirectory(const std::filesystem::path& path);
/// Creates the local Iceberg FileIO implementation.
std::shared_ptr<iceberg::FileIO> MakeLocalFileIO();
/// Opens the SQLite-backed Iceberg SQL catalog.
std::shared_ptr<SqlCatalog> OpenCatalog(const std::string& catalog_root);

} // namespace duckdb::rootlake::iceberg_internal
