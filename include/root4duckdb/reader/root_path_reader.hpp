#ifndef ROOT4DUCKDB_READER_ROOT_PATH_READER_HPP
#define ROOT4DUCKDB_READER_ROOT_PATH_READER_HPP

#include "root4duckdb/reader/root_semantic_reader.hpp"
#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb::rootlake
{

/// Shared access limits and cleanup policy for scans and index builds.
struct RootAccessOptions
{
    RootReaderMode reader_mode = RootReaderMode::AUTO;
    uint32_t validation_entries = 0;
    uint64_t max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    bool enable_all_branches_on_fallback = false;
    RootDictionaryCleanupMode dictionary_cleanup_mode = RootDictionaryCleanupMode::FULL;
    std::string operation = "read";

    void Validate() const;
};

using RootPathReaderOptions = RootAccessOptions;

enum class RootPathStartRoute : uint8_t
{
    OBJECT_ONLY = 0,
    SERIALIZED = 1,
    OBJECT_FALLBACK = 2
};

/// Mutually exclusive result of starting the serialized reader.
struct RootPathReaderStartResult
{
    RootPathStartRoute route = RootPathStartRoute::OBJECT_ONLY;

    [[nodiscard]] bool SerializedActive() const noexcept;
    [[nodiscard]] bool FallbackActivated() const noexcept;
};

enum class RootPathReadRoute : uint8_t
{
    NOT_ATTEMPTED = 0,
    SERIALIZED = 1,
    OBJECT_FALLBACK = 2
};

/// Mutually exclusive routing result of one serialized read attempt.
struct RootPathReadResult
{
    RootPathReadRoute route = RootPathReadRoute::NOT_ATTEMPTED;
    // A validation mismatch may decode successfully and still select fallback.
    bool attempt_decoded = false;

    [[nodiscard]] bool Decoded() const noexcept;
    [[nodiscard]] bool Serialized() const noexcept;
    [[nodiscard]] bool FallbackActivated() const noexcept;
};

/// Origin of a numeric page returned by the common path reader.
enum class RootNumericPageSource : uint8_t
{
    NONE = 0,
    SERIALIZED = 1,
    OBJECT = 2
};

/// A single entry-start decision for the indexed numeric dataset reader.
enum class RootNumericEntryRoute : uint8_t
{
    NONE = 0,
    SERIALIZED_VALUES = 1,
    SERIALIZED_PAGE = 2,
    OBJECT_PAGE = 3
};

struct RootNumericEntryStartResult
{
    RootNumericEntryRoute route = RootNumericEntryRoute::NONE;
    bool attempt_decoded = false;
    bool fallback_activated = false;
};

class RootPathReader
{
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
    void Resolve(TTree* tree_p, TBranch* object_branch, TClass* root_class, ParsedPath path_p,
                 std::vector<PathLevel> levels_p);
    /// Adopt an already validated semantic path for compatibility traversal.
    /// Direct scans can reuse this reader without re-planning serialized access.
    void BindObjectPath(std::vector<PathLevel> levels_p);
    RootPathReaderStartResult StartSerialized(RootPathReaderOptions options_p, std::string rejection_reason = {});
    void SetSerializedBasketCache(std::shared_ptr<SerializedBasketCache> cache);
    // Reuse the physical basket cache across paths within one ROOT worker/file.
    void AttachSharedBasketCache(std::unordered_map<TBranch*, std::shared_ptr<SerializedBasketCache>>& caches);
    /// Page-oriented compatibility traversal owned by the resolved path.
    void BeginObjectEntry(void* object, int64_t entry_id);
    bool ReadObjectPage(idx_t maximum_rows, ReadResult& result);
    [[nodiscard]] bool HasObjectPage() const noexcept;
    void EndObjectEntry() noexcept;
    RootPathReadResult TryReadSerialized(uint64_t entry, RootEntryReader& object_entry,
                                         std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                         bool collect_indices = true);
    /// Choose serialized pages, serialized values, or object pages once per entry.
    RootNumericEntryStartResult BeginNumericEntry(uint64_t entry, RootEntryReader& object_entry, int64_t entry_id,
                                                  std::vector<RootPrimitiveValue>& values,
                                                  std::vector<int32_t>& flat_indices);
    RootNumericPageSource ReadNumericPage(idx_t rows, std::vector<RootPrimitiveValue>& values,
                                          std::vector<int32_t>& flat_indices);
    [[nodiscard]] bool HasNumericPage() const noexcept;
    void EndNumericPage() noexcept;
    void Reset();
    /// @}

    /// @name Reader state
    /// @{
    bool SerializedActive() const;
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
    void CollectTypedFlat(void* object, std::vector<RootPrimitiveValue>& values,
                          std::vector<int32_t>& flat_indices) const;
    struct ObjectCursorState;
    bool BindIsolatedValidationReader(std::string& failure_reason);
    bool ActivateFallback(const std::string& reason);
    RootPathReadResult BeginSerializedPage(uint64_t entry);

    TTree* tree = nullptr;
    ParsedPath path;
    std::vector<PathLevel> levels;
    TBranch* physical_branch = nullptr;
    std::string physical_mode;
    idx_t index_depth = 0;
    RootPathReaderOptions options;
    SerializedReadPlan serialized_plan;
    std::shared_ptr<SerializedBasketCache> serialized_basket_cache;
    SerializedBasketReader serialized_reader;
    std::unique_ptr<TFile> validation_file;
    // A heap-held state preserves cursor level references across move operations.
    std::unique_ptr<ObjectCursorState> object_cursor_state;
    RootObjectReader validation_reader;
    bool serialized_active = false;
    bool fallback_recorded = false;
    uint32_t validation_remaining = 0;
};

} // namespace duckdb::rootlake

#endif
