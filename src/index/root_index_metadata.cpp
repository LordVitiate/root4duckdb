#include "root4duckdb/index/root_index_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace duckdb::rootlake {

namespace fs = std::filesystem;

namespace {

static std::string SqlLiteral(const std::string& value) {
    std::string out = "'";
    for (const char c : value) {
        out += c == '\'' ? "''" : std::string(1, c);
    }
    return out + "'";
}

static void EnsureQueryOK(MaterializedQueryResult& result, const std::string& label) {
    if (result.HasError()) {
        throw IOException(label + ": " + result.GetError());
    }
}

static uint64_t StringBytes(const std::string& value) {
    return static_cast<uint64_t>(value.capacity());
}

static void AppendValues(Appender& appender, const std::vector<Value>& values) {
    appender.BeginRow();
    for (const auto& value : values) {
        appender.Append<Value>(value);
    }
    appender.EndRow();
}

static Value NullableDouble(bool present, double value) {
    return present ? Value::DOUBLE(value) : Value(LogicalType::DOUBLE);
}

static Value NullableBlob(const std::string& value) {
    return value.empty() ? Value(LogicalType::BLOB) : Value::BLOB_RAW(value);
}

} // namespace

uint64_t RootFileIndexMetadata::EstimatedBytes() const {
    uint64_t bytes =
        files.capacity() * sizeof(RootFileMetadataRow) + schemas.capacity() * sizeof(RootSchemaMetadataRow) +
        access.capacity() * sizeof(RootAccessMetadataRow) + baskets.capacity() * sizeof(RootBasketMetadataRow);
    for (const auto& row : files) {
        bytes += StringBytes(row.dataset_id) + StringBytes(row.snapshot_id) + StringBytes(row.file_id) +
                 StringBytes(row.root_uri) + StringBytes(row.tree_name) + StringBytes(row.schema_id) +
                 StringBytes(row.column_id);
    }
    for (const auto& row : schemas) {
        bytes += StringBytes(row.schema_id) + StringBytes(row.column_id) + StringBytes(row.logical_path) +
                 StringBytes(row.root_class) + StringBytes(row.root_type) + StringBytes(row.duckdb_type) +
                 StringBytes(row.access_plan_id) + StringBytes(row.index_signature);
    }
    for (const auto& row : access) {
        bytes += StringBytes(row.access_plan_id) + StringBytes(row.field_name) + StringBytes(row.root_type) +
                 StringBytes(row.array_dimensions);
    }
    for (const auto& row : baskets) {
        bytes += StringBytes(row.snapshot_id) + StringBytes(row.file_id) + StringBytes(row.column_id) +
                 StringBytes(row.basket_branch_name) + StringBytes(row.basket_branch_mode) +
                 StringBytes(row.bloom_filter);
    }
    return bytes;
}

class RootIndexMetadataWriter::Impl {
  public:
    Impl(DatabaseInstance& database_p, std::string parts_root_p, idx_t worker_id_p, uint64_t flush_bytes_p)
        : connection(database_p),
          part_dir(fs::path(std::move(parts_root_p)) / ("worker-" + std::to_string(worker_id_p))),
          flush_bytes(std::max<uint64_t>(16ULL * 1024ULL * 1024ULL, flush_bytes_p)) {
        fs::create_directories(part_dir);
        CreateBuffers();
    }

    void CreateBuffers() {
        Run("CREATE TEMP TABLE root_index_files ("
            "index_version UINTEGER, dataset_id VARCHAR, snapshot_id VARCHAR, file_id VARCHAR, root_uri VARCHAR, "
            "tree_name VARCHAR, schema_id VARCHAR, column_id VARCHAR, event_base UBIGINT, total_entries UBIGINT, "
            "file_size UBIGINT, mtime_ns BIGINT, min_value DOUBLE, max_value DOUBLE, value_count UBIGINT, "
            "null_count UBIGINT, nan_count UBIGINT, pos_inf_count UBIGINT, neg_inf_count UBIGINT, basket_count "
            "UBIGINT)",
            "create typed files buffer");
        Run("CREATE TEMP TABLE root_index_schemas ("
            "index_version UINTEGER, schema_id VARCHAR, column_id VARCHAR, logical_path VARCHAR, root_class VARCHAR, "
            "root_type VARCHAR, duckdb_type VARCHAR, access_plan_id VARCHAR, index_signature VARCHAR, container_depth "
            "UINTEGER)",
            "create typed schemas buffer");
        Run("CREATE TEMP TABLE root_index_access ("
            "index_version UINTEGER, access_plan_id VARCHAR, level_no UINTEGER, field_name VARCHAR, root_type VARCHAR, "
            "offset_in_parent BIGINT, cumulative_offset BIGINT, is_pointer BOOLEAN, is_container BOOLEAN, "
            "is_primitive BOOLEAN, is_string BOOLEAN, is_fixed_array BOOLEAN, array_rank UINTEGER, "
            "array_length UBIGINT, array_dimensions VARCHAR, element_size UINTEGER)",
            "create typed access buffer");
        Run("CREATE TEMP TABLE root_index_baskets ("
            "index_version UINTEGER, snapshot_id VARCHAR, file_id VARCHAR, column_id VARCHAR, basket_id UINTEGER, "
            "basket_branch_name VARCHAR, basket_branch_mode VARCHAR, entry_begin UBIGINT, entry_end UBIGINT, "
            "event_base UBIGINT, flat_value_begin UBIGINT, value_count UBIGINT, physical_offset UBIGINT, "
            "key_length UINTEGER, compressed_size UINTEGER, uncompressed_size UINTEGER, compression UINTEGER, "
            "min_value DOUBLE, max_value DOUBLE, null_count UBIGINT, nan_count UBIGINT, pos_inf_count UBIGINT, "
            "neg_inf_count UBIGINT, bloom_filter BLOB)",
            "create typed baskets buffer");
    }

