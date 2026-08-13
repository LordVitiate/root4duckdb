#include "root4duckdb/index/root_filter.hpp"

namespace duckdb::rootlake {

RootScalarActual RootScalarActual::Null(const LogicalType& type) {
    RootScalarActual result;
    result.type = type;
    result.is_null = true;
    return result;
}

RootScalarActual RootScalarActual::Event(uint64_t value) {
    RootScalarActual result;
    result.type = LogicalType::UBIGINT;
    result.unsigned_value = value;
    return result;
}

RootScalarActual RootScalarActual::Signed(int64_t value, const LogicalType& logical_type) {
    RootScalarActual result;
    result.type = logical_type;
    result.signed_value = value;
    result.unsigned_value = static_cast<uint64_t>(value);
    result.numeric = static_cast<double>(value);
    return result;
}

RootScalarActual RootScalarActual::Unsigned(uint64_t value, const LogicalType& logical_type) {
    RootScalarActual result;
    result.type = logical_type;
    result.unsigned_value = value;
    result.numeric = static_cast<double>(value);
    return result;
}

RootScalarActual RootScalarActual::Index(std::optional<int32_t> value) {
    RootScalarActual result;
    result.type = LogicalType::INTEGER;
    result.is_null = !value.has_value();
    result.signed_value = value.value_or(0);
    return result;
}

RootScalarActual RootScalarActual::String(std::string value) {
    RootScalarActual result;
    result.type = LogicalType::VARCHAR;
    result.string_value = std::move(value);
    return result;
}

RootScalarActual RootScalarActual::Numeric(const LogicalType& logical_type, double value) {
    RootScalarActual result;
    result.type = logical_type;
    result.numeric = value;
    result.signed_value = static_cast<int64_t>(value);
    result.unsigned_value = static_cast<uint64_t>(value);
    return result;
}

Value RootScalarActual::ToValue() const {
    if (is_null) {
        return Value(type);
    }
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        return Value::BOOLEAN(numeric != 0);
    case LogicalTypeId::TINYINT:
        return Value::TINYINT(static_cast<int8_t>(signed_value));
    case LogicalTypeId::UTINYINT:
        return Value::UTINYINT(static_cast<uint8_t>(unsigned_value));
    case LogicalTypeId::SMALLINT:
        return Value::SMALLINT(static_cast<int16_t>(signed_value));
    case LogicalTypeId::USMALLINT:
        return Value::USMALLINT(static_cast<uint16_t>(unsigned_value));
    case LogicalTypeId::INTEGER:
        return Value::INTEGER(static_cast<int32_t>(signed_value));
    case LogicalTypeId::UINTEGER:
        return Value::UINTEGER(static_cast<uint32_t>(unsigned_value));
    case LogicalTypeId::BIGINT:
        return Value::BIGINT(signed_value);
    case LogicalTypeId::UBIGINT:
        return Value::UBIGINT(unsigned_value);
    case LogicalTypeId::FLOAT:
        return Value::FLOAT(static_cast<float>(numeric));
    case LogicalTypeId::DOUBLE:
        return Value::DOUBLE(numeric);
    case LogicalTypeId::VARCHAR:
        return Value(string_value);
    default:
        return Value::DOUBLE(numeric).DefaultCastAs(type);
    }
}

} // namespace duckdb::rootlake
