#include "root4duckdb/direct/root_scan_internal.hpp"
#include "root4duckdb/core/root_scan_contract.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"

namespace duckdb
{

unique_ptr<FunctionData> RootScanBind(ClientContext& context, TableFunctionBindInput& input,
                                      vector<LogicalType>& return_types, vector<Identifier>& return_names)
{
    vector<string> names;
    auto result = RootScanBinder().Bind(context, input, return_types, names);
    return_names = StringsToIdentifiers(names);
    return result;
}

unique_ptr<GlobalTableFunctionState> RootScanInit(ClientContext& context, TableFunctionInitInput& input)
{
    return RootScanStateFactory().CreateGlobal(context, input);
}

unique_ptr<LocalTableFunctionState> RootScanInitLocal(ExecutionContext&, TableFunctionInitInput& input,
                                                      GlobalTableFunctionState* global_state)
{
    return RootScanStateFactory().CreateLocal(input, global_state);
}

void RootScanFunction(ClientContext& context, TableFunctionInput& input, DataChunk& output)
{
    RootScanExecutor().Execute(context, input, output);
}

InsertionOrderPreservingMap<string> RootScanToString(TableFunctionToStringInput& input)
{
    return RootScanExplain().Bound(input);
}

void RootScanGetMetrics(TableFunctionGetMetricsInput& input)
{
    const auto details = RootScanExplain().Running(input);
    for (const auto& item : details)
    {
        input.operator_metrics.AddExtraInfo(item.first, item.second);
    }
}

void RegisterRootScan(ExtensionLoader& loader)
{
    TableFunction root_scan("read_root", {LogicalType::VARCHAR}, RootScanFunction, RootScanBind, RootScanInit);
    root_scan.GetSignature().WithTypedKwargs("options",
                                             [](TypedKwargs& options)
                                             {
                                                 options.Add("dictionary", LogicalType::VARCHAR);
                                                 options.Add("path_prefix", LogicalType::VARCHAR);
                                                 options.Add("reader_mode", LogicalType::VARCHAR);
                                                 options.Add("raw_validation_entries", LogicalType::UINTEGER);
                                                 options.Add("raw_max_entry_bytes", LogicalType::UBIGINT);
                                                 options.Add("raw_max_values_per_entry", LogicalType::UBIGINT);
                                                 options.Add("tree_cache_bytes", LogicalType::UBIGINT);
                                             });
    rootlake::ConfigureRootRelationScan(root_scan);
    root_scan.pushdown_complex_filter = RootScanCollectPruningHints;
    root_scan.init_local = RootScanInitLocal;
    root_scan.to_string = RootScanToString;
    root_scan.get_metrics = RootScanGetMetrics;
    loader.RegisterFunction(root_scan);
}

} // namespace duckdb
