#include "root4duckdb/dataset/root_dataset_pruning.hpp"

#include <limits>

namespace duckdb::rootlake
{

void DatasetScanExecutor::ValidateAccessPlan(const SchemaBinding& schema, const std::vector<PathLevel>& actual) const
{
    if (actual.size() != schema.expected_levels.size())
    {
        throw IOException("ROOT access plan depth differs from indexed schema " + schema.schema_id);
    }
    for (idx_t i = 0; i < actual.size(); ++i)
    {
        const auto& expected = schema.expected_levels[i];
        if (actual[i].name != expected.name || PrimitiveBaseType(actual[i].type) != PrimitiveBaseType(expected.type) ||
            actual[i].offset_in_parent != expected.offset_in_parent || actual[i].is_pointer != expected.is_pointer ||
            actual[i].is_container != expected.is_container || actual[i].is_fixed_array != expected.is_fixed_array ||
            actual[i].fixed_array_length != expected.fixed_array_length ||
            actual[i].array_dimensions != expected.array_dimensions || actual[i].element_size != expected.element_size)
        {
            throw IOException("ROOT streamer layout changed at level " + std::to_string(i) + " for schema " +
                              schema.schema_id);
        }
    }
}

void DatasetScanExecutor::SyncSerializedCounters(DatasetLocalState& local, DatasetGlobalState& global) const
{
    const auto& counters = local.path_reader.SerializedCounters();
    if (counters.baskets >= local.reported_serialized_baskets)
    {
        global.serialized_baskets.fetch_add(counters.baskets - local.reported_serialized_baskets);
    }
    if (counters.compressed_bytes >= local.reported_serialized_compressed_bytes)
    {
        global.serialized_compressed_bytes.fetch_add(counters.compressed_bytes -
                                                     local.reported_serialized_compressed_bytes);
    }
    if (counters.serialized_bytes >= local.reported_serialized_entry_bytes)
    {
        global.serialized_entry_bytes.fetch_add(counters.serialized_bytes - local.reported_serialized_entry_bytes);
    }
    local.reported_serialized_baskets = counters.baskets;
    local.reported_serialized_compressed_bytes = counters.compressed_bytes;
    if (counters.nested_fixed_direct_entries >= local.reported_nested_fixed_direct)
    {
        global.nested_fixed_direct_entries.fetch_add(counters.nested_fixed_direct_entries -
                                                     local.reported_nested_fixed_direct);
    }
    if (counters.nested_fixed_scratch_entries >= local.reported_nested_fixed_scratch)
    {
        global.nested_fixed_scratch_entries.fetch_add(counters.nested_fixed_scratch_entries -
                                                      local.reported_nested_fixed_scratch);
    }
    local.reported_nested_fixed_direct = counters.nested_fixed_direct_entries;
    local.reported_nested_fixed_scratch = counters.nested_fixed_scratch_entries;
    local.reported_serialized_entry_bytes = counters.serialized_bytes;
}

bool DatasetScanExecutor::PrefetchPhysicalRange(TFile* file, uint64_t offset, uint64_t size,
                                                DatasetGlobalState& global) const
{
    if (!file)
    {
        return false;
    }
    constexpr uint64_t max_chunk = static_cast<uint64_t>(std::numeric_limits<Int_t>::max());
    constexpr uint64_t max_offset = static_cast<uint64_t>(std::numeric_limits<Long64_t>::max());
    while (size)
    {
        const auto chunk_bytes = std::min<uint64_t>(size, max_chunk);
        if (offset > max_offset || chunk_bytes > max_offset - offset)
        {
            global.prefetch_hint_rejections.fetch_add(1);
            return false;
        }
        const auto chunk = static_cast<Int_t>(chunk_bytes);
        global.prefetch_hint_calls.fetch_add(1);
        global.prefetch_hint_bytes.fetch_add(chunk_bytes);
        if (file->ReadBufferAsync(static_cast<Long64_t>(offset), chunk))
        {
            global.prefetch_hint_rejections.fetch_add(1);
            return false;
        }
        offset += chunk_bytes;
        size -= chunk_bytes;
    }
    return true;
}

bool DatasetScanExecutor::ClaimTask(const DatasetBindData& bind, DatasetGlobalState& global,
                                    DatasetLocalState& local) const
{
    while (true)
    {
        if (local.next_task_in_group >= local.task_group_end)
        {
            const auto group_index = global.next_group.fetch_add(1);
            if (group_index >= global.task_groups.size())
            {
                return false;
            }
            local.next_task_in_group = global.task_groups[group_index].begin;
            local.next_prefetch_task_in_group = local.next_task_in_group;
            local.task_group_end = global.task_groups[group_index].end;
            local.file_cache_bytes = global.task_groups[group_index].cache_bytes;
        }
        const auto task_index = local.next_task_in_group++;
        local.current_task = global.tasks[task_index];
        if (global.has_event_range)
        {
            const uint64_t task_event_begin = local.current_task.event_base + local.current_task.entry_begin;
            const uint64_t task_event_end = local.current_task.event_base + local.current_task.entry_end;
            const uint64_t wanted_begin = std::max(task_event_begin, global.event_lower);
            const uint64_t wanted_end_exclusive = global.event_upper == std::numeric_limits<uint64_t>::max()
                                                      ? task_event_end
                                                      : std::min(task_event_end, global.event_upper + 1);
            if (wanted_begin >= wanted_end_exclusive)
            {
                global.skipped_entries.fetch_add(local.current_task.entry_end - local.current_task.entry_begin);
                continue;
            }
            const uint64_t old_begin = local.current_task.entry_begin;
            const uint64_t old_end = local.current_task.entry_end;
            local.current_task.entry_begin = wanted_begin - local.current_task.event_base;
            local.current_task.entry_end = wanted_end_exclusive - local.current_task.event_base;
            global.skipped_entries.fetch_add((local.current_task.entry_begin - old_begin) +
                                             (old_end - local.current_task.entry_end));
        }
        if (global.local_entry_range.known)
        {
            const uint64_t old_begin = local.current_task.entry_begin;
            const uint64_t old_end = local.current_task.entry_end;
            const uint64_t lower = global.local_entry_range.lower;
            const uint64_t upper_exclusive = global.local_entry_range.upper == std::numeric_limits<uint64_t>::max()
                                                 ? old_end
                                                 : std::min(old_end, global.local_entry_range.upper + 1);
            local.current_task.entry_begin = std::max(old_begin, lower);
            local.current_task.entry_end = upper_exclusive;
            if (local.current_task.entry_begin >= local.current_task.entry_end)
            {
                global.skipped_entries.fetch_add(old_end - old_begin);
                continue;
            }
            global.skipped_entries.fetch_add((local.current_task.entry_begin - old_begin) +
                                             (old_end - local.current_task.entry_end));
        }
        const bool new_file_context = !local.file || local.open_uri != local.current_task.root_uri ||
                                      local.open_schema != local.current_task.schema_id;
        OpenTaskFile(bind, global, local, local.current_task);
        if (new_file_context)
        {
            local.next_prefetch_task_in_group = task_index;
            local.prefetch_hints_accepted = true;
        }
        if (bind.prefetch_ranges && local.prefetch_hints_accepted)
        {
            const idx_t available = local.task_group_end - task_index;
            const idx_t lookahead = std::min<idx_t>(available, static_cast<idx_t>(bind.prefetch_depth) + 1);
            const idx_t prefetch_end = task_index + lookahead;
            const idx_t prefetch_begin = std::max(task_index, local.next_prefetch_task_in_group);
            for (idx_t index = prefetch_begin; index < prefetch_end; ++index)
            {
                const auto& next = global.tasks[index];
                if (!PrefetchPhysicalRange(local.file.get(), next.physical_offset, next.compressed_size, global))
                {
                    local.prefetch_hints_accepted = false;
                    break;
                }
            }
            local.next_prefetch_task_in_group = prefetch_end;
        }
        local.current_entry = local.current_task.entry_begin;
        local.values.clear();
        local.value_offset = 0;
        local.has_task = true;
        // The ROOT tree cache owns cluster-range state for the lifetime of this file.
        // Basket-sized range resets here make FillBuffer refill inside an existing cluster.
        // Pruning is already enforced by the indexed task and entry cursors.
        return true;
    }
}

bool DatasetScanExecutor::PassesPathPredicates(const DatasetBindData& bind, DatasetLocalState& local,
                                               RootEntryReader& object_entry, uint64_t entry) const
{
    for (idx_t predicate_index = 0; predicate_index < bind.path_predicates.size(); ++predicate_index)
    {
        auto& predicate_reader = local.predicate_readers[predicate_index];
        std::vector<RootPrimitiveValue> values;
        std::vector<int32_t> unused_indices;
        bool serialized = false;
        if (predicate_reader.SerializedActive())
        {
            // Preserve exact signed/unsigned/float types and event-grain any/all semantics.
            const auto read = predicate_reader.TryReadSerialized(entry, object_entry, values, unused_indices, false);
            serialized = read.Serialized();
        }
        if (!serialized)
        {
            void* object = object_entry.Read();
            if (!object)
            {
                return false;
            }
            ReadResult result;
            OffsetValueReader::CollectDirect(object, local.predicate_levels[predicate_index], -1, 0, result);
            values = std::move(result.numbers);
        }
        if (!PathPredicateEventMatches(bind.path_predicates[predicate_index], values))
        {
            return false;
        }
    }
    return true;
}

bool DatasetScanExecutor::LoadNextEntry(ClientContext& context, const DatasetBindData& bind, DatasetLocalState& local,
                                        DatasetGlobalState& global) const
{

    const bool typed_transport = RequiresTypedDatasetValue(bind.value_type);
    // Cursor pages are bounded independently of the number of values in an entry.
    auto consume_page = [&]() -> bool
    {
        local.values.clear();
        local.typed_values.clear();
        local.flat_indices.clear();
        const auto source =
            local.path_reader.ReadNumericPage(STANDARD_VECTOR_SIZE, local.typed_values, local.flat_indices);
        if (source == RootNumericPageSource::NONE)
        {
            local.path_reader.EndNumericPage();
            return false;
        }
        const auto decoded = local.typed_values.size();
        if (!typed_transport)
        {
            local.values.reserve(decoded);
            for (const auto& value : local.typed_values)
            {
                local.values.push_back(value.AsDouble());
            }
            local.typed_values.clear();
        }
        if (source == RootNumericPageSource::SERIALIZED)
        {
            SyncSerializedCounters(local, global);
            global.serialized_page_calls.fetch_add(1);
            global.serialized_values.fetch_add(decoded);
        }
        global.decoded_values.fetch_add(decoded);
        local.value_offset = 0;
        return decoded != 0;
    };
    if (local.path_reader.HasNumericPage() && consume_page())
    {
        return true;
    }
    while (local.current_entry < local.current_task.entry_end)
    {
        const auto entry = local.current_entry++;
        if (entry > std::numeric_limits<uint64_t>::max() - local.current_task.event_base)
        {
            throw IOException("Indexed ROOT event_fk exceeds UINT64 range");
        }
        if (!PassesDatasetEntryFilters(context, local, bind, global, entry))
        {
            continue;
        }
        RootEntryReader object_entry(local.object_reader);
        object_entry.Begin(entry);
        if (!PassesPathPredicates(bind, local, object_entry, entry))
        {
            global.get_entry_calls.fetch_add(object_entry.LoadCount());
            continue;
        }
        global.get_entry_calls.fetch_add(object_entry.LoadCount());
        local.values.clear();
        const auto prior_loads = object_entry.LoadCount();
        const auto started =
            local.path_reader.BeginNumericEntry(entry, object_entry, 0, local.typed_values, local.flat_indices);
        global.get_entry_calls.fetch_add(object_entry.LoadCount() - prior_loads);
        SyncSerializedCounters(local, global);
        if (started.fallback_activated)
        {
            global.fallback_files.fetch_add(1);
        }
        if (started.attempt_decoded)
        {
            global.serialized_entry_calls.fetch_add(1);
            global.serialized_values.fetch_add(local.typed_values.size());
        }
        if (started.route == RootNumericEntryRoute::NONE)
        {
            continue;
        }
        local.value_event_fk = local.current_task.event_base + entry;
        local.value_entry_id = entry;
        local.value_source_id = local.current_task.file_id;
        local.value_offset = 0;
        if (started.route == RootNumericEntryRoute::SERIALIZED_VALUES)
        {
            const idx_t decoded_count = local.typed_values.size();
            if (!typed_transport)
            {
                local.values.reserve(decoded_count);
                for (const auto& value : local.typed_values)
                {
                    local.values.push_back(value.AsDouble());
                }
                local.typed_values.clear();
            }
            global.decoded_values.fetch_add(decoded_count);
            if (decoded_count != 0)
            {
                return true;
            }
            continue;
        }
        if (started.route == RootNumericEntryRoute::SERIALIZED_PAGE)
        {
            global.serialized_entry_calls.fetch_add(1);
        }
        if (consume_page())
        {
            return true;
        }
    }
    return false;
}

void DatasetScanExecutor::SetDoubleAsType(Vector& vector, idx_t row, const LogicalType& type, double value) const
{
    switch (type.id())
    {
    case LogicalTypeId::BOOLEAN:
        FlatVector::GetDataMutable<bool>(vector)[row] = value != 0;
        break;
    case LogicalTypeId::TINYINT:
        FlatVector::GetDataMutable<int8_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<int8_t>();
        break;
    case LogicalTypeId::UTINYINT:
        FlatVector::GetDataMutable<uint8_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<uint8_t>();
        break;
    case LogicalTypeId::SMALLINT:
        FlatVector::GetDataMutable<int16_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<int16_t>();
        break;
    case LogicalTypeId::USMALLINT:
        FlatVector::GetDataMutable<uint16_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<uint16_t>();
        break;
    case LogicalTypeId::INTEGER:
        FlatVector::GetDataMutable<int32_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<int32_t>();
        break;
    case LogicalTypeId::UINTEGER:
        FlatVector::GetDataMutable<uint32_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<uint32_t>();
        break;
    case LogicalTypeId::BIGINT:
        FlatVector::GetDataMutable<int64_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<int64_t>();
        break;
    case LogicalTypeId::UBIGINT:
        FlatVector::GetDataMutable<uint64_t>(vector)[row] = RootPrimitiveValue::Floating(value).AsInteger<uint64_t>();
        break;
    case LogicalTypeId::FLOAT:
        FlatVector::GetDataMutable<float>(vector)[row] = RootPrimitiveValue::Floating(value).AsFloat();
        break;
    case LogicalTypeId::DOUBLE:
        FlatVector::GetDataMutable<double>(vector)[row] = value;
        break;
    default:
        throw NotImplementedException("Unsupported ROOT dataset output type " + type.ToString());
    }
    FlatVector::ValidityMutable(vector).SetValid(row);
}

void DatasetScanExecutor::SetPrimitiveAsType(Vector& vector, idx_t row, const LogicalType& type,
                                             const RootPrimitiveValue& value) const
{

    switch (type.id())
    {
    case LogicalTypeId::BIGINT:
        FlatVector::GetDataMutable<int64_t>(vector)[row] = value.AsSigned();
        break;
    case LogicalTypeId::UBIGINT:
        FlatVector::GetDataMutable<uint64_t>(vector)[row] = value.AsUnsigned();
        break;
    default:
        throw InternalException("Typed dataset writer used for non-64-bit type " + type.ToString());
    }
    FlatVector::ValidityMutable(vector).SetValid(row);
}

void DatasetScanExecutor::EmitProjectedRow(const DatasetBindData& bind, const DatasetGlobalState& global,
                                           DataChunk& output, idx_t output_row, uint64_t event_fk,
                                           const std::string& source_id, uint64_t entry_id, double numeric_value,
                                           const int32_t* indices, idx_t index_count) const
{
    for (idx_t output_col = 0; output_col < global.output_column_ids.size(); ++output_col)
    {
        const auto full_col = global.output_column_ids[output_col];
        auto& vector = output.data[output_col];
        if (full_col == COLUMN_IDENTIFIER_EMPTY)
        {
            // DuckDB requests a dummy column for COUNT(*); only the chunk cardinality matters.
            FlatVector::ValidityMutable(vector).SetInvalid(output_row);
        }
        else if (full_col == COLUMN_IDENTIFIER_ROW_ID || full_col == 0)
        {
            FlatVector::GetDataMutable<uint64_t>(vector)[output_row] = event_fk;
            FlatVector::ValidityMutable(vector).SetValid(output_row);
        }
        else if (full_col >= 1 && static_cast<idx_t>(full_col) < bind.value_column)
        {
            const idx_t index_position = static_cast<idx_t>(full_col) - 1;
            if (index_position < index_count)
            {
                FlatVector::GetDataMutable<int32_t>(vector)[output_row] = indices[index_position];
                FlatVector::ValidityMutable(vector).SetValid(output_row);
            }
            else
            {
                FlatVector::ValidityMutable(vector).SetInvalid(output_row);
            }
        }
        else if (static_cast<idx_t>(full_col) == bind.value_column)
        {
            SetDoubleAsType(vector, output_row, bind.value_type, numeric_value);
        }
        else if (static_cast<idx_t>(full_col) == bind.source_id_column)
        {
            FlatVector::GetDataMutable<string_t>(vector)[output_row] = StringVector::AddString(vector, source_id);
            FlatVector::ValidityMutable(vector).SetValid(output_row);
        }
        else if (static_cast<idx_t>(full_col) == bind.entry_id_column)
        {
            FlatVector::GetDataMutable<uint64_t>(vector)[output_row] = entry_id;
            FlatVector::ValidityMutable(vector).SetValid(output_row);
        }
        else
        {
            FlatVector::ValidityMutable(vector).SetInvalid(output_row);
        }
    }
}

unique_ptr<LocalTableFunctionState> DatasetScanStateFactory::CreateLocal() const
{
    auto local = make_uniq<DatasetLocalState>();
    return local;
}

void DatasetScanExecutor::EmitProjectedTypedRow(const DatasetBindData& bind, const DatasetGlobalState& global,
                                                DataChunk& output, idx_t output_row, uint64_t event_fk,
                                                const std::string& source_id, uint64_t entry_id,
                                                const RootPrimitiveValue& numeric_value, const int32_t* indices,
                                                idx_t index_count) const
{

    for (idx_t output_col = 0; output_col < global.output_column_ids.size(); ++output_col)
    {
        const auto full_col = global.output_column_ids[output_col];
        auto& vector = output.data[output_col];
        if (full_col == COLUMN_IDENTIFIER_EMPTY)
        {
            FlatVector::ValidityMutable(vector).SetInvalid(output_row);
        }
        else if (full_col == COLUMN_IDENTIFIER_ROW_ID || full_col == 0)
        {
            FlatVector::GetDataMutable<uint64_t>(vector)[output_row] = event_fk;
            FlatVector::ValidityMutable(vector).SetValid(output_row);
        }
        else if (full_col >= 1 && static_cast<idx_t>(full_col) < bind.value_column)
        {
            const idx_t index_position = static_cast<idx_t>(full_col) - 1;
            if (index_position < index_count)
            {
                FlatVector::GetDataMutable<int32_t>(vector)[output_row] = indices[index_position];
                FlatVector::ValidityMutable(vector).SetValid(output_row);
            }
            else
            {
                FlatVector::ValidityMutable(vector).SetInvalid(output_row);
            }
        }
        else if (static_cast<idx_t>(full_col) == bind.value_column)
        {
            SetPrimitiveAsType(vector, output_row, bind.value_type, numeric_value);
        }
        else if (static_cast<idx_t>(full_col) == bind.source_id_column)
        {
            FlatVector::GetDataMutable<string_t>(vector)[output_row] = StringVector::AddString(vector, source_id);
            FlatVector::ValidityMutable(vector).SetValid(output_row);
        }
        else if (static_cast<idx_t>(full_col) == bind.entry_id_column)
        {
            FlatVector::GetDataMutable<uint64_t>(vector)[output_row] = entry_id;
            FlatVector::ValidityMutable(vector).SetValid(output_row);
        }
        else
        {
            FlatVector::ValidityMutable(vector).SetInvalid(output_row);
        }
    }
}

idx_t DatasetScanExecutor::ReserveOutputRows(DatasetGlobalState& global, idx_t requested) const
{
    if (requested == 0 || global.stop_requested.load(std::memory_order_relaxed))
    {
        return 0;
    }
    if (global.row_limit == std::numeric_limits<uint64_t>::max())
    {
        global.emitted_rows.fetch_add(requested, std::memory_order_relaxed);
        return requested;
    }
    auto current = global.emitted_rows.load(std::memory_order_relaxed);
    while (current < global.row_limit)
    {
        const auto remaining = global.row_limit - current;
        const auto granted = static_cast<uint64_t>(std::min<uint64_t>(remaining, requested));
        if (global.emitted_rows.compare_exchange_weak(current, current + granted, std::memory_order_relaxed))
        {
            if (current + granted >= global.row_limit)
            {
                global.stop_requested.store(true, std::memory_order_relaxed);
            }
            return static_cast<idx_t>(granted);
        }
    }
    global.stop_requested.store(true, std::memory_order_relaxed);
    return 0;
}

void DatasetScanExecutor::Scan(ClientContext& context, TableFunctionInput& input, DataChunk& output) const
{
    auto& bind = input.bind_data->Cast<DatasetBindData>();
    auto& global = input.global_state->Cast<DatasetGlobalState>();
    auto& local = input.local_state->Cast<DatasetLocalState>();
    idx_t output_count = 0;
    // Unfiltered COUNT(*) can use exact basket cardinalities without ROOT reads.
    if (global.metadata_count_only)
    {
        const auto already_emitted = global.metadata_rows_emitted.load(std::memory_order_relaxed);
        if (already_emitted < global.metadata_total_rows)
        {
            const auto wanted = static_cast<idx_t>(
                std::min<uint64_t>(STANDARD_VECTOR_SIZE, global.metadata_total_rows - already_emitted));
            const auto emit_count = ReserveOutputRows(global, wanted);
            if (!output.data.empty())
            {
                auto& dummy = output.data[0];
                for (idx_t row = 0; row < emit_count; ++row)
                {
                    FlatVector::ValidityMutable(dummy).SetInvalid(row);
                }
            }
            output_count = emit_count;
            global.metadata_rows_emitted.fetch_add(emit_count, std::memory_order_relaxed);
        }
        output.SetCardinalityUnsafe(output_count);
        return;
    }
    const bool typed_transport = RequiresTypedDatasetValue(bind.value_type);
    while (output_count < STANDARD_VECTOR_SIZE)
    {
        if (!local.has_task)
        {
            if (!ClaimTask(bind, global, local))
            {
                break;
            }
        }
        const idx_t available = typed_transport ? local.typed_values.size() : local.values.size();
        if (local.value_offset >= available)
        {
            if (!LoadNextEntry(context, bind, local, global))
            {
                local.has_task = false;
                global.completed_tasks.fetch_add(1);
                continue;
            }
        }
        const idx_t loaded = typed_transport ? local.typed_values.size() : local.values.size();
        while (local.value_offset < loaded && output_count < STANDARD_VECTOR_SIZE)
        {
            const auto value_index = local.value_offset++;
            const idx_t index_count = bind.index_names.size();
            const int32_t* indices = index_count ? local.flat_indices.data() + value_index * index_count : nullptr;
            if (typed_transport)
            {
                const auto& numeric_value = local.typed_values[value_index];
                if (!PassesTypedFilters(context, local, bind, global, local.value_event_fk, numeric_value, indices,
                                        index_count))
                {
                    continue;
                }
                if (ReserveOutputRows(global, 1) == 0)
                {
                    output.SetCardinalityUnsafe(output_count);
                    return;
                }
                EmitProjectedTypedRow(bind, global, output, output_count++, local.value_event_fk, local.value_source_id,
                                      local.value_entry_id, numeric_value, indices, index_count);
            }
            else
            {
                const auto numeric_value = local.values[value_index];
                if (!PassesFilters(context, local, bind, global, local.value_event_fk, numeric_value, indices,
                                   index_count))
                {
                    continue;
                }
                if (ReserveOutputRows(global, 1) == 0)
                {
                    output.SetCardinalityUnsafe(output_count);
                    return;
                }
                EmitProjectedRow(bind, global, output, output_count++, local.value_event_fk, local.value_source_id,
                                 local.value_entry_id, numeric_value, indices, index_count);
            }
        }
    }
    output.SetCardinalityUnsafe(output_count);
}

InsertionOrderPreservingMap<string> DatasetScanExplain::Bound(TableFunctionToStringInput& input) const
{
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data)
    {
        return result;
    }
    const auto& bind = input.bind_data->Cast<DatasetBindData>();
    result["Logical Path"] = bind.logical_path;
    result["Value Type"] = bind.value_type.ToString();
    result["Index Columns"] = bind.index_names.empty() ? "none" : JoinStrings(bind.index_names, ", ");
    result["Metadata Source"] = bind.sources.sql_tables ? "SQL/Iceberg relations" : bind.catalog_path;
    if (!bind.sources.snapshot_id.empty())
    {
        result["Snapshot"] = bind.sources.snapshot_id;
    }
    result["Filter Pushdown"] = "zonemap + optional Bloom + exact in-scan evaluation";
    result["Projection Pushdown"] = "enabled";
    result["ROOT Decode Mode"] = RootReaderModeName(bind.root_access.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.root_access.validation_entries);
    result["Path Predicate Indexes"] = std::to_string(bind.path_predicates.size());
    result["Explicit Entry Selections"] = std::to_string(bind.entry_selection.size());
    result["Estimated Rows"] = std::to_string(bind.estimated_cardinality);
    result["ROOT Tree Cache Policy"] = bind.tree_cache_explicit ? "explicit" : "adaptive";
    if (bind.row_limit != std::numeric_limits<uint64_t>::max())
    {
        result["Row Limit"] = std::to_string(bind.row_limit);
    }
    return result;
}

