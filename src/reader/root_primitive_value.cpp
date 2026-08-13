#include "root4duckdb/reader/root_semantic_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace duckdb::rootlake {

RootPrimitiveValue RootPrimitiveValue::Signed(int64_t value) {
    RootPrimitiveValue result;
    result.kind = RootPrimitiveKind::SIGNED;
    result.signed_value = value;
    return result;
}

RootPrimitiveValue RootPrimitiveValue::Unsigned(uint64_t value) {
    RootPrimitiveValue result;
    result.kind = RootPrimitiveKind::UNSIGNED;
    result.unsigned_value = value;
    return result;
}

RootPrimitiveValue RootPrimitiveValue::Floating(double value) {
    RootPrimitiveValue result;
    result.kind = RootPrimitiveKind::FLOATING;
    result.floating_value = value;
    return result;
}

RootPrimitiveValue RootPrimitiveValue::FromPointer(void* pointer, const std::string& raw_type) {
    if (!pointer) {
        return Floating(0.0);
    }

    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") {
        return Unsigned(*reinterpret_cast<bool*>(pointer) ? 1 : 0);
    }
    if (type == "Char_t" || type == "char" || type == "b") {
        return Signed(*reinterpret_cast<int8_t*>(pointer));
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B") {
        return Unsigned(*reinterpret_cast<uint8_t*>(pointer));
    }
    if (type == "Short_t" || type == "short" || type == "S") {
        return Signed(*reinterpret_cast<int16_t*>(pointer));
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s") {
        return Unsigned(*reinterpret_cast<uint16_t*>(pointer));
    }
    if (type == "Int_t" || type == "int" || type == "I") {
        return Signed(*reinterpret_cast<int32_t*>(pointer));
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i") {
        return Unsigned(*reinterpret_cast<uint32_t*>(pointer));
    }
    if (type == "Long_t" || type == "long") {
        return Signed(static_cast<int64_t>(*reinterpret_cast<long*>(pointer)));
    }
    if (type == "ULong_t" || type == "unsigned long") {
        return Unsigned(static_cast<uint64_t>(*reinterpret_cast<unsigned long*>(pointer)));
    }
    if (type == "Long64_t" || type == "long long" || type == "L") {
        return Signed(*reinterpret_cast<int64_t*>(pointer));
    }
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l") {
        return Unsigned(*reinterpret_cast<uint64_t*>(pointer));
    }
    if (type == "Float_t" || type == "float" || type == "F") {
        return Floating(*reinterpret_cast<float*>(pointer));
    }
    if (type == "Double_t" || type == "double" || type == "D") {
        return Floating(*reinterpret_cast<double*>(pointer));
    }
    throw NotImplementedException("Unsupported primitive ROOT type: " + raw_type);
}

RootPrimitiveValue RootPrimitiveValue::FromDouble(double value, const std::string& raw_type) {
    switch (RootTypeToLogicalType(raw_type).id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
        return Unsigned(static_cast<uint64_t>(value));
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
        return Signed(static_cast<int64_t>(value));
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        return Floating(value);
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UBIGINT:
        throw InternalException("Lossy double-to-64-bit ROOT primitive conversion was attempted");
    default:
        throw NotImplementedException("Unsupported ROOT primitive type: " + raw_type);
    }
}

int64_t RootPrimitiveValue::AsSigned() const {
    switch (kind) {
    case RootPrimitiveKind::SIGNED:
        return signed_value;
    case RootPrimitiveKind::UNSIGNED:
        return static_cast<int64_t>(unsigned_value);
    case RootPrimitiveKind::FLOATING:
        return static_cast<int64_t>(floating_value);
    }
    return 0;
}

uint64_t RootPrimitiveValue::AsUnsigned() const {
    switch (kind) {
    case RootPrimitiveKind::SIGNED:
        return static_cast<uint64_t>(signed_value);
    case RootPrimitiveKind::UNSIGNED:
        return unsigned_value;
    case RootPrimitiveKind::FLOATING:
        return static_cast<uint64_t>(floating_value);
    }
    return 0;
}

double RootPrimitiveValue::AsDouble() const {
    switch (kind) {
    case RootPrimitiveKind::SIGNED:
        return static_cast<double>(signed_value);
    case RootPrimitiveKind::UNSIGNED:
        return static_cast<double>(unsigned_value);
    case RootPrimitiveKind::FLOATING:
        return floating_value;
    }
    return 0.0;
}

bool RootPrimitiveValue::AsBool() const {
    switch (kind) {
    case RootPrimitiveKind::SIGNED:
        return signed_value != 0;
    case RootPrimitiveKind::UNSIGNED:
        return unsigned_value != 0;
    case RootPrimitiveKind::FLOATING:
        return floating_value != 0.0;
    }
    return false;
}

size_t ReadResult::size() const {
    return strings.size();
}

bool ReadResult::empty() const {
    return strings.empty();
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

void ReadResult::AddString(const std::string& value, int64_t event_id, const std::vector<int32_t>& indices,
                           const std::vector<std::string>& index_names) {
    strings.push_back(value);
    numbers.emplace_back();
    is_string_flag.push_back(true);
    event_ids.push_back(event_id);
    vector_indices.emplace_back(indices.begin(), indices.end());
    if (vector_names.empty()) {
        vector_names = index_names;
    }
}

void ReadResult::AddNumber(const RootPrimitiveValue& value, int64_t event_id, const std::vector<int32_t>& indices,
                           const std::vector<std::string>& index_names) {
    strings.emplace_back();
    numbers.push_back(value);
    is_string_flag.push_back(false);
    event_ids.push_back(event_id);
    vector_indices.emplace_back(indices.begin(), indices.end());
    if (vector_names.empty()) {
        vector_names = index_names;
    }
}

void ReadResult::AddNumber(double value, int64_t event_id, const std::vector<int32_t>& indices,
                           const std::vector<std::string>& index_names) {
    AddNumber(RootPrimitiveValue::Floating(value), event_id, indices, index_names);
}

} // namespace duckdb::rootlake
