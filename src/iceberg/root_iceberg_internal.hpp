#pragma once

#include "root_iceberg_catalog.hpp"

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

template <class T>
T Take(iceberg::Result<T> result, const std::string &operation) {
    if (!result) {
        throw IOException(operation + ": " + result.error().message);
    }
    return std::move(result).value();
}

void Ensure(iceberg::Status status, const std::string &operation);

std::string SqlLiteral(const std::string &value);
std::string AbsolutePath(const std::string &path);
std::string FileUri(const std::string &path);
void EnsureDirectory(const std::filesystem::path &path);
std::shared_ptr<iceberg::FileIO> MakeLocalFileIO();
std::shared_ptr<SqlCatalog> OpenCatalog(const std::string &catalog_root);

} // namespace duckdb::rootlake::iceberg_internal
