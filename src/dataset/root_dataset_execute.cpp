#include "root4duckdb/dataset/root_dataset_scan_internal.hpp"

#include "root4duckdb/core/root_file_opener.hpp"

namespace duckdb::rootlake {

void DatasetScanExecutor::ValidateAccessPlan(const SchemaBinding& schema, const std::vector<PathLevel>& actual) const {
    if (actual.size() != schema.expected_levels.size()) {
        throw IOException("ROOT access plan depth differs from indexed schema " + schema.schema_id);
    }
    for (idx_t i = 0; i < actual.size(); ++i) {
        const auto& expected = schema.expected_levels[i];
        if (actual[i].name != expected.name || PrimitiveBaseType(actual[i].type) != PrimitiveBaseType(expected.type) ||
            actual[i].offset_in_parent != expected.offset_in_parent || actual[i].is_pointer != expected.is_pointer ||
            actual[i].is_container != expected.is_container || actual[i].is_fixed_array != expected.is_fixed_array ||
            actual[i].fixed_array_length != expected.fixed_array_length ||
            actual[i].array_dimensions != expected.array_dimensions ||
            actual[i].element_size != expected.element_size) {
            throw IOException("ROOT streamer layout changed at level " + std::to_string(i) + " for schema " +
                              schema.schema_id);
        }
    }
}

void DatasetScanExecutor::SyncSerializedCounters(DatasetLocalState& local, DatasetGlobalState& global) const {
    const auto& counters = local.path_reader.SerializedCounters();
    if (counters.baskets >= local.reported_serialized_baskets) {
        global.serialized_baskets.fetch_add(counters.baskets - local.reported_serialized_baskets);
    }
    if (counters.compressed_bytes >= local.reported_serialized_compressed_bytes) {
        global.serialized_compressed_bytes.fetch_add(counters.compressed_bytes -
                                                     local.reported_serialized_compressed_bytes);
    }
    if (counters.serialized_bytes >= local.reported_serialized_entry_bytes) {
        global.serialized_entry_bytes.fetch_add(counters.serialized_bytes - local.reported_serialized_entry_bytes);
    }
    local.reported_serialized_baskets = counters.baskets;
    local.reported_serialized_compressed_bytes = counters.compressed_bytes;
    local.reported_serialized_entry_bytes = counters.serialized_bytes;
}

void DatasetScanExecutor::OpenTaskFile(const DatasetBindData& bind, DatasetGlobalState& global,
                                       DatasetLocalState& local, const RootBasketTask& task) {
    if (local.open_uri == task.root_uri && local.open_schema == task.schema_id && local.file) {
        global.reused_open_files.fetch_add(1);
        return;
    }
    SyncSerializedCounters(local, global);
    local.path_reader.Reset();
    local.object_reader.Reset();
    local.reported_serialized_baskets = 0;
    local.reported_serialized_compressed_bytes = 0;
    local.reported_serialized_entry_bytes = 0;
    auto opened = OpenRootFile(task.root_uri);
    global.opened_files.fetch_add(1);
    if (!opened) {
        throw IOException("Failed to open ROOT file: " + task.root_uri + ": " + opened.error);
    }
    local.file = std::move(opened.file);
    auto schema_it = bind.schema_lookup.find(task.schema_id);
    if (schema_it == bind.schema_lookup.end()) {
        throw IOException("Unknown schema_id in task: " + task.schema_id);
    }
    const auto& schema = bind.schemas[schema_it->second];
    local.object_reader.Bind(local.file.get(), task.tree_name, schema.root_class,
                             bind.root_access.dictionary_cleanup_mode);
    auto parsed = ParsePath(bind.logical_path);
    auto* root_class = local.object_reader.RootClass();
    auto* tree = local.object_reader.Tree();
    auto* object_branch = local.object_reader.ObjectBranch();
    auto full_levels = PathResolver::Resolve(root_class, parsed.fields);
    ValidateAccessPlan(schema, full_levels);

    local.predicate_levels.clear();
    local.predicate_levels.reserve(bind.path_predicates.size());
    for (const auto& predicate : bind.path_predicates) {
        const auto predicate_path = ParsePath(predicate.path);
        if (predicate_path.root_class != schema.root_class) {
            throw IOException("Predicate path root class differs from target path root class: " + predicate.path);
        }
        auto predicate_levels = PathResolver::Resolve(root_class, predicate_path.fields);
        const auto predicate_schema_id = SchemaFingerprint(predicate_path.root_class, predicate_levels);
        const auto predicate_schema_it = predicate.schema_lookup.find(predicate_schema_id);
        if (predicate_schema_it == predicate.schema_lookup.end()) {
            throw IOException("ROOT file streamer schema is absent from predicate index for " + predicate.path);
        }
        ValidateAccessPlan(predicate.schemas[predicate_schema_it->second], predicate_levels);
        local.predicate_levels.push_back(std::move(predicate_levels));
    }
    local.path_reader.Resolve(tree, object_branch, root_class, std::move(parsed), std::move(full_levels));
    std::string serialized_rejection;
    if (!bind.path_predicates.empty()) {
        serialized_rejection = "path_predicates require universal object materialization";
    }

    bool projection_applied = false;
    if (bind.path_predicates.empty() && local.path_reader.PhysicalMode() == "ancestor") {
        const auto projection =
            ApplyBranchProjection(tree, {local.path_reader.PhysicalBranch()}, bind.root_access.tree_cache_bytes);
        projection_applied = projection.applied;
        if (projection_applied) {
            global.projected_files.fetch_add(1);
        }
    }
    if (!projection_applied) {
        EnableAllBranches(tree, bind.root_access.tree_cache_bytes);
    }

    auto reader_options = bind.root_access;
    reader_options.enable_all_branches_on_fallback = true;
    const auto started =
        local.path_reader.StartSerialized(std::move(reader_options), std::move(serialized_rejection));
    if (started.FallbackActivated()) {
        global.fallback_files.fetch_add(1);
    }

    const auto runtime_index_depth = local.path_reader.IndexDepth();
    if (runtime_index_depth != bind.index_names.size()) {
        throw IOException("ROOT container depth differs from the catalog output schema for " + bind.logical_path);
    }
    local.open_uri = task.root_uri;
    local.open_schema = task.schema_id;
}

void DatasetScanExecutor::PrefetchPhysicalRange(TFile* file, uint64_t offset, uint64_t size) const {
    if (!file || !size) {
        return;
    }
    constexpr uint64_t max_chunk = static_cast<uint64_t>(std::numeric_limits<Int_t>::max());
    while (size) {
        const auto chunk = static_cast<Int_t>(std::min<uint64_t>(size, max_chunk));
        file->ReadBufferAsync(static_cast<Long64_t>(offset), chunk);
        offset += static_cast<uint64_t>(chunk);
        size -= static_cast<uint64_t>(chunk);
    }
}

bool DatasetScanExecutor::ClaimTask(const DatasetBindData& bind, DatasetGlobalState& global, DatasetLocalState& local) {
    while (true) {
        if (local.next_task_in_group >= local.task_group_end) {
            const auto group_index = global.next_group.fetch_add(1);
            if (group_index >= global.task_groups.size()) {
                return false;
            }
            local.next_task_in_group = global.task_groups[group_index].begin;
            local.task_group_end = global.task_groups[group_index].end;
        }
        const auto task_index = local.next_task_in_group++;
        local.current_task = global.tasks[task_index];
        if (global.has_event_range) {
            const uint64_t task_event_begin = local.current_task.event_base + local.current_task.entry_begin;
            const uint64_t task_event_end = local.current_task.event_base + local.current_task.entry_end;
            const uint64_t wanted_begin = std::max(task_event_begin, global.event_lower);
            const uint64_t wanted_end_exclusive = global.event_upper == std::numeric_limits<uint64_t>::max()
                                                      ? task_event_end
                                                      : std::min(task_event_end, global.event_upper + 1);
            if (wanted_begin >= wanted_end_exclusive) {
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
        OpenTaskFile(bind, global, local, local.current_task);
        if (bind.prefetch_ranges) {
            PrefetchPhysicalRange(local.file.get(), local.current_task.physical_offset,
                                  local.current_task.compressed_size);
            for (uint32_t ahead = 1; ahead <= bind.prefetch_depth && task_index + ahead < local.task_group_end;
                 ++ahead) {
                const auto& next = global.tasks[task_index + ahead];
                PrefetchPhysicalRange(local.file.get(), next.physical_offset, next.compressed_size);
            }
        }
        local.current_entry = local.current_task.entry_begin;
        local.values.clear();
        local.value_offset = 0;
        local.has_task = true;
        if (bind.root_access.tree_cache_bytes) {
            local.object_reader.Tree()->SetCacheEntryRange(static_cast<Long64_t>(local.current_task.entry_begin),
                                                           static_cast<Long64_t>(local.current_task.entry_end));
        }
        return true;
    }
}

bool DatasetScanExecutor::PassesPathPredicates(const DatasetBindData& bind, DatasetLocalState& local,
                                               void* object) const {

    if (bind.path_predicates.empty()) {
        return true;
    }

    for (idx_t predicate_index = 0; predicate_index < bind.path_predicates.size(); ++predicate_index) {

        const auto& levels = local.predicate_levels[predicate_index];

        ReadResult result;

        OffsetValueReader::CollectDirect(object, levels, -1, 0, result);

        if (!PathPredicateEventMatches(bind.path_predicates[predicate_index], result.numbers)) {
            return false;
        }
    }

    return true;
}

bool DatasetScanExecutor::LoadNextEntry(const DatasetBindData& bind, DatasetLocalState& local,
                                        DatasetGlobalState& global) {

    const bool typed_transport = RequiresTypedDatasetValue(bind.value_type);

    while (local.current_entry < local.current_task.entry_end) {

        const auto entry = local.current_entry++;

        RootEntryReader object_entry(local.object_reader);

        object_entry.Begin(entry);

        if (local.path_reader.SerializedActive()) {
            RootPathReadResult read;

            if (typed_transport) {
                read = local.path_reader.TryReadSerialized(entry, object_entry, local.typed_values, local.flat_indices);
            } else {
                read = local.path_reader.TryReadSerialized(entry, object_entry, local.values, local.flat_indices);
            }

            SyncSerializedCounters(local, global);

            global.get_entry_calls.fetch_add(object_entry.LoadCount());

            if (read.FallbackActivated()) {
                global.fallback_files.fetch_add(1);
            }

            const idx_t decoded_count = typed_transport ? local.typed_values.size() : local.values.size();

            if (read.Decoded()) {
                global.serialized_entry_calls.fetch_add(1);
                global.serialized_values.fetch_add(decoded_count);
            }

            if (read.Serialized()) {
                global.decoded_values.fetch_add(decoded_count);

                local.value_offset = 0;
                local.value_event_fk = local.current_task.event_base + entry;

                local.value_entry_id = entry;
                local.value_source_id = local.current_task.file_id;

                if (decoded_count != 0) {
                    return true;
                }

                continue;
            }

            if (read.FallbackActivated() && object_entry.LoadCount()) {
                object_entry.Invalidate();
            }
        }

        const auto prior_loads = object_entry.LoadCount();

        auto* object = object_entry.Read();

        global.get_entry_calls.fetch_add(object_entry.LoadCount() - prior_loads);

        if (!object) {
            continue;
        }

        if (!PassesPathPredicates(bind, local, object)) {
            continue;
        }

        local.values.clear();
        local.typed_values.clear();
        local.flat_indices.clear();

        if (typed_transport) {
            local.path_reader.CollectTypedFlat(object, local.typed_values, local.flat_indices);

            global.decoded_values.fetch_add(local.typed_values.size());
        } else {
            local.path_reader.CollectFlat(object, local.values, local.flat_indices);

            global.decoded_values.fetch_add(local.values.size());
        }

        local.value_offset = 0;
        local.value_event_fk = local.current_task.event_base + entry;

        local.value_entry_id = entry;
        local.value_source_id = local.current_task.file_id;

        if (typed_transport ? !local.typed_values.empty() : !local.values.empty()) {
            return true;
        }
    }

    return false;
}

void DatasetScanExecutor::SetDoubleAsType(Vector& vector, idx_t row, const LogicalType& type, double value) const {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        FlatVector::GetData<bool>(vector)[row] = value != 0;
        break;
    case LogicalTypeId::TINYINT:
        FlatVector::GetData<int8_t>(vector)[row] = static_cast<int8_t>(value);
        break;
    case LogicalTypeId::UTINYINT:
        FlatVector::GetData<uint8_t>(vector)[row] = static_cast<uint8_t>(value);
        break;
    case LogicalTypeId::SMALLINT:
        FlatVector::GetData<int16_t>(vector)[row] = static_cast<int16_t>(value);
        break;
    case LogicalTypeId::USMALLINT:
        FlatVector::GetData<uint16_t>(vector)[row] = static_cast<uint16_t>(value);
        break;
    case LogicalTypeId::INTEGER:
        FlatVector::GetData<int32_t>(vector)[row] = static_cast<int32_t>(value);
        break;
    case LogicalTypeId::UINTEGER:
        FlatVector::GetData<uint32_t>(vector)[row] = static_cast<uint32_t>(value);
        break;
    case LogicalTypeId::BIGINT:
        FlatVector::GetData<int64_t>(vector)[row] = static_cast<int64_t>(value);
        break;
    case LogicalTypeId::UBIGINT:
        FlatVector::GetData<uint64_t>(vector)[row] = static_cast<uint64_t>(value);
        break;
    case LogicalTypeId::FLOAT:
        FlatVector::GetData<float>(vector)[row] = static_cast<float>(value);
        break;
    case LogicalTypeId::DOUBLE:
        FlatVector::GetData<double>(vector)[row] = value;
        break;
    default:
        throw NotImplementedException("Unsupported ROOT dataset output type " + type.ToString());
    }
    FlatVector::Validity(vector).SetValid(row);
}

void DatasetScanExecutor::SetPrimitiveAsType(Vector& vector, idx_t row, const LogicalType& type,
                                             const RootPrimitiveValue& value) const {

    switch (type.id()) {
    case LogicalTypeId::BIGINT:
        FlatVector::GetData<int64_t>(vector)[row] = value.AsSigned();
        break;

    case LogicalTypeId::UBIGINT:
        FlatVector::GetData<uint64_t>(vector)[row] = value.AsUnsigned();
        break;

    default:
        throw InternalException("Typed dataset writer used for non-64-bit type " + type.ToString());
    }

    FlatVector::Validity(vector).SetValid(row);
}

void DatasetScanExecutor::EmitProjectedRow(const DatasetBindData& bind, const DatasetGlobalState& global,
                                           DataChunk& output, idx_t output_row, uint64_t event_fk,
                                           const std::string& source_id, uint64_t entry_id, double numeric_value,
                                           const int32_t* indices, idx_t index_count) const {
    for (idx_t output_col = 0; output_col < global.output_column_ids.size(); ++output_col) {
        const auto full_col = global.output_column_ids[output_col];
        auto& vector = output.data[output_col];
        if (full_col == COLUMN_IDENTIFIER_EMPTY) {
            // DuckDB requests a dummy column for COUNT(*); only the chunk cardinality matters.
            FlatVector::Validity(vector).SetInvalid(output_row);
        } else if (full_col == COLUMN_IDENTIFIER_ROW_ID || full_col == 0) {
            FlatVector::GetData<uint64_t>(vector)[output_row] = event_fk;
            FlatVector::Validity(vector).SetValid(output_row);
        } else if (full_col >= 1 && static_cast<idx_t>(full_col) < bind.value_column) {
            const idx_t index_position = static_cast<idx_t>(full_col) - 1;
            if (index_position < index_count) {
                FlatVector::GetData<int32_t>(vector)[output_row] = indices[index_position];
                FlatVector::Validity(vector).SetValid(output_row);
            } else {
                FlatVector::Validity(vector).SetInvalid(output_row);
            }
        } else if (static_cast<idx_t>(full_col) == bind.value_column) {
            SetDoubleAsType(vector, output_row, bind.value_type, numeric_value);
        } else if (static_cast<idx_t>(full_col) == bind.source_id_column) {
            FlatVector::GetData<string_t>(vector)[output_row] = StringVector::AddString(vector, source_id);
            FlatVector::Validity(vector).SetValid(output_row);
        } else if (static_cast<idx_t>(full_col) == bind.entry_id_column) {
            FlatVector::GetData<uint64_t>(vector)[output_row] = entry_id;
            FlatVector::Validity(vector).SetValid(output_row);
        } else {
            FlatVector::Validity(vector).SetInvalid(output_row);
        }
    }
}

unique_ptr<LocalTableFunctionState> DatasetScanStateFactory::CreateLocal() {
    auto local = make_uniq<DatasetLocalState>();
    return std::move(local);
}

void DatasetScanExecutor::EmitProjectedTypedRow(const DatasetBindData& bind, const DatasetGlobalState& global,
                                                DataChunk& output, idx_t output_row, uint64_t event_fk,
                                                const std::string& source_id, uint64_t entry_id,
                                                const RootPrimitiveValue& numeric_value, const int32_t* indices,
                                                idx_t index_count) const {

    for (idx_t output_col = 0; output_col < global.output_column_ids.size(); ++output_col) {

        const auto full_col = global.output_column_ids[output_col];

        auto& vector = output.data[output_col];

        if (full_col == COLUMN_IDENTIFIER_EMPTY) {
            FlatVector::Validity(vector).SetInvalid(output_row);

        } else if (full_col == COLUMN_IDENTIFIER_ROW_ID || full_col == 0) {

            FlatVector::GetData<uint64_t>(vector)[output_row] = event_fk;

            FlatVector::Validity(vector).SetValid(output_row);

        } else if (full_col >= 1 && static_cast<idx_t>(full_col) < bind.value_column) {

            const idx_t index_position = static_cast<idx_t>(full_col) - 1;

            if (index_position < index_count) {
                FlatVector::GetData<int32_t>(vector)[output_row] = indices[index_position];

                FlatVector::Validity(vector).SetValid(output_row);
            } else {
                FlatVector::Validity(vector).SetInvalid(output_row);
            }

        } else if (static_cast<idx_t>(full_col) == bind.value_column) {

            SetPrimitiveAsType(vector, output_row, bind.value_type, numeric_value);

        } else if (static_cast<idx_t>(full_col) == bind.source_id_column) {

            FlatVector::GetData<string_t>(vector)[output_row] = StringVector::AddString(vector, source_id);

            FlatVector::Validity(vector).SetValid(output_row);

        } else if (static_cast<idx_t>(full_col) == bind.entry_id_column) {

            FlatVector::GetData<uint64_t>(vector)[output_row] = entry_id;

            FlatVector::Validity(vector).SetValid(output_row);

        } else {
            FlatVector::Validity(vector).SetInvalid(output_row);
        }
    }
}

idx_t DatasetScanExecutor::ReserveOutputRows(DatasetGlobalState& global, idx_t requested) const {
    if (requested == 0 || global.stop_requested.load(std::memory_order_relaxed)) {
        return 0;
    }
    if (global.row_limit == std::numeric_limits<uint64_t>::max()) {
        global.emitted_rows.fetch_add(requested, std::memory_order_relaxed);
        return requested;
    }
    auto current = global.emitted_rows.load(std::memory_order_relaxed);
    while (current < global.row_limit) {
        const auto remaining = global.row_limit - current;
        const auto granted = static_cast<uint64_t>(std::min<uint64_t>(remaining, requested));
        if (global.emitted_rows.compare_exchange_weak(current, current + granted, std::memory_order_relaxed)) {
            if (current + granted >= global.row_limit) {
                global.stop_requested.store(true, std::memory_order_relaxed);
            }
            return static_cast<idx_t>(granted);
        }
    }
    global.stop_requested.store(true, std::memory_order_relaxed);
    return 0;
}

void DatasetScanExecutor::Scan(ClientContext& context, TableFunctionInput& input, DataChunk& output) {
    auto& bind = input.bind_data->Cast<DatasetBindData>();
    auto& global = input.global_state->Cast<DatasetGlobalState>();
    auto& local = input.local_state->Cast<DatasetLocalState>();
    idx_t output_count = 0;

    // DuckDB represents COUNT(*) as a single dummy projection. With no filters,
    // root_baskets.value_count is already the exact row cardinality, so produce
    // dummy rows directly from metadata and avoid opening or decoding ROOT files.
    if (global.metadata_count_only) {
        const auto already_emitted = global.metadata_rows_emitted.load(std::memory_order_relaxed);
        if (already_emitted < global.metadata_total_rows) {
            const auto wanted = static_cast<idx_t>(
                std::min<uint64_t>(STANDARD_VECTOR_SIZE, global.metadata_total_rows - already_emitted));
            const auto emit_count = ReserveOutputRows(global, wanted);
            if (!output.data.empty()) {
                auto& dummy = output.data[0];
                for (idx_t row = 0; row < emit_count; ++row) {
                    FlatVector::Validity(dummy).SetInvalid(row);
                }
            }
            output_count = emit_count;
            global.metadata_rows_emitted.fetch_add(emit_count, std::memory_order_relaxed);
        }
        output.SetCardinality(output_count);
        return;
    }

    const bool typed_transport = RequiresTypedDatasetValue(bind.value_type);

    while (output_count < STANDARD_VECTOR_SIZE) {
        if (!local.has_task) {
            if (!ClaimTask(bind, global, local)) {
                break;
            }
        }

        const idx_t available = typed_transport ? local.typed_values.size() : local.values.size();

        if (local.value_offset >= available) {
            if (!LoadNextEntry(bind, local, global)) {
                local.has_task = false;
                global.completed_tasks.fetch_add(1);
                continue;
            }
        }

        const idx_t loaded = typed_transport ? local.typed_values.size() : local.values.size();

        while (local.value_offset < loaded && output_count < STANDARD_VECTOR_SIZE) {

            const auto value_index = local.value_offset++;

            const idx_t index_count = bind.index_names.size();

            const int32_t* indices = index_count ? local.flat_indices.data() + value_index * index_count : nullptr;

            if (typed_transport) {
                const auto& numeric_value = local.typed_values[value_index];

                if (!PassesTypedFilters(context, local, bind, global, local.value_event_fk, numeric_value, indices,
                                        index_count)) {
                    continue;
                }

                if (ReserveOutputRows(global, 1) == 0) {
                    output.SetCardinality(output_count);
                    return;
                }

                EmitProjectedTypedRow(bind, global, output, output_count++, local.value_event_fk, local.value_source_id,
                                      local.value_entry_id, numeric_value, indices, index_count);

            } else {
                const auto numeric_value = local.values[value_index];

                if (!PassesFilters(context, local, bind, global, local.value_event_fk, numeric_value, indices,
                                   index_count)) {
                    continue;
                }

                if (ReserveOutputRows(global, 1) == 0) {
                    output.SetCardinality(output_count);
                    return;
                }

                EmitProjectedRow(bind, global, output, output_count++, local.value_event_fk, local.value_source_id,
                                 local.value_entry_id, numeric_value, indices, index_count);
            }
        }
    }

    output.SetCardinality(output_count);
}

InsertionOrderPreservingMap<string> DatasetScanExplain::Bound(TableFunctionToStringInput& input) const {
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data) {
        return result;
    }
    const auto& bind = input.bind_data->Cast<DatasetBindData>();
    result["Logical Path"] = bind.logical_path;
    result["Value Type"] = bind.value_type.ToString();
    result["Index Columns"] = bind.index_names.empty() ? "none" : JoinStrings(bind.index_names, ", ");
    result["Metadata Source"] = bind.sources.sql_tables ? "SQL/Iceberg relations" : bind.catalog_path;
    if (!bind.sources.snapshot_id.empty()) {
        result["Snapshot"] = bind.sources.snapshot_id;
    }
    result["Filter Pushdown"] = "zonemap + optional Bloom + exact in-scan evaluation";
    result["Projection Pushdown"] = "enabled";
    result["ROOT Decode Mode"] = RootReaderModeName(bind.root_access.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.root_access.validation_entries);
    result["Path Predicate Indexes"] = std::to_string(bind.path_predicates.size());
    result["Explicit Entry Selections"] = std::to_string(bind.entry_selection.size());
    result["Estimated Rows"] = std::to_string(bind.estimated_cardinality);
    if (bind.row_limit != std::numeric_limits<uint64_t>::max()) {
        result["Row Limit"] = std::to_string(bind.row_limit);
    }
    return result;
}

InsertionOrderPreservingMap<string> DatasetScanExplain::Running(TableFunctionDynamicToStringInput& input) const {
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data) {
        const auto& bind = input.bind_data->Cast<DatasetBindData>();
        result["Logical Path"] = bind.logical_path;
        result["Value Type"] = bind.value_type.ToString();
        result["Index Columns"] = bind.index_names.empty() ? "none" : JoinStrings(bind.index_names, ", ");
        result["Metadata Source"] = bind.sources.sql_tables ? "SQL/Iceberg relations" : bind.catalog_path;
        result["ROOT Decode Mode"] = RootReaderModeName(bind.root_access.reader_mode);
        if (!bind.sources.snapshot_id.empty()) {
            result["Snapshot"] = bind.sources.snapshot_id;
        }
        result["Path Predicate Indexes"] = std::to_string(bind.path_predicates.size());
        result["Estimated Rows"] = std::to_string(bind.estimated_cardinality);
    }
    if (!input.global_state) {
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
    result["GetEntry Calls"] = std::to_string(global.get_entry_calls.load());
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entry_calls.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Projected ROOT Files"] = std::to_string(global.projected_files.load());
    result["Object Fallback Files"] = std::to_string(global.fallback_files.load());
    result["Coalesced Read Tasks"] = std::to_string(global.tasks.size());
    result["File-affinity Groups"] = std::to_string(global.task_groups.size());
    result["Selected Basket Bytes"] = std::to_string(global.planned_compressed_bytes);
    result["Scheduled Range Bytes"] = std::to_string(global.scheduled_read_bytes);
    result["Indexed Values in Tasks"] = std::to_string(global.planned_values);
    result["Skipped ROOT Entries"] = std::to_string(global.skipped_entries.load());
    result["Metadata-only Rows"] = std::to_string(global.metadata_rows_emitted.load());
    if (global.metadata_count_only) {
        result["Metadata Cardinality"] = std::to_string(global.metadata_total_rows);
    }
    result["Decoded Values"] = std::to_string(global.decoded_values.load());
    result["Emitted Rows"] = std::to_string(global.emitted_rows.load());
    return result;
}

double DatasetScanExecutor::Progress(const GlobalTableFunctionState* state) const {
    if (!state) {
        return 0.0;
    }
    const auto& global = state->Cast<DatasetGlobalState>();
    if (global.tasks.empty()) {
        return 100.0;
    }
    const auto completed = std::min<idx_t>(global.completed_tasks.load(), global.tasks.size());
    return 100.0 * static_cast<double>(completed) / static_cast<double>(global.tasks.size());
}

} // namespace duckdb::rootlake
