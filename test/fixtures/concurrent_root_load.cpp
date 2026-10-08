#include "duckdb.h"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int WORKER_COUNT = 8;

struct TestQueries
{
    std::string scalar;
    std::string nested;
    std::string object;
    std::string index;
};

struct Failures
{
    std::atomic<int> count{0};
    std::mutex output_mutex;

    void Report(int worker, const std::string& stage, const std::string& error)
    {
        const std::lock_guard<std::mutex> lock(output_mutex);
        std::cerr << stage << " worker " << worker << ": " << error << '\n';
        ++count;
    }
};

bool ExecuteSQL(duckdb_connection connection, const std::string& sql, bool require_true, std::string& error)
{
    duckdb_result result{};
    const auto status = duckdb_query(connection, sql.c_str(), &result);
    if (status != DuckDBSuccess)
    {
        const char* message = duckdb_result_error(&result);
        error = message ? message : "unknown DuckDB query failure";
        duckdb_destroy_result(&result);
        return false;
    }
    if (require_true && (duckdb_row_count(&result) != 1 || !duckdb_value_boolean(&result, 0, 0)))
    {
        error = "query returned an incorrect result";
        duckdb_destroy_result(&result);
        return false;
    }
    duckdb_destroy_result(&result);
    return true;
}

bool OpenConnection(duckdb_database database, duckdb_connection& connection, std::string& error)
{
    if (duckdb_connect(database, &connection) == DuckDBSuccess)
    {
        return true;
    }
    error = "could not connect to DuckDB database";
    return false;
}

