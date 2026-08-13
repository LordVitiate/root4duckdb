#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb {

bool RootEntryScheduler::WorkBatch::HasWork() const {
    return start < end;
}

RootEntryScheduler::RootEntryScheduler(uint64_t& next_row, uint64_t total_rows, std::mutex& mutex)
    : next_row_(next_row), total_rows_(total_rows), mutex_(mutex) {
}

RootEntryScheduler::WorkBatch RootEntryScheduler::ClaimWork(uint64_t preferred_batch_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_row_ >= total_rows_) {
        return {0, 0};
    }

    WorkBatch batch;
    batch.start = next_row_;
    batch.end = std::min(next_row_ + preferred_batch_size, total_rows_);
    next_row_ = batch.end;
    return batch;
}

idx_t RootEntryScheduler::EstimateOptimalThreads(uint64_t total_rows) {
    constexpr uint64_t ROWS_PER_THREAD = 500000;
    const idx_t threads = static_cast<idx_t>(total_rows / ROWS_PER_THREAD);
    return std::max<idx_t>(1, threads);
}

bool RootScanBindData::IsMultiFile() const {
    return root_paths.size() > 1;
}

RootScanBindData::~RootScanBindData() {
    RootDebug("BIND_DATA.DTOR_BODY",
              "this=" + RootPointer(this) + " root_path=" + root_path + " columns=" + std::to_string(columns.size()));
}

idx_t RootScanGlobalState::MaxThreads() const {
    if (histogram_mode) {
        return 1;
    }
    if (file_scheduler) {
        return file_scheduler->MaxThreads();
    }
    return RootEntryScheduler::EstimateOptimalThreads(scheduled_rows);
}

RootScanLocalState::~RootScanLocalState() = default;

unique_ptr<GlobalTableFunctionState> RootScanStateFactory::CreateGlobal(ClientContext& context,
                                                                        TableFunctionInitInput& input) {
    auto& bind_data = input.bind_data->Cast<RootScanBindData>();
    auto global_state = make_uniq<RootScanGlobalState>();

    global_state->browse_offset = 0;
    global_state->histogram_mode = bind_data.is_histogram_mode;
    if (input.filters) {
        global_state->filters = input.filters->Copy();
    }
    global_state->scan_column_ids = input.column_ids;
    if (input.projection_ids.empty()) {
        global_state->output_column_ids = input.column_ids;
    } else {
        global_state->output_column_ids.reserve(input.projection_ids.size());
        for (const auto projection_id : input.projection_ids) {
            if (projection_id >= input.column_ids.size()) {
                throw InternalException("Invalid read_root projection id");
            }
            global_state->output_column_ids.push_back(input.column_ids[projection_id]);
        }
    }
    global_state->total_rows = bind_data.total_rows;
    global_state->next_row = 0;
    if (bind_data.IsMultiFile() && !bind_data.is_browse_mode && !bind_data.is_empty_mode &&
        !bind_data.is_histogram_mode) {
        const auto runtime = rootlake::RootRuntimeSettings::From(context, bind_data.root_paths.size());
        global_state->file_scheduler =
            make_uniq<rootlake::RootDirectFileScheduler>(bind_data.root_paths, runtime.threads);
    }
    if (!bind_data.is_browse_mode && !bind_data.is_histogram_mode && global_state->filters) {
        for (const auto& entry : global_state->filters->filters) {
            if (entry.first >= global_state->scan_column_ids.size()) {
                continue;
            }
            const auto full_column = global_state->scan_column_ids[entry.first];
            if (full_column == bind_data.source_id_column && global_state->file_scheduler) {
                const auto source_range = rootlake::ExtractRootUnsignedRange(*entry.second);
                if (!source_range.known) {
                    continue;
                }
                if (source_range.impossible) {
                    global_state->event_range_impossible = true;
                    break;
                }
                global_state->file_scheduler->SetSourceRange(source_range.lower, source_range.upper);
                continue;
            }
            if (full_column != 0 && full_column != COLUMN_IDENTIFIER_ROW_ID) {
                continue;
            }
            const auto range = rootlake::ExtractRootUnsignedRange(*entry.second);
            if (!range.known) {
                continue;
            }
            if (range.impossible) {
                global_state->event_range_impossible = true;
                if (!bind_data.IsMultiFile()) {
                    global_state->next_row = bind_data.total_rows;
                    global_state->total_rows = bind_data.total_rows;
                }
                break;
            }
            global_state->event_lower = std::max(global_state->event_lower, range.lower);
            if (range.upper != std::numeric_limits<uint64_t>::max()) {
                global_state->event_upper = std::min(global_state->event_upper, range.upper);
            }
            if (!bind_data.IsMultiFile()) {
                global_state->next_row = std::max(global_state->next_row, range.lower);
                if (range.upper != std::numeric_limits<uint64_t>::max()) {
                    global_state->total_rows = std::min(global_state->total_rows, range.upper + 1);
                }
            }
        }
    }
    global_state->scheduled_rows =
        global_state->total_rows >= global_state->next_row ? global_state->total_rows - global_state->next_row : 0;
    return std::move(global_state);
}

