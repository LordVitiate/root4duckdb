#pragma once

#include "root4duckdb/serialized/root_serialized_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb::rootlake::serialized_codec {

/// ROOT marker indicating a framed byte count.
constexpr uint32_t ROOT_BYTE_COUNT_MASK = 0x40000000U;
/// Mask extracting the payload length from a framed byte count.
constexpr uint32_t ROOT_BYTE_COUNT_VALUE_MASK = 0x3fffffffU;

/// Bounds-checked cursor for untrusted serialized ROOT bytes.
class CheckedByteCursor final {
  public:
    CheckedByteCursor(const uint8_t* data, size_t size) noexcept;

    [[nodiscard]] size_t Offset() const noexcept;
    [[nodiscard]] size_t Remaining() const noexcept;
    [[nodiscard]] const uint8_t* Current() const noexcept;
    bool ReadU8(uint8_t& value) noexcept;
    bool ReadBE16(uint16_t& value) noexcept;
    bool ReadBE32(uint32_t& value) noexcept;
    bool ReadBE64(uint64_t& value) noexcept;
    bool Take(size_t size, const uint8_t*& begin) noexcept;
    bool Skip(size_t size) noexcept;

  private:
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t offset = 0;
};

/// Reads an unsigned big-endian 16-bit integer.
/// The caller must already have validated the input span.
uint16_t ReadBE16(const uint8_t* pointer);
/// Reads an unsigned big-endian 32-bit integer from an already validated span.
uint32_t ReadBE32(const uint8_t* pointer);
/// Reads an unsigned big-endian 64-bit integer from an already validated span.
uint64_t ReadBE64(const uint8_t* pointer);
/// Returns the serialized width of a supported primitive.
uint32_t PrimitiveWidth(SerializedPrimitiveKind kind);
/// Decodes one primitive into the common numeric representation.
bool DecodePrimitive(const uint8_t* pointer, SerializedPrimitiveKind kind, double& value);
/// Decodes one primitive without routing 64-bit integers through double.
bool DecodePrimitiveExact(const uint8_t* pointer, SerializedPrimitiveKind kind, RootPrimitiveValue& value);
/// Normalizes ROOT and C++ primitive type spelling.
std::string NormalizePrimitiveType(std::string type);

} // namespace duckdb::rootlake::serialized_codec