bool RunIndependentDatabases(const TestQueries& queries)
{
    constexpr int scans_per_worker = 6;
    std::barrier start(WORKER_COUNT);
    Failures failures;
    std::vector<std::thread> workers;
    workers.reserve(WORKER_COUNT);

    for (int worker = 0; worker < WORKER_COUNT; ++worker)
    {
        workers.emplace_back([&, worker]()
                             {
                                 start.arrive_and_wait();
                                 duckdb_database database = nullptr;
                                 duckdb_connection connection = nullptr;
                                 std::string error;
                                 if (duckdb_open(":memory:", &database) != DuckDBSuccess)
                                 {
                                     error = "could not create independent DuckDB database";
                                 }
                                 else if (OpenConnection(database, connection, error) &&
                                          ExecuteSQL(connection, "LOAD root", false, error))
                                 {
                                     for (int iteration = 0; iteration < scans_per_worker; ++iteration)
                                     {
                                         if (!ExecuteSQL(connection, queries.scalar, true, error))
                                         {
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
                                     failures.Report(worker, "independent LOAD", error);
                                 }
                             });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    if (failures.count)
    {
        return false;
    }
    std::cout << "[OK] Concurrent ROOT LOAD: 8 databases, 48 real ROOT reads\n";
    return true;
}

bool RunSharedDatabase(duckdb_database database, const TestQueries& queries, bool concurrent_index)
{
    constexpr int scans_per_reader = 72;
    std::barrier start(WORKER_COUNT);
    Failures failures;
    std::vector<std::thread> workers;
    workers.reserve(WORKER_COUNT);
    const char* label = concurrent_index ? "shared DB/index" : "shared DB";

    for (int worker = 0; worker < WORKER_COUNT; ++worker)
    {
        workers.emplace_back([&, worker]()
                             {
                                 duckdb_connection connection = nullptr;
                                 std::string error;
                                 const bool connected = OpenConnection(database, connection, error);
                                 start.arrive_and_wait();
                                 if (connected)
                                 {
                                     if (concurrent_index && worker == 0)
                                     {
                                         ExecuteSQL(connection, queries.index, true, error);
                                     }
                                     else
                                     {
                                         for (int iteration = 0; iteration < scans_per_reader; ++iteration)
                                         {
                                             const auto& query = iteration % 3 == 0 ? queries.scalar :
                                                                 iteration % 3 == 1 ? queries.nested : queries.object;
                                             if (!ExecuteSQL(connection, query, true, error))
                                             {
                                                 break;
                                             }
                                         }
                                     }
                                 }
                                 if (connection)
                                 {
                                     duckdb_disconnect(&connection);
                                 }
                                 if (!error.empty())
                                 {
                                     failures.Report(worker, label, error);
                                 }
                             });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    if (failures.count)
    {
        return false;
    }
    const int readers = concurrent_index ? WORKER_COUNT - 1 : WORKER_COUNT;
    std::cout << "[OK] Concurrent ROOT " << label << ": " << WORKER_COUNT << " connections, "
              << readers * scans_per_reader << " validated reads"
              << (concurrent_index ? " + concurrently dispatched index build" : "") << '\n';
    return true;
}

uint64_t ResidentMemoryKiB()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    uint64_t kib = 0;
    std::string unit;
    while (status >> key)
    {
        if (key == "VmRSS:")
        {
            status >> kib >> unit;
            return kib;
        }
        std::getline(status, unit);
    }
    return 0;
}

bool RunMemoryProbe(duckdb_database database, const TestQueries& queries)
{
    using Counter = std::uint64_t (*)();
    const auto constructed = reinterpret_cast<Counter>(dlsym(RTLD_DEFAULT, "root4duckdb_fixture_constructed_events"));
    const auto destroyed = reinterpret_cast<Counter>(dlsym(RTLD_DEFAULT, "root4duckdb_fixture_destroyed_events"));
    if (!constructed || !destroyed)
    {
        std::cerr << "memory probe: fixture object lifecycle counters are unavailable\n";
        return false;
    }
    duckdb_connection connection = nullptr;
    std::string error;
    if (!OpenConnection(database, connection, error))
    {
        std::cerr << "memory probe: " << error << '\n';
        return false;
    }
    constexpr int warmup = 50;
    constexpr int batch = 750;
    const auto initial_created = constructed();
    const auto initial_destroyed = destroyed();
    std::uint64_t samples[3] = {};
    bool success = true;
    for (int iteration = 0; iteration < warmup + batch * 2; ++iteration)
    {
        if (!ExecuteSQL(connection, queries.object, true, error))
        {
            success = false;
            break;
        }
        if (iteration + 1 == warmup)
        {
            samples[0] = ResidentMemoryKiB();
        }
        else if (iteration + 1 == warmup + batch)
        {
            samples[1] = ResidentMemoryKiB();
        }
        else if (iteration + 1 == warmup + batch * 2)
        {
            samples[2] = ResidentMemoryKiB();
        }
    }
    duckdb_disconnect(&connection);
    if (!success)
    {
        std::cerr << "memory probe: " << error << '\n';
        return false;
    }
    const auto created_delta = constructed() - initial_created;
    const auto destroyed_delta = destroyed() - initial_destroyed;
    if (created_delta < warmup + batch * 2 || created_delta != destroyed_delta)
    {
        std::cerr << "ROOT object ownership violation: created=" << created_delta
                  << ", destroyed=" << destroyed_delta << "\n";
        return false;
    }
    std::cout << "[OK] ROOT object lifetime: " << created_delta << " constructed, "
              << destroyed_delta << " destroyed during 1550 queries\n";
    if (samples[0] && samples[1] && samples[2])
    {
        std::cout << "[MEMORY] ROOT object-mode VmRSS KiB after 50/800/1550 queries: "
                  << samples[0] << '/' << samples[1] << '/' << samples[2] << '\n';
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: concurrent_root_load <root-file> <dictionary> <index-output-dir>\n";
        return 2;
    }
    const std::string file = argv[1];
    const std::string dictionary = argv[2];
    const std::string index_output_dir = argv[3];
    const std::string options = "'" + file + "', dictionary := '" + dictionary + "'";
    const TestQueries queries{
        "SELECT count(*) = 3 AND sum(run) = 303 FROM read_root(" + options + ", path_prefix := '/TestEvent')",
        "SELECT count(*) = 5 AND abs(sum(u) - 53.0) < 0.00001 FROM read_root(" + options +
            ", path_prefix := '/TestEvent/vecHit')",
        "SELECT count(*) = 3 AND sum(run) = 303 FROM read_root(" + options +
            ", path_prefix := '/TestEvent', reader_mode := 'object')",
        "SELECT count(*) = 1 AND bool_and(status = 'OK') FROM root_build_index('" + file +
            "', 'Events', '[\"/TestEvent/run\",\"/TestEvent/vecHit/u\"]', '" + index_output_dir +
            "', dictionary := '" + dictionary + "', index_threads := 1, allow_partial := false)"};

    if (!RunIndependentDatabases(queries))
    {
        return 1;
    }

    duckdb_database database = nullptr;
    if (duckdb_open(":memory:", &database) != DuckDBSuccess)
    {
        std::cerr << "could not open shared DuckDB database\n";
        return 1;
    }
    duckdb_connection bootstrap = nullptr;
    std::string error;
    const bool initialized = OpenConnection(database, bootstrap, error) &&
                             ExecuteSQL(bootstrap, "LOAD root", false, error);
    if (bootstrap)
    {
        duckdb_disconnect(&bootstrap);
    }
    if (!initialized)
    {
        std::cerr << "shared database initialization failed: " << error << '\n';
        duckdb_close(&database);
        return 1;
    }
    const bool shared_reads = RunSharedDatabase(database, queries, false);
    const bool index_race = shared_reads && RunSharedDatabase(database, queries, true);
    const bool memory_probe = index_race && RunMemoryProbe(database, queries);
    duckdb_close(&database);
    return shared_reads && index_race && memory_probe ? 0 : 1;
}
