#include "root4duckdb/dataset/root_dataset_pruning.hpp"

namespace duckdb::rootlake {

static void NormalizeIntervals(std::vector<EntryInterval>& intervals) {
    std::sort(intervals.begin(), intervals.end(), [](const EntryInterval& left, const EntryInterval& right) {
        return left.begin < right.begin || (left.begin == right.begin && left.end < right.end);
    });
    std::vector<EntryInterval> merged;
    for (const auto& interval : intervals) {
        if (interval.begin >= interval.end) {
            continue;
        }
        if (!merged.empty() && interval.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, interval.end);
        } else {
            merged.push_back(interval);
        }
    }
    intervals.swap(merged);
}

void ParseEntrySelection(DatasetBindData& bind, const std::string& raw_json) {
    if (raw_json.empty()) {
        return;
    }
    const auto json = nlohmann::json::parse(raw_json);
    if (!json.is_object()) {
        throw InvalidInputException("entry_selection must be a JSON object keyed by source_id");
    }
    bind.entry_selection_active = true;
    for (auto it = json.begin(); it != json.end(); ++it) {
        const auto source_id = it.key();
        const auto& selection = it.value();
        if (!selection.is_object()) {
            throw InvalidInputException("entry_selection source value must be an object");
        }
        auto& intervals = bind.entry_selection[source_id];
        if (selection.contains("ranges")) {
            if (!selection["ranges"].is_array()) {
                throw InvalidInputException("entry_selection.ranges must be an array");
            }
            for (const auto& range : selection["ranges"]) {
                if (!range.is_array() || range.size() != 2) {
                    throw InvalidInputException("entry_selection range must be [begin,end)");
                }
                intervals.push_back({range[0].get<uint64_t>(), range[1].get<uint64_t>()});
            }
        }
        if (selection.contains("entries")) {
            if (!selection["entries"].is_array()) {
                throw InvalidInputException("entry_selection.entries must be an array");
            }
            for (const auto& entry : selection["entries"]) {
                const auto value = entry.get<uint64_t>();
                if (value == std::numeric_limits<uint64_t>::max()) {
                    throw InvalidInputException("entry_selection entry is too large");
                }
                intervals.push_back({value, value + 1});
            }
        }
        if (selection.contains("entries_delta")) {
            const auto& encoded = selection["entries_delta"];
            if (!encoded.is_object() || !encoded.contains("base") || !encoded.contains("deltas") ||
                !encoded["deltas"].is_array()) {
                throw InvalidInputException("entry_selection.entries_delta must contain base and deltas[]");
            }
            auto value = encoded["base"].get<uint64_t>();
            if (value == std::numeric_limits<uint64_t>::max()) {
                throw InvalidInputException("entry_selection delta base is too large");
            }
            intervals.push_back({value, value + 1});
            for (const auto& delta_json : encoded["deltas"]) {
                const auto delta = delta_json.get<uint64_t>();
                if (delta == 0 || value > std::numeric_limits<uint64_t>::max() - delta) {
                    throw InvalidInputException("entry_selection deltas must be positive and non-overflowing");
                }
                value += delta;
                if (value == std::numeric_limits<uint64_t>::max()) {
                    throw InvalidInputException("entry_selection delta entry is too large");
                }
                intervals.push_back({value, value + 1});
            }
        }
        NormalizeIntervals(intervals);
    }
}

static std::vector<EntryInterval> IntersectIntervals(const std::vector<EntryInterval>& left,
                                                     const std::vector<EntryInterval>& right) {
    std::vector<EntryInterval> result;
    idx_t i = 0;
    idx_t j = 0;
    while (i < left.size() && j < right.size()) {
        const uint64_t begin = std::max(left[i].begin, right[j].begin);
        const uint64_t end = std::min(left[i].end, right[j].end);
        if (begin < end) {
            result.push_back({begin, end});
        }
        if (left[i].end < right[j].end) {
            ++i;
        } else {
            ++j;
        }
    }
    NormalizeIntervals(result);
    return result;
}

