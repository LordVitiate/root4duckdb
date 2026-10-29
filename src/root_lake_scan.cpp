#include "root4duckdb/dataset/root_dataset_scan_internal.hpp"
#include "root4duckdb/core/root_scan_contract.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"

namespace duckdb::rootlake
{

static virtual_column_map_t DatasetVirtualColumns(ClientContext&, optional_ptr<FunctionData>)
{
    virtual_column_map_t columns;
    columns.emplace(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN));
    return columns;
}

struct DatasetStatsBindData final : public TableFunctionData
{
    unique_ptr<FunctionData> Copy() const override
    {
        return make_uniq<DatasetStatsBindData>(*this);
    }
    bool Equals(const FunctionData&) const override
    {
        return false;
    }
    bool SupportStatementCache() const override
    {
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

struct DatasetStatsGlobalState final : public GlobalTableFunctionState
{
    std::atomic<bool> emitted{false};
    idx_t MaxThreads() const override
    {
        return 1;
    }
};

static unique_ptr<FunctionData> DatasetStatsBind(ClientContext& context, TableFunctionBindInput& input,
                                                 vector<LogicalType>& return_types, vector<Identifier>& return_names)
{
    const auto catalog_path = input.inputs[0].ToString();
    const auto logical_path = NormalizePath(input.inputs[1].ToString());
    const RootDatasetCatalog catalog(context, catalog_path, input.named_parameters);
    const auto& sources = catalog.Sources();
    const auto physical_snapshot = catalog.PathSnapshot(logical_path);
    const auto files_relation = CatalogRelationSQL(sources.files, sources.sql_tables);
    const auto schemas_relation = CatalogRelationSQL(sources.schemas, sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(sources.baskets, sources.sql_tables);
    std::string snapshot_files;
    std::string snapshot_baskets;
    if (!sources.snapshot_id.empty())
    {
        snapshot_files = " AND f.snapshot_id=" + SqlLiteral(physical_snapshot);
        snapshot_baskets = " AND b.snapshot_id=" + SqlLiteral(physical_snapshot);
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
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() != 1)
    {
        throw IOException("ROOT dataset statistics query returned no row");
    }
    bind->row_count = chunk->GetValue(0, 0).GetValue<uint64_t>();
    bind->non_null_count = chunk->GetValue(1, 0).GetValue<uint64_t>();
    bind->null_count = chunk->GetValue(2, 0).GetValue<uint64_t>();
    bind->nan_count = chunk->GetValue(3, 0).GetValue<uint64_t>();
    bind->pos_inf_count = chunk->GetValue(4, 0).GetValue<uint64_t>();
    bind->neg_inf_count = chunk->GetValue(5, 0).GetValue<uint64_t>();
    bind->min_value = chunk->GetValue(6, 0);
    bind->max_value = chunk->GetValue(7, 0);
    bind->basket_count = chunk->GetValue(8, 0).GetValue<uint64_t>();
    bind->compressed_bytes = chunk->GetValue(9, 0).GetValue<uint64_t>();
    bind->snapshot_id = physical_snapshot;
    return_names = {"row_count", "non_null_count", "null_count",   "nan_count",        "pos_inf_count", "neg_inf_count",
                    "min_value", "max_value",      "basket_count", "compressed_bytes", "snapshot_id"};
    return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE,  LogicalType::DOUBLE,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR};
    return bind;
}

static unique_ptr<GlobalTableFunctionState> DatasetStatsInit(ClientContext&, TableFunctionInitInput&)
{
    return make_uniq<DatasetStatsGlobalState>();
}

static void DatasetStatsScan(ClientContext&, TableFunctionInput& input, DataChunk& output)
{
    auto& global = input.global_state->Cast<DatasetStatsGlobalState>();
    if (global.emitted.exchange(true))
    {
        output.SetCardinalityUnsafe(0);
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
    output.SetCardinalityUnsafe(1);
}

static void ConfigureCatalogParameters(TypedKwargs& options)
{
    options.Add("catalog_prefix", LogicalType::VARCHAR);
    options.Add("files_table", LogicalType::VARCHAR);
    options.Add("schemas_table", LogicalType::VARCHAR);
    options.Add("access_table", LogicalType::VARCHAR);
    options.Add("baskets_table", LogicalType::VARCHAR);
    options.Add("snapshots_table", LogicalType::VARCHAR);
    options.Add("commits_table", LogicalType::VARCHAR);
    options.Add("snapshot_id", LogicalType::VARCHAR);
}

unique_ptr<FunctionData> DatasetBindCallback(ClientContext& context, TableFunctionBindInput& input,
                                             vector<LogicalType>& return_types, vector<Identifier>& return_names)
{
    vector<string> names;
    auto result = DatasetScanBinder().Bind(context, input, return_types, names);
    return_names = StringsToIdentifiers(names);
    return result;
}

unique_ptr<GlobalTableFunctionState> DatasetInitCallback(ClientContext& context, TableFunctionInitInput& input)
{
    return DatasetScanStateFactory().CreateGlobal(context, input);
}

unique_ptr<LocalTableFunctionState> DatasetInitLocalCallback(ExecutionContext&, TableFunctionInitInput&,
                                                             GlobalTableFunctionState*)
{
    return DatasetScanStateFactory().CreateLocal();
}

void DatasetScanCallback(ClientContext& context, TableFunctionInput& input, DataChunk& output)
{
    DatasetScanExecutor().Scan(context, input, output);
}

InsertionOrderPreservingMap<string> DatasetToStringCallback(TableFunctionToStringInput& input)
{
    return DatasetScanExplain().Bound(input);
}

void DatasetGetMetricsCallback(TableFunctionGetMetricsInput& input)
{
    const auto details = DatasetScanExplain().Running(input);
    for (const auto& item : details)
    {
        input.operator_metrics.AddExtraInfo(item.first, item.second);
    }
}

double DatasetProgressCallback(ClientContext&, const FunctionData*, const GlobalTableFunctionState* state)
{
    return DatasetScanExecutor().Progress(state);
}

unique_ptr<NodeStatistics> DatasetCardinalityCallback(ClientContext&, const FunctionData* bind_data)
{
    return DatasetScanStateFactory().Cardinality(bind_data);
}

void RegisterRootLakeScan(ExtensionLoader& loader)
{
    TableFunction function("read_root_dataset", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DatasetScanCallback,
                           DatasetBindCallback, DatasetInitCallback, DatasetInitLocalCallback);
    function.GetSignature().WithTypedKwargs("options",
                                            [](TypedKwargs& options)
                                            {
                                                options.Add("dictionary", LogicalType::VARCHAR);
                                                options.Add("dictionary_cleanup", LogicalType::VARCHAR);
                                                ConfigureCatalogParameters(options);
                                                options.Add("path_predicates", LogicalType::VARCHAR);
                                                options.Add("tree_cache_bytes", LogicalType::UBIGINT);
                                                options.Add("reader_mode", LogicalType::VARCHAR);
                                                options.Add("raw_validation_entries", LogicalType::UINTEGER);
                                                options.Add("raw_max_entry_bytes", LogicalType::UBIGINT);
                                                options.Add("raw_max_values_per_entry", LogicalType::UBIGINT);
                                                options.Add("coalesce_gap_bytes", LogicalType::UBIGINT);
                                                options.Add("prefetch_depth", LogicalType::UINTEGER);
                                                options.Add("prefetch_ranges", LogicalType::BOOLEAN);
                                                options.Add("require_fresh_index", LogicalType::BOOLEAN);
                                                options.Add("max_open_files", LogicalType::UINTEGER);
                                                options.Add("memory_budget_bytes", LogicalType::UBIGINT);
                                                options.Add("estimated_reader_bytes", LogicalType::UBIGINT);
                                                options.Add("entry_selection", LogicalType::VARCHAR);
                                                options.Add("entry_selection_file", LogicalType::VARCHAR);
                                                options.Add("row_limit", LogicalType::UBIGINT);
                                            });
    ConfigureRootRelationScan(function);
    function.to_string = DatasetToStringCallback;
    function.get_metrics = DatasetGetMetricsCallback;
    function.table_scan_progress = DatasetProgressCallback;
    function.cardinality = DatasetCardinalityCallback;
    function.get_virtual_columns = DatasetVirtualColumns;
    loader.RegisterFunction(function);
    TableFunction stats("root_dataset_stats", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DatasetStatsScan,
                        DatasetStatsBind, DatasetStatsInit);
    stats.GetSignature().WithTypedKwargs("options",
                                         [](TypedKwargs& options)
                                         {
                                             ConfigureCatalogParameters(options);
                                         });
    loader.RegisterFunction(stats);
}

} // namespace duckdb::rootlake
