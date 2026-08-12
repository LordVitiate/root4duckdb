#include "root_runtime_settings.hpp"
#include <cstdlib>
#include <sys/stat.h>
#include "root_iceberg_catalog.hpp"
#include "include/root_bloom.hpp"
#include "include/root_branch_projection.hpp"
#include "include/root_dataset_catalog.hpp"
#include "include/root_dictionary.hpp"
#include "include/root_filter.hpp"
#include "include/root_lake_common.hpp"
#include "include/root_path_reader.hpp"
#include "include/root_headers.hpp"
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

enum class PathPredicateOp {
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    BETWEEN,
    IN
};

struct PathPredicateBinding {
    std::string path;
    PathPredicateOp op = PathPredicateOp::EQ;    std::vector<RootPrimitiveValue> values;
    bool require_all_values = false;
    LogicalType value_type;
    std::vector<SchemaBinding> schemas;
    std::unordered_map<std::string, idx_t> schema_lookup;
};

struct EntryInterval {
    uint64_t begin = 0;
    uint64_t end = 0;
};

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

struct RootFileTaskGroup {
    idx_t begin = 0;
    idx_t end = 0;
};

struct DatasetBindData final : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override {
        auto result = make_uniq<DatasetBindData>(*this);
        return std::move(result);
    }
    bool Equals(const FunctionData &) const override {
        return false;
    }
    bool SupportStatementCache() const override {
        return false;
    }

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
    uint32_t raw_validation_entries = 4;
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

struct DatasetGlobalState final : public GlobalTableFunctionState {
    std::vector<RootBasketTask> tasks;
    std::vector<RootFileTaskGroup> task_groups;
    std::atomic<idx_t> next_group {0};
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
    std::atomic<idx_t> completed_tasks {0};
    std::atomic<uint64_t> opened_files {0};
    std::atomic<uint64_t> reused_open_files {0};
    std::atomic<uint64_t> get_entry_calls {0};
    std::atomic<uint64_t> serialized_entry_calls {0};
    std::atomic<uint64_t> serialized_values {0};
    std::atomic<uint64_t> serialized_baskets {0};
    std::atomic<uint64_t> serialized_compressed_bytes {0};
    std::atomic<uint64_t> serialized_entry_bytes {0};
    std::atomic<uint64_t> projected_files {0};
    std::atomic<uint64_t> fallback_files {0};
    std::atomic<uint64_t> decoded_values {0};
    std::atomic<uint64_t> emitted_rows {0};
    uint64_t row_limit = std::numeric_limits<uint64_t>::max();
    std::atomic<bool> stop_requested {false};
    bool has_event_range = false;
    bool event_range_impossible = false;
    uint64_t event_lower = 0;
    uint64_t event_upper = std::numeric_limits<uint64_t>::max();
    std::atomic<uint64_t> skipped_entries {0};
    bool metadata_count_only = false;
    std::atomic<uint64_t> metadata_rows_emitted {0};
    std::unordered_map<std::string, std::vector<EntryInterval>> candidate_intervals;
    uint64_t predicate_index_baskets = 0;
    uint64_t predicate_intersections = 0;
    uint64_t bloom_metadata_bytes = 0;
    uint64_t planning_time_us = 0;
    uint64_t metadata_total_rows = 0;

    idx_t MaxThreads() const override {
        if (metadata_count_only) return 1;
        return std::max<idx_t>(1, std::min<idx_t>(worker_limit, std::min<idx_t>(task_groups.size(),
            static_cast<idx_t>(GlobalTableFunctionState::MAX_THREADS))));
    }
};

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

class DatasetTaskPlanner final {
public:
    void Plan(ClientContext &context, const DatasetBindData &bind,
              DatasetGlobalState &global,
              optional_ptr<TableFilterSet> filters);
    void PlanMetadataCount(ClientContext &context,
                           const DatasetBindData &bind,
                           DatasetGlobalState &global);

private:
    void BuildFileTaskGroups(DatasetGlobalState &global);
};

class DatasetScanBinder final {
public:
    unique_ptr<FunctionData> Bind(
        ClientContext &context, TableFunctionBindInput &input,
        vector<LogicalType> &return_types, vector<string> &return_names);
};

class DatasetScanStateFactory final {
public:
    unique_ptr<NodeStatistics> Cardinality(const FunctionData *bind_data);
    unique_ptr<GlobalTableFunctionState> CreateGlobal(
        ClientContext &context, TableFunctionInitInput &input);
    unique_ptr<LocalTableFunctionState> CreateLocal();
};

class DatasetScanExecutor final {
public:
    void Scan(ClientContext &context, TableFunctionInput &input,
              DataChunk &output);
    double Progress(const GlobalTableFunctionState *state) const;

private:
    void ValidateAccessPlan(const SchemaBinding &schema,
                            const std::vector<PathLevel> &actual) const;
    void SyncSerializedCounters(DatasetLocalState &local,
                                DatasetGlobalState &global) const;
    void OpenTaskFile(const DatasetBindData &bind,
                      DatasetGlobalState &global,
                      DatasetLocalState &local,
                      const RootBasketTask &task);
    void PrefetchPhysicalRange(TFile *file, uint64_t offset,
                               uint64_t size) const;
    bool ClaimTask(const DatasetBindData &bind, DatasetGlobalState &global,
                   DatasetLocalState &local);
    bool PassesPathPredicates(const DatasetBindData &bind,
                              DatasetLocalState &local,
                              void *object) const;
    bool LoadNextEntry(const DatasetBindData &bind, DatasetLocalState &local,
                       DatasetGlobalState &global);
    void SetPrimitiveAsType(
        Vector &vector, idx_t row,
        const LogicalType &type,
        const RootPrimitiveValue &value) const;
    void SetDoubleAsType(Vector &vector, idx_t row,
                         const LogicalType &type, double value) const;
    void EmitProjectedTypedRow(
        const DatasetBindData &bind,
        const DatasetGlobalState &global,
        DataChunk &output, idx_t output_row,
        uint64_t event_fk,
        const std::string &source_id,
        uint64_t entry_id,
        const RootPrimitiveValue &value,
        const int32_t *indices,
        idx_t index_count) const;
    void EmitProjectedRow(const DatasetBindData &bind,
                          const DatasetGlobalState &global,
                          DataChunk &output, idx_t output_row,
                          uint64_t event_fk,
                          const std::string &source_id,
                          uint64_t entry_id, double value,
                          const int32_t *indices,
                          idx_t index_count) const;
    idx_t ReserveOutputRows(DatasetGlobalState &global,
                            idx_t requested) const;
};

class DatasetScanExplain final {
public:
    InsertionOrderPreservingMap<string> Bound(
        TableFunctionToStringInput &input) const;
    InsertionOrderPreservingMap<string> Running(
        TableFunctionDynamicToStringInput &input) const;
};


static bool RequiresTypedDatasetValue(
    const LogicalType &type) {
    return type.id() == LogicalTypeId::BIGINT ||
           type.id() == LogicalTypeId::UBIGINT;
}

static bool UsesDoubleBackedValueMetadata(
    const LogicalType &type) {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        return true;
    default:
        return false;
    }
}

static PathPredicateOp ParsePathPredicateOp(std::string op) {
    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (op == "=" || op == "==" || op == "eq") return PathPredicateOp::EQ;
    if (op == "!=" || op == "<>" || op == "ne") return PathPredicateOp::NE;
    if (op == "<" || op == "lt") return PathPredicateOp::LT;
    if (op == "<=" || op == "le") return PathPredicateOp::LE;
    if (op == ">" || op == "gt") return PathPredicateOp::GT;
    if (op == ">=" || op == "ge") return PathPredicateOp::GE;
    if (op == "between") return PathPredicateOp::BETWEEN;
    if (op == "in") return PathPredicateOp::IN;
    throw InvalidInputException("Unsupported path predicate operator: " + op);
}


static RootPrimitiveValue PathPredicateJsonValue(
    const nlohmann::json &value,
    const LogicalType &type) {

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        if (value.is_boolean()) {
            return RootPrimitiveValue::Unsigned(
                value.get<bool>() ? 1 : 0);
        }
        if (value.is_number_integer()) {
            const auto number = value.get<int64_t>();
            if (number == 0 || number == 1) {
                return RootPrimitiveValue::Unsigned(
                    static_cast<uint64_t>(number));
            }
        }
        throw InvalidInputException(
            "Boolean path predicate requires true/false or 0/1");

    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
        if (value.is_number_unsigned()) {
            const auto number = value.get<uint64_t>();

            if (number >
                static_cast<uint64_t>(
                    std::numeric_limits<int64_t>::max())) {
                throw InvalidInputException(
                    "Signed path predicate constant is too large");
            }

            return RootPrimitiveValue::Signed(
                static_cast<int64_t>(number));
        }

        if (!value.is_number_integer()) {
            throw InvalidInputException(
                "Signed integer path predicate requires "
                "an integer constant");
        }

        return RootPrimitiveValue::Signed(
            value.get<int64_t>());

    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
        if (value.is_number_unsigned()) {
            return RootPrimitiveValue::Unsigned(
                value.get<uint64_t>());
        }

        if (value.is_number_integer()) {
            const auto number = value.get<int64_t>();

            if (number < 0) {
                throw InvalidInputException(
                    "Unsigned path predicate cannot use "
                    "a negative constant");
            }

            return RootPrimitiveValue::Unsigned(
                static_cast<uint64_t>(number));
        }

        throw InvalidInputException(
            "Unsigned integer path predicate requires "
            "an integer constant");

    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        if (!value.is_number()) {
            throw InvalidInputException(
                "Floating path predicate requires "
                "a numeric constant");
        }

        return RootPrimitiveValue::Floating(
            value.get<double>());

    default:
        throw NotImplementedException(
            "Unsupported path predicate type " +
            type.ToString());
    }
}

