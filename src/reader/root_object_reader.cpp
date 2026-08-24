#include "root4duckdb/reader/root_object_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace duckdb::rootlake {

RootObjectContext::RootObjectContext() = default;

RootObjectReader::RootObjectReader() = default;
RootObjectReader::RootObjectReader(RootObjectReader&&) noexcept = default;
RootObjectReader& RootObjectReader::operator=(RootObjectReader&&) noexcept = default;

void* RootObjectContext::CurrentObject() const {
    return address_slot;
}

TFile* RootObjectReader::File() const {
    return file;
}

TTree* RootObjectReader::Tree() const {
    return context.tree;
}

TBranch* RootObjectReader::ObjectBranch() const {
    return context.branch;
}

TClass* RootObjectReader::RootClass() const {
    return context.root_class;
}

void* RootObjectReader::CurrentObject() const {
    return context.CurrentObject();
}

uint64_t RootEntryReader::LoadCount() const {
    return load_count;
}

RootDictionaryCleanupMode ParseDictionaryCleanupMode(std::string mode, RootDictionaryCleanupMode automatic_mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (mode.empty() || mode == "auto") {
        return automatic_mode;
    }
    if (mode == "retain" || mode == "none" || mode == "skip") {
        return RootDictionaryCleanupMode::RETAIN;
    }
    if (mode == "destruct_only" || mode == "dtor_only") {
        return RootDictionaryCleanupMode::DESTRUCT_ONLY;
    }
    if (mode == "full" || mode == "strict" || mode == "delete") {
        return RootDictionaryCleanupMode::FULL;
    }
    throw InvalidInputException("dictionary_cleanup must be one of: auto, retain, destruct_only, full");
}

void RootObjectReader::Bind(TFile* file_p, const std::string& tree_name, const std::string& root_class_name,
                            RootDictionaryCleanupMode cleanup_mode) {
    Reset();
    if (!file_p || file_p->IsZombie()) {
        throw InvalidInputException("Cannot bind an invalid ROOT file");
    }
    auto* root_class = TClass::GetClass(root_class_name.c_str());
    if (!root_class || !root_class->HasDictionary()) {
        throw InvalidInputException("ROOT dictionary is unavailable for class " + root_class_name);
    }
    auto* tree = FindTree(file_p, tree_name, root_class_name);
    if (!tree) {
        throw InvalidInputException("No TTree found for ROOT class " + root_class_name);
    }
    auto* branch = FindObjectBranch(tree, root_class_name);
    if (!branch) {
        throw InvalidInputException("No object branch for ROOT class " + root_class_name);
    }
    context.Bind(tree, branch, root_class, cleanup_mode, root_class_name);
    file = file_p;
}

void RootObjectReader::Reset() {
    context.Reset();
    file = nullptr;
}

void* RootObjectReader::Read(uint64_t entry) {
    return context.Read(entry);
}

bool RootObjectReader::IsBound() const {
    return file && context.tree && context.branch && context.root_class;
}

RootEntryReader::RootEntryReader(RootObjectReader& reader_p) : reader(reader_p) {
}

void RootEntryReader::Begin(uint64_t entry_p) {
    entry = entry_p;
    object = nullptr;
    loaded_source = nullptr;
    loaded = false;
}

void* RootEntryReader::Read() {
    return ReadFrom(reader);
}

void* RootEntryReader::ReadFrom(RootObjectReader& source) {
    if (!loaded || loaded_source != &source) {
        object = source.Read(entry);
        loaded_source = &source;
        loaded = true;
        ++load_count;
    }
    return object;
}

void RootEntryReader::Invalidate() {
    object = nullptr;
    loaded_source = nullptr;
    loaded = false;
}

RootObjectContext::RootObjectContext(RootObjectContext&& other) noexcept {
    MoveFrom(std::move(other));
}

RootObjectContext& RootObjectContext::operator=(RootObjectContext&& other) noexcept {
    if (this != &other) {
        Reset();
        MoveFrom(std::move(other));
    }
    return *this;
}

RootObjectContext::~RootObjectContext() {
    Reset();
}

void RootObjectContext::Bind(TTree* tree_p, TBranch* branch_p, TClass* root_class_p,
                             RootDictionaryCleanupMode cleanup_mode_p, std::string class_name_p) {
    Reset();
    tree = tree_p;
    branch = branch_p;
    root_class = root_class_p;
    cleanup_mode = cleanup_mode_p;
    class_name =
        class_name_p.empty() && root_class && root_class->GetName() ? root_class->GetName() : std::move(class_name_p);
    if (!tree || !branch || !root_class) {
        throw InvalidInputException("Cannot bind null ROOT tree/branch/class");
    }
    // Binding is intentionally metadata-only.  Serialized scans must not pay
    // for a top-level C++ object unless validation or fallback actually asks
    // for RootObjectReader::Read().
    RootDebug("OBJECT.LAZY_BOUND", "class=" + class_name);
}

void* RootObjectContext::Read(uint64_t entry) {
    if (!tree || !branch || !root_class) {
        return nullptr;
    }
    if (!address_slot) {
        RootDebug("OBJECT.BEFORE_NEW", "class=" + class_name);
        owned_object = root_class->New();
        if (!owned_object) {
            throw IOException("TClass::New failed for " + class_name);
        }
        address_slot = owned_object;
        branch->SetAutoDelete(kFALSE);
        branch->SetAddress(&address_slot);
        RootDebug("OBJECT.BOUND", "class=" + class_name);
    }
    const auto bytes = tree->GetEntry(static_cast<Long64_t>(entry));
    return bytes < 0 ? nullptr : address_slot;
}

void RootObjectContext::MoveFrom(RootObjectContext&& other) {
    tree = other.tree;
    branch = other.branch;
    root_class = other.root_class;
    owned_object = other.owned_object;
    address_slot = other.address_slot;
    cleanup_mode = other.cleanup_mode;
    class_name = std::move(other.class_name);
    other.tree = nullptr;
    other.branch = nullptr;
    other.root_class = nullptr;
    other.owned_object = nullptr;
    other.address_slot = nullptr;
    other.cleanup_mode = RootDictionaryCleanupMode::FULL;
    if (branch) {
        branch->SetAddress(&address_slot);
    }
}

void RootObjectContext::Reset() {
    if (branch) {
        branch->ResetAddress();
    }
    if (owned_object && root_class) {
        switch (cleanup_mode) {
        case RootDictionaryCleanupMode::FULL:
            root_class->Destructor(owned_object, kFALSE);
            break;
        case RootDictionaryCleanupMode::DESTRUCT_ONLY:
            root_class->Destructor(owned_object, kTRUE);
            break;
        case RootDictionaryCleanupMode::RETAIN:
            break;
        }
    }
    tree = nullptr;
    branch = nullptr;
    root_class = nullptr;
    owned_object = nullptr;
    address_slot = nullptr;
    cleanup_mode = RootDictionaryCleanupMode::FULL;
    class_name.clear();
}

} // namespace duckdb::rootlake
