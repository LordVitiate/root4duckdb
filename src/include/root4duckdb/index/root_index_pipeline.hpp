#pragma once

#include "root4duckdb/core/root_runtime_settings.hpp"
#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"
#include "root4duckdb/core/root_dictionary.hpp"
#include "root4duckdb/index/root_index_builder.hpp"
#include "root4duckdb/index/root_index_metadata.hpp"
#include "root4duckdb/core/root_input_resolver.hpp"
#include "root4duckdb/core/root_lake_common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>

#include <nlohmann/json.hpp>

namespace duckdb::rootlake {

namespace fs = std::filesystem;

/// Bind-time configuration for one index publication.
struct BuildIndexBindData final : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData& other) const override;
    bool SupportStatementCache() const override;

    std::string root_glob;
    std::string tree_name;
    std::string logical_path;
    std::vector<std::string> logical_paths;
    std::string output_dir;
    std::string catalog_prefix;
    std::string dictionary;
    uint32_t bloom_bytes = 0;
    double bloom_false_positive_rate = 0.01;
    uint32_t index_threads = 0;
    uint32_t max_in_flight_files = 0;
    uint64_t memory_budget_bytes = 0;
    uint64_t estimated_worker_bytes = 512ULL * 1024ULL * 1024ULL;
    uint64_t metadata_flush_bytes = 128ULL * 1024ULL * 1024ULL;
    RootAccessOptions root_access;
    bool has_bloom_bytes = false;
    bool has_index_threads = false;
    bool has_max_in_flight_files = false;
    bool has_memory_budget_bytes = false;
    bool has_estimated_worker_bytes = false;
    bool has_metadata_flush_bytes = false;
    std::string chunk_id;
    std::string manifest_fingerprint;
    std::string dictionary_fingerprint;
    bool overwrite = false;
    bool allow_partial = false;
    std::string files_table;
    std::string schemas_table;
    std::string access_table;
    std::string baskets_table;
    std::string snapshots_table;
    std::string publish_mode = "none";
    std::string catalog_mode = "local";
    bool has_catalog_mode = false;
};

/// Materialized index build result returned by the table function.
struct BuildIndexGlobalState final : public GlobalTableFunctionState {
    std::vector<RootIndexBuildStatus> statuses;
    idx_t offset = 0;
    std::string snapshot_id;
    std::string snapshot_dir;
    uint32_t requested_threads = 0;
    uint32_t effective_threads = 1;
    bool published = false;
    std::string publish_mode = "none";
    std::string chunk_id;
    std::string manifest_fingerprint;
    std::string dictionary_fingerprint;

    idx_t MaxThreads() const override;
};

/// Validates SQL parameters and creates the immutable build request.
class RootIndexBinder final {
  public:
    unique_ptr<FunctionData> Bind(TableFunctionBindInput& input, vector<LogicalType>& return_types,
                                  vector<string>& return_names);
};

/// Publishes validated metadata through Parquet or Iceberg commit paths.
class RootIndexPublisher final {
  public:
    void Publish(ClientContext& context, const BuildIndexBindData& bind, BuildIndexGlobalState& state,
                 const std::string& dataset_id, const fs::path& staging);

  private:
    void PublishTables(DatabaseInstance& database, const fs::path& staging, const BuildIndexBindData& bind,
                       const BuildIndexGlobalState& state, const std::string& dataset_id,
                       const std::string& snapshot_id);
    void CommitParquetSnapshot(const BuildIndexBindData& bind, const std::string& dataset_id,
                               BuildIndexGlobalState& state, const fs::path& staging);
};

/// Runs inspect, build, validate, and publish stages in order.
class RootIndexCoordinator final {
  public:
    unique_ptr<GlobalTableFunctionState> Run(ClientContext& context, TableFunctionInitInput& input);

  private:
    RootIndexBuildOptions ConfigureRuntime(ClientContext& context, BuildIndexBindData& bind,
                                           BuildIndexGlobalState& state, const std::vector<std::string>& files) const;
    std::string DatasetId(const BuildIndexBindData& bind, const std::vector<std::string>& files) const;
    idx_t WorkerCount(BuildIndexBindData& bind, BuildIndexGlobalState& state, idx_t file_count) const;
    std::vector<RootIndexFilePlan> InspectFiles(const RootIndexBuildOptions& options,
                                                const std::vector<std::string>& files, idx_t thread_count,
                                                const BuildIndexBindData& bind, BuildIndexGlobalState& state,
                                                const fs::path& failure_directory) const;
    void BuildFiles(ClientContext& context, const RootIndexBuildOptions& options, const std::string& dataset_id,
                    const fs::path& staging, const std::vector<RootIndexFilePlan>& plans, idx_t thread_count,
                    const BuildIndexBindData& bind, BuildIndexGlobalState& state) const;
    void ValidateBuild(const BuildIndexBindData& bind, const fs::path& failure_directory,
                       BuildIndexGlobalState& state) const;
    void PreserveFailedStaging(const BuildIndexBindData& bind, const fs::path& staging_root, const fs::path& staging,
                               const std::string& snapshot_id) const;
};

/// Emits one status row per indexed file.
class RootIndexResultWriter final {
  public:
    void Write(TableFunctionInput& input, DataChunk& output) const;
};

} // namespace duckdb::rootlake