static int ComparePathPredicateValues(
    const RootPrimitiveValue &left,
    const RootPrimitiveValue &right,
    const LogicalType &type) {

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT: {
        const auto a = left.AsUnsigned();
        const auto b = right.AsUnsigned();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT: {
        const auto a = left.AsSigned();
        const auto b = right.AsSigned();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE: {
        const auto a = left.AsDouble();
        const auto b = right.AsDouble();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    default:
        throw NotImplementedException(
            "Unsupported path predicate type " +
            type.ToString());
    }
}

static void ParsePathPredicates(
    RootDatasetCatalog &catalog,
    DatasetBindData &bind,
    const std::string &raw_json) {

    if (raw_json.empty()) {
        return;
    }

    auto json = nlohmann::json::parse(raw_json);

    if (!json.is_array()) {
        throw InvalidInputException(
            "path_predicates must be a JSON array");
    }

    for (const auto &item : json) {
        if (!item.is_object() ||
            !item.contains("path") ||
            !item.contains("op")) {
            throw InvalidInputException(
                "Each path predicate needs path and op");
        }

        PathPredicateBinding predicate;

        predicate.path =
            NormalizePath(
                item.at("path").get<std::string>());

        predicate.op =
            ParsePathPredicateOp(
                item.at("op").get<std::string>());

        const auto quantifier =
            item.value(
                "quantifier",
                std::string("any"));

        predicate.require_all_values =
            quantifier == "all";

        if (quantifier != "any" &&
            quantifier != "all") {
            throw InvalidInputException(
                "path predicate quantifier "
                "must be any or all");
        }

        // Resolve physical type BEFORE parsing constants.
        predicate.schemas =
            catalog.LoadPathSchemas(
                predicate.path,
                predicate.value_type,
                predicate.schema_lookup);

        auto add_value =
            [&](const nlohmann::json &value) {
                predicate.values.push_back(
                    PathPredicateJsonValue(
                        value,
                        predicate.value_type));
            };

        if (predicate.op == PathPredicateOp::IN) {
            if (!item.contains("values") ||
                !item.at("values").is_array()) {
                throw InvalidInputException(
                    "IN path predicate needs values array");
            }

            for (const auto &value :
                 item.at("values")) {
                add_value(value);
            }

        } else if (
            predicate.op ==
            PathPredicateOp::BETWEEN) {

            if (item.contains("values") &&
                item.at("values").is_array() &&
                item.at("values").size() == 2) {

                add_value(item.at("values")[0]);
                add_value(item.at("values")[1]);

            } else if (
                item.contains("lower") &&
                item.contains("upper")) {

                add_value(item.at("lower"));
                add_value(item.at("upper"));

            } else {
                throw InvalidInputException(
                    "BETWEEN path predicate needs "
                    "lower/upper or two values");
            }

            if (ComparePathPredicateValues(
                    predicate.values[0],
                    predicate.values[1],
                    predicate.value_type) > 0) {
                std::swap(
                    predicate.values[0],
                    predicate.values[1]);
            }

        } else {
            if (!item.contains("value")) {
                throw InvalidInputException(
                    "Path predicate needs value");
            }

            add_value(item.at("value"));
        }

        if (predicate.values.empty()) {
            throw InvalidInputException(
                "Path predicate has no constants");
        }

        bind.path_predicates.push_back(
            std::move(predicate));
    }
}

static bool PathPredicateValueMatches(
    const PathPredicateBinding &predicate,
    const RootPrimitiveValue &value) {

    const auto compare =
        [&](const RootPrimitiveValue &constant) {
            return ComparePathPredicateValues(
                value,
                constant,
                predicate.value_type);
        };

    switch (predicate.op) {
    case PathPredicateOp::EQ:
        return compare(predicate.values[0]) == 0;

    case PathPredicateOp::NE:
        return compare(predicate.values[0]) != 0;

    case PathPredicateOp::LT:
        return compare(predicate.values[0]) < 0;

    case PathPredicateOp::LE:
        return compare(predicate.values[0]) <= 0;

    case PathPredicateOp::GT:
        return compare(predicate.values[0]) > 0;

    case PathPredicateOp::GE:
        return compare(predicate.values[0]) >= 0;

    case PathPredicateOp::BETWEEN:
        return
            compare(predicate.values[0]) >= 0 &&
            compare(predicate.values[1]) <= 0;

    case PathPredicateOp::IN:
        return std::any_of(
            predicate.values.begin(),
            predicate.values.end(),
            [&](const RootPrimitiveValue &constant) {
                return compare(constant) == 0;
            });
    }

    return false;
}

static bool PathPredicateEventMatches(
    const PathPredicateBinding &predicate,
    const std::vector<RootPrimitiveValue> &values) {

    if (values.empty()) {
        return false;
    }

    if (predicate.require_all_values) {
        return std::all_of(
            values.begin(),
            values.end(),
            [&](const RootPrimitiveValue &value) {
                return PathPredicateValueMatches(
                    predicate, value);
            });
    }

    return std::any_of(
        values.begin(),
        values.end(),
        [&](const RootPrimitiveValue &value) {
            return PathPredicateValueMatches(
                predicate, value);
        });
}

static std::optional<double> FilterConstantAsPhysicalDouble(const Value &value, const LogicalType &physical_type) {
    if (value.IsNull()) return std::nullopt;
    try {
        const auto casted = value.DefaultCastAs(physical_type);
        double result = 0;
        switch (physical_type.id()) {
        case LogicalTypeId::BOOLEAN: result = casted.GetValue<bool>() ? 1.0 : 0.0; break;
        case LogicalTypeId::TINYINT: result = static_cast<double>(casted.GetValue<int8_t>()); break;
        case LogicalTypeId::UTINYINT: result = static_cast<double>(casted.GetValue<uint8_t>()); break;
        case LogicalTypeId::SMALLINT: result = static_cast<double>(casted.GetValue<int16_t>()); break;
        case LogicalTypeId::USMALLINT: result = static_cast<double>(casted.GetValue<uint16_t>()); break;
        case LogicalTypeId::INTEGER: result = static_cast<double>(casted.GetValue<int32_t>()); break;
        case LogicalTypeId::UINTEGER: result = static_cast<double>(casted.GetValue<uint32_t>()); break;
        case LogicalTypeId::FLOAT: result = static_cast<double>(casted.GetValue<float>()); break;
        case LogicalTypeId::DOUBLE: result = casted.GetValue<double>(); break;
        default: return std::nullopt;
        }
        if (!std::isfinite(result)) return std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

static std::string NumberSQL(double value) {
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return ss.str();
}

static std::optional<std::string> FilterConstantSQL(const Value &value, const LogicalType &physical_type) {
    if (value.IsNull()) return std::nullopt;
    try {
        const auto casted = value.DefaultCastAs(physical_type);
        switch (physical_type.id()) {
        case LogicalTypeId::BOOLEAN:
            return casted.GetValue<bool>() ? std::string("1") : std::string("0");
        case LogicalTypeId::TINYINT:
            return std::to_string(static_cast<int64_t>(casted.GetValue<int8_t>()));
        case LogicalTypeId::SMALLINT:
            return std::to_string(static_cast<int64_t>(casted.GetValue<int16_t>()));
        case LogicalTypeId::INTEGER:
            return std::to_string(static_cast<int64_t>(casted.GetValue<int32_t>()));
        case LogicalTypeId::BIGINT:
            return std::to_string(casted.GetValue<int64_t>());
        case LogicalTypeId::UTINYINT:
            return std::to_string(static_cast<uint64_t>(casted.GetValue<uint8_t>()));
        case LogicalTypeId::USMALLINT:
            return std::to_string(static_cast<uint64_t>(casted.GetValue<uint16_t>()));
        case LogicalTypeId::UINTEGER:
            return std::to_string(static_cast<uint64_t>(casted.GetValue<uint32_t>()));
        case LogicalTypeId::UBIGINT:
            return std::to_string(casted.GetValue<uint64_t>());
        case LogicalTypeId::FLOAT: {
            const auto number = static_cast<double>(casted.GetValue<float>());
            if (!std::isfinite(number)) return std::nullopt;
            return NumberSQL(number);
        }
        case LogicalTypeId::DOUBLE: {
            const auto number = casted.GetValue<double>();
            if (!std::isfinite(number)) return std::nullopt;
            return NumberSQL(number);
        }
        default:
            return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
}

static std::string ZonemapClause(const TableFilter &filter, const std::string &min_expr,
                                 const std::string &max_expr, const LogicalType &physical_type) {
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto &constant = filter.Cast<ConstantFilter>();
        auto literal = FilterConstantSQL(constant.constant, physical_type);
        if (!literal) return {};
        switch (constant.comparison_type) {
        case ExpressionType::COMPARE_EQUAL:
            return min_expr + " <= " + *literal + " AND " + max_expr + " >= " + *literal;
        case ExpressionType::COMPARE_LESSTHAN:
            return min_expr + " < " + *literal;
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            return min_expr + " <= " + *literal;
        case ExpressionType::COMPARE_GREATERTHAN:
            return max_expr + " > " + *literal;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            return max_expr + " >= " + *literal;
        default:
            return {};
        }
    }
    case TableFilterType::IN_FILTER: {
        const auto &in_filter = filter.Cast<InFilter>();
        std::vector<std::string> clauses;
        for (const auto &constant : in_filter.values) {
            auto literal = FilterConstantSQL(constant, physical_type);
            if (!literal) return {};
            clauses.push_back("(" + min_expr + " <= " + *literal + " AND " + max_expr + " >= " + *literal + ")");
        }
        std::string out;
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) out += " OR ";
            out += clauses[i];
        }
        return out;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto &optional = filter.Cast<OptionalFilter>();
        return optional.child_filter ? ZonemapClause(*optional.child_filter, min_expr, max_expr, physical_type) : std::string();
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto &dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) return {};
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) return {};
        return ZonemapClause(*dynamic.filter_data->filter, min_expr, max_expr, physical_type);
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto &conjunction = filter.Cast<ConjunctionAndFilter>();
        std::vector<std::string> clauses;
        for (const auto &child : conjunction.child_filters) {
            auto clause = ZonemapClause(*child, min_expr, max_expr, physical_type);
            if (!clause.empty()) clauses.push_back("(" + clause + ")");
        }
        std::string out;
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) out += " AND ";
            out += clauses[i];
        }
        return out;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto &conjunction = filter.Cast<ConjunctionOrFilter>();
        std::vector<std::string> clauses;
        for (const auto &child : conjunction.child_filters) {
            auto clause = ZonemapClause(*child, min_expr, max_expr, physical_type);
            if (clause.empty()) return {};
            clauses.push_back("(" + clause + ")");
        }
        std::string out;
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) out += " OR ";
            out += clauses[i];
        }
        return out;
    }
    default:
        return {};
    }
}

static optional_ptr<TableFilter> FilterForFullColumn(const DatasetGlobalState &global, column_t full_column) {
    if (!global.filters) return nullptr;
    for (const auto &entry : global.filters->filters) {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size()) continue;
        if (global.scan_column_ids[scan_position] == full_column) return entry.second.get();
    }
    return nullptr;
}

static bool PassesFilters(ClientContext &context, DatasetLocalState &local, const DatasetBindData &bind,
                          const DatasetGlobalState &global, uint64_t event_fk, double numeric_value,
                          const int32_t *indices, idx_t index_count) {
    if (!global.filters) return true;
    for (const auto &entry : global.filters->filters) {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size()) continue;
        const column_t column = global.scan_column_ids[scan_position];
        RootScalarActual actual;
        if (column == 0) {
            actual = RootScalarActual::Event(event_fk);
        } else if (column >= 1 && static_cast<idx_t>(column) < bind.value_column) {
            const idx_t index_position = static_cast<idx_t>(column) - 1;
            actual = RootScalarActual::Index(index_position < index_count
                                             ? std::optional<int32_t>(indices[index_position])
                                             : std::nullopt);
        } else if (static_cast<idx_t>(column) == bind.value_column) {
            actual = RootScalarActual::Numeric(bind.value_type, numeric_value);
        } else if (static_cast<idx_t>(column) == bind.source_id_column) {
            actual = RootScalarActual::String(local.value_source_id);
        } else if (static_cast<idx_t>(column) == bind.entry_id_column) {
            actual = RootScalarActual::Event(local.value_entry_id);
        } else {
            actual = RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!local.filter_evaluator.Evaluate(context, *entry.second, actual)) return false;
    }
    return true;
}

static RootScalarActual DatasetPrimitiveActual(
    const LogicalType &type,
    const RootPrimitiveValue &value) {
    switch (value.kind) {
    case RootPrimitiveKind::SIGNED:
        return RootScalarActual::Signed(
            value.signed_value, type);

    case RootPrimitiveKind::UNSIGNED:
        return RootScalarActual::Unsigned(
            value.unsigned_value, type);

    case RootPrimitiveKind::FLOATING:
        return RootScalarActual::Numeric(
            type, value.floating_value);
    }

    return RootScalarActual::Null(type);
}

static bool PassesTypedFilters(
    ClientContext &context,
    DatasetLocalState &local,
    const DatasetBindData &bind,
    const DatasetGlobalState &global,
    uint64_t event_fk,
    const RootPrimitiveValue &numeric_value,
    const int32_t *indices,
    idx_t index_count) {

    if (!global.filters) return true;

    for (const auto &entry : global.filters->filters) {
        const auto scan_position = entry.first;

        if (scan_position >= global.scan_column_ids.size()) {
            continue;
        }

        const column_t column =
            global.scan_column_ids[scan_position];

        RootScalarActual actual;

        if (column == 0) {
            actual = RootScalarActual::Event(event_fk);

        } else if (
            column >= 1 &&
            static_cast<idx_t>(column) < bind.value_column) {

            const idx_t index_position =
                static_cast<idx_t>(column) - 1;

            actual = RootScalarActual::Index(
                index_position < index_count
                    ? std::optional<int32_t>(
                          indices[index_position])
                    : std::nullopt);

        } else if (
            static_cast<idx_t>(column) ==
            bind.value_column) {

            actual = DatasetPrimitiveActual(
                bind.value_type, numeric_value);

        } else if (
            static_cast<idx_t>(column) ==
            bind.source_id_column) {

            actual =
                RootScalarActual::String(
                    local.value_source_id);

        } else if (
            static_cast<idx_t>(column) ==
            bind.entry_id_column) {

            actual =
                RootScalarActual::Event(
                    local.value_entry_id);

        } else {
            actual =
                RootScalarActual::Null(
                    LogicalType::SQLNULL);
        }

        if (!local.filter_evaluator.Evaluate(
                context, *entry.second, actual)) {
            return false;
        }
    }

    return true;
}

static bool BloomMayContain(const string &bytes, double value) {
    return RootBloomFilter::MayContain(bytes, value);
}

static const Expression *StripExpressionCasts(const Expression *expression) {
    while (expression && expression->expression_class == ExpressionClass::BOUND_CAST) {
        expression = expression->Cast<BoundCastExpression>().child.get();
    }
    return expression;
}

