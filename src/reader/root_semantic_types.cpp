#include "root4duckdb/reader/root_semantic_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace duckdb::rootlake {

std::string NormalizePath(std::string path) {
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

ParsedPath ParsePathPrefix(const std::string& raw_path) {
    ParsedPath result;
    std::string path = raw_path;
    if (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.rfind("events/", 0) == 0) {
        path.erase(0, 8);
    }
    std::stringstream stream(path);
    std::string component;
    while (std::getline(stream, component, '/')) {
        if (component.empty()) {
            continue;
        }
        if (result.root_class.empty()) {
            result.root_class = component;
        } else {
            result.fields.push_back(component);
        }
    }
    return result;
}

ParsedPath ParsePath(const std::string& raw_path) {
    const auto path = NormalizePath(raw_path);
    ParsedPath result;
    std::stringstream stream(path.substr(1));
    std::string component;
    while (std::getline(stream, component, '/')) {
        if (component.empty()) {
            continue;
        }
        if (result.root_class.empty()) {
            result.root_class = component;
        } else {
            result.fields.push_back(component);
        }
    }
    if (result.root_class.empty() || result.fields.empty()) {
        throw InvalidInputException("Expected a leaf path such as /PaEvent/vecHit/u, got: " + raw_path);
    }
    return result;
}

std::string StripStd(std::string type) {
    if (type.rfind("std::", 0) == 0) {
        type.erase(0, 5);
    }
    return type;
}

std::string TrimType(std::string type) {
    const auto first = type.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = type.find_last_not_of(" \t\r\n");
    type = type.substr(first, last - first + 1);
    while (type.rfind("const ", 0) == 0) {
        type.erase(0, 6);
    }
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
        type.pop_back();
    }
    return StripStd(type);
}

std::string TemplatePrimaryName(const std::string& raw_type) {
    auto type = TrimType(raw_type);
    const auto open = type.find('<');
    if (open != std::string::npos) {
        type.resize(open);
    }
    return TrimType(type);
}