InsertionOrderPreservingMap<string> DatasetScanExplain::Running(TableFunctionGetMetricsInput& input) const
{
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data)
    {
        const auto& bind = input.bind_data->Cast<DatasetBindData>();
        result["Logical Path"] = bind.logical_path;
        result["Value Type"] = bind.value_type.ToString();
        result["Index Columns"] = bind.index_names.empty() ? "none" : JoinStrings(bind.index_names, ", ");
        result["Metadata Source"] = bind.sources.sql_tables ? "SQL/Iceberg relations" : bind.catalog_path;
        result["ROOT Decode Mode"] = RootReaderModeName(bind.root_access.reader_mode);
        if (!bind.sources.snapshot_id.empty())
        {
            result["Snapshot"] = bind.sources.snapshot_id;
        }
        result["Path Predicate Indexes"] = std::to_string(bind.path_predicates.size());
        result["Estimated Rows"] = std::to_string(bind.estimated_cardinality);
        result["ROOT Tree Cache Policy"] = bind.tree_cache_explicit ? "explicit" : "adaptive";
    }
    if (!input.global_state)
    {
        return result;
    }
    const auto& global = input.global_state->Cast<DatasetGlobalState>();
    result["Predicate Index Baskets"] = std::to_string(global.predicate_index_baskets);
    result["Predicate Intersections"] = std::to_string(global.predicate_intersections);
    result["Planning Time (us)"] = std::to_string(global.planning_time_us);
    result["Bloom Metadata Bytes"] = std::to_string(global.bloom_metadata_bytes);
    result["Catalog ROOT Files"] = std::to_string(global.catalog_files);
    result["Selected ROOT Files"] = std::to_string(global.planned_files);
    result["Skipped ROOT Files"] = std::to_string(global.skipped_files);
    result["Catalog Baskets"] = std::to_string(global.catalog_baskets);
    result["Selected Baskets"] = std::to_string(global.planned_baskets);
    result["Skipped Baskets"] = std::to_string(global.skipped_baskets);
    result["Worker Limit"] = std::to_string(global.worker_limit);
    result["Stop Requested"] = global.stop_requested.load() ? "true" : "false";
    result["Opened ROOT Files"] = std::to_string(global.opened_files.load());
    result["Reused Open Files"] = std::to_string(global.reused_open_files.load());
    result["Prefetch Hint Calls"] = std::to_string(global.prefetch_hint_calls.load());
    result["Prefetch Hint Rejections"] = std::to_string(global.prefetch_hint_rejections.load());
    result["Prefetch Hint Bytes"] = std::to_string(global.prefetch_hint_bytes.load());
    result["GetEntry Calls"] = std::to_string(global.get_entry_calls.load());
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entry_calls.load());
    result["Serialized Page Calls"] = std::to_string(global.serialized_page_calls.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Nested Fixed Direct Entries"] = std::to_string(global.nested_fixed_direct_entries.load());
    result["Nested Fixed Scratch Entries"] = std::to_string(global.nested_fixed_scratch_entries.load());
    result["Projected ROOT Files"] = std::to_string(global.projected_files.load());
    result["Object Fallback Files"] = std::to_string(global.fallback_files.load());
    result["Coalesced Read Tasks"] = std::to_string(global.tasks.size());
    result["File-affinity Groups"] = std::to_string(global.task_groups.size());
    result["ROOT Cache Enabled Files"] = std::to_string(global.tree_cache_enabled_files);
    result["ROOT Cache Disabled Files"] = std::to_string(global.tree_cache_disabled_files);
    result["Selected Basket Bytes"] = std::to_string(global.planned_compressed_bytes);
    result["Scheduled Range Bytes"] = std::to_string(global.scheduled_read_bytes);
    result["Indexed Values in Tasks"] = std::to_string(global.planned_values);
    result["Skipped ROOT Entries"] = std::to_string(global.skipped_entries.load());
    result["Metadata-only Rows"] = std::to_string(global.metadata_rows_emitted.load());
    if (global.metadata_count_only)
    {
        result["Metadata Cardinality"] = std::to_string(global.metadata_total_rows);
    }
    result["Decoded Values"] = std::to_string(global.decoded_values.load());
    result["Emitted Rows"] = std::to_string(global.emitted_rows.load());
    return result;
}

double DatasetScanExecutor::Progress(const GlobalTableFunctionState* state) const
{
    if (!state)
    {
        return 0.0;
    }
    const auto& global = state->Cast<DatasetGlobalState>();
    if (global.tasks.empty())
    {
        return 100.0;
    }
    const auto completed = std::min<idx_t>(global.completed_tasks.load(), global.tasks.size());
    return 100.0 * static_cast<double>(completed) / static_cast<double>(global.tasks.size());
}

} // namespace duckdb::rootlake