static std::optional<Value> ConstantExpressionValue(const Expression &expression) {
    if (expression.expression_class == ExpressionClass::BOUND_CONSTANT) {
        return expression.Cast<BoundConstantExpression>().value;
    }
    if (expression.expression_class == ExpressionClass::BOUND_CAST) {
        const auto &cast = expression.Cast<BoundCastExpression>();
        auto child = ConstantExpressionValue(*cast.child);
        if (!child) return std::nullopt;
        try {
            return child->DefaultCastAs(expression.return_type);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static bool ExtractEqualityConstants(const Expression &expression, std::vector<Value> &values) {
    if (expression.expression_class == ExpressionClass::BOUND_CONJUNCTION &&
        expression.type == ExpressionType::CONJUNCTION_OR) {
        const auto &conjunction = expression.Cast<BoundConjunctionExpression>();
        if (conjunction.children.empty()) return false;
        for (const auto &child : conjunction.children) {
            if (!ExtractEqualityConstants(*child, values)) return false;
        }
        return true;
    }
    if (expression.expression_class == ExpressionClass::BOUND_COMPARISON &&
        expression.type == ExpressionType::COMPARE_EQUAL) {
        const auto &comparison = expression.Cast<BoundComparisonExpression>();
        auto left = ConstantExpressionValue(*comparison.left);
        auto right = ConstantExpressionValue(*comparison.right);
        if (left && !right) {
            values.push_back(*left);
            return true;
        }
        if (right && !left) {
            values.push_back(*right);
            return true;
        }
        return false;
    }
    if (expression.expression_class == ExpressionClass::BOUND_OPERATOR &&
        expression.type == ExpressionType::COMPARE_IN) {
        const auto &op = expression.Cast<BoundOperatorExpression>();
        if (op.children.size() < 2) return false;
        for (idx_t i = 1; i < op.children.size(); ++i) {
            auto value = ConstantExpressionValue(*op.children[i]);
            if (!value) return false;
            values.push_back(*value);
        }
        return !values.empty();
    }
    return false;
}

static bool BloomMayContainFilter(const TableFilter &filter, const string &bytes, const LogicalType &physical_type) {
    if (bytes.empty()) return true;
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto &constant = filter.Cast<ConstantFilter>();
        if (constant.comparison_type != ExpressionType::COMPARE_EQUAL) return true;
        auto value = FilterConstantAsPhysicalDouble(constant.constant, physical_type);
        return !value || BloomMayContain(bytes, *value);
    }
    case TableFilterType::IN_FILTER: {
        const auto &in_filter = filter.Cast<InFilter>();
        for (const auto &constant : in_filter.values) {
            auto value = FilterConstantAsPhysicalDouble(constant, physical_type);
            if (!value) return true;
            if (BloomMayContain(bytes, *value)) return true;
        }
        return false;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto &optional = filter.Cast<OptionalFilter>();
        return !optional.child_filter || BloomMayContainFilter(*optional.child_filter, bytes, physical_type);
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto &dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) return true;
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) return true;
        return BloomMayContainFilter(*dynamic.filter_data->filter, bytes, physical_type);
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto &conjunction = filter.Cast<ConjunctionAndFilter>();
        for (const auto &child : conjunction.child_filters) {
            if (!BloomMayContainFilter(*child, bytes, physical_type)) return false;
        }
        return true;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto &conjunction = filter.Cast<ConjunctionOrFilter>();
        for (const auto &child : conjunction.child_filters) {
            if (BloomMayContainFilter(*child, bytes, physical_type)) return true;
        }
        return false;
    }
    case TableFilterType::EXPRESSION_FILTER: {
        const auto &expression = filter.Cast<ExpressionFilter>();
        std::vector<Value> constants;
        if (!expression.expr || !ExtractEqualityConstants(*expression.expr, constants)) return true;
        for (const auto &constant : constants) {
            auto value = FilterConstantAsPhysicalDouble(constant, physical_type);
            if (!value || BloomMayContain(bytes, *value)) return true;
        }
        return false;
    }
    default:
        return true;
    }
}

static bool FilterNeedsBloom(const TableFilter &filter) {
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON:
        return filter.Cast<ConstantFilter>().comparison_type == ExpressionType::COMPARE_EQUAL;
    case TableFilterType::IN_FILTER:
        return true;
    case TableFilterType::OPTIONAL_FILTER: {
        const auto &optional = filter.Cast<OptionalFilter>();
        return optional.child_filter && FilterNeedsBloom(*optional.child_filter);
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto &dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) return false;
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        return dynamic.filter_data->initialized && dynamic.filter_data->filter &&
               FilterNeedsBloom(*dynamic.filter_data->filter);
    }
    case TableFilterType::CONJUNCTION_AND: {
        for (const auto &child : filter.Cast<ConjunctionAndFilter>().child_filters) {
            if (FilterNeedsBloom(*child)) return true;
        }
        return false;
    }
    case TableFilterType::CONJUNCTION_OR: {
        for (const auto &child : filter.Cast<ConjunctionOrFilter>().child_filters) {
            if (FilterNeedsBloom(*child)) return true;
        }
        return false;
    }
    case TableFilterType::EXPRESSION_FILTER: {
        const auto &expression = filter.Cast<ExpressionFilter>();
        std::vector<Value> constants;
        return expression.expr && ExtractEqualityConstants(*expression.expr, constants);
    }
    default:
        return false;
    }
}

static void MergeEventRangeIntoGlobal(DatasetGlobalState &global, const RootUnsignedFilterRange &range) {
    if (!range.known) return;
    if (range.impossible) {
        global.has_event_range = true;
        global.event_range_impossible = true;
        return;
    }
    if (!global.has_event_range) {
        global.has_event_range = true;
        global.event_lower = range.lower;
        global.event_upper = range.upper;
        return;
    }
    global.event_lower = std::max(global.event_lower, range.lower);
    global.event_upper = std::min(global.event_upper, range.upper);
    global.event_range_impossible = global.event_lower > global.event_upper;
}

static std::string ExactStringFilterClause(const TableFilter &filter, const std::string &column_sql) {
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto &constant = filter.Cast<ConstantFilter>();
        if (constant.comparison_type != ExpressionType::COMPARE_EQUAL || constant.constant.IsNull()) return {};
        return column_sql + " = " + SqlLiteral(constant.constant.ToString());
    }
    case TableFilterType::IN_FILTER: {
        const auto &in_filter = filter.Cast<InFilter>();
        if (in_filter.values.empty()) return "FALSE";
        std::string clause = column_sql + " IN (";
        bool first = true;
        for (const auto &constant : in_filter.values) {
            if (constant.IsNull()) continue;
            if (!first) clause += ", ";
            clause += SqlLiteral(constant.ToString());
            first = false;
        }
        if (first) return "FALSE";
        clause += ")";
        return clause;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto &optional = filter.Cast<OptionalFilter>();
        return optional.child_filter ? ExactStringFilterClause(*optional.child_filter, column_sql) : std::string();
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto &dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) return {};
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) return {};
        return ExactStringFilterClause(*dynamic.filter_data->filter, column_sql);
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto &conjunction = filter.Cast<ConjunctionAndFilter>();
        std::vector<std::string> clauses;
        for (const auto &child : conjunction.child_filters) {
            auto clause = ExactStringFilterClause(*child, column_sql);
            if (!clause.empty()) clauses.push_back(std::move(clause));
        }
        if (clauses.empty()) return {};
        std::string result = "(";
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) result += " AND ";
            result += clauses[i];
        }
        result += ")";
        return result;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto &conjunction = filter.Cast<ConjunctionOrFilter>();
        if (conjunction.child_filters.empty()) return {};
        std::vector<std::string> clauses;
        for (const auto &child : conjunction.child_filters) {
            auto clause = ExactStringFilterClause(*child, column_sql);
            if (clause.empty()) return {};
            clauses.push_back(std::move(clause));
        }
        std::string result = "(";
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) result += " OR ";
            result += clauses[i];
        }
        result += ")";
        return result;
    }
    default:
        return {};
    }
}


static bool RejectsAllMaterializedRows(const TableFilter &filter) {
    switch (filter.filter_type) {
    case TableFilterType::IS_NULL:
        // read_root_dataset emits only materialized numeric rows. Empty containers
        // emit zero rows rather than a row containing SQL NULL.
        return true;
    case TableFilterType::CONJUNCTION_AND: {
        const auto &conjunction = filter.Cast<ConjunctionAndFilter>();
        for (const auto &child : conjunction.child_filters) {
            if (RejectsAllMaterializedRows(*child)) return true;
        }
        return false;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto &conjunction = filter.Cast<ConjunctionOrFilter>();
        if (conjunction.child_filters.empty()) return false;
        for (const auto &child : conjunction.child_filters) {
            if (!RejectsAllMaterializedRows(*child)) return false;
        }
        return true;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto &optional = filter.Cast<OptionalFilter>();
        return optional.child_filter && RejectsAllMaterializedRows(*optional.child_filter);
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto &dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) return false;
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) return false;
        return RejectsAllMaterializedRows(*dynamic.filter_data->filter);
    }
    default:
        return false;
    }
}

static std::string IdListSQL(const std::vector<SchemaBinding> &schemas, bool column_ids) {
    std::string out = "(";
    for (idx_t i = 0; i < schemas.size(); ++i) {
        if (i) out += ',';
        out += SqlLiteral(column_ids ? schemas[i].column_id : schemas[i].schema_id);
    }
    out += ')';
    return out;
}

static bool PredicateMetadataMayMatch(
    const PathPredicateBinding &predicate,
    const Value &min_value,
    const Value &max_value,
    uint64_t nan_count,
    uint64_t pos_inf_count,
    uint64_t neg_inf_count,
    const Value &bloom_value) {

    // Current index-format min/max/Bloom transport is DOUBLE.
    // Never prune 64-bit integer predicates through it.
    if (!UsesDoubleBackedValueMetadata(
            predicate.value_type)) {
        return true;
    }

    const bool has_finite =
        !min_value.IsNull() &&
        !max_value.IsNull();

    const double min_number =
        has_finite
            ? min_value.GetValue<double>()
            : 0.0;

    const double max_number =
        has_finite
            ? max_value.GetValue<double>()
            : 0.0;

    const string *bloom = nullptr;
    string bloom_storage;

    if (!bloom_value.IsNull()) {
        bloom_storage =
            StringValue::Get(bloom_value);

        bloom = &bloom_storage;
    }

    auto constant =
        [&](idx_t index) {
            return predicate.values[index].AsDouble();
        };

    auto equality_possible =
        [&](double value) {
            if (!std::isfinite(value) ||
                !has_finite ||
                value < min_number ||
                value > max_number) {
                return false;
            }

            return
                !bloom ||
                BloomMayContain(*bloom, value);
        };

    switch (predicate.op) {
    case PathPredicateOp::EQ:
        return equality_possible(constant(0));

    case PathPredicateOp::NE:
        if (nan_count ||
            pos_inf_count ||
            neg_inf_count) {
            return true;
        }

        return
            !has_finite ||
            min_number != constant(0) ||
            max_number != constant(0);

    case PathPredicateOp::LT:
        return
            neg_inf_count ||
            (has_finite &&
             min_number < constant(0));

    case PathPredicateOp::LE:
        return
            neg_inf_count ||
            (has_finite &&
             min_number <= constant(0));

    case PathPredicateOp::GT:
        return
            pos_inf_count ||
            (has_finite &&
             max_number > constant(0));

    case PathPredicateOp::GE:
        return
            pos_inf_count ||
            (has_finite &&
             max_number >= constant(0));

    case PathPredicateOp::BETWEEN:
        return
            has_finite &&
            max_number >= constant(0) &&
            min_number <= constant(1);

    case PathPredicateOp::IN:
        for (const auto &value :
             predicate.values) {
            if (equality_possible(
                    value.AsDouble())) {
                return true;
            }
        }
        return false;
    }

    return true;
}

static void NormalizeIntervals(std::vector<EntryInterval> &intervals) {
    std::sort(intervals.begin(), intervals.end(), [](const EntryInterval &left, const EntryInterval &right) {
        return left.begin < right.begin || (left.begin == right.begin && left.end < right.end);
    });
    std::vector<EntryInterval> merged;
    for (const auto &interval : intervals) {
        if (interval.begin >= interval.end) continue;
        if (!merged.empty() && interval.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, interval.end);
        } else {
            merged.push_back(interval);
        }
    }
    intervals.swap(merged);
}

static void ParseEntrySelection(DatasetBindData &bind, const std::string &raw_json) {
    if (raw_json.empty()) return;
    const auto json = nlohmann::json::parse(raw_json);
    if (!json.is_object()) throw InvalidInputException("entry_selection must be a JSON object keyed by source_id");
    bind.entry_selection_active = true;
    for (auto it = json.begin(); it != json.end(); ++it) {
        const auto source_id = it.key();
        const auto &selection = it.value();
        if (!selection.is_object()) throw InvalidInputException("entry_selection source value must be an object");
        auto &intervals = bind.entry_selection[source_id];
        if (selection.contains("ranges")) {
            if (!selection["ranges"].is_array()) throw InvalidInputException("entry_selection.ranges must be an array");
            for (const auto &range : selection["ranges"]) {
                if (!range.is_array() || range.size() != 2) {
                    throw InvalidInputException("entry_selection range must be [begin,end)");
                }
                intervals.push_back({range[0].get<uint64_t>(), range[1].get<uint64_t>()});
            }
        }
        if (selection.contains("entries")) {
            if (!selection["entries"].is_array()) throw InvalidInputException("entry_selection.entries must be an array");
            for (const auto &entry : selection["entries"]) {
                const auto value = entry.get<uint64_t>();
                if (value == std::numeric_limits<uint64_t>::max()) {
                    throw InvalidInputException("entry_selection entry is too large");
                }
                intervals.push_back({value, value + 1});
            }
        }
        if (selection.contains("entries_delta")) {
            const auto &encoded = selection["entries_delta"];
            if (!encoded.is_object() || !encoded.contains("base") || !encoded.contains("deltas") ||
                !encoded["deltas"].is_array()) {
                throw InvalidInputException("entry_selection.entries_delta must contain base and deltas[]");
            }
            auto value = encoded["base"].get<uint64_t>();
            if (value == std::numeric_limits<uint64_t>::max()) {
                throw InvalidInputException("entry_selection delta base is too large");
            }
            intervals.push_back({value, value + 1});
            for (const auto &delta_json : encoded["deltas"]) {
                const auto delta = delta_json.get<uint64_t>();
                if (delta == 0 || value > std::numeric_limits<uint64_t>::max() - delta) {
                    throw InvalidInputException("entry_selection deltas must be positive and non-overflowing");
                }
                value += delta;
                if (value == std::numeric_limits<uint64_t>::max()) {
                    throw InvalidInputException("entry_selection delta entry is too large");
                }
                intervals.push_back({value, value + 1});
            }
        }
        NormalizeIntervals(intervals);
    }
}

