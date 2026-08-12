#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

class TBranch;
class TClass;
class TFile;
class TStreamerElement;
class TTree;

namespace duckdb::rootlake {

struct ParsedPath {
    std::string root_class;
    std::vector<std::string> fields;
};

std::string NormalizePath(std::string path);
ParsedPath ParsePath(const std::string &raw_path);
ParsedPath ParsePathPrefix(const std::string &raw_path);

std::string StripStd(std::string type);
std::string TrimType(std::string type);
std::string TemplatePrimaryName(const std::string &raw_type);
std::vector<std::string> TemplateArguments(const std::string &raw_type);
bool IsAssociativeContainerType(const std::string &raw_type);
bool IsContiguousVectorType(const std::string &raw_type);
bool ContiguousVectorPathEnabled();
bool IsPointerType(std::string raw_type);

struct FixedArrayTypeInfo {
    std::string base_type;
    std::vector<uint32_t> dimensions;
    uint64_t length = 1;
};

FixedArrayTypeInfo ParseFixedArrayType(const std::string &raw_type);
std::string PrimitiveBaseType(const std::string &raw_type);
uint32_t PrimitiveTypeSize(const std::string &raw_type);
std::string ArrayDimensionsText(const std::vector<uint32_t> &dimensions);
bool IsPrimitiveType(const std::string &raw_type);
bool IsStringType(const std::string &raw_type);
std::string ExtractInnerType(const std::string &container_type);
LogicalType RootTypeToLogicalType(const std::string &raw_type);
LogicalType RootTypeToScanLogicalType(const std::string &raw_type,
                                      bool is_string, bool is_primitive);
bool IsLosslessDoubleBackedType(const std::string &raw_type);
double ReadPrimitiveAsDouble(void *ptr, const std::string &raw_type);

struct PathLevel {
    std::string name;
    std::string type;
    int64_t offset_in_parent = -1;
    int64_t cumulative_offset = 0;
    bool is_primitive = false;
    bool is_string = false;
    bool is_pointer = false;
    bool is_container = false;
    bool is_fixed_array = false;
    uint64_t fixed_array_length = 0;
    uint32_t element_size = 0;
    std::vector<uint32_t> array_dimensions;
    TClass *klass = nullptr;
    TClass *element_class = nullptr;
};

class PathResolver {
public:
    static std::vector<PathLevel> Resolve(TClass *root_class,
                                          const std::vector<std::string> &fields);
    static std::vector<PathLevel> TryResolve(TClass *root_class,
                                             const std::vector<std::string> &fields) noexcept;
};

void AppendLevelIndexNames(const PathLevel &level, std::vector<std::string> &names);
std::string IndexSignature(const std::vector<PathLevel> &levels);
idx_t IndexDepth(const std::vector<PathLevel> &levels);

struct SemanticPathSelection {
    std::string bind_prefix;
    std::vector<std::string> primitive_paths;
    std::set<std::string> child_paths;
};

bool SelectSemanticPath(TClass *root_class, const ParsedPath &path,
                        const std::string &raw_path,
                        SemanticPathSelection &selection);

struct ReadResult {
    std::vector<std::string> strings;
    std::vector<double> numbers;
    std::vector<bool> is_string_flag;
    std::vector<int64_t> event_ids;
    std::vector<std::vector<int>> vector_indices;
    std::vector<std::string> vector_names;
    std::string source_path;

