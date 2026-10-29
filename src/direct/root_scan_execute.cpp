#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb
{
namespace
{

int64_t CheckedEntryId(uint64_t entry)
{
    if (entry > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        throw InternalException("ROOT entry id exceeds the supported signed 64-bit range");
    }
    return static_cast<int64_t>(entry);
}

void RecordDirectSerializedCounters(RootScanGlobalState& global, RootSerializedColumnState& column,
                                    const rootlake::RootPathReader& reader)
{
    const auto& current = reader.SerializedCounters();
    global.serialized_baskets.fetch_add(current.baskets - column.reported_baskets);
    global.serialized_compressed_bytes.fetch_add(current.compressed_bytes - column.reported_compressed_bytes);
    global.serialized_entry_bytes.fetch_add(current.serialized_bytes - column.reported_entry_bytes);
    global.nested_fixed_direct_entries.fetch_add(current.nested_fixed_direct_entries -
                                                 column.reported_nested_fixed_direct);
    global.nested_fixed_scratch_entries.fetch_add(current.nested_fixed_scratch_entries -
                                                  column.reported_nested_fixed_scratch);
    column.reported_baskets = current.baskets;
    column.reported_compressed_bytes = current.compressed_bytes;
    column.reported_entry_bytes = current.serialized_bytes;
    column.reported_nested_fixed_direct = current.nested_fixed_direct_entries;
    column.reported_nested_fixed_scratch = current.nested_fixed_scratch_entries;
}

} // namespace

std::vector<std::string> RootScanExecutor::SplitIndexSignature(const std::string& signature) const
{
    std::vector<std::string> names;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        if (!name.empty())
        {
            names.push_back(name);
        }
    }
    return names;
}

void RootScanExecutor::MaterializeSerializedResult(const RootScanColumn& column, uint64_t entry,
                                                   const rootlake::SerializedReadPlan& plan,
                                                   const std::vector<rootlake::RootPrimitiveValue>& values,
                                                   const std::vector<int32_t>& flat_indices,
                                                   rootlake::ReadResult& result) const
{
    result.Clear();
    const auto entry_id = CheckedEntryId(entry);
    const auto index_names = SplitIndexSignature(column.index_signature);
    if (plan.index_depth != 0 && values.size() > flat_indices.max_size() / plan.index_depth)
    {
        throw IOException("serialized ROOT index capacity exceeds supported size");
    }
    const auto expected_indices = values.size() * plan.index_depth;
    if (index_names.size() != plan.index_depth || flat_indices.size() != expected_indices)
    {
        throw InternalException("serialized ROOT index shape differs from bound SQL schema");
    }
    const auto value_count = values.size();
    result.strings.resize(value_count);
    result.numbers = values;
    result.is_string_flag.resize(value_count, false);
    result.entry_ids.resize(value_count, entry_id);
    result.vector_indices.resize(value_count);
    if (value_count > 0)
    {
        result.vector_names = index_names;
    }
    for (idx_t value_index = 0; value_index < value_count; ++value_index)
    {
        using IndexDifference = std::vector<int32_t>::difference_type;
        const auto row_offset = static_cast<IndexDifference>(value_index * plan.index_depth);
        const auto row_width = static_cast<IndexDifference>(plan.index_depth);
        const auto first = flat_indices.cbegin() + row_offset;
        result.vector_indices[value_index].assign(first, first + row_width);
    }
}

