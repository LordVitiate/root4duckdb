#include "include/root_headers.hpp"

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

#include "include/root_branch_projection.hpp"
#include "include/root_debug.hpp"
#include "include/root_dictionary.hpp"
#include "include/root_direct_scheduler.hpp"
#include "include/root_file_opener.hpp"
#include "include/root_filter.hpp"
#include "include/root_histogram_reader.hpp"
#include "include/root_input_resolver.hpp"
#include "include/root_lake_common.hpp"
#include "include/root_path_reader.hpp"
#include "include/root_runtime_settings.hpp"

namespace duckdb {

struct RootPrimitiveBranch {
    std::string name;
    std::string type_name;
    TBranch* branch = nullptr;
    TLeaf* leaf = nullptr;
};

struct RootScanColumn
{
    std::string name;
    std::string logical_path;
    std::string branch_name;
    std::string root_type;
    bool is_string = false;
    std::vector<rootlake::PathLevel> levels;
    std::string index_signature;
    bool is_virtual_index = false;

};

class RootEntryScheduler
{
    uint64_t& next_row_;
    uint64_t total_rows_;
    std::mutex& mutex_;

public:
    struct WorkBatch
    {
        uint64_t start;
        uint64_t end;
        [[nodiscard]] bool HasWork() const { return start < end; }
    };

    RootEntryScheduler(uint64_t& next_row, uint64_t total_rows, std::mutex& mtx)
        : next_row_(next_row), total_rows_(total_rows), mutex_(mtx) {}

    WorkBatch ClaimWork(uint64_t preferred_batch_size = 100000)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (next_row_ >= total_rows_)
        {
            return {0, 0};
        }

        WorkBatch batch;
        batch.start = next_row_;
        batch.end = std::min(next_row_ + preferred_batch_size, total_rows_);

        next_row_ = batch.end;
        return batch;
    }

    [[nodiscard]] static idx_t EstimateOptimalThreads(uint64_t total_rows)
    {
        const uint64_t rows_per_thread = 500000;
        idx_t threads = static_cast<idx_t>(total_rows / rows_per_thread);
        return std::max<idx_t>(1, threads);
    }
};


struct RootScanBindData : public TableFunctionData
{
    RootDebugLifetimeSentinel lifetime_sentinel {"RootScanBindData"};
    std::string root_path;
    std::string input_specification;
    std::vector<std::string> root_paths;
    idx_t representative_source_id = 0;
    uint64_t bind_open_us = 0;
    std::string tree_name;
    uint64_t total_rows = 0;
    std::vector<RootScanColumn> columns;

    bool is_browse_mode = false;
    bool is_direct_branch_mode = false;
    bool is_primitive_tree_mode = false;
    bool is_empty_mode = false;
    bool is_histogram_mode = false;
    rootlake::RootDictionaryCleanupMode dictionary_cleanup_mode = rootlake::RootDictionaryCleanupMode::FULL;
    std::vector<std::string> browse_children;
    RootPrimitiveBranch direct_branch_info;
    rootlake::RootHistogramBinding histogram_binding;
    std::unique_ptr<TH1> histogram_object;
    rootlake::RootReaderMode reader_mode = rootlake::RootReaderMode::AUTO;
    uint32_t raw_validation_entries = 4;
    uint64_t raw_max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t raw_max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    idx_t source_id_column = DConstants::INVALID_INDEX;
    idx_t source_path_column = DConstants::INVALID_INDEX;

    bool IsMultiFile() const { return root_paths.size() > 1; }

    ~RootScanBindData()
    {
        RootDebug("BIND_DATA.DTOR_BODY",
                  "this=" + RootPointer(this) +
                  " root_path=" + root_path +
                  " columns=" + std::to_string(columns.size()));
    }
};

struct RootScanGlobalState : public GlobalTableFunctionState
{
    uint64_t next_row = 0;
    uint64_t total_rows = 0;
    uint64_t scheduled_rows = 0;
    size_t browse_offset = 0;
    std::mutex coordination_mutex;
    unique_ptr<TableFilterSet> filters;
    std::vector<idx_t> scan_column_ids;
    std::vector<idx_t> output_column_ids;
    std::atomic<uint64_t> serialized_entries {0};
    std::atomic<uint64_t> serialized_values {0};
    std::atomic<uint64_t> serialized_baskets {0};
    std::atomic<uint64_t> serialized_compressed_bytes {0};
    std::atomic<uint64_t> serialized_entry_bytes {0};
    std::atomic<uint64_t> object_validation_entries {0};
    std::atomic<uint64_t> object_fallback_entries {0};
    unique_ptr<rootlake::RootDirectFileScheduler> file_scheduler;
    uint64_t event_lower = 0;
    uint64_t event_upper = std::numeric_limits<uint64_t>::max();
    bool event_range_impossible = false;
    bool histogram_mode = false;

    idx_t MaxThreads() const override
    {
        if (histogram_mode) return 1;
        if (file_scheduler) return file_scheduler->MaxThreads();
        return RootEntryScheduler::EstimateOptimalThreads(scheduled_rows);
    }
};

struct RootScanLocalState : public LocalTableFunctionState
{
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

    rootlake::RootPathReader path_reader;
    idx_t serialized_column = DConstants::INVALID_INDEX;
    std::vector<double> serialized_values;
    std::vector<int32_t> serialized_indices;
    uint64_t reported_serialized_baskets = 0;
    uint64_t reported_serialized_compressed_bytes = 0;
    uint64_t reported_serialized_entry_bytes = 0;

    TBranch* direct_branch = nullptr;
    TLeaf* direct_leaf = nullptr;
    std::vector<TBranch *> primitive_tree_branches;
    std::vector<TLeaf *> primitive_tree_leaves;
    bool primitive_tree_requires_read = false;

    bool file_active = false;
    rootlake::RootDirectFileTask file_task;
    std::chrono::steady_clock::time_point file_started;

    ~RootScanLocalState() = default;
};

enum class CacheResult {
    CACHED,
    CONTINUE_LOOP,
    BREAK_LOOP
};