    size_t size() const { return strings.size(); }
    bool empty() const { return strings.empty(); }
    void Clear();
    void AddString(const std::string &value, int64_t event_id,
                   const std::vector<int32_t> &indices,
                   const std::vector<std::string> &index_names);
    void AddNumber(double value, int64_t event_id,
                   const std::vector<int32_t> &indices,
                   const std::vector<std::string> &index_names);
};

class OffsetValueReader {
public:
    static void CollectValues(void *root_object, const std::vector<PathLevel> &levels,
                              std::vector<double> &out);
    static void CollectFlat(void *root_object, const std::vector<PathLevel> &levels,
                            idx_t index_depth, std::vector<double> &values,
                            std::vector<int32_t> &flat_indices);
    static void CollectDirect(void *root_object, const std::vector<PathLevel> &levels,
                              int64_t max_values, int64_t event_id,
                              ReadResult &out);
};

enum class RootDictionaryCleanupMode : uint8_t {
    FULL = 0,
    DESTRUCT_ONLY = 1,
    RETAIN = 2
};

RootDictionaryCleanupMode ParseDictionaryCleanupMode(
    std::string mode,
    RootDictionaryCleanupMode automatic_mode = RootDictionaryCleanupMode::FULL);

class RootObjectContext {
public:
    RootObjectContext() = default;
    RootObjectContext(const RootObjectContext &) = delete;
    RootObjectContext &operator=(const RootObjectContext &) = delete;
    RootObjectContext(RootObjectContext &&other) noexcept;
    RootObjectContext &operator=(RootObjectContext &&other) noexcept;
    ~RootObjectContext();

    void Bind(TTree *tree, TBranch *branch, TClass *root_class,
              RootDictionaryCleanupMode cleanup_mode = RootDictionaryCleanupMode::FULL,
              std::string class_name = {});
    void *Read(uint64_t entry);
    void *CurrentObject() const { return address_slot; }
    void Reset();

    TTree *tree = nullptr;
    TBranch *branch = nullptr;
    TClass *root_class = nullptr;
    void *owned_object = nullptr;
    void *address_slot = nullptr;
    RootDictionaryCleanupMode cleanup_mode = RootDictionaryCleanupMode::FULL;

private:
    void MoveFrom(RootObjectContext &&other);

    std::string class_name;
};

class RootObjectReader {
public:
    RootObjectReader() = default;
    RootObjectReader(const RootObjectReader &) = delete;
    RootObjectReader &operator=(const RootObjectReader &) = delete;
    RootObjectReader(RootObjectReader &&) noexcept = default;
    RootObjectReader &operator=(RootObjectReader &&) noexcept = default;

    void Bind(TFile *file, const std::string &tree_name,
              const std::string &root_class_name,
              RootDictionaryCleanupMode cleanup_mode =
                  RootDictionaryCleanupMode::FULL);
    void Reset();
    void *Read(uint64_t entry);

    bool IsBound() const;
    TFile *File() const { return file; }
    TTree *Tree() const { return context.tree; }
    TBranch *ObjectBranch() const { return context.branch; }
    TClass *RootClass() const { return context.root_class; }
    void *CurrentObject() const { return context.CurrentObject(); }

private:
    TFile *file = nullptr;
    RootObjectContext context;
};

class RootEntryReader {
public:
    explicit RootEntryReader(RootObjectReader &reader);

    void Begin(uint64_t entry);
    void *Read();
    void Invalidate();
    uint64_t LoadCount() const { return load_count; }

private:
    RootObjectReader &reader;
    uint64_t entry = 0;
    void *object = nullptr;
    bool loaded = false;
    uint64_t load_count = 0;
};

TTree *FindTree(TFile *file, const std::string &tree_name,
                const std::string &root_class);
TBranch *FindObjectBranch(TTree *tree, const std::string &root_class);
void CollectBranchTree(TBranch *branch, std::vector<TBranch *> &out);
bool BranchNameEndsWithToken(const std::string &name, const std::string &token);
bool ContainsTokensInOrder(const std::string &name,
                           const std::vector<std::string> &tokens);
TBranch *FindPhysicalBranch(TBranch *object_branch,
                            const std::vector<std::string> &fields);
bool HasPersistentBaskets(TBranch *branch);

struct PhysicalBranchResolution {
    TBranch *branch = nullptr;
    std::string mode;
};

PhysicalBranchResolution ResolvePhysicalBranch(
    TBranch *object_branch, const std::vector<std::string> &fields);
std::string SchemaFingerprint(const std::string &root_class,
                              const std::vector<PathLevel> &levels);

} // namespace duckdb::rootlake
