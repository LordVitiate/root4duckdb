#include "root_semantic_reader.hpp"

#include "root_debug.hpp"
#include "root_headers.hpp"
#include "root_lake_common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace duckdb::rootlake {
namespace {

struct StreamerFieldMatch {
    TStreamerElement *element = nullptr;
    int64_t offset = 0;
};

std::optional<StreamerFieldMatch> FindStreamerField(
    TClass *klass, const std::string &field, std::vector<std::string> &visited) {
    if (!klass) return std::nullopt;
    const std::string class_name = klass->GetName();
    if (std::find(visited.begin(), visited.end(), class_name) != visited.end()) {
        return std::nullopt;
    }
    visited.push_back(class_name);

    RootDebug("PATH.FIND_FIELD", "class=" + class_name + " field=" + field);
    auto *streamer = klass->GetStreamerInfo();
    auto *elements = streamer ? streamer->GetElements() : nullptr;
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element || element->IsBase() || field != element->GetName()) continue;
        StreamerFieldMatch result;
        result.element = element;
        result.offset = streamer->GetElementOffset(i);
        visited.pop_back();
        return result;
    }
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto *base = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!base || !base->IsBase()) continue;
        auto *base_class = base->GetClassPointer();
        if (!base_class) base_class = TClass::GetClass(base->GetTypeName());
        auto nested = FindStreamerField(base_class, field, visited);
        if (!nested) continue;
        nested->offset += streamer->GetElementOffset(i);
        visited.pop_back();
        return nested;
    }
    visited.pop_back();
    return std::nullopt;
}

void CollectImmediateChildren(TClass *klass, const std::string &prefix,
                              SemanticPathSelection &selection,
                              std::set<std::string> &active_bases) {
    if (!klass) return;
    const std::string class_name = klass->GetName();
    if (!active_bases.insert(class_name).second) return;

    auto *streamer = klass->GetStreamerInfo();
    auto *elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements) {
        active_bases.erase(class_name);
        return;
    }
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto *base = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!base || !base->IsBase()) continue;
        auto *base_class = base->GetClassPointer();
        if (!base_class) base_class = TClass::GetClass(base->GetTypeName());
        CollectImmediateChildren(base_class, prefix, selection, active_bases);
    }
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element || element->IsBase()) continue;
        const std::string full_path = prefix + element->GetName();
        const std::string type = element->GetTypeName();
        if (IsPrimitiveType(type) || IsStringType(type)) {
            selection.primitive_paths.push_back(full_path);
        } else if (element->GetClassPointer() || element->IsaPointer()) {
            selection.child_paths.insert(full_path + "/");
        }
    }
    active_bases.erase(class_name);
}


std::string ReadRootString(void *pointer, const std::string &raw_type) {
    if (!pointer) return {};
    try {
        if (TrimType(raw_type) == "TString") {
            auto *value = static_cast<TString *>(pointer);
            return std::string(value->Data(), value->Length());
        }
        return *reinterpret_cast<std::string *>(pointer);
    } catch (...) {
        return TrimType(raw_type) == "TString" ? "<tstring_error>" : "<string_error>";
    }
}

void AppendArrayIndexNames(const PathLevel &level, size_t current_depth,
                           std::vector<std::string> &names) {
    const size_t rank = level.array_dimensions.size() <= 1
                            ? 1
                            : level.array_dimensions.size();
    const size_t target_size = current_depth + rank;
    if (level.array_dimensions.size() <= 1) {
        if (names.size() < target_size) names.push_back(level.name + "_idx");
        return;
    }
    for (size_t dim = 0; dim < level.array_dimensions.size(); ++dim) {
        if (names.size() < target_size) {
            names.push_back(level.name + "_dim" + std::to_string(dim) + "_idx");
        }
    }
}

void PushArrayCoordinates(uint64_t flat_index,
                          const std::vector<uint32_t> &dimensions,
                          std::vector<int32_t> &indices) {
    if (dimensions.empty()) {
        indices.push_back(static_cast<int32_t>(flat_index));
        return;
    }
    std::vector<int32_t> coordinates(dimensions.size(), 0);
    for (size_t reverse = dimensions.size(); reverse > 0; --reverse) {
        const size_t dim = reverse - 1;
        coordinates[dim] = static_cast<int32_t>(flat_index % dimensions[dim]);
        flat_index /= dimensions[dim];
    }
    indices.insert(indices.end(), coordinates.begin(), coordinates.end());
}

struct ContainerAccess {
    char *base = nullptr;
    uint32_t stride = 0;
    bool contiguous = false;
};

ContainerAccess PrepareContainerAccess(const PathLevel &level,
                                       TVirtualCollectionProxy *proxy,
                                       size_t size) {
    ContainerAccess result;
    if (!proxy || !size || !ContiguousVectorPathEnabled() ||
        !IsContiguousVectorType(level.type)) {
        return result;
    }
    const auto inner = TrimType(ExtractInnerType(level.type));
    if (inner.empty() || inner == "bool" || inner == "Bool_t" ||
        IsPointerType(inner)) {
        return result;
    }
    uint32_t stride = PrimitiveTypeSize(inner);
    if (!stride && level.element_class) {
        stride = static_cast<uint32_t>(level.element_class->Size());
    }
    if (!stride) return result;
    auto *first = static_cast<char *>(proxy->At(0));
    if (!first) return result;
    if (size > 1) {
        auto *second = static_cast<char *>(proxy->At(1));
        if (!second || second != first + stride) return result;
    }
    result.base = first;
    result.stride = stride;
    result.contiguous = true;
    return result;
}

