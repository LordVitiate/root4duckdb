#pragma once

#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <thread>
#include <variant>
#include <vector>

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"

#include "root4duckdb/reader/root_branch_projection.hpp"
#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_dictionary.hpp"
#include "root4duckdb/direct/root_direct_scheduler.hpp"
#include "root4duckdb/core/root_file_opener.hpp"
#include "root4duckdb/index/root_filter.hpp"
#include "root4duckdb/histogram/root_histogram_reader.hpp"
#include "root4duckdb/core/root_input_resolver.hpp"
#include "root4duckdb/core/root_lake_common.hpp"
#include "root4duckdb/reader/root_path_reader.hpp"
#include "root4duckdb/core/root_runtime_settings.hpp"

namespace duckdb {

class Expression;
class LogicalGet;

/// Converts one primitive value into an exact filter scalar.
rootlake::RootScalarActual PrimitiveScalarActual(const LogicalType& logical_type,
                                                 const rootlake::RootPrimitiveValue& value);


/// Physical primitive branch used by compatibility scans.
struct RootPrimitiveBranch {
    std::string name;
    std::string type_name;
    TBranch* branch = nullptr;
    TLeaf* leaf = nullptr;
};

struct RootSemanticScanMode {};
struct RootPrimitiveTreeScanMode {};
struct RootEmptyScanMode {};

struct RootBrowseScanMode {
    std::vector<std::string> children;
};

struct RootDirectBranchScanMode {
    RootPrimitiveBranch branch;
};

struct RootHistogramScanMode {
    rootlake::RootHistogramBinding binding;
    std::unique_ptr<TH1> object;
};

/// Exactly one direct-scan strategy selected at bind time.
using RootScanMode = std::variant<RootSemanticScanMode, RootBrowseScanMode, RootDirectBranchScanMode,
                                  RootPrimitiveTreeScanMode, RootHistogramScanMode, RootEmptyScanMode>;

/// One logical output column in the direct scan plan.
struct RootScanColumn {
    std::string name;
    std::string logical_path;
    std::string branch_name;
    std::string root_type;
    bool is_string = false;
    std::vector<rootlake::PathLevel> levels;
    std::string index_signature;
    bool is_virtual_index = false;
};

/// Claims non-overlapping entry batches for single-file scans.
class RootEntryScheduler {
    uint64_t& next_row_;
    uint64_t total_rows_;
    std::mutex& mutex_;

  public:
    struct WorkBatch {
        uint64_t start;
        uint64_t end;
        [[nodiscard]] bool HasWork() const;
    };

    RootEntryScheduler(uint64_t& next_row, uint64_t total_rows, std::mutex& mutex);

    static constexpr uint64_t DEFAULT_BATCH_SIZE = 65536;

    WorkBatch ClaimWork(uint64_t preferred_batch_size = DEFAULT_BATCH_SIZE);
    [[nodiscard]] static idx_t EstimateWorkUnits(uint64_t total_rows,
                                                 uint64_t batch_size = DEFAULT_BATCH_SIZE);
};

/// Immutable bind-time plan for read_root.
struct RootScanBindData : public TableFunctionData {
    RootDebugLifetimeSentinel lifetime_sentinel{"RootScanBindData"};
    std::string root_path;
    std::string input_specification;
    std::vector<std::string> root_paths;
    idx_t representative_source_id = 0;
    uint64_t bind_open_us = 0;
    std::string tree_name;
    uint64_t total_rows = 0;
    std::vector<RootScanColumn> columns;

    RootScanMode scan_mode;
    rootlake::RootAccessOptions root_access;
    idx_t source_id_column = DConstants::INVALID_INDEX;
    idx_t source_path_column = DConstants::INVALID_INDEX;

    // Conservative optimizer hints for physical entry scheduling. The SQL
    // predicate remains above read_root and is still evaluated by DuckDB.
    uint64_t entry_prune_lower = 0;
    uint64_t entry_prune_upper = std::numeric_limits<uint64_t>::max();
    bool entry_prune_impossible = false;
    bool entry_prune_active = false;

