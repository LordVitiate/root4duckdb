#include "root4duckdb/reader/root_path_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/reader/root_branch_projection.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace duckdb::rootlake {

namespace {

std::string PrimitiveText(const RootPrimitiveValue& value) {
    switch (value.kind) {
    case RootPrimitiveKind::SIGNED:
        return std::to_string(value.signed_value);
    case RootPrimitiveKind::UNSIGNED:
        return std::to_string(value.unsigned_value);
    case RootPrimitiveKind::FLOATING: {
        std::ostringstream stream;
        stream << std::setprecision(17) << value.floating_value;
        return stream.str();
    }
    }
    return "unknown";
}

template <class VALUE, class FORMAT>
std::string ValidationMismatch(const std::vector<VALUE>& decoded, const std::vector<int32_t>& decoded_indices,
                               const std::vector<VALUE>& reference, const std::vector<int32_t>& reference_indices,
                               FORMAT&& format) {
    std::string reason = "serialized values differ from universal ROOT reader"
                         " (decoded_values=" +
                         std::to_string(decoded.size()) + ", reference_values=" +
                         std::to_string(reference.size()) + ", decoded_indices=" +
                         std::to_string(decoded_indices.size()) + ", reference_indices=" +
                         std::to_string(reference_indices.size());
    const auto value_count = std::min(decoded.size(), reference.size());
    for (size_t i = 0; i < value_count; ++i) {
        if (decoded[i] != reference[i] && !(std::isnan(decoded[i]) && std::isnan(reference[i]))) {
            reason += ", first_value=" + std::to_string(i) + ", decoded=" + format(decoded[i]) +
                      ", reference=" + format(reference[i]);
            break;
        }
    }
    const auto index_count = std::min(decoded_indices.size(), reference_indices.size());
    for (size_t i = 0; i < index_count; ++i) {
        if (decoded_indices[i] != reference_indices[i]) {
            reason += ", first_index=" + std::to_string(i) + ", decoded_index=" +
                      std::to_string(decoded_indices[i]) + ", reference_index=" +
                      std::to_string(reference_indices[i]);
            break;
        }
    }
    reason += ')';
    return reason;
}

bool PrimitiveEqual(const RootPrimitiveValue& left, const RootPrimitiveValue& right) {
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
    case RootPrimitiveKind::SIGNED:
        return left.signed_value == right.signed_value;
    case RootPrimitiveKind::UNSIGNED:
        return left.unsigned_value == right.unsigned_value;
    case RootPrimitiveKind::FLOATING:
        return left.floating_value == right.floating_value ||
               (std::isnan(left.floating_value) && std::isnan(right.floating_value));
    }
    return false;
}

} // namespace

RootPathReader::RootPathReader() = default;
RootPathReader::~RootPathReader() = default;
RootPathReader::RootPathReader(RootPathReader&&) noexcept = default;
RootPathReader& RootPathReader::operator=(RootPathReader&&) noexcept = default;

bool RootPathReader::SerializedActive() const {
    return serialized_active;
}

bool RootPathReader::FallbackRecorded() const {
    return fallback_recorded;
}

uint32_t RootPathReader::ValidationRemaining() const {
    return validation_remaining;
}

idx_t RootPathReader::IndexDepth() const {
    return index_depth;
}

const ParsedPath& RootPathReader::Path() const {
    return path;
}

const std::vector<PathLevel>& RootPathReader::Levels() const {
    return levels;
}

TBranch* RootPathReader::PhysicalBranch() const {
    return physical_branch;
}

const std::string& RootPathReader::PhysicalMode() const {
    return physical_mode;
}

const SerializedReadPlan& RootPathReader::SerializedPlan() const {
    return serialized_plan;
}

