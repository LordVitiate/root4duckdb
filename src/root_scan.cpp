#include "include/root_headers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
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

#include <nlohmann/json.hpp>
#include "include/root_meta.hpp"
#include "include/root_branch_projection.hpp"
#include "include/root_debug.hpp"
#include "include/root_dictionary.hpp"
#include "include/root_direct_scheduler.hpp"
#include "include/root_file_opener.hpp"
#include "include/root_filter.hpp"
#include "include/root_input_resolver.hpp"
#include "include/root_lake_common.hpp"
#include "include/root_runtime_settings.hpp"
#include "include/root_serialized_reader.hpp"

namespace duckdb {

struct SimpleBranchInfo {
    std::string name;
    std::string type_name;
    TBranch* branch = nullptr;
    TLeaf* leaf = nullptr;
};

static LogicalType RootTypeToDuckDB(const std::string& root_type, bool is_string, bool is_primitive)
{
    if (is_string || !is_primitive)
    {
        return LogicalType::VARCHAR;
    }

    std::string t = root_type;
    if (t == "I") return LogicalType::INTEGER;
    if (t == "F") return LogicalType::FLOAT;
    if (t == "D") return LogicalType::DOUBLE;
    if (t == "L") return LogicalType::BIGINT;
    if (t == "b") return LogicalType::TINYINT;   // signed char
    if (t == "B") return LogicalType::UTINYINT;  // unsigned char
    if (t == "O") return LogicalType::BOOLEAN;   // bool
    if (t == "S") return LogicalType::SMALLINT;
    if (t == "s") return LogicalType::USMALLINT;
    if (t == "i") return LogicalType::UINTEGER;
    if (t == "l") return LogicalType::UBIGINT;

    if (t == "Double_t" || t == "double") return LogicalType::DOUBLE;
    if (t == "Float_t" || t == "float") return LogicalType::FLOAT;
    if (t == "Int_t" || t == "int") return LogicalType::INTEGER;
    if (t == "Long64_t" || t == "long long") return LogicalType::BIGINT;
    if (t == "Char_t" || t == "char") return LogicalType::TINYINT;
    if (t == "UChar_t" || t == "unsigned char") return LogicalType::UTINYINT;    
    if (t == "Bool_t" || t == "bool") return LogicalType::BOOLEAN;
    if (t == "Short_t" || t == "short") return LogicalType::SMALLINT;
    if (t == "UShort_t" || t == "unsigned short") return LogicalType::USMALLINT;
    if (t == "UInt_t" || t == "unsigned int") return LogicalType::UINTEGER;
    if (t == "ULong_t" || t == "unsigned long" ||
        t == "ULong64_t" || t == "unsigned long long") return LogicalType::UBIGINT;

    return LogicalType::VARCHAR;
}

struct RootLakeColumnInfo
{
    std::string name;
    std::string logical_path;
    MetaColumnType type;
    std::string branch_name;
    std::string root_type;
    bool is_string = false;
    std::vector<rootlake::PathLevel> levels;
    std::string index_signature;
    bool is_virtual_index = false;

};

struct BasketMeta
{
    uint16_t column_index;
    uint64_t start_row;
    uint32_t num_rows;
    double min_value;
    double max_value;
    uint32_t bloom_size;
    std::vector<uint8_t> bloom_filter;

    [[nodiscard]] bool ContainsRow(uint64_t global_row) const noexcept
    {
        return global_row >= start_row && global_row < start_row + num_rows;
    }

    [[nodiscard]] uint64_t EndRow() const noexcept
    {
        return start_row + num_rows;
    }
};

class BinaryMetadataReader
{
    const std::vector<uint8_t>& buffer_;
    size_t offset_ = 0;

public:
    explicit BinaryMetadataReader(const std::vector<uint8_t>& buffer) : buffer_(buffer) {}

