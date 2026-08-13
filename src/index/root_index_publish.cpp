#include "root4duckdb/index/root_index_pipeline.hpp"
#include "root4duckdb/index/root_index_pipeline_utils.hpp"
namespace duckdb::rootlake {

void RootIndexPublisher::PublishTables(DatabaseInstance& db, const fs::path& staging, const BuildIndexBindData& bind,
                                       const BuildIndexGlobalState& state, const std::string& dataset_id,
                                       const std::string& snapshot_id) {
    if (bind.publish_mode == "none") {
        return;
    }
    const std::vector<std::pair<std::string, fs::path>> tables = {
        {bind.files_table, staging / "root_files.parquet"},
        {bind.schemas_table, staging / "root_schemas.parquet"},
        {bind.access_table, staging / "root_access_levels.parquet"},
        {bind.baskets_table, staging / "root_baskets.parquet"},
    };
    Connection connection(db);
    auto begin = connection.Query("BEGIN TRANSACTION");
    EnsureQueryOK(*begin, "begin metadata catalog publication");
    try {
        if (bind.publish_mode == "replace") {
            auto drop_snapshots = connection.Query("DROP TABLE IF EXISTS " + bind.snapshots_table);
            EnsureQueryOK(*drop_snapshots, "drop old ROOT dataset snapshots table");
        }
        for (const auto& entry : tables) {
            const auto relation = entry.first;
            const auto parquet = SqlLiteral(entry.second.string());
            std::string sql;
            if (bind.publish_mode == "replace") {
                auto drop = connection.Query("DROP TABLE IF EXISTS " + relation);
                EnsureQueryOK(*drop, "drop old metadata catalog table " + relation);
                sql = "CREATE TABLE " + relation + " AS SELECT * FROM read_parquet(" + parquet + ")";
            } else if (bind.publish_mode == "append") {
                auto create = connection.Query("CREATE TABLE IF NOT EXISTS " + relation +
                                               " AS SELECT * FROM read_parquet(" + parquet + ") WHERE false");
                EnsureQueryOK(*create, "create metadata catalog table " + relation);
                sql = "INSERT INTO " + relation + " SELECT * FROM read_parquet(" + parquet + ")";
            } else {
                throw InvalidInputException("publish_mode must be one of: none, replace, append");
            }
            auto result = connection.Query(sql);
            EnsureQueryOK(*result, "publish metadata table " + relation);
        }

        // The snapshots relation is the commit log.  It is written last so a
        // reader never observes a partially published four-table generation.
        auto create_snapshots = connection.Query(
            "CREATE TABLE IF NOT EXISTS " + bind.snapshots_table +
            " ("
            "index_version UINTEGER, dataset_id VARCHAR, snapshot_id VARCHAR, parent_snapshot_id VARCHAR, "
            "created_at_ns UBIGINT, state VARCHAR, root_glob VARCHAR, tree_name VARCHAR, logical_paths VARCHAR, "
            "files_table VARCHAR, schemas_table VARCHAR, access_table VARCHAR, baskets_table VARCHAR, "
            "chunk_id VARCHAR, manifest_fingerprint VARCHAR, dictionary_fingerprint VARCHAR)");
        EnsureQueryOK(*create_snapshots, "create ROOT dataset snapshots table");

        if (!state.manifest_fingerprint.empty()) {
            auto duplicate = connection.Query(
                "SELECT count(*) FROM " + bind.snapshots_table +
                " WHERE state='COMMITTED' AND "
                "manifest_fingerprint=" +
                SqlLiteral(state.manifest_fingerprint) +
                (state.chunk_id.empty() ? std::string() : " AND chunk_id=" + SqlLiteral(state.chunk_id)));
            EnsureQueryOK(*duplicate, "check duplicate ROOT dataset snapshot");
            if (duplicate->GetValue(0, 0).GetValue<uint64_t>() != 0) {
                throw InvalidInputException("This manifest/chunk generation is already committed");
            }
        }

        std::string parent_snapshot;
        auto parent = connection.Query("SELECT snapshot_id FROM " + bind.snapshots_table +
                                       " WHERE dataset_id=" + SqlLiteral(dataset_id) +
                                       " AND state='COMMITTED' ORDER BY created_at_ns DESC LIMIT 1");
        EnsureQueryOK(*parent, "resolve parent ROOT dataset snapshot");
        if (parent->RowCount()) {
            parent_snapshot = parent->GetValue(0, 0).ToString();
        }

        uint64_t created_at_ns = 0;
        try {
            created_at_ns = static_cast<uint64_t>(std::stoull(snapshot_id));
        } catch (...) {
            created_at_ns = 0;
        }
        const auto insert_snapshot =
            "INSERT INTO " + bind.snapshots_table + " VALUES (" + std::to_string(ROOT_LAKE_INDEX_VERSION) + "," +
            SqlLiteral(dataset_id) + "," + SqlLiteral(snapshot_id) + "," +
            (parent_snapshot.empty() ? "NULL" : SqlLiteral(parent_snapshot)) + "," + std::to_string(created_at_ns) +
            ",'COMMITTED'," + SqlLiteral(bind.root_glob) + "," + SqlLiteral(bind.tree_name) + "," +
            SqlLiteral(LogicalPathsJSON(bind.logical_paths)) + "," + SqlLiteral(bind.files_table) + "," +
            SqlLiteral(bind.schemas_table) + "," + SqlLiteral(bind.access_table) + "," +
            SqlLiteral(bind.baskets_table) + "," + (state.chunk_id.empty() ? "NULL" : SqlLiteral(state.chunk_id)) +
            "," + (state.manifest_fingerprint.empty() ? "NULL" : SqlLiteral(state.manifest_fingerprint)) + "," +
            (state.dictionary_fingerprint.empty() ? "NULL" : SqlLiteral(state.dictionary_fingerprint)) + ")";
        auto snapshot_result = connection.Query(insert_snapshot);
        EnsureQueryOK(*snapshot_result, "commit ROOT dataset snapshot");

        auto commit = connection.Query("COMMIT");
        EnsureQueryOK(*commit, "commit metadata catalog publication");
    } catch (...) {
        auto rollback = connection.Query("ROLLBACK");
        (void)rollback;
        throw;
    }
}

void RootIndexPublisher::CommitParquetSnapshot(const BuildIndexBindData& bind, const std::string& dataset_id,
                                               BuildIndexGlobalState& state, const fs::path& staging) {
    const fs::path root(bind.output_dir);
    const fs::path snapshots = root / "snapshots";
    fs::create_directories(snapshots);
    const fs::path final_snapshot = snapshots / state.snapshot_id;
    if (fs::exists(final_snapshot)) {
        if (!bind.overwrite) {
            throw IOException("Snapshot already exists: " + final_snapshot.string());
        }
        fs::remove_all(final_snapshot);
    }
    fs::rename(staging, final_snapshot);
    state.snapshot_dir = final_snapshot.string();

    nlohmann::json snapshot;
    snapshot["format"] = "root4duckdb-lakehouse";
    snapshot["index_version"] = ROOT_LAKE_INDEX_VERSION;
    snapshot["dataset_id"] = dataset_id;
    snapshot["snapshot_id"] = state.snapshot_id;
    snapshot["logical_paths"] = bind.logical_paths;
    snapshot["tree_name"] = bind.tree_name;
    snapshot["chunk_id"] = state.chunk_id;
    snapshot["manifest_fingerprint"] = state.manifest_fingerprint;
    snapshot["dictionary_fingerprint"] = state.dictionary_fingerprint;
    snapshot["state"] = "COMMITTED";
    const auto relative_snapshot = fs::path("snapshots") / state.snapshot_id;
    snapshot["snapshot_dir"] = relative_snapshot.string();
    snapshot["tables"] = {{"files", (relative_snapshot / "root_files.parquet").string()},
                          {"schemas", (relative_snapshot / "root_schemas.parquet").string()},
                          {"access_levels", (relative_snapshot / "root_access_levels.parquet").string()},
                          {"baskets", (relative_snapshot / "root_baskets.parquet").string()}};

    const fs::path temp_current = root / "current.json.tmp";
    const fs::path current = root / "current.json";
    {
        std::ofstream out(temp_current, std::ios::binary);
        out << snapshot.dump(2) << '\n';
    }
    if (std::rename(temp_current.string().c_str(), current.string().c_str()) != 0) {
        const auto message = std::string(std::strerror(errno));
        std::error_code cleanup_ec;
        fs::remove(temp_current, cleanup_ec);
        throw IOException("Cannot atomically publish ROOT index manifest: " + message);
    }
    nlohmann::json success = snapshot;
    success["format"] = "root4duckdb-index-success-v1";
    success["source_file_count"] = state.statuses.size();
    const fs::path success_tmp = root / "_SUCCESS.json.tmp";
    const fs::path success_path = root / "_SUCCESS.json";
    {
        std::ofstream out(success_tmp, std::ios::binary);
        out << success.dump(2) << '\n';
    }
    if (std::rename(success_tmp.string().c_str(), success_path.string().c_str()) != 0) {
        throw IOException("Cannot atomically publish ROOT index success marker");
    }
}

void RootIndexPublisher::Publish(ClientContext& context, const BuildIndexBindData& bind, BuildIndexGlobalState& state,
                                 const std::string& dataset_id, const fs::path& staging) {
    if (bind.catalog_mode == "tables") {
        PublishTables(*context.db, staging, bind, state, dataset_id, state.snapshot_id);
        state.published = true;
        state.publish_mode = bind.publish_mode;
        state.snapshot_dir = "tables:" + (bind.catalog_prefix.empty() ? bind.snapshots_table : bind.catalog_prefix);
        fs::remove_all(staging);
        return;
    }
    if (bind.catalog_mode == "sqlite") {
        const auto iceberg_commit =
            PublishRootIndexStagingToIceberg(context, bind.output_dir, state.snapshot_id, staging.string(),
                                             state.manifest_fingerprint, state.dictionary_fingerprint);
        (void)iceberg_commit;
        fs::remove_all(staging);
        state.snapshot_dir = bind.output_dir;
        state.published = true;
        state.publish_mode = "sqlite-local";
        return;
    }

    CommitParquetSnapshot(bind, dataset_id, state, staging);
    if (bind.catalog_mode == "local") {
        state.published = true;
        state.publish_mode = "local-parquet";
    } else {
        state.published = false;
        state.publish_mode = "external-staging";
    }
}

} // namespace duckdb::rootlake
