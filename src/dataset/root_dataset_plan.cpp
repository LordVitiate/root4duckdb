#include "root4duckdb/dataset/root_dataset_pruning.hpp"

namespace duckdb::rootlake {

unique_ptr<FunctionData> DatasetBindData::Copy() const {
    auto result = make_uniq<DatasetBindData>(*this);
    return std::move(result);
}

bool DatasetBindData::Equals(const FunctionData&) const {
    return false;
}

bool DatasetBindData::SupportStatementCache() const {
    return false;
}

/// Measures catalog and basket planning without affecting the plan.
class DatasetPlanningTimer {
  public:
    explicit DatasetPlanningTimer(uint64_t& elapsed_us);
    ~DatasetPlanningTimer();

  private:
    uint64_t& elapsed_us;
    std::chrono::steady_clock::time_point started;
};

DatasetPlanningTimer::DatasetPlanningTimer(uint64_t& elapsed_us_p)
    : elapsed_us(elapsed_us_p), started(std::chrono::steady_clock::now()) {
}

DatasetPlanningTimer::~DatasetPlanningTimer() {
    elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}

void DatasetTaskPlanner::BuildFileTaskGroups(DatasetGlobalState& global) {
    global.task_groups.clear();
    idx_t begin = 0;
    while (begin < global.tasks.size()) {
        idx_t end = begin + 1;
        while (end < global.tasks.size() && global.tasks[end].root_uri == global.tasks[begin].root_uri) {
            ++end;
        }
        global.task_groups.push_back({begin, end});
        begin = end;
    }
}

void DatasetTaskPlanner::Plan(ClientContext& context, const DatasetBindData& bind, DatasetGlobalState& global,
                              optional_ptr<TableFilterSet> filters) {
    DatasetPlanningTimer planning_timer(global.planning_time_us);
    if (filters) {
        global.filters = filters->Copy();
    }
    // event_fk is the first public output column. Both event_fk and entry_id
    // address the same global event coordinate and can prune ROOT baskets.
    constexpr column_t kEventFkOutputColumn = 0;
    if (auto entry_filter = FilterForFullColumn(global, static_cast<column_t>(bind.entry_id_column))) {
        MergeEventRangeIntoGlobal(global, ExtractRootUnsignedRange(*entry_filter));
    }
    if (auto event_fk_filter = FilterForFullColumn(global, kEventFkOutputColumn)) {
        MergeEventRangeIntoGlobal(global, ExtractRootUnsignedRange(*event_fk_filter));
    }
    if (global.event_range_impossible) {
        return;
    }
    if (global.filters) {
        for (const auto& entry : global.filters->filters) {
            if (RejectsAllMaterializedRows(*entry.second)) {
                return;
            }
        }
    }
    BuildPredicateIntersection(context, bind, global);
    const bool interval_filter_active = bind.entry_selection_active || !bind.path_predicates.empty();
    if (interval_filter_active && global.candidate_intervals.empty()) {
        return;
    }
    const auto files_relation = CatalogRelationSQL(bind.sources.files, bind.sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(bind.sources.baskets, bind.sources.sql_tables);
    {
        Connection metrics(*context.db);
        std::string totals_filter = " WHERE f.column_id IN " + IdListSQL(bind.schemas, true) + " AND f.schema_id IN " +
                                    IdListSQL(bind.schemas, false);
        if (!bind.sources.snapshot_id.empty()) {
            totals_filter += " AND f.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id);
        }
        auto totals =
            metrics.Query("SELECT count(DISTINCT f.file_id)::UBIGINT, COALESCE(sum(f.basket_count),0)::UBIGINT, "
                          "COALESCE(sum(f.value_count),0)::UBIGINT FROM " +
                          files_relation + " f" + totals_filter);
        EnsureQueryOK(*totals, "read ROOT catalog totals");
        global.catalog_files = totals->GetValue(0, 0).GetValue<uint64_t>();
        global.catalog_baskets = totals->GetValue(1, 0).GetValue<uint64_t>();
    }
    std::string predicate;
    if (!bind.sources.snapshot_id.empty()) {
        predicate += " AND f.snapshot_id = " + SqlLiteral(bind.sources.snapshot_id);
        predicate += " AND b.snapshot_id = " + SqlLiteral(bind.sources.snapshot_id);
    }
    if (UsesDoubleBackedValueMetadata(bind.value_type)) {
        if (auto value_filter = FilterForFullColumn(global, static_cast<column_t>(bind.value_column))) {
            const auto file_clause = ZonemapClause(*value_filter, "f.min_value", "f.max_value", bind.value_type);

            const auto basket_clause = ZonemapClause(*value_filter, "b.min_value", "b.max_value", bind.value_type);

            if (!file_clause.empty()) {
                predicate += " AND ((" + file_clause + ") OR f.nan_count > 0)";
            }

            if (!basket_clause.empty()) {
                predicate += " AND ((" + basket_clause + ") OR b.nan_count > 0)";
            }
        }
    }
    if (auto entry_filter = FilterForFullColumn(global, static_cast<column_t>(bind.entry_id_column))) {
        const auto file_clause =
            ZonemapClause(*entry_filter, "f.event_base", "f.event_base + f.total_entries - 1", LogicalType::UBIGINT);
        const auto basket_clause = ZonemapClause(*entry_filter, "f.event_base + b.entry_begin",
                                                 "f.event_base + b.entry_end - 1", LogicalType::UBIGINT);
        if (!file_clause.empty()) {
            predicate += " AND (" + file_clause + ")";
        }
        if (!basket_clause.empty()) {
            predicate += " AND (" + basket_clause + ")";
        }
    }
    if (auto event_fk_filter = FilterForFullColumn(global, kEventFkOutputColumn)) {
        const auto file_clause =
            ZonemapClause(*event_fk_filter, "f.event_base", "f.event_base + f.total_entries - 1", LogicalType::UBIGINT);
        const auto basket_clause = ZonemapClause(*event_fk_filter, "f.event_base + b.entry_begin",
                                                 "f.event_base + b.entry_end - 1", LogicalType::UBIGINT);
        if (!file_clause.empty()) {
            predicate += " AND (" + file_clause + ")";
        }
        if (!basket_clause.empty()) {
            predicate += " AND (" + basket_clause + ")";
        }
    }
    if (auto source_filter = FilterForFullColumn(global, static_cast<column_t>(bind.source_id_column))) {
        const auto source_clause = ExactStringFilterClause(*source_filter, "f.file_id");
        if (!source_clause.empty()) {
            predicate += " AND (" + source_clause + ")";
        }
    }

    auto value_filter = FilterForFullColumn(global, static_cast<column_t>(bind.value_column));

    const bool needs_bloom =
        UsesDoubleBackedValueMetadata(bind.value_type) && value_filter && FilterNeedsBloom(*value_filter);
    const auto sql = "SELECT f.file_id, f.root_uri, f.tree_name, f.schema_id, f.event_base, f.file_size, f.mtime_ns, "
                     "b.basket_id, b.entry_begin, b.entry_end, b.flat_value_begin, b.value_count, b.physical_offset, "
                     "b.compressed_size, " +
                     std::string(needs_bloom ? "b.bloom_filter" : "NULL::BLOB") + " FROM " + baskets_relation +
                     " b JOIN " + files_relation +
                     " f ON f.file_id=b.file_id AND f.column_id=b.column_id WHERE b.column_id IN " +
                     IdListSQL(bind.schemas, true) + " AND f.schema_id IN " + IdListSQL(bind.schemas, false) +
                     predicate + " ORDER BY f.root_uri, b.physical_offset";

    Connection connection(*context.db);
    auto result = connection.SendQuery(sql);
    if (result->HasError()) {
        throw IOException("plan ROOT basket scan: " + result->GetError());
    }
    std::unordered_map<std::string, bool> freshness_checked;
    std::unordered_map<std::string, bool> selected_files;
    const bool can_stop_after_row_limit = bind.row_limit != std::numeric_limits<uint64_t>::max() &&
                                          (!global.filters || global.filters->filters.empty()) &&
                                          !interval_filter_active;
    bool row_limit_planned = false;

    while (auto chunk = result->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); ++row) {
            RootBasketTask task;
            task.file_id = chunk->GetValue(0, row).GetValue<std::string>();
            task.root_uri = chunk->GetValue(1, row).GetValue<std::string>();
            task.tree_name = chunk->GetValue(2, row).GetValue<std::string>();
            task.schema_id = chunk->GetValue(3, row).GetValue<std::string>();
            task.event_base = chunk->GetValue(4, row).GetValue<uint64_t>();
            const auto indexed_size = chunk->GetValue(5, row).GetValue<uint64_t>();
            const auto indexed_mtime = chunk->GetValue(6, row).GetValue<int64_t>();
            task.basket_id = chunk->GetValue(7, row).GetValue<uint32_t>();
            task.entry_begin = chunk->GetValue(8, row).GetValue<uint64_t>();
            task.entry_end = chunk->GetValue(9, row).GetValue<uint64_t>();
            task.flat_value_begin = chunk->GetValue(10, row).GetValue<uint64_t>();
            task.value_count = chunk->GetValue(11, row).GetValue<uint64_t>();
            task.physical_offset = chunk->GetValue(12, row).GetValue<uint64_t>();
            task.compressed_size = chunk->GetValue(13, row).GetValue<uint32_t>();
            const auto bloom_value = chunk->GetValue(14, row);
            if (value_filter && !bloom_value.IsNull()) {
                const auto& bloom = StringValue::Get(bloom_value);
                global.bloom_metadata_bytes += bloom.size();
                if (!BloomMayContainFilter(*value_filter, bloom, bind.value_type)) {
                    continue;
                }
            }

            if (bind.require_fresh_index && indexed_size && freshness_checked.emplace(task.root_uri, true).second) {
                std::error_code ec;
                const auto current_size = fs::file_size(task.root_uri, ec);
                if (!ec && current_size != indexed_size) {
                    throw IOException("ROOT file size changed after indexing: " + task.root_uri);
                }
                // The index stores POSIX/Unix mtime_ns. Do not compare it with
                // std::filesystem::file_time_type::time_since_epoch(): that clock
                // is implementation-defined and may use a different epoch.
                struct stat current_stat {};
                if (indexed_mtime && ::stat(task.root_uri.c_str(), &current_stat) == 0) {
                    const auto current_ns = static_cast<int64_t>(current_stat.st_mtim.tv_sec) * 1000000000LL +
                                            static_cast<int64_t>(current_stat.st_mtim.tv_nsec);
                    if (current_ns != indexed_mtime) {
                        throw IOException("ROOT file mtime changed after indexing: " + task.root_uri +
                                          " (indexed_mtime_ns=" + std::to_string(indexed_mtime) +
                                          ", current_mtime_ns=" + std::to_string(current_ns) + ")");
                    }
                }
            }
            std::vector<RootBasketTask> candidate_tasks;
            if (!interval_filter_active) {
                candidate_tasks.push_back(task);
            } else {
                auto candidates = global.candidate_intervals.find(task.file_id);
                if (candidates == global.candidate_intervals.end()) {
                    continue;
                }
                for (const auto& interval : candidates->second) {
                    const uint64_t begin = std::max(task.entry_begin, interval.begin);
                    const uint64_t end = std::min(task.entry_end, interval.end);
                    if (begin >= end) {
                        continue;
                    }
                    auto clipped = task;
                    clipped.entry_begin = begin;
                    clipped.entry_end = end;
                    candidate_tasks.push_back(std::move(clipped));
                }
            }

            bool counted_basket = false;
            for (auto& candidate : candidate_tasks) {
                if (!counted_basket) {
                    global.planned_compressed_bytes += candidate.compressed_size;
                    global.planned_values += candidate.value_count;
                    ++global.planned_baskets;
                    counted_basket = true;
                }
                selected_files.emplace(candidate.root_uri, true);
                if (!global.tasks.empty()) {
                    auto& previous = global.tasks.back();
                    const uint64_t previous_end = previous.physical_offset + previous.compressed_size;
                    const bool ordered = candidate.physical_offset >= previous_end;
                    const uint64_t gap =
                        ordered ? candidate.physical_offset - previous_end : std::numeric_limits<uint64_t>::max();
                    if (previous.root_uri == candidate.root_uri && previous.tree_name == candidate.tree_name &&
                        previous.schema_id == candidate.schema_id && previous.entry_end == candidate.entry_begin &&
                        ordered && gap <= bind.coalesce_gap_bytes) {
                        previous.entry_end = candidate.entry_end;
                        previous.value_count += candidate.value_count;
                        previous.compressed_size =
                            candidate.physical_offset + candidate.compressed_size - previous.physical_offset;
                        previous.basket_count += 1;
                        continue;
                    }
                }
                global.tasks.push_back(std::move(candidate));
            }
            if (can_stop_after_row_limit && global.planned_values >= bind.row_limit) {
                row_limit_planned = true;
                break;
            }
        }
        if (row_limit_planned) {
            break;
        }
    }
    if (result->HasError()) {
        throw IOException("plan ROOT basket scan: " + result->GetError());
    }
    global.planned_files = selected_files.size();
    global.skipped_files =
        global.catalog_files >= global.planned_files ? global.catalog_files - global.planned_files : 0;
    global.skipped_baskets =
        global.catalog_baskets >= global.planned_baskets ? global.catalog_baskets - global.planned_baskets : 0;
    for (const auto& task : global.tasks) {
        global.scheduled_read_bytes += task.compressed_size;
    }
    BuildFileTaskGroups(global);
}