    template<typename T>
    T Read()
    {
        if (offset_ + sizeof(T) > buffer_.size())
        {
            throw IOException("Binary metadata EOF.");
        }
        T value;
        std::memcpy(&value, &buffer_[offset_], sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    std::vector<uint8_t> ReadBytes(uint32_t count)
    {
        if (offset_ + count > buffer_.size())
        {
            throw IOException("Binary metadata block EOF.");
        }
        std::vector<uint8_t> result(buffer_.begin() + offset_, buffer_.begin() + offset_ + count);
        offset_ += count;
        return result;
    }

    [[nodiscard]] bool HasMore() const
    {
        return offset_ < buffer_.size();
    }
};

class MetadataLoader
{
public:
    struct LoadResult
    {
        std::string tree_name;
        uint64_t total_rows = 0;
        std::vector<RootLakeColumnInfo> columns;
        std::vector<BasketMeta> baskets;
    };

private:
    static MetaColumnType ParseColumnType(const std::string& type_name)
    {
        if (type_name == "Int_t" || type_name == "I") return MetaColumnType::INT32;
        if (type_name == "Float_t" || type_name == "F") return MetaColumnType::FLOAT;
        if (type_name == "Double_t" || type_name == "D") return MetaColumnType::DOUBLE;
        if (type_name == "Long64_t" || type_name == "L") return MetaColumnType::INT64;
        return MetaColumnType::UNKNOWN;
    }

    static std::vector<BasketMeta> ParseBinaryTail(const std::vector<uint8_t>& buffer)
    {
        std::vector<BasketMeta> baskets;
        BinaryMetadataReader reader(buffer);

        while (reader.HasMore())
        {
            BasketMeta basket;
            basket.column_index = reader.Read<uint16_t>();
            basket.start_row = reader.Read<uint64_t>();
            basket.num_rows = reader.Read<uint32_t>();
            basket.min_value = reader.Read<double>();
            basket.max_value = reader.Read<double>();
            basket.bloom_size = reader.Read<uint32_t>();

            if (basket.bloom_size > 0)
            {
                basket.bloom_filter = reader.ReadBytes(basket.bloom_size);
            }
            baskets.push_back(std::move(basket));
        }
        return baskets;
    }

public:
    static LoadResult Load(const std::string& meta_path)
    {
        LoadResult result;

        std::ifstream in(meta_path, std::ios::binary);
        if (!in)
        {
            throw IOException("Index not found: " + meta_path);
        }

        std::string json_line;
        std::getline(in, json_line);
        auto root_meta_json = nlohmann::json::parse(json_line);

        result.tree_name = root_meta_json["tree_name"].get<std::string>();
        result.total_rows = root_meta_json["total_entries"].get<uint64_t>();

        for (const auto& col : root_meta_json["columns"])
        {
            RootLakeColumnInfo info;
            info.name = col["name"].get<std::string>();
            info.type = ParseColumnType(col["type"].get<std::string>());
            result.columns.push_back(std::move(info));
        }

        std::vector<uint8_t> binary_tail(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );
        in.close();

        result.baskets = ParseBinaryTail(binary_tail);
        return result;
    }
};

class WorkScheduler
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

    WorkScheduler(uint64_t& next_row, uint64_t total_rows, std::mutex& mtx)
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


struct FastRootBindData : public TableFunctionData
{
    RootDebugLifetimeSentinel lifetime_sentinel {"FastRootBindData"};
    std::string root_path;
    std::string input_specification;
    std::vector<std::string> root_paths;
    idx_t representative_source_id = 0;
    uint64_t bind_open_us = 0;
    std::string meta_path;
    std::string tree_name;
    uint64_t total_rows = 0;
    std::vector<RootLakeColumnInfo> columns;
    std::vector<BasketMeta> baskets;

    bool is_browse_mode = false;
    bool is_direct_branch_mode = false;
    bool is_empty_mode = false;           // safe zero-row result; never opens a TTree
    rootlake::RootDictionaryCleanupMode dictionary_cleanup_mode = rootlake::RootDictionaryCleanupMode::FULL;
    std::vector<std::string> browse_children;
    SimpleBranchInfo direct_branch_info;
    rootlake::RootReaderMode reader_mode = rootlake::RootReaderMode::AUTO;
    uint32_t raw_validation_entries = 4;
    uint64_t raw_max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t raw_max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    idx_t source_id_column = DConstants::INVALID_INDEX;
    idx_t source_path_column = DConstants::INVALID_INDEX;

    bool IsMultiFile() const { return root_paths.size() > 1; }

    ~FastRootBindData()
    {
        RootDebug("BIND_DATA.DTOR_BODY",
                  "this=" + RootPointer(this) +
                  " root_path=" + root_path +
                  " columns=" + std::to_string(columns.size()));
    }
};

struct FastRootGlobalState : public GlobalTableFunctionState
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

    idx_t MaxThreads() const override
    {
        if (file_scheduler) return file_scheduler->MaxThreads();
        return WorkScheduler::EstimateOptimalThreads(scheduled_rows);
    }
};

struct FastRootLocalState : public LocalTableFunctionState
{
    rootlake::RootFileHandle root_file;
    std::unordered_map<std::string, rootlake::RootObjectContext> root_contexts;

    uint64_t local_current_row = 0;
    uint64_t local_end_row = 0;

    uint64_t current_entry = std::numeric_limits<uint64_t>::max();
    size_t current_elem_idx = 0;
    std::vector<rootlake::ReadResult> cached_results;
    bool has_cached_entry = false;
    
    bool has_container_columns = false;
    rootlake::RootFilterEvaluator filter_evaluator;

    rootlake::SerializedReadPlan serialized_plan;
    rootlake::SerializedBasketReader serialized_reader;
    bool serialized_active = false;
    idx_t serialized_column = DConstants::INVALID_INDEX;
    std::string serialized_context;
    uint32_t validation_remaining = 0;
    std::vector<double> serialized_values;
    std::vector<int32_t> serialized_indices;
    uint64_t reported_serialized_baskets = 0;
    uint64_t reported_serialized_compressed_bytes = 0;
    uint64_t reported_serialized_entry_bytes = 0;

    TBranch* direct_branch = nullptr;
    TLeaf* direct_leaf = nullptr;

    bool file_active = false;
    rootlake::RootDirectFileTask file_task;
    std::chrono::steady_clock::time_point file_started;

