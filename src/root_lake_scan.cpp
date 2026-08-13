#include "root4duckdb/dataset/root_dataset_scan_internal.hpp"

namespace duckdb::rootlake {

static virtual_column_map_t DatasetVirtualColumns(ClientContext&, optional_ptr<FunctionData>) {
    virtual_column_map_t columns;
    columns.emplace(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN));
    return columns;
}

struct DatasetStatsBindData final : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<DatasetStatsBindData>(*this);
    }
    bool Equals(const FunctionData&) const override {
        return false;
    }
    bool SupportStatementCache() const override {
        return false;
    }

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
    std::atomic<bool> emitted{false};
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> DatasetStatsBind(ClientContext& context, TableFunctionBindInput& input,
                                                 vector<LogicalType>& return_types, vector<string>& return_names) {
    const auto catalog_path = input.inputs[0].ToString();
    const auto logical_path = NormalizePath(input.inputs[1].ToString());
    RootDatasetCatalog catalog(context, catalog_path, input.named_parameters);
    const auto& sources = catalog.Sources();
    const auto files_relation = CatalogRelationSQL(sources.files, sources.sql_tables);
    const auto schemas_relation = CatalogRelationSQL(sources.schemas, sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(sources.baskets, sources.sql_tables);
    std::string snapshot_files;
    std::string snapshot_baskets;
    if (!sources.snapshot_id.empty()) {
        snapshot_files = " AND f.snapshot_id=" + SqlLiteral(sources.snapshot_id);
        snapshot_baskets = " AND b.snapshot_id=" + SqlLiteral(sources.snapshot_id);
    }
    const auto sql = "WITH cols AS (SELECT DISTINCT column_id FROM " + schemas_relation +
                     " WHERE logical_path=" + SqlLiteral(logical_path) +
                     "), f AS (SELECT "
                     "COALESCE(sum(value_count),0)::UBIGINT row_count, "
                     "COALESCE(sum(value_count-null_count),0)::UBIGINT non_null_count, "
                     "COALESCE(sum(null_count),0)::UBIGINT null_count, "
                     "COALESCE(sum(nan_count),0)::UBIGINT nan_count, "
                     "COALESCE(sum(pos_inf_count),0)::UBIGINT pos_inf_count, "
                     "COALESCE(sum(neg_inf_count),0)::UBIGINT neg_inf_count, "
                     "min(min_value)::DOUBLE min_value, max(max_value)::DOUBLE max_value "
                     "FROM " +
                     files_relation + " f JOIN cols USING(column_id) WHERE true" + snapshot_files +
                     "), b AS (SELECT COALESCE(count(*),0)::UBIGINT basket_count, "
                     "COALESCE(sum(compressed_size),0)::UBIGINT compressed_bytes FROM " +
                     baskets_relation + " b JOIN cols USING(column_id) WHERE true" + snapshot_baskets +
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
    return_names = {"row_count", "non_null_count", "null_count",   "nan_count",        "pos_inf_count", "neg_inf_count",
                    "min_value", "max_value",      "basket_count", "compressed_bytes", "snapshot_id"};
    return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE,  LogicalType::DOUBLE,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> DatasetStatsInit(ClientContext&, TableFunctionInitInput&) {
    return make_uniq<DatasetStatsGlobalState>();
}

static void DatasetStatsScan(ClientContext&, TableFunctionInput& input, DataChunk& output) {
    auto& global = input.global_state->Cast<DatasetStatsGlobalState>();
    if (global.emitted.exchange(true)) {
        output.SetCardinality(0);
        return;
    }
    const auto& bind = input.bind_data->Cast<DatasetStatsBindData>();
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

static void ConfigureCatalogParameters(TableFunction& function) {
    function.named_parameters["catalog_prefix"] = LogicalType::VARCHAR;
    function.named_parameters["files_table"] = LogicalType::VARCHAR;
    function.named_parameters["schemas_table"] = LogicalType::VARCHAR;
    function.named_parameters["access_table"] = LogicalType::VARCHAR;
    function.named_parameters["baskets_table"] = LogicalType::VARCHAR;
    function.named_parameters["snapshots_table"] = LogicalType::VARCHAR;
    function.named_parameters["commits_table"] = LogicalType::VARCHAR;
    function.named_parameters["snapshot_id"] = LogicalType::VARCHAR;
}

unique_ptr<FunctionData> DatasetBindCallback(ClientContext& context, TableFunctionBindInput& input,
                                             vector<LogicalType>& return_types, vector<string>& return_names) {
    return DatasetScanBinder().Bind(context, input, return_types, return_names);
}

unique_ptr<GlobalTableFunctionState> DatasetInitCallback(ClientContext& context, TableFunctionInitInput& input) {
    return DatasetScanStateFactory().CreateGlobal(context, input);
}

unique_ptr<LocalTableFunctionState> DatasetInitLocalCallback(ExecutionContext&, TableFunctionInitInput&,
                                                             GlobalTableFunctionState*) {
    return DatasetScanStateFactory().CreateLocal();
}

void DatasetScanCallback(ClientContext& context, TableFunctionInput& input, DataChunk& output) {
    DatasetScanExecutor().Scan(context, input, output);
}

InsertionOrderPreservingMap<string> DatasetToStringCallback(TableFunctionToStringInput& input) {
    return DatasetScanExplain().Bound(input);
}

InsertionOrderPreservingMap<string> DatasetDynamicToStringCallback(TableFunctionDynamicToStringInput& input) {
    return DatasetScanExplain().Running(input);
}

double DatasetProgressCallback(ClientContext&, const FunctionData*, const GlobalTableFunctionState* state) {
    return DatasetScanExecutor().Progress(state);
}

unique_ptr<NodeStatistics> DatasetCardinalityCallback(ClientContext&, const FunctionData* bind_data) {
    return DatasetScanStateFactory().Cardinality(bind_data);
}

void RegisterRootLakeScan(ExtensionLoader& loader) {
    TableFunction function("read_root_dataset", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DatasetScanCallback,
                           DatasetBindCallback, DatasetInitCallback, DatasetInitLocalCallback);
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

    TableFunction stats("root_dataset_stats", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DatasetStatsScan,
                        DatasetStatsBind, DatasetStatsInit);
    ConfigureCatalogParameters(stats);
    loader.RegisterFunction(stats);
}

} // namespace duckdb::rootlake
