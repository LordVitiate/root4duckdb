#include "duckdb.hpp"
#include "duckdb/main/config.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

/// Quotes a path for a DuckDB LOAD statement.
std::string SqlLiteral(const std::string& value) {
    std::string result = "'";
    result.reserve(value.size() + 2);

    for (const char character : value) {
        if (character == '\'') {
            result += "''";
        } else {
            result += character;
        }
    }

    result += '\'';
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: root_loadable_extension_smoke EXTENSION\n";
        return 2;
    }

    duckdb::DBConfig config;
    config.options.allow_unsigned_extensions = true;

    duckdb::DuckDB database(nullptr, &config);
    duckdb::Connection connection(database);

    auto load = connection.Query("LOAD " + SqlLiteral(argv[1]));
    if (load->HasError()) {
        std::cerr << "[ERROR] Cannot load extension: "
                  << load->GetError() << '\n';
        return 1;
    }

    auto functions = connection.Query(R"SQL(
        SELECT count(DISTINCT function_name)
        FROM duckdb_functions()
        WHERE function_name IN (
            'read_root',
            'root_build_index',
            'root_build_dataset_index',
            'read_root_dataset',
            'root_dataset_stats',
            'root_iceberg_catalog'
        )
    )SQL");

    if (functions->HasError()) {
        std::cerr << "[ERROR] Cannot inspect extension functions: "
                  << functions->GetError() << '\n';
        return 1;
    }

    const auto registered =
        functions->GetValue(0, 0).GetValue<int64_t>();

    if (registered != 6) {
        std::cerr << "[ERROR] Expected 6 ROOT4DuckDB functions, found "
                  << registered << '\n';
        return 1;
    }

    std::cout
        << "[OK] Loadable extension registered all public functions\n";
    return 0;
}
