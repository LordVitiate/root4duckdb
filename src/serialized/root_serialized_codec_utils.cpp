#include "root4duckdb/serialized/root_serialized_codec_utils.hpp"

#include <cstring>

namespace duckdb::rootlake::serialized_codec {

uint16_t ReadBE16(const uint8_t* pointer) {
    return static_cast<uint16_t>((static_cast<uint16_t>(pointer[0]) << 8U) | static_cast<uint16_t>(pointer[1]));
}

uint32_t ReadBE32(const uint8_t* pointer) {
    return (static_cast<uint32_t>(pointer[0]) << 24U) | (static_cast<uint32_t>(pointer[1]) << 16U) |
           (static_cast<uint32_t>(pointer[2]) << 8U) | static_cast<uint32_t>(pointer[3]);
}

uint64_t ReadBE64(const uint8_t* pointer) {
    return (static_cast<uint64_t>(ReadBE32(pointer)) << 32U) | static_cast<uint64_t>(ReadBE32(pointer + 4));
}

uint32_t PrimitiveWidth(SerializedPrimitiveKind kind) {
    switch (kind) {
    case SerializedPrimitiveKind::BOOL:
    case SerializedPrimitiveKind::INT8:
    case SerializedPrimitiveKind::UINT8:
        return 1;
    case SerializedPrimitiveKind::INT16:
    case SerializedPrimitiveKind::UINT16:
        return 2;
    case SerializedPrimitiveKind::INT32:
    case SerializedPrimitiveKind::UINT32:
    case SerializedPrimitiveKind::FLOAT32:
        return 4;
    case SerializedPrimitiveKind::INT64:
    case SerializedPrimitiveKind::UINT64:
    case SerializedPrimitiveKind::FLOAT64:
        return 8;
    case SerializedPrimitiveKind::UNKNOWN:
        return 0;
    }
    return 0;
}

bool DecodePrimitive(const uint8_t* pointer, SerializedPrimitiveKind kind, double& value) {
    switch (kind) {
    case SerializedPrimitiveKind::BOOL:
        value = pointer[0] ? 1.0 : 0.0;
        return true;
    case SerializedPrimitiveKind::INT8:
        value = static_cast<double>(static_cast<int8_t>(pointer[0]));
        return true;
    case SerializedPrimitiveKind::UINT8:
        value = static_cast<double>(pointer[0]);
        return true;
    case SerializedPrimitiveKind::INT16:
        value = static_cast<double>(static_cast<int16_t>(ReadBE16(pointer)));
        return true;
    case SerializedPrimitiveKind::UINT16:
        value = static_cast<double>(ReadBE16(pointer));
        return true;
    case SerializedPrimitiveKind::INT32:
        value = static_cast<double>(static_cast<int32_t>(ReadBE32(pointer)));
        return true;
    case SerializedPrimitiveKind::UINT32:
        value = static_cast<double>(ReadBE32(pointer));
        return true;
    case SerializedPrimitiveKind::INT64:
        value = static_cast<double>(static_cast<int64_t>(ReadBE64(pointer)));
        return true;
    case SerializedPrimitiveKind::UINT64:
        value = static_cast<double>(ReadBE64(pointer));
        return true;
    case SerializedPrimitiveKind::FLOAT32: {
        const auto bits = ReadBE32(pointer);
        float decoded = 0.0F;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        value = static_cast<double>(decoded);
        return true;
    }
    case SerializedPrimitiveKind::FLOAT64: {
        const auto bits = ReadBE64(pointer);
        double decoded = 0.0;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        value = decoded;
        return true;
    }
    case SerializedPrimitiveKind::UNKNOWN:
        return false;
    }
    return false;
}

std::string NormalizePrimitiveType(std::string type) {
    const auto first = type.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = type.find_last_not_of(" \t\r\n");
    type = type.substr(first, last - first + 1);
    if (type.rfind("std::", 0) == 0) {
        type.erase(0, 5);
    }
    return type;
}

} // namespace duckdb::rootlake::serialized_codec
