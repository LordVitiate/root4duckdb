#ifndef ROOT4DUCKDB_DIRECT_ROOT_SCAN_INTERNAL_HPP
#define ROOT4DUCKDB_DIRECT_ROOT_SCAN_INTERNAL_HPP

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
#include <utility>
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
#include "duckdb/planner/table_filter_set.hpp"
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

namespace duckdb
{

class Expression;
class LogicalGet;

/// Converts one primitive value into an exact filter scalar.
rootlake::RootScalarActual PrimitiveScalarActual(const LogicalType& logical_type,
                                                 const rootlake::RootPrimitiveValue& value);

/// Physical primitive branch used by compatibility scans.
struct RootPrimitiveBranch
{
    std::string name;
    std::string type_name;
    TBranch* branch = nullptr;
    TLeaf* leaf = nullptr;
};

struct RootSemanticScanMode
{
};
struct RootPrimitiveTreeScanMode
{
};
struct RootEmptyScanMode
{
};

struct RootBrowseScanMode
{
    std::vector<std::string> children;
};

struct RootHistogramScanMode
{
    rootlake::RootHistogramBinding binding;
    std::unique_ptr<TH1> object;
};

/// Exactly one direct-scan strategy selected at bind time.
using RootScanMode = std::variant<RootSemanticScanMode, RootBrowseScanMode, RootPrimitiveTreeScanMode,
                                  RootHistogramScanMode, RootEmptyScanMode>;

/// One logical output column in the direct scan plan.
struct RootScanColumn
{
    std::string name;
    std::string logical_path;
    // Semantic ROOT class identity; never a physical TBranch.
    std::string root_class_name;
    // Only for primitive TTree compatibility scans.
    std::string primitive_branch_name;
    std::string root_type;
    bool is_string = false;
    std::vector<rootlake::PathLevel> levels;
    std::string index_signature;
    bool is_virtual_index = false;
};

/// Claims non-overlapping entry batches for single-file scans.
class RootEntryScheduler
{
  public:
    struct WorkBatch
    {
        uint64_t start;
        uint64_t end;
        [[nodiscard]] bool HasWork() const;
    };

    RootEntryScheduler(uint64_t& next_row, uint64_t total_rows, std::mutex& mutex);

    static constexpr uint64_t DEFAULT_BATCH_SIZE = 65536;

    WorkBatch ClaimWork(uint64_t preferred_batch_size = DEFAULT_BATCH_SIZE);
    [[nodiscard]] static idx_t EstimateWorkUnits(uint64_t total_rows, uint64_t batch_size = DEFAULT_BATCH_SIZE);

  private:
    uint64_t& next_row_;
    uint64_t total_rows_;
    std::mutex& mutex_;
};

/// Immutable bind-time plan for read_root.
struct RootScanBindData : public TableFunctionData
{
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
    bool IsPrimitiveTreeMode() const noexcept;
    bool IsHistogramMode() const noexcept;
    bool IsEmptyMode() const noexcept;
    const RootBrowseScanMode* BrowseMode() const noexcept;
    const RootHistogramScanMode* HistogramMode() const noexcept;
    void SelectSemanticMode();
    void SelectBrowseMode(std::vector<std::string> children);
    void SelectPrimitiveTreeMode();
    void SelectHistogramMode(rootlake::RootHistogramBinding binding, std::unique_ptr<TH1> object);
    void SelectEmptyMode();
    ~RootScanBindData() noexcept;
};

using RootScanFilterBinding = rootlake::RootFilterBinding;

/// Shared scheduling and profiling state for read_root.
struct RootScanGlobalState : public GlobalTableFunctionState
{
    uint64_t next_row = 0;
    uint64_t total_rows = 0;
    uint64_t scheduled_rows = 0;
    size_t browse_offset = 0;
    std::mutex coordination_mutex;
    unique_ptr<TableFilterSet> filters;
    std::vector<RootScanFilterBinding> entry_filters;
    std::vector<RootScanFilterBinding> element_filters;
    std::vector<idx_t> scan_column_ids;
    std::vector<idx_t> output_column_ids;
    std::atomic<uint64_t> serialized_entries{0};
    std::atomic<uint64_t> serialized_page_calls{0};
    std::atomic<uint64_t> serialized_multi_column_pages{0};
    std::atomic<uint64_t> serialized_values{0};
    std::atomic<uint64_t> serialized_baskets{0};
    std::atomic<uint64_t> serialized_compressed_bytes{0};
    std::atomic<uint64_t> serialized_entry_bytes{0};
    std::atomic<uint64_t> nested_fixed_direct_entries{0};
    std::atomic<uint64_t> nested_fixed_scratch_entries{0};
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
struct RootSerializedColumnState
{
    idx_t column_id = DConstants::INVALID_INDEX;
    std::vector<rootlake::RootPrimitiveValue> values;
    std::vector<int32_t> indices;
    uint64_t reported_baskets = 0;
    uint64_t reported_compressed_bytes = 0;
    uint64_t reported_entry_bytes = 0;
    uint64_t reported_nested_fixed_direct = 0;
    uint64_t reported_nested_fixed_scratch = 0;
};

/// Thread-local ROOT handles and decoded row buffers.
struct RootScanLocalState : public LocalTableFunctionState
{
    rootlake::RootFileHandle root_file;
    std::unordered_map<std::string, rootlake::RootObjectReader> root_readers;
    // One canonical reader per projected path; object fallback shares its state.
    std::vector<rootlake::RootPathReader> path_readers;
    std::vector<idx_t> projected_relation_columns;

