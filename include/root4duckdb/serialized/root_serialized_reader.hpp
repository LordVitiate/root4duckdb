#ifndef ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_READER_HPP
#define ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_READER_HPP

#include "root4duckdb/core/root_lake_common.hpp"
#include "root4duckdb/reader/root_semantic_reader.hpp"
#include "root4duckdb/serialized/root_serialized_basket_cache.hpp"
#include "root4duckdb/serialized/root_serialized_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

class TBranch;
class TBasket;
class TBufferFile;
class TLeaf;
namespace TStreamerInfoActions
{
class TActionSequence;
}

namespace duckdb::rootlake
{

/// Requested basket decoding strategy.
enum class RootReaderMode : uint8_t
{
    AUTO = 0,
    SERIALIZED = 1,
    OBJECT = 2
};

/// Supported serialized projection shapes.
enum class SerializedProjectionKind : uint8_t
{
    FIXED_MEMBER = 0,
    NESTED_PRIMITIVE_VECTOR = 1,
    // Materialize the first selected subtree, then traverse the remaining semantic path.
    SELECTED_SUBTREE = 2,
    /// A self-contained primitive split branch read without a root object.
    LEAF_BRANCH = 3,
    /// Member-wise actions over an unsplit top-level branch or one standalone inline-object member branch.
    ROOT_SELECTED_SUBTREE = 4,
    /// One self-contained split STL member decoded into a private root scratch object.
    COLLECTION_BRANCH = 5,
    // Checked byte-offset decoder for nested vector<object>/vector<object>/fixed primitive.
    NESTED_FIXED_MEMBER = 6
};

/// Parses and formats reader and projection modes.
/// @{
RootReaderMode ParseRootReaderMode(std::string mode);
const char* RootReaderModeName(RootReaderMode mode);
bool IsSerializedNestedProjection(SerializedProjectionKind kind);
const char* SerializedProjectionName(SerializedProjectionKind kind);
/// @}

struct SerializedPlanRejected
{
    std::string reason;
};
struct SerializedFixedProjection
{
};
struct SerializedNestedPrimitiveProjection
{
};
struct SerializedSelectedSubtreeProjection
{
};
struct SerializedLeafBranchProjection
{
};
struct SerializedRootSubtreeProjection
{
};
struct SerializedCollectionBranchProjection
{
};
struct SerializedNestedFixedProjection
{
};

using SerializedProjectionState =
    std::variant<SerializedPlanRejected, SerializedFixedProjection, SerializedNestedPrimitiveProjection,
                 SerializedSelectedSubtreeProjection, SerializedLeafBranchProjection, SerializedRootSubtreeProjection,
                 SerializedCollectionBranchProjection, SerializedNestedFixedProjection>;

/// Conservative plan for decoding one physical basket projection.
struct SerializedReadPlan
{
    SerializedProjectionState projection;
    std::string logical_path;
    std::string root_class;
    std::string container_name;
    std::string element_class;
    std::string projected_member_name;
    std::string value_type;
    std::string physical_branch_name;
    std::string schema_fingerprint;
    // Loaded dictionaries own these pointers; ROOT resolves variable-width prefixes.
    TClass* outer_container_class = nullptr;
    TClass* outer_element_class = nullptr;
    TClass* nested_container_class = nullptr;
    TClass* nested_element_class = nullptr;
    std::string nested_member_name;
    TClass* scratch_class = nullptr;
    std::vector<int> prefix_element_ids;
    std::vector<int> root_action_ids;
    // Needed when ROOT materializes one selected member directly from the
    // basket before the offset walker projects an arbitrary-depth child.
    std::vector<PathLevel> projection_levels;
    uint32_t streamer_version = 0;
    uint32_t bytes_before_value_per_element = 0;
    uint32_t value_bytes = 0;
    uint64_t fixed_array_length = 1;
    std::vector<uint32_t> array_dimensions;
    idx_t index_depth = 0;

