#pragma once

#include "root4duckdb/reader/root_semantic_types.hpp"

class TFile;
class TTree;

namespace duckdb::rootlake {

enum class RootDictionaryCleanupMode : uint8_t { FULL = 0, DESTRUCT_ONLY = 1, RETAIN = 2 };

/// Parses the object cleanup policy used by PHAST dictionaries.
RootDictionaryCleanupMode
ParseDictionaryCleanupMode(std::string mode,
                           RootDictionaryCleanupMode automatic_mode = RootDictionaryCleanupMode::FULL);

/// Owns one dictionary-created ROOT object and its branch address.
class RootObjectContext {
  public:
    /// @name Ownership
    /// @{
    RootObjectContext();
    RootObjectContext(const RootObjectContext&) = delete;
    RootObjectContext& operator=(const RootObjectContext&) = delete;
    RootObjectContext(RootObjectContext&& other) noexcept;
    RootObjectContext& operator=(RootObjectContext&& other) noexcept;
    ~RootObjectContext();
    /// @}

    /// @name Object access
    /// @{
    void Bind(TTree* tree, TBranch* branch, TClass* root_class,
              RootDictionaryCleanupMode cleanup_mode = RootDictionaryCleanupMode::FULL, std::string class_name = {});
    void* Read(uint64_t entry);
    void* CurrentObject() const;
    void Reset();
    /// @}

    TTree* tree = nullptr;
    TBranch* branch = nullptr;
    TClass* root_class = nullptr;
    void* owned_object = nullptr;
    void* address_slot = nullptr;
    RootDictionaryCleanupMode cleanup_mode = RootDictionaryCleanupMode::FULL;

  private:
    void MoveFrom(RootObjectContext&& other);

    std::string class_name;
};

/// Materializes top-level ROOT objects through TTree::GetEntry.
class RootObjectReader {
  public:
    /// @name Ownership
    /// @{
    RootObjectReader();
    RootObjectReader(const RootObjectReader&) = delete;
    RootObjectReader& operator=(const RootObjectReader&) = delete;
    RootObjectReader(RootObjectReader&&) noexcept;
    RootObjectReader& operator=(RootObjectReader&&) noexcept;
    /// @}

    /// @name Reader lifecycle
    /// @{
    void Bind(TFile* file, const std::string& tree_name, const std::string& root_class_name,
              RootDictionaryCleanupMode cleanup_mode = RootDictionaryCleanupMode::FULL);
    void Reset();
    void* Read(uint64_t entry);
    /// @}

    /// @name Bound ROOT state
    /// @{
    bool IsBound() const;
    TFile* File() const;
    TTree* Tree() const;
    TBranch* ObjectBranch() const;
    TClass* RootClass() const;
    void* CurrentObject() const;
    /// @}

  private:
    TFile* file = nullptr;
    RootObjectContext context;
};

/// Caches one object load shared by all projected paths for an entry.
class RootEntryReader {
  public:
    /// Creates an entry cache over an already bound object reader.
    explicit RootEntryReader(RootObjectReader& reader);

    /// @name Entry lifecycle
    /// @{
    void Begin(uint64_t entry);
    void* Read();
    void Invalidate();
    uint64_t LoadCount() const;
    /// @}

  private:
    RootObjectReader& reader;
    uint64_t entry = 0;
    void* object = nullptr;
    bool loaded = false;
    uint64_t load_count = 0;
};

/// Resolves trees, branches, physical ancestors, and schema identity.
/// @{
TTree* FindTree(TFile* file, const std::string& tree_name, const std::string& root_class);
TBranch* FindObjectBranch(TTree* tree, const std::string& root_class);
void CollectBranchTree(TBranch* branch, std::vector<TBranch*>& out);
bool BranchNameEndsWithToken(const std::string& name, const std::string& token);
bool ContainsTokensInOrder(const std::string& name, const std::vector<std::string>& tokens);
TBranch* FindPhysicalBranch(TBranch* object_branch, const std::vector<std::string>& fields);
bool HasPersistentBaskets(TBranch* branch);
PhysicalBranchResolution ResolvePhysicalBranch(TBranch* object_branch, const std::vector<std::string>& fields);
std::string SchemaFingerprint(const std::string& root_class, const std::vector<PathLevel>& levels);
/// @}

} // namespace duckdb::rootlake