static std::vector<EntryInterval> IntersectIntervals(const std::vector<EntryInterval> &left,
                                                     const std::vector<EntryInterval> &right) {
    std::vector<EntryInterval> result;
    idx_t i = 0;
    idx_t j = 0;
    while (i < left.size() && j < right.size()) {
        const uint64_t begin = std::max(left[i].begin, right[j].begin);
        const uint64_t end = std::min(left[i].end, right[j].end);
        if (begin < end) result.push_back({begin, end});
        if (left[i].end < right[j].end) ++i;
        else ++j;
    }
    NormalizeIntervals(result);
    return result;
}

static std::unordered_map<std::string, std::vector<EntryInterval>> LoadPredicateIntervals(
    ClientContext &context, const DatasetBindData &bind, const PathPredicateBinding &predicate,
    uint64_t &basket_counter, uint64_t &bloom_bytes) {
    const auto files_relation = CatalogRelationSQL(bind.sources.files, bind.sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(bind.sources.baskets, bind.sources.sql_tables);
    std::string snapshot_clause;
    if (!bind.sources.snapshot_id.empty()) {
        snapshot_clause = " AND f.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id) +
                          " AND b.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id);
    }
    const bool needs_bloom =
        UsesDoubleBackedValueMetadata(
            predicate.value_type) &&
        (predicate.op == PathPredicateOp::EQ ||
         predicate.op == PathPredicateOp::IN);
    const auto sql =
        "SELECT f.file_id, b.entry_begin, b.entry_end, b.min_value, b.max_value, b.nan_count, "
        "b.pos_inf_count, b.neg_inf_count, " + std::string(needs_bloom ? "b.bloom_filter" : "NULL::BLOB") +
        " FROM " + baskets_relation + " b JOIN " +
        files_relation + " f ON f.file_id=b.file_id AND f.column_id=b.column_id WHERE b.column_id IN " +
        IdListSQL(predicate.schemas, true) + " AND f.schema_id IN " + IdListSQL(predicate.schemas, false) +
        snapshot_clause + " ORDER BY f.file_id, b.entry_begin";
    Connection connection(*context.db);
    auto result = connection.SendQuery(sql);
    if (result->HasError()) throw IOException("plan predicate ROOT basket scan: " + result->GetError());
    std::unordered_map<std::string, std::vector<EntryInterval>> intervals;
    while (auto chunk = result->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); ++row) {
            ++basket_counter;
            if (!chunk->GetValue(8, row).IsNull()) {
                bloom_bytes += StringValue::Get(chunk->GetValue(8, row)).size();
            }
            if (!PredicateMetadataMayMatch(predicate, chunk->GetValue(3, row), chunk->GetValue(4, row),
                                           chunk->GetValue(5, row).GetValue<uint64_t>(),
                                           chunk->GetValue(6, row).GetValue<uint64_t>(),
                                           chunk->GetValue(7, row).GetValue<uint64_t>(),
                                           chunk->GetValue(8, row))) {
                continue;
            }
            intervals[chunk->GetValue(0, row).ToString()].push_back(
                {chunk->GetValue(1, row).GetValue<uint64_t>(),
                 chunk->GetValue(2, row).GetValue<uint64_t>()});
        }
    }
    if (result->HasError()) throw IOException("plan predicate ROOT basket scan: " + result->GetError());
    for (auto &entry : intervals) NormalizeIntervals(entry.second);
    return intervals;
}

static void BuildPredicateIntersection(ClientContext &context, const DatasetBindData &bind,
                                       DatasetGlobalState &global) {
    if (bind.entry_selection_active) global.candidate_intervals = bind.entry_selection;
    if (bind.path_predicates.empty()) return;
    bool first = !bind.entry_selection_active;
    for (const auto &predicate : bind.path_predicates) {
        auto intervals = LoadPredicateIntervals(context, bind, predicate, global.predicate_index_baskets,
                                                global.bloom_metadata_bytes);
        if (first) {
            global.candidate_intervals = std::move(intervals);
            first = false;
            continue;
        }
        std::unordered_map<std::string, std::vector<EntryInterval>> intersection;
        for (const auto &entry : global.candidate_intervals) {
            auto other = intervals.find(entry.first);
            if (other == intervals.end()) continue;
            auto common = IntersectIntervals(entry.second, other->second);
            if (!common.empty()) intersection.emplace(entry.first, std::move(common));
        }
        global.candidate_intervals.swap(intersection);
        ++global.predicate_intersections;
        if (global.candidate_intervals.empty()) break;
    }
}

class DatasetPlanningTimer {
public:
    explicit DatasetPlanningTimer(uint64_t &elapsed_us_p)
        : elapsed_us(elapsed_us_p), started(std::chrono::steady_clock::now()) {
    }
    ~DatasetPlanningTimer() {
        elapsed_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    }

private:
    uint64_t &elapsed_us;
    std::chrono::steady_clock::time_point started;
};

void DatasetTaskPlanner::BuildFileTaskGroups(DatasetGlobalState &global) {
    global.task_groups.clear();
    idx_t begin = 0;
    while (begin < global.tasks.size()) {
        idx_t end = begin + 1;
        while (end < global.tasks.size() && global.tasks[end].root_uri == global.tasks[begin].root_uri) ++end;
        global.task_groups.push_back({begin, end});
        begin = end;
    }
}

void DatasetTaskPlanner::Plan(
    ClientContext &context, const DatasetBindData &bind,
    DatasetGlobalState &global, optional_ptr<TableFilterSet> filters) {
    DatasetPlanningTimer planning_timer(global.planning_time_us);
    if (filters) global.filters = filters->Copy();
    // event_fk is the first public output column. Both event_fk and entry_id
    // address the same global event coordinate and can prune ROOT baskets.
    constexpr column_t kEventFkOutputColumn = 0;
    if (auto entry_filter =
            FilterForFullColumn(global, static_cast<column_t>(bind.entry_id_column))) {
        MergeEventRangeIntoGlobal(global, ExtractRootUnsignedRange(*entry_filter));
    }
    if (auto event_fk_filter = FilterForFullColumn(global, kEventFkOutputColumn)) {
        MergeEventRangeIntoGlobal(global, ExtractRootUnsignedRange(*event_fk_filter));
    }
    if (global.event_range_impossible) return;
    if (global.filters) {
        for (const auto &entry : global.filters->filters) {
            if (RejectsAllMaterializedRows(*entry.second)) return;
        }
    }
    BuildPredicateIntersection(context, bind, global);
    const bool interval_filter_active = bind.entry_selection_active || !bind.path_predicates.empty();
    if (interval_filter_active && global.candidate_intervals.empty()) return;
    const auto files_relation = CatalogRelationSQL(bind.sources.files, bind.sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(bind.sources.baskets, bind.sources.sql_tables);
    {
        Connection metrics(*context.db);
        std::string totals_filter = " WHERE f.column_id IN " + IdListSQL(bind.schemas, true) +
                                    " AND f.schema_id IN " + IdListSQL(bind.schemas, false);
        if (!bind.sources.snapshot_id.empty()) {
            totals_filter += " AND f.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id);
        }
        auto totals = metrics.Query(
            "SELECT count(DISTINCT f.file_id)::UBIGINT, COALESCE(sum(f.basket_count),0)::UBIGINT, "
            "COALESCE(sum(f.value_count),0)::UBIGINT FROM " + files_relation + " f" + totals_filter);
        EnsureQueryOK(*totals, "read ROOT catalog totals");
        global.catalog_files = totals->GetValue(0, 0).GetValue<uint64_t>();
        global.catalog_baskets = totals->GetValue(1, 0).GetValue<uint64_t>();
    }
    std::string predicate;
    if (!bind.sources.snapshot_id.empty()) {
        predicate += " AND f.snapshot_id = " + SqlLiteral(bind.sources.snapshot_id);
        predicate += " AND b.snapshot_id = " + SqlLiteral(bind.sources.snapshot_id);
    }
    if (UsesDoubleBackedValueMetadata(bind.value_type)) {
        if (auto value_filter =
                FilterForFullColumn(
                    global,
                    static_cast<column_t>(
                        bind.value_column))) {
            const auto file_clause =
                ZonemapClause(
                    *value_filter,
                    "f.min_value",
                    "f.max_value",
                    bind.value_type);

            const auto basket_clause =
                ZonemapClause(
                    *value_filter,
                    "b.min_value",
                    "b.max_value",
                    bind.value_type);

            if (!file_clause.empty()) {
                predicate +=
                    " AND ((" + file_clause +
                    ") OR f.nan_count > 0)";
            }

            if (!basket_clause.empty()) {
                predicate +=
                    " AND ((" + basket_clause +
                    ") OR b.nan_count > 0)";
            }
        }
    }
    if (auto entry_filter = FilterForFullColumn(global, static_cast<column_t>(bind.entry_id_column))) {
        const auto file_clause = ZonemapClause(*entry_filter, "f.event_base",
                                               "f.event_base + f.total_entries - 1", LogicalType::UBIGINT);
        const auto basket_clause = ZonemapClause(*entry_filter, "f.event_base + b.entry_begin",
                                                 "f.event_base + b.entry_end - 1", LogicalType::UBIGINT);
        if (!file_clause.empty()) predicate += " AND (" + file_clause + ")";
        if (!basket_clause.empty()) predicate += " AND (" + basket_clause + ")";
    }
    if (auto event_fk_filter = FilterForFullColumn(global, kEventFkOutputColumn)) {
        const auto file_clause = ZonemapClause(*event_fk_filter, "f.event_base",
                                               "f.event_base + f.total_entries - 1", LogicalType::UBIGINT);
        const auto basket_clause = ZonemapClause(*event_fk_filter, "f.event_base + b.entry_begin",
                                                 "f.event_base + b.entry_end - 1", LogicalType::UBIGINT);
        if (!file_clause.empty()) predicate += " AND (" + file_clause + ")";
        if (!basket_clause.empty()) predicate += " AND (" + basket_clause + ")";
    }
    if (auto source_filter =
            FilterForFullColumn(global, static_cast<column_t>(bind.source_id_column))) {
        const auto source_clause = ExactStringFilterClause(*source_filter, "f.file_id");
        if (!source_clause.empty()) predicate += " AND (" + source_clause + ")";
    }

    auto value_filter =
        FilterForFullColumn(
            global,
            static_cast<column_t>(
                bind.value_column));

    const bool needs_bloom =
        UsesDoubleBackedValueMetadata(bind.value_type) &&
        value_filter &&
        FilterNeedsBloom(*value_filter);
    const auto sql =
        "SELECT f.file_id, f.root_uri, f.tree_name, f.schema_id, f.event_base, f.file_size, f.mtime_ns, "
        "b.basket_id, b.entry_begin, b.entry_end, b.flat_value_begin, b.value_count, b.physical_offset, "
        "b.compressed_size, " + std::string(needs_bloom ? "b.bloom_filter" : "NULL::BLOB") + " FROM " +
        baskets_relation + " b JOIN " + files_relation +
        " f ON f.file_id=b.file_id AND f.column_id=b.column_id WHERE b.column_id IN " +
        IdListSQL(bind.schemas, true) + " AND f.schema_id IN " + IdListSQL(bind.schemas, false) + predicate +
        " ORDER BY f.root_uri, b.physical_offset";

    Connection connection(*context.db);
    auto result = connection.SendQuery(sql);
    if (result->HasError()) throw IOException("plan ROOT basket scan: " + result->GetError());
    std::unordered_map<std::string, bool> freshness_checked;
    std::unordered_map<std::string, bool> selected_files;
    const bool can_stop_after_row_limit = bind.row_limit != std::numeric_limits<uint64_t>::max() &&
        (!global.filters || global.filters->filters.empty()) && !interval_filter_active;
    bool row_limit_planned = false;

    while (auto chunk = result->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); ++row) {
            RootBasketTask task;
            task.file_id = chunk->GetValue(0, row).GetValue<std::string>();
            task.root_uri = chunk->GetValue(1, row).GetValue<std::string>();
            task.tree_name = chunk->GetValue(2, row).GetValue<std::string>();
            task.schema_id = chunk->GetValue(3, row).GetValue<std::string>();
            task.event_base = chunk->GetValue(4, row).GetValue<uint64_t>();
            const auto indexed_size = chunk->GetValue(5, row).GetValue<uint64_t>();
            const auto indexed_mtime = chunk->GetValue(6, row).GetValue<int64_t>();
            task.basket_id = chunk->GetValue(7, row).GetValue<uint32_t>();
            task.entry_begin = chunk->GetValue(8, row).GetValue<uint64_t>();
            task.entry_end = chunk->GetValue(9, row).GetValue<uint64_t>();
            task.flat_value_begin = chunk->GetValue(10, row).GetValue<uint64_t>();
            task.value_count = chunk->GetValue(11, row).GetValue<uint64_t>();
            task.physical_offset = chunk->GetValue(12, row).GetValue<uint64_t>();
            task.compressed_size = chunk->GetValue(13, row).GetValue<uint32_t>();
            const auto bloom_value = chunk->GetValue(14, row);
            if (value_filter && !bloom_value.IsNull()) {
                const auto &bloom = StringValue::Get(bloom_value);
                global.bloom_metadata_bytes += bloom.size();
                if (!BloomMayContainFilter(*value_filter, bloom, bind.value_type)) continue;
            }

            if (bind.require_fresh_index && indexed_size && freshness_checked.emplace(task.root_uri, true).second) {
                std::error_code ec;
                const auto current_size = fs::file_size(task.root_uri, ec);
                if (!ec && current_size != indexed_size) {
                    throw IOException("ROOT file size changed after indexing: " + task.root_uri);
                }
                // The index stores POSIX/Unix mtime_ns. Do not compare it with
                // std::filesystem::file_time_type::time_since_epoch(): that clock
                // is implementation-defined and may use a different epoch.
                struct stat current_stat {};
                if (indexed_mtime && ::stat(task.root_uri.c_str(), &current_stat) == 0) {
                    const auto current_ns =
                        static_cast<int64_t>(current_stat.st_mtim.tv_sec) * 1000000000LL +
                        static_cast<int64_t>(current_stat.st_mtim.tv_nsec);
                    if (current_ns != indexed_mtime) {
                        throw IOException(
                            "ROOT file mtime changed after indexing: " + task.root_uri +
                            " (indexed_mtime_ns=" + std::to_string(indexed_mtime) +
                            ", current_mtime_ns=" + std::to_string(current_ns) + ")");
                    }
                }
            }
            std::vector<RootBasketTask> candidate_tasks;
            if (!interval_filter_active) {
                candidate_tasks.push_back(task);
            } else {
                auto candidates = global.candidate_intervals.find(task.file_id);
                if (candidates == global.candidate_intervals.end()) continue;
                for (const auto &interval : candidates->second) {
                    const uint64_t begin = std::max(task.entry_begin, interval.begin);
                    const uint64_t end = std::min(task.entry_end, interval.end);
                    if (begin >= end) continue;
                    auto clipped = task;
                    clipped.entry_begin = begin;
                    clipped.entry_end = end;
                    candidate_tasks.push_back(std::move(clipped));
                }
            }

            bool counted_basket = false;
            for (auto &candidate : candidate_tasks) {
                if (!counted_basket) {
                    global.planned_compressed_bytes += candidate.compressed_size;
                    global.planned_values += candidate.value_count;
                    ++global.planned_baskets;
                    counted_basket = true;
                }
                selected_files.emplace(candidate.root_uri, true);
                if (!global.tasks.empty()) {
                    auto &previous = global.tasks.back();
                    const uint64_t previous_end = previous.physical_offset + previous.compressed_size;
                    const bool ordered = candidate.physical_offset >= previous_end;
                    const uint64_t gap = ordered ? candidate.physical_offset - previous_end
                                                 : std::numeric_limits<uint64_t>::max();
                    if (previous.root_uri == candidate.root_uri && previous.tree_name == candidate.tree_name &&
                        previous.schema_id == candidate.schema_id && previous.entry_end == candidate.entry_begin &&
                        ordered && gap <= bind.coalesce_gap_bytes) {
                        previous.entry_end = candidate.entry_end;
                        previous.value_count += candidate.value_count;
                        previous.compressed_size = candidate.physical_offset + candidate.compressed_size -
                                                   previous.physical_offset;
                        previous.basket_count += 1;
                        continue;
                    }
                }
                global.tasks.push_back(std::move(candidate));
            }
            if (can_stop_after_row_limit && global.planned_values >= bind.row_limit) {
                row_limit_planned = true;
                break;
            }
        }
        if (row_limit_planned) break;
    }
    if (result->HasError()) throw IOException("plan ROOT basket scan: " + result->GetError());
    global.planned_files = selected_files.size();
    global.skipped_files = global.catalog_files >= global.planned_files ? global.catalog_files - global.planned_files : 0;
    global.skipped_baskets = global.catalog_baskets >= global.planned_baskets ? global.catalog_baskets - global.planned_baskets : 0;
    for (const auto &task : global.tasks) global.scheduled_read_bytes += task.compressed_size;
    BuildFileTaskGroups(global);
}

