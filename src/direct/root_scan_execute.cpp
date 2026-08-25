#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb {

std::vector<std::string> RootScanExecutor::SplitIndexSignature(const std::string& signature) {
    std::vector<std::string> names;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

void RootScanExecutor::MaterializeSerializedResult(const RootScanColumn& column, uint64_t entry,
                                                   const rootlake::SerializedReadPlan& plan,
                                                   const std::vector<rootlake::RootPrimitiveValue>& values,
                                                   const std::vector<int32_t>& flat_indices,
                                                   rootlake::ReadResult& result) {
    result.Clear();
    const auto index_names = SplitIndexSignature(column.index_signature);
    if (index_names.size() != plan.index_depth || flat_indices.size() != values.size() * plan.index_depth) {
        throw InternalException("serialized ROOT index shape differs from bound SQL schema");
    }
    std::vector<int> indices(plan.index_depth);
    for (idx_t value_index = 0; value_index < values.size(); ++value_index) {
        for (idx_t depth = 0; depth < plan.index_depth; ++depth) {
            indices[depth] = flat_indices[value_index * plan.index_depth + depth];
        }
        std::vector<int32_t> flat_index_row(indices.begin(), indices.end());
        result.AddNumber(values[value_index], static_cast<Long64_t>(entry), flat_index_row, index_names);
    }
}

CacheResult RootScanExecutor::ReadAndCacheEntry(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                                                RootScanLocalState& lstate, DataChunk& output, idx_t& out_count) {
    uint64_t entry = lstate.local_current_row;

    std::map<std::string, std::vector<idx_t>> branch_columns;
    for (idx_t out_idx = 0; out_idx < gstate.scan_column_ids.size(); ++out_idx) {
        idx_t col_idx = gstate.scan_column_ids[out_idx];
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size()) {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (!col.levels.empty() && !col.is_virtual_index && col.name != "event_id") {
            branch_columns[col.branch_name].push_back(col_idx);
        }
    }

    lstate.cached_results.resize(bind_data.columns.size());
    bool has_data = false;

    if (lstate.path_reader.SerializedActive() && lstate.serialized_column < bind_data.columns.size()) {
        const auto& column = bind_data.columns[lstate.serialized_column];
        auto reader_it = lstate.root_readers.find(column.branch_name);
        if (reader_it == lstate.root_readers.end()) {
            throw InternalException("serialized ROOT path has no universal object reader");
        }
        rootlake::RootEntryReader object_entry(reader_it->second);
        object_entry.Begin(entry);
        const auto read = lstate.path_reader.TryReadSerialized(entry, object_entry, lstate.serialized_values,
                                                               lstate.serialized_indices);
        gstate.object_validation_entries.fetch_add(object_entry.LoadCount());

        const auto& serialized_counters = lstate.path_reader.SerializedCounters();
        gstate.serialized_baskets.fetch_add(serialized_counters.baskets - lstate.reported_serialized_baskets);
        gstate.serialized_compressed_bytes.fetch_add(serialized_counters.compressed_bytes -
                                                     lstate.reported_serialized_compressed_bytes);
        gstate.serialized_entry_bytes.fetch_add(serialized_counters.serialized_bytes -
                                                lstate.reported_serialized_entry_bytes);
        lstate.reported_serialized_baskets = serialized_counters.baskets;
        lstate.reported_serialized_compressed_bytes = serialized_counters.compressed_bytes;
        lstate.reported_serialized_entry_bytes = serialized_counters.serialized_bytes;
        if (read.Decoded()) {
            gstate.serialized_entries.fetch_add(1);
            gstate.serialized_values.fetch_add(lstate.serialized_values.size());
        }
        if (read.Serialized()) {
            MaterializeSerializedResult(column, entry, lstate.path_reader.SerializedPlan(), lstate.serialized_values,
                                        lstate.serialized_indices, lstate.cached_results[lstate.serialized_column]);
            has_data = !lstate.cached_results[lstate.serialized_column].empty();
            if (!has_data) {
                lstate.local_current_row++;
                return CacheResult::CONTINUE_LOOP;
            }
            lstate.current_entry = entry;
            lstate.current_elem_idx = 0;
            lstate.has_cached_entry = true;
            return CacheResult::CACHED;
        }
    }

    if (branch_columns.empty() && lstate.has_container_columns) {
        idx_t sample_col_idx = static_cast<idx_t>(-1);
        std::string sample_branch;

        for (idx_t i = 0; i < bind_data.columns.size(); ++i) {
            const auto& col = bind_data.columns[i];
            if (!col.levels.empty() && !col.is_virtual_index && col.name != "event_id") {
                sample_col_idx = i;
                sample_branch = col.branch_name;
                break;
            }
        }

        if (sample_col_idx != static_cast<idx_t>(-1)) {
            auto it = lstate.root_readers.find(sample_branch);
            if (it != lstate.root_readers.end()) {
                auto& reader = it->second;
                if (entry < static_cast<uint64_t>(reader.Tree()->GetEntries())) {
                    RootDebug("READ.BEFORE_GET_ENTRY", "mode=sample class=" + sample_branch +
                                                           " entry=" + std::to_string(entry) +
                                                           " tree_ptr=" + RootPointer(reader.Tree()));
                    void* object = reader.Read(entry);
                    gstate.object_fallback_entries.fetch_add(1);
                    RootDebug("READ.AFTER_GET_ENTRY", "mode=sample class=" + sample_branch + " entry=" +
                                                          std::to_string(entry) + " object_ptr=" + RootPointer(object));
                    if (object) {
                        const auto& col = bind_data.columns[sample_col_idx];
                        lstate.cached_results[sample_col_idx].Clear();
                        rootlake::OffsetValueReader::CollectDirect(object, col.levels,
                                                                   std::numeric_limits<Long64_t>::max(), entry,
                                                                   lstate.cached_results[sample_col_idx]);
                        has_data = !lstate.cached_results[sample_col_idx].empty();
                    }
                }
            }
        }
    } else {
        for (const auto& [branch_name, col_indices] : branch_columns) {
            auto it = lstate.root_readers.find(branch_name);
            if (it == lstate.root_readers.end()) {
                continue;
            }
            auto& reader = it->second;
            if (entry >= static_cast<uint64_t>(reader.Tree()->GetEntries())) {
                continue;
            }

            RootDebug("READ.BEFORE_GET_ENTRY", "mode=group class=" + branch_name + " entry=" + std::to_string(entry) +
                                                   " tree_ptr=" + RootPointer(reader.Tree()));
            void* object = reader.Read(entry);
            gstate.object_fallback_entries.fetch_add(1);
            RootDebug("READ.AFTER_GET_ENTRY", "mode=group class=" + branch_name + " entry=" + std::to_string(entry) +
                                                  " object_ptr=" + RootPointer(object));
            if (!object) {
                continue;
            }

            for (idx_t col_idx : col_indices) {
                const auto& col = bind_data.columns[col_idx];
                if (object) {
                    lstate.cached_results[col_idx].Clear();
                    rootlake::OffsetValueReader::CollectDirect(object, col.levels, std::numeric_limits<Long64_t>::max(),
                                                               entry, lstate.cached_results[col_idx]);
                    if (!lstate.cached_results[col_idx].empty()) {
                        has_data = true;
                    }
                }
            }
        }
    }

    if (!has_data) {
        if (lstate.has_container_columns) {
            lstate.local_current_row++;
            return CacheResult::CONTINUE_LOOP;
        } else {
            if (entry > 0) {
                lstate.local_current_row = lstate.local_end_row;
                return CacheResult::BREAK_LOOP;
            }
        }
    }

    lstate.current_entry = entry;
    lstate.current_elem_idx = 0;
    lstate.has_cached_entry = true;
    return CacheResult::CACHED;
}

void RootScanExecutor::Execute(ClientContext& context, TableFunctionInput& data_p, DataChunk& output) {
    auto& bind_data = data_p.bind_data->Cast<RootScanBindData>();
    auto& gstate = data_p.global_state->Cast<RootScanGlobalState>();
    auto& lstate = data_p.local_state->Cast<RootScanLocalState>();

    if (bind_data.IsEmptyMode()) {
        output.SetCardinality(0);
        return;
    }
    if (gstate.event_range_impossible) {
        output.SetCardinality(0);
        return;
    }

    if (bind_data.IsHistogramMode()) {
        ProcessHistogramMode(context, bind_data, gstate, lstate, output);
        return;
    }

    if (bind_data.IsBrowseMode()) {
        ProcessBrowseMode(context, bind_data, gstate, lstate, output);
        return;
    }

    if (bind_data.IsPrimitiveTreeMode()) {
        while (true) {
            if (gstate.file_scheduler && !file_manager.EnsureReady(bind_data, gstate, lstate)) {
                output.SetCardinality(0);
                return;
            }

            ProcessPrimitiveTree(context, bind_data, gstate, lstate, output);

            if (output.size() > 0) {
                if (gstate.file_scheduler) {
                    gstate.file_scheduler->RecordFirstRow();
                }
                return;
            }

            if (!gstate.file_scheduler) {
                return;
            }
        }
    }

    if (bind_data.IsDirectBranchMode()) {
        while (true) {
            if (gstate.file_scheduler && !file_manager.EnsureReady(bind_data, gstate, lstate)) {
                output.SetCardinality(0);
                return;
            }
            ProcessDirectBranch(context, bind_data, gstate, lstate, output);
            if (output.size() > 0) {
                if (gstate.file_scheduler) {
                    gstate.file_scheduler->RecordFirstRow();
                }
                return;
            }
            if (!gstate.file_scheduler) {
                return;
            }
        }
    }

    while (true) {
        if (gstate.file_scheduler && !file_manager.EnsureReady(bind_data, gstate, lstate)) {
            output.SetCardinality(0);
            return;
        }

        idx_t out_count = 0;
        while (out_count < STANDARD_VECTOR_SIZE) {
            if (lstate.has_cached_entry) {
                ProcessCachedEntry(context, bind_data, gstate, lstate, output, out_count);
            } else {
                if (lstate.local_current_row >= lstate.local_end_row) {
                    if (gstate.file_scheduler) {
                        break;
                    }
                    RootEntryScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
                    auto batch = scheduler.ClaimWork(100000);
                    if (!batch.HasWork()) {
                        break;
                    }
                    lstate.local_current_row = batch.start;
                    lstate.local_end_row = batch.end;
                }
                CacheResult res = ReadAndCacheEntry(bind_data, gstate, lstate, output, out_count);
                if (res == CacheResult::CONTINUE_LOOP) {
                    continue;
                }
                if (res == CacheResult::BREAK_LOOP) {
                    lstate.local_current_row = lstate.local_end_row;
                    continue;
                }
            }
        }

        output.SetCardinality(out_count);
        if (out_count > 0) {
            if (gstate.file_scheduler) {
                gstate.file_scheduler->RecordFirstRow();
            }
            return;
        }
        if (!gstate.file_scheduler) {
            return;
        }
    }
}

InsertionOrderPreservingMap<string> RootScanExplain::Bound(TableFunctionToStringInput& input) {
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data) {
        return result;
    }
    const auto& bind = input.bind_data->Cast<RootScanBindData>();
    result["ROOT Input"] = bind.input_specification;
    result["ROOT Files"] = std::to_string(bind.root_paths.size());
    result["ROOT Representative"] = bind.root_path;
    result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.root_access.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.root_access.validation_entries);
    return result;
}