struct DirectSink {
    int64_t max_values;
    int64_t event_id;
    ReadResult &result;

    bool Full() const {
        return max_values >= 0 && result.size() >= static_cast<size_t>(max_values);
    }
    bool RequiresContainerDictionary() const { return true; }
    void Number(void *pointer, const std::string &type,
                const std::vector<int32_t> &indices,
                const std::vector<std::string> &names) {
        result.AddNumber(RootPrimitiveValue::FromPointer(pointer, type), event_id, indices, names);
    }
    void String(void *pointer, const std::string &type,
                const std::vector<int32_t> &indices,
                const std::vector<std::string> &names) {
        result.AddString(ReadRootString(pointer, type), event_id, indices, names);
    }
};

struct NumericOnlySink {
    std::vector<double> &result;

    bool Full() const { return false; }
    bool RequiresContainerDictionary() const { return false; }
    void Number(void *pointer, const std::string &type,
                const std::vector<int32_t> &,
                const std::vector<std::string> &) {
        result.push_back(ReadPrimitiveAsDouble(pointer, type));
    }
    void String(void *, const std::string &, const std::vector<int32_t> &,
                const std::vector<std::string> &) {
    }
};

struct FlatNumericSink {
    idx_t index_depth;
    std::vector<double> &values;
    std::vector<int32_t> &indices;

    bool Full() const { return false; }
    bool RequiresContainerDictionary() const { return false; }
    void Number(void *pointer, const std::string &type,
                const std::vector<int32_t> &current_indices,
                const std::vector<std::string> &) {
        if (current_indices.size() != index_depth) {
            throw IOException(
                "ROOT container depth mismatch while executing indexed access plan: expected " +
                std::to_string(index_depth) + ", got " +
                std::to_string(current_indices.size()));
        }
        values.push_back(ReadPrimitiveAsDouble(pointer, type));
        indices.insert(indices.end(), current_indices.begin(), current_indices.end());
    }
    void String(void *, const std::string &, const std::vector<int32_t> &,
                const std::vector<std::string> &) {
    }
};

template <class SINK>
void CollectValuesRecursive(void *current, const std::vector<PathLevel> &levels,
                            size_t level_index, std::vector<int32_t> &indices,
                            std::vector<std::string> &index_names, SINK &sink) {
    if (!current || level_index >= levels.size() || sink.Full()) return;
    const auto &level = levels[level_index];
    auto *field = static_cast<char *>(current) + level.offset_in_parent;
    if (level.is_pointer) {
        field = field ? *reinterpret_cast<char **>(field) : nullptr;
        if (!field) return;
    }
    const bool last = level_index + 1 == levels.size();

    if (level.is_fixed_array) {
        AppendArrayIndexNames(level, indices.size(), index_names);
        const size_t pushed = std::max<size_t>(1, level.array_dimensions.size());
        for (uint64_t i = 0; i < level.fixed_array_length && !sink.Full(); ++i) {
            auto *element = field + i * level.element_size;
            PushArrayCoordinates(i, level.array_dimensions, indices);
            if (last) {
                if (level.is_primitive) sink.Number(element, level.type, indices, index_names);
            } else {
                CollectValuesRecursive(element, levels, level_index + 1,
                                       indices, index_names, sink);
            }
            indices.resize(indices.size() - pushed);
        }
        return;
    }

    if (last && level.is_container) {
        auto *proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
        if (!proxy) return;
        TVirtualCollectionProxy::TPushPop guard(proxy, field);
        const auto inner = ExtractInnerType(level.type);
        const auto size = proxy->Size();
        if (index_names.size() < indices.size() + 1) {
            index_names.push_back(level.name + "_idx");
        }
        const auto access = PrepareContainerAccess(level, proxy, size);
        for (size_t i = 0; i < size && !sink.Full(); ++i) {
            auto *element = access.contiguous
                                ? static_cast<void *>(access.base + i * access.stride)
                                : proxy->At(i);
            if (!element) continue;
            indices.push_back(static_cast<int32_t>(i));
            if (IsPrimitiveType(inner)) sink.Number(element, inner, indices, index_names);
            else if (IsStringType(inner)) sink.String(element, inner, indices, index_names);
            indices.pop_back();
        }
        return;
    }

    if (last) {
        if (level.is_primitive) sink.Number(field, level.type, indices, index_names);
        else if (level.is_string) sink.String(field, level.type, indices, index_names);
        return;
    }

    if (level.is_container) {
        if (sink.RequiresContainerDictionary() &&
            (!level.klass || !level.klass->HasDictionary())) {
            return;
        }
        auto *proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
        if (!proxy) return;
        TVirtualCollectionProxy::TPushPop guard(proxy, field);
        const auto size = proxy->Size();
        if (index_names.size() < indices.size() + 1) {
            index_names.push_back(level.name + "_idx");
        }
        const auto access = PrepareContainerAccess(level, proxy, size);
        for (size_t i = 0; i < size && !sink.Full(); ++i) {
            auto *element = access.contiguous
                                ? static_cast<void *>(access.base + i * access.stride)
                                : proxy->At(i);
            if (!element) continue;
            indices.push_back(static_cast<int32_t>(i));
            CollectValuesRecursive(element, levels, level_index + 1,
                                   indices, index_names, sink);
            indices.pop_back();
        }
        return;
    }

    if (level.klass) {
        CollectValuesRecursive(field, levels, level_index + 1,
                               indices, index_names, sink);
    }
}

template <class SINK>
void RunTraversal(void *root_object, const std::vector<PathLevel> &levels,
                  SINK &sink) {
    std::vector<int32_t> indices;
    std::vector<std::string> index_names;
    CollectValuesRecursive(root_object, levels, 0, indices, index_names, sink);
}

} // namespace