unique_ptr<FunctionData> DatasetScanBinder::Bind(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &return_names) {
    auto bind = make_uniq<DatasetBindData>();
    bind->catalog_path = input.inputs[0].ToString();
    bind->logical_path = NormalizePath(input.inputs[1].ToString());
    RootDatasetCatalog catalog(
        context, bind->catalog_path, input.named_parameters);
    bind->sources = catalog.Sources();

    auto it = input.named_parameters.find("dictionary");
    if (it != input.named_parameters.end()) bind->dictionary = it->second.ToString();
    std::string dictionary_cleanup;
    it = input.named_parameters.find("dictionary_cleanup");
    if (it != input.named_parameters.end()) dictionary_cleanup = it->second.ToString();
    bind->dictionary_cleanup_mode =
        ParseDictionaryCleanupMode(dictionary_cleanup);
    it = input.named_parameters.find("tree_cache_bytes");
    if (it != input.named_parameters.end()) bind->tree_cache_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("reader_mode");
    if (it != input.named_parameters.end()) bind->reader_mode = ParseRootReaderMode(it->second.ToString());
    it = input.named_parameters.find("raw_validation_entries");
    if (it != input.named_parameters.end()) bind->raw_validation_entries = it->second.GetValue<uint32_t>();
    it = input.named_parameters.find("raw_max_entry_bytes");
    if (it != input.named_parameters.end()) bind->raw_max_entry_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("raw_max_values_per_entry");
    if (it != input.named_parameters.end()) bind->raw_max_values_per_entry = it->second.GetValue<uint64_t>();
    if (bind->raw_max_entry_bytes < 12) {
        throw InvalidInputException("raw_max_entry_bytes must be at least 12");
    }
    if (bind->raw_max_values_per_entry == 0) {
        throw InvalidInputException("raw_max_values_per_entry must be positive");
    }
    it = input.named_parameters.find("coalesce_gap_bytes");
    if (it != input.named_parameters.end()) bind->coalesce_gap_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("prefetch_depth");
    if (it != input.named_parameters.end()) bind->prefetch_depth = it->second.GetValue<uint32_t>();
    it = input.named_parameters.find("prefetch_ranges");
    if (it != input.named_parameters.end()) bind->prefetch_ranges = it->second.GetValue<bool>();
    it = input.named_parameters.find("require_fresh_index");
    if (it != input.named_parameters.end()) bind->require_fresh_index = it->second.GetValue<bool>();
    it = input.named_parameters.find("max_open_files");
    if (it != input.named_parameters.end()) bind->max_open_files = it->second.GetValue<uint32_t>();
    it = input.named_parameters.find("memory_budget_bytes");
    if (it != input.named_parameters.end()) bind->memory_budget_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("estimated_reader_bytes");
    if (it != input.named_parameters.end()) bind->estimated_reader_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("row_limit");
    if (it != input.named_parameters.end()) bind->row_limit = it->second.GetValue<uint64_t>();
    if (bind->estimated_reader_bytes == 0) throw InvalidInputException("estimated_reader_bytes must be positive");

    LoadRootDictionary(bind->dictionary);
    auto schema_set = catalog.LoadSchemas(bind->logical_path);
    bind->schemas = std::move(schema_set.schemas);
    bind->schema_lookup = std::move(schema_set.lookup);
    bind->index_names = std::move(schema_set.index_names);
    bind->value_type = schema_set.value_type;
    bind->value_column = 1 + bind->index_names.size();
    it = input.named_parameters.find("path_predicates");
    if (it != input.named_parameters.end()) {
        ParsePathPredicates(catalog, *bind, it->second.ToString());
    }
    it = input.named_parameters.find("entry_selection");
    if (it != input.named_parameters.end()) {
        ParseEntrySelection(*bind, it->second.ToString());
    }
    it = input.named_parameters.find("entry_selection_file");
    if (it != input.named_parameters.end()) {
        const auto selection_path = it->second.ToString();
        std::ifstream selection_stream(selection_path);
        if (!selection_stream) throw IOException("Failed to open entry_selection_file: " + selection_path);
        std::string selection_json((std::istreambuf_iterator<char>(selection_stream)),
                                   std::istreambuf_iterator<char>());
        ParseEntrySelection(*bind, selection_json);
    }

    {
        const auto files_relation = CatalogRelationSQL(bind->sources.files, bind->sources.sql_tables);
        std::string sql = "SELECT CAST(COALESCE(sum(value_count), 0) AS UBIGINT) FROM " + files_relation +
                          " WHERE column_id IN " + IdListSQL(bind->schemas, true);
        if (!bind->sources.snapshot_id.empty()) {
            sql += " AND snapshot_id = " + SqlLiteral(bind->sources.snapshot_id);
        }
        Connection connection(*context.db);
        auto result = connection.Query(sql);
        EnsureQueryOK(*result, "estimate ROOT dataset cardinality");
        const auto total = result->GetValue(0, 0).GetValue<uint64_t>();
        bind->estimated_cardinality = static_cast<idx_t>(
            std::min<uint64_t>(total, static_cast<uint64_t>(std::numeric_limits<idx_t>::max())));
    }

    return_names.push_back("event_fk");
    return_types.push_back(LogicalType::UBIGINT);
    for (const auto &index_name : bind->index_names) {
        return_names.push_back(index_name);
        return_types.push_back(LogicalType::INTEGER);
    }
    return_names.push_back("value");
    return_types.push_back(bind->value_type);
    bind->source_id_column = return_names.size();
    return_names.push_back("source_id");
    return_types.push_back(LogicalType::VARCHAR);
    bind->entry_id_column = return_names.size();
    return_names.push_back("entry_id");
    return_types.push_back(LogicalType::UBIGINT);
    return std::move(bind);
}

unique_ptr<NodeStatistics> DatasetScanStateFactory::Cardinality(
    const FunctionData *bind_data) {
    if (!bind_data) return nullptr;
    const auto &bind = bind_data->Cast<DatasetBindData>();
    return make_uniq<NodeStatistics>(bind.estimated_cardinality, bind.estimated_cardinality);
}

void DatasetTaskPlanner::PlanMetadataCount(
    ClientContext &context, const DatasetBindData &bind,
    DatasetGlobalState &global) {
    DatasetPlanningTimer planning_timer(global.planning_time_us);
    const auto files_relation = CatalogRelationSQL(bind.sources.files, bind.sources.sql_tables);
    std::string predicate = " WHERE f.column_id IN " + IdListSQL(bind.schemas, true) +
                            " AND f.schema_id IN " + IdListSQL(bind.schemas, false);
    if (!bind.sources.snapshot_id.empty()) {
        predicate += " AND f.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id);
    }
    Connection connection(*context.db);
    auto totals = connection.Query(
        "SELECT count(DISTINCT f.file_id)::UBIGINT, COALESCE(sum(f.basket_count),0)::UBIGINT, "
        "COALESCE(sum(f.value_count),0)::UBIGINT FROM " + files_relation + " f" + predicate);
    EnsureQueryOK(*totals, "plan metadata-only ROOT count");
    global.catalog_files = totals->GetValue(0, 0).GetValue<uint64_t>();
    global.catalog_baskets = totals->GetValue(1, 0).GetValue<uint64_t>();
    global.metadata_total_rows = totals->GetValue(2, 0).GetValue<uint64_t>();
    global.skipped_files = global.catalog_files;
    global.skipped_baskets = global.catalog_baskets;
}

unique_ptr<GlobalTableFunctionState> DatasetScanStateFactory::CreateGlobal(
    ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<DatasetBindData>();
    auto global = make_uniq<DatasetGlobalState>();
    global->scan_column_ids = input.column_ids;
    if (input.projection_ids.empty()) {
        global->output_column_ids = input.column_ids;
    } else {
        global->output_column_ids.reserve(input.projection_ids.size());
        for (const auto projection_id : input.projection_ids) {
            if (projection_id >= input.column_ids.size()) {
                throw InternalException("Invalid ROOT table-function projection id");
            }
            global->output_column_ids.push_back(input.column_ids[projection_id]);
        }
    }
    global->row_limit = bind.row_limit;
    // COUNT(*) is represented by DuckDB using a synthetic/dummy column
    // identifier. Only ordinals in the actual public schema require ROOT
    // materialization. Treat EMPTY, ROW_ID and any other out-of-schema
    // synthetic identifier as metadata-only.
    const auto public_column_count = static_cast<column_t>(bind.entry_id_column + 1);
    const bool no_materialized_projection = global->output_column_ids.empty() ||
        std::none_of(global->output_column_ids.begin(), global->output_column_ids.end(),
                     [public_column_count](column_t column) { return column < public_column_count; });
    const bool has_materialized_filters = input.filters && !input.filters->filters.empty();
    global->metadata_count_only = !has_materialized_filters && bind.path_predicates.empty() &&
                                  !bind.entry_selection_active && no_materialized_projection;
    if (global->metadata_count_only) {
        DatasetTaskPlanner().PlanMetadataCount(context, bind, *global);
    } else {
        DatasetTaskPlanner().Plan(context, bind, *global, input.filters);
    }
    const auto root_runtime = RootRuntimeSettings::From(
        context, std::max<idx_t>(1, global->catalog_files), global->catalog_baskets);
    global->worker_limit = std::max<idx_t>(1, std::min(
        root_runtime.threads, root_runtime.max_in_flight_files));
    if (bind.max_open_files > 0) {
        global->worker_limit = std::min<idx_t>(global->worker_limit, bind.max_open_files);
    }
    if (bind.memory_budget_bytes > 0) {
        const auto memory_workers = std::max<uint64_t>(
            1, bind.memory_budget_bytes / bind.estimated_reader_bytes);
        global->worker_limit = std::min<idx_t>(global->worker_limit, memory_workers);
    }
    return std::move(global);
}

