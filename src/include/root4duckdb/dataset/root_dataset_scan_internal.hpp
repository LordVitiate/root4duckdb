#pragma once

#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/core/root_runtime_settings.hpp"
#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"
#include "root4duckdb/index/root_bloom.hpp"
#include "root4duckdb/reader/root_branch_projection.hpp"
#include "root4duckdb/dataset/root_dataset_catalog.hpp"
#include "root4duckdb/core/root_dictionary.hpp"
#include "root4duckdb/index/root_filter.hpp"
#include "root4duckdb/core/root_lake_common.hpp"
#include "root4duckdb/reader/root_path_reader.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

#include <cstdlib>
#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace duckdb::rootlake {

namespace fs = std::filesystem;

/// Comparison supported by an auxiliary path predicate.
enum class PathPredicateOp { EQ, NE, LT, LE, GT, GE, BETWEEN, IN };

/// Bound predicate evaluated against a decoded semantic path.
struct PathPredicateBinding {
    std::string path;
    PathPredicateOp op = PathPredicateOp::EQ;
    std::vector<RootPrimitiveValue> values;
    bool require_all_values = false;
    LogicalType value_type;
    std::vector<SchemaBinding> schemas;
    std::unordered_map<std::string, idx_t> schema_lookup;
};

/// Half-open ROOT entry interval.
struct EntryInterval {
    uint64_t begin = 0;
    uint64_t end = 0;
};

/// One physically pruned basket range scheduled for decoding.
struct RootBasketTask {
    std::string file_id;
    std::string root_uri;
    std::string tree_name;
    std::string schema_id;
    uint64_t event_base = 0;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
    uint64_t flat_value_begin = 0;
    uint64_t value_count = 0;
    uint64_t physical_offset = 0;
    uint64_t compressed_size = 0;
    uint32_t basket_id = 0;
    uint32_t basket_count = 1;
};

/// Contiguous tasks sharing one open ROOT file.
struct RootFileTaskGroup {
    idx_t begin = 0;
    idx_t end = 0;
};

/// Immutable bind-time plan for read_root_dataset.
struct DatasetBindData final : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData& other) const override;
    bool SupportStatementCache() const override;

    std::string catalog_path;
    std::string logical_path;
    std::string dictionary;
    CatalogSources sources;
    std::vector<SchemaBinding> schemas;
    std::unordered_map<std::string, idx_t> schema_lookup;
    std::vector<std::string> index_names;
    std::vector<PathPredicateBinding> path_predicates;
    LogicalType value_type;
    idx_t value_column = 0;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    RootReaderMode reader_mode = RootReaderMode::AUTO;
    uint32_t raw_validation_entries = 0;
    uint64_t raw_max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t raw_max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t coalesce_gap_bytes = 64ULL * 1024ULL;
    uint32_t prefetch_depth = 2;
    bool prefetch_ranges = true;
    bool require_fresh_index = true;
    RootDictionaryCleanupMode dictionary_cleanup_mode = RootDictionaryCleanupMode::FULL;
    std::unordered_map<std::string, std::vector<EntryInterval>> entry_selection;
    bool entry_selection_active = false;
    uint32_t max_open_files = 0;
    uint64_t memory_budget_bytes = 0;
    uint64_t estimated_reader_bytes = 512ULL * 1024ULL * 1024ULL;
    uint64_t row_limit = std::numeric_limits<uint64_t>::max();
    idx_t source_id_column = 0;
    idx_t entry_id_column = 0;
    idx_t estimated_cardinality = 0;
};

/// Shared task plan, counters, and pushed filters.
struct DatasetGlobalState final : public GlobalTableFunctionState {
    std::vector<RootBasketTask> tasks;
    std::vector<RootFileTaskGroup> task_groups;
    std::atomic<idx_t> next_group{0};
    vector<column_t> scan_column_ids;
    vector<column_t> output_column_ids;
    unique_ptr<TableFilterSet> filters;
    uint64_t planned_compressed_bytes = 0;
    uint64_t scheduled_read_bytes = 0;
    uint64_t planned_values = 0;
    uint64_t planned_baskets = 0;
    uint64_t catalog_files = 0;
    uint64_t planned_files = 0;
    uint64_t skipped_files = 0;
    uint64_t catalog_baskets = 0;
    uint64_t skipped_baskets = 0;
    idx_t worker_limit = 1;
    std::atomic<idx_t> completed_tasks{0};
    std::atomic<uint64_t> opened_files{0};
    std::atomic<uint64_t> reused_open_files{0};
    std::atomic<uint64_t> get_entry_calls{0};
    std::atomic<uint64_t> serialized_entry_calls{0};
    std::atomic<uint64_t> serialized_values{0};
    std::atomic<uint64_t> serialized_baskets{0};
    std::atomic<uint64_t> serialized_compressed_bytes{0};
    std::atomic<uint64_t> serialized_entry_bytes{0};
    std::atomic<uint64_t> projected_files{0};
    std::atomic<uint64_t> fallback_files{0};
    std::atomic<uint64_t> decoded_values{0};
    std::atomic<uint64_t> emitted_rows{0};
    uint64_t row_limit = std::numeric_limits<uint64_t>::max();
    std::atomic<bool> stop_requested{false};
    bool has_event_range = false;
    bool event_range_impossible = false;
    uint64_t event_lower = 0;
    uint64_t event_upper = std::numeric_limits<uint64_t>::max();
    std::atomic<uint64_t> skipped_entries{0};
    bool metadata_count_only = false;
    std::atomic<uint64_t> metadata_rows_emitted{0};
    std::unordered_map<std::string, std::vector<EntryInterval>> candidate_intervals;
    uint64_t predicate_index_baskets = 0;
    uint64_t predicate_intersections = 0;
    uint64_t bloom_metadata_bytes = 0;
    uint64_t planning_time_us = 0;
    uint64_t metadata_total_rows = 0;