    void Append(const RootFileIndexMetadata& metadata) {
        if (finished) {
            throw InternalException("Cannot append after RootIndexMetadataWriter::Finish");
        }
        Run("BEGIN TRANSACTION", "begin typed metadata append");
        try {
            Appender files(connection, "root_index_files");
            for (const auto& row : metadata.files) {
                AppendValues(files, {Value::UINTEGER(row.index_version),
                                     Value(row.dataset_id),
                                     Value(row.snapshot_id),
                                     Value(row.file_id),
                                     Value(row.root_uri),
                                     Value(row.tree_name),
                                     Value(row.schema_id),
                                     Value(row.column_id),
                                     Value::UBIGINT(row.event_base),
                                     Value::UBIGINT(row.total_entries),
                                     Value::UBIGINT(row.file_size),
                                     Value::BIGINT(row.mtime_ns),
                                     NullableDouble(row.has_minmax, row.min_value),
                                     NullableDouble(row.has_minmax, row.max_value),
                                     Value::UBIGINT(row.value_count),
                                     Value::UBIGINT(row.null_count),
                                     Value::UBIGINT(row.nan_count),
                                     Value::UBIGINT(row.pos_inf_count),
                                     Value::UBIGINT(row.neg_inf_count),
                                     Value::UBIGINT(row.basket_count)});
            }
            files.Close();

            Appender schemas(connection, "root_index_schemas");
            for (const auto& row : metadata.schemas) {
                AppendValues(schemas, {Value::UINTEGER(row.index_version), Value(row.schema_id), Value(row.column_id),
                                       Value(row.logical_path), Value(row.root_class), Value(row.root_type),
                                       Value(row.duckdb_type), Value(row.access_plan_id), Value(row.index_signature),
                                       Value::UINTEGER(row.container_depth)});
            }
            schemas.Close();

            Appender access(connection, "root_index_access");
            for (const auto& row : metadata.access) {
                AppendValues(access, {Value::UINTEGER(row.index_version), Value(row.access_plan_id),
                                      Value::UINTEGER(row.level_no), Value(row.field_name), Value(row.root_type),
                                      Value::BIGINT(row.offset_in_parent), Value::BIGINT(row.cumulative_offset),
                                      Value::BOOLEAN(row.is_pointer), Value::BOOLEAN(row.is_container),
                                      Value::BOOLEAN(row.is_primitive), Value::BOOLEAN(row.is_string),
                                      Value::BOOLEAN(row.is_fixed_array), Value::UINTEGER(row.array_rank),
                                      Value::UBIGINT(row.array_length), Value(row.array_dimensions),
                                      Value::UINTEGER(row.element_size)});
            }
            access.Close();

            Appender baskets(connection, "root_index_baskets");
            for (const auto& row : metadata.baskets) {
                AppendValues(baskets, {Value::UINTEGER(row.index_version),
                                       Value(row.snapshot_id),
                                       Value(row.file_id),
                                       Value(row.column_id),
                                       Value::UINTEGER(row.basket_id),
                                       Value(row.basket_branch_name),
                                       Value(row.basket_branch_mode),
                                       Value::UBIGINT(row.entry_begin),
                                       Value::UBIGINT(row.entry_end),
                                       Value::UBIGINT(row.event_base),
                                       Value::UBIGINT(row.flat_value_begin),
                                       Value::UBIGINT(row.value_count),
                                       Value::UBIGINT(row.physical_offset),
                                       Value::UINTEGER(row.key_length),
                                       Value::UINTEGER(row.compressed_size),
                                       Value::UINTEGER(row.uncompressed_size),
                                       Value::UINTEGER(row.compression),
                                       NullableDouble(row.has_minmax, row.min_value),
                                       NullableDouble(row.has_minmax, row.max_value),
                                       Value::UBIGINT(row.null_count),
                                       Value::UBIGINT(row.nan_count),
                                       Value::UBIGINT(row.pos_inf_count),
                                       Value::UBIGINT(row.neg_inf_count),
                                       NullableBlob(row.bloom_filter)});
            }
            baskets.Close();
            Run("COMMIT", "commit typed metadata append");
        } catch (...) {
            auto rollback = connection.Query("ROLLBACK");
            (void)rollback;
            throw;
        }
        buffered_bytes += metadata.EstimatedBytes();
        buffered_files += metadata.files.empty() ? 0 : 1;
        if (buffered_bytes >= flush_bytes) {
            Flush();
        }
    }