    ~FastRootLocalState() = default;
};

static void AddEventIdColumn(
    FastRootBindData& bind_data,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    return_names.emplace_back("event_id");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    
    RootLakeColumnInfo col;
    col.name = "event_id";
    col.type = MetaColumnType::INT64;
    bind_data.columns.emplace_back(std::move(col));
}

static void BindDirectPrimitives(
    FastRootBindData& bind_data,
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
        RootLakeColumnInfo column;
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

        RootLakeColumnInfo col;
        col.name = flat_name;
        col.logical_path = full_path;
        col.type = MetaColumnType::UNKNOWN;
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
        resolved.duckdb_type = RootTypeToDuckDB(col.root_type, col.is_string, true);
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
        RootLakeColumnInfo index_column;
        index_column.name = index_name;
        index_column.type = MetaColumnType::INT32;
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

static void BindBrowseMode(
    FastRootBindData& bind_data,
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

static void BindEmptyResult(
    FastRootBindData& bind_data,
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
            RootLakeColumnInfo idx_col;
            idx_col.name = idx_name;
            idx_col.type = MetaColumnType::INT32;
            idx_col.is_virtual_index = true;
            idx_col.branch_name = rootlake::ParsePathPrefix(path_prefix).root_class;
            bind_data.columns.emplace_back(std::move(idx_col));
            return_names.emplace_back(idx_name);
            return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
        }
    }
}

static bool LoadRequestedDictionary(ClientContext& context, TableFunctionBindInput& input)
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

static rootlake::RootDictionaryCleanupMode ResolveDictionaryCleanupMode(const TableFunctionBindInput& input,
                                                                       bool dictionary_loaded)
{
    auto it = input.named_parameters.find("dictionary_cleanup");
    std::string mode = it == input.named_parameters.end() ? "auto" : it->second.ToString();
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode.empty() || mode == "auto") {
        return dictionary_loaded ? rootlake::RootDictionaryCleanupMode::RETAIN
                                 : rootlake::RootDictionaryCleanupMode::FULL;
    }
    if (mode == "retain" || mode == "none" || mode == "skip") {
        return rootlake::RootDictionaryCleanupMode::RETAIN;
    }
    if (mode == "destruct_only" || mode == "dtor_only") {
        return rootlake::RootDictionaryCleanupMode::DESTRUCT_ONLY;
    }
    if (mode == "full" || mode == "strict" || mode == "delete") {
        return rootlake::RootDictionaryCleanupMode::FULL;
    }
    throw InvalidInputException("dictionary_cleanup must be one of: auto, retain, destruct_only, full");
}

static bool BindSemanticPathWithoutMetadata(
    FastRootBindData& bind_data,
    TFile* file,
    const std::string& path_prefix_raw,
    const std::string& normalized_prefix,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    (void)normalized_prefix;
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

static std::unique_ptr<TFile> OpenRepresentativeFile(FastRootBindData& bind_data)
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

static void AddMultiFileIdentityColumns(FastRootBindData& bind_data,
                                        vector<string>& return_names,
                                        vector<LogicalType>& return_types)
{
    if (!bind_data.IsMultiFile() || bind_data.is_browse_mode ||
        bind_data.source_id_column != DConstants::INVALID_INDEX) return;

    bind_data.source_id_column = bind_data.columns.size();
    RootLakeColumnInfo source_id;
    source_id.name = "source_id";
    source_id.root_type = "ULong64_t";
    bind_data.columns.push_back(std::move(source_id));
    return_names.emplace_back("source_id");
    return_types.emplace_back(LogicalTypeId::UBIGINT);

    bind_data.source_path_column = bind_data.columns.size();
    RootLakeColumnInfo source_path;
    source_path.name = "source_path";
    source_path.root_type = "string";
    source_path.is_string = true;
    bind_data.columns.push_back(std::move(source_path));
    return_names.emplace_back("source_path");
    return_types.emplace_back(LogicalTypeId::VARCHAR);
}

unique_ptr<FunctionData> RootScanBind(
    ClientContext& context,
    TableFunctionBindInput& input,
    vector<LogicalType>& return_types,
    vector<string>& return_names)
{
    RootDebugOperationScope debug_operation("RootScanBind");
    auto bind_data = make_uniq<FastRootBindData>();

    bind_data->input_specification = input.inputs[0].ToString();
    bind_data->root_paths = rootlake::ResolveRootInputs(context, bind_data->input_specification);
    bind_data->root_path = bind_data->root_paths.front();
    auto reader_mode = input.named_parameters.find("reader_mode");
    if (reader_mode != input.named_parameters.end()) {
        bind_data->reader_mode = rootlake::ParseRootReaderMode(reader_mode->second.ToString());
    }
    auto raw_validation = input.named_parameters.find("raw_validation_entries");
    if (raw_validation != input.named_parameters.end()) {
        bind_data->raw_validation_entries = raw_validation->second.GetValue<uint32_t>();
    }
    auto raw_entry_limit = input.named_parameters.find("raw_max_entry_bytes");
    if (raw_entry_limit != input.named_parameters.end()) {
        bind_data->raw_max_entry_bytes = raw_entry_limit->second.GetValue<uint64_t>();
    }
    auto raw_value_limit = input.named_parameters.find("raw_max_values_per_entry");
    if (raw_value_limit != input.named_parameters.end()) {
        bind_data->raw_max_values_per_entry = raw_value_limit->second.GetValue<uint64_t>();
    }
    auto tree_cache = input.named_parameters.find("tree_cache_bytes");
    if (tree_cache != input.named_parameters.end()) {
        bind_data->tree_cache_bytes = tree_cache->second.GetValue<uint64_t>();
    }
    if (bind_data->raw_max_entry_bytes < 12) {
        throw InvalidInputException("raw_max_entry_bytes must be at least 12");
    }
    if (bind_data->raw_max_values_per_entry == 0) {
        throw InvalidInputException("raw_max_values_per_entry must be positive");
    }
    RootDebug("BIND.BEGIN",
              "root_input=" + bind_data->input_specification +
              " resolved_files=" + std::to_string(bind_data->root_paths.size()) +
              " inputs=" + std::to_string(input.inputs.size()) +
              " named_parameters=" + std::to_string(input.named_parameters.size()));
    bind_data->meta_path = bind_data->IsMultiFile() ? std::string() : bind_data->root_path + ".json";
    if (!bind_data->IsMultiFile() && input.inputs.size() > 1)
    {
        auto& fs = FileSystem::GetFileSystem(context);
        bind_data->meta_path = input.inputs[1].ToString() + "/" + fs.ExtractBaseName(bind_data->root_path) + ".json";
    }

    const bool dictionary_loaded = LoadRequestedDictionary(context, input);
    bind_data->dictionary_cleanup_mode =
        ResolveDictionaryCleanupMode(input, dictionary_loaded);
    RootDebug("BIND.DICTIONARY_DONE",
              "loaded=" + std::to_string(dictionary_loaded ? 1 : 0));

    bool use_path_prefix = input.named_parameters.find("path_prefix") != input.named_parameters.end();
    if (!use_path_prefix)
    {
        RootDebug("FILE.BEFORE_OPEN", "mode=root_browse path=" + bind_data->root_path);
        auto file = OpenRepresentativeFile(*bind_data);
        RootDebug("FILE.AFTER_OPEN",
                  "mode=root_browse file_ptr=" + RootPointer(file.get()) +
                  " zombie=" + std::to_string(file && file->IsZombie() ? 1 : 0));
        if (!file || file->IsZombie())
        {
            throw IOException("Failed to open ROOT file: " + bind_data->root_path);
        }
        TTree* tree = rootlake::FindTree(file.get(), "", "");
        if (!tree)
        {
            throw IOException("No TTree found in ROOT file.");
        }
        auto* branches = tree->GetListOfBranches();
        std::vector<std::string> root_paths;
        for (int i = 0; i < branches->GetEntries(); ++i)
        {
            auto* be = dynamic_cast<TBranchElement*>(branches->At(i));
            if (be && be->GetClassName())
            {
                root_paths.push_back("/" + std::string(be->GetClassName()));
            }
            else
            {
                auto* br = dynamic_cast<TBranch*>(branches->At(i));
                if (br)
                {
                    root_paths.push_back("/" + std::string(br->GetName()));
                }
            }
        }
        std::sort(root_paths.begin(), root_paths.end());
        root_paths.erase(std::unique(root_paths.begin(), root_paths.end()), root_paths.end());

        bind_data->is_browse_mode = true;
        bind_data->browse_children = root_paths;
        return_names.emplace_back("path");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        RootDebug("FILE.BEFORE_CLOSE", "mode=root_browse file_ptr=" + RootPointer(file.get()));
        file.reset();
        RootDebug("FILE.AFTER_CLOSE", "mode=root_browse");
        RootDebug("BIND.RETURN", "mode=root_browse");
        return std::move(bind_data);
    }

    std::string path_prefix_raw = input.named_parameters["path_prefix"].ToString();
    RootDebug("BIND.PATH", "path_prefix=" + path_prefix_raw);
    const auto requested_path = rootlake::ParsePathPrefix(path_prefix_raw);
    if (!requested_path.fields.empty() && !dictionary_loaded)
    {
        RootDebug("BIND.NO_DICTIONARY",
                  "semantic_path=" + path_prefix_raw + " refusing unsafe emulated-class bind");
        throw InvalidInputException(
            "Semantic ROOT path '" + path_prefix_raw +
            "' requires dictionary := '/path/to/libDictionary.so'. "
            "Binding complex classes from embedded StreamerInfo without a runtime dictionary is disabled because ROOT may construct unsafe emulated classes.");
    }
    std::string path_prefix = path_prefix_raw;
    if (!path_prefix.empty() && path_prefix.back() == '/')
    {
        path_prefix.pop_back();
    }
    size_t last_slash = path_prefix.find_last_of('/');
    std::string last_part = (last_slash == std::string::npos) 
        ? path_prefix : path_prefix.substr(last_slash + 1);
    if (last_part.find("vec") == 0 || 
        last_part.find("set") == 0 || 
        last_part.find("list") == 0 ||
        (!last_part.empty() && std::isupper(last_part[0])))
    {
        path_prefix += '/';
    }

    auto& fs = FileSystem::GetFileSystem(context);
    bool index_exists = !bind_data->meta_path.empty() && fs.FileExists(bind_data->meta_path);

    if (!index_exists)
    {
        RootDebug("FILE.BEFORE_OPEN", "mode=path_bind path=" + bind_data->root_path);
        auto file = OpenRepresentativeFile(*bind_data);
        RootDebug("FILE.AFTER_OPEN",
                  "mode=path_bind file_ptr=" + RootPointer(file.get()) +
                  " zombie=" + std::to_string(file && file->IsZombie() ? 1 : 0));
        if (!file || file->IsZombie())
        {
            throw IOException("Failed to open ROOT file: " + bind_data->root_path);
        }
        TTree* tree = rootlake::FindTree(file.get(), "", "");
        if (!tree)
        {
            throw IOException("No TTree found in ROOT file.");
        }

        RootDebug("BIND.BEFORE_SEMANTIC", "path=" + path_prefix_raw);
        if (BindSemanticPathWithoutMetadata(*bind_data, file.get(), path_prefix_raw, path_prefix,
                                            return_names, return_types))
        {
            AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
            RootDebug("BIND.AFTER_SEMANTIC", "result=success path=" + path_prefix_raw);
            RootDebug("FILE.BEFORE_CLOSE", "mode=semantic_success file_ptr=" + RootPointer(file.get()));
            file.reset();
            RootDebug("FILE.AFTER_CLOSE", "mode=semantic_success");
            RootDebug("BIND.RETURN", "mode=semantic path=" + path_prefix_raw);
            return std::move(bind_data);
        }
        RootDebug("BIND.AFTER_SEMANTIC", "result=not_bound path=" + path_prefix_raw);

        std::vector<SimpleBranchInfo> simple_branches;
        auto* branches = tree->GetListOfBranches();
        for (int i = 0; i < branches->GetEntries(); ++i)
        {
            auto* br = dynamic_cast<TBranch*>(branches->At(i));
            if (!br) continue;
            if (dynamic_cast<TBranchElement*>(br)) continue;
            TLeaf* leaf = br->GetLeaf(br->GetName());
            if (!leaf) continue;
            SimpleBranchInfo info;
            info.name = br->GetName();
            info.type_name = leaf->GetTypeName();
            info.branch = br;
            info.leaf = leaf;
            simple_branches.push_back(std::move(info));
        }

        std::string target_name = path_prefix_raw;
        if (!target_name.empty() && target_name[0] == '/') target_name = target_name.substr(1);

        bool is_tree_name = false;
        TIter next(file->GetListOfKeys());
        TKey* key;
        while ((key = dynamic_cast<TKey*>(next())))
        {
            if (std::string(key->GetClassName()) == "TTree" && std::string(key->GetName()) == target_name)
            {
                is_tree_name = true;
                break;
            }
        }
        if (is_tree_name)
        {
            bind_data->is_browse_mode = true;
            for (const auto& info : simple_branches)
            {
                bind_data->browse_children.push_back("/" + info.name);
            }
            return_names.emplace_back("path");
            return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
            RootDebug("FILE.BEFORE_CLOSE", "mode=tree_browse file_ptr=" + RootPointer(file.get()));
            file.reset();
            RootDebug("FILE.AFTER_CLOSE", "mode=tree_browse");
            RootDebug("BIND.RETURN", "mode=tree_browse");
            return std::move(bind_data);
        }

        for (const auto& info : simple_branches)
        {
            if (info.name == target_name)
            {
                bind_data->is_direct_branch_mode = true;
                bind_data->direct_branch_info = info;
                bind_data->tree_name = tree->GetName();
                bind_data->total_rows = tree->GetEntries();

                AddEventIdColumn(*bind_data, return_names, return_types);
                RootLakeColumnInfo col;
                col.name = info.name;
                col.type = MetaColumnType::UNKNOWN;
                col.branch_name = info.name;
                col.root_type = info.type_name;
                col.is_string = rootlake::IsStringType(info.type_name);
                bind_data->columns.push_back(std::move(col));
                return_names.emplace_back(info.name);
                return_types.emplace_back(RootTypeToDuckDB(info.type_name, false, true));
                AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
                RootDebug("FILE.BEFORE_CLOSE", "mode=direct_branch file_ptr=" + RootPointer(file.get()));
                file.reset();
                RootDebug("FILE.AFTER_CLOSE", "mode=direct_branch");
                RootDebug("BIND.RETURN", "mode=direct_branch name=" + info.name);
                return std::move(bind_data);
            }
        }

        BindEmptyResult(*bind_data, path_prefix_raw, return_names, return_types);
        AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
        RootDebug("FILE.BEFORE_CLOSE", "mode=empty file_ptr=" + RootPointer(file.get()));
        file.reset();
        RootDebug("FILE.AFTER_CLOSE", "mode=empty");
        RootDebug("BIND.RETURN", "mode=empty path=" + path_prefix_raw);
        return std::move(bind_data);
    }

    auto meta = MetadataLoader::Load(bind_data->meta_path);
    bind_data->tree_name = std::move(meta.tree_name);
    bind_data->total_rows = meta.total_rows;
    bind_data->baskets = std::move(meta.baskets);

    // Indexed and non-indexed reads use the same direct TStreamerInfo path
    // resolver.  Basket metadata affects scheduling only; it never changes how
    // a semantic value is bound or materialized.
    {
        rootlake::RootFileHandle inspector;
        inspector.Open(bind_data->root_path, bind_data->tree_name, nullptr);
        if (inspector.IsValid() &&
            BindSemanticPathWithoutMetadata(
                *bind_data, inspector.GetTFile(), path_prefix_raw, path_prefix,
                return_names, return_types))
        {
            AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
            return std::move(bind_data);
        }
    }

    BindEmptyResult(*bind_data, path_prefix_raw, return_names, return_types);
    AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
    return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> RootScanInit(ClientContext& context, TableFunctionInitInput& input)
{
    auto& bind_data = input.bind_data->Cast<FastRootBindData>();
    auto global_state = make_uniq<FastRootGlobalState>();

    global_state->browse_offset = 0;
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
    if (bind_data.IsMultiFile() && !bind_data.is_browse_mode && !bind_data.is_empty_mode) {
        const auto runtime = rootlake::RootRuntimeSettings::From(context, bind_data.root_paths.size());
        global_state->file_scheduler = make_uniq<rootlake::RootDirectFileScheduler>(
            bind_data.root_paths, runtime.threads);
    }
    if (!bind_data.is_browse_mode && global_state->filters) {
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

static void InitializeRootLocalFile(const FastRootBindData& bind_data,
                                    FastRootGlobalState& gstate,
                                    FastRootLocalState& target,
                                    const std::string& file_path,
                                    bool synchronize_open)
{
    RootDebugOperationScope debug_operation("RootScanInitLocal");
    RootDebug("INIT_LOCAL.BEGIN",
              "root_path=" + file_path +
              " tree=" + bind_data.tree_name +
              " columns=" + std::to_string(bind_data.columns.size()));

    auto* local_state = &target;
    auto* open_mutex = synchronize_open ? &gstate.coordination_mutex : nullptr;

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
        auto* tree = rootlake::FindTree(file, "", root_class_name);
        if (!tree)
        {
            if (gstate.file_scheduler) {
                throw IOException("ROOT schema mismatch in " + file_path +
                                  ": tree for class '" + root_class_name + "' is absent");
            }
            continue;
        }

        auto* branch = rootlake::FindObjectBranch(tree, root_class_name);
        if (!branch)
        {
            if (gstate.file_scheduler) {
                throw IOException("ROOT schema mismatch in " + file_path +
                                  ": object branch for class '" + root_class_name + "' is absent");
            }
            continue;
        }

        auto* rc = TClass::GetClass(root_class_name.c_str());
        if (!rc || !rc->HasDictionary())
        {
            if (gstate.file_scheduler) {
                throw IOException("ROOT dictionary is unavailable for class '" + root_class_name + "'");
            }
            continue;
        }

        rootlake::RootObjectContext ctx;
        ctx.Bind(tree, branch, rc, bind_data.dictionary_cleanup_mode,
                 root_class_name);
        local_state->root_contexts.emplace(root_class_name, std::move(ctx));
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
        auto context_it = local_state->root_contexts.find(col.branch_name);
        if (context_it != local_state->root_contexts.end())
        {
            auto& object_context = context_it->second;
            const auto parsed = rootlake::ParsePath(col.logical_path);
            const auto physical = rootlake::ResolvePhysicalBranch(object_context.branch, parsed.fields);
            local_state->serialized_plan = rootlake::BuildSerializedReadPlan(
                object_context.root_class, parsed, physical.branch);
            const auto projection = physical.mode == "ancestor"
                ? rootlake::ApplyBranchProjection(
                    object_context.tree, {physical.branch}, bind_data.tree_cache_bytes)
                : rootlake::BranchProjectionResult {};
            if (!projection.applied) {
                rootlake::EnableAllBranches(object_context.tree, bind_data.tree_cache_bytes);
            }
            if (bind_data.reader_mode != rootlake::RootReaderMode::OBJECT &&
                local_state->serialized_plan.supported)
            {
                local_state->serialized_reader.Bind(
                    physical.branch, local_state->serialized_plan,
                    bind_data.raw_max_entry_bytes, bind_data.raw_max_values_per_entry,
                    object_context.CurrentObject());
                local_state->serialized_active = true;
                local_state->serialized_column = col_idx;
                local_state->serialized_context = col.branch_name;
                local_state->validation_remaining = bind_data.raw_validation_entries;
            }
            else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED)
            {
                throw InvalidInputException("reader_mode='serialized' cannot read " + col.logical_path +
                                            ": " + local_state->serialized_plan.reason);
            }
            else if (bind_data.reader_mode == rootlake::RootReaderMode::AUTO)
            {
                rootlake::WarnRootFallbackOnce(col.logical_path,
                    local_state->serialized_plan.schema_fingerprint,
                    local_state->serialized_plan.reason);
            }
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
        if (!local_state->root_contexts.empty()) {
            entries = static_cast<uint64_t>(std::max<Long64_t>(
                0, local_state->root_contexts.begin()->second.tree->GetEntries()));
        }
        local_state->local_current_row = gstate.event_range_impossible
            ? entries : std::min(entries, gstate.event_lower);
        local_state->local_end_row = entries;
        if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
            local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
        }
        local_state->file_active = true;
        const auto fingerprint = local_state->serialized_plan.schema_fingerprint.empty()
            ? std::string("object:") + bind_data.tree_name
            : local_state->serialized_plan.schema_fingerprint;
        gstate.file_scheduler->ObserveSchema(fingerprint);
    }
}

unique_ptr<LocalTableFunctionState> RootScanInitLocal(ExecutionContext& context,
                                                      TableFunctionInitInput& input,
                                                      GlobalTableFunctionState* global_state_p)
{
    auto& bind_data = input.bind_data->Cast<FastRootBindData>();
    auto& gstate = global_state_p->Cast<FastRootGlobalState>();
    auto local_state = make_uniq<FastRootLocalState>();
    if (!bind_data.is_empty_mode && !bind_data.is_browse_mode && !bind_data.IsMultiFile()) {
        InitializeRootLocalFile(bind_data, gstate, *local_state, bind_data.root_path, true);
    }
    return std::move(local_state);
}

static void ResetRootLocalFile(FastRootGlobalState& gstate,
                               FastRootLocalState& local_state,
                               bool completed)
{
    if (completed && local_state.file_active && gstate.file_scheduler) {
        const auto elapsed = std::chrono::steady_clock::now() - local_state.file_started;
        gstate.file_scheduler->RecordComplete(
            local_state.file_task,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    }
    local_state.serialized_reader.Bind(nullptr, rootlake::SerializedReadPlan {});
    local_state.serialized_active = false;
    local_state.serialized_column = DConstants::INVALID_INDEX;
    local_state.serialized_context.clear();
    local_state.serialized_plan = {};
    local_state.validation_remaining = 0;
    local_state.serialized_values.clear();
    local_state.serialized_indices.clear();
    local_state.reported_serialized_baskets = 0;
    local_state.reported_serialized_compressed_bytes = 0;
    local_state.reported_serialized_entry_bytes = 0;
    local_state.cached_results.clear();
    local_state.has_cached_entry = false;
    local_state.current_entry = std::numeric_limits<uint64_t>::max();
    local_state.current_elem_idx = 0;
    local_state.root_contexts.clear();
    local_state.direct_branch = nullptr;
    local_state.direct_leaf = nullptr;
    local_state.root_file.Close();
    local_state.local_current_row = 0;
    local_state.local_end_row = 0;
    local_state.has_container_columns = false;
    local_state.file_active = false;
}

static bool EnsureMultiFileReady(const FastRootBindData& bind_data,
                                 FastRootGlobalState& gstate,
                                 FastRootLocalState& local_state)
{
    if (!gstate.file_scheduler) throw InternalException("multi-file ROOT scheduler is unavailable");
    while (true) {
        if (local_state.file_active &&
            (local_state.has_cached_entry || local_state.local_current_row < local_state.local_end_row)) {
            return true;
        }
        if (local_state.file_active) ResetRootLocalFile(gstate, local_state, true);

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
            InitializeRootLocalFile(bind_data, gstate, local_state,
                                    local_state.file_task.path, false);
        } catch (const rootlake::RootFileUnavailableException &exception) {
            gstate.file_scheduler->RecordUnavailable(
                local_state.file_task, exception.attempts, exception.elapsed_us);
            ResetRootLocalFile(gstate, local_state, false);
            continue;
        } catch (const std::exception &exception) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, exception.what());
            ResetRootLocalFile(gstate, local_state, false);
            continue;
        } catch (...) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, "unknown ROOT reader error");
            ResetRootLocalFile(gstate, local_state, false);
            continue;
        }
    }
}