class RootScanBinder final {
public:
    unique_ptr<FunctionData> Bind(
        ClientContext &context, TableFunctionBindInput &input,
        vector<LogicalType> &return_types, vector<string> &return_names);

private:
    void ConfigureOptions(
        RootScanBindData &bind_data, ClientContext &context,
        TableFunctionBindInput &input);
    void BindRootBrowse(
        RootScanBindData &bind_data,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    void BindRequestedPath(
        RootScanBindData &bind_data, TableFunctionBindInput &input,
        bool dictionary_loaded,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    void BindPrimitiveCompatibility(
        RootScanBindData &bind_data, TFile &file, TTree &tree,
        const std::string &path_prefix,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    std::vector<RootPrimitiveBranch> CollectPrimitiveBranches(TTree &tree);
    bool IsTreeName(TFile &file, const std::string &name);
    void AddEventIdColumn(
        RootScanBindData &bind_data,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    void BindDirectPrimitives(
        RootScanBindData &bind_data, const std::string &path_prefix,
        const std::vector<std::string> &matching_paths,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    void BindBrowseMode(
        RootScanBindData &bind_data, const std::string &path_prefix,
        const std::set<std::string> &direct_children,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    void BindEmptyResult(
        RootScanBindData &bind_data, const std::string &path_prefix,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    bool LoadRequestedDictionary(
        ClientContext &context, TableFunctionBindInput &input);
    bool BindSemanticPath(
        RootScanBindData &bind_data, TFile *file,
        const std::string &path_prefix_raw,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
    std::unique_ptr<TFile> OpenRepresentativeFile(
        RootScanBindData &bind_data);
    void AddMultiFileIdentityColumns(
        RootScanBindData &bind_data,
        std::vector<std::string> &return_names,
        std::vector<LogicalType> &return_types);
};

class RootScanFileManager final {
public:
    void Open(
        const RootScanBindData &bind_data, RootScanGlobalState &global,
        RootScanLocalState &local, const std::string &file_path,
        bool synchronize_open);
    void Reset(
        RootScanGlobalState &global, RootScanLocalState &local,
        bool completed);
    bool EnsureReady(
        const RootScanBindData &bind_data, RootScanGlobalState &global,
        RootScanLocalState &local);
};

class RootScanStateFactory final {
public:
    unique_ptr<GlobalTableFunctionState> CreateGlobal(
        ClientContext &context, TableFunctionInitInput &input);
    unique_ptr<LocalTableFunctionState> CreateLocal(
        TableFunctionInitInput &input,
        GlobalTableFunctionState *global_state);
};

class RootScanExecutor final {
public:
    void Execute(
        ClientContext &context, TableFunctionInput &input,
        DataChunk &output);

private:
    void ProcessHistogramMode(
        ClientContext &context,
        const RootScanBindData &bind_data,
        RootScanGlobalState &global,
        RootScanLocalState &local,
        DataChunk &output);
    void ProcessBrowseMode(
        ClientContext &context, const RootScanBindData &bind_data,
        RootScanGlobalState &global, RootScanLocalState &local,
        DataChunk &output);
    bool WriteNumericValue(Vector &vector, idx_t row, const rootlake::RootPrimitiveValue &value);
    std::optional<int32_t> ResolveCachedIndexValue(
        const RootScanBindData &bind_data,
        const RootScanLocalState &local, idx_t column_index,
        size_t element_index);
    rootlake::RootScalarActual CachedScalar(
        const RootScanBindData &bind_data,
        const RootScanLocalState &local, idx_t column_index,
        uint64_t entry, size_t element_index);
    bool PassesCachedFilters(
        ClientContext &context, const RootScanBindData &bind_data,
        const RootScanGlobalState &global, RootScanLocalState &local,
        uint64_t entry, size_t element_index);
    bool PassesPrimitiveTreeFilters(
        ClientContext &context,
        const RootScanBindData &bind_data,
        const RootScanGlobalState &global,
        RootScanLocalState &local,
        uint64_t entry);
    void ProcessPrimitiveTree(
        ClientContext &context,
        const RootScanBindData &bind_data,
        RootScanGlobalState &global,
        RootScanLocalState &local,
        DataChunk &output);
    bool PassesDirectBranchFilters(
        ClientContext &context, const RootScanBindData &bind_data,
        const RootScanGlobalState &global, RootScanLocalState &local,
        uint64_t entry, const rootlake::RootPrimitiveValue &value);
    void ProcessDirectBranch(
        ClientContext &context, const RootScanBindData &bind_data,
        RootScanGlobalState &global, RootScanLocalState &local,
        DataChunk &output);
    void ProcessCachedEntry(
        ClientContext &context, const RootScanBindData &bind_data,
        RootScanGlobalState &global, RootScanLocalState &local,
        DataChunk &output, idx_t &output_count);
    std::vector<std::string> SplitIndexSignature(
        const std::string &signature);
    void MaterializeSerializedResult(
        const RootScanColumn &column, uint64_t entry,
        const rootlake::SerializedReadPlan &plan,
        const std::vector<double> &values,
        const std::vector<int32_t> &flat_indices,
        rootlake::ReadResult &result);
    CacheResult ReadAndCacheEntry(
        const RootScanBindData &bind_data, RootScanGlobalState &global,
        RootScanLocalState &local, DataChunk &output,
        idx_t &output_count);

    RootScanFileManager file_manager;
};

class RootScanExplain final {
public:
    InsertionOrderPreservingMap<string> Bound(
        TableFunctionToStringInput &input);
    InsertionOrderPreservingMap<string> Running(
        TableFunctionDynamicToStringInput &input);
};

void RootScanBinder::AddEventIdColumn(
    RootScanBindData& bind_data,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    return_names.emplace_back("event_id");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    
    RootScanColumn col;
    col.name = "event_id";
    bind_data.columns.emplace_back(std::move(col));
}

void RootScanBinder::BindDirectPrimitives(
    RootScanBindData& bind_data,
    const std::string& path_prefix,
    const std::vector<std::string>& matching_paths,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    RootDebug("BIND.PRIMITIVES_BEGIN",
              "prefix=" + path_prefix +
              " matching_paths=" + std::to_string(matching_paths.size()));
    struct ResolvedColumn
    {
        RootScanColumn column;
        LogicalType duckdb_type;
    };

    std::vector<ResolvedColumn> resolved_columns;
    std::set<std::string> seen_value_names;
    std::vector<std::string> ordered_index_names;
    std::set<std::string> seen_index_names;

    for (const auto& full_path : matching_paths)
    {
        RootDebug("BIND.PRIMITIVE_PATH", "path=" + full_path);
        std::string rest = full_path.size() < path_prefix.size()
            ? std::string()
            : full_path.substr(path_prefix.size());
        if (rest.find('/') != std::string::npos)
        {
            continue;
        }

        std::string flat_name = rest;
        if (flat_name.empty())
        {
            std::string clean_prefix = path_prefix;
            if (!clean_prefix.empty() && clean_prefix.back() == '/')
            {
                clean_prefix.pop_back();
            }
            if (clean_prefix.size() >= 6 && clean_prefix.substr(clean_prefix.size() - 6) == "/value")
            {
                clean_prefix.resize(clean_prefix.size() - 6);
            }
            const size_t last_slash = clean_prefix.find_last_of('/');
            flat_name = last_slash == std::string::npos
                ? clean_prefix
                : clean_prefix.substr(last_slash + 1);
        }
        for (char& c : flat_name)
        {
            if (c == '/') c = '_';
        }

        std::string lower_name = flat_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (!seen_value_names.insert(lower_name).second)
        {
            continue;
        }

        const auto parsed = rootlake::ParsePathPrefix(full_path);
        if (parsed.fields.empty())
        {
            continue;
        }
        RootDebug("TCLASS.BEFORE",
                  "GetClass name=" + parsed.root_class + " source=" + full_path);
        auto* cls = TClass::GetClass(parsed.root_class.c_str());
        RootDebug("TCLASS.AFTER",
                  "name=" + parsed.root_class + " ptr=" + RootPointer(cls) +
                  " has_dictionary=" + std::to_string(cls && cls->HasDictionary() ? 1 : 0));
        if (!cls || !cls->HasDictionary())
        {
            continue;
        }
        auto levels = rootlake::PathResolver::TryResolve(cls, parsed.fields);
        if (levels.empty())
        {
            continue;
        }
        const auto& leaf = levels.back();
        if (!leaf.is_primitive && !leaf.is_string)
        {
            continue;
        }

        RootScanColumn col;
        col.name = flat_name;
        col.logical_path = full_path;
        col.branch_name = parsed.root_class;
        col.root_type = leaf.type;
        col.is_string = leaf.is_string;
        col.levels = std::move(levels);
        col.index_signature = rootlake::IndexSignature(col.levels);

        if (!col.index_signature.empty())
        {
            std::stringstream signature(col.index_signature);
            std::string index_name;
            while (std::getline(signature, index_name, ','))
            {
                std::string lower_index = index_name;
                std::transform(lower_index.begin(), lower_index.end(), lower_index.begin(), ::tolower);
                if (seen_index_names.insert(lower_index).second)
                {
                    ordered_index_names.push_back(index_name);
                }
            }
        }

        ResolvedColumn resolved;
        resolved.duckdb_type = rootlake::RootTypeToScanLogicalType(
            col.root_type, col.is_string, true);
        resolved.column = std::move(col);
        RootDebug("BIND.COLUMN_READY",
                  "name=" + resolved.column.name +
                  " root_type=" + resolved.column.root_type +
                  " signature=" + resolved.column.index_signature);
        resolved_columns.emplace_back(std::move(resolved));
    }

    AddEventIdColumn(bind_data, return_names, return_types);

    const std::string root_class_name = rootlake::ParsePathPrefix(path_prefix).root_class;
    for (const auto& index_name : ordered_index_names)
    {
        RootScanColumn index_column;
        index_column.name = index_name;
        index_column.is_virtual_index = true;
        index_column.branch_name = root_class_name;
        bind_data.columns.emplace_back(std::move(index_column));
        return_names.emplace_back(index_name);
        return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
    }

    for (auto& resolved : resolved_columns)
    {
        return_names.emplace_back(resolved.column.name);
        return_types.emplace_back(resolved.duckdb_type);
        bind_data.columns.emplace_back(std::move(resolved.column));
    }

    RootDebug("BIND.PRIMITIVES_END",
              "columns=" + std::to_string(bind_data.columns.size()) +
              " return_names=" + std::to_string(return_names.size()));
}

void RootScanBinder::BindBrowseMode(
    RootScanBindData& bind_data,
    const std::string& path_prefix,
    const std::set<std::string>& direct_children,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    bind_data.is_browse_mode = true;
    return_names.emplace_back("path");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    for (const auto& child_full_path : direct_children)
    {
        std::string folder_name = child_full_path.substr(path_prefix.size());
        if (!folder_name.empty() && folder_name.back() == '/')
        {
            folder_name.pop_back();
        }
        if (folder_name.empty())
        {
            continue;
        }
        bind_data.browse_children.push_back(folder_name);
    }
}

void RootScanBinder::BindEmptyResult(
    RootScanBindData& bind_data,
    const std::string& path_prefix,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    bind_data.is_empty_mode = true;
    bind_data.total_rows = 0;
    AddEventIdColumn(bind_data, return_names, return_types);
    if (!path_prefix.empty())
    {
        std::string clean_prefix = path_prefix;
        if (clean_prefix.back() == '/')
        {
            clean_prefix.pop_back();
        }
        size_t last_slash = clean_prefix.find_last_of('/');
        std::string container_name = (last_slash == std::string::npos) ? clean_prefix : clean_prefix.substr(last_slash + 1);

        if (container_name.find("vec") == 0 || container_name.find("set") == 0)
        {
            std::string idx_name = container_name + "_idx";
            RootScanColumn idx_col;
            idx_col.name = idx_name;
            idx_col.is_virtual_index = true;
            idx_col.branch_name = rootlake::ParsePathPrefix(path_prefix).root_class;
            bind_data.columns.emplace_back(std::move(idx_col));
            return_names.emplace_back(idx_name);
            return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
        }
    }
}

bool RootScanBinder::LoadRequestedDictionary(
    ClientContext& context, TableFunctionBindInput& input)
{
    auto it = input.named_parameters.find("dictionary");
    if (it == input.named_parameters.end())
    {
        RootDebug("DICT.NONE", "dictionary parameter was not supplied");
        return false;
    }
    const std::string dict_path = it->second.ToString();
    if (dict_path.empty())
    {
        RootDebug("DICT.EMPTY", "dictionary parameter is empty");
        return false;
    }
    return rootlake::LoadRootDictionary(context, dict_path);
}

bool RootScanBinder::BindSemanticPath(
    RootScanBindData& bind_data,
    TFile* file,
    const std::string& path_prefix_raw,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    RootDebug("SEMANTIC.BEGIN",
              "path=" + path_prefix_raw + " file_ptr=" + RootPointer(file));

    const auto parsed = rootlake::ParsePathPrefix(path_prefix_raw);
    RootDebug("SEMANTIC.PARSED",
              "root_class=" + parsed.root_class +
              " fields=" + JoinDebugFields(parsed.fields));
    if (parsed.root_class.empty())
    {
        return false;
    }

    RootDebug("TCLASS.BEFORE", "GetClass name=" + parsed.root_class + " semantic_bind=1");
    auto* root_class = TClass::GetClass(parsed.root_class.c_str());
    RootDebug("TCLASS.AFTER",
              "name=" + parsed.root_class + " ptr=" + RootPointer(root_class) +
              " has_dictionary=" +
                  std::to_string(root_class && root_class->HasDictionary() ? 1 : 0));
    if (!root_class || !root_class->HasDictionary())
    {
        return false;
    }
    RootDebug("TREE.BEFORE_FIND", "class=" + parsed.root_class);
    auto* tree = rootlake::FindTree(file, "", parsed.root_class);
    RootDebug("TREE.AFTER_FIND",
              "class=" + parsed.root_class + " tree_ptr=" + RootPointer(tree) +
              " tree_name=" + std::string(tree ? tree->GetName() : "<null>"));
    auto* branch = rootlake::FindObjectBranch(tree, parsed.root_class);
    RootDebug("BRANCH.AFTER_FIND",
              "class=" + parsed.root_class + " branch_ptr=" + RootPointer(branch) +
              " branch_name=" + std::string(branch ? branch->GetName() : "<null>"));
    if (!tree || !branch)
    {
        return false;
    }

    rootlake::SemanticPathSelection selection;
    RootDebug("SEMANTIC.BEFORE_SELECT", "path=" + path_prefix_raw);
    if (!rootlake::SelectSemanticPath(
            root_class, parsed, path_prefix_raw, selection))
    {
        return false;
    }

    RootDebug("SEMANTIC.AFTER_SELECT",
              "bind_prefix=" + selection.bind_prefix +
              " primitive_paths=" + std::to_string(selection.primitive_paths.size()) +
              " direct_children=" + std::to_string(selection.child_paths.size()));
    bind_data.tree_name = tree->GetName();
    bind_data.total_rows = static_cast<uint64_t>(tree->GetEntries());

    if (!selection.primitive_paths.empty())
    {
        BindDirectPrimitives(
            bind_data, selection.bind_prefix, selection.primitive_paths,
            return_names, return_types);
        if (bind_data.columns.size() > 1)
        {
            RootDebug("SEMANTIC.SUCCESS",
                      "mode=primitive columns=" + std::to_string(bind_data.columns.size()));
            return true;
        }

        // An exact non-scalar path must not trigger unrelated schema discovery.
        bind_data.columns.clear();
        return_names.clear();
        return_types.clear();
        return false;
    }

    if (!selection.child_paths.empty())
    {
        BindBrowseMode(
            bind_data, selection.bind_prefix, selection.child_paths,
            return_names, return_types);
        RootDebug("SEMANTIC.SUCCESS",
                  "mode=browse children=" + std::to_string(selection.child_paths.size()));
        return true;
    }
    return false;
}

std::unique_ptr<TFile> RootScanBinder::OpenRepresentativeFile(
    RootScanBindData& bind_data)
{
    std::vector<std::string> failures;
    for (idx_t source_id = 0; source_id < bind_data.root_paths.size(); ++source_id)
    {
        RootDebug("FILE.BEFORE_OPEN",
                  "mode=representative source_id=" + std::to_string(source_id) +
                  " path=" + bind_data.root_paths[source_id]);
        auto result = rootlake::OpenRootFile(bind_data.root_paths[source_id]);
        bind_data.bind_open_us += result.elapsed_us;
        RootDebug("FILE.AFTER_OPEN",
                  "mode=representative source_id=" + std::to_string(source_id) +
                  " attempts=" + std::to_string(result.attempts) +
                  " elapsed_us=" + std::to_string(result.elapsed_us) +
                  " zombie=" + std::to_string(result.file && result.file->IsZombie() ? 1 : 0));
        if (result)
        {
            bind_data.representative_source_id = source_id;
            bind_data.root_path = bind_data.root_paths[source_id];
            return std::move(result.file);
        }
        failures.push_back(bind_data.root_paths[source_id] + ": " + result.error);
    }
    std::ostringstream message;
    message << "Failed to open every ROOT input while selecting a representative file";
    const auto limit = std::min<size_t>(failures.size(), 4);
    for (size_t index = 0; index < limit; ++index) message << "; " << failures[index];
    if (failures.size() > limit) message << "; ...";
    throw IOException(message.str());
}

void RootScanBinder::AddMultiFileIdentityColumns(
    RootScanBindData& bind_data,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    if (!bind_data.IsMultiFile() || bind_data.is_browse_mode ||
        bind_data.source_id_column != DConstants::INVALID_INDEX) return;

    bind_data.source_id_column = bind_data.columns.size();
    RootScanColumn source_id;
    source_id.name = "source_id";
    source_id.root_type = "ULong64_t";
    bind_data.columns.push_back(std::move(source_id));
    return_names.emplace_back("source_id");
    return_types.emplace_back(LogicalTypeId::UBIGINT);

    bind_data.source_path_column = bind_data.columns.size();
    RootScanColumn source_path;
    source_path.name = "source_path";
    source_path.root_type = "string";
    source_path.is_string = true;
    bind_data.columns.push_back(std::move(source_path));
    return_names.emplace_back("source_path");
    return_types.emplace_back(LogicalTypeId::VARCHAR);
}

unique_ptr<FunctionData> RootScanBinder::Bind(
    ClientContext& context,
    TableFunctionBindInput& input,
    vector<LogicalType>& return_types,
    vector<string>& return_names)
{
    RootDebugOperationScope debug_operation("RootScanBind");
    auto bind_data = make_uniq<RootScanBindData>();
    ConfigureOptions(*bind_data, context, input);

    const bool dictionary_loaded = LoadRequestedDictionary(context, input);
    auto cleanup_parameter = input.named_parameters.find("dictionary_cleanup");
    const std::string cleanup_mode =
        cleanup_parameter == input.named_parameters.end()
            ? "auto" : cleanup_parameter->second.ToString();
    bind_data->dictionary_cleanup_mode = rootlake::ParseDictionaryCleanupMode(
        cleanup_mode,
        dictionary_loaded ? rootlake::RootDictionaryCleanupMode::RETAIN
                          : rootlake::RootDictionaryCleanupMode::FULL);

    if (input.named_parameters.find("path_prefix") ==
        input.named_parameters.end()) {
        BindRootBrowse(*bind_data, return_names, return_types);
    } else {
        BindRequestedPath(*bind_data, input, dictionary_loaded,
                          return_names, return_types);
    }
    return std::move(bind_data);
}

void RootScanBinder::ConfigureOptions(
    RootScanBindData &bind_data, ClientContext &context,
    TableFunctionBindInput &input) {
    bind_data.input_specification = input.inputs[0].ToString();
    bind_data.root_paths = rootlake::ResolveRootInputs(
        context, bind_data.input_specification);
    bind_data.root_path = bind_data.root_paths.front();
    auto reader_mode = input.named_parameters.find("reader_mode");
    if (reader_mode != input.named_parameters.end()) {
        bind_data.reader_mode =
            rootlake::ParseRootReaderMode(reader_mode->second.ToString());
    }
    auto raw_validation = input.named_parameters.find("raw_validation_entries");
    if (raw_validation != input.named_parameters.end()) {
        bind_data.raw_validation_entries =
            raw_validation->second.GetValue<uint32_t>();
    }
    auto raw_entry_limit = input.named_parameters.find("raw_max_entry_bytes");
    if (raw_entry_limit != input.named_parameters.end()) {
        bind_data.raw_max_entry_bytes =
            raw_entry_limit->second.GetValue<uint64_t>();
    }
    auto raw_value_limit = input.named_parameters.find("raw_max_values_per_entry");
    if (raw_value_limit != input.named_parameters.end()) {
        bind_data.raw_max_values_per_entry =
            raw_value_limit->second.GetValue<uint64_t>();
    }
    auto tree_cache = input.named_parameters.find("tree_cache_bytes");
    if (tree_cache != input.named_parameters.end()) {
        bind_data.tree_cache_bytes = tree_cache->second.GetValue<uint64_t>();
    }
    if (bind_data.raw_max_entry_bytes < 12) {
        throw InvalidInputException("raw_max_entry_bytes must be at least 12");
    }
    if (bind_data.raw_max_values_per_entry == 0) {
        throw InvalidInputException("raw_max_values_per_entry must be positive");
    }
    RootDebug("BIND.BEGIN",
              "root_input=" + bind_data.input_specification +
              " resolved_files=" + std::to_string(bind_data.root_paths.size()) +
              " inputs=" + std::to_string(input.inputs.size()) +
              " named_parameters=" + std::to_string(input.named_parameters.size()));
}

void RootScanBinder::BindRootBrowse(
    RootScanBindData &bind_data,
    std::vector<std::string> &return_names,
    std::vector<LogicalType> &return_types)
{
    auto file =
        OpenRepresentativeFile(bind_data);

    std::set<std::string> children;

    TIter next_key(file->GetListOfKeys());

    while (auto *key =
               dynamic_cast<TKey *>(next_key())) {
        const std::string class_name =
            key->GetClassName();

        if (class_name != "TTree") {
            children.insert(
                "/" +
                std::string(key->GetName()));
        }
    }

    TTree *tree =
        rootlake::FindTree(
            file.get(),
            "",
            "");

    if (tree) {
        auto *branches =
            tree->GetListOfBranches();

        for (int index = 0;
             branches &&
             index < branches->GetEntries();
             ++index) {

            auto *element =
                dynamic_cast<TBranchElement *>(
                    branches->At(index));

            if (element &&
                element->GetClassName()) {
                children.insert(
                    "/" +
                    std::string(
                        element->GetClassName()));
                continue;
            }

            auto *branch =
                dynamic_cast<TBranch *>(
                    branches->At(index));

            if (branch) {
                children.insert(
                    "/" +
                    std::string(
                        branch->GetName()));
            }
        }
    }

    if (children.empty()) {
        throw IOException(
            "No readable ROOT objects were found in file");
    }

    bind_data.browse_children.assign(
        children.begin(),
        children.end());

    bind_data.is_browse_mode = true;

    return_names.emplace_back("path");

    return_types.emplace_back(
        LogicalType(
            LogicalTypeId::VARCHAR));
}

void RootScanBinder::BindRequestedPath(
    RootScanBindData &bind_data,
    TableFunctionBindInput &input,
    bool dictionary_loaded,
    std::vector<std::string> &return_names,
    std::vector<LogicalType> &return_types)
{
    const std::string path_prefix_raw =
        input.named_parameters[
            "path_prefix"].ToString();

    RootDebug(
        "BIND.PATH",
        "path_prefix=" + path_prefix_raw);

    auto file =
        OpenRepresentativeFile(bind_data);

    if (rootlake::TryBindRootHistogram(
            *file,
            path_prefix_raw,
            bind_data.histogram_binding,
            bind_data.histogram_object)) {

        if (bind_data.IsMultiFile()) {
            throw NotImplementedException(
                "ROOT histogram object mode currently "
                "accepts one ROOT file per read_root() call");
        }

        bind_data.is_histogram_mode = true;

        bind_data.total_rows =
            bind_data.histogram_binding.row_count;

        const auto &schema =
            bind_data.histogram_binding.schema;

        return_names = schema.names;
        return_types = schema.types;

        RootDebug(
            "BIND.HISTOGRAM",
            "path=" +
                bind_data.histogram_binding.object_path +
            " class=" +
                bind_data.histogram_binding.class_name +
            " view=" +
                rootlake::RootHistogramViewName(
                    bind_data.histogram_binding.view) +
            " rows=" +
                std::to_string(
                    bind_data.histogram_binding.row_count));

        return;
    }

    const auto requested_path =
        rootlake::ParsePathPrefix(
            path_prefix_raw);

    if (!requested_path.fields.empty() &&
        !dictionary_loaded) {
        throw InvalidInputException(
            "Semantic ROOT path '" +
            path_prefix_raw +
            "' requires dictionary := "
            "'/path/to/libDictionary.so'. "
            "Binding complex classes from embedded "
            "StreamerInfo without a runtime dictionary "
            "is disabled because ROOT may construct "
            "unsafe emulated classes.");
    }

    TTree *tree =
        rootlake::FindTree(
            file.get(),
            "",
            "");

    if (!tree) {
        throw IOException(
            "No TTree found in ROOT file and requested "
            "path is not a supported ROOT analysis object.");
    }

    if (!BindSemanticPath(
            bind_data,
            file.get(),
            path_prefix_raw,
            return_names,
            return_types)) {

        BindPrimitiveCompatibility(
            bind_data,
            *file,
            *tree,
            path_prefix_raw,
            return_names,
            return_types);
    }

    AddMultiFileIdentityColumns(
        bind_data,
        return_names,
        return_types);
}

std::vector<RootPrimitiveBranch> RootScanBinder::CollectPrimitiveBranches(
    TTree &tree) {
    std::vector<RootPrimitiveBranch> result;
    auto *branches = tree.GetListOfBranches();
    for (int index = 0; branches && index < branches->GetEntries(); ++index) {
        auto *branch = dynamic_cast<TBranch *>(branches->At(index));
        if (!branch || dynamic_cast<TBranchElement *>(branch)) continue;
        TLeaf *leaf = branch->GetLeaf(branch->GetName());
        if (!leaf) continue;
        result.push_back({branch->GetName(), leaf->GetTypeName(), branch, leaf});
    }
    return result;
}

bool RootScanBinder::IsTreeName(TFile &file, const std::string &name) {
    TIter next(file.GetListOfKeys());
    while (auto *key = dynamic_cast<TKey *>(next())) {
        if (std::string(key->GetClassName()) == "TTree" &&
            std::string(key->GetName()) == name) return true;
    }
    return false;
}

void RootScanBinder::BindPrimitiveCompatibility(
    RootScanBindData &bind_data, TFile &file, TTree &tree,
    const std::string &path_prefix,
    std::vector<std::string> &return_names,
    std::vector<LogicalType> &return_types) {
    const auto branches = CollectPrimitiveBranches(tree);
    const std::string target_name =
        !path_prefix.empty() && path_prefix.front() == '/'
            ? path_prefix.substr(1) : path_prefix;

    if (IsTreeName(file, target_name)) {
        auto *target_tree =
            dynamic_cast<TTree *>(
                file.Get(target_name.c_str()));

        if (!target_tree &&
            std::string(tree.GetName()) == target_name) {
            target_tree = &tree;
        }

        if (!target_tree) {
            throw IOException(
                "ROOT TTree '" + target_name +
                "' was found in file keys but could not be opened");
        }

        const auto tree_primitives =
            CollectPrimitiveBranches(*target_tree);

        auto *tree_branches =
            target_tree->GetListOfBranches();

        bool primitive_only =
            tree_branches &&
            tree_branches->GetEntries() > 0 &&
            static_cast<idx_t>(
                tree_branches->GetEntries()) ==
                tree_primitives.size();

        if (primitive_only) {
            for (const auto &branch : tree_primitives) {
                if (!branch.leaf ||
                    branch.leaf->GetLeafCount() ||
                    branch.leaf->GetLenStatic() != 1) {
                    primitive_only = false;
                    break;
                }
            }
        }

        if (primitive_only) {
            bind_data.is_primitive_tree_mode = true;
            bind_data.tree_name =
                target_tree->GetName();
            bind_data.total_rows =
                static_cast<uint64_t>(
                    std::max<Long64_t>(
                        0,
                        target_tree->GetEntries()));

            AddEventIdColumn(
                bind_data,
                return_names,
                return_types);

            for (const auto &branch :
                 tree_primitives) {
                RootScanColumn column;
                column.name = branch.name;
                column.branch_name = branch.name;
                column.root_type = branch.type_name;

                bind_data.columns.push_back(
                    std::move(column));

                return_names.emplace_back(
                    branch.name);

                return_types.emplace_back(
                    rootlake::RootTypeToScanLogicalType(
                        branch.type_name,
                        false,
                        true));
            }

            RootDebug(
                "BIND.PRIMITIVE_TREE",
                "tree=" + bind_data.tree_name +
                " columns=" +
                std::to_string(
                    tree_primitives.size()));

            return;
        }

        bind_data.is_browse_mode = true;

        for (const auto &branch :
             tree_primitives) {
            bind_data.browse_children.push_back(
                "/" + branch.name);
        }

        return_names.emplace_back("path");
        return_types.emplace_back(
            LogicalType(LogicalTypeId::VARCHAR));
        return;
    }

    for (const auto &branch : branches) {
        if (branch.name != target_name) continue;
        bind_data.is_direct_branch_mode = true;
        bind_data.direct_branch_info = branch;
        bind_data.tree_name = tree.GetName();
        bind_data.total_rows = tree.GetEntries();
        AddEventIdColumn(bind_data, return_names, return_types);

        RootScanColumn column;
        column.name = branch.name;
        column.branch_name = branch.name;
        column.root_type = branch.type_name;
        column.is_string = rootlake::IsStringType(branch.type_name);
        bind_data.columns.push_back(std::move(column));
        return_names.emplace_back(branch.name);
        return_types.emplace_back(rootlake::RootTypeToScanLogicalType(
            branch.type_name, false, true));
        return;
    }

    BindEmptyResult(bind_data, path_prefix, return_names, return_types);
}

unique_ptr<GlobalTableFunctionState> RootScanStateFactory::CreateGlobal(
    ClientContext& context, TableFunctionInitInput& input)
{
    auto& bind_data = input.bind_data->Cast<RootScanBindData>();
    auto global_state = make_uniq<RootScanGlobalState>();

    global_state->browse_offset = 0;
    global_state->histogram_mode = bind_data.is_histogram_mode;
    if (input.filters) global_state->filters = input.filters->Copy();
    global_state->scan_column_ids = input.column_ids;
    if (input.projection_ids.empty()) {
        global_state->output_column_ids = input.column_ids;
    } else {
        global_state->output_column_ids.reserve(input.projection_ids.size());
        for (const auto projection_id : input.projection_ids) {
            if (projection_id >= input.column_ids.size()) {
                throw InternalException("Invalid read_root projection id");
            }
            global_state->output_column_ids.push_back(input.column_ids[projection_id]);
        }
    }
    global_state->total_rows = bind_data.total_rows;
    global_state->next_row = 0;
    if (bind_data.IsMultiFile() &&
        !bind_data.is_browse_mode &&
        !bind_data.is_empty_mode &&
        !bind_data.is_histogram_mode) {
        const auto runtime = rootlake::RootRuntimeSettings::From(context, bind_data.root_paths.size());
        global_state->file_scheduler = make_uniq<rootlake::RootDirectFileScheduler>(
            bind_data.root_paths, runtime.threads);
    }
    if (!bind_data.is_browse_mode &&
        !bind_data.is_histogram_mode &&
        global_state->filters) {
        for (const auto &entry : global_state->filters->filters) {
            if (entry.first >= global_state->scan_column_ids.size()) continue;
            const auto full_column = global_state->scan_column_ids[entry.first];
            if (full_column == bind_data.source_id_column && global_state->file_scheduler) {
                const auto source_range = rootlake::ExtractRootUnsignedRange(*entry.second);
                if (!source_range.known) continue;
                if (source_range.impossible) {
                    global_state->event_range_impossible = true;
                    break;
                }
                global_state->file_scheduler->SetSourceRange(source_range.lower, source_range.upper);
                continue;
            }
            if (full_column != 0 && full_column != COLUMN_IDENTIFIER_ROW_ID) continue;
            const auto range = rootlake::ExtractRootUnsignedRange(*entry.second);
            if (!range.known) continue;
            if (range.impossible) {
                global_state->event_range_impossible = true;
                if (!bind_data.IsMultiFile()) {
                    global_state->next_row = bind_data.total_rows;
                    global_state->total_rows = bind_data.total_rows;
                }
                break;
            }
            global_state->event_lower = std::max(global_state->event_lower, range.lower);
            if (range.upper != std::numeric_limits<uint64_t>::max()) {
                global_state->event_upper = std::min(global_state->event_upper, range.upper);
            }
            if (!bind_data.IsMultiFile()) {
                global_state->next_row = std::max(global_state->next_row, range.lower);
                if (range.upper != std::numeric_limits<uint64_t>::max()) {
                    global_state->total_rows = std::min(global_state->total_rows, range.upper + 1);
                }
            }
        }
    }
    global_state->scheduled_rows = global_state->total_rows >= global_state->next_row
        ? global_state->total_rows - global_state->next_row : 0;
    return std::move(global_state);
}

void RootScanFileManager::Open(
    const RootScanBindData& bind_data, RootScanGlobalState& gstate,
    RootScanLocalState& target, const std::string& file_path,
    bool synchronize_open)
{
    RootDebugOperationScope debug_operation("RootScanInitLocal");
    RootDebug("INIT_LOCAL.BEGIN",
              "root_path=" + file_path +
              " tree=" + bind_data.tree_name +
              " columns=" + std::to_string(bind_data.columns.size()));

    auto* local_state = &target;
    auto* open_mutex = synchronize_open ? &gstate.coordination_mutex : nullptr;

    if (bind_data.is_primitive_tree_mode)
    {
        const auto open_result =
            local_state->root_file.Open(
                file_path,
                bind_data.tree_name,
                open_mutex);

        if (gstate.file_scheduler) {
            gstate.file_scheduler->RecordOpen(
                local_state->file_task,
                open_result.attempts,
                open_result.elapsed_us);
        }

        auto *tree =
            local_state->root_file.GetTTree();

        if (!tree) {
            throw IOException(
                "ROOT schema mismatch in " +
                file_path +
                ": primitive TTree '" +
                bind_data.tree_name +
                "' is absent");
        }

        local_state->primitive_tree_branches.assign(
            bind_data.columns.size(),
            nullptr);

        local_state->primitive_tree_leaves.assign(
            bind_data.columns.size(),
            nullptr);

        local_state->primitive_tree_requires_read =
            false;

        std::set<idx_t> required_columns;

        for (const auto column :
             gstate.scan_column_ids) {
            if (column != COLUMN_IDENTIFIER_ROW_ID &&
                column < bind_data.columns.size()) {
                required_columns.insert(column);
            }
        }

        for (const auto column :
             gstate.output_column_ids) {
            if (column != COLUMN_IDENTIFIER_ROW_ID &&
                column < bind_data.columns.size()) {
                required_columns.insert(column);
            }
        }

        std::vector<TBranch *>
            projected_branches;

        for (const auto column_index :
             required_columns) {

            if (column_index == 0 ||
                column_index ==
                    bind_data.source_id_column ||
                column_index ==
                    bind_data.source_path_column) {
                continue;
            }

            const auto &column =
                bind_data.columns[
                    column_index];

            if (column.branch_name.empty()) {
                continue;
            }

            auto *branch =
                tree->GetBranch(
                    column.branch_name.c_str());

            if (!branch) {
                throw IOException(
                    "ROOT schema mismatch in " +
                    file_path +
                    ": primitive branch '" +
                    column.branch_name +
                    "' is absent");
            }

            auto *leaf =
                branch->GetLeaf(
                    branch->GetName());

            if (!leaf) {
                throw IOException(
                    "ROOT schema mismatch in " +
                    file_path +
                    ": primitive leaf '" +
                    column.branch_name +
                    "' is absent");
            }

            if (std::string(
                    leaf->GetTypeName()) !=
                column.root_type) {
                throw IOException(
                    "ROOT schema mismatch in " +
                    file_path +
                    ": primitive branch '" +
                    column.branch_name +
                    "' changed type from " +
                    column.root_type +
                    " to " +
                    leaf->GetTypeName());
            }

            local_state
                ->primitive_tree_branches[
                    column_index] = branch;

            local_state
                ->primitive_tree_leaves[
                    column_index] = leaf;

            projected_branches.push_back(
                branch);
        }

        if (projected_branches.empty()) {
            tree->SetBranchStatus("*", 0);
        } else {
            const auto projection =
                rootlake::ApplyBranchProjection(
                    tree,
                    projected_branches,
                    bind_data.tree_cache_bytes);

            if (!projection.applied) {
                rootlake::EnableAllBranches(
                    tree,
                    bind_data.tree_cache_bytes);
            }

            local_state
                ->primitive_tree_requires_read =
                    true;
        }

        RootDebug(
            "PRIMITIVE_TREE.PROJECTION",
            "file=" + file_path +
            " projected_branches=" +
            std::to_string(
                projected_branches.size()));

        if (gstate.file_scheduler) {
            const auto entries =
                static_cast<uint64_t>(
                    std::max<Long64_t>(
                        0,
                        tree->GetEntries()));

            local_state->local_current_row =
                gstate.event_range_impossible
                    ? entries
                    : std::min(
                          entries,
                          gstate.event_lower);

            local_state->local_end_row =
                entries;

            if (gstate.event_upper !=
                std::numeric_limits<
                    uint64_t>::max()) {
                local_state->local_end_row =
                    std::min(
                        entries,
                        gstate.event_upper + 1);
            }

            local_state->file_active = true;

            std::ostringstream fingerprint;
            fingerprint
                << "primitive-tree:"
                << bind_data.tree_name;

            for (const auto &column :
                 bind_data.columns) {
                if (!column.branch_name.empty()) {
                    fingerprint
                        << "|"
                        << column.branch_name
                        << ":"
                        << column.root_type;
                }
            }

            gstate.file_scheduler->ObserveSchema(
                fingerprint.str());
        }

        return;
    }

    if (bind_data.is_direct_branch_mode)
    {
        const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
        if (gstate.file_scheduler) {
            gstate.file_scheduler->RecordOpen(local_state->file_task,
                                              open_result.attempts, open_result.elapsed_us);
        }
        auto* tree = local_state->root_file.GetTTree();
        if (tree)
        {
            local_state->direct_branch = tree->GetBranch(bind_data.direct_branch_info.name.c_str());
            if (local_state->direct_branch)
            {
                local_state->direct_leaf = local_state->direct_branch->GetLeaf(
                    local_state->direct_branch->GetName());
                std::vector<TBranch *> projected_branches {local_state->direct_branch};
                if (local_state->direct_leaf && local_state->direct_leaf->GetLeafCount() &&
                    local_state->direct_leaf->GetLeafCount()->GetBranch()) {
                    projected_branches.push_back(
                        local_state->direct_leaf->GetLeafCount()->GetBranch());
                }
                const auto projection = rootlake::ApplyBranchProjection(
                    tree, projected_branches, bind_data.tree_cache_bytes);
                if (!projection.applied) {
                    rootlake::EnableAllBranches(tree, bind_data.tree_cache_bytes);
                }
            }
        }
        if (!local_state->direct_branch || !local_state->direct_leaf) {
            throw IOException("ROOT schema mismatch in " + file_path +
                              ": primitive branch '" + bind_data.direct_branch_info.name + "' is absent");
        }
        if (gstate.file_scheduler) {
            const auto entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));
            local_state->local_current_row = gstate.event_range_impossible
                ? entries : std::min(entries, gstate.event_lower);
            local_state->local_end_row = entries;
            if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
                local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
            }
            local_state->file_active = true;
            gstate.file_scheduler->ObserveSchema("primitive:" + bind_data.direct_branch_info.type_name);
        }
        return;
    }

    const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
    if (gstate.file_scheduler) {
        gstate.file_scheduler->RecordOpen(local_state->file_task,
                                          open_result.attempts, open_result.elapsed_us);
    }
    auto* file = local_state->root_file.GetTFile();
    if (!file || file->IsZombie())
    {
        throw IOException("Invalid ROOT file: " + file_path);
    }

    std::set<std::string> unique_root_classes;
    for (idx_t col_idx : gstate.scan_column_ids)
    {
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size())
        {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (!col.branch_name.empty())
        {
            unique_root_classes.insert(col.branch_name);
        }
    }

    if (unique_root_classes.empty())
    {
        for (const auto& col : bind_data.columns)
        {
            if (!col.branch_name.empty())
            {
                unique_root_classes.insert(col.branch_name);
            }
        }
    }

    for (const auto& root_class_name : unique_root_classes)
    {
        try {
            rootlake::RootObjectReader reader;
            reader.Bind(file, "", root_class_name,
                        bind_data.dictionary_cleanup_mode);
            local_state->root_readers.emplace(
                root_class_name, std::move(reader));
        } catch (const std::exception &exception) {
            if (gstate.file_scheduler) {
                throw IOException("ROOT schema mismatch in " + file_path +
                                  ": " + exception.what());
            }
        }
    }

    std::vector<idx_t> serialized_candidates;
    for (const auto col_idx : gstate.scan_column_ids)
    {
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size()) continue;
        const auto& col = bind_data.columns[col_idx];
        if (col.is_virtual_index || col.is_string || col.levels.empty() || col.logical_path.empty()) continue;
        if (std::find(serialized_candidates.begin(), serialized_candidates.end(), col_idx) ==
            serialized_candidates.end()) {
            serialized_candidates.push_back(col_idx);
        }
    }
    if (serialized_candidates.empty())
    {
        for (idx_t col_idx = 0; col_idx < bind_data.columns.size(); ++col_idx)
        {
            const auto& col = bind_data.columns[col_idx];
            if (!col.is_virtual_index && !col.is_string && !col.levels.empty() && !col.logical_path.empty()) {
                serialized_candidates.push_back(col_idx);
            }
        }
    }

    if (serialized_candidates.size() == 1)
    {
        const auto col_idx = serialized_candidates.front();
        const auto& col = bind_data.columns[col_idx];
        auto reader_it = local_state->root_readers.find(col.branch_name);
        if (reader_it != local_state->root_readers.end())
        {
            auto& object_reader = reader_it->second;
            auto parsed = rootlake::ParsePath(col.logical_path);
            local_state->path_reader.Resolve(
                object_reader.Tree(), object_reader.ObjectBranch(),
                object_reader.RootClass(), std::move(parsed), col.levels);
            const auto projection =
                local_state->path_reader.PhysicalMode() == "ancestor"
                ? rootlake::ApplyBranchProjection(
                    object_reader.Tree(),
                    {local_state->path_reader.PhysicalBranch()},
                    bind_data.tree_cache_bytes)
                : rootlake::BranchProjectionResult {};
            if (!projection.applied) {
                rootlake::EnableAllBranches(
                    object_reader.Tree(), bind_data.tree_cache_bytes);
            }
            rootlake::RootPathReaderOptions reader_options;
            reader_options.reader_mode = bind_data.reader_mode;
            reader_options.validation_entries =
                bind_data.raw_validation_entries;
            reader_options.max_entry_bytes = bind_data.raw_max_entry_bytes;
            reader_options.max_values_per_entry =
                bind_data.raw_max_values_per_entry;
            reader_options.tree_cache_bytes = bind_data.tree_cache_bytes;
            reader_options.enable_all_branches_on_fallback = true;
            local_state->path_reader.StartSerialized(
                object_reader.CurrentObject(), std::move(reader_options));
            local_state->serialized_column = col_idx;
        }
        else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED)
        {
            throw InvalidInputException("reader_mode='serialized' cannot bind ROOT object context for " +
                                        col.logical_path);
        }
        else
        {
            rootlake::WarnRootFallbackOnce(col.logical_path, "unknown",
                                           "ROOT object context is unavailable");
        }
    }
    else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED &&
             serialized_candidates.size() > 1)
    {
        throw InvalidInputException(
            "reader_mode='serialized' requires exactly one materialized logical ROOT value column");
    }

    for (const auto& col : bind_data.columns)
    {
        if (col.is_virtual_index)
        {
            local_state->has_container_columns = true;
            break;
        }
    }

    if (gstate.file_scheduler) {
        uint64_t entries = 0;
        if (!local_state->root_readers.empty()) {
            entries = static_cast<uint64_t>(std::max<Long64_t>(
                0, local_state->root_readers.begin()->second.Tree()->GetEntries()));
        }
        local_state->local_current_row = gstate.event_range_impossible
            ? entries : std::min(entries, gstate.event_lower);
        local_state->local_end_row = entries;
        if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
            local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
        }
        local_state->file_active = true;
        const auto fingerprint =
            local_state->path_reader.SerializedPlan().schema_fingerprint.empty()
            ? std::string("object:") + bind_data.tree_name
            : local_state->path_reader.SerializedPlan().schema_fingerprint;
        gstate.file_scheduler->ObserveSchema(fingerprint);
    }
}

unique_ptr<LocalTableFunctionState> RootScanStateFactory::CreateLocal(
    TableFunctionInitInput& input,
    GlobalTableFunctionState* global_state_p)
{
    auto& bind_data = input.bind_data->Cast<RootScanBindData>();
    auto& gstate = global_state_p->Cast<RootScanGlobalState>();
    auto local_state = make_uniq<RootScanLocalState>();
    if (!bind_data.is_empty_mode &&
        !bind_data.is_browse_mode &&
        !bind_data.is_histogram_mode &&
        !bind_data.IsMultiFile()) {
        RootScanFileManager().Open(
            bind_data, gstate, *local_state, bind_data.root_path, true);
    }
    return std::move(local_state);
}

void RootScanFileManager::Reset(
    RootScanGlobalState& gstate, RootScanLocalState& local_state,
    bool completed)
{
    if (completed && local_state.file_active && gstate.file_scheduler) {
        const auto elapsed = std::chrono::steady_clock::now() - local_state.file_started;
        gstate.file_scheduler->RecordComplete(
            local_state.file_task,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    }
    local_state.path_reader.Reset();
    local_state.serialized_column = DConstants::INVALID_INDEX;
    local_state.serialized_values.clear();
    local_state.serialized_indices.clear();
    local_state.reported_serialized_baskets = 0;
    local_state.reported_serialized_compressed_bytes = 0;
    local_state.reported_serialized_entry_bytes = 0;
    local_state.cached_results.clear();
    local_state.has_cached_entry = false;
    local_state.current_entry = std::numeric_limits<uint64_t>::max();
    local_state.current_elem_idx = 0;
    local_state.root_readers.clear();
    local_state.direct_branch = nullptr;
    local_state.direct_leaf = nullptr;
    local_state.primitive_tree_branches.clear();
    local_state.primitive_tree_leaves.clear();
    local_state.primitive_tree_requires_read = false;
    local_state.root_file.Close();
    local_state.local_current_row = 0;
    local_state.local_end_row = 0;
    local_state.has_container_columns = false;
    local_state.file_active = false;
}

bool RootScanFileManager::EnsureReady(
    const RootScanBindData& bind_data, RootScanGlobalState& gstate,
    RootScanLocalState& local_state)
{
    if (!gstate.file_scheduler) throw InternalException("multi-file ROOT scheduler is unavailable");
    while (true) {
        if (local_state.file_active &&
            (local_state.has_cached_entry || local_state.local_current_row < local_state.local_end_row)) {
            return true;
        }
        if (local_state.file_active) Reset(gstate, local_state, true);

        rootlake::RootDirectFileTask task;
        if (!gstate.file_scheduler->Claim(task)) {
            if (gstate.file_scheduler->AllFilesFinished()) {
                const auto failures = gstate.file_scheduler->FailureSummary();
                if (!failures.empty()) throw IOException(failures);
            }
            return false;
        }

        local_state.file_task = std::move(task);
        local_state.file_started = std::chrono::steady_clock::now();
        try {
            Open(bind_data, gstate, local_state,
                 local_state.file_task.path, false);
        } catch (const rootlake::RootFileUnavailableException &exception) {
            gstate.file_scheduler->RecordUnavailable(
                local_state.file_task, exception.attempts, exception.elapsed_us);
            Reset(gstate, local_state, false);
            continue;
        } catch (const std::exception &exception) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, exception.what());
            Reset(gstate, local_state, false);
            continue;
        } catch (...) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, "unknown ROOT reader error");
            Reset(gstate, local_state, false);
            continue;
        }
    }
}