bool RootScanExecutor::ReadCursorPage(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                                      RootScanLocalState& lstate) const
{
    lstate.cached_index_sources.clear();
    lstate.cached_results.resize(lstate.path_readers.size());
    if (!lstate.multi_numeric_page_state_indices.empty())
    {
        // The same source-row positions are valid only when *all* flat
        // structural index tuples match, including at SQL page boundaries.
        const RootSerializedColumnState* reference = nullptr;
        bool has_data = false;
        bool has_more = false;
        rootlake::RootNumericPageSource first_source = rootlake::RootNumericPageSource::NONE;
        for (const auto state_index : lstate.multi_numeric_page_state_indices)
        {
            auto& state = lstate.serialized_columns[state_index];
            auto& reader = lstate.path_readers[state.column_id];
            const auto source = reader.ReadNumericPage(STANDARD_VECTOR_SIZE, state.values, state.indices);
            RecordDirectSerializedCounters(gstate, state, reader);
            has_more = has_more || reader.HasNumericPage();
            if (reference && (source != first_source || state.values.size() != reference->values.size() ||
                              state.indices != reference->indices))
            {
                if (!lstate.multi_numeric_page_emitted &&
                    bind_data.root_access.reader_mode != rootlake::RootReaderMode::SERIALIZED)
                {
                    // No rows have escaped: retry through the existing whole-entry
                    // multi-column alignment check and object fallback.
                    lstate.multi_numeric_shape_mismatch = true;
                    lstate.cursor_has_more = false;
                    return false;
                }
                throw IOException("serialized ROOT sibling numeric pages have mismatched structural coordinates "
                                  "at entry " +
                                  std::to_string(lstate.local_current_row));
            }
            if (!reference)
            {
                reference = &state;
                first_source = source;
            }
            auto& result = lstate.cached_results[state.column_id];
            result.Clear();
            if (source == rootlake::RootNumericPageSource::SERIALIZED)
            {
                MaterializeSerializedResult(bind_data.columns[state.column_id], lstate.local_current_row,
                                            reader.SerializedPlan(), state.values, state.indices, result);
                gstate.serialized_page_calls.fetch_add(1);
                gstate.serialized_values.fetch_add(state.values.size());
                has_data = has_data || !result.empty();
            }
            else if (source != rootlake::RootNumericPageSource::NONE)
            {
                throw IOException("a synchronized serialized ROOT page unexpectedly switched to object mode");
            }
        }
        lstate.cursor_has_more = has_more;
        if (has_data)
        {
            lstate.multi_numeric_page_emitted = true;
            gstate.serialized_multi_column_pages.fetch_add(1);
        }
        return has_data;
    }
    if (lstate.numeric_page_column != DConstants::INVALID_INDEX)
    {
        auto& state = lstate.serialized_columns.front();
        const auto col_idx = lstate.numeric_page_column;
        auto& reader = lstate.path_readers[col_idx];
        const auto source = reader.ReadNumericPage(STANDARD_VECTOR_SIZE, state.values, state.indices);
        RecordDirectSerializedCounters(gstate, state, reader);
        lstate.cursor_has_more = reader.HasNumericPage();
        if (source == rootlake::RootNumericPageSource::NONE)
        {
            return false;
        }
        MaterializeSerializedResult(bind_data.columns[col_idx], lstate.local_current_row, reader.SerializedPlan(),
                                    state.values, state.indices, lstate.cached_results[col_idx]);
        if (source == rootlake::RootNumericPageSource::SERIALIZED)
        {
            gstate.serialized_page_calls.fetch_add(1);
            gstate.serialized_values.fetch_add(state.values.size());
        }
        return !lstate.cached_results[col_idx].empty();
    }
    bool has_data = false;
    bool has_more = false;
    for (idx_t i = 0; i < lstate.path_readers.size(); ++i)
    {
        auto& result = lstate.cached_results[i];
        result.Clear();
        auto& reader = lstate.path_readers[i];
        if (reader.HasObjectPage())
        {
            reader.ReadObjectPage(STANDARD_VECTOR_SIZE, result);
        }
        has_data = has_data || !result.empty();
        has_more = has_more || reader.HasObjectPage();
    }
    lstate.cursor_has_more = has_more;
    return has_data;
}

void RootScanExecutor::EndEntryCursors(RootScanLocalState& lstate) const
{
    for (const auto state_index : lstate.multi_numeric_page_state_indices)
    {
        const auto column_id = lstate.serialized_columns[state_index].column_id;
        lstate.path_readers[column_id].EndNumericPage();
    }
    lstate.multi_numeric_page_state_indices.clear();
    lstate.multi_numeric_page_emitted = false;
    lstate.multi_numeric_shape_mismatch = false;
    if (lstate.numeric_page_column != DConstants::INVALID_INDEX)
    {
        lstate.path_readers[lstate.numeric_page_column].EndNumericPage();
        lstate.numeric_page_column = DConstants::INVALID_INDEX;
    }
    for (auto& reader : lstate.path_readers)
    {
        reader.EndObjectEntry();
    }
    lstate.cursor_entry_active = false;
    lstate.cursor_has_more = false;
}

