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

    if (preferred_batch_size == 0) {
        preferred_batch_size = DEFAULT_BATCH_SIZE;
    }
    WorkBatch batch;
    batch.start = next_row_;
    const auto remaining = total_rows_ - next_row_;
    const auto count = std::min(preferred_batch_size, remaining);
    batch.end = next_row_ + count;
    next_row_ = batch.end;
    return batch;
}

idx_t RootEntryScheduler::EstimateWorkUnits(uint64_t total_rows, uint64_t batch_size) {
    if (total_rows == 0 || batch_size == 0) {
        return 1;
    }
    const auto units = 1 + ((total_rows - 1) / batch_size);
    return static_cast<idx_t>(std::min<uint64_t>(units, std::numeric_limits<idx_t>::max()));
}

bool RootScanBindData::IsMultiFile() const {
    return root_paths.size() > 1;
}

bool RootScanBindData::IsSemanticMode() const noexcept {
    return std::holds_alternative<RootSemanticScanMode>(scan_mode);
}

bool RootScanBindData::IsBrowseMode() const noexcept {
    return std::holds_alternative<RootBrowseScanMode>(scan_mode);
}

bool RootScanBindData::IsDirectBranchMode() const noexcept {
    return std::holds_alternative<RootDirectBranchScanMode>(scan_mode);
}

bool RootScanBindData::IsPrimitiveTreeMode() const noexcept {
    return std::holds_alternative<RootPrimitiveTreeScanMode>(scan_mode);
}

bool RootScanBindData::IsHistogramMode() const noexcept {
    return std::holds_alternative<RootHistogramScanMode>(scan_mode);
}

bool RootScanBindData::IsEmptyMode() const noexcept {
    return std::holds_alternative<RootEmptyScanMode>(scan_mode);
}

const RootBrowseScanMode* RootScanBindData::BrowseMode() const noexcept {
    return std::get_if<RootBrowseScanMode>(&scan_mode);
}

const RootDirectBranchScanMode* RootScanBindData::DirectBranchMode() const noexcept {
    return std::get_if<RootDirectBranchScanMode>(&scan_mode);
}

const RootHistogramScanMode* RootScanBindData::HistogramMode() const noexcept {
    return std::get_if<RootHistogramScanMode>(&scan_mode);
}

void RootScanBindData::SelectSemanticMode() {
    scan_mode.emplace<RootSemanticScanMode>();
}

void RootScanBindData::SelectBrowseMode(std::vector<std::string> children) {
    scan_mode.emplace<RootBrowseScanMode>(RootBrowseScanMode{std::move(children)});
}

void RootScanBindData::SelectDirectBranchMode(RootPrimitiveBranch branch) {
    scan_mode.emplace<RootDirectBranchScanMode>(RootDirectBranchScanMode{std::move(branch)});
}

void RootScanBindData::SelectPrimitiveTreeMode() {
    scan_mode.emplace<RootPrimitiveTreeScanMode>();
}

void RootScanBindData::SelectHistogramMode(rootlake::RootHistogramBinding binding, std::unique_ptr<TH1> object) {
    scan_mode.emplace<RootHistogramScanMode>(RootHistogramScanMode{std::move(binding), std::move(object)});
}

void RootScanBindData::SelectEmptyMode() {
    scan_mode.emplace<RootEmptyScanMode>();
}

RootScanBindData::~RootScanBindData() noexcept {
    try {
        RootDebug("BIND_DATA.DTOR_BODY", "this=" + RootPointer(this) + " root_path=" + root_path +
                                             " columns=" + std::to_string(columns.size()));
    } catch (...) {
        // Diagnostics must not make destruction terminate the process.
    }
}

idx_t RootScanGlobalState::MaxThreads() const {
    return force_single_thread ? 1 : std::max<idx_t>(1, worker_limit);
}

RootScanLocalState::~RootScanLocalState() = default;