std::string NormalizePath(std::string path) {
    if (path.empty()) throw InvalidInputException("ROOT logical path is empty");
    if (path.front() != '/') path.insert(path.begin(), '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

ParsedPath ParsePathPrefix(const std::string &raw_path) {
    ParsedPath result;
    std::string path = raw_path;
    if (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.rfind("events/", 0) == 0) path.erase(0, 8);
    std::stringstream stream(path);
    std::string component;
    while (std::getline(stream, component, '/')) {
        if (component.empty()) continue;
        if (result.root_class.empty()) result.root_class = component;
        else result.fields.push_back(component);
    }
    return result;
}

ParsedPath ParsePath(const std::string &raw_path) {
    const auto path = NormalizePath(raw_path);
    ParsedPath result;
    std::stringstream stream(path.substr(1));
    std::string component;
    while (std::getline(stream, component, '/')) {
        if (component.empty()) continue;
        if (result.root_class.empty()) result.root_class = component;
        else result.fields.push_back(component);
    }
    if (result.root_class.empty() || result.fields.empty()) {
        throw InvalidInputException(
            "Expected a leaf path such as /PaEvent/vecHit/u, got: " + raw_path);
    }
    return result;
}

std::string StripStd(std::string type) {
    if (type.rfind("std::", 0) == 0) type.erase(0, 5);
    return type;
}

std::string TrimType(std::string type) {
    const auto first = type.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = type.find_last_not_of(" \t\r\n");
    type = type.substr(first, last - first + 1);
    while (type.rfind("const ", 0) == 0) type.erase(0, 6);
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
        type.pop_back();
    }
    return StripStd(type);
}

std::string TemplatePrimaryName(const std::string &raw_type) {
    auto type = TrimType(raw_type);
    const auto open = type.find('<');
    if (open != std::string::npos) type.resize(open);
    return TrimType(type);
}

std::vector<std::string> TemplateArguments(const std::string &raw_type) {
    const auto open = raw_type.find('<');
    if (open == std::string::npos) return {};
    std::vector<std::string> result;
    int depth = 0;
    size_t token_begin = open + 1;
    for (size_t i = open + 1; i < raw_type.size(); ++i) {
        const char character = raw_type[i];
        if (character == '<') ++depth;
        else if (character == '>') {
            if (depth == 0) {
                result.push_back(TrimType(raw_type.substr(token_begin, i - token_begin)));
                return result;
            }
            --depth;
        } else if (character == ',' && depth == 0) {
            result.push_back(TrimType(raw_type.substr(token_begin, i - token_begin)));
            token_begin = i + 1;
        }
    }
    return {};
}

bool IsAssociativeContainerType(const std::string &raw_type) {
    const auto name = TemplatePrimaryName(raw_type);
    return name == "map" || name == "multimap" || name == "unordered_map" ||
           name == "unordered_multimap";
}

bool IsContiguousVectorType(const std::string &raw_type) {
    return TemplatePrimaryName(raw_type) == "vector";
}

bool ContiguousVectorPathEnabled() {
    const char *value = std::getenv("ROOT4DUCKDB_DISABLE_CONTIGUOUS_VECTOR");
    return !(value && *value && std::string(value) != "0");
}

bool IsPointerType(std::string raw_type) {
    raw_type = TrimType(std::move(raw_type));
    return !raw_type.empty() && raw_type.back() == '*';
}

FixedArrayTypeInfo ParseFixedArrayType(const std::string &raw_type) {
    FixedArrayTypeInfo result;
    result.base_type = StripStd(raw_type);
    const auto first_bracket = result.base_type.find('[');
    if (first_bracket == std::string::npos) {
        result.length = 0;
        return result;
    }
    const auto suffix = result.base_type.substr(first_bracket);
    result.base_type.resize(first_bracket);
    result.base_type = TrimType(result.base_type);
    size_t position = 0;
    result.length = 1;
    while (position < suffix.size()) {
        const auto open = suffix.find('[', position);
        if (open == std::string::npos) break;
        const auto close = suffix.find(']', open + 1);
        if (close == std::string::npos) {
            result.dimensions.clear();
            result.length = 0;
            return result;
        }
        const auto token = suffix.substr(open + 1, close - open - 1);
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c));
            })) {
            result.dimensions.clear();
            result.length = 0;
            return result;
        }
        const auto dimension = static_cast<uint32_t>(std::stoul(token));
        if (!dimension || result.length > std::numeric_limits<uint64_t>::max() / dimension) {
            throw InvalidInputException("Invalid or overflowing fixed ROOT array type: " + raw_type);
        }
        result.dimensions.push_back(dimension);
        result.length *= dimension;
        position = close + 1;
    }
    if (result.dimensions.empty()) result.length = 0;
    return result;
}

std::string PrimitiveBaseType(const std::string &raw_type) {
    const auto array = ParseFixedArrayType(raw_type);
    return TrimType(array.length ? array.base_type : raw_type);
}