    idx_t MaxThreads() const override;
};

/// Thread-local dataset reader and flattened value buffers.
struct DatasetLocalState final : public LocalTableFunctionState {
    std::unique_ptr<TFile> file;
    RootObjectReader object_reader;
    RootPathReader path_reader;
    uint64_t reported_serialized_baskets = 0;
    uint64_t reported_serialized_compressed_bytes = 0;
    uint64_t reported_serialized_entry_bytes = 0;
    std::vector<std::vector<PathLevel>> predicate_levels;
    std::string open_uri;
    std::string open_schema;

    RootBasketTask current_task;
    bool has_task = false;
    uint64_t current_entry = 0;
    std::vector<double> values;
    std::vector<RootPrimitiveValue> typed_values;
    std::vector<int32_t> flat_indices;
    idx_t value_offset = 0;
    uint64_t value_event_fk = 0;
    uint64_t value_entry_id = 0;
    std::string value_source_id;
    idx_t next_task_in_group = 0;
    idx_t task_group_end = 0;
    RootFilterEvaluator filter_evaluator;
};

/// Prunes metadata and groups selected baskets by file.
class DatasetTaskPlanner final {
  public:
    void Plan(ClientContext& context, const DatasetBindData& bind, DatasetGlobalState& global,
              optional_ptr<TableFilterSet> filters);
    void PlanMetadataCount(ClientContext& context, const DatasetBindData& bind, DatasetGlobalState& global);

  private:
    void BuildFileTaskGroups(DatasetGlobalState& global);
};

/// Resolves catalog schemas and the SQL output contract.
class DatasetScanBinder final {
  public:
    unique_ptr<FunctionData> Bind(ClientContext& context, TableFunctionBindInput& input,
                                  vector<LogicalType>& return_types, vector<string>& return_names);
};

/// Constructs dataset scan execution states.
class DatasetScanStateFactory final {
  public:
    unique_ptr<NodeStatistics> Cardinality(const FunctionData* bind_data);
    unique_ptr<GlobalTableFunctionState> CreateGlobal(ClientContext& context, TableFunctionInitInput& input);
    unique_ptr<LocalTableFunctionState> CreateLocal();
};

/// Decodes selected entries and emits exact filtered rows.
class DatasetScanExecutor final {
  public:
    void Scan(ClientContext& context, TableFunctionInput& input, DataChunk& output);
    double Progress(const GlobalTableFunctionState* state) const;

  private:
    void ValidateAccessPlan(const SchemaBinding& schema, const std::vector<PathLevel>& actual) const;
    void SyncSerializedCounters(DatasetLocalState& local, DatasetGlobalState& global) const;
    void OpenTaskFile(const DatasetBindData& bind, DatasetGlobalState& global, DatasetLocalState& local,
                      const RootBasketTask& task);
    void PrefetchPhysicalRange(TFile* file, uint64_t offset, uint64_t size) const;
    bool ClaimTask(const DatasetBindData& bind, DatasetGlobalState& global, DatasetLocalState& local);
    bool PassesPathPredicates(const DatasetBindData& bind, DatasetLocalState& local, void* object) const;
    bool LoadNextEntry(const DatasetBindData& bind, DatasetLocalState& local, DatasetGlobalState& global);
    void SetPrimitiveAsType(Vector& vector, idx_t row, const LogicalType& type, const RootPrimitiveValue& value) const;
    void SetDoubleAsType(Vector& vector, idx_t row, const LogicalType& type, double value) const;
    void EmitProjectedTypedRow(const DatasetBindData& bind, const DatasetGlobalState& global, DataChunk& output,
                               idx_t output_row, uint64_t event_fk, const std::string& source_id, uint64_t entry_id,
                               const RootPrimitiveValue& value, const int32_t* indices, idx_t index_count) const;
    void EmitProjectedRow(const DatasetBindData& bind, const DatasetGlobalState& global, DataChunk& output,
                          idx_t output_row, uint64_t event_fk, const std::string& source_id, uint64_t entry_id,
                          double value, const int32_t* indices, idx_t index_count) const;
    idx_t ReserveOutputRows(DatasetGlobalState& global, idx_t requested) const;
};

/// Renders stable dataset planning and execution counters.
class DatasetScanExplain final {
  public:
    InsertionOrderPreservingMap<string> Bound(TableFunctionToStringInput& input) const;
    InsertionOrderPreservingMap<string> Running(TableFunctionDynamicToStringInput& input) const;
};

bool RequiresTypedDatasetValue(const LogicalType& type);
bool PathPredicateEventMatches(const PathPredicateBinding& predicate, const std::vector<RootPrimitiveValue>& values);
bool PassesFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                   const DatasetGlobalState& global, uint64_t event_fk, double numeric_value, const int32_t* indices,
                   idx_t index_count);
bool PassesTypedFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                        const DatasetGlobalState& global, uint64_t event_fk, const RootPrimitiveValue& numeric_value,
                        const int32_t* indices, idx_t index_count);

} // namespace duckdb::rootlake