std::vector<std::string> TemplateArguments(const std::string& raw_type) {
    const auto open = raw_type.find('<');
    if (open == std::string::npos) {
        return {};
    }
    std::vector<std::string> result;
    int depth = 0;
    size_t token_begin = open + 1;
    for (size_t i = open + 1; i < raw_type.size(); ++i) {
        const char character = raw_type[i];
        if (character == '<') {
            ++depth;
        } else if (character == '>') {
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

bool IsAssociativeContainerType(const std::string& raw_type) {
    const auto name = TemplatePrimaryName(raw_type);
    return name == "map" || name == "multimap" || name == "unordered_map" || name == "unordered_multimap";
}

bool IsContiguousVectorType(const std::string& raw_type) {
    return TemplatePrimaryName(raw_type) == "vector";
}

bool ContiguousVectorPathEnabled() {
    const char* value = std::getenv("ROOT4DUCKDB_DISABLE_CONTIGUOUS_VECTOR");
    return !(value && *value && std::string(value) != "0");
}

bool IsPointerType(std::string raw_type) {
    raw_type = TrimType(std::move(raw_type));
    return !raw_type.empty() && raw_type.back() == '*';
}

FixedArrayTypeInfo ParseFixedArrayType(const std::string& raw_type) {
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
        if (open == std::string::npos) {
            break;
        }
        const auto close = suffix.find(']', open + 1);
        if (close == std::string::npos) {
            result.dimensions.clear();
            result.length = 0;
            return result;
        }
        const auto token = suffix.substr(open + 1, close - open - 1);
        if (token.empty() || !std::all_of(token.begin(), token.end(),
                                          [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
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
    if (result.dimensions.empty()) {
        result.length = 0;
    }
    return result;
}

std::string PrimitiveBaseType(const std::string& raw_type) {
    const auto array = ParseFixedArrayType(raw_type);
    return TrimType(array.length ? array.base_type : raw_type);
}

uint32_t PrimitiveTypeSize(const std::string& raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") {
        return sizeof(bool);
    }
    if (type == "Char_t" || type == "char" || type == "b") {
        return sizeof(int8_t);
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B") {
        return sizeof(uint8_t);
    }
    if (type == "Short_t" || type == "short" || type == "S") {
        return sizeof(int16_t);
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s") {
        return sizeof(uint16_t);
    }
    if (type == "Int_t" || type == "int" || type == "I") {
        return sizeof(int32_t);
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i") {
        return sizeof(uint32_t);
    }
    if (type == "Long_t" || type == "long") {
        return sizeof(long);
    }
    if (type == "ULong_t" || type == "unsigned long") {
        return sizeof(unsigned long);
    }
    if (type == "Long64_t" || type == "long long" || type == "L") {
        return sizeof(int64_t);
    }
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l") {
        return sizeof(uint64_t);
    }
    if (type == "Float_t" || type == "float" || type == "F") {
        return sizeof(float);
    }
    if (type == "Double_t" || type == "double" || type == "D") {
        return sizeof(double);
    }
    return 0;
}

std::string ArrayDimensionsText(const std::vector<uint32_t>& dimensions) {
    std::string result;
    for (idx_t i = 0; i < dimensions.size(); ++i) {
        if (i) {
            result += 'x';
        }
        result += std::to_string(dimensions[i]);
    }
    return result;
}

bool IsPrimitiveType(const std::string& raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    static const std::vector<std::string> primitive_types = {
        "Bool_t",   "bool",           "Char_t",   "char",      "UChar_t",   "unsigned char",      "Short_t", "short",
        "UShort_t", "unsigned short", "Int_t",    "int",       "UInt_t",    "unsigned int",       "Long_t",  "long",
        "ULong_t",  "unsigned long",  "Long64_t", "long long", "ULong64_t", "unsigned long long", "Float_t", "float",
        "Double_t", "double"};
    return std::find(primitive_types.begin(), primitive_types.end(), type) != primitive_types.end();
}

bool IsStringType(const std::string& raw_type) {
    const auto type = TrimType(raw_type);
    return type == "string" || type == "TString";
}

std::string ExtractInnerType(const std::string& container_type) {
    const auto arguments = TemplateArguments(container_type);
    return arguments.empty() ? std::string() : arguments.front();
}

LogicalType RootTypeToLogicalType(const std::string& raw_type) {
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") {
        return LogicalType::BOOLEAN;
    }
    if (type == "Char_t" || type == "char" || type == "b") {
        return LogicalType::TINYINT;
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B") {
        return LogicalType::UTINYINT;
    }
    if (type == "Short_t" || type == "short" || type == "S") {
        return LogicalType::SMALLINT;
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s") {
        return LogicalType::USMALLINT;
    }
    if (type == "Int_t" || type == "int" || type == "I") {
        return LogicalType::INTEGER;
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i") {
        return LogicalType::UINTEGER;
    }
    if (type == "Long_t" || type == "long" || type == "Long64_t" || type == "long long" || type == "L") {
        return LogicalType::BIGINT;
    }
    if (type == "ULong_t" || type == "unsigned long" || type == "ULong64_t" || type == "unsigned long long" ||
        type == "l") {
        return LogicalType::UBIGINT;
    }
    if (type == "Float_t" || type == "float" || type == "F") {
        return LogicalType::FLOAT;
    }
    if (type == "Double_t" || type == "double" || type == "D") {
        return LogicalType::DOUBLE;
    }
    if (IsStringType(type)) {
        return LogicalType::VARCHAR;
    }
    throw NotImplementedException("Unsupported ROOT leaf type: " + raw_type);
}

LogicalType RootTypeToScanLogicalType(const std::string& raw_type, bool is_string, bool is_primitive) {
    if (is_string || !is_primitive) {
        return LogicalType::VARCHAR;
    }

    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") {
        return LogicalType::BOOLEAN;
    }
    if (type == "Char_t" || type == "char" || type == "b") {
        return LogicalType::TINYINT;
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B") {
        return LogicalType::UTINYINT;
    }
    if (type == "Short_t" || type == "short" || type == "S") {
        return LogicalType::SMALLINT;
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s") {
        return LogicalType::USMALLINT;
    }
    if (type == "Int_t" || type == "int" || type == "I") {
        return LogicalType::INTEGER;
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i") {
        return LogicalType::UINTEGER;
    }
    if (type == "Long_t" || type == "long" || type == "Long64_t" || type == "long long" || type == "L") {
        return LogicalType::BIGINT;
    }
    if (type == "ULong_t" || type == "unsigned long" || type == "ULong64_t" || type == "unsigned long long" ||
        type == "l") {
        return LogicalType::UBIGINT;
    }
    if (type == "Float_t" || type == "float" || type == "F") {
        return LogicalType::FLOAT;
    }
    if (type == "Double_t" || type == "double" || type == "D") {
        return LogicalType::DOUBLE;
    }
    return LogicalType::VARCHAR;
}

bool IsLosslessDoubleBackedType(const std::string& raw_type) {
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

double ReadPrimitiveAsDouble(void* pointer, const std::string& raw_type) {
    return RootPrimitiveValue::FromPointer(pointer, raw_type).AsDouble();
}

} // namespace duckdb::rootlake