unique_ptr<GlobalTableFunctionState> RootScanStateFactory::CreateGlobal(ClientContext& context,
                                                                        TableFunctionInitInput& input) {
    auto& bind_data = input.bind_data->Cast<RootScanBindData>();
    auto global_state = make_uniq<RootScanGlobalState>();

    global_state->browse_offset = 0;
    global_state->force_single_thread = bind_data.IsHistogramMode();
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
    if (!bind_data.IsBrowseMode() && !bind_data.IsHistogramMode() && global_state->filters) {
        for (const auto& entry : global_state->filters->filters) {
            if (entry.first >= global_state->scan_column_ids.size()) {
                continue;
            }
            const auto full_column = global_state->scan_column_ids[entry.first];
            if (full_column == bind_data.source_id_column && bind_data.IsMultiFile()) {
                const auto source_range = rootlake::ExtractRootUnsignedRange(*entry.second);
                if (!source_range.known) {
                    continue;
                }
                if (source_range.impossible) {
                    global_state->entry_range_impossible = true;
                    break;
                }
                global_state->source_lower = std::max(global_state->source_lower, source_range.lower);
                if (source_range.upper != std::numeric_limits<uint64_t>::max()) {
                    global_state->source_upper = std::min(global_state->source_upper, source_range.upper);
                }
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
                global_state->entry_range_impossible = true;
                if (!bind_data.IsMultiFile()) {
                    global_state->next_row = bind_data.total_rows;
                    global_state->total_rows = bind_data.total_rows;
                }
                break;
            }
            global_state->entry_lower = std::max(global_state->entry_lower, range.lower);
            if (range.upper != std::numeric_limits<uint64_t>::max()) {
                global_state->entry_upper = std::min(global_state->entry_upper, range.upper);
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

    if (!bind_data.IsBrowseMode() && !bind_data.IsEmptyMode() && !bind_data.IsHistogramMode()) {
        const idx_t work_units = bind_data.IsMultiFile()
                                     ? std::max<idx_t>(1, bind_data.root_paths.size())
                                     : RootEntryScheduler::EstimateWorkUnits(global_state->scheduled_rows);
        const auto runtime = rootlake::RootRuntimeSettings::From(
            context, std::max<idx_t>(1, bind_data.root_paths.size()), 0, work_units);
        global_state->worker_limit = runtime.threads;
        if (bind_data.IsMultiFile()) {
            global_state->file_scheduler =
                make_uniq<rootlake::RootDirectFileScheduler>(bind_data.root_paths, global_state->worker_limit);
            global_state->file_scheduler->SetSourceRange(global_state->source_lower, global_state->source_upper);
        }
    }
    return std::move(global_state);
}

void RootScanFileManager::Open(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                               RootScanLocalState& target, const std::string& file_path, bool synchronize_open) {
    RootDebugOperationScope debug_operation("RootScanInitLocal");
    RootDebug("INIT_LOCAL.BEGIN", "root_path=" + file_path + " tree=" + bind_data.tree_name +
                                      " columns=" + std::to_string(bind_data.columns.size()));

    auto* local_state = &target;
    auto* open_mutex = synchronize_open ? &gstate.coordination_mutex : nullptr;

    if (bind_data.IsPrimitiveTreeMode()) {
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
                rootlake::ApplyBranchProjection(tree, projected_branches, bind_data.root_access.tree_cache_bytes);

            if (!projection.applied) {
                rootlake::EnableAllBranches(tree, bind_data.root_access.tree_cache_bytes);
            }

            local_state->primitive_tree_requires_read = true;
        }

        RootDebug("PRIMITIVE_TREE.PROJECTION",
                  "file=" + file_path + " projected_branches=" + std::to_string(projected_branches.size()));

        if (gstate.file_scheduler) {
            const auto entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));

            local_state->local_current_row =
                gstate.entry_range_impossible ? entries : std::min(entries, gstate.entry_lower);

            local_state->local_end_row = entries;

            if (gstate.entry_upper != std::numeric_limits<uint64_t>::max()) {
                local_state->local_end_row = std::min(entries, gstate.entry_upper + 1);
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

    if (bind_data.IsDirectBranchMode()) {
        const auto* direct_mode = bind_data.DirectBranchMode();
        if (!direct_mode) {
            throw InternalException("read_root direct branch mode has no branch plan");
        }
        const auto& direct_branch_info = direct_mode->branch;
        const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
        if (gstate.file_scheduler) {
            gstate.file_scheduler->RecordOpen(local_state->file_task, open_result.attempts, open_result.elapsed_us);
        }
        auto* tree = local_state->root_file.GetTTree();
        if (tree) {
            local_state->direct_branch = tree->GetBranch(direct_branch_info.name.c_str());
            if (local_state->direct_branch) {
                local_state->direct_leaf = local_state->direct_branch->GetLeaf(local_state->direct_branch->GetName());
                std::vector<TBranch*> projected_branches{local_state->direct_branch};
                if (local_state->direct_leaf && local_state->direct_leaf->GetLeafCount() &&
                    local_state->direct_leaf->GetLeafCount()->GetBranch()) {
                    projected_branches.push_back(local_state->direct_leaf->GetLeafCount()->GetBranch());
                }
                const auto projection =
                    rootlake::ApplyBranchProjection(tree, projected_branches, bind_data.root_access.tree_cache_bytes);
                if (!projection.applied) {
                    rootlake::EnableAllBranches(tree, bind_data.root_access.tree_cache_bytes);
                }
            }
        }
        if (!local_state->direct_branch || !local_state->direct_leaf) {
            throw IOException("ROOT schema mismatch in " + file_path + ": primitive branch '" +
                              direct_branch_info.name + "' is absent");
        }
        if (gstate.file_scheduler) {
            const auto entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));
            local_state->local_current_row =
                gstate.entry_range_impossible ? entries : std::min(entries, gstate.entry_lower);
            local_state->local_end_row = entries;
            if (gstate.entry_upper != std::numeric_limits<uint64_t>::max()) {
                local_state->local_end_row = std::min(entries, gstate.entry_upper + 1);
            }
            local_state->file_active = true;
            gstate.file_scheduler->ObserveSchema("primitive:" + direct_branch_info.type_name);
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
            reader.Bind(file, "", root_class_name, bind_data.root_access.dictionary_cleanup_mode);
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
        // An index-only projection needs one representative value column to
        // recover the collection shape; decoding every sibling is wasteful.
        for (idx_t col_idx = 0; col_idx < bind_data.columns.size(); ++col_idx) {
            const auto& col = bind_data.columns[col_idx];
            if (!col.is_virtual_index && !col.is_string && !col.levels.empty() && !col.logical_path.empty()) {
                serialized_candidates.push_back(col_idx);
                break;
            }
        }
    }

    std::vector<TBranch*> projected_branches;
    std::unordered_map<TBranch*, std::shared_ptr<rootlake::SerializedBasketCache>> serialized_basket_caches;
    TTree* projection_tree = nullptr;
    bool all_candidates_projectable = !serialized_candidates.empty();
    local_state->serialized_columns.reserve(serialized_candidates.size());
    for (const auto col_idx : serialized_candidates) {
        const auto& col = bind_data.columns[col_idx];
        auto reader_it = local_state->root_readers.find(col.branch_name);
        if (reader_it == local_state->root_readers.end()) {
            all_candidates_projectable = false;
            if (bind_data.root_access.reader_mode == rootlake::RootReaderMode::SERIALIZED) {
                throw InvalidInputException("reader_mode='serialized' cannot bind ROOT object context for " +
                                            col.logical_path);
            }
            rootlake::WarnRootFallbackOnce(col.logical_path, "unknown", "ROOT object context is unavailable");
            continue;
        }

        RootSerializedColumnState state;
        state.column_id = col_idx;
        auto& object_reader = reader_it->second;
        if (!projection_tree) {
            projection_tree = object_reader.Tree();
        } else if (projection_tree != object_reader.Tree()) {
            all_candidates_projectable = false;
        }
        auto parsed = rootlake::ParsePath(col.logical_path);
        state.path_reader.Resolve(object_reader.Tree(), object_reader.ObjectBranch(), object_reader.RootClass(),
                                  std::move(parsed), col.levels);
        if (auto* physical_branch = state.path_reader.PhysicalBranch()) {
            auto cache_it = serialized_basket_caches.find(physical_branch);
            if (cache_it == serialized_basket_caches.end()) {
                auto cache = std::make_shared<rootlake::SerializedBasketCache>();
                cache->Bind(physical_branch);
                cache_it = serialized_basket_caches.emplace(physical_branch, std::move(cache)).first;
            }
            state.path_reader.SetSerializedBasketCache(cache_it->second);
        }
        if (state.path_reader.PhysicalMode() == "ancestor") {
            projected_branches.push_back(state.path_reader.PhysicalBranch());
        } else {
            all_candidates_projectable = false;
        }
        local_state->serialized_columns.emplace_back(std::move(state));
    }

    if (!local_state->serialized_columns.empty()) {
        const auto projection =
            all_candidates_projectable
                ? rootlake::ApplyBranchProjection(projection_tree, projected_branches,
                                                  bind_data.root_access.tree_cache_bytes)
                : rootlake::BranchProjectionResult{};
        if (!projection.applied) {
            for (auto& [root_class_name, reader] : local_state->root_readers) {
                (void)root_class_name;
                rootlake::EnableAllBranches(reader.Tree(), bind_data.root_access.tree_cache_bytes);
            }
        }

        auto reader_options = bind_data.root_access;
        reader_options.enable_all_branches_on_fallback = true;
        for (auto& state : local_state->serialized_columns) {
            state.path_reader.StartSerialized(reader_options);
        }
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
            gstate.entry_range_impossible ? entries : std::min(entries, gstate.entry_lower);
        local_state->local_end_row = entries;
        if (gstate.entry_upper != std::numeric_limits<uint64_t>::max()) {
            local_state->local_end_row = std::min(entries, gstate.entry_upper + 1);
        }
        local_state->file_active = true;
        std::string fingerprint;
        for (const auto& state : local_state->serialized_columns) {
            const auto& column_fingerprint = state.path_reader.SerializedPlan().schema_fingerprint;
            if (column_fingerprint.empty()) {
                continue;
            }
            if (!fingerprint.empty()) {
                fingerprint += '|';
            }
            fingerprint += column_fingerprint;
        }
        if (fingerprint.empty()) {
            fingerprint = std::string("object:") + bind_data.tree_name;
        }
        gstate.file_scheduler->ObserveSchema(fingerprint);
    }
}

unique_ptr<LocalTableFunctionState> RootScanStateFactory::CreateLocal(TableFunctionInitInput& input,
                                                                      GlobalTableFunctionState* global_state_p) {
    auto& bind_data = input.bind_data->Cast<RootScanBindData>();
    auto& gstate = global_state_p->Cast<RootScanGlobalState>();
    auto local_state = make_uniq<RootScanLocalState>();
    if (!bind_data.IsEmptyMode() && !bind_data.IsBrowseMode() && !bind_data.IsHistogramMode() &&
        !bind_data.IsMultiFile()) {
        RootScanFileManager().Open(bind_data, gstate, *local_state, bind_data.root_path, false);
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
    local_state.serialized_columns.clear();
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