void RootScanFileManager::Open(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                               RootScanLocalState& target, const std::string& file_path, bool synchronize_open) {
    RootDebugOperationScope debug_operation("RootScanInitLocal");
    RootDebug("INIT_LOCAL.BEGIN", "root_path=" + file_path + " tree=" + bind_data.tree_name +
                                      " columns=" + std::to_string(bind_data.columns.size()));

    auto* local_state = &target;
    auto* open_mutex = synchronize_open ? &gstate.coordination_mutex : nullptr;

    if (bind_data.is_primitive_tree_mode) {
        const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);

        if (gstate.file_scheduler) {
            gstate.file_scheduler->RecordOpen(local_state->file_task, open_result.attempts, open_result.elapsed_us);
        }

        auto* tree = local_state->root_file.GetTTree();

        if (!tree) {
            throw IOException("ROOT schema mismatch in " + file_path + ": primitive TTree '" + bind_data.tree_name +
                              "' is absent");
        }

        local_state->primitive_tree_branches.assign(bind_data.columns.size(), nullptr);

        local_state->primitive_tree_leaves.assign(bind_data.columns.size(), nullptr);

        local_state->primitive_tree_requires_read = false;

        std::set<idx_t> required_columns;

        for (const auto column : gstate.scan_column_ids) {
            if (column != COLUMN_IDENTIFIER_ROW_ID && column < bind_data.columns.size()) {
                required_columns.insert(column);
            }
        }

        for (const auto column : gstate.output_column_ids) {
            if (column != COLUMN_IDENTIFIER_ROW_ID && column < bind_data.columns.size()) {
                required_columns.insert(column);
            }
        }

        std::vector<TBranch*> projected_branches;

        for (const auto column_index : required_columns) {

            if (column_index == 0 || column_index == bind_data.source_id_column ||
                column_index == bind_data.source_path_column) {
                continue;
            }

            const auto& column = bind_data.columns[column_index];

            if (column.branch_name.empty()) {
                continue;
            }

            auto* branch = tree->GetBranch(column.branch_name.c_str());

            if (!branch) {
                throw IOException("ROOT schema mismatch in " + file_path + ": primitive branch '" + column.branch_name +
                                  "' is absent");
            }

            auto* leaf = branch->GetLeaf(branch->GetName());

            if (!leaf) {
                throw IOException("ROOT schema mismatch in " + file_path + ": primitive leaf '" + column.branch_name +
                                  "' is absent");
            }

            if (std::string(leaf->GetTypeName()) != column.root_type) {
                throw IOException("ROOT schema mismatch in " + file_path + ": primitive branch '" + column.branch_name +
                                  "' changed type from " + column.root_type + " to " + leaf->GetTypeName());
            }

            local_state->primitive_tree_branches[column_index] = branch;

            local_state->primitive_tree_leaves[column_index] = leaf;

            projected_branches.push_back(branch);
        }

        if (projected_branches.empty()) {
            tree->SetBranchStatus("*", 0);
        } else {
            const auto projection =
                rootlake::ApplyBranchProjection(tree, projected_branches, bind_data.tree_cache_bytes);

            if (!projection.applied) {
                rootlake::EnableAllBranches(tree, bind_data.tree_cache_bytes);
            }

            local_state->primitive_tree_requires_read = true;
        }

        RootDebug("PRIMITIVE_TREE.PROJECTION",
                  "file=" + file_path + " projected_branches=" + std::to_string(projected_branches.size()));

        if (gstate.file_scheduler) {
            const auto entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));

            local_state->local_current_row =
                gstate.event_range_impossible ? entries : std::min(entries, gstate.event_lower);

            local_state->local_end_row = entries;

            if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
                local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
            }

            local_state->file_active = true;

            std::ostringstream fingerprint;
            fingerprint << "primitive-tree:" << bind_data.tree_name;

            for (const auto& column : bind_data.columns) {
                if (!column.branch_name.empty()) {
                    fingerprint << "|" << column.branch_name << ":" << column.root_type;
                }
            }

            gstate.file_scheduler->ObserveSchema(fingerprint.str());
        }

        return;
    }

    if (bind_data.is_direct_branch_mode) {
        const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
        if (gstate.file_scheduler) {
            gstate.file_scheduler->RecordOpen(local_state->file_task, open_result.attempts, open_result.elapsed_us);
        }
        auto* tree = local_state->root_file.GetTTree();
        if (tree) {
            local_state->direct_branch = tree->GetBranch(bind_data.direct_branch_info.name.c_str());
            if (local_state->direct_branch) {
                local_state->direct_leaf = local_state->direct_branch->GetLeaf(local_state->direct_branch->GetName());
                std::vector<TBranch*> projected_branches{local_state->direct_branch};
                if (local_state->direct_leaf && local_state->direct_leaf->GetLeafCount() &&
                    local_state->direct_leaf->GetLeafCount()->GetBranch()) {
                    projected_branches.push_back(local_state->direct_leaf->GetLeafCount()->GetBranch());
                }
                const auto projection =
                    rootlake::ApplyBranchProjection(tree, projected_branches, bind_data.tree_cache_bytes);
                if (!projection.applied) {
                    rootlake::EnableAllBranches(tree, bind_data.tree_cache_bytes);
                }
            }
        }
        if (!local_state->direct_branch || !local_state->direct_leaf) {
            throw IOException("ROOT schema mismatch in " + file_path + ": primitive branch '" +
                              bind_data.direct_branch_info.name + "' is absent");
        }
        if (gstate.file_scheduler) {
            const auto entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));
            local_state->local_current_row =
                gstate.event_range_impossible ? entries : std::min(entries, gstate.event_lower);
            local_state->local_end_row = entries;
            if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
                local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
            }
            local_state->file_active = true;
            gstate.file_scheduler->ObserveSchema("primitive:" + bind_data.direct_branch_info.type_name);
        }
        return;
    }

    const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
    if (gstate.file_scheduler) {
        gstate.file_scheduler->RecordOpen(local_state->file_task, open_result.attempts, open_result.elapsed_us);
    }
    auto* file = local_state->root_file.GetTFile();
    if (!file || file->IsZombie()) {
        throw IOException("Invalid ROOT file: " + file_path);
    }

    std::set<std::string> unique_root_classes;
    for (idx_t col_idx : gstate.scan_column_ids) {
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size()) {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (!col.branch_name.empty()) {
            unique_root_classes.insert(col.branch_name);
        }
    }

    if (unique_root_classes.empty()) {
        for (const auto& col : bind_data.columns) {
            if (!col.branch_name.empty()) {
                unique_root_classes.insert(col.branch_name);
            }
        }
    }

    for (const auto& root_class_name : unique_root_classes) {
        try {
            rootlake::RootObjectReader reader;
            reader.Bind(file, "", root_class_name, bind_data.dictionary_cleanup_mode);
            local_state->root_readers.emplace(root_class_name, std::move(reader));
        } catch (const std::exception& exception) {
            if (gstate.file_scheduler) {
                throw IOException("ROOT schema mismatch in " + file_path + ": " + exception.what());
            }
        }
    }

    std::vector<idx_t> serialized_candidates;
    for (const auto col_idx : gstate.scan_column_ids) {
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size()) {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (col.is_virtual_index || col.is_string || col.levels.empty() || col.logical_path.empty()) {
            continue;
        }
        if (std::find(serialized_candidates.begin(), serialized_candidates.end(), col_idx) ==
            serialized_candidates.end()) {
            serialized_candidates.push_back(col_idx);
        }
    }
    if (serialized_candidates.empty()) {
        for (idx_t col_idx = 0; col_idx < bind_data.columns.size(); ++col_idx) {
            const auto& col = bind_data.columns[col_idx];
            if (!col.is_virtual_index && !col.is_string && !col.levels.empty() && !col.logical_path.empty()) {
                serialized_candidates.push_back(col_idx);
            }
        }
    }

    if (serialized_candidates.size() == 1) {
        const auto col_idx = serialized_candidates.front();
        const auto& col = bind_data.columns[col_idx];
        auto reader_it = local_state->root_readers.find(col.branch_name);
        if (reader_it != local_state->root_readers.end()) {
            auto& object_reader = reader_it->second;
            auto parsed = rootlake::ParsePath(col.logical_path);
            local_state->path_reader.Resolve(object_reader.Tree(), object_reader.ObjectBranch(),
                                             object_reader.RootClass(), std::move(parsed), col.levels);
            const auto projection =
                local_state->path_reader.PhysicalMode() == "ancestor"
                    ? rootlake::ApplyBranchProjection(object_reader.Tree(), {local_state->path_reader.PhysicalBranch()},
                                                      bind_data.tree_cache_bytes)
                    : rootlake::BranchProjectionResult{};
            if (!projection.applied) {
                rootlake::EnableAllBranches(object_reader.Tree(), bind_data.tree_cache_bytes);
            }
            rootlake::RootPathReaderOptions reader_options;
            reader_options.reader_mode = bind_data.reader_mode;
            reader_options.validation_entries = bind_data.raw_validation_entries;
            reader_options.max_entry_bytes = bind_data.raw_max_entry_bytes;
            reader_options.max_values_per_entry = bind_data.raw_max_values_per_entry;
            reader_options.tree_cache_bytes = bind_data.tree_cache_bytes;
            reader_options.enable_all_branches_on_fallback = true;
            local_state->path_reader.StartSerialized(object_reader.CurrentObject(), std::move(reader_options));
            local_state->serialized_column = col_idx;
        } else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED) {
            throw InvalidInputException("reader_mode='serialized' cannot bind ROOT object context for " +
                                        col.logical_path);
        } else {
            rootlake::WarnRootFallbackOnce(col.logical_path, "unknown", "ROOT object context is unavailable");
        }
    } else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED && serialized_candidates.size() > 1) {
        throw InvalidInputException(
            "reader_mode='serialized' requires exactly one materialized logical ROOT value column");
    }

    for (const auto& col : bind_data.columns) {
        if (col.is_virtual_index) {
            local_state->has_container_columns = true;
            break;
        }
    }

    if (gstate.file_scheduler) {
        uint64_t entries = 0;
        if (!local_state->root_readers.empty()) {
            entries = static_cast<uint64_t>(
                std::max<Long64_t>(0, local_state->root_readers.begin()->second.Tree()->GetEntries()));
        }
        local_state->local_current_row =
            gstate.event_range_impossible ? entries : std::min(entries, gstate.event_lower);
        local_state->local_end_row = entries;
        if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
            local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
        }
        local_state->file_active = true;
        const auto fingerprint = local_state->path_reader.SerializedPlan().schema_fingerprint.empty()
                                     ? std::string("object:") + bind_data.tree_name
                                     : local_state->path_reader.SerializedPlan().schema_fingerprint;
        gstate.file_scheduler->ObserveSchema(fingerprint);
    }
}

