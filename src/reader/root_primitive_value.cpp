#include "root4duckdb/reader/root_primitive_value.hpp"

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
        return static_cast<int64_t>(unsigned_value);
    case RootPrimitiveKind::FLOATING:
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
        return static_cast<uint64_t>(floating_value);
    }
    return 0;
}

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
