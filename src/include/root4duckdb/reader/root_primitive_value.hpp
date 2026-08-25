#pragma once

#include <cstdint>
#include <string>

namespace duckdb::rootlake {

enum class RootPrimitiveKind : uint8_t { SIGNED, UNSIGNED, FLOATING };

/// Exact numeric transport shared by object and serialized readers.
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

} // namespace duckdb::rootlake