void RootScanExecutor::ProcessHistogramMode(
    ClientContext &context,
    const RootScanBindData &bind_data,
    RootScanGlobalState &gstate,
    RootScanLocalState &lstate,
    DataChunk &output)
{
    if (!bind_data.histogram_object) {
        throw InternalException(
            "ROOT histogram object is unavailable");
    }

    idx_t output_count = 0;

    std::vector<rootlake::RootScalarActual>
        row_values;

    while (output_count <
           STANDARD_VECTOR_SIZE) {

        if (lstate.local_current_row >=
            lstate.local_end_row) {

            RootEntryScheduler scheduler(
                gstate.next_row,
                gstate.total_rows,
                gstate.coordination_mutex);

            const auto batch =
                scheduler.ClaimWork(100000);

            if (!batch.HasWork()) {
                break;
            }

            lstate.local_current_row =
                batch.start;

            lstate.local_end_row =
                batch.end;
        }

        const auto row =
            lstate.local_current_row++;

        rootlake::MaterializeRootHistogramRow(
            bind_data.histogram_binding,
            *bind_data.histogram_object,
            row,
            row_values);

        bool passes = true;

        if (gstate.filters) {
            for (const auto &filter :
                 gstate.filters->filters) {

                if (filter.first >=
                    gstate.scan_column_ids.size()) {
                    continue;
                }

                const auto column =
                    gstate.scan_column_ids[
                        filter.first];

                rootlake::RootScalarActual actual;

                if (column ==
                    COLUMN_IDENTIFIER_ROW_ID) {
                    actual =
                        rootlake::RootScalarActual
                            ::Unsigned(
                                row,
                                LogicalType(
                                    LogicalTypeId::UBIGINT));

                } else if (
                    column <
                    row_values.size()) {
                    actual =
                        row_values[column];

                } else {
                    actual =
                        rootlake::RootScalarActual
                            ::Null(
                                LogicalType::SQLNULL);
                }

                if (!lstate.filter_evaluator.Evaluate(
                        context,
                        *filter.second,
                        actual)) {
                    passes = false;
                    break;
                }
            }
        }

        if (!passes) {
            continue;
        }

        for (idx_t output_index = 0;
             output_index <
                 gstate.output_column_ids.size();
             ++output_index) {

            const auto column =
                gstate.output_column_ids[
                    output_index];

            auto &vector =
                output.data[output_index];

            if (column ==
                COLUMN_IDENTIFIER_ROW_ID) {

                FlatVector::GetData<int64_t>(
                    vector)[output_count] =
                    static_cast<int64_t>(row);

                FlatVector::Validity(vector)
                    .SetValid(output_count);

            } else if (
                column <
                row_values.size()) {

                rootlake::WriteRootHistogramActual(
                    vector,
                    output_count,
                    row_values[column]);

            } else {
                FlatVector::Validity(vector)
                    .SetInvalid(output_count);
            }
        }

        ++output_count;
    }

    output.SetCardinality(output_count);
}