CacheResult RootScanExecutor::ReadAndCacheEntry(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                                                RootScanLocalState& lstate) const
{
    const uint64_t entry = lstate.local_current_row;
    const auto entry_id = CheckedEntryId(entry);
    if (lstate.cursor_entry_active)
    {
        if (ReadCursorPage(bind_data, gstate, lstate))
        {
            lstate.current_elem_idx = 0;
            lstate.has_cached_entry = true;
            return CacheResult::CACHED;
        }
        EndEntryCursors(lstate);
        lstate.local_current_row++;
        return CacheResult::CONTINUE_LOOP;
    }
    lstate.cached_index_sources.clear();
    lstate.cached_results.resize(bind_data.columns.size());

    // Scalar FIXED_MEMBER siblings have a common container shape. Decode in
    // lockstep, verifying every full index tuple, never joining by page offset
    // alone. Complex or mixed-shape relations retain the original safe path.
    if (lstate.projected_relation_columns.size() > 1 &&
        lstate.serialized_columns.size() == lstate.projected_relation_columns.size())
    {
        const auto& first_state = lstate.serialized_columns.front();
        const auto& first_column = bind_data.columns[first_state.column_id];
        const auto& first_reader = lstate.path_readers[first_state.column_id];
        const auto& first_plan = first_reader.SerializedPlan();
        bool eligible = first_reader.SerializedActive() && first_reader.ValidationRemaining() == 0 &&
                        first_plan.Is(rootlake::SerializedProjectionKind::FIXED_MEMBER) &&
                        first_plan.fixed_array_length == 1 && !first_column.is_string;
        for (const auto& state : lstate.serialized_columns)
        {
            const auto& col = bind_data.columns[state.column_id];
            const auto& reader = lstate.path_readers[state.column_id];
            const auto& plan = reader.SerializedPlan();
            if (std::find(lstate.projected_relation_columns.begin(), lstate.projected_relation_columns.end(),
                          state.column_id) == lstate.projected_relation_columns.end() ||
                !reader.SerializedActive() || reader.ValidationRemaining() != 0 ||
                !plan.Is(rootlake::SerializedProjectionKind::FIXED_MEMBER) || plan.fixed_array_length != 1 ||
                col.is_string || col.index_signature != first_column.index_signature ||
                plan.root_class != first_plan.root_class || plan.container_name != first_plan.container_name ||
                plan.element_class != first_plan.element_class || plan.index_depth != first_plan.index_depth)
            {
                eligible = false;
                break;
            }
        }
        if (eligible)
        {
            auto reader_it = lstate.root_readers.find(first_column.root_class_name);
            if (reader_it == lstate.root_readers.end())
            {
                throw InternalException("synchronized direct ROOT columns have no object context");
            }
            rootlake::RootEntryReader object_entry(reader_it->second);
            object_entry.Begin(entry);
            std::vector<idx_t> started_states;
            started_states.reserve(lstate.serialized_columns.size());
            for (idx_t state_index = 0; state_index < lstate.serialized_columns.size(); ++state_index)
            {
                auto& state = lstate.serialized_columns[state_index];
                const auto started = lstate.path_readers[state.column_id].BeginNumericEntry(
                    entry, object_entry, entry_id, state.values, state.indices);
                if (started.route != rootlake::RootNumericEntryRoute::SERIALIZED_PAGE)
                {
                    lstate.path_readers[state.column_id].EndNumericPage();
                    break;
                }
                started_states.push_back(state_index);
            }
            gstate.object_validation_entries.fetch_add(object_entry.LoadCount());
            if (started_states.size() == lstate.serialized_columns.size())
            {
                for (auto& result : lstate.cached_results)
                {
                    result.Clear();
                }
                lstate.multi_numeric_page_state_indices = std::move(started_states);
                lstate.multi_numeric_page_emitted = false;
                lstate.multi_numeric_shape_mismatch = false;
                lstate.cursor_entry_active = true;
                if (ReadCursorPage(bind_data, gstate, lstate))
                {
                    gstate.serialized_entries.fetch_add(lstate.multi_numeric_page_state_indices.size());
                    lstate.current_entry = entry;
                    lstate.current_elem_idx = 0;
                    lstate.has_cached_entry = true;
                    return CacheResult::CACHED;
                }
                const bool shape_mismatch = lstate.multi_numeric_shape_mismatch;
                EndEntryCursors(lstate);
                if (!shape_mismatch)
                {
                    lstate.local_current_row++;
                    return CacheResult::CONTINUE_LOOP;
                }
                // A mismatch on the first page has emitted nothing. The
                // established whole-entry path can still select object fallback.
            }
            else
            {
                for (const auto state_index : started_states)
                {
                    lstate.path_readers[lstate.serialized_columns[state_index].column_id].EndNumericPage();
                }
            }
        }
    }

    // One numeric relation column needs no sibling shape alignment. Let the
    // existing shared ROOT path reader produce bounded FIXED_MEMBER pages.
    if (lstate.serialized_columns.size() == 1 && lstate.projected_relation_columns.size() == 1)
    {
        auto& column_state = lstate.serialized_columns.front();
        const auto column_id = column_state.column_id;
        if (column_id == lstate.projected_relation_columns.front() && column_id < bind_data.columns.size())
        {
            auto& reader = lstate.path_readers[column_id];
            if (reader.SerializedActive() && reader.ValidationRemaining() == 0 &&
                reader.SerializedPlan().Is(rootlake::SerializedProjectionKind::FIXED_MEMBER))
            {
                const auto& column = bind_data.columns[column_id];
                auto object_reader = lstate.root_readers.find(column.root_class_name);
                if (object_reader == lstate.root_readers.end())
                {
                    throw InternalException("paged direct ROOT path has no object context");
                }
                rootlake::RootEntryReader object_entry(object_reader->second);
                object_entry.Begin(entry);
                const auto started =
                    reader.BeginNumericEntry(entry, object_entry, entry_id, column_state.values, column_state.indices);
                gstate.object_validation_entries.fetch_add(object_entry.LoadCount());
                if (started.route == rootlake::RootNumericEntryRoute::SERIALIZED_PAGE ||
                    started.route == rootlake::RootNumericEntryRoute::OBJECT_PAGE)
                {
                    if (started.route == rootlake::RootNumericEntryRoute::SERIALIZED_PAGE)
                    {
                        gstate.serialized_entries.fetch_add(1);
                    }
                    else
                    {
                        gstate.object_fallback_entries.fetch_add(1);
                    }
                    lstate.numeric_page_column = column_id;
                    lstate.cursor_entry_active = true;
                    if (ReadCursorPage(bind_data, gstate, lstate))
                    {
                        lstate.current_entry = entry;
                        lstate.current_elem_idx = 0;
                        lstate.has_cached_entry = true;
                        return CacheResult::CACHED;
                    }
                    reader.EndNumericPage();
                    lstate.numeric_page_column = DConstants::INVALID_INDEX;
                    lstate.cursor_entry_active = false;
                    lstate.local_current_row++;
                    return CacheResult::CONTINUE_LOOP;
                }
            }
        }
    }

    std::set<idx_t> serialized_success;
    for (auto& serialized : lstate.serialized_columns)
    {
        if (serialized.column_id >= bind_data.columns.size() ||
            !lstate.path_readers[serialized.column_id].SerializedActive())
        {
            continue;
        }
        const auto& column = bind_data.columns[serialized.column_id];
        auto reader_it = lstate.root_readers.find(column.root_class_name);
        if (reader_it == lstate.root_readers.end())
        {
            throw InternalException("serialized ROOT path has no universal object reader");
        }
        rootlake::RootEntryReader object_entry(reader_it->second);
        object_entry.Begin(entry);
        const auto read = lstate.path_readers[serialized.column_id].TryReadSerialized(
            entry, object_entry, serialized.values, serialized.indices);
        gstate.object_validation_entries.fetch_add(object_entry.LoadCount());
        RecordDirectSerializedCounters(gstate, serialized, lstate.path_readers[serialized.column_id]);
        if (read.Decoded())
        {
            gstate.serialized_entries.fetch_add(1);
            gstate.serialized_values.fetch_add(serialized.values.size());
        }
        if (!read.Serialized())
        {
            continue;
        }
        MaterializeSerializedResult(column, entry, lstate.path_readers[serialized.column_id].SerializedPlan(),
                                    serialized.values, serialized.indices, lstate.cached_results[serialized.column_id]);
        serialized_success.insert(serialized.column_id);
    }
    // Whole-entry serialized vectors and object pages must not be mixed:
    // sibling columns need a common logical row offset at every page.
    bool object_required = false;
    bool has_data = false;
    const rootlake::ReadResult* sibling = nullptr;
    for (const auto col_idx : lstate.projected_relation_columns)
    {
        if (serialized_success.find(col_idx) == serialized_success.end())
        {
            object_required = true;
            break;
        }
        const auto& result = lstate.cached_results[col_idx];
        if (sibling && (result.size() != sibling->size() || result.vector_names != sibling->vector_names ||
                        result.vector_indices != sibling->vector_indices))
        {
            if (bind_data.root_access.reader_mode == rootlake::RootReaderMode::SERIALIZED)
            {
                throw IOException("serialized sibling columns have different collection shapes for ROOT relation at "
                                  "entry " +
                                  std::to_string(entry));
            }
            object_required = true;
            break;
        }
        sibling = &result;
        has_data = has_data || !result.empty();
    }
    if (lstate.projected_relation_columns.empty() && lstate.has_container_columns)
    {
        object_required = true;
    }
    if (object_required)
    {
        for (auto& reader : lstate.path_readers)
        {
            reader.EndObjectEntry();
        }
        // Index-only scans use one semantic value path to establish row geometry.
        idx_t representative = DConstants::INVALID_INDEX;
        if (!lstate.projected_relation_columns.empty())
        {
            representative = lstate.projected_relation_columns.front();
        }
        else if (lstate.has_container_columns)
        {
            for (idx_t i = 0; i < bind_data.columns.size(); ++i)
            {
                const auto& col = bind_data.columns[i];
                if (!col.levels.empty() && !col.is_virtual_index && col.name != "entry_id")
                {
                    representative = i;
                    break;
                }
            }
        }
        if (representative != DConstants::INVALID_INDEX)
        {
            const auto& root_class = bind_data.columns[representative].root_class_name;
            auto it = lstate.root_readers.find(root_class);
            if (it != lstate.root_readers.end() && entry < static_cast<uint64_t>(it->second.Tree()->GetEntries()))
            {
                void* object = it->second.Read(entry);
                gstate.object_fallback_entries.fetch_add(1);
                if (object)
                {
                    if (lstate.projected_relation_columns.empty())
                    {
                        lstate.path_readers[representative].BeginObjectEntry(object, entry_id);
                    }
                    else
                    {
                        for (const auto col_idx : lstate.projected_relation_columns)
                        {
                            lstate.path_readers[col_idx].BeginObjectEntry(object, entry_id);
                        }
                    }
                }
            }
        }
        lstate.cursor_entry_active = true;
        has_data = ReadCursorPage(bind_data, gstate, lstate);
    }
    if (!has_data && lstate.has_container_columns)
    {
        lstate.cursor_entry_active = false;
        for (auto& reader : lstate.path_readers)
        {
            reader.EndObjectEntry();
        }
        // An empty collection has no flattened rows. For scalar relations,
        // however, a projection of entry_id alone still has one row per event.
        lstate.local_current_row++;
        return CacheResult::CONTINUE_LOOP;
    }
    lstate.current_entry = entry;
    lstate.current_elem_idx = 0;
    lstate.has_cached_entry = true;
    return CacheResult::CACHED;
}