uint32_t PrimitiveTypeSize(const std::string &raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") return sizeof(bool);
    if (type == "Char_t" || type == "char" || type == "b") return sizeof(int8_t);
    if (type == "UChar_t" || type == "unsigned char" || type == "B") return sizeof(uint8_t);
    if (type == "Short_t" || type == "short" || type == "S") return sizeof(int16_t);
    if (type == "UShort_t" || type == "unsigned short" || type == "s") return sizeof(uint16_t);
    if (type == "Int_t" || type == "int" || type == "I") return sizeof(int32_t);
    if (type == "UInt_t" || type == "unsigned int" || type == "i") return sizeof(uint32_t);
    if (type == "Long_t" || type == "long") return sizeof(long);
    if (type == "ULong_t" || type == "unsigned long") return sizeof(unsigned long);
    if (type == "Long64_t" || type == "long long" || type == "L") return sizeof(int64_t);
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l") return sizeof(uint64_t);
    if (type == "Float_t" || type == "float" || type == "F") return sizeof(float);
    if (type == "Double_t" || type == "double" || type == "D") return sizeof(double);
    return 0;
}

std::string ArrayDimensionsText(const std::vector<uint32_t> &dimensions) {
    std::string result;
    for (idx_t i = 0; i < dimensions.size(); ++i) {
        if (i) result += 'x';
        result += std::to_string(dimensions[i]);
    }
    return result;
}

bool IsPrimitiveType(const std::string &raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    static const std::vector<std::string> primitive_types = {
        "Bool_t", "bool", "Char_t", "char", "UChar_t", "unsigned char",
        "Short_t", "short", "UShort_t", "unsigned short", "Int_t", "int",
        "UInt_t", "unsigned int", "Long_t", "long", "ULong_t", "unsigned long",
        "Long64_t", "long long", "ULong64_t", "unsigned long long",
        "Float_t", "float", "Double_t", "double"};
    return std::find(primitive_types.begin(), primitive_types.end(), type) !=
           primitive_types.end();
}

bool IsStringType(const std::string &raw_type) {
    const auto type = TrimType(raw_type);
    return type == "string" || type == "TString";
}

std::string ExtractInnerType(const std::string &container_type) {
    const auto arguments = TemplateArguments(container_type);
    return arguments.empty() ? std::string() : arguments.front();
}

LogicalType RootTypeToLogicalType(const std::string &raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") return LogicalType::BOOLEAN;
    if (type == "Char_t" || type == "char" || type == "b") return LogicalType::TINYINT;
    if (type == "UChar_t" || type == "unsigned char" || type == "B") return LogicalType::UTINYINT;
    if (type == "Short_t" || type == "short" || type == "S") return LogicalType::SMALLINT;
    if (type == "UShort_t" || type == "unsigned short" || type == "s") return LogicalType::USMALLINT;
    if (type == "Int_t" || type == "int" || type == "I") return LogicalType::INTEGER;
    if (type == "UInt_t" || type == "unsigned int" || type == "i") return LogicalType::UINTEGER;
    if (type == "Long_t" || type == "long" || type == "Long64_t" ||
        type == "long long" || type == "L") return LogicalType::BIGINT;
    if (type == "ULong_t" || type == "unsigned long" || type == "ULong64_t" ||
        type == "unsigned long long" || type == "l") return LogicalType::UBIGINT;
    if (type == "Float_t" || type == "float" || type == "F") return LogicalType::FLOAT;
    if (type == "Double_t" || type == "double" || type == "D") return LogicalType::DOUBLE;
    if (IsStringType(type)) return LogicalType::VARCHAR;
    throw NotImplementedException("Unsupported ROOT leaf type: " + raw_type);
}

LogicalType RootTypeToScanLogicalType(const std::string &raw_type,
                                      bool is_string, bool is_primitive) {
    if (is_string || !is_primitive) return LogicalType::VARCHAR;

    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") return LogicalType::BOOLEAN;
    if (type == "Char_t" || type == "char" || type == "b") return LogicalType::TINYINT;
    if (type == "UChar_t" || type == "unsigned char" || type == "B") return LogicalType::UTINYINT;
    if (type == "Short_t" || type == "short" || type == "S") return LogicalType::SMALLINT;
    if (type == "UShort_t" || type == "unsigned short" || type == "s") return LogicalType::USMALLINT;
    if (type == "Int_t" || type == "int" || type == "I") return LogicalType::INTEGER;
    if (type == "UInt_t" || type == "unsigned int" || type == "i") return LogicalType::UINTEGER;
    if (type == "Long_t" || type == "long" || type == "Long64_t" || type == "long long" || type == "L") return LogicalType::BIGINT;
    if (type == "ULong_t" || type == "unsigned long" ||
        type == "ULong64_t" || type == "unsigned long long" ||
        type == "l") return LogicalType::UBIGINT;
    if (type == "Float_t" || type == "float" || type == "F") return LogicalType::FLOAT;
    if (type == "Double_t" || type == "double" || type == "D") return LogicalType::DOUBLE;
    return LogicalType::VARCHAR;
}

bool IsLosslessDoubleBackedType(const std::string &raw_type) {
    switch (RootTypeToLogicalType(raw_type).id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        return true;
    default:
        return false;
    }
}

double ReadPrimitiveAsDouble(void *pointer, const std::string &raw_type) {
    return RootPrimitiveValue::FromPointer(pointer, raw_type).AsDouble();
}