void RootPathReader::Resolve(TTree* tree_p, TBranch* object_branch, TClass* root_class, ParsedPath path_p,
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

RootPathReaderStartResult RootPathReader::StartSerialized(RootPathReaderOptions options_p,
                                                          std::string rejection_reason) {
    options = std::move(options_p);
    validation_remaining = options.validation_entries;
    serialized_active = false;
    fallback_recorded = false;

    if (options.reader_mode == RootReaderMode::OBJECT) {
        return {};
    }
    if (rejection_reason.empty() && !serialized_plan.supported) {
        rejection_reason = serialized_plan.reason.empty() ? "serialized reader is unavailable" : serialized_plan.reason;
    }
    if (!rejection_reason.empty()) {
        const bool activated = ActivateFallback(rejection_reason);
        return {false, activated};
    }

    if (validation_remaining > 0 && !BindIsolatedValidationReader(rejection_reason)) {
        const bool activated = ActivateFallback(rejection_reason);
        return {false, activated};
    }

    serialized_reader.Bind(physical_branch, serialized_plan, options.max_entry_bytes, options.max_values_per_entry);
    serialized_active = true;
    return {true, false};
}

RootPathReadResult RootPathReader::TryReadSerialized(uint64_t entry, RootEntryReader& object_entry,
                                                     std::vector<double>& values, std::vector<int32_t>& flat_indices,
                                                     bool collect_indices) {
    if (!serialized_active) {
        return {};
    }

    std::vector<double> reference_values;
    std::vector<int32_t> reference_indices;
    void* reference_object = nullptr;
    if (validation_remaining > 0) {
        // The oracle owns a separate TFile/TTree so ROOT branch addresses and
        // associative-container staging cannot affect the projected reader.
        reference_object = object_entry.ReadFrom(validation_reader);
        if (reference_object) {
            OffsetValueReader::CollectFlat(reference_object, levels, index_depth, reference_values,
                                           reference_indices);
        }
    }

    std::string failure_reason;
    const bool decoded = serialized_reader.Decode(entry, values, flat_indices, failure_reason,
                                                  collect_indices || validation_remaining > 0);
    if (!decoded) {
        return {false, false, ActivateFallback(failure_reason)};
    }

    if (validation_remaining > 0) {
        if (!reference_object ||
            !EqualDecodedValues(values, flat_indices, reference_values, reference_indices)) {
            const auto reason = ValidationMismatch(values, flat_indices, reference_values, reference_indices,
                                                   [](double value) {
                                                       std::ostringstream stream;
                                                       stream << std::setprecision(17) << value;
                                                       return stream.str();
                                                   });
            return {true, false, ActivateFallback(reason)};
        }
        --validation_remaining;
    }
    return {true, true, false};
}

RootPathReadResult RootPathReader::TryReadSerialized(uint64_t entry, RootEntryReader& object_entry,
                                                     std::vector<RootPrimitiveValue>& values,
                                                     std::vector<int32_t>& flat_indices, bool collect_indices) {
    values.clear();
    if (!serialized_active) {
        return {};
    }

    std::vector<RootPrimitiveValue> reference_values;
    std::vector<int32_t> reference_indices;
    void* reference_object = nullptr;
    if (validation_remaining > 0) {
        // Keep the oracle independent from branch-local addresses and cursors.
        reference_object = object_entry.ReadFrom(validation_reader);
        if (reference_object) {
            CollectTypedFlat(reference_object, reference_values, reference_indices);
        }
    }

    std::string failure_reason;
    const bool decoded = serialized_reader.Decode(entry, values, flat_indices, failure_reason,
                                                  collect_indices || validation_remaining > 0);
    if (!decoded) {
        return {false, false, ActivateFallback(failure_reason)};
    }

    if (validation_remaining > 0) {
        if (!reference_object ||
            !EqualDecodedValues(values, flat_indices, reference_values, reference_indices)) {
            std::string reason = "serialized values differ from universal ROOT reader"
                                 " (decoded_values=" +
                                 std::to_string(values.size()) + ", reference_values=" +
                                 std::to_string(reference_values.size()) + ", decoded_indices=" +
                                 std::to_string(flat_indices.size()) + ", reference_indices=" +
                                 std::to_string(reference_indices.size());
            const auto value_count = std::min(values.size(), reference_values.size());
            for (size_t i = 0; i < value_count; ++i) {
                if (!PrimitiveEqual(values[i], reference_values[i])) {
                    reason += ", first_value=" + std::to_string(i) + ", decoded=" + PrimitiveText(values[i]) +
                              ", reference=" + PrimitiveText(reference_values[i]);
                    break;
                }
            }
            const auto index_count = std::min(flat_indices.size(), reference_indices.size());
            for (size_t i = 0; i < index_count; ++i) {
                if (flat_indices[i] != reference_indices[i]) {
                    reason += ", first_index=" + std::to_string(i) + ", decoded_index=" +
                              std::to_string(flat_indices[i]) + ", reference_index=" +
                              std::to_string(reference_indices[i]);
                    break;
                }
            }
            reason += ')';
            return {true, false, ActivateFallback(reason)};
        }
        --validation_remaining;
    }
    return {true, true, false};
}

void RootPathReader::Reset() {
    serialized_reader.Reset();
    validation_reader.Reset();
    validation_file.reset();
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

void RootPathReader::CollectValues(void* object, std::vector<double>& values) const {
    OffsetValueReader::CollectValues(object, levels, values);
}

void RootPathReader::CollectFlat(void* object, std::vector<double>& values, std::vector<int32_t>& flat_indices) const {
    OffsetValueReader::CollectFlat(object, levels, index_depth, values, flat_indices);
}

void RootPathReader::CollectTypedValues(void* object, std::vector<RootPrimitiveValue>& values) const {

    ReadResult result;

    OffsetValueReader::CollectDirect(object, levels, -1, 0, result);

    values = std::move(result.numbers);
}

void RootPathReader::CollectTypedFlat(void* object, std::vector<RootPrimitiveValue>& values,
                                      std::vector<int32_t>& flat_indices) const {

    ReadResult result;

    OffsetValueReader::CollectDirect(object, levels, -1, 0, result);

    values = std::move(result.numbers);

    flat_indices.clear();
    flat_indices.reserve(result.vector_indices.size() * index_depth);

    for (const auto& indices : result.vector_indices) {
        if (indices.size() != index_depth) {
            throw IOException("ROOT container depth mismatch in typed path reader: "
                              "expected " +
                              std::to_string(index_depth) + ", got " + std::to_string(indices.size()));
        }

        for (const auto index : indices) {
            flat_indices.push_back(static_cast<int32_t>(index));
        }
    }
}

void RootPathReader::CollectDirect(void* object, int64_t max_values, int64_t event_id, ReadResult& result) const {
    OffsetValueReader::CollectDirect(object, levels, max_values, event_id, result);
}

const SerializedReadCounters& RootPathReader::SerializedCounters() const {
    return serialized_reader.Counters();
}

bool RootPathReader::CurrentBasketInfo(SerializedBasketInfo& info) const {
    return serialized_reader.CurrentBasketInfo(info);
}

bool RootPathReader::BindIsolatedValidationReader(std::string& failure_reason) {
    auto* source_file = tree ? tree->GetCurrentFile() : nullptr;
    const std::string source_name = source_file && source_file->GetName() ? source_file->GetName() : "";
    const std::string tree_name = tree && tree->GetName() ? tree->GetName() : "";
    if (source_name.empty() || tree_name.empty() || path.root_class.empty()) {
        failure_reason = "serialized validation cannot resolve the source ROOT file, tree or class";
        return false;
    }

    try {
        validation_file.reset(TFile::Open(source_name.c_str(), "READ"));
        if (!validation_file || validation_file->IsZombie()) {
            validation_file.reset();
            failure_reason = "serialized validation cannot open an isolated ROOT file context";
            return false;
        }
        validation_reader.Bind(validation_file.get(), tree_name, path.root_class, options.dictionary_cleanup_mode);
        if (validation_reader.Tree() == tree) {
            validation_reader.Reset();
            validation_file.reset();
            failure_reason = "serialized validation ROOT context is not isolated from the projected tree";
            return false;
        }
        EnableAllBranches(validation_reader.Tree(), options.tree_cache_bytes);
        RootDebug("SERIALIZED.VALIDATION_ISOLATED",
                  "path=" + serialized_plan.logical_path + " file=" + source_name + " tree=" + tree_name);
        return true;
    } catch (const std::exception& exception) {
        validation_reader.Reset();
        validation_file.reset();
        failure_reason = std::string("serialized validation cannot create an isolated ROOT context: ") +
                         exception.what();
        return false;
    }
}

bool RootPathReader::ActivateFallback(const std::string& reason) {
    if (options.reader_mode == RootReaderMode::SERIALIZED) {
        throw IOException("reader_mode='serialized' cannot " + options.operation + " " + serialized_plan.logical_path +
                          ": " + reason);
    }
    serialized_active = false;
    serialized_reader.ReleaseBindings();
    if (options.enable_all_branches_on_fallback && tree) {
        EnableAllBranches(tree, options.tree_cache_bytes);
    }
    if (fallback_recorded) {
        return false;
    }
    fallback_recorded = true;
    WarnRootFallbackOnce(serialized_plan.logical_path, serialized_plan.schema_fingerprint, reason);
    return true;
}

} // namespace duckdb::rootlake
