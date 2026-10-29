#include "root4duckdb/reader/root_primitive_value.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace duckdb::rootlake
{

RootPrimitiveValue RootPrimitiveValue::Signed(int64_t value)
{
    RootPrimitiveValue result;
    result.kind = RootPrimitiveKind::SIGNED;
    result.signed_value = value;
    return result;
}

RootPrimitiveValue RootPrimitiveValue::Unsigned(uint64_t value)
{
    RootPrimitiveValue result;
    result.kind = RootPrimitiveKind::UNSIGNED;
    result.unsigned_value = value;
    return result;
}

RootPrimitiveValue RootPrimitiveValue::Floating(double value)
{
    RootPrimitiveValue result;
    result.kind = RootPrimitiveKind::FLOATING;
    result.floating_value = value;
    return result;
}

int64_t RootPrimitiveValue::AsSigned() const
{
    switch (kind)
    {
    case RootPrimitiveKind::SIGNED:
        return signed_value;
    case RootPrimitiveKind::UNSIGNED:
        if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            throw std::out_of_range("ROOT unsigned primitive cannot be represented as a signed integer");
        }
        return static_cast<int64_t>(unsigned_value);
    case RootPrimitiveKind::FLOATING:
        // The upper bound is exclusive: double(INT64_MAX) rounds to 2^63.
        if (!std::isfinite(floating_value) || floating_value < -std::ldexp(1.0, 63) ||
            floating_value >= std::ldexp(1.0, 63))
        {
            throw std::out_of_range("ROOT floating primitive cannot be represented as a signed integer");
        }
        return static_cast<int64_t>(floating_value);
    }
    return 0;
}

uint64_t RootPrimitiveValue::AsUnsigned() const
{
    switch (kind)
    {
    case RootPrimitiveKind::SIGNED:
        return static_cast<uint64_t>(signed_value);
    case RootPrimitiveKind::UNSIGNED:
        return unsigned_value;
    case RootPrimitiveKind::FLOATING:
        if (!std::isfinite(floating_value) || std::trunc(floating_value) < 0.0 || floating_value >= std::ldexp(1.0, 64))
        {
            throw std::out_of_range("ROOT floating primitive cannot be represented as an unsigned integer");
        }
        return static_cast<uint64_t>(floating_value);
    }
    return 0;
}

template <class T> T RootPrimitiveValue::AsInteger() const
{
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    if constexpr (std::is_signed_v<T>)
    {
        const auto number = AsSigned();
        if (number < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
            number > static_cast<int64_t>(std::numeric_limits<T>::max()))
        {
            throw std::out_of_range("ROOT primitive exceeds its signed SQL integer target range");
        }
        return static_cast<T>(number);
    }
    else
    {
        const auto number = AsUnsigned();
        if (number > static_cast<uint64_t>(std::numeric_limits<T>::max()))
        {
            throw std::out_of_range("ROOT primitive exceeds its unsigned SQL integer target range");
        }
        return static_cast<T>(number);
    }
}

// Keep the conversion implementation out of the public header and instantiate
// exactly the integer SQL transport types needed by the two scan executors.
template int8_t RootPrimitiveValue::AsInteger<int8_t>() const;
template uint8_t RootPrimitiveValue::AsInteger<uint8_t>() const;
template int16_t RootPrimitiveValue::AsInteger<int16_t>() const;
template uint16_t RootPrimitiveValue::AsInteger<uint16_t>() const;
template int32_t RootPrimitiveValue::AsInteger<int32_t>() const;
template uint32_t RootPrimitiveValue::AsInteger<uint32_t>() const;
template int64_t RootPrimitiveValue::AsInteger<int64_t>() const;
template uint64_t RootPrimitiveValue::AsInteger<uint64_t>() const;

double RootPrimitiveValue::AsDouble() const
{
    switch (kind)
    {
    case RootPrimitiveKind::SIGNED:
        return static_cast<double>(signed_value);
    case RootPrimitiveKind::UNSIGNED:
        return static_cast<double>(unsigned_value);
    case RootPrimitiveKind::FLOATING:
        return floating_value;
    }
    return 0.0;
}

float RootPrimitiveValue::AsFloat() const
{
    const auto value = AsDouble();
    if (std::isfinite(value) && (value > static_cast<double>(std::numeric_limits<float>::max()) ||
                                 value < -static_cast<double>(std::numeric_limits<float>::max())))
    {
        throw std::out_of_range("ROOT primitive exceeds its SQL FLOAT target range");
    }
    return static_cast<float>(value);
}

bool RootPrimitiveValue::AsBool() const
{
    switch (kind)
    {
    case RootPrimitiveKind::SIGNED:
        return signed_value != 0;
    case RootPrimitiveKind::UNSIGNED:
        return unsigned_value != 0;
    case RootPrimitiveKind::FLOATING:
        return floating_value != 0.0;
    }
    return false;
}

} // namespace duckdb::rootlake