void DatasetScanExecutor::ValidateAccessPlan(
    const SchemaBinding &schema,
    const std::vector<PathLevel> &actual) const {
    if (actual.size() != schema.expected_levels.size()) {
        throw IOException("ROOT access plan depth differs from indexed schema " + schema.schema_id);
    }
    for (idx_t i = 0; i < actual.size(); ++i) {
        const auto &expected = schema.expected_levels[i];
        if (actual[i].name != expected.name || PrimitiveBaseType(actual[i].type) != PrimitiveBaseType(expected.type) ||
            actual[i].offset_in_parent != expected.offset_in_parent || actual[i].is_pointer != expected.is_pointer ||
            actual[i].is_container != expected.is_container || actual[i].is_fixed_array != expected.is_fixed_array ||
            actual[i].fixed_array_length != expected.fixed_array_length ||
            actual[i].array_dimensions != expected.array_dimensions || actual[i].element_size != expected.element_size) {
            throw IOException("ROOT streamer layout changed at level " + std::to_string(i) + " for schema " +
                              schema.schema_id);
        }
    }
}

void DatasetScanExecutor::SyncSerializedCounters(
    DatasetLocalState &local, DatasetGlobalState &global) const {
    const auto &counters = local.path_reader.SerializedCounters();
    if (counters.baskets >= local.reported_serialized_baskets) {
        global.serialized_baskets.fetch_add(counters.baskets - local.reported_serialized_baskets);
    }
    if (counters.compressed_bytes >= local.reported_serialized_compressed_bytes) {
        global.serialized_compressed_bytes.fetch_add(
            counters.compressed_bytes - local.reported_serialized_compressed_bytes);
    }
    if (counters.serialized_bytes >= local.reported_serialized_entry_bytes) {
        global.serialized_entry_bytes.fetch_add(
            counters.serialized_bytes - local.reported_serialized_entry_bytes);
    }
    local.reported_serialized_baskets = counters.baskets;
    local.reported_serialized_compressed_bytes = counters.compressed_bytes;
    local.reported_serialized_entry_bytes = counters.serialized_bytes;
}

void DatasetScanExecutor::OpenTaskFile(
    const DatasetBindData &bind, DatasetGlobalState &global,
    DatasetLocalState &local, const RootBasketTask &task) {
    if (local.open_uri == task.root_uri && local.open_schema == task.schema_id && local.file) {
        global.reused_open_files.fetch_add(1);
        return;
    }
    SyncSerializedCounters(local, global);
    local.path_reader.Reset();
    local.object_reader.Reset();
    local.reported_serialized_baskets = 0;
    local.reported_serialized_compressed_bytes = 0;
    local.reported_serialized_entry_bytes = 0;
    local.file.reset(TFile::Open(task.root_uri.c_str(), "READ"));
    global.opened_files.fetch_add(1);
    if (!local.file || local.file->IsZombie()) throw IOException("Failed to open ROOT file: " + task.root_uri);
    auto schema_it = bind.schema_lookup.find(task.schema_id);
    if (schema_it == bind.schema_lookup.end()) throw IOException("Unknown schema_id in task: " + task.schema_id);
    const auto &schema = bind.schemas[schema_it->second];
    local.object_reader.Bind(local.file.get(), task.tree_name, schema.root_class,
                             bind.dictionary_cleanup_mode);
    auto parsed = ParsePath(bind.logical_path);
    auto *root_class = local.object_reader.RootClass();
    auto *tree = local.object_reader.Tree();
    auto *object_branch = local.object_reader.ObjectBranch();
    auto full_levels = PathResolver::Resolve(root_class, parsed.fields);
    ValidateAccessPlan(schema, full_levels);

    local.predicate_levels.clear();
    local.predicate_levels.reserve(bind.path_predicates.size());
    for (const auto &predicate : bind.path_predicates) {
        const auto predicate_path = ParsePath(predicate.path);
        if (predicate_path.root_class != schema.root_class) {
            throw IOException("Predicate path root class differs from target path root class: " + predicate.path);
        }
        auto predicate_levels = PathResolver::Resolve(root_class, predicate_path.fields);
        const auto predicate_schema_id = SchemaFingerprint(predicate_path.root_class, predicate_levels);
        const auto predicate_schema_it = predicate.schema_lookup.find(predicate_schema_id);
        if (predicate_schema_it == predicate.schema_lookup.end()) {
            throw IOException("ROOT file streamer schema is absent from predicate index for " + predicate.path);
        }
        ValidateAccessPlan(predicate.schemas[predicate_schema_it->second], predicate_levels);
        local.predicate_levels.push_back(std::move(predicate_levels));
    }
    local.path_reader.Resolve(tree, object_branch, root_class,
                              std::move(parsed), std::move(full_levels));
    std::string serialized_rejection;
    if (!bind.path_predicates.empty()) {
        serialized_rejection = "path_predicates require universal object materialization";
    }

    bool projection_applied = false;
    if (bind.path_predicates.empty() &&
        local.path_reader.PhysicalMode() == "ancestor") {
        const auto projection = ApplyBranchProjection(
            tree, {local.path_reader.PhysicalBranch()},
                                                      bind.tree_cache_bytes);
        projection_applied = projection.applied;
        if (projection_applied) global.projected_files.fetch_add(1);
    }
    if (!projection_applied) EnableAllBranches(tree, bind.tree_cache_bytes);

    RootPathReaderOptions reader_options;
    reader_options.reader_mode = bind.reader_mode;
    reader_options.validation_entries = bind.raw_validation_entries;
    reader_options.max_entry_bytes = bind.raw_max_entry_bytes;
    reader_options.max_values_per_entry = bind.raw_max_values_per_entry;
    reader_options.tree_cache_bytes = bind.tree_cache_bytes;
    reader_options.enable_all_branches_on_fallback = true;
    const auto started = local.path_reader.StartSerialized(
        local.object_reader.CurrentObject(), std::move(reader_options),
        std::move(serialized_rejection));
    if (started.fallback_activated) global.fallback_files.fetch_add(1);

    const auto runtime_index_depth = local.path_reader.IndexDepth();
    if (runtime_index_depth != bind.index_names.size()) {
        throw IOException("ROOT container depth differs from the catalog output schema for " + bind.logical_path);
    }
    local.open_uri = task.root_uri;
    local.open_schema = task.schema_id;
}

void DatasetScanExecutor::PrefetchPhysicalRange(
    TFile *file, uint64_t offset, uint64_t size) const {
    if (!file || !size) return;
    constexpr uint64_t max_chunk = static_cast<uint64_t>(std::numeric_limits<Int_t>::max());
    while (size) {
        const auto chunk = static_cast<Int_t>(std::min<uint64_t>(size, max_chunk));
        file->ReadBufferAsync(static_cast<Long64_t>(offset), chunk);
        offset += static_cast<uint64_t>(chunk);
        size -= static_cast<uint64_t>(chunk);
    }
}

bool DatasetScanExecutor::ClaimTask(
    const DatasetBindData &bind, DatasetGlobalState &global,
    DatasetLocalState &local) {
    while (true) {
        if (local.next_task_in_group >= local.task_group_end) {
            const auto group_index = global.next_group.fetch_add(1);
            if (group_index >= global.task_groups.size()) return false;
            local.next_task_in_group = global.task_groups[group_index].begin;
            local.task_group_end = global.task_groups[group_index].end;
        }
        const auto task_index = local.next_task_in_group++;
        local.current_task = global.tasks[task_index];
        if (global.has_event_range) {
            const uint64_t task_event_begin = local.current_task.event_base + local.current_task.entry_begin;
            const uint64_t task_event_end = local.current_task.event_base + local.current_task.entry_end;
            const uint64_t wanted_begin = std::max(task_event_begin, global.event_lower);
            const uint64_t wanted_end_exclusive = global.event_upper == std::numeric_limits<uint64_t>::max()
                                                      ? task_event_end
                                                      : std::min(task_event_end, global.event_upper + 1);
            if (wanted_begin >= wanted_end_exclusive) {
                global.skipped_entries.fetch_add(local.current_task.entry_end - local.current_task.entry_begin);
                continue;
            }
            const uint64_t old_begin = local.current_task.entry_begin;
            const uint64_t old_end = local.current_task.entry_end;
            local.current_task.entry_begin = wanted_begin - local.current_task.event_base;
            local.current_task.entry_end = wanted_end_exclusive - local.current_task.event_base;
            global.skipped_entries.fetch_add((local.current_task.entry_begin - old_begin) +
                                             (old_end - local.current_task.entry_end));
        }
        OpenTaskFile(bind, global, local, local.current_task);
        if (bind.prefetch_ranges) {
            PrefetchPhysicalRange(local.file.get(), local.current_task.physical_offset,
                                  local.current_task.compressed_size);
            for (uint32_t ahead = 1;
                 ahead <= bind.prefetch_depth && task_index + ahead < local.task_group_end; ++ahead) {
                const auto &next = global.tasks[task_index + ahead];
                PrefetchPhysicalRange(local.file.get(), next.physical_offset, next.compressed_size);
            }
        }
        local.current_entry = local.current_task.entry_begin;
        local.values.clear();
        local.value_offset = 0;
        local.has_task = true;
        if (bind.tree_cache_bytes) {
            local.object_reader.Tree()->SetCacheEntryRange(
                static_cast<Long64_t>(local.current_task.entry_begin),
                static_cast<Long64_t>(local.current_task.entry_end));
        }
        return true;
    }
}

bool DatasetScanExecutor::PassesPathPredicates(
    const DatasetBindData &bind,
    DatasetLocalState &local,
    void *object) const {

    if (bind.path_predicates.empty()) {
        return true;
    }

    for (idx_t predicate_index = 0;
         predicate_index <
             bind.path_predicates.size();
         ++predicate_index) {

        const auto &levels =
            local.predicate_levels[
                predicate_index];

        ReadResult result;

        OffsetValueReader::CollectDirect(
            object,
            levels,
            -1,
            0,
            result);

        if (!PathPredicateEventMatches(
                bind.path_predicates[
                    predicate_index],
                result.numbers)) {
            return false;
        }
    }

    return true;
}

bool DatasetScanExecutor::LoadNextEntry(
    const DatasetBindData &bind,
    DatasetLocalState &local,
    DatasetGlobalState &global) {

    const bool typed_transport =
        RequiresTypedDatasetValue(bind.value_type);

    while (local.current_entry <
           local.current_task.entry_end) {

        const auto entry =
            local.current_entry++;

        RootEntryReader object_entry(
            local.object_reader);

        object_entry.Begin(entry);

        if (local.path_reader.SerializedActive()) {
            RootPathReadResult read;

            if (typed_transport) {
                read =
                    local.path_reader.TryReadSerialized(
                        entry,
                        object_entry,
                        local.typed_values,
                        local.flat_indices);
            } else {
                read =
                    local.path_reader.TryReadSerialized(
                        entry,
                        object_entry,
                        local.values,
                        local.flat_indices);
            }

            SyncSerializedCounters(local, global);

            global.get_entry_calls.fetch_add(
                object_entry.LoadCount());

            if (read.fallback_activated) {
                global.fallback_files.fetch_add(1);
            }

            const idx_t decoded_count =
                typed_transport
                    ? local.typed_values.size()
                    : local.values.size();

            if (read.decoded) {
                global.serialized_entry_calls.fetch_add(1);
                global.serialized_values.fetch_add(
                    decoded_count);
            }

            if (read.serialized) {
                global.decoded_values.fetch_add(
                    decoded_count);

                local.value_offset = 0;
                local.value_event_fk =
                    local.current_task.event_base +
                    entry;

                local.value_entry_id = entry;
                local.value_source_id =
                    local.current_task.file_id;

                if (decoded_count != 0) {
                    return true;
                }

                continue;
            }

            if (read.fallback_activated &&
                object_entry.LoadCount()) {
                object_entry.Invalidate();
            }
        }

        const auto prior_loads =
            object_entry.LoadCount();

        auto *object = object_entry.Read();

        global.get_entry_calls.fetch_add(
            object_entry.LoadCount() -
            prior_loads);

        if (!object) {
            continue;
        }

        if (!PassesPathPredicates(
                bind, local, object)) {
            continue;
        }

        local.values.clear();
        local.typed_values.clear();
        local.flat_indices.clear();

        if (typed_transport) {
            local.path_reader.CollectTypedFlat(
                object,
                local.typed_values,
                local.flat_indices);

            global.decoded_values.fetch_add(
                local.typed_values.size());
        } else {
            local.path_reader.CollectFlat(
                object,
                local.values,
                local.flat_indices);

            global.decoded_values.fetch_add(
                local.values.size());
        }

        local.value_offset = 0;
        local.value_event_fk =
            local.current_task.event_base + entry;

        local.value_entry_id = entry;
        local.value_source_id =
            local.current_task.file_id;

        if (typed_transport
                ? !local.typed_values.empty()
                : !local.values.empty()) {
            return true;
        }
    }

    return false;
}