void RootScanExecutor::ProcessBrowseMode(
    ClientContext& context,
    const RootScanBindData& bind_data,
    RootScanGlobalState& gstate,
    RootScanLocalState& lstate,
    DataChunk& output)
{
    const auto& children = bind_data.browse_children;
    std::lock_guard<std::mutex> lock(gstate.coordination_mutex);
    size_t start = gstate.browse_offset;

    if (start >= children.size())
    {
        output.SetCardinality(0);
        return;
    }

    size_t count = 0;
    size_t i = start;
    for (; i < children.size() && count < STANDARD_VECTOR_SIZE; ++i)
    {
        bool passes = true;
        if (gstate.filters)
        {
            for (const auto& filter : gstate.filters->filters)
            {
                if (filter.first >= gstate.scan_column_ids.size()) continue;
                const auto column = gstate.scan_column_ids[filter.first];
                const auto actual = column == COLUMN_IDENTIFIER_ROW_ID
                    ? rootlake::RootScalarActual::Event(i)
                    : (column == 0 ? rootlake::RootScalarActual::String(children[i])
                                   : rootlake::RootScalarActual::Null(LogicalType::SQLNULL));
                if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) {
                    passes = false;
                    break;
                }
            }
        }
        if (!passes) continue;
        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            auto& vector = output.data[output_index];
            const auto column = gstate.output_column_ids[output_index];
            if (column == 0)
            {
                FlatVector::GetData<string_t>(vector)[count] = StringVector::AddString(vector, children[i]);
                FlatVector::Validity(vector).SetValid(count);
            }
            else if (column == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(vector)[count] = static_cast<int64_t>(i);
                FlatVector::Validity(vector).SetValid(count);
            }
            else
            {
                FlatVector::Validity(vector).SetInvalid(count);
            }
        }
        ++count;
    }
    gstate.browse_offset = i;
    output.SetCardinality(count);
}

