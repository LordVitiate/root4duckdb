#pragma once
#include "TBranch.h"
#include "TBranchElement.h"
#include "TBasket.h"
#include "TClass.h"
#include "TFile.h"
#include "TKey.h"
#include "TStreamerElement.h"
#include "TStreamerInfo.h"
#include "TString.h"
#include "TSystem.h"
#include "TTree.h"
#include "TTreeCache.h"
#include "TVirtualCollectionProxy.h"

#ifdef BIT
#undef BIT
#endif

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb::rootlake {

static constexpr uint32_t ROOT_LAKE_INDEX_VERSION = 12;

inline uint64_t FNV1a64(const void *ptr, size_t size, uint64_t seed = 14695981039346656037ULL) {
    auto bytes = static_cast<const uint8_t *>(ptr);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t FNV1a64(const std::string &value, uint64_t seed = 14695981039346656037ULL) {
    return FNV1a64(value.data(), value.size(), seed);
}

inline std::string Hex64(uint64_t value) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << value;
    return ss.str();
}

inline std::string DoubleText(double value) {
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return ss.str();
}

inline std::string CsvEscape(const std::string &value) {
    bool quote = value.empty() || value.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote) {
        return value;
    }
    std::string out = "\"";
    for (char c : value) {
        if (c == '\"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

inline std::string JoinStrings(const std::vector<std::string> &values, const std::string &delimiter) {
    std::string result;
    for (idx_t i = 0; i < values.size(); ++i) {
        if (i) result += delimiter;
        result += values[i];
    }
    return result;
}

inline std::string SqlLiteral(const std::string &value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

inline std::string NormalizePath(std::string path) {
    if (path.empty()) {
        throw InvalidInputException("ROOT logical path is empty");
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

struct ParsedPath {
    std::string root_class;
    std::vector<std::string> fields;
};

inline ParsedPath ParsePath(const std::string &raw_path) {
    auto path = NormalizePath(raw_path);
    ParsedPath result;
    std::stringstream ss(path.substr(1));
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty()) {
            continue;
        }
        if (result.root_class.empty()) {
            result.root_class = part;
        } else {
            result.fields.push_back(part);
        }
    }
    if (result.root_class.empty() || result.fields.empty()) {
        throw InvalidInputException("Expected a leaf path such as /PaEvent/vecHit/u, got: " + raw_path);
    }
    return result;
}

inline std::string StripStd(std::string type) {
    if (type.rfind("std::", 0) == 0) {
        type.erase(0, 5);
    }
    return type;
}

inline std::string TrimType(std::string type) {
    const auto first = type.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = type.find_last_not_of(" \t\r\n");
    type = type.substr(first, last - first + 1);
    while (type.rfind("const ", 0) == 0) type.erase(0, 6);
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
    return StripStd(type);
}

inline std::string TemplatePrimaryName(const std::string &raw_type) {
    auto type = TrimType(raw_type);
    const auto open = type.find('<');
    if (open != std::string::npos) type.resize(open);
    return TrimType(type);
}

inline std::vector<std::string> TemplateArguments(const std::string &raw_type) {
    const auto open = raw_type.find('<');
    if (open == std::string::npos) return {};
    std::vector<std::string> result;
    int depth = 0;
    size_t token_begin = open + 1;
    for (size_t i = open + 1; i < raw_type.size(); ++i) {
        const char c = raw_type[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            if (depth == 0) {
                result.push_back(TrimType(raw_type.substr(token_begin, i - token_begin)));
                return result;
            }
            --depth;
        } else if (c == ',' && depth == 0) {
            result.push_back(TrimType(raw_type.substr(token_begin, i - token_begin)));
            token_begin = i + 1;
        }
    }
    return {};
}

inline bool IsAssociativeContainerType(const std::string &raw_type) {
    const auto name = TemplatePrimaryName(raw_type);
    return name == "map" || name == "multimap" || name == "unordered_map" ||
           name == "unordered_multimap";
}

inline bool IsSetContainerType(const std::string &raw_type) {
    const auto name = TemplatePrimaryName(raw_type);
    return name == "set" || name == "multiset" || name == "unordered_set" ||
           name == "unordered_multiset";
}

inline bool IsPairType(const std::string &raw_type) {
    return TemplatePrimaryName(raw_type) == "pair";
}

inline bool IsContiguousVectorType(const std::string &raw_type) {
    return TemplatePrimaryName(raw_type) == "vector";
}

inline bool ContiguousVectorPathEnabled() {
    const char *value = std::getenv("ROOT4DUCKDB_DISABLE_CONTIGUOUS_VECTOR");
    return !(value && *value && std::string(value) != "0");
}

inline bool IsPointerType(std::string raw_type) {
    raw_type = TrimType(raw_type);
    return !raw_type.empty() && raw_type.back() == '*';
}


struct FixedArrayTypeInfo {
    std::string base_type;
    std::vector<uint32_t> dimensions;
    uint64_t length = 1;
};

inline FixedArrayTypeInfo ParseFixedArrayType(const std::string &raw_type) {
    FixedArrayTypeInfo result;
    result.base_type = StripStd(raw_type);
    const auto first_bracket = result.base_type.find('[');
    if (first_bracket == std::string::npos) {
        result.length = 0;
        return result;
    }
    const auto suffix = result.base_type.substr(first_bracket);
    result.base_type = result.base_type.substr(0, first_bracket);
    while (!result.base_type.empty() && std::isspace(static_cast<unsigned char>(result.base_type.back()))) {
        result.base_type.pop_back();
    }
    size_t pos = 0;
    result.length = 1;
    while (pos < suffix.size()) {
        const auto open = suffix.find('[', pos);
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
        pos = close + 1;
    }
    if (result.dimensions.empty()) result.length = 0;
    return result;
}

inline std::string PrimitiveBaseType(const std::string &raw_type) {
    const auto parsed = ParseFixedArrayType(raw_type);
    return TrimType(parsed.length ? parsed.base_type : raw_type);
}

inline uint32_t PrimitiveTypeSize(const std::string &raw_type) {
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

inline std::string ArrayDimensionsText(const std::vector<uint32_t> &dimensions) {
    std::string result;
    for (idx_t i = 0; i < dimensions.size(); ++i) {
        if (i) result += 'x';
        result += std::to_string(dimensions[i]);
    }
    return result;
}

inline bool IsPrimitiveType(const std::string &raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    static const std::vector<std::string> primitive_types = {
        "Bool_t", "bool", "Char_t", "char", "UChar_t", "unsigned char", "Short_t", "short",
        "UShort_t", "unsigned short", "Int_t", "int", "UInt_t", "unsigned int", "Long_t", "long",
        "ULong_t", "unsigned long", "Long64_t", "long long", "ULong64_t", "unsigned long long",
        "Float_t", "float", "Double_t", "double"
    };
    return std::find(primitive_types.begin(), primitive_types.end(), type) != primitive_types.end();
}

inline bool IsStringType(const std::string &raw_type) {
    const auto type = TrimType(raw_type);
    return type == "string" || type == "TString";
}

inline std::string ExtractInnerType(const std::string &container_type) {
    const auto args = TemplateArguments(container_type);
    return args.empty() ? std::string() : args.front();
}

inline std::string ExtractMappedType(const std::string &container_type) {
    const auto args = TemplateArguments(container_type);
    return args.size() < 2 ? std::string() : args[1];
}

inline LogicalType RootTypeToLogicalType(const std::string &raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") return LogicalType::BOOLEAN;
    if (type == "Char_t" || type == "char" || type == "b") return LogicalType::TINYINT;
    if (type == "UChar_t" || type == "unsigned char" || type == "B") return LogicalType::UTINYINT;
    if (type == "Short_t" || type == "short" || type == "S") return LogicalType::SMALLINT;
    if (type == "UShort_t" || type == "unsigned short" || type == "s") return LogicalType::USMALLINT;
    if (type == "Int_t" || type == "int" || type == "I") return LogicalType::INTEGER;
    if (type == "UInt_t" || type == "unsigned int" || type == "i") return LogicalType::UINTEGER;
    if (type == "Long_t" || type == "long" || type == "Long64_t" || type == "long long" || type == "L") {
        return LogicalType::BIGINT;
    }
    if (type == "ULong_t" || type == "unsigned long" || type == "ULong64_t" || type == "unsigned long long" ||
        type == "l") {
        return LogicalType::UBIGINT;
    }
    if (type == "Float_t" || type == "float" || type == "F") return LogicalType::FLOAT;
    if (type == "Double_t" || type == "double" || type == "D") return LogicalType::DOUBLE;
    if (IsStringType(type)) return LogicalType::VARCHAR;
    throw NotImplementedException("Unsupported ROOT leaf type: " + raw_type);
}

inline bool IsLosslessDoubleBackedType(const std::string &raw_type) {
    const auto type = RootTypeToLogicalType(raw_type);
    switch (type.id()) {
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

inline double ReadPrimitiveAsDouble(void *ptr, const std::string &raw_type) {
    if (!ptr) {
        return 0;
    }
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Float_t" || type == "float" || type == "F") return *reinterpret_cast<float *>(ptr);
    if (type == "Double_t" || type == "double" || type == "D") return *reinterpret_cast<double *>(ptr);
    if (type == "Int_t" || type == "int" || type == "I") return *reinterpret_cast<int32_t *>(ptr);
    if (type == "UInt_t" || type == "unsigned int" || type == "i") return *reinterpret_cast<uint32_t *>(ptr);
    if (type == "Long64_t" || type == "long long" || type == "L") return static_cast<double>(*reinterpret_cast<int64_t *>(ptr));
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l") return static_cast<double>(*reinterpret_cast<uint64_t *>(ptr));
    if (type == "Long_t" || type == "long") return static_cast<double>(*reinterpret_cast<long *>(ptr));
    if (type == "ULong_t" || type == "unsigned long") return static_cast<double>(*reinterpret_cast<unsigned long *>(ptr));
    if (type == "Short_t" || type == "short" || type == "S") return *reinterpret_cast<int16_t *>(ptr);
    if (type == "UShort_t" || type == "unsigned short" || type == "s") return *reinterpret_cast<uint16_t *>(ptr);
    if (type == "Char_t" || type == "char" || type == "b") return *reinterpret_cast<int8_t *>(ptr);
    if (type == "UChar_t" || type == "unsigned char" || type == "B") return *reinterpret_cast<uint8_t *>(ptr);
    if (type == "Bool_t" || type == "bool" || type == "O") return *reinterpret_cast<bool *>(ptr) ? 1.0 : 0.0;
    throw NotImplementedException("Unsupported primitive ROOT type: " + raw_type);
}

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

struct StreamerFieldMatch {
    TStreamerElement *element = nullptr;
    int64_t offset = 0;
};

inline std::optional<StreamerFieldMatch> FindStreamerFieldRecursive(TClass *klass, const std::string &field,
                                                                    std::vector<std::string> &visited) {
    if (!klass) return std::nullopt;
    const std::string class_name = klass->GetName();
    if (std::find(visited.begin(), visited.end(), class_name) != visited.end()) return std::nullopt;
    visited.push_back(class_name);
    auto *streamer = klass->GetStreamerInfo();
    if (!streamer) {
        visited.pop_back();
        return std::nullopt;
    }
    auto *elements = streamer->GetElements();
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element || element->IsBase()) continue;
        if (field == element->GetName()) {
            StreamerFieldMatch result;
            result.element = element;
            result.offset = streamer->GetElementOffset(i);
            visited.pop_back();
            return result;
        }
    }
    // Semantic paths normally omit implementation-level base-class names. Search
    // persistent base streamer layouts and fold their offsets into the requested field.
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto *base = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!base || !base->IsBase()) continue;
        auto *base_class = base->GetClassPointer();
        if (!base_class) base_class = TClass::GetClass(base->GetTypeName());
        auto nested = FindStreamerFieldRecursive(base_class, field, visited);
        if (!nested) continue;
        nested->offset += streamer->GetElementOffset(i);
        visited.pop_back();
        return nested;
    }
    visited.pop_back();
    return std::nullopt;
}

class PathResolver {
public:
    static std::vector<PathLevel> Resolve(TClass *root_class, const std::vector<std::string> &fields) {
        if (!root_class) {
            throw InvalidInputException("ROOT dictionary class is null");
        }
        auto *current_class = root_class;
        if (!current_class->GetStreamerInfo()) {
            throw InvalidInputException("No TStreamerInfo for class " + std::string(root_class->GetName()));
        }

        std::vector<PathLevel> levels;
        int64_t cumulative = 0;
        for (idx_t field_idx = 0; field_idx < fields.size(); ++field_idx) {
            const auto &field = fields[field_idx];
            // /value is a synthetic element step for sequence/set containers.  It can
            // terminate at a primitive or continue through nested containers/objects.
            // Associative containers reserve /key and /value as aliases for pair.first/second.
            if (field == "value" && !levels.empty() && levels.back().is_container &&
                !IsAssociativeContainerType(levels.back().type)) {
                PathLevel value_level;
                value_level.name = "value";
                value_level.type = levels.back().element_class ? levels.back().element_class->GetName()
                                                                : ExtractInnerType(levels.back().type);
                value_level.offset_in_parent = 0;
                value_level.cumulative_offset = levels.back().cumulative_offset;
                value_level.klass = levels.back().element_class;
                if (!value_level.klass && !value_level.type.empty()) {
                    value_level.klass = TClass::GetClass(value_level.type.c_str());
                }
                value_level.is_primitive = IsPrimitiveType(value_level.type);
                value_level.is_string = IsStringType(value_level.type);
                value_level.is_container = value_level.klass && value_level.klass->GetCollectionProxy();
                if (value_level.is_container) {
                    value_level.element_class = value_level.klass->GetCollectionProxy()->GetValueClass();
                }
                value_level.element_size = value_level.is_primitive ? PrimitiveTypeSize(value_level.type)
                                                                     : (value_level.klass ? static_cast<uint32_t>(value_level.klass->Size()) : 0);
                const bool terminal = field_idx + 1 == fields.size();
                if (terminal && !value_level.is_primitive && !value_level.is_string) {
                    throw InvalidInputException("Container /value does not terminate in a primitive/string: " +
                                                value_level.type);
                }
                if (!terminal && !value_level.klass) {
                    throw InvalidInputException("Cannot descend through container value type: " + value_level.type);
                }
                levels.push_back(value_level);
                cumulative = value_level.is_container ? 0 : value_level.cumulative_offset;
                current_class = value_level.is_container ? value_level.element_class : value_level.klass;
                continue;
            }

            if (!current_class) {
                throw InvalidInputException("Cannot descend through primitive field before '" + field + "'");
            }
            std::string streamer_field = field;
            if (!levels.empty() && levels.back().is_container &&
                IsAssociativeContainerType(levels.back().type)) {
                if (field == "key") streamer_field = "first";
                else if (field == "value") streamer_field = "second";
            }
            std::vector<std::string> visited;
            auto match = FindStreamerFieldRecursive(current_class, streamer_field, visited);
            if (!match || !match->element) {
                throw InvalidInputException("Field '" + field + "' is absent in ROOT streamer path");
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

            const int array_rank = element->GetArrayDim();
            uint64_t array_length = 1;
            for (int dim = 0; dim < array_rank; ++dim) {
                const int extent = element->GetMaxIndex(dim);
                if (extent <= 0 || array_length > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(extent)) {
                    array_length = 0;
                    level.array_dimensions.clear();
                    break;
                }
                level.array_dimensions.push_back(static_cast<uint32_t>(extent));
                array_length *= static_cast<uint64_t>(extent);
            }
            // Some dictionaries expose the brackets only through the type name.
            if (!array_length || level.array_dimensions.empty()) {
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
            level.element_size = level.is_primitive ? PrimitiveTypeSize(level.type)
                                                    : (level.klass ? static_cast<uint32_t>(level.klass->Size()) : 0);
            if (level.is_fixed_array && !level.element_size) {
                throw NotImplementedException("Cannot determine element size for fixed ROOT array " + field);
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
            if (current_class && !current_class->GetStreamerInfo()) {
                throw InvalidInputException("No TStreamerInfo for nested class " + std::string(current_class->GetName()));
            }
        }
        if (levels.empty()) {
            throw InvalidInputException("Resolved ROOT path is empty");
        }
        const auto &leaf = levels.back();
        if (!leaf.is_primitive && !leaf.is_string) {
            throw InvalidInputException("ROOT path does not end in a primitive/string value");
        }
        return levels;
    }
};

inline void AppendLevelIndexNames(const PathLevel &level, std::vector<std::string> &names) {
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

inline std::string IndexSignature(const std::vector<PathLevel> &levels) {
    std::vector<std::string> names;
    for (const auto &level : levels) AppendLevelIndexNames(level, names);
    return JoinStrings(names, ",");
}

inline idx_t IndexDepth(const std::vector<PathLevel> &levels) {
    idx_t result = 0;
    for (const auto &level : levels) {
        if (level.is_container) ++result;
        if (level.is_fixed_array) result += std::max<idx_t>(1, level.array_dimensions.size());
    }
    return result;
}

struct NumericValue {
    double value = 0;
    std::vector<int32_t> indices;
};

class OffsetValueReader {
public:
    // Compatibility helper matching the original universal reader result shape.
    static void Collect(void *root_object, const std::vector<PathLevel> &levels, std::vector<NumericValue> &out) {
        std::vector<int32_t> indices;
        auto emit = [&](double value, const std::vector<int32_t> &current_indices) {
            out.push_back({value, current_indices});
        };
        CollectRecursive(root_object, levels, 0, indices, emit);
    }

    // Index building needs values only. Avoid allocating/copying an index vector for every hit.
    static void CollectValues(void *root_object, const std::vector<PathLevel> &levels, std::vector<double> &out) {
        std::vector<int32_t> indices;
        auto emit = [&](double value, const std::vector<int32_t> &) { out.push_back(value); };
        CollectRecursive(root_object, levels, 0, indices, emit);
    }

    // Runtime output uses a fixed container depth. Store all indices in one contiguous array:
    // [row0_idx0, row0_idx1, ..., row1_idx0, row1_idx1, ...].
    static void CollectFlat(void *root_object, const std::vector<PathLevel> &levels, idx_t index_depth,
                            std::vector<double> &values, std::vector<int32_t> &flat_indices) {
        std::vector<int32_t> indices;
        indices.reserve(index_depth);
        auto emit = [&](double value, const std::vector<int32_t> &current_indices) {
            if (current_indices.size() != index_depth) {
                throw IOException("ROOT container depth mismatch while executing indexed access plan: expected " +
                                  std::to_string(index_depth) + ", got " +
                                  std::to_string(current_indices.size()));
            }
            values.push_back(value);
            flat_indices.insert(flat_indices.end(), current_indices.begin(), current_indices.end());
        };
        CollectRecursive(root_object, levels, 0, indices, emit);
    }

private:
    static void PushArrayCoordinates(uint64_t flat_index, const std::vector<uint32_t> &dimensions,
                                     std::vector<int32_t> &indices) {
        if (dimensions.empty()) {
            indices.push_back(static_cast<int32_t>(flat_index));
            return;
        }
        std::vector<int32_t> coordinates(dimensions.size(), 0);
        for (idx_t reverse = dimensions.size(); reverse > 0; --reverse) {
            const idx_t dim = reverse - 1;
            const auto extent = static_cast<uint64_t>(dimensions[dim]);
            coordinates[dim] = static_cast<int32_t>(flat_index % extent);
            flat_index /= extent;
        }
        indices.insert(indices.end(), coordinates.begin(), coordinates.end());
    }

    struct ContainerAccess {
        char *base = nullptr;
        uint32_t stride = 0;
        bool contiguous = false;
    };

    static ContainerAccess PrepareContainerAccess(const PathLevel &level, TVirtualCollectionProxy *proxy,
                                                  size_t size) {
        ContainerAccess access;
        if (!proxy || size == 0 || !ContiguousVectorPathEnabled() || !IsContiguousVectorType(level.type)) return access;
        const auto inner = ExtractInnerType(level.type);
        if (inner.empty() || TrimType(inner) == "bool" || TrimType(inner) == "Bool_t" || IsPointerType(inner)) {
            return access;
        }
        uint32_t stride = PrimitiveTypeSize(inner);
        if (!stride && level.element_class) stride = static_cast<uint32_t>(level.element_class->Size());
        if (!stride) return access;
        auto *first = static_cast<char *>(proxy->At(0));
        if (!first) return access;
        if (size > 1) {
            auto *second = static_cast<char *>(proxy->At(1));
            if (!second || second != first + stride) return access;
        }
        access.base = first;
        access.stride = stride;
        access.contiguous = true;
        return access;
    }

    template <class EMIT>
    static void CollectRecursive(void *current, const std::vector<PathLevel> &levels, idx_t level_idx,
                                 std::vector<int32_t> &indices, EMIT &emit) {
        if (!current || level_idx >= levels.size()) return;
        const auto &level = levels[level_idx];
        auto *field = static_cast<char *>(current) + level.offset_in_parent;
        if (level.is_pointer) {
            field = field ? *reinterpret_cast<char **>(field) : nullptr;
            if (!field) return;
        }
        const bool last = level_idx + 1 == levels.size();

        if (level.is_fixed_array) {
            const idx_t pushed = std::max<idx_t>(1, level.array_dimensions.size());
            for (uint64_t i = 0; i < level.fixed_array_length; ++i) {
                auto *element = field + i * level.element_size;
                PushArrayCoordinates(i, level.array_dimensions, indices);
                if (last) {
                    if (level.is_primitive) emit(ReadPrimitiveAsDouble(element, level.type), indices);
                } else {
                    CollectRecursive(element, levels, level_idx + 1, indices, emit);
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
            if (!IsPrimitiveType(inner)) return;
            const auto size = proxy->Size();
            const auto access = PrepareContainerAccess(level, proxy, size);
            for (size_t i = 0; i < size; ++i) {
                auto *element = access.contiguous ? static_cast<void *>(access.base + i * access.stride)
                                                  : proxy->At(i);
                if (!element) continue;
                indices.push_back(static_cast<int32_t>(i));
                emit(ReadPrimitiveAsDouble(element, inner), indices);
                indices.pop_back();
            }
            return;
        }

        if (last) {
            if (level.is_primitive) emit(ReadPrimitiveAsDouble(field, level.type), indices);
            return;
        }

        if (level.is_container) {
            auto *proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
            if (!proxy) return;
            TVirtualCollectionProxy::TPushPop guard(proxy, field);
            const auto size = proxy->Size();
            const auto access = PrepareContainerAccess(level, proxy, size);
            for (size_t i = 0; i < size; ++i) {
                auto *element = access.contiguous ? static_cast<void *>(access.base + i * access.stride)
                                                  : proxy->At(i);
                if (!element) continue;
                indices.push_back(static_cast<int32_t>(i));
                CollectRecursive(element, levels, level_idx + 1, indices, emit);
                indices.pop_back();
            }
            return;
        }

        if (level.klass) CollectRecursive(field, levels, level_idx + 1, indices, emit);
    }

};

enum class RootDictionaryCleanupMode : uint8_t {
    FULL = 0,
    DESTRUCT_ONLY = 1,
    RETAIN = 2
};

inline RootDictionaryCleanupMode ParseDictionaryCleanupMode(std::string mode, bool external_dictionary_loaded) {
    if (mode.empty() || mode == "auto") {
        // External PHAST dictionaries may allocate and destroy objects through a
        // different DSO/allocator.  Until allocator ownership is proven, the only
        // teardown that cannot corrupt the process heap is to retain the one
        // top-level TClass::New allocation per active reader.  Production workers
        // bound the number of readers/files, so this is a contained compatibility
        // mode rather than an unbounded year-scale leak.
        return RootDictionaryCleanupMode::FULL;
    }
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

class RootObjectContext {
public:
    RootObjectContext() = default;
    RootObjectContext(const RootObjectContext &) = delete;
    RootObjectContext &operator=(const RootObjectContext &) = delete;
    RootObjectContext(RootObjectContext &&other) noexcept { MoveFrom(std::move(other)); }
    RootObjectContext &operator=(RootObjectContext &&other) noexcept {
        if (this == &other) return *this;
        Reset();
        MoveFrom(std::move(other));
        return *this;
    }
    ~RootObjectContext() { Reset(); }

    void Bind(TTree *tree_p, TBranch *branch_p, TClass *class_p,
              RootDictionaryCleanupMode cleanup_mode_p = RootDictionaryCleanupMode::FULL) {
        Reset();
        tree = tree_p;
        branch = branch_p;
        root_class = class_p;
        cleanup_mode = cleanup_mode_p;
        if (!tree || !branch || !root_class) {
            throw InvalidInputException("Cannot bind null ROOT tree/branch/class");
        }

        // Mirror the proven universal_reader ownership model exactly:
        // keep the allocation we own separate from the address slot ROOT may update.
        owned_object = root_class->New();
        if (!owned_object) {
            throw IOException("TClass::New failed for " + std::string(root_class->GetName()));
        }
        address_slot = owned_object;
        branch->SetAutoDelete(kFALSE);
        branch->SetAddress(&address_slot);

        // Do not disable child branches. Split ROOT objects are materialized only
        // when TTree::GetEntry loads their physical leaves.
    }

    void *Read(uint64_t entry) {
        if (!tree || !branch || !address_slot) return nullptr;
        const auto bytes = tree->GetEntry(static_cast<Long64_t>(entry));
        if (bytes < 0) return nullptr;
        return address_slot;
    }

    TTree *tree = nullptr;
    TBranch *branch = nullptr;
    TClass *root_class = nullptr;
    void *owned_object = nullptr;
    void *address_slot = nullptr;
    RootDictionaryCleanupMode cleanup_mode = RootDictionaryCleanupMode::FULL;
private:
    void MoveFrom(RootObjectContext &&other) {
        tree = other.tree;
        branch = other.branch;
        root_class = other.root_class;
        owned_object = other.owned_object;
        address_slot = other.address_slot;
        cleanup_mode = other.cleanup_mode;
        other.tree = nullptr;
        other.branch = nullptr;
        other.root_class = nullptr;
        other.owned_object = nullptr;
        other.address_slot = nullptr;
        other.cleanup_mode = RootDictionaryCleanupMode::FULL;
        if (branch) branch->SetAddress(&address_slot);
    }

    void Reset() {
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
                // Intentionally skip both destructor and deallocation.  The PHAST
                // runtime has demonstrated cross-DSO heap corruption even after a
                // dtor-only teardown.  A bounded worker process is the ownership
                // boundary until the external dictionary is rebuilt consistently.
                break;
            }
        }
        tree = nullptr;
        branch = nullptr;
        root_class = nullptr;
        owned_object = nullptr;
        address_slot = nullptr;
        cleanup_mode = RootDictionaryCleanupMode::FULL;
    }
};

inline TTree *FindTree(TFile *file, const std::string &tree_name, const std::string &root_class) {
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
        auto *branches = tree->GetListOfBranches();
        for (int i = 0; branches && i < branches->GetEntries(); ++i) {
            auto *branch = dynamic_cast<TBranchElement *>(branches->At(i));
            if (branch && branch->GetClassName() && root_class == branch->GetClassName()) return tree;
        }
    }
    return root_class.empty() ? first : nullptr;
}

inline TBranch *FindObjectBranch(TTree *tree, const std::string &root_class) {
    if (!tree) return nullptr;
    auto *branches = tree->GetListOfBranches();
    for (int i = 0; branches && i < branches->GetEntries(); ++i) {
        auto *branch = dynamic_cast<TBranchElement *>(branches->At(i));
        if (branch && branch->GetClassName() && root_class == branch->GetClassName()) return branch;
    }
    auto *by_name = tree->GetBranch(root_class.c_str());
    if (by_name) return by_name;
    return nullptr;
}

inline void CollectBranchTree(TBranch *branch, std::vector<TBranch *> &out) {
    if (!branch) return;
    out.push_back(branch);
    auto *children = branch->GetListOfBranches();
    for (int i = 0; children && i < children->GetEntries(); ++i) {
        CollectBranchTree(dynamic_cast<TBranch *>(children->At(i)), out);
    }
}

inline bool BranchNameEndsWithToken(const std::string &name, const std::string &token) {
    if (name == token) return true;
    if (name.size() <= token.size()) return false;
    const auto pos = name.size() - token.size();
    if (name.compare(pos, token.size(), token) != 0) return false;
    const char separator = name[pos - 1];
    return separator == '.' || separator == '/' || separator == '_';
}

inline bool ContainsTokensInOrder(const std::string &name, const std::vector<std::string> &tokens) {
    size_t pos = 0;
    for (const auto &token : tokens) {
        const auto found = name.find(token, pos);
        if (found == std::string::npos) return false;
        pos = found + token.size();
    }
    return true;
}

// Resolve the persistent branch that owns basket ranges and bytes. The guarded
// serialized reader may consume its decompressed buffer; the universal reader
// continues to materialize only through the complete top-level object branch.
inline TBranch *FindPhysicalBranch(TBranch *object_branch, const std::vector<std::string> &fields) {
    if (!object_branch || fields.empty()) return nullptr;
    std::vector<TBranch *> all;
    CollectBranchTree(object_branch, all);
    const auto &leaf = fields.back();
    const auto dotted = JoinStrings(fields, ".");
    // A terminal-only match is unambiguous only for a one-component path.
    // For a nested prefix such as vecParticle/vecVertex, accepting an unrelated
    // top-level PaEvent.vecVertex branch prevents ResolvePhysicalBranch from
    // shortening the prefix to the actual persistent vecParticle ancestor.
    const bool allow_terminal_only_match = fields.size() == 1;
    TBranch *best = nullptr;
    int best_score = -1;
    for (auto *candidate : all) {
        if (!candidate) continue;
        const std::string name = candidate->GetName();
        int score = -1;
        if (name == dotted) score = 500;
        else if (BranchNameEndsWithToken(name, dotted)) score = 450;
        else if (ContainsTokensInOrder(name, fields)) score = 350;
        else if (allow_terminal_only_match && name == leaf) score = 250;
        else if (allow_terminal_only_match && BranchNameEndsWithToken(name, leaf)) {
            score = 200;
        }
        if (score < 0) continue;

        const int basket_count = candidate->GetWriteBasket() + 1;
        auto *entries = candidate->GetBasketEntry();
        if (basket_count <= 0 || !entries) continue;
        bool persistent = false;
        auto *basket_bytes = candidate->GetBasketBytes();
        for (int basket_id = 0; basket_id < basket_count; ++basket_id) {
            if (candidate->GetBasketSeek(basket_id) > 0 ||
                (basket_bytes && basket_bytes[basket_id] > 0) || candidate->GetBasket(basket_id)) {
                persistent = true;
                break;
            }
        }
        if (!persistent) continue;
        if (candidate->GetListOfBranches() && candidate->GetListOfBranches()->GetEntries() == 0) score += 25;
        if (candidate->GetBasketSeek(0) > 0) score += 20;
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    return best;
}


inline bool HasPersistentBaskets(TBranch *branch) {
    if (!branch) return false;
    const int basket_count = branch->GetWriteBasket() + 1;
    auto *entries = branch->GetBasketEntry();
    if (basket_count <= 0 || !entries) return false;
    for (int basket_id = 0; basket_id < basket_count; ++basket_id) {
        if (branch->GetBasketSeek(basket_id) > 0) return true;
        auto *bytes = branch->GetBasketBytes();
        if (bytes && bytes[basket_id] > 0) return true;
        if (branch->GetBasket(basket_id)) return true;
    }
    return false;
}

struct PhysicalBranchResolution {
    TBranch *branch = nullptr;
    std::string mode;
};

inline PhysicalBranchResolution ResolvePhysicalBranch(TBranch *object_branch,
                                                       const std::vector<std::string> &fields) {
    if (!object_branch) return {};
    if (auto *exact = FindPhysicalBranch(object_branch, fields)) {
        return {exact, "exact"};
    }
    // If no leaf branch exists, an ancestor may own a self-contained member-wise
    // byte stream. The serialized planner proves that separately; otherwise this
    // resolution remains scheduling metadata for the universal reader.
    for (idx_t prefix_size = fields.size(); prefix_size > 0; --prefix_size) {
        std::vector<std::string> prefix(fields.begin(), fields.begin() + prefix_size);
        if (auto *ancestor = FindPhysicalBranch(object_branch, prefix)) {
            return {ancestor, prefix_size == fields.size() ? "exact" : "ancestor"};
        }
    }
    if (HasPersistentBaskets(object_branch)) return {object_branch, "object"};

    // A proxy branch may provide basket ranges only. Value materialization always
    // uses the complete object branch and the original offset access plan.
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

inline std::string SchemaFingerprint(const std::string &root_class, const std::vector<PathLevel> &levels) {
    // A schema is the StreamerInfo/offset layout only. Split-branch layout is file metadata,
    // never part of semantic identity or correctness.
    uint64_t hash = FNV1a64(root_class);
    for (const auto &level : levels) {
        hash = FNV1a64(level.name, hash);
        hash = FNV1a64(level.type, hash);
        hash = FNV1a64(&level.offset_in_parent, sizeof(level.offset_in_parent), hash);
        const uint8_t flags = static_cast<uint8_t>((level.is_pointer ? 1 : 0) | (level.is_container ? 2 : 0) |
                                                   (level.is_primitive ? 4 : 0) | (level.is_string ? 8 : 0) |
                                                   (level.is_fixed_array ? 16 : 0));
        hash = FNV1a64(&flags, sizeof(flags), hash);
        hash = FNV1a64(&level.fixed_array_length, sizeof(level.fixed_array_length), hash);
        for (const auto dimension : level.array_dimensions) {
            hash = FNV1a64(&dimension, sizeof(dimension), hash);
        }
    }
    return Hex64(hash);
}

inline std::string FileId(const std::string &uri, uint64_t size, int64_t mtime) {
    uint64_t hash = FNV1a64(uri);
    hash = FNV1a64(&size, sizeof(size), hash);
    hash = FNV1a64(&mtime, sizeof(mtime), hash);
    return Hex64(hash);
}

inline std::string ColumnId(const std::string &schema_id, const std::string &logical_path) {
    return Hex64(FNV1a64(logical_path, FNV1a64(schema_id)));
}

inline void EnsureQueryOK(MaterializedQueryResult &result, const std::string &label) {
    if (result.HasError()) {
        throw IOException(label + ": " + result.GetError());
    }
}

} // namespace duckdb::rootlake