    bool IsMultiFile() const;
    bool IsSemanticMode() const noexcept;
    bool IsBrowseMode() const noexcept;
    bool IsDirectBranchMode() const noexcept;
    bool IsPrimitiveTreeMode() const noexcept;
    bool IsHistogramMode() const noexcept;
    bool IsEmptyMode() const noexcept;
    const RootBrowseScanMode* BrowseMode() const noexcept;
    const RootDirectBranchScanMode* DirectBranchMode() const noexcept;
    const RootHistogramScanMode* HistogramMode() const noexcept;
    void SelectSemanticMode();
    void SelectBrowseMode(std::vector<std::string> children);
    void SelectDirectBranchMode(RootPrimitiveBranch branch);
    void SelectPrimitiveTreeMode();
    void SelectHistogramMode(rootlake::RootHistogramBinding binding, std::unique_ptr<TH1> object);
    void SelectEmptyMode();
    ~RootScanBindData() noexcept;
};

/// Shared scheduling and profiling state for read_root.
struct RootScanGlobalState : public GlobalTableFunctionState {
    uint64_t next_row = 0;
    uint64_t total_rows = 0;
    uint64_t scheduled_rows = 0;
    size_t browse_offset = 0;
    std::mutex coordination_mutex;
    unique_ptr<TableFilterSet> filters;
    std::vector<idx_t> scan_column_ids;
    std::vector<idx_t> output_column_ids;
    std::atomic<uint64_t> serialized_entries{0};
    std::atomic<uint64_t> serialized_values{0};
    std::atomic<uint64_t> serialized_baskets{0};
    std::atomic<uint64_t> serialized_compressed_bytes{0};
    std::atomic<uint64_t> serialized_entry_bytes{0};
    std::atomic<uint64_t> object_validation_entries{0};
    std::atomic<uint64_t> object_fallback_entries{0};
    unique_ptr<rootlake::RootDirectFileScheduler> file_scheduler;
    uint64_t entry_lower = 0;
    uint64_t entry_upper = std::numeric_limits<uint64_t>::max();
    uint64_t source_lower = 0;
    uint64_t source_upper = std::numeric_limits<uint64_t>::max();
    bool entry_range_impossible = false;
    idx_t worker_limit = 1;
    bool force_single_thread = false;

    idx_t MaxThreads() const override;
};

/// One serialized primitive projection owned by a single DuckDB worker.
struct RootSerializedColumnState {
    idx_t column_id = DConstants::INVALID_INDEX;
    rootlake::RootPathReader path_reader;
    std::vector<rootlake::RootPrimitiveValue> values;
    std::vector<int32_t> indices;
    uint64_t reported_baskets = 0;
    uint64_t reported_compressed_bytes = 0;
    uint64_t reported_entry_bytes = 0;
};

/// Thread-local ROOT handles and decoded row buffers.
struct RootScanLocalState : public LocalTableFunctionState {
    rootlake::RootFileHandle root_file;
    std::unordered_map<std::string, rootlake::RootObjectReader> root_readers;

    uint64_t local_current_row = 0;
    uint64_t local_end_row = 0;

    uint64_t current_entry = std::numeric_limits<uint64_t>::max();
    size_t current_elem_idx = 0;
    std::vector<rootlake::ReadResult> cached_results;
    bool has_cached_entry = false;

    bool has_container_columns = false;
    rootlake::RootFilterEvaluator filter_evaluator;

    std::vector<RootSerializedColumnState> serialized_columns;

    TBranch* direct_branch = nullptr;
    TLeaf* direct_leaf = nullptr;
    std::vector<TBranch*> primitive_tree_branches;
    std::vector<TLeaf*> primitive_tree_leaves;
    bool primitive_tree_requires_read = false;

    bool file_active = false;
    rootlake::RootDirectFileTask file_task;
    std::chrono::steady_clock::time_point file_started;

    ~RootScanLocalState();
};

enum class CacheResult { CACHED, CONTINUE_LOOP, BREAK_LOOP };

/// Builds direct scan schemas and semantic access plans.
class RootScanBinder final {
  public:
    unique_ptr<FunctionData> Bind(ClientContext& context, TableFunctionBindInput& input,
                                  vector<LogicalType>& return_types, vector<string>& return_names);

