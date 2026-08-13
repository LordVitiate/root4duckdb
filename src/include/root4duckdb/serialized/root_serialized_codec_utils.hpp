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

/// Reads an unsigned big-endian 16-bit integer.
uint16_t ReadBE16(const uint8_t* pointer);
/// Reads an unsigned big-endian 32-bit integer.
uint32_t ReadBE32(const uint8_t* pointer);
/// Reads an unsigned big-endian 64-bit integer.
uint64_t ReadBE64(const uint8_t* pointer);
/// Returns the serialized width of a supported primitive.
uint32_t PrimitiveWidth(SerializedPrimitiveKind kind);
/// Decodes one primitive into the common numeric representation.
bool DecodePrimitive(const uint8_t* pointer, SerializedPrimitiveKind kind, double& value);
/// Normalizes ROOT and C++ primitive type spelling.
std::string NormalizePrimitiveType(std::string type);

} // namespace duckdb::rootlake::serialized_codec