static rootlake::RootScalarActual PrimitiveScalarActual(
    const LogicalType &logical_type,
    const rootlake::RootPrimitiveValue &value)
{
    switch (value.kind)
    {
        case rootlake::RootPrimitiveKind::SIGNED:
            return rootlake::RootScalarActual::Signed(
                value.signed_value, logical_type);

        case rootlake::RootPrimitiveKind::UNSIGNED:
            return rootlake::RootScalarActual::Unsigned(
                value.unsigned_value, logical_type);

        case rootlake::RootPrimitiveKind::FLOATING:
            return rootlake::RootScalarActual::Numeric(
                logical_type, value.floating_value);
    }

    return rootlake::RootScalarActual::Null(logical_type);
}

bool RootScanExecutor::WriteNumericValue(
    Vector& vector, idx_t row,
    const rootlake::RootPrimitiveValue& value)
{
    switch (vector.GetType().id())
    {
        case LogicalTypeId::TINYINT:
            FlatVector::GetData<int8_t>(vector)[row] =
                static_cast<int8_t>(value.AsSigned());
            break;

        case LogicalTypeId::UTINYINT:
            FlatVector::GetData<uint8_t>(vector)[row] =
                static_cast<uint8_t>(value.AsUnsigned());
            break;

        case LogicalTypeId::SMALLINT:
            FlatVector::GetData<int16_t>(vector)[row] =
                static_cast<int16_t>(value.AsSigned());
            break;

        case LogicalTypeId::USMALLINT:
            FlatVector::GetData<uint16_t>(vector)[row] =
                static_cast<uint16_t>(value.AsUnsigned());
            break;

        case LogicalTypeId::INTEGER:
            FlatVector::GetData<int32_t>(vector)[row] =
                static_cast<int32_t>(value.AsSigned());
            break;

        case LogicalTypeId::UINTEGER:
            FlatVector::GetData<uint32_t>(vector)[row] =
                static_cast<uint32_t>(value.AsUnsigned());
            break;

        case LogicalTypeId::BIGINT:
            FlatVector::GetData<int64_t>(vector)[row] =
                value.AsSigned();
            break;

        case LogicalTypeId::UBIGINT:
            FlatVector::GetData<uint64_t>(vector)[row] =
                value.AsUnsigned();
            break;

        case LogicalTypeId::FLOAT:
            FlatVector::GetData<float>(vector)[row] =
                static_cast<float>(value.AsDouble());
            break;

        case LogicalTypeId::DOUBLE:
            FlatVector::GetData<double>(vector)[row] =
                value.AsDouble();
            break;

        case LogicalTypeId::BOOLEAN:
            FlatVector::GetData<bool>(vector)[row] =
                value.AsBool();
            break;

        default:
            FlatVector::Validity(vector).SetInvalid(row);
            return false;
    }

    FlatVector::Validity(vector).SetValid(row);
    return true;
}

