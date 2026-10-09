#include "duckdb.h"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdlib>
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

std::string QuoteSQL(const std::string& value);

std::string RootLoadSQL()
{
    const char* extension_path = std::getenv("ROOT4DUCKDB_TEST_EXTENSION_PATH");
    if (!extension_path || !*extension_path)
    {
        return "LOAD root";
    }
    return "LOAD " + QuoteSQL(extension_path);
}

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

bool OpenTestDatabase(duckdb_database& database, std::string& error)
{
    const char* extension_path = std::getenv("ROOT4DUCKDB_TEST_EXTENSION_PATH");
    if (!extension_path || !*extension_path)
    {
        if (duckdb_open(":memory:", &database) == DuckDBSuccess)
        {
            return true;
        }
        error = "could not create DuckDB database";
        return false;
    }

    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess)
    {
        error = "could not create DuckDB test configuration";
        return false;
    }
    if (duckdb_set_config(config, "allow_unsigned_extensions", "true") != DuckDBSuccess)
    {
        duckdb_destroy_config(&config);
        error = "could not allow unsigned ROOT extension in DuckDB test configuration";
        return false;
    }

    char* open_error = nullptr;
    const auto status = duckdb_open_ext(":memory:", &database, config, &open_error);
    duckdb_destroy_config(&config);
    if (status != DuckDBSuccess)
    {
        error = open_error ? open_error : "could not create DuckDB database";
    }
    if (open_error)
    {
        duckdb_free(open_error);
    }
    return status == DuckDBSuccess;
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
                                 if (OpenTestDatabase(database, error) &&
                                     OpenConnection(database, connection, error) &&
                                     ExecuteSQL(connection, RootLoadSQL(), false, error))
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


struct PhastDigest
{
    uint64_t rows = 0;
    uint64_t hash = 0;

    bool operator==(const PhastDigest& other) const
    {
        return rows == other.rows && hash == other.hash;
    }
};

struct PhastCase
{
    const char* path;
    const char* columns;
};

bool ComputePhastDigest(duckdb_connection connection, const std::string& sql, PhastDigest& digest,
                        std::string& error)
{
    duckdb_result result{};
    if (duckdb_query(connection, sql.c_str(), &result) != DuckDBSuccess)
    {
        const char* message = duckdb_result_error(&result);
        error = message ? message : "DuckDB query failed";
        duckdb_destroy_result(&result);
        return false;
    }
    if (duckdb_row_count(&result) != 1 || duckdb_column_count(&result) != 2)
    {
        error = "PHAST digest query did not return exactly two scalar columns";
        duckdb_destroy_result(&result);
        return false;
    }
    digest.rows = duckdb_value_uint64(&result, 0, 0);
    digest.hash = duckdb_value_uint64(&result, 1, 0);
    duckdb_destroy_result(&result);
    if (digest.rows == 0)
    {
        error = "PHAST query returned zero rows";
        return false;
    }
    return true;
}

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