namespace {

std::vector<PathLevel> ResolvePath(TClass *root_class,
                                   const std::vector<std::string> &fields,
                                   bool strict_layout) {
    if (!root_class) throw InvalidInputException("ROOT dictionary class is null");
    auto *current_class = root_class;
    if (strict_layout && !current_class->GetStreamerInfo()) {
        throw InvalidInputException("No TStreamerInfo for class " +
                                    std::string(root_class->GetName()));
    }

    std::vector<PathLevel> levels;
    int64_t cumulative = 0;
    for (idx_t field_index = 0; field_index < fields.size(); ++field_index) {
        const auto &field = fields[field_index];
        if (field == "value" && !levels.empty() && levels.back().is_container &&
            !IsAssociativeContainerType(levels.back().type)) {
            PathLevel value_level;
            value_level.name = "value";
            value_level.type = levels.back().element_class
                                   ? levels.back().element_class->GetName()
                                   : ExtractInnerType(levels.back().type);
            value_level.offset_in_parent = 0;
            value_level.cumulative_offset = levels.back().cumulative_offset;
            value_level.klass = levels.back().element_class;
            if (!value_level.klass && !value_level.type.empty()) {
                value_level.klass = TClass::GetClass(value_level.type.c_str());
            }
            value_level.is_primitive = IsPrimitiveType(value_level.type);
            value_level.is_string = IsStringType(value_level.type);
            value_level.is_container =
                value_level.klass && value_level.klass->GetCollectionProxy();
            if (value_level.is_container) {
                value_level.element_class =
                    value_level.klass->GetCollectionProxy()->GetValueClass();
            }
            value_level.element_size = value_level.is_primitive
                                           ? PrimitiveTypeSize(value_level.type)
                                           : (value_level.klass
                                                  ? static_cast<uint32_t>(value_level.klass->Size())
                                                  : 0);
            const bool terminal = field_index + 1 == fields.size();
            if (terminal && !value_level.is_primitive && !value_level.is_string) {
                throw InvalidInputException(
                    "Container /value does not terminate in a primitive/string: " +
                    value_level.type);
            }
            if (!terminal && !value_level.klass) {
                throw InvalidInputException(
                    "Cannot descend through container value type: " + value_level.type);
            }
            levels.push_back(value_level);
            cumulative = value_level.is_container ? 0 : value_level.cumulative_offset;
            current_class = value_level.is_container
                                ? value_level.element_class
                                : value_level.klass;
            continue;
        }

        if (!current_class) {
            throw InvalidInputException(
                "Cannot descend through primitive field before '" + field + "'");
        }
        std::string streamer_field = field;
        if (!levels.empty() && levels.back().is_container &&
            IsAssociativeContainerType(levels.back().type)) {
            if (field == "key") streamer_field = "first";
            else if (field == "value") streamer_field = "second";
        }
        std::vector<std::string> visited;
        auto match = FindStreamerField(current_class, streamer_field, visited);
        if (!match || !match->element) {
            throw InvalidInputException("Field '" + field +
                                        "' is absent in ROOT streamer path");
        }

        auto *element = match->element;
        PathLevel level;
        level.name = field;
        level.type = element->GetTypeName();
        level.offset_in_parent = match->offset;
        level.cumulative_offset = cumulative + level.offset_in_parent;
        level.is_primitive = IsPrimitiveType(level.type);
        level.is_string = IsStringType(level.type);
        level.is_pointer = element->IsaPointer();
        level.klass = element->GetClassPointer();
        level.is_container = level.klass && level.klass->GetCollectionProxy();
        if (level.is_container) {
            level.element_class = level.klass->GetCollectionProxy()->GetValueClass();
        }

        const int rank = element->GetArrayDim();
        uint64_t array_length = 1;
        for (int dim = 0; dim < rank; ++dim) {
            const int extent = element->GetMaxIndex(dim);
            if (extent <= 0 ||
                (strict_layout &&
                 array_length > std::numeric_limits<uint64_t>::max() /
                                    static_cast<uint64_t>(extent))) {
                array_length = 0;
                level.array_dimensions.clear();
                break;
            }
            level.array_dimensions.push_back(static_cast<uint32_t>(extent));
            array_length *= static_cast<uint64_t>(extent);
        }
        if (strict_layout &&
            (!array_length || level.array_dimensions.empty())) {
            const auto parsed_array = ParseFixedArrayType(level.type);
            if (parsed_array.length) {
                level.array_dimensions = parsed_array.dimensions;
                array_length = parsed_array.length;
                level.type = parsed_array.base_type;
                level.is_primitive = IsPrimitiveType(level.type);
            }
        }
        level.is_fixed_array = !level.array_dimensions.empty() && array_length > 0;
        level.fixed_array_length = level.is_fixed_array ? array_length : 0;
        level.element_size = level.is_primitive
                                 ? PrimitiveTypeSize(level.type)
                                 : (level.klass
                                        ? static_cast<uint32_t>(level.klass->Size())
                                        : 0);
        if (level.is_fixed_array && !level.element_size) {
            throw NotImplementedException(
                "Cannot determine element size for fixed ROOT array " + field);
        }
        levels.push_back(level);

        if (level.is_pointer) {
            cumulative = 0;
            current_class = level.klass;
        } else if (level.is_container) {
            cumulative = 0;
            current_class = level.element_class;
        } else if (level.is_fixed_array && level.klass) {
            cumulative = 0;
            current_class = level.klass;
        } else if (level.klass) {
            cumulative = level.cumulative_offset;
            current_class = level.klass;
        } else {
            current_class = nullptr;
        }
        if (strict_layout && current_class &&
            !current_class->GetStreamerInfo()) {
            throw InvalidInputException(
                "No TStreamerInfo for nested class " +
                std::string(current_class->GetName()));
        }
    }
    if (levels.empty()) throw InvalidInputException("Resolved ROOT path is empty");
    return levels;
}

} // namespace

std::vector<PathLevel> PathResolver::Resolve(
    TClass *root_class, const std::vector<std::string> &fields) {
    auto levels = ResolvePath(root_class, fields, true);
    const auto &leaf = levels.back();
    if (!leaf.is_primitive && !leaf.is_string) {
        throw InvalidInputException(
            "ROOT path does not end in a primitive/string value");
    }
    return levels;
}