std::optional<int32_t> RootScanExecutor::ResolveCachedIndexValue(
    const RootScanBindData& bind_data, const RootScanLocalState& lstate,
    idx_t col_idx, size_t elem_idx)
{
    if (col_idx >= bind_data.columns.size()) return std::nullopt;
    const auto& col = bind_data.columns[col_idx];
    std::string search = col.name;
    if (search.size() > 4 && search.substr(search.size() - 4) == "_idx") search.resize(search.size() - 4);
    for (idx_t candidate_index = 0; candidate_index < bind_data.columns.size(); ++candidate_index)
    {
        const auto& candidate = bind_data.columns[candidate_index];
        if (candidate.is_virtual_index || candidate.levels.empty() ||
            candidate.branch_name != col.branch_name || candidate_index >= lstate.cached_results.size()) continue;
        const auto& result = lstate.cached_results[candidate_index];
        if (elem_idx >= result.size() || elem_idx >= result.vector_indices.size()) continue;
        for (size_t name_index = 0; name_index < result.vector_names.size(); ++name_index)
        {
            std::string name = result.vector_names[name_index];
            if (name.size() > 4 && name.substr(name.size() - 4) == "_idx") name.resize(name.size() - 4);
            if (name == search && name_index < result.vector_indices[elem_idx].size())
            {
                return static_cast<int32_t>(result.vector_indices[elem_idx][name_index]);
            }
        }
    }
    return std::nullopt;
}

rootlake::RootScalarActual RootScanExecutor::CachedScalar(
    const RootScanBindData& bind_data, const RootScanLocalState& lstate,
    idx_t col_idx, uint64_t entry, size_t elem_idx)
{
    if (col_idx == COLUMN_IDENTIFIER_ROW_ID) return rootlake::RootScalarActual::Signed(entry);
    if (col_idx >= bind_data.columns.size()) return rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
    const auto& column = bind_data.columns[col_idx];
    if (column.name == "event_id" && column.levels.empty()) {
        return rootlake::RootScalarActual::Signed(entry);
    }
    if (col_idx == bind_data.source_id_column) {
        return rootlake::RootScalarActual::Event(lstate.file_task.source_id);
    }
    if (col_idx == bind_data.source_path_column) {
        return rootlake::RootScalarActual::String(lstate.file_task.path);
    }
    if (column.is_virtual_index) return rootlake::RootScalarActual::Index(
        ResolveCachedIndexValue(bind_data, lstate, col_idx, elem_idx));
    const auto logical_type = rootlake::RootTypeToScanLogicalType(
        column.root_type, column.is_string, true);
    if (col_idx >= lstate.cached_results.size()) return rootlake::RootScalarActual::Null(logical_type);
    const auto& result = lstate.cached_results[col_idx];
    if (elem_idx >= result.size()) return rootlake::RootScalarActual::Null(logical_type);
    if (result.is_string_flag[elem_idx]) return rootlake::RootScalarActual::String(result.strings[elem_idx]);
    return PrimitiveScalarActual(logical_type, result.numbers[elem_idx]);
}

bool RootScanExecutor::PassesCachedFilters(
    ClientContext& context, const RootScanBindData& bind_data,
    const RootScanGlobalState& gstate, RootScanLocalState& lstate,
    uint64_t entry, size_t elem_idx)
{
    if (!gstate.filters) return true;
    for (const auto& filter : gstate.filters->filters)
    {
        if (filter.first >= gstate.scan_column_ids.size()) continue;
        const auto actual = CachedScalar(bind_data, lstate, gstate.scan_column_ids[filter.first], entry, elem_idx);
        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) return false;
    }
    return true;
}

bool RootScanExecutor::PassesPrimitiveTreeFilters(
    ClientContext &context,
    const RootScanBindData &bind_data,
    const RootScanGlobalState &gstate,
    RootScanLocalState &lstate,
    uint64_t entry)
{
    if (!gstate.filters) {
        return true;
    }

    for (const auto &filter :
         gstate.filters->filters)
    {
        if (filter.first >=
            gstate.scan_column_ids.size()) {
            continue;
        }

        const auto column_index =
            gstate.scan_column_ids[
                filter.first];

        auto actual =
            rootlake::RootScalarActual::Null(
                LogicalType::SQLNULL);

        if (column_index == 0 ||
            column_index ==
                COLUMN_IDENTIFIER_ROW_ID) {

            actual =
                rootlake::RootScalarActual::Signed(
                    static_cast<int64_t>(
                        entry));

        } else if (
            column_index ==
                bind_data.source_id_column) {

            actual =
                rootlake::RootScalarActual::Event(
                    lstate.file_task.source_id);

        } else if (
            column_index ==
                bind_data.source_path_column) {

            actual =
                rootlake::RootScalarActual::String(
                    lstate.file_task.path);

        } else if (
            column_index <
                bind_data.columns.size() &&
            column_index <
                lstate
                    .primitive_tree_leaves
                    .size()) {

            const auto &column =
                bind_data.columns[
                    column_index];

            auto *leaf =
                lstate.primitive_tree_leaves[
                    column_index];

            if (leaf &&
                leaf->GetValuePointer()) {

                const auto value =
                    rootlake::RootPrimitiveValue
                        ::FromPointer(
                            leaf->GetValuePointer(),
                            column.root_type);

                const auto logical_type =
                    rootlake
                        ::RootTypeToScanLogicalType(
                            column.root_type,
                            false,
                            true);

                switch (value.kind) {
                case rootlake
                         ::RootPrimitiveKind
                         ::SIGNED:
                    actual =
                        rootlake
                            ::RootScalarActual
                            ::Signed(
                                value.signed_value,
                                logical_type);
                    break;

                case rootlake
                         ::RootPrimitiveKind
                         ::UNSIGNED:
                    actual =
                        rootlake
                            ::RootScalarActual
                            ::Unsigned(
                                value.unsigned_value,
                                logical_type);
                    break;

                case rootlake
                         ::RootPrimitiveKind
                         ::FLOATING:
                    actual =
                        rootlake
                            ::RootScalarActual
                            ::Numeric(
                                logical_type,
                                value.floating_value);
                    break;
                }
            }
        }

        if (!lstate.filter_evaluator.Evaluate(
                context,
                *filter.second,
                actual)) {
            return false;
        }
    }

    return true;
}

void RootScanExecutor::ProcessPrimitiveTree(
    ClientContext &context,
    const RootScanBindData &bind_data,
    RootScanGlobalState &gstate,
    RootScanLocalState &lstate,
    DataChunk &output)
{
    auto *tree =
        lstate.root_file.GetTTree();

    if (!tree) {
        output.SetCardinality(0);
        return;
    }

    idx_t output_count = 0;

    while (output_count <
           STANDARD_VECTOR_SIZE)
    {
        if (lstate.local_current_row >=
            lstate.local_end_row)
        {
            if (gstate.file_scheduler) {
                break;
            }

            RootEntryScheduler scheduler(
                gstate.next_row,
                gstate.total_rows,
                gstate.coordination_mutex);

            const auto batch =
                scheduler.ClaimWork(100000);

            if (!batch.HasWork()) {
                break;
            }

            lstate.local_current_row =
                batch.start;
            lstate.local_end_row =
                batch.end;
        }

        const auto entry =
            lstate.local_current_row++;

        if (lstate.primitive_tree_requires_read) {
            if (tree->GetEntry(
                    static_cast<Long64_t>(
                        entry)) < 0) {
                break;
            }
        }

        if (!PassesPrimitiveTreeFilters(
                context,
                bind_data,
                gstate,
                lstate,
                entry)) {
            continue;
        }

        for (idx_t output_index = 0;
             output_index <
                 gstate.output_column_ids.size();
             ++output_index)
        {
            const auto column_index =
                gstate.output_column_ids[
                    output_index];

            auto &vector =
                output.data[
                    output_index];

            if (column_index == 0 ||
                column_index ==
                    COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(
                    vector)[output_count] =
                    static_cast<int64_t>(
                        entry);

                FlatVector::Validity(vector)
                    .SetValid(output_count);

                continue;
            }

            if (column_index ==
                bind_data.source_id_column)
            {
                FlatVector::GetData<uint64_t>(
                    vector)[output_count] =
                    lstate.file_task.source_id;

                FlatVector::Validity(vector)
                    .SetValid(output_count);

                continue;
            }

            if (column_index ==
                bind_data.source_path_column)
            {
                FlatVector::GetData<string_t>(
                    vector)[output_count] =
                    StringVector::AddString(
                        vector,
                        lstate.file_task.path);

                FlatVector::Validity(vector)
                    .SetValid(output_count);

                continue;
            }

            if (column_index >=
                    bind_data.columns.size() ||
                column_index >=
                    lstate
                        .primitive_tree_leaves
                        .size())
            {
                FlatVector::Validity(vector)
                    .SetInvalid(output_count);
                continue;
            }

            auto *leaf =
                lstate.primitive_tree_leaves[
                    column_index];

            const auto &column =
                bind_data.columns[
                    column_index];

            if (!leaf ||
                !leaf->GetValuePointer())
            {
                FlatVector::Validity(vector)
                    .SetInvalid(output_count);
                continue;
            }

            const auto value =
                rootlake::RootPrimitiveValue
                    ::FromPointer(
                        leaf->GetValuePointer(),
                        column.root_type);

            WriteNumericValue(
                vector,
                output_count,
                value);
        }

        ++output_count;
    }

    output.SetCardinality(output_count);
}