void DatasetScanExecutor::SetDoubleAsType(
    Vector &vector, idx_t row, const LogicalType &type,
    double value) const {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: FlatVector::GetData<bool>(vector)[row] = value != 0; break;
    case LogicalTypeId::TINYINT: FlatVector::GetData<int8_t>(vector)[row] = static_cast<int8_t>(value); break;
    case LogicalTypeId::UTINYINT: FlatVector::GetData<uint8_t>(vector)[row] = static_cast<uint8_t>(value); break;
    case LogicalTypeId::SMALLINT: FlatVector::GetData<int16_t>(vector)[row] = static_cast<int16_t>(value); break;
    case LogicalTypeId::USMALLINT: FlatVector::GetData<uint16_t>(vector)[row] = static_cast<uint16_t>(value); break;
    case LogicalTypeId::INTEGER: FlatVector::GetData<int32_t>(vector)[row] = static_cast<int32_t>(value); break;
    case LogicalTypeId::UINTEGER: FlatVector::GetData<uint32_t>(vector)[row] = static_cast<uint32_t>(value); break;
    case LogicalTypeId::BIGINT: FlatVector::GetData<int64_t>(vector)[row] = static_cast<int64_t>(value); break;
    case LogicalTypeId::UBIGINT: FlatVector::GetData<uint64_t>(vector)[row] = static_cast<uint64_t>(value); break;
    case LogicalTypeId::FLOAT: FlatVector::GetData<float>(vector)[row] = static_cast<float>(value); break;
    case LogicalTypeId::DOUBLE: FlatVector::GetData<double>(vector)[row] = value; break;
    default: throw NotImplementedException("Unsupported ROOT dataset output type " + type.ToString());
    }
    FlatVector::Validity(vector).SetValid(row);
}

void DatasetScanExecutor::SetPrimitiveAsType(
    Vector &vector,
    idx_t row,
    const LogicalType &type,
    const RootPrimitiveValue &value) const {

    switch (type.id()) {
    case LogicalTypeId::BIGINT:
        FlatVector::GetData<int64_t>(vector)[row] =
            value.AsSigned();
        break;

    case LogicalTypeId::UBIGINT:
        FlatVector::GetData<uint64_t>(vector)[row] =
            value.AsUnsigned();
        break;

    default:
        throw InternalException(
            "Typed dataset writer used for non-64-bit type " +
            type.ToString());
    }

    FlatVector::Validity(vector).SetValid(row);
}

void DatasetScanExecutor::EmitProjectedRow(
    const DatasetBindData &bind, const DatasetGlobalState &global,
    DataChunk &output, idx_t output_row, uint64_t event_fk,
    const std::string &source_id, uint64_t entry_id,
    double numeric_value, const int32_t *indices,
    idx_t index_count) const {
    for (idx_t output_col = 0; output_col < global.output_column_ids.size(); ++output_col) {
        const auto full_col = global.output_column_ids[output_col];
        auto &vector = output.data[output_col];
        if (full_col == COLUMN_IDENTIFIER_EMPTY) {
            // DuckDB requests a dummy column for COUNT(*); only the chunk cardinality matters.
            FlatVector::Validity(vector).SetInvalid(output_row);
        } else if (full_col == COLUMN_IDENTIFIER_ROW_ID || full_col == 0) {
            FlatVector::GetData<uint64_t>(vector)[output_row] = event_fk;
            FlatVector::Validity(vector).SetValid(output_row);
        } else if (full_col >= 1 && static_cast<idx_t>(full_col) < bind.value_column) {
            const idx_t index_position = static_cast<idx_t>(full_col) - 1;
            if (index_position < index_count) {
                FlatVector::GetData<int32_t>(vector)[output_row] = indices[index_position];
                FlatVector::Validity(vector).SetValid(output_row);
            } else {
                FlatVector::Validity(vector).SetInvalid(output_row);
            }
        } else if (static_cast<idx_t>(full_col) == bind.value_column) {
            SetDoubleAsType(vector, output_row, bind.value_type, numeric_value);
        } else if (static_cast<idx_t>(full_col) == bind.source_id_column) {
            FlatVector::GetData<string_t>(vector)[output_row] = StringVector::AddString(vector, source_id);
            FlatVector::Validity(vector).SetValid(output_row);
        } else if (static_cast<idx_t>(full_col) == bind.entry_id_column) {
            FlatVector::GetData<uint64_t>(vector)[output_row] = entry_id;
            FlatVector::Validity(vector).SetValid(output_row);
        } else {
            FlatVector::Validity(vector).SetInvalid(output_row);
        }
    }
}

unique_ptr<LocalTableFunctionState> DatasetScanStateFactory::CreateLocal() {
    auto local = make_uniq<DatasetLocalState>();
    return std::move(local);
}

void DatasetScanExecutor::EmitProjectedTypedRow(
    const DatasetBindData &bind,
    const DatasetGlobalState &global,
    DataChunk &output,
    idx_t output_row,
    uint64_t event_fk,
    const std::string &source_id,
    uint64_t entry_id,
    const RootPrimitiveValue &numeric_value,
    const int32_t *indices,
    idx_t index_count) const {

    for (idx_t output_col = 0;
         output_col < global.output_column_ids.size();
         ++output_col) {

        const auto full_col =
            global.output_column_ids[output_col];

        auto &vector = output.data[output_col];

        if (full_col == COLUMN_IDENTIFIER_EMPTY) {
            FlatVector::Validity(vector)
                .SetInvalid(output_row);

        } else if (
            full_col == COLUMN_IDENTIFIER_ROW_ID ||
            full_col == 0) {

            FlatVector::GetData<uint64_t>(
                vector)[output_row] = event_fk;

            FlatVector::Validity(vector)
                .SetValid(output_row);

        } else if (
            full_col >= 1 &&
            static_cast<idx_t>(full_col) <
                bind.value_column) {

            const idx_t index_position =
                static_cast<idx_t>(full_col) - 1;

            if (index_position < index_count) {
                FlatVector::GetData<int32_t>(
                    vector)[output_row] =
                    indices[index_position];

                FlatVector::Validity(vector)
                    .SetValid(output_row);
            } else {
                FlatVector::Validity(vector)
                    .SetInvalid(output_row);
            }

        } else if (
            static_cast<idx_t>(full_col) ==
            bind.value_column) {

            SetPrimitiveAsType(
                vector,
                output_row,
                bind.value_type,
                numeric_value);

        } else if (
            static_cast<idx_t>(full_col) ==
            bind.source_id_column) {

            FlatVector::GetData<string_t>(
                vector)[output_row] =
                StringVector::AddString(
                    vector, source_id);

            FlatVector::Validity(vector)
                .SetValid(output_row);

        } else if (
            static_cast<idx_t>(full_col) ==
            bind.entry_id_column) {

            FlatVector::GetData<uint64_t>(
                vector)[output_row] =
                entry_id;

            FlatVector::Validity(vector)
                .SetValid(output_row);

        } else {
            FlatVector::Validity(vector)
                .SetInvalid(output_row);
        }
    }
}

idx_t DatasetScanExecutor::ReserveOutputRows(
    DatasetGlobalState &global, idx_t requested) const {
    if (requested == 0 || global.stop_requested.load(std::memory_order_relaxed)) return 0;
    if (global.row_limit == std::numeric_limits<uint64_t>::max()) {
        global.emitted_rows.fetch_add(requested, std::memory_order_relaxed);
        return requested;
    }
    auto current = global.emitted_rows.load(std::memory_order_relaxed);
    while (current < global.row_limit) {
        const auto remaining = global.row_limit - current;
        const auto granted = static_cast<uint64_t>(std::min<uint64_t>(remaining, requested));
        if (global.emitted_rows.compare_exchange_weak(current, current + granted,
                                                       std::memory_order_relaxed)) {
            if (current + granted >= global.row_limit) {
                global.stop_requested.store(true, std::memory_order_relaxed);
            }
            return static_cast<idx_t>(granted);
        }
    }
    global.stop_requested.store(true, std::memory_order_relaxed);
    return 0;
}

void DatasetScanExecutor::Scan(
    ClientContext &context, TableFunctionInput &input,
    DataChunk &output) {
    auto &bind = input.bind_data->Cast<DatasetBindData>();
    auto &global = input.global_state->Cast<DatasetGlobalState>();
    auto &local = input.local_state->Cast<DatasetLocalState>();
    idx_t output_count = 0;

    // DuckDB represents COUNT(*) as a single dummy projection. With no filters,
    // root_baskets.value_count is already the exact row cardinality, so produce
    // dummy rows directly from metadata and avoid opening or decoding ROOT files.
    if (global.metadata_count_only) {
        const auto already_emitted = global.metadata_rows_emitted.load(std::memory_order_relaxed);
        if (already_emitted < global.metadata_total_rows) {
            const auto wanted = static_cast<idx_t>(std::min<uint64_t>(
                STANDARD_VECTOR_SIZE, global.metadata_total_rows - already_emitted));
            const auto emit_count = ReserveOutputRows(global, wanted);
            if (!output.data.empty()) {
                auto &dummy = output.data[0];
                for (idx_t row = 0; row < emit_count; ++row) FlatVector::Validity(dummy).SetInvalid(row);
            }
            output_count = emit_count;
            global.metadata_rows_emitted.fetch_add(emit_count, std::memory_order_relaxed);
        }
        output.SetCardinality(output_count);
        return;
    }

    const bool typed_transport =
        RequiresTypedDatasetValue(bind.value_type);

    while (output_count < STANDARD_VECTOR_SIZE) {
        if (!local.has_task) {
            if (!ClaimTask(bind, global, local)) {
                break;
            }
        }

        const idx_t available =
            typed_transport
                ? local.typed_values.size()
                : local.values.size();

        if (local.value_offset >= available) {
            if (!LoadNextEntry(
                    bind, local, global)) {
                local.has_task = false;
                global.completed_tasks.fetch_add(1);
                continue;
            }
        }

        const idx_t loaded =
            typed_transport
                ? local.typed_values.size()
                : local.values.size();

        while (local.value_offset < loaded &&
               output_count < STANDARD_VECTOR_SIZE) {

            const auto value_index =
                local.value_offset++;

            const idx_t index_count =
                bind.index_names.size();

            const int32_t *indices =
                index_count
                    ? local.flat_indices.data() +
                          value_index * index_count
                    : nullptr;

            if (typed_transport) {
                const auto &numeric_value =
                    local.typed_values[value_index];

                if (!PassesTypedFilters(
                        context,
                        local,
                        bind,
                        global,
                        local.value_event_fk,
                        numeric_value,
                        indices,
                        index_count)) {
                    continue;
                }

                if (ReserveOutputRows(global, 1) == 0) {
                    output.SetCardinality(output_count);
                    return;
                }

                EmitProjectedTypedRow(
                    bind,
                    global,
                    output,
                    output_count++,
                    local.value_event_fk,
                    local.value_source_id,
                    local.value_entry_id,
                    numeric_value,
                    indices,
                    index_count);

            } else {
                const auto numeric_value =
                    local.values[value_index];

                if (!PassesFilters(
                        context,
                        local,
                        bind,
                        global,
                        local.value_event_fk,
                        numeric_value,
                        indices,
                        index_count)) {
                    continue;
                }

                if (ReserveOutputRows(global, 1) == 0) {
                    output.SetCardinality(output_count);
                    return;
                }

                EmitProjectedRow(
                    bind,
                    global,
                    output,
                    output_count++,
                    local.value_event_fk,
                    local.value_source_id,
                    local.value_entry_id,
                    numeric_value,
                    indices,
                    index_count);
            }
        }
    }

    output.SetCardinality(output_count);
}

InsertionOrderPreservingMap<string> DatasetScanExplain::Bound(
    TableFunctionToStringInput &input) const {
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data) return result;
    const auto &bind = input.bind_data->Cast<DatasetBindData>();
    result["Logical Path"] = bind.logical_path;
    result["Value Type"] = bind.value_type.ToString();
    result["Index Columns"] = bind.index_names.empty() ? "none" : JoinStrings(bind.index_names, ", ");
    result["Metadata Source"] = bind.sources.sql_tables ? "SQL/Iceberg relations" : bind.catalog_path;
    if (!bind.sources.snapshot_id.empty()) result["Snapshot"] = bind.sources.snapshot_id;
    result["Filter Pushdown"] = "zonemap + optional Bloom + exact in-scan evaluation";
    result["Projection Pushdown"] = "enabled";
    result["ROOT Decode Mode"] = RootReaderModeName(bind.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.raw_validation_entries);
    result["Path Predicate Indexes"] = std::to_string(bind.path_predicates.size());
    result["Explicit Entry Selections"] = std::to_string(bind.entry_selection.size());
    result["Estimated Rows"] = std::to_string(bind.estimated_cardinality);
    if (bind.row_limit != std::numeric_limits<uint64_t>::max()) result["Row Limit"] = std::to_string(bind.row_limit);
    return result;
}