static std::unordered_map<std::string, std::vector<EntryInterval>>
LoadPredicateIntervals(ClientContext& context, const DatasetBindData& bind, const PathPredicateBinding& predicate,
                       uint64_t& basket_counter, uint64_t& bloom_bytes) {
    const auto files_relation = CatalogRelationSQL(bind.sources.files, bind.sources.sql_tables);
    const auto baskets_relation = CatalogRelationSQL(bind.sources.baskets, bind.sources.sql_tables);
    std::string snapshot_clause;
    if (!bind.sources.snapshot_id.empty()) {
        snapshot_clause = " AND f.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id) +
                          " AND b.snapshot_id=" + SqlLiteral(bind.sources.snapshot_id);
    }
    const bool needs_bloom = UsesDoubleBackedValueMetadata(predicate.value_type) &&
                             (predicate.op == PathPredicateOp::EQ || predicate.op == PathPredicateOp::IN);
    const auto sql = "SELECT f.file_id, b.entry_begin, b.entry_end, b.min_value, b.max_value, b.nan_count, "
                     "b.pos_inf_count, b.neg_inf_count, " +
                     std::string(needs_bloom ? "b.bloom_filter" : "NULL::BLOB") + " FROM " + baskets_relation +
                     " b JOIN " + files_relation +
                     " f ON f.file_id=b.file_id AND f.column_id=b.column_id WHERE b.column_id IN " +
                     IdListSQL(predicate.schemas, true) + " AND f.schema_id IN " + IdListSQL(predicate.schemas, false) +
                     snapshot_clause + " ORDER BY f.file_id, b.entry_begin";
    Connection connection(*context.db);
    auto result = connection.SendQuery(sql);
    if (result->HasError()) {
        throw IOException("plan predicate ROOT basket scan: " + result->GetError());
    }
    std::unordered_map<std::string, std::vector<EntryInterval>> intervals;
    while (auto chunk = result->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); ++row) {
            ++basket_counter;
            if (!chunk->GetValue(8, row).IsNull()) {
                bloom_bytes += StringValue::Get(chunk->GetValue(8, row)).size();
            }
            if (!PredicateMetadataMayMatch(predicate, chunk->GetValue(3, row), chunk->GetValue(4, row),
                                           chunk->GetValue(5, row).GetValue<uint64_t>(),
                                           chunk->GetValue(6, row).GetValue<uint64_t>(),
                                           chunk->GetValue(7, row).GetValue<uint64_t>(), chunk->GetValue(8, row))) {
                continue;
            }
            intervals[chunk->GetValue(0, row).ToString()].push_back(
                {chunk->GetValue(1, row).GetValue<uint64_t>(), chunk->GetValue(2, row).GetValue<uint64_t>()});
        }
    }
    if (result->HasError()) {
        throw IOException("plan predicate ROOT basket scan: " + result->GetError());
    }
    for (auto& entry : intervals) {
        NormalizeIntervals(entry.second);
    }
    return intervals;
}

void BuildPredicateIntersection(ClientContext& context, const DatasetBindData& bind, DatasetGlobalState& global) {
    if (bind.entry_selection_active) {
        global.candidate_intervals = bind.entry_selection;
    }
    if (bind.path_predicates.empty()) {
        return;
    }
    bool first = !bind.entry_selection_active;
    for (const auto& predicate : bind.path_predicates) {
        auto intervals = LoadPredicateIntervals(context, bind, predicate, global.predicate_index_baskets,
                                                global.bloom_metadata_bytes);
        if (first) {
            global.candidate_intervals = std::move(intervals);
            first = false;
            continue;
        }
        std::unordered_map<std::string, std::vector<EntryInterval>> intersection;
        for (const auto& entry : global.candidate_intervals) {
            auto other = intervals.find(entry.first);
            if (other == intervals.end()) {
                continue;
            }
            auto common = IntersectIntervals(entry.second, other->second);
            if (!common.empty()) {
                intersection.emplace(entry.first, std::move(common));
            }
        }
        global.candidate_intervals.swap(intersection);
        ++global.predicate_intersections;
        if (global.candidate_intervals.empty()) {
            break;
        }
    }
}

} // namespace duckdb::rootlake