bool RootScanExecutor::PassesDirectBranchFilters(
    ClientContext& context, const RootScanBindData& bind_data,
    const RootScanGlobalState& gstate, RootScanLocalState& lstate,
    uint64_t entry, const rootlake::RootPrimitiveValue &value)
{
    if (!gstate.filters) return true;
    const auto value_type = rootlake::RootTypeToScanLogicalType(
        bind_data.direct_branch_info.type_name, false, true);
    for (const auto& filter : gstate.filters->filters)
    {
        if (filter.first >= gstate.scan_column_ids.size()) continue;
        const auto column = gstate.scan_column_ids[filter.first];
        auto actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID) {
            actual = rootlake::RootScalarActual::Signed(entry);
        } else if (column == 1) {
            actual = PrimitiveScalarActual(value_type, value);
        } else if (column == bind_data.source_id_column) {
            actual = rootlake::RootScalarActual::Event(lstate.file_task.source_id);
        } else if (column == bind_data.source_path_column) {
            actual = rootlake::RootScalarActual::String(lstate.file_task.path);
        } else {
            actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) return false;
    }
    return true;
}

void RootScanExecutor::ProcessDirectBranch(
    ClientContext& context,
    const RootScanBindData& bind_data,
    RootScanGlobalState& gstate,
    RootScanLocalState& lstate,
    DataChunk& output)
{
    if (!lstate.direct_branch || !lstate.direct_leaf)
    {
        output.SetCardinality(0);
        return;
    }

    idx_t out_count = 0;
    while (out_count < STANDARD_VECTOR_SIZE)
    {
        if (lstate.local_current_row >= lstate.local_end_row)
        {
            if (gstate.file_scheduler) break;
            RootEntryScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
            auto batch = scheduler.ClaimWork(100000);
            if (!batch.HasWork()) break;
            lstate.local_current_row = batch.start;
            lstate.local_end_row = batch.end;
        }
        Long64_t entry = lstate.local_current_row;
        // Bare primitive-branch compatibility only.  Even here the entry is loaded
        // through TTree so no physical branch becomes an alternative semantic reader.
        auto* direct_tree = lstate.root_file.GetTTree();
        if (!direct_tree || direct_tree->GetEntry(entry) < 0)
        {
            break;
        }

        const auto val = rootlake::RootPrimitiveValue::FromPointer(
            lstate.direct_leaf->GetValuePointer(),
            bind_data.direct_branch_info.type_name);
        ++lstate.local_current_row;
        if (!PassesDirectBranchFilters(context, bind_data, gstate, lstate, entry, val)) continue;

        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            const auto column = gstate.output_column_ids[output_index];
            auto& vector = output.data[output_index];
            if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(vector)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vector).SetValid(out_count);
            }
            else if (column == 1)
            {
                WriteNumericValue(vector, out_count, val);
            }
            else if (column == bind_data.source_id_column)
            {
                FlatVector::GetData<uint64_t>(vector)[out_count] = lstate.file_task.source_id;
                FlatVector::Validity(vector).SetValid(out_count);
            }
            else if (column == bind_data.source_path_column)
            {
                FlatVector::GetData<string_t>(vector)[out_count] =
                    StringVector::AddString(vector, lstate.file_task.path);
                FlatVector::Validity(vector).SetValid(out_count);
            }
            else
            {
                FlatVector::Validity(vector).SetInvalid(out_count);
            }
        }

        out_count++;
    }
    output.SetCardinality(out_count);
}