static void ProcessBrowseMode(
    ClientContext& context,
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
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

static bool WriteNumericValue(Vector& vector, idx_t row, double value)
{
    switch (vector.GetType().id())
    {
        case LogicalTypeId::TINYINT:
            FlatVector::GetData<int8_t>(vector)[row] = static_cast<int8_t>(value); break;
        case LogicalTypeId::UTINYINT:
            FlatVector::GetData<uint8_t>(vector)[row] = static_cast<uint8_t>(value); break;
        case LogicalTypeId::SMALLINT:
            FlatVector::GetData<int16_t>(vector)[row] = static_cast<int16_t>(value); break;
        case LogicalTypeId::USMALLINT:
            FlatVector::GetData<uint16_t>(vector)[row] = static_cast<uint16_t>(value); break;
        case LogicalTypeId::INTEGER:
            FlatVector::GetData<int32_t>(vector)[row] = static_cast<int32_t>(value); break;
        case LogicalTypeId::UINTEGER:
            FlatVector::GetData<uint32_t>(vector)[row] = static_cast<uint32_t>(value); break;
        case LogicalTypeId::BIGINT:
            FlatVector::GetData<int64_t>(vector)[row] = static_cast<int64_t>(value); break;
        case LogicalTypeId::UBIGINT:
            FlatVector::GetData<uint64_t>(vector)[row] = static_cast<uint64_t>(value); break;
        case LogicalTypeId::FLOAT:
            FlatVector::GetData<float>(vector)[row] = static_cast<float>(value); break;
        case LogicalTypeId::DOUBLE:
            FlatVector::GetData<double>(vector)[row] = value; break;
        case LogicalTypeId::BOOLEAN:
            FlatVector::GetData<bool>(vector)[row] = value != 0.0; break;
        default:
            FlatVector::Validity(vector).SetInvalid(row);
            return false;
    }
    FlatVector::Validity(vector).SetValid(row);
    return true;
}

static std::optional<int32_t> ResolveCachedIndexValue(const FastRootBindData& bind_data,
                                                       const FastRootLocalState& lstate,
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

static rootlake::RootScalarActual CachedScalar(const FastRootBindData& bind_data,
                                                const FastRootLocalState& lstate,
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
    const auto logical_type = RootTypeToDuckDB(column.root_type, column.is_string, true);
    if (col_idx >= lstate.cached_results.size()) return rootlake::RootScalarActual::Null(logical_type);
    const auto& result = lstate.cached_results[col_idx];
    if (elem_idx >= result.size()) return rootlake::RootScalarActual::Null(logical_type);
    if (result.is_string_flag[elem_idx]) return rootlake::RootScalarActual::String(result.strings[elem_idx]);
    return rootlake::RootScalarActual::Numeric(logical_type, result.numbers[elem_idx]);
}

static bool PassesCachedFilters(ClientContext& context, const FastRootBindData& bind_data,
                                const FastRootGlobalState& gstate, FastRootLocalState& lstate,
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

static bool PassesDirectBranchFilters(ClientContext& context, const FastRootBindData& bind_data,
                                      const FastRootGlobalState& gstate, FastRootLocalState& lstate,
                                      uint64_t entry, double value)
{
    if (!gstate.filters) return true;
    const auto value_type = RootTypeToDuckDB(bind_data.direct_branch_info.type_name, false, true);
    for (const auto& filter : gstate.filters->filters)
    {
        if (filter.first >= gstate.scan_column_ids.size()) continue;
        const auto column = gstate.scan_column_ids[filter.first];
        auto actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID) {
            actual = rootlake::RootScalarActual::Signed(entry);
        } else if (column == 1) {
            actual = rootlake::RootScalarActual::Numeric(value_type, value);
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

static void ProcessDirectBranch(
    ClientContext& context,
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
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
            WorkScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
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

        const double val = lstate.direct_leaf->GetValue();
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

static void ProcessCachedEntry(
    ClientContext& context,
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
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
            {                // Index-only projections use a cached materialized leaf from the same object branch.
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
                const double val = res.numbers[elem_idx];
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

enum class CacheResult
{
    CACHED,
    CONTINUE_LOOP,
    BREAK_LOOP
};

static std::vector<std::string> SplitIndexSignature(const std::string& signature)
{
    std::vector<std::string> names;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

static void MaterializeSerializedResult(const RootLakeColumnInfo& column, uint64_t entry,
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

static void ReadResultAsSerializedVectors(const rootlake::ReadResult& result,
                                          std::vector<double>& values,
                                          std::vector<int32_t>& flat_indices)
{
    values.clear();
    flat_indices.clear();
    values.reserve(result.numbers.size());
    for (idx_t i = 0; i < result.numbers.size(); ++i) {
        if (result.is_string_flag[i]) continue;
        values.push_back(result.numbers[i]);
        for (const auto index : result.vector_indices[i]) {
            flat_indices.push_back(static_cast<int32_t>(index));
        }
    }
}

static CacheResult ReadAndCacheEntry(
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
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

    if (lstate.serialized_active && lstate.serialized_column < bind_data.columns.size())
    {
        std::string failure_reason;
        const bool decoded = lstate.serialized_reader.Decode(
            entry, lstate.serialized_values, lstate.serialized_indices, failure_reason);
        const auto& serialized_counters = lstate.serialized_reader.Counters();
        gstate.serialized_baskets.fetch_add(
            serialized_counters.baskets - lstate.reported_serialized_baskets);
        gstate.serialized_compressed_bytes.fetch_add(
            serialized_counters.compressed_bytes - lstate.reported_serialized_compressed_bytes);
        gstate.serialized_entry_bytes.fetch_add(
            serialized_counters.serialized_bytes - lstate.reported_serialized_entry_bytes);
        lstate.reported_serialized_baskets = serialized_counters.baskets;
        lstate.reported_serialized_compressed_bytes = serialized_counters.compressed_bytes;
        lstate.reported_serialized_entry_bytes = serialized_counters.serialized_bytes;
        if (decoded)
        {
            gstate.serialized_entries.fetch_add(1);
            gstate.serialized_values.fetch_add(lstate.serialized_values.size());
            if (lstate.validation_remaining > 0)
            {
                auto context_it = lstate.root_contexts.find(lstate.serialized_context);
                rootlake::ReadResult reference;
                bool reference_ok = false;
                if (context_it != lstate.root_contexts.end())
                {
                    auto& ctx = context_it->second;
                    const auto bytes = ctx.tree->GetEntry(static_cast<Long64_t>(entry));
                    gstate.object_validation_entries.fetch_add(1);
                    if (bytes >= 0 && ctx.CurrentObject())
                    {
                        const auto& column = bind_data.columns[lstate.serialized_column];
                        rootlake::OffsetValueReader::CollectDirect(
                            ctx.CurrentObject(), column.levels,
                            std::numeric_limits<Long64_t>::max(),
                            static_cast<Long64_t>(entry), reference);
                        std::vector<double> reference_values;
                        std::vector<int32_t> reference_indices;
                        ReadResultAsSerializedVectors(reference, reference_values, reference_indices);
                        reference_ok = rootlake::EqualDecodedValues(
                            lstate.serialized_values, lstate.serialized_indices,
                            reference_values, reference_indices);
                    }
                }
                if (!reference_ok) failure_reason = "serialized values differ from universal ROOT reader";
                else --lstate.validation_remaining;
            }
        }

        if (!decoded || !failure_reason.empty())
        {
            if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED) {
                throw IOException("reader_mode='serialized' failed for " +
                                  lstate.serialized_plan.logical_path + ": " + failure_reason);
            }
            rootlake::WarnRootFallbackOnce(lstate.serialized_plan.logical_path,
                                           lstate.serialized_plan.schema_fingerprint,
                                           failure_reason);
            auto context_it = lstate.root_contexts.find(lstate.serialized_context);
            if (context_it != lstate.root_contexts.end()) {
                rootlake::EnableAllBranches(context_it->second.tree, bind_data.tree_cache_bytes);
            }
            lstate.serialized_active = false;
        }
        else
        {
            const auto& column = bind_data.columns[lstate.serialized_column];
            MaterializeSerializedResult(column, entry, lstate.serialized_plan,
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
            auto it = lstate.root_contexts.find(sample_branch);
            if (it != lstate.root_contexts.end())
            {
                auto& ctx = it->second;
                if (entry < static_cast<uint64_t>(ctx.tree->GetEntries()))
                {
                    RootDebug("READ.BEFORE_GET_ENTRY",
                              "mode=sample class=" + sample_branch +
                              " entry=" + std::to_string(entry) +
                              " tree_ptr=" + RootPointer(ctx.tree));
                    Long64_t bytes = ctx.tree->GetEntry(entry);
                    gstate.object_fallback_entries.fetch_add(1);
                    RootDebug("READ.AFTER_GET_ENTRY",
                              "mode=sample class=" + sample_branch +
                              " entry=" + std::to_string(entry) +
                              " bytes=" + std::to_string(bytes) +
                              " object_ptr=" + RootPointer(ctx.CurrentObject()));
                    if (bytes >= 0 && ctx.CurrentObject())
                    {
                        const auto& col = bind_data.columns[sample_col_idx];
                        lstate.cached_results[sample_col_idx].Clear();
                        rootlake::OffsetValueReader::CollectDirect(
                            ctx.CurrentObject(), col.levels,
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
            auto it = lstate.root_contexts.find(branch_name);
            if (it == lstate.root_contexts.end())
            {
                continue;
            }
            auto& ctx = it->second;
            if (entry >= static_cast<uint64_t>(ctx.tree->GetEntries()))
            {
                continue;
            }

            RootDebug("READ.BEFORE_GET_ENTRY",
                      "mode=group class=" + branch_name +
                      " entry=" + std::to_string(entry) +
                      " tree_ptr=" + RootPointer(ctx.tree));
            Long64_t bytes = ctx.tree->GetEntry(entry);
            gstate.object_fallback_entries.fetch_add(1);
            RootDebug("READ.AFTER_GET_ENTRY",
                      "mode=group class=" + branch_name +
                      " entry=" + std::to_string(entry) +
                      " bytes=" + std::to_string(bytes) +
                      " object_ptr=" + RootPointer(ctx.CurrentObject()));
            if (bytes < 0)
            {
                continue;
            }

            for (idx_t col_idx : col_indices)
            {
                const auto& col = bind_data.columns[col_idx];
                if (ctx.CurrentObject())
                {
                    lstate.cached_results[col_idx].Clear();
                    rootlake::OffsetValueReader::CollectDirect(
                        ctx.CurrentObject(), col.levels,
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

void RootScanFunction(ClientContext& context, TableFunctionInput& data_p, DataChunk& output)
{
    auto& bind_data = data_p.bind_data->Cast<FastRootBindData>();
    auto& gstate = data_p.global_state->Cast<FastRootGlobalState>();
    auto& lstate = data_p.local_state->Cast<FastRootLocalState>();

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

    if (bind_data.is_browse_mode)
    {
        ProcessBrowseMode(context, bind_data, gstate, lstate, output);
        return;
    }

    if (bind_data.is_direct_branch_mode)
    {
        while (true) {
            if (gstate.file_scheduler && !EnsureMultiFileReady(bind_data, gstate, lstate)) {
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
        if (gstate.file_scheduler && !EnsureMultiFileReady(bind_data, gstate, lstate)) {
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
                    WorkScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
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

static InsertionOrderPreservingMap<string> RootScanToString(TableFunctionToStringInput& input)
{
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data) return result;
    const auto& bind = input.bind_data->Cast<FastRootBindData>();
    result["ROOT Input"] = bind.input_specification;
    result["ROOT Files"] = std::to_string(bind.root_paths.size());
    result["ROOT Representative"] = bind.root_path;
    result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.raw_validation_entries);
    return result;
}

static InsertionOrderPreservingMap<string> RootScanDynamicToString(TableFunctionDynamicToStringInput& input)
{
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data) {
        const auto& bind = input.bind_data->Cast<FastRootBindData>();
        result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.reader_mode);
        result["ROOT Input Files"] = std::to_string(bind.root_paths.size());
        result["ROOT Representative"] = bind.root_path;
        result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    }
    if (!input.global_state) return result;
    const auto& global = input.global_state->Cast<FastRootGlobalState>();
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
