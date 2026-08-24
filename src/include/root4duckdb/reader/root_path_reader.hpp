#pragma once

#include "root4duckdb/reader/root_semantic_reader.hpp"
#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb::rootlake {

/// Serialized-first reader limits and fallback policy.
struct RootPathReaderOptions {
    RootReaderMode reader_mode = RootReaderMode::AUTO;
    uint32_t validation_entries = 0;
    uint64_t max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    bool enable_all_branches_on_fallback = false;
    RootDictionaryCleanupMode dictionary_cleanup_mode = RootDictionaryCleanupMode::FULL;
    std::string operation = "read";
};

/// Result of starting the serialized reader.
struct RootPathReaderStartResult {
    bool serialized_active = false;
    bool fallback_activated = false;
};

/// Result of one serialized read attempt.
struct RootPathReadResult {
    bool decoded = false;
    bool serialized = false;
    bool fallback_activated = false;
};

class RootPathReader {
  public:
    /// @name Ownership
    /// @{
    RootPathReader();
    ~RootPathReader();
    RootPathReader(const RootPathReader&) = delete;
    RootPathReader& operator=(const RootPathReader&) = delete;
    RootPathReader(RootPathReader&&) noexcept;
    RootPathReader& operator=(RootPathReader&&) noexcept;
    /// @}

    /// @name Serialized-first pipeline
    /// @{
    void Resolve(TTree* tree, TBranch* object_branch, TClass* root_class, ParsedPath path,
                 std::vector<PathLevel> levels);
    RootPathReaderStartResult StartSerialized(RootPathReaderOptions options, std::string rejection_reason = {});
    RootPathReadResult TryReadSerialized(uint64_t entry, RootEntryReader& object_entry, std::vector<double>& values,
                                         std::vector<int32_t>& flat_indices, bool collect_indices = true);
    RootPathReadResult TryReadSerialized(uint64_t entry, RootEntryReader& object_entry,
                                         std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                         bool collect_indices = true);
    void Reset();
    /// @}

    /// @name Universal object fallback
    /// @{
    void CollectValues(void* object, std::vector<double>& values) const;
    void CollectFlat(void* object, std::vector<double>& values, std::vector<int32_t>& flat_indices) const;
    void CollectTypedValues(void* object, std::vector<RootPrimitiveValue>& values) const;
    void CollectTypedFlat(void* object, std::vector<RootPrimitiveValue>& values,
                          std::vector<int32_t>& flat_indices) const;
    void CollectDirect(void* object, int64_t max_values, int64_t event_id, ReadResult& result) const;
    /// @}

    /// @name Reader state
    /// @{
    bool SerializedActive() const;
    bool FallbackRecorded() const;
    uint32_t ValidationRemaining() const;
    idx_t IndexDepth() const;
    const ParsedPath& Path() const;
    const std::vector<PathLevel>& Levels() const;
    TBranch* PhysicalBranch() const;
    const std::string& PhysicalMode() const;
    const SerializedReadPlan& SerializedPlan() const;
    const SerializedReadCounters& SerializedCounters() const;
    bool CurrentBasketInfo(SerializedBasketInfo& info) const;
    /// @}

  private:
    bool BindIsolatedValidationReader(std::string& failure_reason);
    bool ActivateFallback(const std::string& reason);

    TTree* tree = nullptr;
    ParsedPath path;
    std::vector<PathLevel> levels;
    TBranch* physical_branch = nullptr;
    std::string physical_mode;
    idx_t index_depth = 0;
    RootPathReaderOptions options;
    SerializedReadPlan serialized_plan;
    SerializedBasketReader serialized_reader;
    std::unique_ptr<TFile> validation_file;
    RootObjectReader validation_reader;
    bool serialized_active = false;
    bool fallback_recorded = false;
    uint32_t validation_remaining = 0;
};

} // namespace duckdb::rootlake
