#include "include/root_path_reader.hpp"

#include "include/root_branch_projection.hpp"

#include "duckdb/common/exception.hpp"

#include <utility>

namespace duckdb::rootlake {

void RootPathReader::Resolve(TTree *tree_p, TBranch *object_branch,
                             TClass *root_class, ParsedPath path_p,
                             std::vector<PathLevel> levels_p) {
    Reset();
    tree = tree_p;
    path = std::move(path_p);
    levels = std::move(levels_p);
    index_depth = rootlake::IndexDepth(levels);

    const auto physical = ResolvePhysicalBranch(object_branch, path.fields);
    physical_branch = physical.branch;
    physical_mode = physical.mode;
    serialized_plan = BuildSerializedReadPlan(root_class, path, physical_branch);
}

RootPathReaderStartResult RootPathReader::StartSerialized(
    void *root_object_scratch, RootPathReaderOptions options_p,
    std::string rejection_reason) {
    options = std::move(options_p);
    validation_remaining = options.validation_entries;
    serialized_active = false;
    fallback_recorded = false;

    if (options.reader_mode == RootReaderMode::OBJECT) return {};
    if (rejection_reason.empty() &&
        serialized_plan.supported &&
        !IsLosslessDoubleBackedType(serialized_plan.value_type)) {
        rejection_reason =
            "serialized reader uses double-backed numeric transport for " +
            serialized_plan.value_type +
            "; universal object reader is required for lossless decoding";
    }
    if (rejection_reason.empty() && !serialized_plan.supported) {
        rejection_reason = serialized_plan.reason.empty()
                               ? "serialized reader is unavailable"
                               : serialized_plan.reason;
    }
    if (!rejection_reason.empty()) {
        const bool activated = ActivateFallback(rejection_reason);
        return {false, activated};
    }

    serialized_reader.Bind(
        physical_branch, serialized_plan, options.max_entry_bytes,
        options.max_values_per_entry, root_object_scratch);
    serialized_active = true;
    return {true, false};
}

RootPathReadResult RootPathReader::TryReadSerialized(
    uint64_t entry, RootEntryReader &object_entry,
    std::vector<double> &values, std::vector<int32_t> &flat_indices,
    bool collect_indices) {
    if (!serialized_active) return {};

    std::string failure_reason;
    const bool decoded = serialized_reader.Decode(
        entry, values, flat_indices, failure_reason,
        collect_indices || validation_remaining > 0);
    if (serialized_plan.projection_kind ==
        SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR) {
        object_entry.Invalidate();
    }
    if (!decoded) {
        return {false, false, ActivateFallback(failure_reason)};
    }

    if (validation_remaining > 0) {
        std::vector<double> reference_values;
        std::vector<int32_t> reference_indices;
        void *object = object_entry.Read();
        if (object) {
            OffsetValueReader::CollectFlat(
                object, levels, index_depth, reference_values,
                reference_indices);
        }
        if (!object || !EqualDecodedValues(
                           values, flat_indices, reference_values,
                           reference_indices)) {
            return {true, false, ActivateFallback(
                               "serialized values differ from universal ROOT reader")};
        }
        --validation_remaining;
    }
    return {true, true, false};
}

RootPathReadResult RootPathReader::TryReadSerialized(
    uint64_t entry, RootEntryReader &object_entry,
    std::vector<RootPrimitiveValue> &values,
    std::vector<int32_t> &flat_indices,
    bool collect_indices) {

    std::vector<double> decoded_values;

    const auto result = TryReadSerialized(
        entry,
        object_entry,
        decoded_values,
        flat_indices,
        collect_indices);

    values.clear();

    if (!result.serialized) {
        return result;
    }

    const auto logical_type =
        RootTypeToScanLogicalType(
            serialized_plan.value_type,
            false,
            true);

    values.reserve(decoded_values.size());

    for (const double value : decoded_values) {
        switch (logical_type.id()) {
        case LogicalTypeId::BOOLEAN:
            values.push_back(
                RootPrimitiveValue::Unsigned(
                    value != 0.0 ? 1 : 0));
            break;

        case LogicalTypeId::TINYINT:
        case LogicalTypeId::SMALLINT:
        case LogicalTypeId::INTEGER:
            values.push_back(
                RootPrimitiveValue::Signed(
                    static_cast<int64_t>(value)));
            break;

        case LogicalTypeId::UTINYINT:
        case LogicalTypeId::USMALLINT:
        case LogicalTypeId::UINTEGER:
            values.push_back(
                RootPrimitiveValue::Unsigned(
                    static_cast<uint64_t>(value)));
            break;

        case LogicalTypeId::FLOAT:
        case LogicalTypeId::DOUBLE:
            values.push_back(
                RootPrimitiveValue::Floating(value));
            break;

        case LogicalTypeId::BIGINT:
        case LogicalTypeId::UBIGINT:
            throw InternalException(
                "64-bit integer reached double-backed serialized "
                "transport despite lossless guard");

        default:
            throw NotImplementedException(
                "Unsupported serialized ROOT numeric type " +
                logical_type.ToString());
        }
    }

    return result;
}

void RootPathReader::Reset() {
    serialized_reader.Reset();
    tree = nullptr;
    path = {};
    levels.clear();
    physical_branch = nullptr;
    physical_mode.clear();
    index_depth = 0;
    options = {};
    serialized_plan = {};
    serialized_active = false;
    fallback_recorded = false;
    validation_remaining = 0;
}

void RootPathReader::CollectValues(void *object,
                                   std::vector<double> &values) const {
    OffsetValueReader::CollectValues(object, levels, values);
}

void RootPathReader::CollectFlat(
    void *object, std::vector<double> &values,
    std::vector<int32_t> &flat_indices) const {
    OffsetValueReader::CollectFlat(
        object, levels, index_depth, values, flat_indices);
}

void RootPathReader::CollectTypedValues(
    void *object,
    std::vector<RootPrimitiveValue> &values) const {

    ReadResult result;

    OffsetValueReader::CollectDirect(
        object,
        levels,
        -1,
        0,
        result);

    values = std::move(result.numbers);
}

void RootPathReader::CollectTypedFlat(
    void *object,
    std::vector<RootPrimitiveValue> &values,
    std::vector<int32_t> &flat_indices) const {

    ReadResult result;

    OffsetValueReader::CollectDirect(
        object,
        levels,
        -1,
        0,
        result);

    values = std::move(result.numbers);

    flat_indices.clear();
    flat_indices.reserve(
        result.vector_indices.size() * index_depth);

    for (const auto &indices : result.vector_indices) {
        if (indices.size() != index_depth) {
            throw IOException(
                "ROOT container depth mismatch in typed path reader: "
                "expected " +
                std::to_string(index_depth) +
                ", got " +
                std::to_string(indices.size()));
        }

        for (const auto index : indices) {
            flat_indices.push_back(
                static_cast<int32_t>(index));
        }
    }
}

void RootPathReader::CollectDirect(void *object, int64_t max_values,
                                   int64_t event_id,
                                   ReadResult &result) const {
    OffsetValueReader::CollectDirect(
        object, levels, max_values, event_id, result);
}

const SerializedReadCounters &RootPathReader::SerializedCounters() const {
    return serialized_reader.Counters();
}

bool RootPathReader::CurrentBasketInfo(SerializedBasketInfo &info) const {
    return serialized_reader.CurrentBasketInfo(info);
}

bool RootPathReader::ActivateFallback(const std::string &reason) {
    if (options.reader_mode == RootReaderMode::SERIALIZED) {
        throw IOException(
            "reader_mode='serialized' cannot " + options.operation + " " +
            serialized_plan.logical_path + ": " + reason);
    }
    serialized_active = false;
    if (options.enable_all_branches_on_fallback && tree) {
        EnableAllBranches(tree, options.tree_cache_bytes);
    }
    if (fallback_recorded) return false;
    fallback_recorded = true;
    WarnRootFallbackOnce(serialized_plan.logical_path,
                         serialized_plan.schema_fingerprint, reason);
    return true;
}

} // namespace duckdb::rootlake
