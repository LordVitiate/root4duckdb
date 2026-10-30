#include "duckdb.h"
#include "duckdb_static_extension.h"

#include <cstdint>
#include <iostream>
#include <string>

extern "C" int32_t duckdb_extension_loadable_extensions_describe(duckdb_extension_descriptor *);

namespace
{

struct DatabaseHandle final
{
    duckdb_database database = nullptr;
    duckdb_connection connection = nullptr;

    ~DatabaseHandle()
    {
        if (connection)
        {
            duckdb_disconnect(&connection);
        }
        if (database)
        {
            duckdb_close(&database);
        }
    }
};

struct QueryResult final
{
    duckdb_result value{};

    ~QueryResult()
    {
        duckdb_destroy_result(&value);
    }
};

std::string QuoteSQL(const std::string& value)
{
    std::string quoted = "'";
    for (const char character : value)
    {
        if (character == '\'')
        {
            quoted += "''";
        }
        else
        {
            quoted += character;
        }
    }
    return quoted + "'";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: root_loadable_extension_smoke EXTENSION\n";
        return 2;
    }

    if (duckdb_register_static_extension(duckdb_extension_loadable_extensions_describe) != 0)
    {
        std::cerr << "[ERROR] Cannot register DuckDB extension loader\n";
        return 1;
    }

    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess)
    {
        std::cerr << "[ERROR] Cannot create DuckDB configuration\n";
        return 1;
    }
    if (duckdb_set_config(config, "allow_unsigned_extensions", "true") != DuckDBSuccess)
    {
        duckdb_destroy_config(&config);
        std::cerr << "[ERROR] Cannot allow unsigned extensions\n";
        return 1;
    }

    DatabaseHandle host;
    char* open_error = nullptr;
    const auto status = duckdb_open_ext(":memory:", &host.database, config, &open_error);
    duckdb_destroy_config(&config);
    if (status != DuckDBSuccess)
    {
        std::cerr << "[ERROR] Cannot open DuckDB: " << (open_error ? open_error : "unknown error") << '\n';
    }
    if (open_error)
    {
        duckdb_free(open_error);
    }
    if (status != DuckDBSuccess)
    {
        return 1;
    }
    if (duckdb_connect(host.database, &host.connection) != DuckDBSuccess)
    {
        std::cerr << "[ERROR] Cannot connect to DuckDB\n";
        return 1;
    }

    const std::string load_sql = "LOAD " + QuoteSQL(argv[1]);
    QueryResult load;
    if (duckdb_query(host.connection, load_sql.c_str(), &load.value) != DuckDBSuccess)
    {
        std::cerr << "[ERROR] Cannot load extension: " << duckdb_result_error(&load.value) << '\n';
        return 1;
    }

    QueryResult functions;
    if (duckdb_query(host.connection, R"SQL(
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
    )SQL",
                     &functions.value) != DuckDBSuccess)
    {
        std::cerr << "[ERROR] Cannot inspect extension functions: " << duckdb_result_error(&functions.value) << '\n';
        return 1;
    }

    if (duckdb_row_count(&functions.value) != 1)
    {
        std::cerr << "[ERROR] ROOT4DuckDB function check returned an unexpected number of rows\n";
        return 1;
    }

    const auto registered = duckdb_value_int64(&functions.value, 0, 0);
    if (registered != 6)
    {
        std::cerr << "[ERROR] Expected 6 ROOT4DuckDB functions, found " << registered << '\n';
        return 1;
    }

    std::cout << "[OK] Loadable extension registered all public functions\n";
    return 0;
}