    uint64_t local_current_row = 0;
    uint64_t local_end_row = 0;

    uint64_t current_entry = std::numeric_limits<uint64_t>::max();
    size_t current_elem_idx = 0;
    std::vector<rootlake::ReadResult> cached_results;
    // Per-current-page source mappings reused by projections and pushed filters.
    std::vector<std::optional<std::pair<idx_t, size_t>>> cached_index_sources;
    bool cursor_entry_active = false;
    bool cursor_has_more = false;
    // A single fixed-member numeric projection uses the shared paged decoder.
    idx_t numeric_page_column = DConstants::INVALID_INDEX;
    // Multi-column scalar siblings use synchronized shared numeric cursors.
    std::vector<idx_t> multi_numeric_page_state_indices;
    bool multi_numeric_page_emitted = false;
    bool multi_numeric_shape_mismatch = false;
    bool has_cached_entry = false;

    bool has_container_columns = false;
    rootlake::RootFilterEvaluator filter_evaluator;

    std::vector<RootSerializedColumnState> serialized_columns;

    std::vector<TBranch*> primitive_tree_branches;
    std::vector<TLeaf*> primitive_tree_leaves;
    bool primitive_tree_requires_read = false;

    bool file_active = false;
    rootlake::RootDirectFileTask file_task;
    std::chrono::steady_clock::time_point file_started;

    ~RootScanLocalState();
};

enum class CacheResult
{
    CACHED,
    CONTINUE_LOOP,
    BREAK_LOOP
};

/// Builds direct scan schemas and semantic access plans.
class RootScanBinder final
{
  public:
    unique_ptr<FunctionData> Bind(ClientContext& context, TableFunctionBindInput& input,
                                  vector<LogicalType>& return_types, vector<string>& return_names) const;

