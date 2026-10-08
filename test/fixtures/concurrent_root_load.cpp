#include "duckdb.h"

#include <atomic>
#include <barrier>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

bool ExecuteSQL(duckdb_connection connection, const std::string& sql, bool require_true, std::string& error)
{
    duckdb_result result{};
    const auto status = duckdb_query(connection, sql.c_str(), &result);
    if (status != DuckDBSuccess)
    {
        const auto* message = duckdb_result_error(&result);
        error = message ? message : "unknown DuckDB query failure";
        duckdb_destroy_result(&result);
        return false;
    }
    if (require_true && (duckdb_row_count(&result) != 1 || !duckdb_value_boolean(&result, 0, 0)))
    {
        error = "ROOT query returned an incorrect result";
        duckdb_destroy_result(&result);
        return false;
    }
    duckdb_destroy_result(&result);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: concurrent_root_load <root-file> <dictionary>\n";
        return 2;
    }

    constexpr int worker_count = 8;
    constexpr int queries_per_worker = 6;
    const std::string root_file = argv[1];
    const std::string dictionary = argv[2];
    const std::string query = "SELECT count(*) = 3 AND sum(run) = 303 FROM read_root('" + root_file +
                              "', dictionary := '" + dictionary + "', path_prefix := '/TestEvent')";

    std::barrier start(worker_count);
    std::atomic<int> failures{0};
    std::mutex output_mutex;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (int index = 0; index < worker_count; ++index)
    {
        workers.emplace_back([&, index]()
                             {
                                 start.arrive_and_wait();
                                 duckdb_database database = nullptr;
                                 duckdb_connection connection = nullptr;
                                 std::string error;

                                 if (duckdb_open(":memory:", &database) != DuckDBSuccess)
                                 {
                                     error = "could not create an independent DuckDB database";
                                 }
                                 else if (duckdb_connect(database, &connection) != DuckDBSuccess)
                                 {
                                     error = "could not connect to an independent DuckDB database";
                                 }
                                 else if (!ExecuteSQL(connection, "LOAD root", false, error))
                                 {
                                     error = "concurrent extension LOAD failed: " + error;
                                 }
                                 else
                                 {
                                     for (int iteration = 0; iteration < queries_per_worker; ++iteration)
                                     {
                                         if (!ExecuteSQL(connection, query, true, error))
                                         {
                                             error = "concurrent ROOT scan failed: " + error;
                                             break;
                                         }
                                     }
                                 }

                                 if (connection)
                                 {
                                     duckdb_disconnect(&connection);
                                 }
                                 if (database)
                                 {
                                     duckdb_close(&database);
                                 }
                                 if (!error.empty())
                                 {
                                     const std::lock_guard<std::mutex> guard(output_mutex);
                                     std::cerr << "worker " << index << ": " << error << '\n';
                                     ++failures;
                                 }
                             });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    if (failures)
    {
        return 1;
    }
    std::cout << "[OK] Concurrent ROOT LOAD: " << worker_count << " databases, "
              << worker_count * queries_per_worker << " real ROOT reads\n";
    return 0;
}
