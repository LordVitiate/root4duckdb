#include "root4duckdb/index/root_index_pipeline.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb::rootlake
{

void RootIndexResultWriter::Write(TableFunctionInput& input, DataChunk& output) const
{
    auto& state = input.global_state->Cast<BuildIndexGlobalState>();
    idx_t count = 0;
    while (state.offset < state.statuses.size() && count < STANDARD_VECTOR_SIZE)
    {
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
    output.SetCardinalityUnsafe(count);
}

unique_ptr<FunctionData> BuildIndexBindCallback(ClientContext&, TableFunctionBindInput& input,
                                                vector<LogicalType>& return_types, vector<Identifier>& return_names)
{
    vector<string> names;
    auto result = RootIndexBinder().Bind(input, return_types, names);
    return_names = StringsToIdentifiers(names);
    return result;
}

unique_ptr<GlobalTableFunctionState> BuildIndexInitCallback(ClientContext& context, TableFunctionInitInput& input)
{
    return RootIndexCoordinator().Run(context, input);
}

void BuildIndexCallback(ClientContext&, TableFunctionInput& input, DataChunk& output)
{
    RootIndexResultWriter().Write(input, output);
}

static TableFunction MakeBuildIndexFunction(const std::string& name)
{
    TableFunction function(Identifier(name),
                           {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                           BuildIndexCallback, BuildIndexBindCallback, BuildIndexInitCallback);
    function.GetSignature().WithTypedKwargs("options",
                                            [](TypedKwargs& options)
                                            {
                                                options.Add("dictionary", LogicalType::VARCHAR);
                                                options.Add("dictionary_cleanup", LogicalType::VARCHAR);
                                                options.Add("reader_mode", LogicalType::VARCHAR);
                                                options.Add("raw_validation_entries", LogicalType::UINTEGER);
                                                options.Add("raw_max_entry_bytes", LogicalType::UBIGINT);
                                                options.Add("raw_max_values_per_entry", LogicalType::UBIGINT);
                                                options.Add("tree_cache_bytes", LogicalType::UBIGINT);
                                                options.Add("bloom_bytes", LogicalType::UINTEGER);
                                                options.Add("bloom_false_positive_rate", LogicalType::DOUBLE);
                                                options.Add("index_threads", LogicalType::UINTEGER);
                                                options.Add("max_in_flight_files", LogicalType::UINTEGER);
                                                options.Add("memory_budget_bytes", LogicalType::UBIGINT);
                                                options.Add("estimated_worker_bytes", LogicalType::UBIGINT);
                                                options.Add("metadata_flush_bytes", LogicalType::UBIGINT);
                                                options.Add("chunk_id", LogicalType::VARCHAR);
                                                options.Add("manifest_fingerprint", LogicalType::VARCHAR);
                                                options.Add("dictionary_fingerprint", LogicalType::VARCHAR);
                                                options.Add("overwrite", LogicalType::BOOLEAN);
                                                options.Add("allow_partial", LogicalType::BOOLEAN);
                                                options.Add("resume", LogicalType::BOOLEAN);
                                                options.Add("update_mode", LogicalType::VARCHAR);
                                                options.Add("catalog_prefix", LogicalType::VARCHAR);
                                                options.Add("files_table", LogicalType::VARCHAR);
                                                options.Add("schemas_table", LogicalType::VARCHAR);
                                                options.Add("access_table", LogicalType::VARCHAR);
                                                options.Add("baskets_table", LogicalType::VARCHAR);
                                                options.Add("snapshots_table", LogicalType::VARCHAR);
                                                options.Add("publish_mode", LogicalType::VARCHAR);
                                                options.Add("catalog_mode", LogicalType::VARCHAR);
                                            });
    return function;
}

void RegisterRootLakeIndex(ExtensionLoader& loader)
{
    auto single_or_multi = MakeBuildIndexFunction("root_build_index");
    loader.RegisterFunction(single_or_multi);
    // Index all requested paths in one top-level object read per entry.
    auto dataset = MakeBuildIndexFunction("root_build_dataset_index");
    loader.RegisterFunction(dataset);
}

} // namespace duckdb::rootlake