bool RunPhastConcurrency(int argc, char** argv)
{
    if (argc != 6)
    {
        std::cerr << "usage: concurrent_root_load --phast <dictionary> <mDST-1> <mDST-2> <last-entry>\n";
        return false;
    }
    const std::string dictionary = argv[2];
    const std::string files[] = {argv[3], argv[4]};
    const std::string upper_entry = argv[5];
    if (upper_entry.empty() || upper_entry.find_first_not_of("0123456789") != std::string::npos)
    {
        std::cerr << "last-entry must be a nonnegative integer\n";
        return false;
    }
    const PhastCase cases[] = {
        {"/PaSetup", "entry_id, run, ver, experiment"},
        {"/PaEvent/vecTrack/vecTPar/Par", "entry_id, vecTrack_idx, vecTPar_idx, Par_idx, Par"},
        {"/PaEvent/vecTrack/vecTPar/Cov", "entry_id, vecTrack_idx, vecTPar_idx, Cov_idx, Cov"},
        {"/PaEvent/vecVertex", "entry_id, vecVertex_idx, chi2tot, bestprim"}
    };
    const std::string modes[] = {"object", "auto", "serialized"};

    duckdb_database database = nullptr;
    std::string error;
    if (!OpenTestDatabase(database, error))
    {
        std::cerr << "could not create shared DuckDB database: " << error << '\n';
        return false;
    }
    duckdb_connection bootstrap = nullptr;
    const bool initialized = OpenConnection(database, bootstrap, error) &&
                             ExecuteSQL(bootstrap, RootLoadSQL(), false, error);
    if (!initialized)
    {
        std::cerr << "PHAST LOAD failed: " << error << '\n';
        if (bootstrap)
        {
            duckdb_disconnect(&bootstrap);
        }
        duckdb_close(&database);
        return false;
    }

    bool success = true;
    for (const auto& file : files)
    {
        std::cout << "[PHAST FILE] " << file << std::endl;
        for (const auto& test : cases)
        {
            PhastDigest reference{};
            for (const auto& mode : modes)
            {
                const std::string sql =
                    "SELECT count(*)::UBIGINT, coalesce(bit_xor(hash(" + std::string(test.columns) +
                    ")), 0)::UBIGINT FROM read_root(" + QuoteSQL(file) +
                    ", dictionary := " + QuoteSQL(dictionary) +
                    ", path_prefix := " + QuoteSQL(test.path) +
                    ", reader_mode := " + QuoteSQL(mode) +
                    ", raw_validation_entries := 2) WHERE entry_id BETWEEN 0 AND " + upper_entry;
                PhastDigest baseline{};
                error.clear();
                if (!ComputePhastDigest(bootstrap, sql, baseline, error))
                {
                    std::cerr << "[PHAST BASELINE FAIL] path=" << test.path
                              << " mode=" << mode << " " << error << '\n';
                    success = false;
                    continue;
                }
                if (mode == "object")
                {
                    reference = baseline;
                }
                if (!(baseline == reference))
                {
                    std::cerr << "[PHAST MODE MISMATCH] path=" << test.path
                              << " mode=" << mode << " rows=" << baseline.rows
                              << " digest=" << baseline.hash << " expected_rows=" << reference.rows
                              << " expected_digest=" << reference.hash << '\n';
                    success = false;
                    continue;
                }
                std::cout << "[PHAST BASELINE] path=" << test.path << " mode=" << mode
                          << " rows=" << baseline.rows << " digest=" << baseline.hash << '\n';
                for (const int worker_count : {1, 2, 4, 8})
                {
                    std::barrier start(worker_count);
                    Failures failures;
                    std::vector<std::thread> workers;
                    workers.reserve(static_cast<size_t>(worker_count));
                    for (int worker = 0; worker < worker_count; ++worker)
                    {
                        workers.emplace_back([&, worker]()
                        {
                            duckdb_connection connection = nullptr;
                            std::string worker_error;
                            const bool connected = OpenConnection(database, connection, worker_error);
                            start.arrive_and_wait();
                            if (connected)
                            {
                                for (int iteration = 0; iteration < 2; ++iteration)
                                {
                                    PhastDigest observed{};
                                    if (!ComputePhastDigest(connection, sql, observed, worker_error))
                                    {
                                        break;
                                    }
                                    if (!(observed == baseline))
                                    {
                                        worker_error = "inconsistent rows/digest in concurrent ROOT read";
                                        break;
                                    }
                                }
                            }
                            if (connection)
                            {
                                duckdb_disconnect(&connection);
                            }
                            if (!worker_error.empty())
                            {
                                failures.Report(worker, "PHAST read", worker_error);
                            }
                        });
                    }
                    for (auto& worker : workers)
                    {
                        worker.join();
                    }
                    if (failures.count)
                    {
                        success = false;
                    }
                    else
                    {
                        std::cout << "[PHAST OK] path=" << test.path << " mode=" << mode
                                  << " workers=" << worker_count << " validated_reads="
                                  << worker_count * 2 << '\n';
                    }
                }
            }
        }
    }
    duckdb_disconnect(&bootstrap);
    duckdb_close(&database);
    if (success)
    {
        std::cout << "[PASS] deep PHAST reads agree across object/auto/serialized and 1/2/4/8 threads\n";
    }
    return success;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--phast")
    {
        return RunPhastConcurrency(argc, argv) ? 0 : 1;
    }
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
    std::string error;
    if (!OpenTestDatabase(database, error))
    {
        std::cerr << "could not open shared DuckDB database: " << error << '\n';
        return 1;
    }
    duckdb_connection bootstrap = nullptr;
    const bool initialized = OpenConnection(database, bootstrap, error) &&
                             ExecuteSQL(bootstrap, RootLoadSQL(), false, error);
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
