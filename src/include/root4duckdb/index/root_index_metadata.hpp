#pragma once

#include "duckdb/common/common.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {
class DatabaseInstance;

namespace rootlake {

/// File-level lineage and aggregate statistics row.
struct RootFileMetadataRow {
    uint32_t index_version = 0;
    std::string dataset_id;
    std::string snapshot_id;
    std::string file_id;
    std::string root_uri;
    std::string tree_name;
    std::string schema_id;
    std::string column_id;
    uint64_t event_base = 0;
    uint64_t total_entries = 0;
    uint64_t file_size = 0;
    int64_t mtime_ns = 0;
    bool has_minmax = false;
    double min_value = 0;
    double max_value = 0;
    uint64_t value_count = 0;
    uint64_t null_count = 0;
    uint64_t nan_count = 0;
    uint64_t pos_inf_count = 0;
    uint64_t neg_inf_count = 0;
    uint64_t basket_count = 0;
};

/// Logical path and schema identity row.
struct RootSchemaMetadataRow {
    uint32_t index_version = 0;
    std::string schema_id;
    std::string column_id;
    std::string logical_path;
    std::string root_class;
    std::string root_type;
    std::string duckdb_type;
    std::string access_plan_id;
    std::string index_signature;
    uint32_t container_depth = 0;
};

/// One offset traversal step in a semantic access plan.
struct RootAccessMetadataRow {
    uint32_t index_version = 0;
    std::string access_plan_id;
    uint32_t level_no = 0;
    std::string field_name;
    std::string root_type;
    int64_t offset_in_parent = 0;
    int64_t cumulative_offset = 0;
    bool is_pointer = false;
    bool is_container = false;
    bool is_primitive = false;
    bool is_string = false;
    bool is_fixed_array = false;
    uint32_t array_rank = 0;
    uint64_t array_length = 0;
    std::string array_dimensions;
    uint32_t element_size = 0;
};

/// Physical basket bounds and pruning metadata row.
struct RootBasketMetadataRow {
    uint32_t index_version = 0;
    std::string snapshot_id;
    std::string file_id;
    std::string column_id;
    uint32_t basket_id = 0;
    std::string basket_branch_name;
    std::string basket_branch_mode;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
    uint64_t event_base = 0;
    uint64_t flat_value_begin = 0;
    uint64_t value_count = 0;
    uint64_t physical_offset = 0;
    uint32_t key_length = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t compression = 0;
    bool has_minmax = false;
    double min_value = 0;
    double max_value = 0;
    uint64_t null_count = 0;
    uint64_t nan_count = 0;
    uint64_t pos_inf_count = 0;
    uint64_t neg_inf_count = 0;
    std::string bloom_filter;
};

/// Typed metadata accumulated for one indexed ROOT file.
struct RootFileIndexMetadata {
    std::vector<RootFileMetadataRow> files;
    std::vector<RootSchemaMetadataRow> schemas;
    std::vector<RootAccessMetadataRow> access;
    std::vector<RootBasketMetadataRow> baskets;

    uint64_t EstimatedBytes() const;
};

/// Flushes typed worker metadata into bounded Parquet segments.
class RootIndexMetadataWriter {
  public:
    RootIndexMetadataWriter(DatabaseInstance& database, std::string parts_root, idx_t worker_id, uint64_t flush_bytes);
    ~RootIndexMetadataWriter();

    RootIndexMetadataWriter(const RootIndexMetadataWriter&) = delete;
    RootIndexMetadataWriter& operator=(const RootIndexMetadataWriter&) = delete;

    /// Appends one file's typed metadata rows.
    void Append(const RootFileIndexMetadata& metadata);
    /// Flushes remaining rows and closes the writer.
    void Finish();

  private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

/// Compacts worker segments into the published catalog tables.
void CompactRootIndexParquet(DatabaseInstance& database, const std::string& staging_root);

} // namespace rootlake
} // namespace duckdb