void RootScanExecutor::Execute(ClientContext& context, TableFunctionInput& data_p, DataChunk& output) const
{
    auto& bind_data = data_p.bind_data->Cast<RootScanBindData>();
    auto& gstate = data_p.global_state->Cast<RootScanGlobalState>();
    auto& lstate = data_p.local_state->Cast<RootScanLocalState>();
    if (bind_data.IsEmptyMode())
    {
        output.SetCardinalityUnsafe(0);
        return;
    }
    if (gstate.entry_range_impossible)
    {
        output.SetCardinalityUnsafe(0);
        return;
    }
    if (bind_data.IsHistogramMode())
    {
        ProcessHistogramMode(context, bind_data, gstate, lstate, output);
        return;
    }
    if (bind_data.IsBrowseMode())
    {
        ProcessBrowseMode(context, bind_data, gstate, lstate, output);
        return;
    }
    if (bind_data.IsPrimitiveTreeMode())
    {
        while (true)
        {
            if (gstate.file_scheduler && !file_manager.EnsureReady(bind_data, gstate, lstate))
            {
                output.SetCardinalityUnsafe(0);
                return;
            }
            ProcessPrimitiveTree(context, bind_data, gstate, lstate, output);
            if (output.size() > 0)
            {
                if (gstate.file_scheduler)
                {
                    gstate.file_scheduler->RecordFirstRow();
                }
                return;
            }
            if (!gstate.file_scheduler)
            {
                return;
            }
        }
    }

    while (true)
    {
        if (gstate.file_scheduler && !file_manager.EnsureReady(bind_data, gstate, lstate))
        {
            output.SetCardinalityUnsafe(0);
            return;
        }
        idx_t out_count = 0;
        while (out_count < STANDARD_VECTOR_SIZE)
        {
            if (lstate.has_cached_entry)
            {
                ProcessCachedEntry(context, bind_data, gstate, lstate, output, out_count);
            }
            else
            {
                if (lstate.local_current_row >= lstate.local_end_row)
                {
                    if (gstate.file_scheduler)
                    {
                        break;
                    }
                    if (!ClaimEntryBatch(gstate, lstate))
                    {
                        break;
                    }
                }
                const CacheResult res = ReadAndCacheEntry(bind_data, gstate, lstate);
                if (res == CacheResult::CONTINUE_LOOP)
                {
                    continue;
                }
                if (res == CacheResult::BREAK_LOOP)
                {
                    lstate.local_current_row = lstate.local_end_row;
                    continue;
                }
            }
        }
        output.SetCardinalityUnsafe(out_count);
        if (out_count > 0)
        {
            if (gstate.file_scheduler)
            {
                gstate.file_scheduler->RecordFirstRow();
            }
            return;
        }
        if (!gstate.file_scheduler)
        {
            return;
        }
    }
}