std::vector<PathLevel> PathResolver::TryResolve(
    TClass *root_class, const std::vector<std::string> &fields) noexcept {
    try {
        return ResolvePath(root_class, fields, false);
    } catch (...) {
        return {};
    }
}

void AppendLevelIndexNames(const PathLevel &level,
                           std::vector<std::string> &names) {
    if (level.is_container) names.push_back(level.name + "_idx");
    if (!level.is_fixed_array) return;
    if (level.array_dimensions.size() <= 1) {
        names.push_back(level.name + "_idx");
        return;
    }
    for (idx_t dim = 0; dim < level.array_dimensions.size(); ++dim) {
        names.push_back(level.name + "_dim" + std::to_string(dim) + "_idx");
    }
}

std::string IndexSignature(const std::vector<PathLevel> &levels) {
    std::vector<std::string> names;
    for (const auto &level : levels) AppendLevelIndexNames(level, names);
    return JoinStrings(names, ",");
}

idx_t IndexDepth(const std::vector<PathLevel> &levels) {
    idx_t result = 0;
    for (const auto &level : levels) {
        if (level.is_container) ++result;
        if (level.is_fixed_array) {
            result += std::max<idx_t>(1, level.array_dimensions.size());
        }
    }
    return result;
}

bool SelectSemanticPath(TClass *root_class, const ParsedPath &path,
                        const std::string &raw_path,
                        SemanticPathSelection &selection) {
    selection = {};
    if (!root_class) return false;
    std::string canonical = raw_path;
    while (canonical.size() > 1 && canonical.back() == '/') canonical.pop_back();

    if (path.fields.empty()) {
        selection.bind_prefix = canonical + "/";
        std::set<std::string> active_bases;
        CollectImmediateChildren(root_class, selection.bind_prefix,
                                 selection, active_bases);
        return !selection.primitive_paths.empty() || !selection.child_paths.empty();
    }

    const auto levels = PathResolver::TryResolve(root_class, path.fields);
    if (levels.empty()) return false;
    const auto &terminal = levels.back();
    if (terminal.is_primitive || terminal.is_string) {
        selection.bind_prefix = canonical;
        selection.primitive_paths.push_back(canonical);
        return true;
    }

    TClass *target_class = nullptr;
    if (terminal.is_container) {
        if (IsAssociativeContainerType(terminal.type)) {
            selection.bind_prefix = canonical + "/";
            selection.primitive_paths.push_back(canonical + "/key");
            selection.primitive_paths.push_back(canonical + "/value");
            return true;
        }
        const auto inner = terminal.element_class
                               ? std::string(terminal.element_class->GetName())
                               : ExtractInnerType(terminal.type);
        if (IsPrimitiveType(inner) || IsStringType(inner)) {
            selection.bind_prefix = canonical + "/";
            selection.primitive_paths.push_back(canonical + "/value");
            return true;
        }
        target_class = terminal.element_class;
    } else {
        target_class = terminal.klass;
    }
    if (!target_class) return false;

    selection.bind_prefix = canonical + "/";
    std::set<std::string> active_bases;
    CollectImmediateChildren(target_class, selection.bind_prefix,
                             selection, active_bases);
    return !selection.primitive_paths.empty() || !selection.child_paths.empty();
}

void ReadResult::Clear() {
    strings.clear();
    numbers.clear();
    is_string_flag.clear();
    event_ids.clear();
    vector_indices.clear();
    vector_names.clear();
    source_path.clear();
}

void ReadResult::AddString(const std::string &value, int64_t event_id,
                           const std::vector<int32_t> &indices,
                           const std::vector<std::string> &index_names) {
    strings.push_back(value);
    numbers.emplace_back();
    is_string_flag.push_back(true);
    event_ids.push_back(event_id);
    vector_indices.emplace_back(indices.begin(), indices.end());
    if (vector_names.empty()) vector_names = index_names;
}

void ReadResult::AddNumber(const RootPrimitiveValue &value, int64_t event_id,
                           const std::vector<int32_t> &indices,
                           const std::vector<std::string> &index_names) {
    strings.emplace_back();
    numbers.push_back(value);
    is_string_flag.push_back(false);
    event_ids.push_back(event_id);
    vector_indices.emplace_back(indices.begin(), indices.end());
    if (vector_names.empty()) vector_names = index_names;
}

void ReadResult::AddNumber(double value, int64_t event_id,
                           const std::vector<int32_t> &indices,
                           const std::vector<std::string> &index_names) {
    AddNumber(RootPrimitiveValue::Floating(value),
              event_id, indices, index_names);
}

void OffsetValueReader::CollectValues(void *root_object,
                                      const std::vector<PathLevel> &levels,
                                      std::vector<double> &out) {
    NumericOnlySink sink {out};
    RunTraversal(root_object, levels, sink);
}

void OffsetValueReader::CollectFlat(void *root_object,
                                    const std::vector<PathLevel> &levels,
                                    idx_t index_depth,
                                    std::vector<double> &values,
                                    std::vector<int32_t> &flat_indices) {
    FlatNumericSink sink {index_depth, values, flat_indices};
    RunTraversal(root_object, levels, sink);
}

void OffsetValueReader::CollectDirect(void *root_object,
                                      const std::vector<PathLevel> &levels,
                                      int64_t max_values, int64_t event_id,
                                      ReadResult &out) {
    DirectSink sink {max_values, event_id, out};
    RunTraversal(root_object, levels, sink);
}

