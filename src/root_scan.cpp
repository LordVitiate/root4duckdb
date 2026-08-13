#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb {

unique_ptr<FunctionData> RootScanBind(ClientContext& context, TableFunctionBindInput& input,
                                      vector<LogicalType>& return_types, vector<string>& return_names) {
    return RootScanBinder().Bind(context, input, return_types, return_names);
}

unique_ptr<GlobalTableFunctionState> RootScanInit(ClientContext& context, TableFunctionInitInput& input) {
    return RootScanStateFactory().CreateGlobal(context, input);
}

unique_ptr<LocalTableFunctionState> RootScanInitLocal(ExecutionContext&, TableFunctionInitInput& input,
                                                      GlobalTableFunctionState* global_state) {
    return RootScanStateFactory().CreateLocal(input, global_state);
}

void RootScanFunction(ClientContext& context, TableFunctionInput& input, DataChunk& output) {
    RootScanExecutor().Execute(context, input, output);
}

InsertionOrderPreservingMap<string> RootScanToString(TableFunctionToStringInput& input) {
    return RootScanExplain().Bound(input);
}

InsertionOrderPreservingMap<string> RootScanDynamicToString(TableFunctionDynamicToStringInput& input) {
    return RootScanExplain().Running(input);
}

void RegisterRootScan(ExtensionLoader& loader) {
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