InsertionOrderPreservingMap<string> RootScanExplain::Running(TableFunctionDynamicToStringInput& input) {
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data) {
        const auto& bind = input.bind_data->Cast<RootScanBindData>();
        result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.root_access.reader_mode);
        result["ROOT Input Files"] = std::to_string(bind.root_paths.size());
        result["ROOT Representative"] = bind.root_path;
        result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    }
    if (!input.global_state) {
        return result;
    }
    const auto& global = input.global_state->Cast<RootScanGlobalState>();
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entries.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Object Validation Entries"] = std::to_string(global.object_validation_entries.load());
    result["Object Fallback Entries"] = std::to_string(global.object_fallback_entries.load());
    if (global.file_scheduler) {
        result["ROOT Opened Files"] = std::to_string(global.file_scheduler->OpenedFiles());
        result["ROOT Completed Files"] = std::to_string(global.file_scheduler->CompletedFiles());
        result["ROOT Skipped Files"] = std::to_string(global.file_scheduler->SkippedFiles());
        result["ROOT Unavailable Files"] = std::to_string(global.file_scheduler->UnavailableFiles());
        result["ROOT Failed Files"] = std::to_string(global.file_scheduler->FailedFiles());
        result["ROOT Retried Opens"] = std::to_string(global.file_scheduler->RetriedOpens());
        result["ROOT Open Time (us)"] = std::to_string(global.file_scheduler->OpenTimeUs());
        result["ROOT Schema Variants"] = std::to_string(global.file_scheduler->SchemaVariants());
        result["ROOT Schema Plan Reuses"] = std::to_string(global.file_scheduler->SchemaPlanReuses());
        result["ROOT Time To First Row (us)"] = std::to_string(global.file_scheduler->FirstRowUs());
        result["ROOT Slowest File"] = global.file_scheduler->SlowestFile();
        result["ROOT Slowest File Time (us)"] = std::to_string(global.file_scheduler->SlowestFileUs());
    }
    return result;
}

} // namespace duckdb