RootDictionaryCleanupMode ParseDictionaryCleanupMode(
    std::string mode, RootDictionaryCleanupMode automatic_mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (mode.empty() || mode == "auto") return automatic_mode;
    if (mode == "retain" || mode == "none" || mode == "skip") {
        return RootDictionaryCleanupMode::RETAIN;
    }
    if (mode == "destruct_only" || mode == "dtor_only") {
        return RootDictionaryCleanupMode::DESTRUCT_ONLY;
    }
    if (mode == "full" || mode == "strict" || mode == "delete") {
        return RootDictionaryCleanupMode::FULL;
    }
    throw InvalidInputException(
        "dictionary_cleanup must be one of: auto, retain, destruct_only, full");
}

void RootObjectReader::Bind(TFile *file_p, const std::string &tree_name,
                            const std::string &root_class_name,
                            RootDictionaryCleanupMode cleanup_mode) {
    Reset();
    if (!file_p || file_p->IsZombie()) {
        throw InvalidInputException("Cannot bind an invalid ROOT file");
    }
    auto *root_class = TClass::GetClass(root_class_name.c_str());
    if (!root_class || !root_class->HasDictionary()) {
        throw InvalidInputException(
            "ROOT dictionary is unavailable for class " + root_class_name);
    }
    auto *tree = FindTree(file_p, tree_name, root_class_name);
    if (!tree) {
        throw InvalidInputException(
            "No TTree found for ROOT class " + root_class_name);
    }
    auto *branch = FindObjectBranch(tree, root_class_name);
    if (!branch) {
        throw InvalidInputException(
            "No object branch for ROOT class " + root_class_name);
    }
    context.Bind(tree, branch, root_class, cleanup_mode, root_class_name);
    file = file_p;
}

void RootObjectReader::Reset() {
    context.Reset();
    file = nullptr;
}

void *RootObjectReader::Read(uint64_t entry) {
    return context.Read(entry);
}

bool RootObjectReader::IsBound() const {
    return file && context.tree && context.branch && context.root_class;
}

RootEntryReader::RootEntryReader(RootObjectReader &reader_p)
    : reader(reader_p) {
}

void RootEntryReader::Begin(uint64_t entry_p) {
    entry = entry_p;
    object = nullptr;
    loaded = false;
}

void *RootEntryReader::Read() {
    if (!loaded) {
        object = reader.Read(entry);
        loaded = true;
        ++load_count;
    }
    return object;
}

void RootEntryReader::Invalidate() {
    object = nullptr;
    loaded = false;
}

RootObjectContext::RootObjectContext(RootObjectContext &&other) noexcept {
    MoveFrom(std::move(other));
}

RootObjectContext &RootObjectContext::operator=(RootObjectContext &&other) noexcept {
    if (this != &other) {
        Reset();
        MoveFrom(std::move(other));
    }
    return *this;
}

RootObjectContext::~RootObjectContext() {
    Reset();
}

void RootObjectContext::Bind(TTree *tree_p, TBranch *branch_p,
                             TClass *root_class_p,
                             RootDictionaryCleanupMode cleanup_mode_p,
                             std::string class_name_p) {
    Reset();
    tree = tree_p;
    branch = branch_p;
    root_class = root_class_p;
    cleanup_mode = cleanup_mode_p;
    class_name = class_name_p.empty() && root_class && root_class->GetName()
                     ? root_class->GetName()
                     : std::move(class_name_p);
    if (!tree || !branch || !root_class) {
        throw InvalidInputException("Cannot bind null ROOT tree/branch/class");
    }
    RootDebug("OBJECT.BEFORE_NEW", "class=" + class_name);
    owned_object = root_class->New();
    if (!owned_object) throw IOException("TClass::New failed for " + class_name);
    address_slot = owned_object;
    branch->SetAutoDelete(kFALSE);
    branch->SetAddress(&address_slot);
    RootDebug("OBJECT.BOUND", "class=" + class_name);
}

void *RootObjectContext::Read(uint64_t entry) {
    if (!tree || !branch || !address_slot) return nullptr;
    const auto bytes = tree->GetEntry(static_cast<Long64_t>(entry));
    return bytes < 0 ? nullptr : address_slot;
}

void RootObjectContext::MoveFrom(RootObjectContext &&other) {
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
    if (branch) branch->SetAddress(&address_slot);
}