    [[nodiscard]] bool Supported() const noexcept;
    [[nodiscard]] bool Is(SerializedProjectionKind kind) const noexcept;
    [[nodiscard]] SerializedProjectionKind Kind() const;
    [[nodiscard]] const std::string& RejectionReason() const noexcept;
    void Select(SerializedProjectionKind kind);
    void Reject(std::string reason);
};

/// Builds a conservative plan; unsupported layouts use object fallback.
SerializedReadPlan BuildSerializedReadPlan(TClass* root_class, const ParsedPath& path, TBranch* physical_branch);

/// Configures nested serialized projections from streamer metadata.
bool ConfigureSerializedDeepProjection(const ParsedPath& path, const std::vector<PathLevel>& levels,
                                       TClass* outer_element_class, SerializedReadPlan& plan,
                                       std::string& failure_reason);

/// Resolves a versioned member action sequence.
bool ResolveSerializedVersionedMember(const SerializedReadPlan& plan, int32_t element_version,
                                      std::vector<int>& prefix_element_ids, std::string& failure_reason);

/// Resolves a nested collection version and its prefix actions.
bool ResolveSerializedNestedVersion(const SerializedReadPlan& plan, int32_t element_version,
                                    int32_t& resolved_element_version, std::vector<int>& prefix_element_ids,
                                    std::string& failure_reason);

/// Rebuilds a fixed primitive projection from the on-file element version.
bool ResolveSerializedFixedLayout(const SerializedReadPlan& plan, int32_t element_version,
                                  SerializedEntryLayout& resolved_layout, std::string& failure_reason);

/// Calculates the versioned offset of a nested fixed member using the fixed-member validator.
bool ResolveSerializedNestedFixedLayout(const SerializedReadPlan& plan, int32_t element_version,
                                        SerializedEntryLayout& resolved_layout, std::string& failure_reason);

// Validates the complete versioned member-wise element layout so that the
// decoder can advance between nested vectors without materializing PaTPar.
bool ResolveSerializedNestedMemberWiseStride(const SerializedReadPlan& plan, int32_t element_version,
                                             uint64_t& bytes_per_element, std::string& failure_reason);

/// Version-specific fixed layout is owned by a single worker-local reader.
struct SerializedNestedFixedLayoutCache
{
    int32_t element_version = -1;
    SerializedEntryLayout layout;
    uint64_t element_stride = 0;
};

/// Reads a nested member-wise vector directly by checked serialized byte offsets.
bool DecodeSerializedNestedFixedColumn(TBufferFile& buffer, const SerializedReadPlan& plan, size_t entry_size,
                                       uint64_t outer_count, uint64_t max_values_per_entry,
                                       SerializedNestedFixedLayoutCache& layout_cache,
                                       std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                                       std::string& failure_reason, bool collect_indices);

/// Lets ROOT consume selected member-wise prefix actions.
bool ConsumeSerializedSelectedMembers(TBufferFile& buffer, const SerializedReadPlan& plan, int32_t element_version,
                                      uint64_t outer_count, void* outer_collection_scratch,
                                      const std::vector<int>& prefix_element_ids,
                                      std::unique_ptr<TStreamerInfoActions::TActionSequence>& cached_actions,
                                      std::string& failure_reason);

/// Projects nested objects from the serialized scratch object.
bool CollectSerializedSelectedSubtree(const SerializedReadPlan& plan, void* outer_collection_scratch,
                                      uint64_t max_values_per_entry, std::vector<RootPrimitiveValue>& values,
                                      std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                      bool collect_indices);

/// Per-reader physical decode counters.
struct SerializedReadCounters
{
    uint64_t entries = 0;
    uint64_t values = 0;
    uint64_t baskets = 0;
    uint64_t compressed_bytes = 0;
    uint64_t serialized_bytes = 0;
    // Actual executed nested-fixed backends, not the chosen planner kind.
    uint64_t nested_fixed_direct_entries = 0;
    uint64_t nested_fixed_scratch_entries = 0;
};

/// Owns one serialized basket decode state.
class SerializedBasketReader
{
  public:
    /// @name Ownership
    /// @{
    SerializedBasketReader();
    ~SerializedBasketReader() noexcept;
    SerializedBasketReader(const SerializedBasketReader&) = delete;
    SerializedBasketReader& operator=(const SerializedBasketReader&) = delete;
    SerializedBasketReader(SerializedBasketReader&&) noexcept;
    SerializedBasketReader& operator=(SerializedBasketReader&&) noexcept;
    /// @}