InsertionOrderPreservingMap<string> RootScanExplain::Bound(TableFunctionToStringInput& input) const
{
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data)
    {
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

InsertionOrderPreservingMap<string> RootScanExplain::Running(TableFunctionGetMetricsInput& input) const
{
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data)
    {
        const auto& bind = input.bind_data->Cast<RootScanBindData>();
        result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.root_access.reader_mode);
        result["ROOT Input Files"] = std::to_string(bind.root_paths.size());
        result["ROOT Representative"] = bind.root_path;
        result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    }
    if (!input.global_state)
    {
        return result;
    }
    const auto& global = input.global_state->Cast<RootScanGlobalState>();
    // Report the admitted scan parallelism, not just DuckDB's SET threads.
    result["ROOT Worker Limit"] = std::to_string(global.MaxThreads());
    result["ROOT Entry Prune Begin"] = std::to_string(global.entry_lower);
    result["ROOT Entry Prune End"] = global.entry_upper == std::numeric_limits<uint64_t>::max()
                                         ? "unbounded"
                                         : std::to_string(global.entry_upper + 1);
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entries.load());
    result["Serialized Page Calls"] = std::to_string(global.serialized_page_calls.load());
    result["Serialized Multi-Column Pages"] = std::to_string(global.serialized_multi_column_pages.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Nested Fixed Direct Entries"] = std::to_string(global.nested_fixed_direct_entries.load());
    result["Nested Fixed Scratch Entries"] = std::to_string(global.nested_fixed_scratch_entries.load());
    result["Object Validation Entries"] = std::to_string(global.object_validation_entries.load());
    result["Object Fallback Entries"] = std::to_string(global.object_fallback_entries.load());
    if (global.file_scheduler)
    {
        result["ROOT Opened Files"] = std::to_string(global.file_scheduler->OpenedFiles());
        result["ROOT Completed Files"] = std::to_string(global.file_scheduler->CompletedFiles());
        result["ROOT Completed File Read Calls"] = std::to_string(global.file_scheduler->PhysicalReadCalls());
        result["ROOT Completed File Read Bytes"] = std::to_string(global.file_scheduler->PhysicalReadBytes());
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
