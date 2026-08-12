#pragma once

#include "root_serialized_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace duckdb::rootlake::serialized_detail {

constexpr uint32_t kRootByteCountMask = 0x40000000U;
constexpr uint32_t kRootByteCountValueMask = 0x3fffffffU;
constexpr size_t kOuterMemberWiseHeaderBytes = 12;
constexpr size_t kStlVectorHeaderBytes = 10;

inline uint16_t ReadBE16(const uint8_t *ptr) {
    return static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8U) |
                                 static_cast<uint16_t>(ptr[1]));
}

inline uint32_t ReadBE32(const uint8_t *ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24U) |
           (static_cast<uint32_t>(ptr[1]) << 16U) |
           (static_cast<uint32_t>(ptr[2]) << 8U) |
           static_cast<uint32_t>(ptr[3]);
}

inline uint64_t ReadBE64(const uint8_t *ptr) {
    return (static_cast<uint64_t>(ReadBE32(ptr)) << 32U) |
           static_cast<uint64_t>(ReadBE32(ptr + 4));
}

inline uint32_t PrimitiveKindWidth(SerializedPrimitiveKind kind) {
    switch (kind) {
    case SerializedPrimitiveKind::BOOL:
    case SerializedPrimitiveKind::INT8:
    case SerializedPrimitiveKind::UINT8: return 1;
    case SerializedPrimitiveKind::INT16:
    case SerializedPrimitiveKind::UINT16: return 2;
    case SerializedPrimitiveKind::INT32:
    case SerializedPrimitiveKind::UINT32:
    case SerializedPrimitiveKind::FLOAT32: return 4;
    case SerializedPrimitiveKind::INT64:
    case SerializedPrimitiveKind::UINT64:
    case SerializedPrimitiveKind::FLOAT64: return 8;
    case SerializedPrimitiveKind::UNKNOWN: return 0;
    }
    return 0;
}

inline bool DecodePrimitive(const uint8_t *ptr, SerializedPrimitiveKind kind, double &value) {
    switch (kind) {
    case SerializedPrimitiveKind::BOOL:
        value = ptr[0] ? 1.0 : 0.0;
        return true;
    case SerializedPrimitiveKind::INT8:
        value = static_cast<double>(static_cast<int8_t>(ptr[0]));
        return true;
    case SerializedPrimitiveKind::UINT8:
        value = static_cast<double>(ptr[0]);
        return true;
    case SerializedPrimitiveKind::INT16: {
        const auto bits = static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8U) | ptr[1]);
        value = static_cast<double>(static_cast<int16_t>(bits));
        return true;
    }
    case SerializedPrimitiveKind::UINT16:
        value = static_cast<double>(static_cast<uint16_t>(
            (static_cast<uint16_t>(ptr[0]) << 8U) | ptr[1]));
        return true;
    case SerializedPrimitiveKind::INT32:
        value = static_cast<double>(static_cast<int32_t>(ReadBE32(ptr)));
        return true;
    case SerializedPrimitiveKind::UINT32:
        value = static_cast<double>(ReadBE32(ptr));
        return true;
    case SerializedPrimitiveKind::INT64:
        value = static_cast<double>(static_cast<int64_t>(ReadBE64(ptr)));
        return true;
    case SerializedPrimitiveKind::UINT64:
        value = static_cast<double>(ReadBE64(ptr));
        return true;
    case SerializedPrimitiveKind::FLOAT32: {
        const auto bits = ReadBE32(ptr);
        float decoded;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        value = static_cast<double>(decoded);
        return true;
    }
    case SerializedPrimitiveKind::FLOAT64: {
        const auto bits = ReadBE64(ptr);
        double decoded;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        value = decoded;
        return true;
    }
    case SerializedPrimitiveKind::UNKNOWN: return false;
    }
    return false;
}

inline bool ReadByteCountedObjectSize(const uint8_t *bytes, size_t available,
                                      size_t minimum_size, size_t &declared_size,
                                      std::string &failure_reason) {
    if (!bytes || available < 4) {
        failure_reason = "serialized byte-counted object is truncated before its byte count";
        return false;
    }
    const uint32_t byte_count = ReadBE32(bytes);
    if ((byte_count & kRootByteCountMask) == 0) {
        failure_reason = "serialized nested object has no ROOT byte-count marker";
        return false;
    }
    const uint64_t declared = static_cast<uint64_t>(byte_count & kRootByteCountValueMask) + 4;
    if (declared < minimum_size || declared > available ||
        declared > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        failure_reason = "serialized nested object byte count is invalid";
        return false;
    }
    declared_size = static_cast<size_t>(declared);
    return true;
}

bool DecodeNestedPrimitiveVector(const uint8_t *bytes, size_t entry_size, size_t &cursor,
                                 uint64_t outer_count, const SerializedEntryLayout &layout,
                                 SerializedPrimitiveKind primitive_kind,
                                 uint64_t max_values_per_entry,
                                 std::vector<double> &values,
                                 std::vector<int32_t> &flat_indices,
                                 std::string &failure_reason,
                                 bool collect_indices);

} // namespace duckdb::rootlake::serialized_detail
