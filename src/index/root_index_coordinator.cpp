#include "root4duckdb/index/root_index_pipeline.hpp"
#include "root4duckdb/index/root_index_pipeline_utils.hpp"
namespace duckdb::rootlake {

idx_t BuildIndexGlobalState::MaxThreads() const {
    return 1;
}

RootIndexBuildOptions RootIndexCoordinator::ConfigureRuntime(ClientContext& context, BuildIndexBindData& bind,
                                                             BuildIndexGlobalState& state,
                                                             const std::vector<std::string>& files) const {
    const auto runtime = RootRuntimeSettings::From(context, files.size());
    if (!bind.has_index_threads) {
        bind.index_threads = static_cast<uint32_t>(runtime.threads);
    }
    if (!bind.has_max_in_flight_files) {
        bind.max_in_flight_files = static_cast<uint32_t>(runtime.max_in_flight_files);
    }
    if (!bind.has_memory_budget_bytes) {
        bind.memory_budget_bytes = runtime.memory_limit_bytes;
    }
    if (!bind.has_estimated_worker_bytes) {
        bind.estimated_worker_bytes = runtime.estimated_worker_bytes;
    }
    if (!bind.has_bloom_bytes) {
        bind.bloom_bytes = runtime.bloom_bytes;
    }

    RootIndexBuildOptions options;
    options.tree_name = bind.tree_name;
    options.logical_paths = bind.logical_paths;
    options.bloom_bytes = bind.bloom_bytes;
    options.bloom_false_positive_rate = bind.bloom_false_positive_rate;
    options.root_access = bind.root_access;
    options.root_access.operation = "index";

    if (state.manifest_fingerprint.empty()) {
        state.manifest_fingerprint = ManifestFingerprint(files);
    }
    if (state.dictionary_fingerprint.empty()) {
        state.dictionary_fingerprint = FileContentFingerprint(bind.dictionary);
    }
    return options;
}

std::string RootIndexCoordinator::DatasetId(const BuildIndexBindData& bind,
                                            const std::vector<std::string>& files) const {
    const auto root_class = ParsePath(bind.logical_paths.front()).root_class;
    uint64_t hash = FNV1a64(root_class, FNV1a64(bind.tree_name));
    for (const auto& file : files) {
        hash = FNV1a64(file, hash);
    }
    return Hex64(hash);
}

idx_t RootIndexCoordinator::WorkerCount(BuildIndexBindData& bind, BuildIndexGlobalState& state,
                                        idx_t file_count) const {
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
    if (!bind.has_metadata_flush_bytes) {
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
                                                                  const fs::path& failure_directory) const {
    std::vector<RootIndexFilePlan> plans(files.size());
    std::atomic<idx_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (idx_t worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&]() {
            while (true) {
                const auto index = next.fetch_add(1);
                if (index >= files.size()) {
                    break;
                }
                plans[index] = InspectRootIndexFile(options, files[index]);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    uint64_t event_base = 0;
    state.statuses.resize(files.size());
    bool failed = false;
    for (idx_t index = 0; index < plans.size(); ++index) {
        plans[index].event_base = event_base;
        event_base += plans[index].entries;
        if (plans[index].error.empty()) {
            continue;
        }
        failed = true;
        state.statuses[index].file_path = plans[index].path;
        state.statuses[index].entries = plans[index].entries;
        state.statuses[index].status = "ERROR";
        state.statuses[index].message = plans[index].error;
    }
    if (failed && !bind.allow_partial) {
        WriteFailureReport(failure_directory, state.snapshot_id, state.statuses);
        throw IOException("ROOT index preflight failed; see failed-" + state.snapshot_id + ".csv");
    }
    return plans;
}

void RootIndexCoordinator::BuildFiles(ClientContext& context, const RootIndexBuildOptions& options,
                                      const std::string& dataset_id, const fs::path& staging,
                                      const std::vector<RootIndexFilePlan>& plans, idx_t thread_count,
                                      const BuildIndexBindData& bind, BuildIndexGlobalState& state) const {
    std::atomic<idx_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (idx_t worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&, worker]() {
            const auto worker_part = staging / "parts" / ("worker-" + std::to_string(worker));
            RootIndexFileBuilder file_builder(options, dataset_id, state.snapshot_id);
            std::vector<idx_t> staged_indices;
            std::set<std::string> written_columns;
            bool writer_failed = false;
            std::string writer_error;
            std::unique_ptr<RootIndexMetadataWriter> writer;
            try {
                writer = std::make_unique<RootIndexMetadataWriter>(*context.db, (staging / "parts").string(), worker,
                                                                   bind.metadata_flush_bytes);
            } catch (const std::exception& exception) {
                writer_failed = true;
                writer_error = exception.what();
            } catch (...) {
                writer_failed = true;
                writer_error = "unknown typed Parquet writer initialization failure";
            }
            while (!writer_failed) {
                const auto index = next.fetch_add(1);
                if (index >= plans.size()) {
                    break;
                }
                if (!plans[index].error.empty()) {
                    continue;
                }
                try {
                    RootFileIndexMetadata metadata;
                    auto next_written_columns = written_columns;
                    auto status =
                        file_builder.Build(plans[index].path, plans[index].event_base, metadata, next_written_columns);
                    if (status.status == "OK") {
                        writer->Append(metadata);
                        written_columns = std::move(next_written_columns);
                        staged_indices.push_back(index);
                    }
                    state.statuses[index] = std::move(status);
                } catch (const std::exception& exception) {
                    state.statuses[index].file_path = plans[index].path;
                    state.statuses[index].entries = plans[index].entries;
                    state.statuses[index].status = "ERROR";
                    state.statuses[index].message = std::string("typed Parquet writer failed: ") + exception.what();
                    writer_failed = true;
                    writer_error = exception.what();
                } catch (...) {
                    state.statuses[index].file_path = plans[index].path;
                    state.statuses[index].entries = plans[index].entries;
                    state.statuses[index].status = "ERROR";
                    state.statuses[index].message = "unknown typed Parquet writer failure";
                    writer_failed = true;
                    writer_error = "unknown failure";
                }
            }
            if (!writer_failed) {
                try {
                    writer->Finish();
                } catch (const std::exception& exception) {
                    writer_failed = true;
                    writer_error = exception.what();
                }
            }
            if (!writer_failed) {
                return;
            }
            std::error_code error;
            fs::remove_all(worker_part, error);
            for (const auto index : staged_indices) {
                state.statuses[index].status = "ERROR";
                state.statuses[index].message = "worker Parquet batch discarded: " + writer_error;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

void RootIndexCoordinator::ValidateBuild(const BuildIndexBindData& bind, const fs::path& failure_directory,
                                         BuildIndexGlobalState& state) const {
    bool failed = false;
    idx_t success_count = 0;
    for (const auto& status : state.statuses) {
        failed |= status.status != "OK";
        success_count += status.status == "OK" ? 1 : 0;
    }
    if (success_count == 0) {
        WriteFailureReport(failure_directory, state.snapshot_id, state.statuses);
        throw IOException("No ROOT file was indexed successfully; see failed-" + state.snapshot_id + ".csv");
    }
    if (failed && !bind.allow_partial) {
        WriteFailureReport(failure_directory, state.snapshot_id, state.statuses);
        throw IOException("ROOT indexing failed; see failed-" + state.snapshot_id + ".csv");
    }
}

void RootIndexCoordinator::PreserveFailedStaging(const BuildIndexBindData& bind, const fs::path& staging_root,
                                                 const fs::path& staging, const std::string& snapshot_id) const {
    std::error_code error;
    if (!bind.output_dir.empty() && fs::exists(staging, error)) {
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

unique_ptr<GlobalTableFunctionState> RootIndexCoordinator::Run(ClientContext& context, TableFunctionInitInput& input) {
    auto& bind = const_cast<BuildIndexBindData&>(input.bind_data->Cast<BuildIndexBindData>());
    auto state = make_uniq<BuildIndexGlobalState>();
    state->snapshot_id = TimestampId();
    state->publish_mode = bind.catalog_mode == "tables" ? bind.publish_mode : bind.catalog_mode;
    state->chunk_id = bind.chunk_id;
    state->manifest_fingerprint = bind.manifest_fingerprint;
    state->dictionary_fingerprint = bind.dictionary_fingerprint;

    LoadRootDictionary(bind.dictionary);
    const auto files = ResolveRootInputs(context, bind.root_glob);
    const auto index_options = ConfigureRuntime(context, bind, *state, files);
    const auto dataset_id = DatasetId(bind, files);
    fs::path staging_root;
    if (!bind.output_dir.empty()) {
        fs::create_directories(bind.output_dir);
        staging_root = fs::path(bind.output_dir);
    } else {
        staging_root = fs::temp_directory_path() / "root4duckdb-catalog-staging";
        fs::create_directories(staging_root);
    }
    const fs::path staging = staging_root / (".staging-" + state->snapshot_id);
    if (fs::exists(staging)) {
        fs::remove_all(staging);
    }
    fs::create_directories(staging / "parts");

    try {
        const auto thread_count = WorkerCount(bind, *state, files.size());
        const auto plans = InspectFiles(index_options, files, thread_count, bind, *state, staging_root);

        BuildFiles(context, index_options, dataset_id, staging, plans, thread_count, bind, *state);
        ValidateBuild(bind, staging_root, *state);

        CompactRootIndexParquet(*context.db, staging.string());
        fs::remove_all(staging / "parts");
        RootIndexPublisher().Publish(context, bind, *state, dataset_id, staging);
    } catch (...) {
        PreserveFailedStaging(bind, staging_root, staging, state->snapshot_id);
        throw;
    }
    return std::move(state);
}

} // namespace duckdb::rootlake
