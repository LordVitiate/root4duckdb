#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

class TBranch;
class TClass;

namespace duckdb::rootlake {

/// Parsed logical path rooted at a persistent ROOT class.
struct ParsedPath {
    std::string root_class;
    std::vector<std::string> fields;
};

/// Normalizes a logical ROOT path to its canonical slash form.
std::string NormalizePath(std::string path);
/// Parses a complete logical path that terminates at a value.
ParsedPath ParsePath(const std::string& raw_path);
/// Parses a logical path that may terminate at an object or container.
ParsedPath ParsePathPrefix(const std::string& raw_path);

std::string StripStd(std::string type);
std::string TrimType(std::string type);
std::string TemplatePrimaryName(const std::string& raw_type);
std::vector<std::string> TemplateArguments(const std::string& raw_type);
bool IsAssociativeContainerType(const std::string& raw_type);
bool IsContiguousVectorType(const std::string& raw_type);
bool ContiguousVectorPathEnabled();
bool IsPointerType(std::string raw_type);

/// Shape and primitive type of a fixed ROOT array.
struct FixedArrayTypeInfo {
    std::string base_type;
    std::vector<uint32_t> dimensions;
    uint64_t length = 1;
};

FixedArrayTypeInfo ParseFixedArrayType(const std::string& raw_type);
std::string PrimitiveBaseType(const std::string& raw_type);
uint32_t PrimitiveTypeSize(const std::string& raw_type);
std::string ArrayDimensionsText(const std::vector<uint32_t>& dimensions);
bool IsPrimitiveType(const std::string& raw_type);
bool IsStringType(const std::string& raw_type);
std::string ExtractInnerType(const std::string& container_type);
LogicalType RootTypeToLogicalType(const std::string& raw_type);
LogicalType RootTypeToScanLogicalType(const std::string& raw_type, bool is_string, bool is_primitive);
bool IsLosslessDoubleBackedType(const std::string& raw_type);
double ReadPrimitiveAsDouble(void* pointer, const std::string& raw_type);

enum class RootPrimitiveKind : uint8_t { SIGNED, UNSIGNED, FLOATING };

/// Exact primitive value used when a double would lose 64-bit integers.
struct RootPrimitiveValue {
    RootPrimitiveKind kind = RootPrimitiveKind::FLOATING;
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    double floating_value = 0.0;

    static RootPrimitiveValue Signed(int64_t value);
    static RootPrimitiveValue Unsigned(uint64_t value);
    static RootPrimitiveValue Floating(double value);
    static RootPrimitiveValue FromPointer(void* pointer, const std::string& raw_type);
    static RootPrimitiveValue FromDouble(double value, const std::string& raw_type);

    int64_t AsSigned() const;
    uint64_t AsUnsigned() const;
    double AsDouble() const;
    bool AsBool() const;
};

/// One resolved semantic step through a ROOT object or container.
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
    TClass* klass = nullptr;
    TClass* element_class = nullptr;
};

/// Primitive children selected when browsing a non-leaf semantic path.
struct SemanticPathSelection {
    std::string bind_prefix;
    std::vector<std::string> primitive_paths;
    std::set<std::string> child_paths;
};

/// Flattened values and indices produced by direct semantic reads.
struct ReadResult {
    std::vector<std::string> strings;
    std::vector<RootPrimitiveValue> numbers;
    std::vector<bool> is_string_flag;
    std::vector<int64_t> event_ids;
    std::vector<std::vector<int>> vector_indices;
    std::vector<std::string> vector_names;
    std::string source_path;

    size_t size() const;
    bool empty() const;
    void Clear();
    void AddString(const std::string& value, int64_t event_id, const std::vector<int32_t>& indices,
                   const std::vector<std::string>& index_names);
    void AddNumber(const RootPrimitiveValue& value, int64_t event_id, const std::vector<int32_t>& indices,
                   const std::vector<std::string>& index_names);
    void AddNumber(double value, int64_t event_id, const std::vector<int32_t>& indices,
                   const std::vector<std::string>& index_names);
};

/// Physical branch selected for basket metadata and serialized decoding.
struct PhysicalBranchResolution {
    TBranch* branch = nullptr;
    std::string mode;
};

} // namespace duckdb::rootlake