unique_ptr<LocalTableFunctionState> RootScanStateFactory::CreateLocal(TableFunctionInitInput& input,
                                                                      GlobalTableFunctionState* global_state_p) {
    auto& bind_data = input.bind_data->Cast<RootScanBindData>();
    auto& gstate = global_state_p->Cast<RootScanGlobalState>();
    auto local_state = make_uniq<RootScanLocalState>();
    if (!bind_data.is_empty_mode && !bind_data.is_browse_mode && !bind_data.is_histogram_mode &&
        !bind_data.IsMultiFile()) {
        RootScanFileManager().Open(bind_data, gstate, *local_state, bind_data.root_path, true);
    }
    return std::move(local_state);
}

void RootScanFileManager::Reset(RootScanGlobalState& gstate, RootScanLocalState& local_state, bool completed) {
    if (completed && local_state.file_active && gstate.file_scheduler) {
        const auto elapsed = std::chrono::steady_clock::now() - local_state.file_started;
        gstate.file_scheduler->RecordComplete(
            local_state.file_task,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    }
    local_state.path_reader.Reset();
    local_state.serialized_column = DConstants::INVALID_INDEX;
    local_state.serialized_values.clear();
    local_state.serialized_indices.clear();
    local_state.reported_serialized_baskets = 0;
    local_state.reported_serialized_compressed_bytes = 0;
    local_state.reported_serialized_entry_bytes = 0;
    local_state.cached_results.clear();
    local_state.has_cached_entry = false;
    local_state.current_entry = std::numeric_limits<uint64_t>::max();
    local_state.current_elem_idx = 0;
    local_state.root_readers.clear();
    local_state.direct_branch = nullptr;
    local_state.direct_leaf = nullptr;
    local_state.primitive_tree_branches.clear();
    local_state.primitive_tree_leaves.clear();
    local_state.primitive_tree_requires_read = false;
    local_state.root_file.Close();
    local_state.local_current_row = 0;
    local_state.local_end_row = 0;
    local_state.has_container_columns = false;
    local_state.file_active = false;
}

bool RootScanFileManager::EnsureReady(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                                      RootScanLocalState& local_state) {
    if (!gstate.file_scheduler) {
        throw InternalException("multi-file ROOT scheduler is unavailable");
    }
    while (true) {
        if (local_state.file_active &&
            (local_state.has_cached_entry || local_state.local_current_row < local_state.local_end_row)) {
            return true;
        }
        if (local_state.file_active) {
            Reset(gstate, local_state, true);
        }

        rootlake::RootDirectFileTask task;
        if (!gstate.file_scheduler->Claim(task)) {
            if (gstate.file_scheduler->AllFilesFinished()) {
                const auto failures = gstate.file_scheduler->FailureSummary();
                if (!failures.empty()) {
                    throw IOException(failures);
                }
            }
            return false;
        }

        local_state.file_task = std::move(task);
        local_state.file_started = std::chrono::steady_clock::now();
        try {
            Open(bind_data, gstate, local_state, local_state.file_task.path, false);
        } catch (const rootlake::RootFileUnavailableException& exception) {
            gstate.file_scheduler->RecordUnavailable(local_state.file_task, exception.attempts, exception.elapsed_us);
            Reset(gstate, local_state, false);
            continue;
        } catch (const std::exception& exception) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, exception.what());
            Reset(gstate, local_state, false);
            continue;
        } catch (...) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, "unknown ROOT reader error");
            Reset(gstate, local_state, false);
            continue;
        }
    }
}

} // namespace duckdb
