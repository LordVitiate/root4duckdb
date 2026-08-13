#include "root4duckdb/index/root_index_pipeline.hpp"

namespace duckdb::rootlake {

void RootIndexResultWriter::Write(TableFunctionInput& input, DataChunk& output) const {
    auto& state = input.global_state->Cast<BuildIndexGlobalState>();
    idx_t count = 0;
    while (state.offset < state.statuses.size() && count < STANDARD_VECTOR_SIZE) {
        const auto& row = state.statuses[state.offset++];
        output.SetValue(0, count, Value(row.file_path));
        output.SetValue(1, count, Value(row.file_id));
        output.SetValue(2, count, Value(row.schema_id));
        output.SetValue(3, count, Value::UBIGINT(row.entries));
        output.SetValue(4, count, Value::UBIGINT(row.flattened_values));
        output.SetValue(5, count, Value::UBIGINT(row.baskets));
        output.SetValue(6, count, Value(row.status));
        output.SetValue(7, count, Value(row.message));
        output.SetValue(8, count, Value(state.snapshot_id));
        output.SetValue(9, count, Value(state.snapshot_dir));
        output.SetValue(10, count, Value::UINTEGER(state.requested_threads));
        output.SetValue(11, count, Value::UINTEGER(state.effective_threads));
        output.SetValue(12, count, Value::BOOLEAN(state.published));
        output.SetValue(13, count, Value(state.publish_mode));
        output.SetValue(14, count, Value(state.chunk_id));
        output.SetValue(15, count, Value(state.manifest_fingerprint));
        output.SetValue(16, count, Value(state.dictionary_fingerprint));
        ++count;
    }
    output.SetCardinality(count);
}

unique_ptr<FunctionData> BuildIndexBindCallback(ClientContext&, TableFunctionBindInput& input,
                                                vector<LogicalType>& return_types, vector<string>& return_names) {
    return RootIndexBinder().Bind(input, return_types, return_names);
}

unique_ptr<GlobalTableFunctionState> BuildIndexInitCallback(ClientContext& context, TableFunctionInitInput& input) {
    return RootIndexCoordinator().Run(context, input);
}

void BuildIndexCallback(ClientContext&, TableFunctionInput& input, DataChunk& output) {
    RootIndexResultWriter().Write(input, output);
}

static TableFunction MakeBuildIndexFunction(const std::string& name) {
    TableFunction function(name,
                           {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                           BuildIndexCallback, BuildIndexBindCallback, BuildIndexInitCallback);
    function.named_parameters["dictionary"] = LogicalType::VARCHAR;
    function.named_parameters["dictionary_cleanup"] = LogicalType::VARCHAR;
    function.named_parameters["reader_mode"] = LogicalType::VARCHAR;
    function.named_parameters["raw_validation_entries"] = LogicalType::UINTEGER;
    function.named_parameters["raw_max_entry_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["raw_max_values_per_entry"] = LogicalType::UBIGINT;
    function.named_parameters["tree_cache_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["bloom_bytes"] = LogicalType::UINTEGER;
    function.named_parameters["bloom_false_positive_rate"] = LogicalType::DOUBLE;
    function.named_parameters["index_threads"] = LogicalType::UINTEGER;
    function.named_parameters["max_in_flight_files"] = LogicalType::UINTEGER;
    function.named_parameters["memory_budget_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["estimated_worker_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["metadata_flush_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["chunk_id"] = LogicalType::VARCHAR;
    function.named_parameters["manifest_fingerprint"] = LogicalType::VARCHAR;
    function.named_parameters["dictionary_fingerprint"] = LogicalType::VARCHAR;
    function.named_parameters["overwrite"] = LogicalType::BOOLEAN;
    function.named_parameters["allow_partial"] = LogicalType::BOOLEAN;
    function.named_parameters["catalog_prefix"] = LogicalType::VARCHAR;
    function.named_parameters["files_table"] = LogicalType::VARCHAR;
    function.named_parameters["schemas_table"] = LogicalType::VARCHAR;
    function.named_parameters["access_table"] = LogicalType::VARCHAR;
    function.named_parameters["baskets_table"] = LogicalType::VARCHAR;
    function.named_parameters["snapshots_table"] = LogicalType::VARCHAR;
    function.named_parameters["publish_mode"] = LogicalType::VARCHAR;
    function.named_parameters["catalog_mode"] = LogicalType::VARCHAR;
    return function;
}

void RegisterRootLakeIndex(ExtensionLoader& loader) {
    auto single_or_multi = MakeBuildIndexFunction("root_build_index");
    loader.RegisterFunction(single_or_multi);
    // Production-facing alias: accepts file, directory, glob, JSON array,
    // comma-separated masks, or @URI-list and indexes all logical paths in one
    // top-level object read per entry.
    auto dataset = MakeBuildIndexFunction("root_build_dataset_index");
    loader.RegisterFunction(dataset);
}

} // namespace duckdb::rootlake