  private:
    void ConfigureOptions(RootScanBindData& bind_data, ClientContext& context, TableFunctionBindInput& input);
    void BindRootBrowse(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                        std::vector<LogicalType>& return_types);
    void BindRequestedPath(RootScanBindData& bind_data, TableFunctionBindInput& input, bool dictionary_loaded,
                           std::vector<std::string>& return_names, std::vector<LogicalType>& return_types);
    void BindPrimitiveCompatibility(RootScanBindData& bind_data, TFile& file, TTree& tree,
                                    const std::string& path_prefix, std::vector<std::string>& return_names,
                                    std::vector<LogicalType>& return_types);
    std::vector<RootPrimitiveBranch> CollectPrimitiveBranches(TTree& tree);
    bool IsTreeName(TFile& file, const std::string& name);
    void AddEntryIdColumn(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                          std::vector<LogicalType>& return_types);
    void BindDirectPrimitives(RootScanBindData& bind_data, const std::string& path_prefix,
                              const std::vector<std::string>& matching_paths, std::vector<std::string>& return_names,
                              std::vector<LogicalType>& return_types);
    void BindBrowseMode(RootScanBindData& bind_data, const std::string& path_prefix,
                        const std::set<std::string>& direct_children, std::vector<std::string>& return_names,
                        std::vector<LogicalType>& return_types);
    void BindEmptyResult(RootScanBindData& bind_data, const std::string& path_prefix,
                         std::vector<std::string>& return_names, std::vector<LogicalType>& return_types);
    bool LoadRequestedDictionary(ClientContext& context, TableFunctionBindInput& input);
    bool BindSemanticPath(RootScanBindData& bind_data, TFile* file, const std::string& path_prefix_raw,
                          std::vector<std::string>& return_names, std::vector<LogicalType>& return_types);
    std::unique_ptr<TFile> OpenRepresentativeFile(RootScanBindData& bind_data);
    void AddMultiFileIdentityColumns(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                                     std::vector<LogicalType>& return_types);
};

/// Opens, reuses, and closes one ROOT input per local state.
class RootScanFileManager final {
  public:
    void Open(const RootScanBindData& bind_data, RootScanGlobalState& global, RootScanLocalState& local,
              const std::string& file_path, bool synchronize_open);
    void Reset(RootScanGlobalState& global, RootScanLocalState& local, bool completed);
    bool EnsureReady(const RootScanBindData& bind_data, RootScanGlobalState& global, RootScanLocalState& local);
};

/// Collects conservative physical entry bounds from optimizer expressions.
void RootScanCollectPruningHints(ClientContext& context, LogicalGet& get, FunctionData* bind_data,
                                 vector<unique_ptr<Expression>>& filters);

/// Constructs direct scan global and local states.
class RootScanStateFactory final {
  public:
    unique_ptr<GlobalTableFunctionState> CreateGlobal(ClientContext& context, TableFunctionInitInput& input);
    unique_ptr<LocalTableFunctionState> CreateLocal(TableFunctionInitInput& input,
                                                    GlobalTableFunctionState* global_state);
};

/// Executes the selected direct read mode.
class RootScanExecutor final {
  public:
    void Execute(ClientContext& context, TableFunctionInput& input, DataChunk& output);

  private:
    void ProcessHistogramMode(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& global,
                              RootScanLocalState& local, DataChunk& output);
    void ProcessBrowseMode(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& global,
                           RootScanLocalState& local, DataChunk& output);
    bool WriteNumericValue(Vector& vector, idx_t row, const rootlake::RootPrimitiveValue& value);
    std::optional<int32_t> ResolveCachedIndexValue(const RootScanBindData& bind_data, const RootScanLocalState& local,
                                                   idx_t column_index, size_t element_index);
    rootlake::RootScalarActual CachedScalar(const RootScanBindData& bind_data, const RootScanLocalState& local,
                                            idx_t column_index, uint64_t entry, size_t element_index);
    bool PassesCachedFilters(ClientContext& context, const RootScanBindData& bind_data,
                             const RootScanGlobalState& global, RootScanLocalState& local, uint64_t entry,
                             size_t element_index);
    bool PassesPrimitiveTreeFilters(ClientContext& context, const RootScanBindData& bind_data,
                                    const RootScanGlobalState& global, RootScanLocalState& local, uint64_t entry);
    void ProcessPrimitiveTree(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& global,
                              RootScanLocalState& local, DataChunk& output);
    bool PassesDirectBranchFilters(ClientContext& context, const RootScanBindData& bind_data,
                                   const RootScanGlobalState& global, RootScanLocalState& local, uint64_t entry,
                                   const rootlake::RootPrimitiveValue& value);
    void ProcessDirectBranch(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& global,
                             RootScanLocalState& local, DataChunk& output);
    void ProcessCachedEntry(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& global,
                            RootScanLocalState& local, DataChunk& output, idx_t& output_count);
    std::vector<std::string> SplitIndexSignature(const std::string& signature);
    void MaterializeSerializedResult(const RootScanColumn& column, uint64_t entry,
                                     const rootlake::SerializedReadPlan& plan,
                                     const std::vector<rootlake::RootPrimitiveValue>& values,
                                     const std::vector<int32_t>& flat_indices, rootlake::ReadResult& result);
    CacheResult ReadAndCacheEntry(const RootScanBindData& bind_data, RootScanGlobalState& global,
                                  RootScanLocalState& local, DataChunk& output, idx_t& output_count);

    RootScanFileManager file_manager;
};

/// Renders stable direct scan profiling fields.
class RootScanExplain final {
  public:
    InsertionOrderPreservingMap<string> Bound(TableFunctionToStringInput& input);
    InsertionOrderPreservingMap<string> Running(TableFunctionDynamicToStringInput& input);
};

} // namespace duckdb
