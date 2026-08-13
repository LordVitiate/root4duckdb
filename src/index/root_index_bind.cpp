#include "root4duckdb/index/root_index_pipeline.hpp"
#include "root4duckdb/index/root_index_pipeline_utils.hpp"
namespace duckdb::rootlake {

unique_ptr<FunctionData> BuildIndexBindData::Copy() const {
    auto result = make_uniq<BuildIndexBindData>(*this);
    return std::move(result);
}

bool BuildIndexBindData::Equals(const FunctionData&) const {
    return false;
}

bool BuildIndexBindData::SupportStatementCache() const {
    return false;
}

unique_ptr<FunctionData> RootIndexBinder::Bind(TableFunctionBindInput& input, vector<LogicalType>& return_types,
                                               vector<string>& return_names) {
    auto result = make_uniq<BuildIndexBindData>();
    result->root_glob = input.inputs[0].ToString();
    result->tree_name = input.inputs[1].ToString();
    result->logical_path = input.inputs[2].ToString();
    result->logical_paths = ParseLogicalPaths(result->logical_path);
    result->logical_path = JoinStrings(result->logical_paths, ",");
    result->output_dir = input.inputs[3].ToString();

    auto it = input.named_parameters.find("dictionary");
    if (it != input.named_parameters.end()) {
        result->dictionary = it->second.ToString();
    }
    it = input.named_parameters.find("reader_mode");
    if (it != input.named_parameters.end()) {
        result->reader_mode = ParseRootReaderMode(it->second.ToString());
    }
    it = input.named_parameters.find("raw_validation_entries");
    if (it != input.named_parameters.end()) {
        result->raw_validation_entries = it->second.GetValue<uint32_t>();
    }
    it = input.named_parameters.find("raw_max_entry_bytes");
    if (it != input.named_parameters.end()) {
        result->raw_max_entry_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("raw_max_values_per_entry");
    if (it != input.named_parameters.end()) {
        result->raw_max_values_per_entry = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("tree_cache_bytes");
    if (it != input.named_parameters.end()) {
        result->tree_cache_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("bloom_bytes");
    if (it != input.named_parameters.end()) {
        result->bloom_bytes = it->second.GetValue<uint32_t>();
        result->has_bloom_bytes = true;
    }
    it = input.named_parameters.find("bloom_false_positive_rate");
    if (it != input.named_parameters.end()) {
        result->bloom_false_positive_rate = it->second.GetValue<double>();
    }
    it = input.named_parameters.find("index_threads");
    if (it != input.named_parameters.end()) {
        result->index_threads = it->second.GetValue<uint32_t>();
        result->has_index_threads = true;
    }
    it = input.named_parameters.find("max_in_flight_files");
    if (it != input.named_parameters.end()) {
        result->max_in_flight_files = it->second.GetValue<uint32_t>();
        result->has_max_in_flight_files = true;
    }
    it = input.named_parameters.find("memory_budget_bytes");
    if (it != input.named_parameters.end()) {
        result->memory_budget_bytes = it->second.GetValue<uint64_t>();
        result->has_memory_budget_bytes = true;
    }
    it = input.named_parameters.find("estimated_worker_bytes");
    if (it != input.named_parameters.end()) {
        result->estimated_worker_bytes = it->second.GetValue<uint64_t>();
        result->has_estimated_worker_bytes = true;
    }
    it = input.named_parameters.find("metadata_flush_bytes");
    if (it != input.named_parameters.end()) {
        result->metadata_flush_bytes = it->second.GetValue<uint64_t>();
        result->has_metadata_flush_bytes = true;
    }
    it = input.named_parameters.find("chunk_id");
    if (it != input.named_parameters.end()) {
        result->chunk_id = it->second.ToString();
    }
    it = input.named_parameters.find("manifest_fingerprint");
    if (it != input.named_parameters.end()) {
        result->manifest_fingerprint = it->second.ToString();
    }
    it = input.named_parameters.find("dictionary_fingerprint");
    if (it != input.named_parameters.end()) {
        result->dictionary_fingerprint = it->second.ToString();
    }
    std::string dictionary_cleanup;
    it = input.named_parameters.find("dictionary_cleanup");
    if (it != input.named_parameters.end()) {
        dictionary_cleanup = it->second.ToString();
    }
    result->dictionary_cleanup_mode = ParseDictionaryCleanupMode(dictionary_cleanup);
    it = input.named_parameters.find("overwrite");
    if (it != input.named_parameters.end()) {
        result->overwrite = it->second.GetValue<bool>();
    }
    it = input.named_parameters.find("allow_partial");
    if (it != input.named_parameters.end()) {
        result->allow_partial = it->second.GetValue<bool>();
    }
    it = input.named_parameters.find("catalog_prefix");
    if (it != input.named_parameters.end()) {
        result->catalog_prefix = it->second.ToString();
    }
    it = input.named_parameters.find("files_table");
    if (it != input.named_parameters.end()) {
        result->files_table = it->second.ToString();
    }
    it = input.named_parameters.find("schemas_table");
    if (it != input.named_parameters.end()) {
        result->schemas_table = it->second.ToString();
    }
    it = input.named_parameters.find("access_table");
    if (it != input.named_parameters.end()) {
        result->access_table = it->second.ToString();
    }
    it = input.named_parameters.find("baskets_table");
    if (it != input.named_parameters.end()) {
        result->baskets_table = it->second.ToString();
    }
    it = input.named_parameters.find("snapshots_table");
    if (it != input.named_parameters.end()) {
        result->snapshots_table = it->second.ToString();
    }
    it = input.named_parameters.find("publish_mode");
    if (it != input.named_parameters.end()) {
        result->publish_mode = it->second.ToString();
    }
    it = input.named_parameters.find("catalog_mode");
    if (it != input.named_parameters.end()) {
        result->catalog_mode = it->second.ToString();
        result->has_catalog_mode = true;
    }
    if (!result->catalog_prefix.empty()) {
        if (!IsSafePublishTableName(result->catalog_prefix)) {
            throw InvalidInputException("Unsafe catalog_prefix: " + result->catalog_prefix);
        }
        result->files_table = result->catalog_prefix + "_files";
        result->schemas_table = result->catalog_prefix + "_schemas";
        result->access_table = result->catalog_prefix + "_access";
        result->baskets_table = result->catalog_prefix + "_baskets";
        result->snapshots_table = result->catalog_prefix + "_snapshots";
        if (result->publish_mode == "none") {
            result->publish_mode = "append";
        }
    }
    const bool any_publish_table = !result->files_table.empty() || !result->schemas_table.empty() ||
                                   !result->access_table.empty() || !result->baskets_table.empty() ||
                                   !result->snapshots_table.empty();
    const bool all_publish_tables = !result->files_table.empty() && !result->schemas_table.empty() &&
                                    !result->access_table.empty() && !result->baskets_table.empty() &&
                                    !result->snapshots_table.empty();
    if (any_publish_table && !all_publish_tables) {
        throw InvalidInputException(
            "files_table, schemas_table, access_table, baskets_table and snapshots_table must be supplied together");
    }
    if (all_publish_tables && result->publish_mode == "none") {
        result->publish_mode = "append";
    }
    if (result->publish_mode != "none" && result->publish_mode != "replace" && result->publish_mode != "append") {
        throw InvalidInputException("publish_mode must be one of: none, replace, append");
    }
    if (!result->has_catalog_mode && all_publish_tables) {
        result->catalog_mode = "tables";
    }
    if (result->catalog_mode != "local" && result->catalog_mode != "external" && result->catalog_mode != "sqlite" &&
        result->catalog_mode != "tables") {
        throw InvalidInputException("catalog_mode must be one of: local, external, sqlite, tables");
    }
    if (result->catalog_mode == "tables" && !all_publish_tables) {
        throw InvalidInputException("catalog_mode='tables' requires catalog_prefix or all five metadata tables");
    }
    if (result->catalog_mode != "tables" && result->output_dir.empty()) {
        throw InvalidInputException("catalog_mode='local', 'external' and 'sqlite' require output_dir");
    }
    if (result->catalog_mode != "tables" && result->publish_mode != "none") {
        throw InvalidInputException("publish_mode applies only to catalog_mode='tables'");
    }
    if (result->publish_mode != "none") {
        for (const auto& table : {result->files_table, result->schemas_table, result->access_table,
                                  result->baskets_table, result->snapshots_table}) {
            if (!IsSafePublishTableName(table)) {
                throw InvalidInputException("Unsafe metadata table name: " + table);
            }
        }
    }
    if (result->bloom_bytes > 16U * 1024U * 1024U) {
        throw InvalidInputException("bloom_bytes exceeds the 16 MiB per-basket safety limit");
    }
    if (!(result->bloom_false_positive_rate > 0.0 && result->bloom_false_positive_rate < 1.0)) {
        throw InvalidInputException("bloom_false_positive_rate must be between 0 and 1");
    }
    if (result->has_estimated_worker_bytes && result->estimated_worker_bytes == 0) {
        throw InvalidInputException("estimated_worker_bytes must be positive");
    }
    if (result->has_metadata_flush_bytes && result->metadata_flush_bytes < 16ULL * 1024ULL * 1024ULL) {
        throw InvalidInputException("metadata_flush_bytes must be at least 16 MiB");
    }
    if (result->raw_max_entry_bytes < 12) {
        throw InvalidInputException("raw_max_entry_bytes must be at least 12");
    }
    if (result->raw_max_values_per_entry == 0) {
        throw InvalidInputException("raw_max_values_per_entry must be positive");
    }

    return_names = {"file_path",
                    "file_id",
                    "schema_id",
                    "entries",
                    "flattened_values",
                    "baskets",
                    "status",
                    "message",
                    "snapshot_id",
                    "snapshot_dir",
                    "requested_threads",
                    "effective_threads",
                    "published",
                    "publish_mode",
                    "chunk_id",
                    "manifest_fingerprint",
                    "dictionary_fingerprint"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,  LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR,  LogicalType::VARCHAR,
                    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UINTEGER, LogicalType::UINTEGER,
                    LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,  LogicalType::VARCHAR,
                    LogicalType::VARCHAR};
    return std::move(result);
}

} // namespace duckdb::rootlake