void RootScanExecutor::ProcessCachedEntry(
    ClientContext& context,
    const RootScanBindData& bind_data,
    RootScanGlobalState& gstate,
    RootScanLocalState& lstate,
    DataChunk& output,
    idx_t& out_count)
{
    uint64_t entry = lstate.current_entry;
    size_t max_elements = 0;
    for (const auto& res : lstate.cached_results)
    {
        if (!res.empty())
        {
            max_elements = std::max(max_elements, res.size());
        }
    }
    if (max_elements == 0)
    {
        max_elements = 1;
    }

    size_t start_elem = lstate.current_elem_idx;
    size_t next_elem = start_elem;

    for (size_t elem_idx = start_elem; elem_idx < max_elements && out_count < STANDARD_VECTOR_SIZE; ++elem_idx)
    {
        next_elem = elem_idx + 1;
        if (!PassesCachedFilters(context, bind_data, gstate, lstate, entry, elem_idx)) continue;
        for (idx_t out_idx = 0; out_idx < gstate.output_column_ids.size(); ++out_idx)
        {
            idx_t col_idx = gstate.output_column_ids[out_idx];
            auto& vec = output.data[out_idx];

            if (col_idx == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx >= bind_data.columns.size())
            {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            const auto& col = bind_data.columns[col_idx];

            if (col.name == "event_id" && col.levels.empty())
            {
                FlatVector::GetData<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx == bind_data.source_id_column)
            {
                FlatVector::GetData<uint64_t>(vec)[out_count] = lstate.file_task.source_id;
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx == bind_data.source_path_column)
            {
                FlatVector::GetData<string_t>(vec)[out_count] =
                    StringVector::AddString(vec, lstate.file_task.path);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col.is_virtual_index)
            {
                std::string search = col.name;
                if (search.size() > 4 && search.substr(search.size() - 4) == "_idx")
                {
                    search = search.substr(0, search.size() - 4);
                }

                idx_t ref_col_idx = static_cast<idx_t>(-1);
                int idx_pos = -1;

                for (idx_t cand = 0; cand < bind_data.columns.size(); ++cand)
                {
                    const auto& candidate = bind_data.columns[cand];
                    if (candidate.is_virtual_index || candidate.levels.empty() ||
                        candidate.branch_name != col.branch_name ||
                        cand >= lstate.cached_results.size())
                    {
                        continue;
                    }

                    const auto& candidate_res = lstate.cached_results[cand];
                    if (candidate_res.empty())
                    {
                        continue;
                    }

                    for (size_t name_idx = 0; name_idx < candidate_res.vector_names.size(); ++name_idx)
                    {
                        std::string vn = candidate_res.vector_names[name_idx];
                        if (vn.size() > 4 && vn.substr(vn.size() - 4) == "_idx")
                        {
                            vn = vn.substr(0, vn.size() - 4);
                        }
                        if (vn == search)
                        {
                            ref_col_idx = cand;
                            idx_pos = static_cast<int>(name_idx);
                            break;
                        }
                    }

                    if (ref_col_idx != static_cast<idx_t>(-1))
                    {
                        break;
                    }
                }

                if (ref_col_idx == static_cast<idx_t>(-1) || idx_pos < 0)
                {
                    RootDebug("INDEX.NO_REFERENCE",
                              "column=" + col.name +
                              " branch=" + col.branch_name +
                              " projected_columns=" + std::to_string(gstate.output_column_ids.size()));
                    FlatVector::Validity(vec).SetInvalid(out_count);
                    continue;
                }

                const auto& ref_res = lstate.cached_results[ref_col_idx];
                if (elem_idx >= ref_res.size() ||
                    elem_idx >= ref_res.vector_indices.size() ||
                    static_cast<size_t>(idx_pos) >= ref_res.vector_indices[elem_idx].size())
                {
                    FlatVector::Validity(vec).SetInvalid(out_count);
                    continue;
                }

                FlatVector::GetData<int32_t>(vec)[out_count] =
                    ref_res.vector_indices[elem_idx][static_cast<size_t>(idx_pos)];
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx >= lstate.cached_results.size())
            {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            const auto& res = lstate.cached_results[col_idx];
            if (elem_idx >= res.size())
            {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            if (res.is_string_flag[elem_idx])
            {
                if (vec.GetType().id() == LogicalTypeId::VARCHAR)
                {
                    FlatVector::GetData<string_t>(vec)[out_count] = StringVector::AddString(vec, res.strings[elem_idx]);
                    FlatVector::Validity(vec).SetValid(out_count);
                }
                else
                {
                    FlatVector::Validity(vec).SetInvalid(out_count);
                }
            }
            else
            {
                const auto& val = res.numbers[elem_idx];
                if (!WriteNumericValue(vec, out_count, val))
                {
                    continue;
                }
            }
        }
        out_count++;
    }

    lstate.current_elem_idx = next_elem;
    if (lstate.current_elem_idx >= max_elements)
    {
        lstate.has_cached_entry = false;
        lstate.cached_results.clear();
        lstate.local_current_row++;
    }
}

std::vector<std::string> RootScanExecutor::SplitIndexSignature(
    const std::string& signature)
{
    std::vector<std::string> names;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

void RootScanExecutor::MaterializeSerializedResult(
    const RootScanColumn& column, uint64_t entry,
    const rootlake::SerializedReadPlan& plan,
    const std::vector<double>& values,
    const std::vector<int32_t>& flat_indices,
    rootlake::ReadResult& result)
{
    result.Clear();
    const auto index_names = SplitIndexSignature(column.index_signature);
    if (index_names.size() != plan.index_depth ||
        flat_indices.size() != values.size() * plan.index_depth) {
        throw InternalException("serialized ROOT index shape differs from bound SQL schema");
    }
    std::vector<int> indices(plan.index_depth);
    for (idx_t value_index = 0; value_index < values.size(); ++value_index) {
        for (idx_t depth = 0; depth < plan.index_depth; ++depth) {
            indices[depth] = flat_indices[value_index * plan.index_depth + depth];
        }
        std::vector<int32_t> flat_index_row(indices.begin(), indices.end());
        result.AddNumber(values[value_index], static_cast<Long64_t>(entry),
                         flat_index_row, index_names);
    }
}

CacheResult RootScanExecutor::ReadAndCacheEntry(
    const RootScanBindData& bind_data,
    RootScanGlobalState& gstate,
    RootScanLocalState& lstate,
    DataChunk& output,
    idx_t& out_count)
{
    uint64_t entry = lstate.local_current_row;

    std::map<std::string, std::vector<idx_t>> branch_columns;
    for (idx_t out_idx = 0; out_idx < gstate.scan_column_ids.size(); ++out_idx)
    {
        idx_t col_idx = gstate.scan_column_ids[out_idx];
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size())
        {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (!col.levels.empty() && !col.is_virtual_index && col.name != "event_id")
        {
            branch_columns[col.branch_name].push_back(col_idx);
        }
    }

    lstate.cached_results.resize(bind_data.columns.size());
    bool has_data = false;

    if (lstate.path_reader.SerializedActive() &&
        lstate.serialized_column < bind_data.columns.size())
    {
        const auto& column = bind_data.columns[lstate.serialized_column];
        auto reader_it = lstate.root_readers.find(column.branch_name);
        if (reader_it == lstate.root_readers.end()) {
            throw InternalException(
                "serialized ROOT path has no universal object reader");
        }
        rootlake::RootEntryReader object_entry(reader_it->second);
        object_entry.Begin(entry);
        const auto read = lstate.path_reader.TryReadSerialized(
            entry, object_entry, lstate.serialized_values,
            lstate.serialized_indices);
        gstate.object_validation_entries.fetch_add(object_entry.LoadCount());

        const auto& serialized_counters =
            lstate.path_reader.SerializedCounters();
        gstate.serialized_baskets.fetch_add(
            serialized_counters.baskets - lstate.reported_serialized_baskets);
        gstate.serialized_compressed_bytes.fetch_add(
            serialized_counters.compressed_bytes - lstate.reported_serialized_compressed_bytes);
        gstate.serialized_entry_bytes.fetch_add(
            serialized_counters.serialized_bytes - lstate.reported_serialized_entry_bytes);
        lstate.reported_serialized_baskets = serialized_counters.baskets;
        lstate.reported_serialized_compressed_bytes = serialized_counters.compressed_bytes;
        lstate.reported_serialized_entry_bytes = serialized_counters.serialized_bytes;
        if (read.decoded) {
            gstate.serialized_entries.fetch_add(1);
            gstate.serialized_values.fetch_add(lstate.serialized_values.size());
        }
        if (read.serialized)
        {
            MaterializeSerializedResult(
                                        column, entry,
                                        lstate.path_reader.SerializedPlan(),
                                        lstate.serialized_values, lstate.serialized_indices,
                                        lstate.cached_results[lstate.serialized_column]);
            has_data = !lstate.cached_results[lstate.serialized_column].empty();
            if (!has_data) {
                lstate.local_current_row++;
                return CacheResult::CONTINUE_LOOP;
            }
            lstate.current_entry = entry;
            lstate.current_elem_idx = 0;
            lstate.has_cached_entry = true;
            return CacheResult::CACHED;
        }
    }

    if (branch_columns.empty() && lstate.has_container_columns)
    {
        idx_t sample_col_idx = static_cast<idx_t>(-1);
        std::string sample_branch;

        for (idx_t i = 0; i < bind_data.columns.size(); ++i)
        {
            const auto& col = bind_data.columns[i];
            if (!col.levels.empty() && !col.is_virtual_index && col.name != "event_id")
            {
                sample_col_idx = i;
                sample_branch = col.branch_name;
                break;
            }
        }

        if (sample_col_idx != static_cast<idx_t>(-1))
        {
            auto it = lstate.root_readers.find(sample_branch);
            if (it != lstate.root_readers.end())
            {
                auto& reader = it->second;
                if (entry < static_cast<uint64_t>(reader.Tree()->GetEntries()))
                {
                    RootDebug("READ.BEFORE_GET_ENTRY",
                              "mode=sample class=" + sample_branch +
                              " entry=" + std::to_string(entry) +
                              " tree_ptr=" + RootPointer(reader.Tree()));
                    void *object = reader.Read(entry);
                    gstate.object_fallback_entries.fetch_add(1);
                    RootDebug("READ.AFTER_GET_ENTRY",
                              "mode=sample class=" + sample_branch +
                              " entry=" + std::to_string(entry) +
                              " object_ptr=" + RootPointer(object));
                    if (object)
                    {
                        const auto& col = bind_data.columns[sample_col_idx];
                        lstate.cached_results[sample_col_idx].Clear();
                        rootlake::OffsetValueReader::CollectDirect(
                            object, col.levels,
                            std::numeric_limits<Long64_t>::max(), entry,
                            lstate.cached_results[sample_col_idx]);
                        has_data = !lstate.cached_results[sample_col_idx].empty();
                    }
                }
            }
        }
    }
    else
    {
        for (const auto& [branch_name, col_indices] : branch_columns)
        {
            auto it = lstate.root_readers.find(branch_name);
            if (it == lstate.root_readers.end())
            {
                continue;
            }
            auto& reader = it->second;
            if (entry >= static_cast<uint64_t>(reader.Tree()->GetEntries()))
            {
                continue;
            }

            RootDebug("READ.BEFORE_GET_ENTRY",
                      "mode=group class=" + branch_name +
                      " entry=" + std::to_string(entry) +
                      " tree_ptr=" + RootPointer(reader.Tree()));
            void *object = reader.Read(entry);
            gstate.object_fallback_entries.fetch_add(1);
            RootDebug("READ.AFTER_GET_ENTRY",
                      "mode=group class=" + branch_name +
                      " entry=" + std::to_string(entry) +
                      " object_ptr=" + RootPointer(object));
            if (!object)
            {
                continue;
            }

            for (idx_t col_idx : col_indices)
            {
                const auto& col = bind_data.columns[col_idx];
                if (object)
                {
                    lstate.cached_results[col_idx].Clear();
                    rootlake::OffsetValueReader::CollectDirect(
                        object, col.levels,
                        std::numeric_limits<Long64_t>::max(), entry,
                        lstate.cached_results[col_idx]);
                    if (!lstate.cached_results[col_idx].empty())
                    {
                        has_data = true;
                    }
                }
            }
        }
    }

    if (!has_data)
    {
        if (lstate.has_container_columns)
        {
            lstate.local_current_row++;
            return CacheResult::CONTINUE_LOOP;
        }
        else
        {
            if (entry > 0)
            {
                lstate.local_current_row = lstate.local_end_row;
                return CacheResult::BREAK_LOOP;
            }
        }
    }

    lstate.current_entry = entry;
    lstate.current_elem_idx = 0;
    lstate.has_cached_entry = true;
    return CacheResult::CACHED;
}

void RootScanExecutor::Execute(
    ClientContext& context, TableFunctionInput& data_p, DataChunk& output)
{
    auto& bind_data = data_p.bind_data->Cast<RootScanBindData>();
    auto& gstate = data_p.global_state->Cast<RootScanGlobalState>();
    auto& lstate = data_p.local_state->Cast<RootScanLocalState>();

    if (bind_data.is_empty_mode)
    {
        output.SetCardinality(0);
        return;
    }
    if (gstate.event_range_impossible)
    {
        output.SetCardinality(0);
        return;
    }

    if (bind_data.is_histogram_mode)
    {
        ProcessHistogramMode(
            context,
            bind_data,
            gstate,
            lstate,
            output);
        return;
    }

    if (bind_data.is_browse_mode)
    {
        ProcessBrowseMode(context, bind_data, gstate, lstate, output);
        return;
    }

    if (bind_data.is_primitive_tree_mode)
    {
        while (true) {
            if (gstate.file_scheduler &&
                !file_manager.EnsureReady(
                    bind_data,
                    gstate,
                    lstate)) {
                output.SetCardinality(0);
                return;
            }

            ProcessPrimitiveTree(
                context,
                bind_data,
                gstate,
                lstate,
                output);

            if (output.size() > 0) {
                if (gstate.file_scheduler) {
                    gstate.file_scheduler
                        ->RecordFirstRow();
                }
                return;
            }

            if (!gstate.file_scheduler) {
                return;
            }
        }
    }

    if (bind_data.is_direct_branch_mode)
    {
        while (true) {
            if (gstate.file_scheduler &&
                !file_manager.EnsureReady(bind_data, gstate, lstate)) {
                output.SetCardinality(0);
                return;
            }
            ProcessDirectBranch(context, bind_data, gstate, lstate, output);
            if (output.size() > 0) {
                if (gstate.file_scheduler) gstate.file_scheduler->RecordFirstRow();
                return;
            }
            if (!gstate.file_scheduler) return;
        }
    }

    while (true) {
        if (gstate.file_scheduler &&
            !file_manager.EnsureReady(bind_data, gstate, lstate)) {
            output.SetCardinality(0);
            return;
        }

        idx_t out_count = 0;
        while (out_count < STANDARD_VECTOR_SIZE)
        {
            if (lstate.has_cached_entry)
            {
                ProcessCachedEntry(context, bind_data, gstate, lstate, output, out_count);
            }
            else
            {
                if (lstate.local_current_row >= lstate.local_end_row)
                {
                    if (gstate.file_scheduler) break;
                    RootEntryScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
                    auto batch = scheduler.ClaimWork(100000);
                    if (!batch.HasWork()) break;
                    lstate.local_current_row = batch.start;
                    lstate.local_end_row = batch.end;
                }
                CacheResult res = ReadAndCacheEntry(bind_data, gstate, lstate, output, out_count);
                if (res == CacheResult::CONTINUE_LOOP)
                {
                    continue;
                }
                if (res == CacheResult::BREAK_LOOP)
                {
                    lstate.local_current_row = lstate.local_end_row;
                    continue;
                }
            }
        }

        output.SetCardinality(out_count);
        if (out_count > 0) {
            if (gstate.file_scheduler) gstate.file_scheduler->RecordFirstRow();
            return;
        }
        if (!gstate.file_scheduler) return;
    }
}

InsertionOrderPreservingMap<string> RootScanExplain::Bound(
    TableFunctionToStringInput& input)
{
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data) return result;
    const auto& bind = input.bind_data->Cast<RootScanBindData>();
    result["ROOT Input"] = bind.input_specification;
    result["ROOT Files"] = std::to_string(bind.root_paths.size());
    result["ROOT Representative"] = bind.root_path;
    result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.raw_validation_entries);
    return result;
}

InsertionOrderPreservingMap<string> RootScanExplain::Running(
    TableFunctionDynamicToStringInput& input)
{
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data) {
        const auto& bind = input.bind_data->Cast<RootScanBindData>();
        result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.reader_mode);
        result["ROOT Input Files"] = std::to_string(bind.root_paths.size());
        result["ROOT Representative"] = bind.root_path;
        result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    }
    if (!input.global_state) return result;
    const auto& global = input.global_state->Cast<RootScanGlobalState>();
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entries.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Object Validation Entries"] = std::to_string(global.object_validation_entries.load());
    result["Object Fallback Entries"] = std::to_string(global.object_fallback_entries.load());
    if (global.file_scheduler) {
        result["ROOT Opened Files"] = std::to_string(global.file_scheduler->OpenedFiles());
        result["ROOT Completed Files"] = std::to_string(global.file_scheduler->CompletedFiles());
        result["ROOT Skipped Files"] = std::to_string(global.file_scheduler->SkippedFiles());
        result["ROOT Unavailable Files"] = std::to_string(global.file_scheduler->UnavailableFiles());
        result["ROOT Failed Files"] = std::to_string(global.file_scheduler->FailedFiles());
        result["ROOT Retried Opens"] = std::to_string(global.file_scheduler->RetriedOpens());
        result["ROOT Open Time (us)"] = std::to_string(global.file_scheduler->OpenTimeUs());
        result["ROOT Schema Variants"] = std::to_string(global.file_scheduler->SchemaVariants());
        result["ROOT Schema Plan Reuses"] = std::to_string(global.file_scheduler->SchemaPlanReuses());
        result["ROOT Time To First Row (us)"] = std::to_string(global.file_scheduler->FirstRowUs());
        result["ROOT Slowest File"] = global.file_scheduler->SlowestFile();
        result["ROOT Slowest File Time (us)"] = std::to_string(global.file_scheduler->SlowestFileUs());
    }
    return result;
}

unique_ptr<FunctionData> RootScanBind(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &return_names) {
    return RootScanBinder().Bind(
        context, input, return_types, return_names);
}

unique_ptr<GlobalTableFunctionState> RootScanInit(
    ClientContext &context, TableFunctionInitInput &input) {
    return RootScanStateFactory().CreateGlobal(context, input);
}

unique_ptr<LocalTableFunctionState> RootScanInitLocal(
    ExecutionContext &, TableFunctionInitInput &input,
    GlobalTableFunctionState *global_state) {
    return RootScanStateFactory().CreateLocal(input, global_state);
}

void RootScanFunction(
    ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    RootScanExecutor().Execute(context, input, output);
}

InsertionOrderPreservingMap<string> RootScanToString(
    TableFunctionToStringInput &input) {
    return RootScanExplain().Bound(input);
}

InsertionOrderPreservingMap<string> RootScanDynamicToString(
    TableFunctionDynamicToStringInput &input) {
    return RootScanExplain().Running(input);
}

void RegisterRootScan(ExtensionLoader &loader)
{
    TableFunction root_scan("read_root", {LogicalType::VARCHAR}, RootScanFunction, RootScanBind, RootScanInit);

    root_scan.named_parameters["dictionary"] = LogicalType::VARCHAR;
    root_scan.named_parameters["path_prefix"] = LogicalType::VARCHAR;
    root_scan.named_parameters["reader_mode"] = LogicalType::VARCHAR;
    root_scan.named_parameters["raw_validation_entries"] = LogicalType::UINTEGER;
    root_scan.named_parameters["raw_max_entry_bytes"] = LogicalType::UBIGINT;
    root_scan.named_parameters["raw_max_values_per_entry"] = LogicalType::UBIGINT;
    root_scan.named_parameters["tree_cache_bytes"] = LogicalType::UBIGINT;

    root_scan.filter_pushdown = true;
    root_scan.filter_prune = true;
    root_scan.projection_pushdown = true;
    root_scan.init_local = RootScanInitLocal;
    root_scan.to_string = RootScanToString;
    root_scan.dynamic_to_string = RootScanDynamicToString;
    loader.RegisterFunction(root_scan);
}

} // namespace duckdb
