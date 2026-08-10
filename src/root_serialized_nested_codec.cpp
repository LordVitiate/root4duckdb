#include "root_serialized_codec.hpp"

#include <cstring>
#include <limits>

namespace duckdb::rootlake {

namespace {

constexpr uint32_t kRootByteCountMask = 0x40000000U;
constexpr uint32_t kRootByteCountValueMask = 0x3fffffffU;

uint16_t ReadBE16(const uint8_t *ptr) {
    return static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8U) | ptr[1]);
}

uint32_t ReadBE32(const uint8_t *ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24U) |
           (static_cast<uint32_t>(ptr[1]) << 16U) |
           (static_cast<uint32_t>(ptr[2]) << 8U) |
           static_cast<uint32_t>(ptr[3]);
}

uint64_t ReadBE64(const uint8_t *ptr) {
    return (static_cast<uint64_t>(ReadBE32(ptr)) << 32U) | ReadBE32(ptr + 4);
}

uint32_t PrimitiveWidth(SerializedPrimitiveKind kind) {
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

bool DecodePrimitive(const uint8_t *ptr, SerializedPrimitiveKind kind, double &value) {
    switch (kind) {
    case SerializedPrimitiveKind::BOOL: value = ptr[0] ? 1.0 : 0.0; return true;
    case SerializedPrimitiveKind::INT8:
        value = static_cast<double>(static_cast<int8_t>(ptr[0])); return true;
    case SerializedPrimitiveKind::UINT8: value = ptr[0]; return true;
    case SerializedPrimitiveKind::INT16:
        value = static_cast<int16_t>(ReadBE16(ptr)); return true;
    case SerializedPrimitiveKind::UINT16: value = ReadBE16(ptr); return true;
    case SerializedPrimitiveKind::INT32:
        value = static_cast<int32_t>(ReadBE32(ptr)); return true;
    case SerializedPrimitiveKind::UINT32: value = ReadBE32(ptr); return true;
    case SerializedPrimitiveKind::INT64:
        value = static_cast<double>(static_cast<int64_t>(ReadBE64(ptr))); return true;
    case SerializedPrimitiveKind::UINT64:
        value = static_cast<double>(ReadBE64(ptr)); return true;
    case SerializedPrimitiveKind::FLOAT32: {
        const uint32_t bits = ReadBE32(ptr);
        float decoded;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        value = decoded;
        return true;
    }
    case SerializedPrimitiveKind::FLOAT64: {
        const uint64_t bits = ReadBE64(ptr);
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    case SerializedPrimitiveKind::UNKNOWN: return false;
    }
    return false;
}

bool ValidateLayout(const SerializedEntryLayout &layout,
                    SerializedPrimitiveKind &kind, std::string &reason) {
    kind = layout.primitive_kind == SerializedPrimitiveKind::UNKNOWN
               ? ClassifySerializedPrimitive(layout.value_type)
               : layout.primitive_kind;
    if (!layout.value_bytes || kind == SerializedPrimitiveKind::UNKNOWN ||
        PrimitiveWidth(kind) != layout.value_bytes) {
        reason = "serialized primitive type and width are inconsistent";
        return false;
    }
    if (layout.fixed_array_length != 1 || !layout.array_dimensions.empty() ||
        layout.index_depth != 2) {
        reason = "nested serialized vector requires scalar primitives and two indices";
        return false;
    }
    return true;
}

void Fail(std::vector<double> &values, std::vector<int32_t> &indices,
          std::string &reason, const char *message) {
    values.clear();
    indices.clear();
    reason = message;
}

} // namespace

bool DecodeSerializedNestedPrimitiveVectorColumn(
    const uint8_t *bytes, size_t entry_size, size_t column_offset,
    uint64_t outer_count, const SerializedEntryLayout &layout,
    uint64_t max_values_per_entry, std::vector<double> &values,
    std::vector<int32_t> &flat_indices, std::string &failure_reason,
    bool collect_indices) {
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    SerializedPrimitiveKind kind = SerializedPrimitiveKind::UNKNOWN;
    if (!ValidateLayout(layout, kind, failure_reason)) return false;
    if (!bytes || column_offset > entry_size || entry_size - column_offset < 6) {
        failure_reason = "serialized nested vector column is shorter than its ROOT header";
        return false;
    }
    if (outer_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        outer_count > max_values_per_entry) {
        failure_reason = "serialized outer vector count exceeds configured safety limit";
        return false;
    }
    const uint32_t byte_count = ReadBE32(bytes + column_offset);
    if ((byte_count & kRootByteCountMask) == 0) {
        failure_reason = "serialized nested vector column has no ROOT byte-count marker";
        return false;
    }
    const uint64_t declared_size = (byte_count & kRootByteCountValueMask) + 4ULL;
    if (declared_size < 6) {
        failure_reason =
            "serialized nested vector column byte-count is shorter than its ROOT header";
        return false;
    }
    if (declared_size > entry_size - column_offset) {
        failure_reason = "serialized nested vector column byte-count exceeds its entry";
        return false;
    }
    const size_t column_end = column_offset + static_cast<size_t>(declared_size);
    size_t cursor = column_offset + 6;
    if (ReadBE16(bytes + column_offset + 4) == 0) {
        if (column_end - cursor < 4) {
            failure_reason = "serialized nested vector column checksum is truncated";
            return false;
        }
        cursor += 4;
    }
    uint64_t total_values = 0;
    for (uint64_t outer = 0; outer < outer_count; ++outer) {
        if (column_end - cursor < 4) {
            Fail(values, flat_indices, failure_reason,
                 "serialized nested vector length is truncated");
            return false;
        }
        const int32_t signed_count = static_cast<int32_t>(ReadBE32(bytes + cursor));
        cursor += 4;
        if (signed_count < 0) {
            Fail(values, flat_indices, failure_reason,
                 "serialized nested vector has a negative element count");
            return false;
        }
        const uint64_t inner_count = static_cast<uint32_t>(signed_count);
        if (inner_count > max_values_per_entry - total_values ||
            inner_count > std::numeric_limits<uint64_t>::max() / layout.value_bytes ||
            total_values + inner_count > std::numeric_limits<size_t>::max()) {
            Fail(values, flat_indices, failure_reason,
                 "serialized nested vector value count exceeds configured safety limit");
            return false;
        }
        const uint64_t payload_bytes = inner_count * layout.value_bytes;
        if (payload_bytes > column_end - cursor) {
            Fail(values, flat_indices, failure_reason,
                 "serialized nested vector payload is truncated");
            return false;
        }
        if (collect_indices &&
            inner_count > (std::numeric_limits<size_t>::max() - flat_indices.size()) / 2) {
            Fail(values, flat_indices, failure_reason,
                 "serialized nested vector index size arithmetic overflow");
            return false;
        }
        values.reserve(static_cast<size_t>(total_values + inner_count));
        if (collect_indices) flat_indices.reserve(flat_indices.size() + inner_count * 2);
        for (uint64_t inner = 0; inner < inner_count; ++inner) {
            double value = 0;
            if (!DecodePrimitive(bytes + cursor + inner * layout.value_bytes, kind, value)) {
                Fail(values, flat_indices, failure_reason,
                     "unsupported primitive decoder for nested serialized vector");
                return false;
            }
            values.push_back(value);
            if (collect_indices) {
                flat_indices.push_back(static_cast<int32_t>(outer));
                flat_indices.push_back(static_cast<int32_t>(inner));
            }
        }
        cursor += static_cast<size_t>(payload_bytes);
        total_values += inner_count;
    }
    if (cursor != column_end) {
        Fail(values, flat_indices, failure_reason,
             "serialized nested vector column byte-count does not match decoded values");
        return false;
    }
    return true;
}

} // namespace duckdb::rootlake
