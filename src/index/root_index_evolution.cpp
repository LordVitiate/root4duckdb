#include "root4duckdb/index/root_index_pipeline_utils.hpp"

#include <map>
#include <set>

namespace duckdb::rootlake
{
namespace
{

void EnsureIndexSQL(QueryResult& result, const std::string& description)
{
    if (result.HasError())
    {
        throw IOException(description + ": " + result.GetError());
    }
}

std::map<std::string, std::string> PreviousPaths(ClientContext& context, const std::string& catalog,
                                                 const std::string& parent, const std::string& dataset_id)
{
    Connection connection(*context.db);
    std::string sql;
    if (RootIcebergTableExists(catalog, "path_sources"))
    {
        sql = "SELECT DISTINCT logical_path, physical_snapshot_id FROM " +
              RootIcebergRelation(catalog, "path_sources") + " WHERE root_snapshot_id=" + SqlLiteral(parent);
    }
    auto paths = std::map<std::string, std::string>{};
    if (!sql.empty())
    {
        auto result = connection.Query(sql);
        EnsureIndexSQL(*result, "read ROOT index path lineage");
        for (const auto& row : *result)
        {
            const auto path = row.GetBaseValue(0).ToString();
            const auto source = row.GetBaseValue(1).ToString();
            if (!paths.emplace(path, source).second)
            {
                throw IOException("Ambiguous ROOT indexed path lineage: " + path);
            }
        }
    }
    if (!paths.empty())
    {
        return paths;
    }
    const auto files = RootIcebergRelation(catalog, "files");
    const auto schemas = RootIcebergRelation(catalog, "schemas");
    sql = "SELECT DISTINCT s.logical_path, f.snapshot_id FROM " + schemas + " s JOIN " + files +
          " f ON s.column_id=f.column_id WHERE f.snapshot_id=" + SqlLiteral(parent) +
          " AND f.dataset_id=" + SqlLiteral(dataset_id);
    auto result = connection.Query(sql);
    EnsureIndexSQL(*result, "read legacy ROOT index paths");
    for (const auto& row : *result)
    {
        const auto path = row.GetBaseValue(0).ToString();
        if (!paths.emplace(path, row.GetBaseValue(1).ToString()).second)
        {
            throw IOException("Ambiguous ROOT indexed path: " + path);
        }
    }
    if (paths.empty())
    {
        throw IOException("No compatible committed ROOT index paths to extend");
    }
    return paths;
}

void ValidateOriginalFiles(ClientContext& context, const std::string& catalog, const std::string& dataset_id,
                           const std::string& tree_name, const std::vector<RootIndexFilePlan>& plans,
                           const std::map<std::string, std::string>& paths)
{
    Connection connection(*context.db);
    const auto files = RootIcebergRelation(catalog, "files");
    std::set<std::string> origins;
    for (const auto& item : paths)
    {
        origins.insert(item.second);
    }
    for (const auto& origin : origins)
    {
        const auto sql = "SELECT DISTINCT root_uri, file_size, mtime_ns, event_base, total_entries, tree_name "
                         "FROM " +
                         files + " WHERE snapshot_id=" + SqlLiteral(origin) +
                         " AND dataset_id=" + SqlLiteral(dataset_id) + " ORDER BY event_base";
        auto result = connection.Query(sql);
        EnsureIndexSQL(*result, "validate existing ROOT source files");
        idx_t position = 0;
        for (const auto& row : *result)
        {
            if (position >= plans.size() || !plans[position].error.empty())
            {
                throw IOException("ROOT index extension has incompatible input files");
            }
            const auto& plan = plans[position];
            const auto uri = row.GetBaseValue(0).ToString();
            if (uri != plan.path || row.GetBaseValue(1).GetValue<uint64_t>() != LocalFileSize(plan.path) ||
                row.GetBaseValue(2).GetValue<int64_t>() != LocalMtimeNS(plan.path) ||
                row.GetBaseValue(3).GetValue<uint64_t>() != plan.event_base ||
                row.GetBaseValue(4).GetValue<uint64_t>() != plan.entries || row.GetBaseValue(5).ToString() != tree_name)
            {
                throw IOException("ROOT index extension source identity changed: " + plan.path);
            }
            ++position;
        }
        if (position != plans.size())
        {
            throw IOException("ROOT index extension input file count differs from the committed generation");
        }
    }
}

void WritePaths(ClientContext& context, const fs::path& staging, const std::string& generation,
                const std::string& parent, const std::map<std::string, std::string>& paths)
{
    const auto output = staging / "root_path_sources.parquet";
    const auto pending = staging / "root_path_sources.pending.parquet";
    if (fs::exists(output))
    {
        Connection connection(*context.db);
        auto result = connection.Query("SELECT DISTINCT parent_root_snapshot_id FROM read_parquet(" +
                                       SqlLiteral(output.string()) + ")");
        EnsureIndexSQL(*result, "validate restored ROOT path lineage");
        if (result->RowCount() != 1 || result->Fetch()->GetValue(0, 0).ToString() != parent)
        {
            throw IOException("ROOT path checkpoint parent changed during recovery");
        }
        return;
    }
    std::error_code error;
    fs::remove(pending, error);
    if (error)
    {
        throw IOException("Cannot clear incomplete ROOT path metadata: " + error.message());
    }
    std::string rows;
    for (const auto& item : paths)
    {
        if (!rows.empty())
        {
            rows += ",";
        }
        rows += "(" + SqlLiteral(generation) + "," + SqlLiteral(item.first) + "," + SqlLiteral(item.second) + "," +
                SqlLiteral(parent) + ")";
    }
    if (rows.empty())
    {
        throw IOException("Cannot publish an empty ROOT path generation");
    }
    Connection connection(*context.db);
    const auto sql = "COPY (SELECT col0::VARCHAR AS root_snapshot_id, col1::VARCHAR AS logical_path, "
                     "col2::VARCHAR AS physical_snapshot_id, col3::VARCHAR AS parent_root_snapshot_id "
                     "FROM (VALUES " +
                     rows + ")) TO " + SqlLiteral(pending.string()) + " (FORMAT PARQUET, COMPRESSION ZSTD)";
    auto result = connection.Query(sql);
    EnsureIndexSQL(*result, "write ROOT path snapshot");
    fs::rename(pending, output);
}

} // namespace

void PrepareRootIcebergPathSources(ClientContext& context, const BuildIndexBindData& bind,
                                   const BuildIndexGlobalState& state, const std::string& dataset_id,
                                   const std::vector<RootIndexFilePlan>& plans, const fs::path& staging)
{
    if (bind.catalog_mode != "sqlite")
    {
        return;
    }
    std::map<std::string, std::string> paths;
    std::string parent;
    const bool existing = IsRootIcebergCatalog(bind.output_dir);
    if (bind.update_mode == "extend")
    {
        if (!existing)
        {
            throw IOException("ROOT index extension requires an existing committed Iceberg catalog");
        }
        Connection connection(*context.db);
        const auto commits = RootIcebergRelation(bind.output_dir, "commits");
        auto latest = connection.Query("SELECT root_snapshot_id, manifest_fingerprint, dictionary_fingerprint FROM " +
                                       commits + " WHERE state='COMMITTED' ORDER BY committed_at_ns DESC LIMIT 1");
        EnsureIndexSQL(*latest, "resolve previous ROOT index generation");
        if (latest->RowCount() != 1)
        {
            throw IOException("ROOT index extension requires a committed generation");
        }
        const auto row = latest->Fetch();
        parent = row->GetValue(0, 0).ToString();
        if (parent == state.snapshot_id)
        {
            if (!fs::is_regular_file(staging / "root_path_sources.parquet"))
            {
                throw IOException("Committed ROOT index recovery has no path checkpoint");
            }
            return;
        }
        if (row->GetValue(1, 0).ToString() != state.manifest_fingerprint ||
            row->GetValue(2, 0).ToString() != state.dictionary_fingerprint)
        {
            throw IOException("ROOT index extension manifest or dictionary differs from committed catalog");
        }
        paths = PreviousPaths(context, bind.output_dir, parent, dataset_id);
        ValidateOriginalFiles(context, bind.output_dir, dataset_id, bind.tree_name, plans, paths);
        for (const auto& path : bind.logical_paths)
        {
            if (paths.count(path))
            {
                throw InvalidInputException("ROOT path is already indexed: " + path);
            }
        }
    }
    for (const auto& path : bind.logical_paths)
    {
        if (!paths.emplace(path, state.snapshot_id).second)
        {
            throw IOException("Duplicate ROOT logical path in index generation: " + path);
        }
    }
    WritePaths(context, staging, state.snapshot_id, parent, paths);
}

} // namespace duckdb::rootlake
