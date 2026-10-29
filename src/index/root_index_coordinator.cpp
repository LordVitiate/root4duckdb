#include "root4duckdb/index/root_index_pipeline.hpp"
#include "root4duckdb/index/root_index_pipeline_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace duckdb::rootlake
{

namespace
{

// A process-scoped advisory lock survives SIGKILL without retaining a stale
// lock directory. Only one index publisher may mutate one output catalog.
class IndexBuildLock final
{
  public:
    explicit IndexBuildLock(const fs::path& directory)
    {
        const auto path = directory / ".root4duckdb-index.lock";
        fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
            throw IOException("Another ROOT index build owns the output catalog lock: " + path.string());
        }
    }
    ~IndexBuildLock() noexcept
    {
        if (fd >= 0)
        {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }
    IndexBuildLock(const IndexBuildLock&) = delete;
    IndexBuildLock& operator=(const IndexBuildLock&) = delete;

  private:
    int fd = -1;
};

nlohmann::json IndexBuildIdentity(const BuildIndexBindData& bind, const RootIndexBuildOptions& options,
                                  const std::vector<std::string>& files, const std::string& dataset_id)
{
    return {{"format", "root4duckdb-index-request-v1"},
            {"index_version", ROOT_LAKE_INDEX_VERSION},
            {"dataset_id", dataset_id},
            {"manifest", ManifestFingerprint(files)},
            {"dictionary", FileContentFingerprint(bind.dictionary)},
            // Root input-list filenames may differ across production retries;
            // the manifest fingerprint already binds the resolved source set.
            {"tree", bind.tree_name},
            {"paths", bind.logical_paths},
            {"catalog_mode", bind.catalog_mode},
            {"update_mode", bind.update_mode},
            {"allow_partial", bind.allow_partial},
            {"bloom_bytes", options.bloom_bytes},
            {"bloom_false_positive_rate", options.bloom_false_positive_rate},
            {"reader_mode", static_cast<int>(options.root_access.reader_mode)},
            {"validation_entries", options.root_access.validation_entries},
            {"max_entry_bytes", options.root_access.max_entry_bytes},
            {"max_values_per_entry", options.root_access.max_values_per_entry}};
}

bool ReadMatchingRequest(const fs::path& candidate, const nlohmann::json& identity, std::string& snapshot_id)
{
    if (fs::is_symlink(candidate) || !fs::is_directory(candidate))
    {
        return false;
    }
    std::ifstream input(candidate / "request.json", std::ios::binary);
    if (!input)
    {
        return false;
    }
    try
    {
        const auto request = nlohmann::json::parse(input);
        if (!request.is_object() || !request.contains("identity") || request.at("identity") != identity ||
            !request.contains("snapshot_id") || !request["snapshot_id"].is_string())
        {
            return false;
        }
        snapshot_id = request["snapshot_id"].get<std::string>();
        return !snapshot_id.empty() && std::all_of(snapshot_id.begin(), snapshot_id.end(),
                                                   [](unsigned char ch)
                                                   {
                                                       return std::isdigit(ch) != 0;
                                                   });
    }
    catch (...)
    {
        return false;
    }
}

fs::path FindResumeCandidate(const fs::path& staging_root, const nlohmann::json& identity, std::string& snapshot_id)
{
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(staging_root))
    {
        if (entry.path().filename().string().rfind(".staging-", 0) == 0)
        {
            candidates.push_back(entry.path());
        }
    }
    const auto failed = staging_root / "failed";
    if (fs::is_directory(failed))
    {
        for (const auto& entry : fs::directory_iterator(failed))
        {
            candidates.push_back(entry.path());
        }
    }
    // A crash can happen after the snapshot directory is renamed but before
    // current.json is atomically updated. An unreferenced snapshot is staging.
    std::string published_id;
    {
        std::ifstream current(staging_root / "current.json", std::ios::binary);
        if (current)
        {
            try
            {
                published_id = nlohmann::json::parse(current).value("snapshot_id", "");
            }
            catch (...)
            {
                throw IOException("ROOT index current.json is invalid; refusing unsafe resumption");
            }
        }
    }
    const auto snapshots = staging_root / "snapshots";
    if (fs::is_directory(snapshots))
    {
        for (const auto& entry : fs::directory_iterator(snapshots))
        {
            // Historical snapshots are immutable even if they are not the
            // current snapshot. Only a newer, unreferenced snapshot can be an
            // orphan of a failed current.json commit.
            const auto id = entry.path().filename().string();
            if (!published_id.empty() && id > published_id &&
                std::all_of(id.begin(), id.end(),
                            [](unsigned char ch)
                            {
                                return std::isdigit(ch) != 0;
                            }))
            {
                candidates.push_back(entry.path());
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& left, const fs::path& right)
              {
                  return left.string() > right.string();
              });
    for (const auto& candidate : candidates)
    {
        std::string candidate_id;
        if (ReadMatchingRequest(candidate, identity, candidate_id))
        {
            snapshot_id = std::move(candidate_id);
            return candidate;
        }
    }
    return {};
}

void WriteResumeRequest(const fs::path& staging, const nlohmann::json& identity, const std::string& snapshot_id)
{
    std::ofstream output(staging / "request.json", std::ios::binary);
    output << nlohmann::json({{"identity", identity}, {"snapshot_id", snapshot_id}}).dump(2) << '\n';
    if (!output)
    {
        throw IOException("Cannot write ROOT index recovery request");
    }
}

void QuarantineIncompleteCheckpoint(const fs::path& segment)
{
    const auto rejected = segment.parent_path() / (".rejected-" + segment.filename().string());
    if (fs::exists(rejected))
    {
        throw IOException("Cannot quarantine duplicate failed ROOT metadata segment: " + rejected.string());
    }
    fs::rename(segment, rejected);
}

// Only restore a file after validating the immutable checkpoint receipt and
// every Parquet table. A partially written batch must never enter compaction.
void RestoreCheckpoints(DatabaseInstance& database, const fs::path& staging, const std::string& dataset_id,
                        const std::string& snapshot_id, const std::vector<RootIndexFilePlan>& plans,
                        BuildIndexGlobalState& state)
{
    const auto parts = staging / "parts";
    std::unordered_map<std::string, idx_t> by_path;
    for (idx_t i = 0; i < plans.size(); ++i)
    {
        by_path.emplace(plans[i].path, i);
    }
    Connection connection(database);
    if (!fs::is_directory(parts))
    {
        return;
    }
    for (const auto& worker : fs::directory_iterator(parts))
    {
        if (!worker.is_directory() || fs::is_symlink(worker.path()))
        {
            continue;
        }
        std::vector<fs::path> segments;
        for (const auto& entry : fs::directory_iterator(worker.path()))
        {
            if (entry.is_directory() && !fs::is_symlink(entry.path()) &&
                entry.path().filename().string().rfind("segment-", 0) == 0)
            {
                segments.push_back(entry.path());
            }
        }
        std::sort(segments.begin(), segments.end());
        for (const auto& segment : segments)
        {
            std::vector<std::pair<idx_t, RootIndexBuildStatus>> recovered;
            bool valid = true;
            try
            {
                std::ifstream file(segment / "checkpoint.json", std::ios::binary);
                if (!file)
                {
                    valid = false;
                }
                else
                {
                    const auto receipt = nlohmann::json::parse(file);
                    if (receipt.value("format", "") != "root4duckdb-segment-v1" || !receipt.contains("files") ||
                        !receipt["files"].is_array() || receipt["files"].empty() || !receipt.contains("checksums") ||
                        !receipt["checksums"].is_object())
                    {
                        valid = false;
                    }
                    if (valid)
                    {
                        for (const auto& row : receipt["files"])
                        {
                            const auto path = row.at("file_path").get<std::string>();
                            const auto id = row.at("file_id").get<std::string>();
                            const auto it = by_path.find(path);
                            if (it == by_path.end() || !plans[it->second].error.empty() ||
                                state.statuses[it->second].status == "OK" ||
                                row.at("snapshot_id").get<std::string>() != snapshot_id ||
                                row.at("event_base").get<uint64_t>() != plans[it->second].event_base ||
                                row.at("entries").get<uint64_t>() != plans[it->second].entries ||
                                id != FileId(path, row.at("file_size").get<uint64_t>(),
                                             row.at("mtime_ns").get<int64_t>()) ||
                                row.at("file_size").get<uint64_t>() != LocalFileSize(path) ||
                                row.at("mtime_ns").get<int64_t>() != LocalMtimeNS(path))
                            {
                                valid = false;
                                break;
                            }
                            RootIndexBuildStatus status;
                            status.file_path = path;
                            status.file_id = id;
                            status.schema_id = row.at("schema_id").get<std::string>();
                            status.entries = row.at("entries").get<uint64_t>();
                            status.flattened_values = row.at("flattened_values").get<uint64_t>();
                            status.baskets = row.at("baskets").get<uint64_t>();
                            status.status = "OK";
                            status.message = "recovered validated Parquet checkpoint";
                            recovered.emplace_back(it->second, std::move(status));
                        }
                    }
                }
                for (const auto& table : {"root_files.parquet", "root_schemas.parquet", "root_access_levels.parquet",
                                          "root_baskets.parquet"})
                {
                    if (!valid)
                    {
                        break;
                    }
                    const auto filename = segment / table;
                    if (!fs::is_regular_file(filename))
                    {
                        valid = false;
                        break;
                    }
                    std::ifstream receipt_file(segment / "checkpoint.json", std::ios::binary);
                    const auto recorded = nlohmann::json::parse(receipt_file);
                    if (!recorded["checksums"].contains(table) ||
                        recorded["checksums"][table].get<std::string>() != FileContentFingerprint(filename.string()))
                    {
                        valid = false;
                        break;
                    }
                    auto check =
                        connection.Query("SELECT count(*) FROM read_parquet(" + SqlLiteral(filename.string()) + ")");
                    if (!check || check->HasError())
                    {
                        valid = false;
                    }
                }
                if (valid)
                {
                    // Match all file IDs and lineage against actual Parquet rows.
                    auto files = connection.Query(
                        "SELECT DISTINCT file_id, root_uri, event_base, total_entries, snapshot_id, dataset_id "
                        "FROM read_parquet(" +
                        SqlLiteral((segment / "root_files.parquet").string()) + ")");
                    if (!files || files->HasError())
                    {
                        valid = false;
                    }
                    else
                    {
                        std::set<std::string> matched;
                        while (auto chunk = files->Fetch())
                        {
                            for (idx_t i = 0; i < chunk->size(); ++i)
                            {
                                const auto id = chunk->GetValue(0, i).ToString();
                                const auto uri = chunk->GetValue(1, i).ToString();
                                const auto pos =
                                    std::find_if(recovered.begin(), recovered.end(),
                                                 [&](const auto& item)
                                                 {
                                                     return item.second.file_id == id && item.second.file_path == uri;
                                                 });
                                if (pos == recovered.end() ||
                                    chunk->GetValue(2, i).GetValue<uint64_t>() != plans[pos->first].event_base ||
                                    chunk->GetValue(3, i).GetValue<uint64_t>() != pos->second.entries ||
                                    chunk->GetValue(4, i).ToString() != snapshot_id ||
                                    chunk->GetValue(5, i).ToString() != dataset_id)
                                {
                                    valid = false;
                                    break;
                                }
                                matched.insert(id);
                            }
                        }
                        valid = valid && matched.size() == recovered.size();
                    }
                }
            }
            catch (...)
            {
                valid = false;
            }
            if (!valid)
            {
                QuarantineIncompleteCheckpoint(segment);
                continue;
            }
            for (auto& item : recovered)
            {
                state.statuses[item.first] = std::move(item.second);
            }
        }
    }
}

idx_t NextCheckpointWorkerId(const fs::path& parts)
{
    idx_t result = 0;
    if (!fs::is_directory(parts))
    {
        return result;
    }
    for (const auto& entry : fs::directory_iterator(parts))
    {
        const auto name = entry.path().filename().string();
        if (name.rfind("worker-", 0) != 0)
        {
            continue;
        }
        try
        {
            result = std::max(result, static_cast<idx_t>(std::stoull(name.substr(7)) + 1));
        }
        catch (...)
        {
            continue;
        }
    }
    return result;
}

} // namespace

idx_t BuildIndexGlobalState::MaxThreads() const
{
    return 1;
}

RootIndexBuildOptions RootIndexCoordinator::ConfigureRuntime(ClientContext& context, BuildIndexBindData& bind,
                                                             BuildIndexGlobalState& state,
                                                             const std::vector<std::string>& files) const
{
    const auto runtime = RootRuntimeSettings::From(context, files.size());
    if (!bind.has_index_threads)
    {
        bind.index_threads = static_cast<uint32_t>(runtime.threads);
    }
    if (!bind.has_max_in_flight_files)
    {
        bind.max_in_flight_files = static_cast<uint32_t>(runtime.max_in_flight_files);
    }
    if (!bind.has_memory_budget_bytes)
    {
        bind.memory_budget_bytes = runtime.memory_limit_bytes;
    }
    if (!bind.has_estimated_worker_bytes)
    {
        bind.estimated_worker_bytes = runtime.estimated_worker_bytes;
    }
    if (!bind.has_bloom_bytes)
    {
        bind.bloom_bytes = runtime.bloom_bytes;
    }
    RootIndexBuildOptions options;
    options.tree_name = bind.tree_name;
    options.logical_paths = bind.logical_paths;
    options.bloom_bytes = bind.bloom_bytes;
    options.bloom_false_positive_rate = bind.bloom_false_positive_rate;
    options.root_access = bind.root_access;
    options.root_access.operation = "index";
    if (state.manifest_fingerprint.empty())
    {
        state.manifest_fingerprint = ManifestFingerprint(files);
    }
    if (state.dictionary_fingerprint.empty())
    {
        state.dictionary_fingerprint = FileContentFingerprint(bind.dictionary);
    }
    return options;
}

std::string RootIndexCoordinator::DatasetId(const BuildIndexBindData& bind, const std::vector<std::string>& files) const
{
    const auto root_class = ParsePath(bind.logical_paths.front()).root_class;
    uint64_t hash = FNV1a64(root_class, FNV1a64(bind.tree_name));
    for (const auto& file : files)
    {
        hash = FNV1a64(file, hash);
    }
    return Hex64(hash);
}

idx_t RootIndexCoordinator::WorkerCount(BuildIndexBindData& bind, BuildIndexGlobalState& state, idx_t file_count) const
{
    const uint32_t hardware_threads = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    state.requested_threads = bind.index_threads;
    const uint32_t requested_threads = bind.index_threads ? bind.index_threads : hardware_threads;
    const uint32_t in_flight_cap = bind.max_in_flight_files ? bind.max_in_flight_files : hardware_threads;
    const uint32_t memory_cap =
        bind.memory_budget_bytes
            ? std::max<uint32_t>(1, static_cast<uint32_t>(bind.memory_budget_bytes / bind.estimated_worker_bytes))
            : hardware_threads;
    const idx_t result = std::max<idx_t>(
        1, std::min<idx_t>(file_count, std::min<uint32_t>(
                                           requested_threads,
                                           std::min<uint32_t>(hardware_threads, std::min(in_flight_cap, memory_cap)))));
    state.effective_threads = static_cast<uint32_t>(result);
    if (!bind.has_metadata_flush_bytes)
    {
        const auto per_worker =
            bind.memory_budget_bytes
                ? bind.memory_budget_bytes / std::max<uint64_t>(1, static_cast<uint64_t>(result) * 8ULL)
                : 128ULL * 1024ULL * 1024ULL;
        bind.metadata_flush_bytes =
            std::clamp<uint64_t>(per_worker, 16ULL * 1024ULL * 1024ULL, 256ULL * 1024ULL * 1024ULL);
    }
    return result;
}

std::vector<RootIndexFilePlan> RootIndexCoordinator::InspectFiles(const RootIndexBuildOptions& options,
                                                                  const std::vector<std::string>& files,
                                                                  idx_t thread_count, const BuildIndexBindData& bind,
                                                                  BuildIndexGlobalState& state,
                                                                  const fs::path& failure_directory) const
{
    std::vector<RootIndexFilePlan> plans(files.size());
    std::atomic<idx_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (idx_t worker = 0; worker < thread_count; ++worker)
    {
        workers.emplace_back(
            [&]()
            {
                while (true)
                {
                    const auto index = next.fetch_add(1);
                    if (index >= files.size())
                    {
                        break;
                    }
                    plans[index] = InspectRootIndexFile(options, files[index]);
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    uint64_t event_base = 0;
    state.statuses.resize(files.size());
    bool failed = false;
    for (idx_t index = 0; index < plans.size(); ++index)
    {
        plans[index].event_base = event_base;
        event_base += plans[index].entries;
        if (plans[index].error.empty())
        {
            continue;
        }
        failed = true;
        state.statuses[index].file_path = plans[index].path;
        state.statuses[index].entries = plans[index].entries;
        state.statuses[index].status = "ERROR";
        state.statuses[index].message = plans[index].error;
    }
    if (failed && !bind.allow_partial)
    {
        WriteFailureReport(failure_directory, state.snapshot_id, state.statuses);
        throw IOException("ROOT index preflight failed; see failed-" + state.snapshot_id + ".csv");
    }
    return plans;
}

void RootIndexCoordinator::BuildFiles(ClientContext& context, const RootIndexBuildOptions& options,
                                      const std::string& dataset_id, const fs::path& staging,
                                      const std::vector<RootIndexFilePlan>& plans, idx_t thread_count,
                                      const BuildIndexBindData& bind, BuildIndexGlobalState& state) const
{
    std::atomic<idx_t> next{0};
    const auto worker_base = NextCheckpointWorkerId(staging / "parts");
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (idx_t worker = 0; worker < thread_count; ++worker)
    {
        workers.emplace_back(
            [&, worker]()
            {
                const RootIndexFileBuilder file_builder(options, dataset_id, state.snapshot_id);
                std::vector<idx_t> staged_indices;
                std::set<std::string> written_columns;
                bool writer_failed = false;
                std::string writer_error;
                std::unique_ptr<RootIndexMetadataWriter> writer;
                try
                {
                    writer = std::make_unique<RootIndexMetadataWriter>(*context.db, (staging / "parts").string(),
                                                                       worker_base + worker, bind.metadata_flush_bytes);
                }
                catch (const std::exception& exception)
                {
                    writer_failed = true;
                    writer_error = exception.what();
                }
                catch (...)
                {
                    writer_failed = true;
                    writer_error = "unknown typed Parquet writer initialization failure";
                }
                while (!writer_failed)
                {
                    const auto index = next.fetch_add(1);
                    if (index >= plans.size())
                    {
                        break;
                    }
                    if (!plans[index].error.empty() || state.statuses[index].status == "OK")
                    {
                        continue;
                    }
                    try
                    {
                        RootFileIndexMetadata metadata;
                        auto next_written_columns = written_columns;
                        auto status = file_builder.Build(plans[index].path, plans[index].event_base, metadata,
                                                         next_written_columns);
                        if (status.status == "OK")
                        {
                            writer->Append(metadata, status.file_path, status.file_id, status.schema_id, status.entries,
                                           status.flattened_values, status.baskets);
                            written_columns = std::move(next_written_columns);
                            staged_indices.push_back(index);
                        }
                        state.statuses[index] = std::move(status);
                    }
                    catch (const std::exception& exception)
                    {
                        state.statuses[index].file_path = plans[index].path;
                        state.statuses[index].entries = plans[index].entries;
                        state.statuses[index].status = "ERROR";
                        state.statuses[index].message = std::string("typed Parquet writer failed: ") + exception.what();
                        writer_failed = true;
                        writer_error = exception.what();
                    }
                    catch (...)
                    {
                        state.statuses[index].file_path = plans[index].path;
                        state.statuses[index].entries = plans[index].entries;
                        state.statuses[index].status = "ERROR";
                        state.statuses[index].message = "unknown typed Parquet writer failure";
                        writer_failed = true;
                        writer_error = "unknown failure";
                    }
                }
                if (!writer_failed)
                {
                    try
                    {
                        writer->Finish();
                    }
                    catch (const std::exception& exception)
                    {
                        writer_failed = true;
                        writer_error = exception.what();
                    }
                }
                if (!writer_failed)
                {
                    return;
                }
                // Completed segment directories are crash-safe checkpoints.
                // Never discard them on a later writer/worker failure.
                for (const auto index : staged_indices)
                {
                    state.statuses[index].status = "ERROR";
                    state.statuses[index].message = "worker Parquet batch discarded: " + writer_error;
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
}

void RootIndexCoordinator::ValidateBuild(const BuildIndexBindData& bind, const fs::path& failure_directory,
                                         BuildIndexGlobalState& state) const
{
    bool failed = false;
    idx_t success_count = 0;
    for (const auto& status : state.statuses)
    {
        failed |= status.status != "OK";
        if (status.status == "OK")
        {
            ++success_count;
        }
    }
    if (success_count == 0)
    {
        WriteFailureReport(failure_directory, state.snapshot_id, state.statuses);
        throw IOException("No ROOT file was indexed successfully; see failed-" + state.snapshot_id + ".csv");
    }
    if (failed && !bind.allow_partial)
    {
        WriteFailureReport(failure_directory, state.snapshot_id, state.statuses);
        throw IOException("ROOT indexing failed; see failed-" + state.snapshot_id + ".csv");
    }
}

void RootIndexCoordinator::PreserveFailedStaging(const BuildIndexBindData& bind, const fs::path& staging_root,
                                                 const fs::path& staging, const std::string& snapshot_id) const
{
    std::error_code error;
    if (!bind.output_dir.empty() && fs::exists(staging, error))
    {
        const auto failed_root = staging_root / "failed";
        fs::create_directories(failed_root, error);
        const auto failed = failed_root / (snapshot_id + "-" + TimestampId());
        error.clear();
        fs::rename(staging, failed, error);
        return;
    }
    error.clear();
    fs::remove_all(staging, error);
}

unique_ptr<GlobalTableFunctionState> RootIndexCoordinator::Run(ClientContext& context,
                                                               TableFunctionInitInput& input) const
{
    auto& bind = const_cast<BuildIndexBindData&>(input.bind_data->Cast<BuildIndexBindData>());
    auto state = make_uniq<BuildIndexGlobalState>();
    state->snapshot_id = TimestampId();
    state->publish_mode = bind.catalog_mode == "tables" ? bind.publish_mode : bind.catalog_mode;
    state->chunk_id = bind.chunk_id;
    state->manifest_fingerprint = bind.manifest_fingerprint;
    state->dictionary_fingerprint = bind.dictionary_fingerprint;
    LoadRootDictionary(bind.dictionary);
    const auto files = RootInputResolver(context).Resolve(bind.root_glob);
    const auto index_options = ConfigureRuntime(context, bind, *state, files);
    const auto dataset_id = DatasetId(bind, files);
    fs::path staging_root;
    if (!bind.output_dir.empty())
    {
        fs::create_directories(bind.output_dir);
        staging_root = fs::path(bind.output_dir);
    }
    else
    {
        staging_root = fs::temp_directory_path() / "root4duckdb-catalog-staging";
        fs::create_directories(staging_root);
    }
    // A stable output directory is the transactional boundary for resumption
    // and publication. The advisory lock automatically releases on SIGKILL.
    std::unique_ptr<IndexBuildLock> build_lock;
    if (!bind.output_dir.empty())
    {
        build_lock = std::make_unique<IndexBuildLock>(staging_root);
    }
    const auto identity = IndexBuildIdentity(bind, index_options, files, dataset_id);
    fs::path staging = staging_root / (".staging-" + state->snapshot_id);
    if (bind.resume)
    {
        std::string recovered_id;
        const auto recoverable = FindResumeCandidate(staging_root, identity, recovered_id);
        if (!recoverable.empty())
        {
            state->snapshot_id = recovered_id;
            staging = staging_root / (".staging-" + recovered_id);
            if (recoverable != staging)
            {
                if (fs::exists(staging))
                {
                    throw IOException("Cannot restore ROOT index checkpoint over existing staging");
                }
                fs::rename(recoverable, staging);
            }
        }
    }
    if (fs::exists(staging))
    {
        std::string checked_id;
        if (!bind.resume || !ReadMatchingRequest(staging, identity, checked_id) || checked_id != state->snapshot_id)
        {
            throw IOException("ROOT index staging exists but cannot be safely resumed: " + staging.string());
        }
    }
    else
    {
        fs::create_directories(staging / "parts");
        WriteResumeRequest(staging, identity, state->snapshot_id);
    }
    try
    {
        const auto thread_count = WorkerCount(bind, *state, files.size());
        const auto plans = InspectFiles(index_options, files, thread_count, bind, *state, staging_root);
        if (bind.resume)
        {
            RestoreCheckpoints(*context.db, staging, dataset_id, state->snapshot_id, plans, *state);
        }
        PrepareRootIcebergPathSources(context, bind, *state, dataset_id, plans, staging);
        BuildFiles(context, index_options, dataset_id, staging, plans, thread_count, bind, *state);
        ValidateBuild(bind, staging_root, *state);
        if (const char* failpoint = std::getenv("ROOT4DUCKDB_TEST_FAIL_AFTER_INDEX"))
        {
            if (std::string(failpoint) == "1")
            {
                throw IOException("ROOT index test interruption after durable checkpoints");
            }
        }
        CompactRootIndexParquet(*context.db, staging.string());
        // Retain committed parts until current.json/Iceberg publication is
        // complete; a crash during publication must remain resumable.
        RootIndexPublisher().Publish(context, bind, *state, dataset_id, staging);
        if (bind.catalog_mode == "local" || bind.catalog_mode == "external")
        {
            std::error_code cleanup_error;
            fs::remove_all(fs::path(state->snapshot_dir) / "parts", cleanup_error);
        }
    }
    catch (...)
    {
        PreserveFailedStaging(bind, staging_root, staging, state->snapshot_id);
        throw;
    }
    return state;
}

} // namespace duckdb::rootlake