    /// @name Decode lifecycle
    /// @{
    void Bind(TBranch* branch_p, SerializedReadPlan plan_p, uint64_t max_entry_bytes_p = 64ULL * 1024ULL * 1024ULL,
              uint64_t max_values_per_entry_p = 10ULL * 1024ULL * 1024ULL,
              std::shared_ptr<SerializedBasketCache> shared_basket_cache_p = {});
    /// Detaches branch-local ROOT addresses without discarding metrics/plan state.
    void ReleaseBindings() noexcept;
    void Reset() noexcept;

    bool Decode(uint64_t entry, std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& flat_indices,
                std::string& failure_reason, bool collect_indices = true);
    /// @}

    /// Serialize fixed members in bounded chunks without materializing the entry.
    bool BeginPagedEntry(uint64_t entry, std::string& reason);
    bool ReadPage(size_t rows, std::vector<RootPrimitiveValue>& values, std::vector<int32_t>& indices,
                  std::string& reason);
    [[nodiscard]] bool HasPage() const noexcept;
    void EndPagedEntry() noexcept;

    /// @name Reader state
    /// @{
    bool IsBound() const;
    const SerializedReadPlan& Plan() const;
    const SerializedReadCounters& Counters() const;
    bool CurrentBasketInfo(SerializedBasketInfo& info) const;
    /// @}

  private:
    bool LoadBasket(uint64_t entry, std::string& failure_reason);
    bool EntrySlice(uint64_t entry, const uint8_t*& begin, size_t& size, std::string& failure_reason);
    bool DecodeNestedProjectionEntry(const uint8_t* bytes, size_t entry_size, std::vector<RootPrimitiveValue>& values,
                                     std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                     bool collect_indices);
    bool DecodeRootProjectionEntry(const uint8_t* bytes, size_t entry_size, std::vector<RootPrimitiveValue>& values,
                                   std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                   bool collect_indices);
    bool PrepareFixedProjectionEntry(const uint8_t* bytes, size_t entry_size, size_t& payload_offset, uint64_t& count,
                                     std::string& reason);
    bool DecodeFixedProjectionEntry(const uint8_t* bytes, size_t entry_size, std::vector<RootPrimitiveValue>& values,
                                    std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                    bool collect_indices);
    bool DecodeLeafBranchEntry(uint64_t entry, std::vector<RootPrimitiveValue>& values,
                               std::vector<int32_t>& flat_indices, std::string& failure_reason, bool collect_indices);
    bool EnsureLeafScratch(uint64_t value_count, std::string& failure_reason);
    TBranch* branch = nullptr;
    TBasket* basket = nullptr;
    bool basket_prepared = false;
    SerializedReadPlan plan;
    SerializedEntryLayout layout;
    SerializedVectorPageCursor page_cursor;
    int current_basket = -1;
    uint64_t current_basket_entry_begin = 0;
    uint64_t current_basket_entry_end = 0;
    uint64_t max_entry_bytes = 0;
    uint64_t max_values_per_entry = 0;
    // Standalone outer collection used while consuming member prefixes and
    // selected subtrees. It never points into a reconstructed root object.
    void* outer_collection_scratch = nullptr;
    bool owns_outer_collection_scratch = false;
    TLeaf* leaf = nullptr;
    std::vector<std::max_align_t> leaf_scratch;
    uint64_t leaf_scratch_capacity = 0;
    bool leaf_make_class = false;
    std::shared_ptr<SerializedBasketCache> shared_basket_cache;
    int32_t resolved_element_version = -1;
    std::vector<int> resolved_prefix_element_ids;
    SerializedNestedFixedLayoutCache nested_fixed_layout_cache;
    std::unique_ptr<TStreamerInfoActions::TActionSequence> cached_action_sequence;
    uint32_t observed_memberwise_header = 0;
    SerializedReadCounters counters;
};

/// Emits one fallback warning per process, schema, path, and reason.
void WarnRootFallbackOnce(const std::string& logical_path, const std::string& schema_fingerprint,
                          const std::string& reason);

} // namespace duckdb::rootlake

#endif