unique_ptr<FunctionData> DatasetScanBinder::Bind(ClientContext& context, TableFunctionBindInput& input,
                                                 vector<LogicalType>& return_types, vector<string>& return_names) {
    auto bind = make_uniq<DatasetBindData>();
    bind->catalog_path = input.inputs[0].ToString();
    bind->logical_path = NormalizePath(input.inputs[1].ToString());
    RootDatasetCatalog catalog(context, bind->catalog_path, input.named_parameters);
    bind->sources = catalog.Sources();

    auto it = input.named_parameters.find("dictionary");
    if (it != input.named_parameters.end()) {
        bind->dictionary = it->second.ToString();
    }
    std::string dictionary_cleanup;
    it = input.named_parameters.find("dictionary_cleanup");
    if (it != input.named_parameters.end()) {
        dictionary_cleanup = it->second.ToString();
    }
    bind->root_access.dictionary_cleanup_mode = ParseDictionaryCleanupMode(dictionary_cleanup);
    it = input.named_parameters.find("tree_cache_bytes");
    if (it != input.named_parameters.end()) {
        bind->root_access.tree_cache_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("reader_mode");
    if (it != input.named_parameters.end()) {
        bind->root_access.reader_mode = ParseRootReaderMode(it->second.ToString());
    }
    it = input.named_parameters.find("raw_validation_entries");
    if (it != input.named_parameters.end()) {
        bind->root_access.validation_entries = it->second.GetValue<uint32_t>();
    }
    it = input.named_parameters.find("raw_max_entry_bytes");
    if (it != input.named_parameters.end()) {
        bind->root_access.max_entry_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("raw_max_values_per_entry");
    if (it != input.named_parameters.end()) {
        bind->root_access.max_values_per_entry = it->second.GetValue<uint64_t>();
    }
    bind->root_access.Validate();
    it = input.named_parameters.find("coalesce_gap_bytes");
    if (it != input.named_parameters.end()) {
        bind->coalesce_gap_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("prefetch_depth");
    if (it != input.named_parameters.end()) {
        bind->prefetch_depth = it->second.GetValue<uint32_t>();
    }
    it = input.named_parameters.find("prefetch_ranges");
    if (it != input.named_parameters.end()) {
        bind->prefetch_ranges = it->second.GetValue<bool>();
    }
    it = input.named_parameters.find("require_fresh_index");
    if (it != input.named_parameters.end()) {
        bind->require_fresh_index = it->second.GetValue<bool>();
    }
    it = input.named_parameters.find("max_open_files");
    if (it != input.named_parameters.end()) {
        bind->max_open_files = it->second.GetValue<uint32_t>();
    }
    it = input.named_parameters.find("memory_budget_bytes");
    if (it != input.named_parameters.end()) {
        bind->memory_budget_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("estimated_reader_bytes");
    if (it != input.named_parameters.end()) {
        bind->estimated_reader_bytes = it->second.GetValue<uint64_t>();
    }
    it = input.named_parameters.find("row_limit");
    if (it != input.named_parameters.end()) {
        bind->row_limit = it->second.GetValue<uint64_t>();
    }
    if (bind->estimated_reader_bytes == 0) {
        throw InvalidInputException("estimated_reader_bytes must be positive");
    }

    LoadRootDictionary(bind->dictionary);
    auto schema_set = catalog.LoadSchemas(bind->logical_path);
    bind->schemas = std::move(schema_set.schemas);
    bind->schema_lookup = std::move(schema_set.lookup);
    bind->index_names = std::move(schema_set.index_names);
    bind->value_type = schema_set.value_type;
    bind->value_column = 1 + bind->index_names.size();
    it = input.named_parameters.find("path_predicates");
    if (it != input.named_parameters.end()) {
        ParsePathPredicates(catalog, *bind, it->second.ToString());
    }
    it = input.named_parameters.find("entry_selection");
    if (it != input.named_parameters.end()) {
        ParseEntrySelection(*bind, it->second.ToString());
    }
    it = input.named_parameters.find("entry_selection_file");
    if (it != input.named_parameters.end()) {
        const auto selection_path = it->second.ToString();
        std::ifstream selection_stream(selection_path);
        if (!selection_stream) {
            throw IOException("Failed to open entry_selection_file: " + selection_path);
        }
        std::string selection_json((std::istreambuf_iterator<char>(selection_stream)),
                                   std::istreambuf_iterator<char>());
        ParseEntrySelection(*bind, selection_json);
    }

    {
        const auto files_relation = CatalogRelationSQL(bind->sources.files, bind->sources.sql_tables);
        std::string sql = "SELECT CAST(COALESCE(sum(value_count), 0) AS UBIGINT) FROM " + files_relation +
                          " WHERE column_id IN " + IdListSQL(bind->schemas, true);
        if (!bind->sources.snapshot_id.empty()) {
            sql += " AND snapshot_id = " + SqlLiteral(bind->sources.snapshot_id);
        }
        Connection connection(*context.db);
        auto result = connection.Query(sql);
        EnsureQueryOK(*result, "estimate ROOT dataset cardinality");
        const auto total = result->GetValue(0, 0).GetValue<uint64_t>();
        bind->estimated_cardinality =
            static_cast<idx_t>(std::min<uint64_t>(total, static_cast<uint64_t>(std::numeric_limits<idx_t>::max())));
    }

    return_names.push_back("event_fk");
    return_types.push_back(LogicalType::UBIGINT);
    for (const auto& index_name : bind->index_names) {
        return_names.push_back(index_name);
        return_types.push_back(LogicalType::INTEGER);
    }
    return_names.push_back("value");
    return_types.push_back(bind->value_type);
    bind->source_id_column = return_names.size();
    return_names.push_back("source_id");
    return_types.push_back(LogicalType::VARCHAR);
    bind->entry_id_column = return_names.size();
    return_names.push_back("entry_id");
    return_types.push_back(LogicalType::UBIGINT);
    return std::move(bind);
}

unique_ptr<NodeStatistics> DatasetScanStateFactory::Cardinality(const FunctionData* bind_data) {
    if (!bind_data) {
        return nullptr;
    }
    const auto& bind = bind_data->Cast<DatasetBindData>();
    return make_uniq<NodeStatistics>(bind.estimated_cardinality, bind.estimated_cardinality);
}

void DatasetTaskPlanner::PlanMetadataCount(ClientContext& context, const DatasetBindData& bind,
                                           DatasetGlobalState& global) {
    DatasetPlanningTimer planning_timer(global.planning_time_us);
    const auto files_relation = CatalogRelationSQL(bind.sources.files, bind.sources.sql_tables);
    std::string predicate = " WHERE f.column_id IN " + IdListSQL(bind.schemas, true) + " AND f.schema_id IN " +
                            IdListSQL(bind.schemas, false);
    if (!bind.sources.snapshot_id.empty()) {
        predicate += " AND f.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id);
    }
    Connection connection(*context.db);
    auto totals =
        connection.Query("SELECT count(DISTINCT f.file_id)::UBIGINT, COALESCE(sum(f.basket_count),0)::UBIGINT, "
                         "COALESCE(sum(f.value_count),0)::UBIGINT FROM " +
                         files_relation + " f" + predicate);
    EnsureQueryOK(*totals, "plan metadata-only ROOT count");
    global.catalog_files = totals->GetValue(0, 0).GetValue<uint64_t>();
    global.catalog_baskets = totals->GetValue(1, 0).GetValue<uint64_t>();
    global.metadata_total_rows = totals->GetValue(2, 0).GetValue<uint64_t>();
    global.skipped_files = global.catalog_files;
    global.skipped_baskets = global.catalog_baskets;
}

unique_ptr<GlobalTableFunctionState> DatasetScanStateFactory::CreateGlobal(ClientContext& context,
                                                                           TableFunctionInitInput& input) {
    auto& bind = input.bind_data->Cast<DatasetBindData>();
    auto global = make_uniq<DatasetGlobalState>();
    global->scan_column_ids = input.column_ids;
    if (input.projection_ids.empty()) {
        global->output_column_ids = input.column_ids;
    } else {
        global->output_column_ids.reserve(input.projection_ids.size());
        for (const auto projection_id : input.projection_ids) {
            if (projection_id >= input.column_ids.size()) {
                throw InternalException("Invalid ROOT table-function projection id");
            }
            global->output_column_ids.push_back(input.column_ids[projection_id]);
        }
    }
    global->row_limit = bind.row_limit;
    // COUNT(*) is represented by DuckDB using a synthetic/dummy column
    // identifier. Only ordinals in the actual public schema require ROOT
    // materialization. Treat EMPTY, ROW_ID and any other out-of-schema
    // synthetic identifier as metadata-only.
    const auto public_column_count = static_cast<column_t>(bind.entry_id_column + 1);
    const bool no_materialized_projection =
        global->output_column_ids.empty() ||
        std::none_of(global->output_column_ids.begin(), global->output_column_ids.end(),
                     [public_column_count](column_t column) { return column < public_column_count; });
    const bool has_materialized_filters = input.filters && !input.filters->filters.empty();
    global->metadata_count_only = !has_materialized_filters && bind.path_predicates.empty() &&
                                  !bind.entry_selection_active && no_materialized_projection;
    if (global->metadata_count_only) {
        DatasetTaskPlanner().PlanMetadataCount(context, bind, *global);
    } else {
        DatasetTaskPlanner().Plan(context, bind, *global, input.filters);
    }
    const auto root_runtime =
        RootRuntimeSettings::From(context, std::max<idx_t>(1, global->catalog_files), global->catalog_baskets);
    global->worker_limit = std::max<idx_t>(1, std::min(root_runtime.threads, root_runtime.max_in_flight_files));
    if (bind.max_open_files > 0) {
        global->worker_limit = std::min<idx_t>(global->worker_limit, bind.max_open_files);
    }
    if (bind.memory_budget_bytes > 0) {
        const auto memory_workers = std::max<uint64_t>(1, bind.memory_budget_bytes / bind.estimated_reader_bytes);
        global->worker_limit = std::min<idx_t>(global->worker_limit, memory_workers);
    }
    return std::move(global);
}

} // namespace duckdb::rootlake
