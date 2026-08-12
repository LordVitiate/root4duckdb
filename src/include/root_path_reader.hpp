#pragma once

#include "root_semantic_reader.hpp"
#include "root_serialized_reader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake {

struct RootPathReaderOptions {
    RootReaderMode reader_mode = RootReaderMode::AUTO;
    uint32_t validation_entries = 4;
    uint64_t max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    bool enable_all_branches_on_fallback = false;
    std::string operation = "read";
};

struct RootPathReaderStartResult {
    bool serialized_active = false;
    bool fallback_activated = false;
};

struct RootPathReadResult {
    bool decoded = false;
    bool serialized = false;
    bool fallback_activated = false;
};

class RootPathReader {
public:
    RootPathReader() = default;
    RootPathReader(const RootPathReader &) = delete;
    RootPathReader &operator=(const RootPathReader &) = delete;
    RootPathReader(RootPathReader &&) noexcept = default;
    RootPathReader &operator=(RootPathReader &&) noexcept = default;

    void Resolve(TTree *tree, TBranch *object_branch, TClass *root_class,
                 ParsedPath path, std::vector<PathLevel> levels);
    RootPathReaderStartResult StartSerialized(
        void *root_object_scratch, RootPathReaderOptions options,
        std::string rejection_reason = {});
    RootPathReadResult TryReadSerialized(
        uint64_t entry, RootEntryReader &object_entry,
        std::vector<double> &values, std::vector<int32_t> &flat_indices,
        bool collect_indices = true);
    RootPathReadResult TryReadSerialized(
        uint64_t entry, RootEntryReader &object_entry,
        std::vector<RootPrimitiveValue> &values,
        std::vector<int32_t> &flat_indices,
        bool collect_indices = true);
    void Reset();

    void CollectValues(void *object, std::vector<double> &values) const;
    void CollectFlat(void *object, std::vector<double> &values,
                     std::vector<int32_t> &flat_indices) const;
    void CollectTypedValues(
        void *object,
        std::vector<RootPrimitiveValue> &values) const;
    void CollectTypedFlat(
        void *object,
        std::vector<RootPrimitiveValue> &values,
        std::vector<int32_t> &flat_indices) const;
    void CollectDirect(void *object, int64_t max_values, int64_t event_id,
                       ReadResult &result) const;

    bool SerializedActive() const { return serialized_active; }
    bool FallbackRecorded() const { return fallback_recorded; }
    uint32_t ValidationRemaining() const { return validation_remaining; }
    idx_t IndexDepth() const { return index_depth; }
    const ParsedPath &Path() const { return path; }
    const std::vector<PathLevel> &Levels() const { return levels; }
    TBranch *PhysicalBranch() const { return physical_branch; }
    const std::string &PhysicalMode() const { return physical_mode; }
    const SerializedReadPlan &SerializedPlan() const { return serialized_plan; }
    const SerializedReadCounters &SerializedCounters() const;
    bool CurrentBasketInfo(SerializedBasketInfo &info) const;

private:
    bool ActivateFallback(const std::string &reason);

    TTree *tree = nullptr;
    ParsedPath path;
    std::vector<PathLevel> levels;
    TBranch *physical_branch = nullptr;
    std::string physical_mode;
    idx_t index_depth = 0;
    RootPathReaderOptions options;
    SerializedReadPlan serialized_plan;
    SerializedBasketReader serialized_reader;
    bool serialized_active = false;
    bool fallback_recorded = false;
    uint32_t validation_remaining = 0;
};

} // namespace duckdb::rootlake