  private:
    void ConfigureOptions(RootScanBindData& bind_data, ClientContext& context, TableFunctionBindInput& input) const;
    void BindRootBrowse(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                        std::vector<LogicalType>& return_types) const;
    void BindRequestedPath(RootScanBindData& bind_data, TableFunctionBindInput& input, bool dictionary_loaded,
                           std::vector<std::string>& return_names, std::vector<LogicalType>& return_types) const;
    void BindPrimitiveCompatibility(RootScanBindData& bind_data, TFile& file, TTree& tree,
                                    const std::string& path_prefix, std::vector<std::string>& return_names,
                                    std::vector<LogicalType>& return_types) const;
    std::vector<RootPrimitiveBranch> CollectPrimitiveBranches(TTree& tree) const;
    bool IsTreeName(TFile& file, const std::string& name) const;
    void AddEntryIdColumn(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                          std::vector<LogicalType>& return_types) const;
    void BindDirectPrimitives(RootScanBindData& bind_data, const std::string& path_prefix,
                              const std::vector<std::string>& matching_paths, std::vector<std::string>& return_names,
                              std::vector<LogicalType>& return_types) const;
    void BindEmptyResult(RootScanBindData& bind_data, const std::string& path_prefix,
                         std::vector<std::string>& return_names, std::vector<LogicalType>& return_types) const;
    bool LoadRequestedDictionary(ClientContext& context, TableFunctionBindInput& input) const;
    bool BindSemanticPath(RootScanBindData& bind_data, TFile* file, const std::string& path_prefix_raw,
                          std::vector<std::string>& return_names, std::vector<LogicalType>& return_types) const;
    std::unique_ptr<TFile> OpenRepresentativeFile(RootScanBindData& bind_data) const;
    void AddMultiFileIdentityColumns(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                                     std::vector<LogicalType>& return_types) const;
};

/// Opens, reuses, and closes one ROOT input per local state.
class RootScanFileManager final
{
  public:
    void Open(const RootScanBindData& bind_data, RootScanGlobalState& gstate, RootScanLocalState& target,
              const std::string& file_path, bool synchronize_open) const;
    void Reset(RootScanGlobalState& gstate, RootScanLocalState& local_state, bool completed) const;
    bool EnsureReady(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                     RootScanLocalState& local_state) const;
};

/// Collects conservative physical entry bounds from optimizer expressions.
void RootScanCollectPruningHints(ClientContext& context, LogicalGet& get, FunctionData* bind_data,
                                 vector<unique_ptr<Expression>>& filters);

/// Constructs direct scan global and local states.
class RootScanStateFactory final
{
  public:
    unique_ptr<GlobalTableFunctionState> CreateGlobal(ClientContext& context, TableFunctionInitInput& input) const;
    unique_ptr<LocalTableFunctionState> CreateLocal(TableFunctionInitInput& input,
                                                    GlobalTableFunctionState* global_state_p) const;
};

/// Executes the selected direct read mode.
class RootScanExecutor final
{
  public:
    static bool ClaimEntryBatch(RootScanGlobalState& global, RootScanLocalState& local);

    void Execute(ClientContext& context, TableFunctionInput& data_p, DataChunk& output) const;

  private:
    void ProcessHistogramMode(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                              RootScanLocalState& lstate, DataChunk& output) const;
    void ProcessBrowseMode(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                           RootScanLocalState& lstate, DataChunk& output) const;
    bool WriteNumericValue(Vector& vector, idx_t row, const rootlake::RootPrimitiveValue& value) const;
    std::optional<int32_t> ResolveCachedIndexValue(const RootScanBindData& bind_data, const RootScanLocalState& lstate,
                                                   idx_t col_idx, size_t elem_idx) const;
    rootlake::RootScalarActual CachedScalar(const RootScanBindData& bind_data, const RootScanLocalState& lstate,
                                            idx_t col_idx, uint64_t entry, size_t elem_idx) const;
    bool PassesCachedFilters(ClientContext& context, const RootScanBindData& bind_data,
                             const RootScanGlobalState& gstate, RootScanLocalState& lstate, uint64_t entry,
                             size_t elem_idx, bool entry_only) const;
    bool PassesPrimitiveTreeFilters(ClientContext& context, const RootScanBindData& bind_data,
                                    const RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                    uint64_t entry) const;
    void ProcessPrimitiveTree(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                              RootScanLocalState& lstate, DataChunk& output) const;
    void ProcessCachedEntry(ClientContext& context, const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                            RootScanLocalState& lstate, DataChunk& output, idx_t& out_count) const;
    std::vector<std::string> SplitIndexSignature(const std::string& signature) const;
    bool ReadCursorPage(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                        RootScanLocalState& lstate) const;
    void EndEntryCursors(RootScanLocalState& lstate) const;
    void MaterializeSerializedResult(const RootScanColumn& column, uint64_t entry,
                                     const rootlake::SerializedReadPlan& plan,
                                     const std::vector<rootlake::RootPrimitiveValue>& values,
                                     const std::vector<int32_t>& flat_indices, rootlake::ReadResult& result) const;
    CacheResult ReadAndCacheEntry(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                                  RootScanLocalState& lstate) const;

    RootScanFileManager file_manager;
};

/// Renders stable direct scan profiling fields.
class RootScanExplain final
{
  public:
    InsertionOrderPreservingMap<string> Bound(TableFunctionToStringInput& input) const;
    InsertionOrderPreservingMap<string> Running(TableFunctionGetMetricsInput& input) const;
};

} // namespace duckdb

#endif