void RootObjectContext::Reset() {
    if (branch) branch->ResetAddress();
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

TTree *FindTree(TFile *file, const std::string &tree_name,
                const std::string &root_class) {
    if (!file) return nullptr;
    if (!tree_name.empty()) {
        TTree *tree = nullptr;
        file->GetObject(tree_name.c_str(), tree);
        if (!tree) throw InvalidInputException("TTree not found: " + tree_name);
        return tree;
    }
    TTree *first = nullptr;
    TIter next(file->GetListOfKeys());
    while (auto *key = dynamic_cast<TKey *>(next())) {
        if (std::string(key->GetClassName()) != "TTree") continue;
        auto *tree = dynamic_cast<TTree *>(file->Get(key->GetName()));
        if (!tree) continue;
        if (!first) first = tree;
        if (root_class.empty()) return tree;
        auto *branches = tree->GetListOfBranches();
        for (int i = 0; branches && i < branches->GetEntries(); ++i) {
            auto *branch = dynamic_cast<TBranchElement *>(branches->At(i));
            if (branch && branch->GetClassName() &&
                root_class == branch->GetClassName()) {
                return tree;
            }
        }
    }
    return root_class.empty() ? first : nullptr;
}

TBranch *FindObjectBranch(TTree *tree, const std::string &root_class) {
    if (!tree) return nullptr;
    auto *branches = tree->GetListOfBranches();
    for (int i = 0; branches && i < branches->GetEntries(); ++i) {
        auto *branch = dynamic_cast<TBranchElement *>(branches->At(i));
        if (branch && branch->GetClassName() &&
            root_class == branch->GetClassName()) {
            return branch;
        }
    }
    auto *named = tree->GetBranch(root_class.c_str());
    if (named) return named;
    return root_class.empty() && branches && branches->GetEntries()
               ? dynamic_cast<TBranch *>(branches->At(0))
               : nullptr;
}

void CollectBranchTree(TBranch *branch, std::vector<TBranch *> &out) {
    if (!branch) return;
    out.push_back(branch);
    auto *children = branch->GetListOfBranches();
    for (int i = 0; children && i < children->GetEntries(); ++i) {
        CollectBranchTree(dynamic_cast<TBranch *>(children->At(i)), out);
    }
}

bool BranchNameEndsWithToken(const std::string &name, const std::string &token) {
    if (name == token) return true;
    if (name.size() <= token.size()) return false;
    const auto position = name.size() - token.size();
    if (name.compare(position, token.size(), token) != 0) return false;
    const char separator = name[position - 1];
    return separator == '.' || separator == '/' || separator == '_';
}

bool ContainsTokensInOrder(const std::string &name,
                           const std::vector<std::string> &tokens) {
    size_t position = 0;
    for (const auto &token : tokens) {
        const auto found = name.find(token, position);
        if (found == std::string::npos) return false;
        position = found + token.size();
    }
    return true;
}

TBranch *FindPhysicalBranch(TBranch *object_branch,
                            const std::vector<std::string> &fields) {
    if (!object_branch || fields.empty()) return nullptr;
    std::vector<TBranch *> all;
    CollectBranchTree(object_branch, all);
    const auto &leaf = fields.back();
    const auto dotted = JoinStrings(fields, ".");
    const bool allow_terminal_only = fields.size() == 1;
    TBranch *best = nullptr;
    int best_score = -1;
    for (auto *candidate : all) {
        if (!candidate) continue;
        const std::string name = candidate->GetName();
        int score = -1;
        if (name == dotted) score = 500;
        else if (BranchNameEndsWithToken(name, dotted)) score = 450;
        else if (ContainsTokensInOrder(name, fields)) score = 350;
        else if (allow_terminal_only && name == leaf) score = 250;
        else if (allow_terminal_only && BranchNameEndsWithToken(name, leaf)) score = 200;
        if (score < 0 || !HasPersistentBaskets(candidate)) continue;
        if (candidate->GetListOfBranches() &&
            candidate->GetListOfBranches()->GetEntries() == 0) {
            score += 25;
        }
        if (candidate->GetBasketSeek(0) > 0) score += 20;
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    return best;
}

bool HasPersistentBaskets(TBranch *branch) {
    if (!branch) return false;
    const int basket_count = branch->GetWriteBasket() + 1;
    auto *entries = branch->GetBasketEntry();
    if (basket_count <= 0 || !entries) return false;
    auto *bytes = branch->GetBasketBytes();
    for (int basket = 0; basket < basket_count; ++basket) {
        if (branch->GetBasketSeek(basket) > 0 ||
            (bytes && bytes[basket] > 0) || branch->GetBasket(basket)) {
            return true;
        }
    }
    return false;
}

PhysicalBranchResolution ResolvePhysicalBranch(
    TBranch *object_branch, const std::vector<std::string> &fields) {
    if (!object_branch) return {};
    if (auto *exact = FindPhysicalBranch(object_branch, fields)) {
        return {exact, "exact"};
    }
    for (idx_t prefix_size = fields.size(); prefix_size > 0; --prefix_size) {
        const std::vector<std::string> prefix(fields.begin(),
                                              fields.begin() + prefix_size);
        if (auto *ancestor = FindPhysicalBranch(object_branch, prefix)) {
            return {ancestor, prefix_size == fields.size() ? "exact" : "ancestor"};
        }
    }
    if (HasPersistentBaskets(object_branch)) return {object_branch, "object"};

    std::vector<TBranch *> all;
    CollectBranchTree(object_branch, all);
    TBranch *best = nullptr;
    int best_score = -1;
    for (auto *candidate : all) {
        if (candidate == object_branch || !HasPersistentBaskets(candidate)) continue;
        const std::string name = candidate->GetName();
        int score = 0;
        for (idx_t i = 0; i < fields.size(); ++i) {
            if (BranchNameEndsWithToken(name, fields[i])) {
                score += static_cast<int>(100 - std::min<idx_t>(99, i));
            }
        }
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    return {best, best ? "proxy" : ""};
}

std::string SchemaFingerprint(const std::string &root_class,
                              const std::vector<PathLevel> &levels) {
    uint64_t hash = FNV1a64(root_class);
    for (const auto &level : levels) {
        hash = FNV1a64(level.name, hash);
        hash = FNV1a64(level.type, hash);
        hash = FNV1a64(&level.offset_in_parent, sizeof(level.offset_in_parent), hash);
        const uint8_t flags = static_cast<uint8_t>(
            (level.is_pointer ? 1 : 0) | (level.is_container ? 2 : 0) |
            (level.is_primitive ? 4 : 0) | (level.is_string ? 8 : 0) |
            (level.is_fixed_array ? 16 : 0));
        hash = FNV1a64(&flags, sizeof(flags), hash);
        hash = FNV1a64(&level.fixed_array_length,
                       sizeof(level.fixed_array_length), hash);
        for (const auto dimension : level.array_dimensions) {
            hash = FNV1a64(&dimension, sizeof(dimension), hash);
        }
    }
    return Hex64(hash);
}

} // namespace duckdb::rootlake