InsertionOrderPreservingMap<string> DatasetScanExplain::Running(
    TableFunctionDynamicToStringInput &input) const {
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data) {
        const auto &bind = input.bind_data->Cast<DatasetBindData>();
        result["Logical Path"] = bind.logical_path;
        result["Value Type"] = bind.value_type.ToString();
        result["Index Columns"] = bind.index_names.empty() ? "none" : JoinStrings(bind.index_names, ", ");
        result["Metadata Source"] = bind.sources.sql_tables ? "SQL/Iceberg relations" : bind.catalog_path;
        result["ROOT Decode Mode"] = RootReaderModeName(bind.reader_mode);
        if (!bind.sources.snapshot_id.empty()) result["Snapshot"] = bind.sources.snapshot_id;
        result["Path Predicate Indexes"] = std::to_string(bind.path_predicates.size());
        result["Estimated Rows"] = std::to_string(bind.estimated_cardinality);
    }
    if (!input.global_state) return result;
    const auto &global = input.global_state->Cast<DatasetGlobalState>();
    result["Predicate Index Baskets"] = std::to_string(global.predicate_index_baskets);
    result["Predicate Intersections"] = std::to_string(global.predicate_intersections);
    result["Planning Time (us)"] = std::to_string(global.planning_time_us);
    result["Bloom Metadata Bytes"] = std::to_string(global.bloom_metadata_bytes);
    result["Catalog ROOT Files"] = std::to_string(global.catalog_files);
    result["Selected ROOT Files"] = std::to_string(global.planned_files);
    result["Skipped ROOT Files"] = std::to_string(global.skipped_files);
    result["Catalog Baskets"] = std::to_string(global.catalog_baskets);
    result["Selected Baskets"] = std::to_string(global.planned_baskets);
    result["Skipped Baskets"] = std::to_string(global.skipped_baskets);
    result["Worker Limit"] = std::to_string(global.worker_limit);
    result["Stop Requested"] = global.stop_requested.load() ? "true" : "false";
    result["Opened ROOT Files"] = std::to_string(global.opened_files.load());
    result["Reused Open Files"] = std::to_string(global.reused_open_files.load());
    result["GetEntry Calls"] = std::to_string(global.get_entry_calls.load());
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entry_calls.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Projected ROOT Files"] = std::to_string(global.projected_files.load());
    result["Object Fallback Files"] = std::to_string(global.fallback_files.load());
    result["Coalesced Read Tasks"] = std::to_string(global.tasks.size());
    result["File-affinity Groups"] = std::to_string(global.task_groups.size());
    result["Selected Basket Bytes"] = std::to_string(global.planned_compressed_bytes);
    result["Scheduled Range Bytes"] = std::to_string(global.scheduled_read_bytes);
    result["Indexed Values in Tasks"] = std::to_string(global.planned_values);
    result["Skipped ROOT Entries"] = std::to_string(global.skipped_entries.load());
    result["Metadata-only Rows"] = std::to_string(global.metadata_rows_emitted.load());
    if (global.metadata_count_only) {
        result["Metadata Cardinality"] = std::to_string(global.metadata_total_rows);
    }
    result["Decoded Values"] = std::to_string(global.decoded_values.load());
    result["Emitted Rows"] = std::to_string(global.emitted_rows.load());
    return result;
}

double DatasetScanExecutor::Progress(
    const GlobalTableFunctionState *state) const {
    if (!state) return 0.0;
    const auto &global = state->Cast<DatasetGlobalState>();
    if (global.tasks.empty()) return 100.0;
    const auto completed = std::min<idx_t>(global.completed_tasks.load(), global.tasks.size());
    return 100.0 * static_cast<double>(completed) / static_cast<double>(global.tasks.size());
}

static virtual_column_map_t DatasetVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
    virtual_column_map_t columns;
    columns.emplace(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN));
    return columns;
}


struct DatasetStatsBindData final : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override { return make_uniq<DatasetStatsBindData>(*this); }
    bool Equals(const FunctionData &) const override { return false; }
    bool SupportStatementCache() const override { return false; }

    uint64_t row_count = 0;
    uint64_t non_null_count = 0;
    uint64_t null_count = 0;
    uint64_t nan_count = 0;
    uint64_t pos_inf_count = 0;
    uint64_t neg_inf_count = 0;
    uint64_t basket_count = 0;
    uint64_t compressed_bytes = 0;
    Value min_value;
    Value max_value;
    std::string snapshot_id;
};

struct DatasetStatsGlobalState final : public GlobalTableFunctionState {
    std::atomic<bool> emitted {false};
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData> DatasetStatsBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types,
                                                  vector<string> &return_names) {
    const auto catalog_path = input.inputs[0].ToString();
    const auto logical_path = NormalizePath(input.inputs[1].ToString());
    RootDatasetCatalog catalog(context, catalog_path,
                               input.named_parameters);
    const auto &sources = catalog.Sources();
    const auto files_relation = CatalogRelationSQL(sources.files, sources.sql_tables);
    const auto schemas_relation = CatalogRelationSQL(sources.schemas, sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(sources.baskets, sources.sql_tables);
    std::string snapshot_files;
    std::string snapshot_baskets;
    if (!sources.snapshot_id.empty()) {
        snapshot_files = " AND f.snapshot_id=" + SqlLiteral(sources.snapshot_id);
        snapshot_baskets = " AND b.snapshot_id=" + SqlLiteral(sources.snapshot_id);
    }
    const auto sql =
        "WITH cols AS (SELECT DISTINCT column_id FROM " + schemas_relation +
        " WHERE logical_path=" + SqlLiteral(logical_path) + "), f AS (SELECT "
        "COALESCE(sum(value_count),0)::UBIGINT row_count, "
        "COALESCE(sum(value_count-null_count),0)::UBIGINT non_null_count, "
        "COALESCE(sum(null_count),0)::UBIGINT null_count, "
        "COALESCE(sum(nan_count),0)::UBIGINT nan_count, "
        "COALESCE(sum(pos_inf_count),0)::UBIGINT pos_inf_count, "
        "COALESCE(sum(neg_inf_count),0)::UBIGINT neg_inf_count, "
        "min(min_value)::DOUBLE min_value, max(max_value)::DOUBLE max_value "
        "FROM " + files_relation + " f JOIN cols USING(column_id) WHERE true" + snapshot_files +
        "), b AS (SELECT COALESCE(count(*),0)::UBIGINT basket_count, "
        "COALESCE(sum(compressed_size),0)::UBIGINT compressed_bytes FROM " + baskets_relation +
        " b JOIN cols USING(column_id) WHERE true" + snapshot_baskets +
        ") SELECT f.*, b.* FROM f CROSS JOIN b";
    Connection connection(*context.db);
    auto result = connection.Query(sql);
    EnsureQueryOK(*result, "compute ROOT dataset metadata statistics");
    auto bind = make_uniq<DatasetStatsBindData>();
    bind->row_count = result->GetValue(0, 0).GetValue<uint64_t>();
    bind->non_null_count = result->GetValue(1, 0).GetValue<uint64_t>();
    bind->null_count = result->GetValue(2, 0).GetValue<uint64_t>();
    bind->nan_count = result->GetValue(3, 0).GetValue<uint64_t>();
    bind->pos_inf_count = result->GetValue(4, 0).GetValue<uint64_t>();
    bind->neg_inf_count = result->GetValue(5, 0).GetValue<uint64_t>();
    bind->min_value = result->GetValue(6, 0);
    bind->max_value = result->GetValue(7, 0);
    bind->basket_count = result->GetValue(8, 0).GetValue<uint64_t>();
    bind->compressed_bytes = result->GetValue(9, 0).GetValue<uint64_t>();
    bind->snapshot_id = sources.snapshot_id;
    return_names = {"row_count", "non_null_count", "null_count", "nan_count", "pos_inf_count",
                    "neg_inf_count", "min_value", "max_value", "basket_count", "compressed_bytes",
                    "snapshot_id"};
    return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> DatasetStatsInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<DatasetStatsGlobalState>();
}

static void DatasetStatsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &global = input.global_state->Cast<DatasetStatsGlobalState>();
    if (global.emitted.exchange(true)) {
        output.SetCardinality(0);
        return;
    }
    const auto &bind = input.bind_data->Cast<DatasetStatsBindData>();
    output.SetValue(0, 0, Value::UBIGINT(bind.row_count));
    output.SetValue(1, 0, Value::UBIGINT(bind.non_null_count));
    output.SetValue(2, 0, Value::UBIGINT(bind.null_count));
    output.SetValue(3, 0, Value::UBIGINT(bind.nan_count));
    output.SetValue(4, 0, Value::UBIGINT(bind.pos_inf_count));
    output.SetValue(5, 0, Value::UBIGINT(bind.neg_inf_count));
    output.SetValue(6, 0, bind.min_value);
    output.SetValue(7, 0, bind.max_value);
    output.SetValue(8, 0, Value::UBIGINT(bind.basket_count));
    output.SetValue(9, 0, Value::UBIGINT(bind.compressed_bytes));
    output.SetValue(10, 0, Value(bind.snapshot_id));
    output.SetCardinality(1);
}

static void ConfigureCatalogParameters(TableFunction &function) {
    function.named_parameters["catalog_prefix"] = LogicalType::VARCHAR;
    function.named_parameters["files_table"] = LogicalType::VARCHAR;
    function.named_parameters["schemas_table"] = LogicalType::VARCHAR;
    function.named_parameters["access_table"] = LogicalType::VARCHAR;
    function.named_parameters["baskets_table"] = LogicalType::VARCHAR;
    function.named_parameters["snapshots_table"] = LogicalType::VARCHAR;
    function.named_parameters["commits_table"] = LogicalType::VARCHAR;
    function.named_parameters["snapshot_id"] = LogicalType::VARCHAR;
}

unique_ptr<FunctionData> DatasetBindCallback(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &return_names) {
    return DatasetScanBinder().Bind(
        context, input, return_types, return_names);
}

unique_ptr<GlobalTableFunctionState> DatasetInitCallback(
    ClientContext &context, TableFunctionInitInput &input) {
    return DatasetScanStateFactory().CreateGlobal(context, input);
}

unique_ptr<LocalTableFunctionState> DatasetInitLocalCallback(
    ExecutionContext &, TableFunctionInitInput &,
    GlobalTableFunctionState *) {
    return DatasetScanStateFactory().CreateLocal();
}

void DatasetScanCallback(
    ClientContext &context, TableFunctionInput &input,
    DataChunk &output) {
    DatasetScanExecutor().Scan(context, input, output);
}

InsertionOrderPreservingMap<string> DatasetToStringCallback(
    TableFunctionToStringInput &input) {
    return DatasetScanExplain().Bound(input);
}

InsertionOrderPreservingMap<string> DatasetDynamicToStringCallback(
    TableFunctionDynamicToStringInput &input) {
    return DatasetScanExplain().Running(input);
}

double DatasetProgressCallback(
    ClientContext &, const FunctionData *,
    const GlobalTableFunctionState *state) {
    return DatasetScanExecutor().Progress(state);
}

unique_ptr<NodeStatistics> DatasetCardinalityCallback(
    ClientContext &, const FunctionData *bind_data) {
    return DatasetScanStateFactory().Cardinality(bind_data);
}

void RegisterRootLakeScan(ExtensionLoader &loader) {
    TableFunction function(
        "read_root_dataset",
        {LogicalType::VARCHAR, LogicalType::VARCHAR},
        DatasetScanCallback, DatasetBindCallback,
        DatasetInitCallback, DatasetInitLocalCallback);
    function.named_parameters["dictionary"] = LogicalType::VARCHAR;
    function.named_parameters["dictionary_cleanup"] = LogicalType::VARCHAR;
    ConfigureCatalogParameters(function);
    function.named_parameters["path_predicates"] = LogicalType::VARCHAR;
    function.named_parameters["tree_cache_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["reader_mode"] = LogicalType::VARCHAR;
    function.named_parameters["raw_validation_entries"] = LogicalType::UINTEGER;
    function.named_parameters["raw_max_entry_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["raw_max_values_per_entry"] = LogicalType::UBIGINT;
    function.named_parameters["coalesce_gap_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["prefetch_depth"] = LogicalType::UINTEGER;
    function.named_parameters["prefetch_ranges"] = LogicalType::BOOLEAN;
    function.named_parameters["require_fresh_index"] = LogicalType::BOOLEAN;
    function.named_parameters["max_open_files"] = LogicalType::UINTEGER;
    function.named_parameters["memory_budget_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["estimated_reader_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["entry_selection"] = LogicalType::VARCHAR;
    function.named_parameters["entry_selection_file"] = LogicalType::VARCHAR;
    function.named_parameters["row_limit"] = LogicalType::UBIGINT;
    function.filter_pushdown = true;
    function.filter_prune = true;
    function.projection_pushdown = true;
    function.to_string = DatasetToStringCallback;
    function.dynamic_to_string = DatasetDynamicToStringCallback;
    function.table_scan_progress = DatasetProgressCallback;
    function.cardinality = DatasetCardinalityCallback;
    function.get_virtual_columns = DatasetVirtualColumns;
    loader.RegisterFunction(function);

    TableFunction stats("root_dataset_stats", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                        DatasetStatsScan, DatasetStatsBind, DatasetStatsInit);
    ConfigureCatalogParameters(stats);
    loader.RegisterFunction(stats);
}

} // namespace duckdb::rootlake