    void Finish() {
        if (finished) {
            return;
        }
        Flush();
        finished = true;
    }

  private:
    void Run(const std::string& sql, const std::string& label) {
        auto result = connection.Query(sql);
        EnsureQueryOK(*result, label);
    }

    void FlushTable(const std::string& table, const std::string& file_prefix, bool distinct) {
        const auto path = part_dir / (file_prefix + "-" + std::to_string(segment) + ".parquet");
        Run("COPY (SELECT " + std::string(distinct ? "DISTINCT " : "") + "* FROM " + table + ") TO " +
                SqlLiteral(path.string()) + " (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 262144)",
            "flush " + file_prefix + " Parquet segment");
    }

    void Flush() {
        if (buffered_files == 0) {
            return;
        }
        FlushTable("root_index_files", "root_files", false);
        FlushTable("root_index_schemas", "root_schemas", true);
        FlushTable("root_index_access", "root_access_levels", true);
        FlushTable("root_index_baskets", "root_baskets", false);
        // Recreate the in-memory buffers instead of DELETE. DELETE leaves old
        // row versions owned by the connection and defeats the bounded-memory
        // guarantee on long (tens of thousands of files) indexing runs.
        Run("DROP TABLE root_index_files", "drop typed files buffer");
        Run("DROP TABLE root_index_schemas", "drop typed schemas buffer");
        Run("DROP TABLE root_index_access", "drop typed access buffer");
        Run("DROP TABLE root_index_baskets", "drop typed baskets buffer");
        CreateBuffers();
        ++segment;
        buffered_bytes = 0;
        buffered_files = 0;
    }

    Connection connection;
    fs::path part_dir;
    uint64_t flush_bytes;
    uint64_t buffered_bytes = 0;
    uint64_t buffered_files = 0;
    idx_t segment = 0;
    bool finished = false;
};

RootIndexMetadataWriter::RootIndexMetadataWriter(DatabaseInstance& database, std::string parts_root, idx_t worker_id,
                                                 uint64_t flush_bytes)
    : impl(std::make_unique<Impl>(database, std::move(parts_root), worker_id, flush_bytes)) {
}

RootIndexMetadataWriter::~RootIndexMetadataWriter() = default;

void RootIndexMetadataWriter::Append(const RootFileIndexMetadata& metadata) {
    impl->Append(metadata);
}

void RootIndexMetadataWriter::Finish() {
    impl->Finish();
}

void CompactRootIndexParquet(DatabaseInstance& database, const std::string& staging_root) {
    Connection connection(database);
    const auto staging = fs::path(staging_root);
    const auto parts = staging / "parts";
    const std::vector<std::tuple<std::string, std::string, bool>> tables = {
        {"root_files-*.parquet", "root_files.parquet", false},
        {"root_schemas-*.parquet", "root_schemas.parquet", true},
        {"root_access_levels-*.parquet", "root_access_levels.parquet", true},
        {"root_baskets-*.parquet", "root_baskets.parquet", false},
    };
    for (const auto& entry : tables) {
        const auto glob = SqlLiteral((parts / "*" / std::get<0>(entry)).string());
        const auto output = SqlLiteral((staging / std::get<1>(entry)).string());
        auto result = connection.Query("COPY (SELECT " + std::string(std::get<2>(entry) ? "DISTINCT " : "") +
                                       "* FROM read_parquet(" + glob + ", union_by_name=true)) TO " + output +
                                       " (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 262144)");
        EnsureQueryOK(*result, "compact " + std::get<1>(entry));
    }
}

} // namespace duckdb::rootlake
