#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake {

enum class SerializedPrimitiveKind : uint8_t {
    UNKNOWN = 0,
    BOOL,
    INT8,
    UINT8,
    INT16,
    UINT16,
    INT32,
    UINT32,
    INT64,
    UINT64,
    FLOAT32,
    FLOAT64
};

SerializedPrimitiveKind ClassifySerializedPrimitive(const std::string &type);

struct SerializedEntryLayout {
    std::string value_type;
    SerializedPrimitiveKind primitive_kind = SerializedPrimitiveKind::UNKNOWN;
    uint32_t bytes_before_value_per_element = 0;
    uint32_t value_bytes = 0;
    uint64_t fixed_array_length = 1;
    std::vector<uint32_t> array_dimensions;
    size_t index_depth = 0;
};

bool DecodeSerializedVectorEntry(const uint8_t *bytes, size_t entry_size,
                                 const SerializedEntryLayout &layout,
                                 uint64_t max_values_per_entry,
                                 uint32_t &observed_memberwise_header,
                                 std::vector<double> &values,
                                 std::vector<int32_t> &flat_indices,
                                 std::string &failure_reason,
                                 bool collect_indices = true);

// Decode one STL member column inside a member-wise vector<object> payload.
// ROOT frames the complete member column once; the column then contains one
// length-prefixed primitive vector for every element of the outer vector.
// column_offset is located by the ROOT member-wise action sequence so variable
// width members (bases, objects and other STL containers) before the projected
// member do not have to be guessed by this codec.
bool DecodeSerializedNestedPrimitiveVectorColumn(
    const uint8_t *bytes, size_t entry_size, size_t column_offset,
    uint64_t outer_count, const SerializedEntryLayout &layout,
    uint64_t max_values_per_entry, std::vector<double> &values,
    std::vector<int32_t> &flat_indices, std::string &failure_reason,
    bool collect_indices = true);

bool EqualDecodedValues(const std::vector<double> &left,
                        const std::vector<int32_t> &left_indices,
                        const std::vector<double> &right,
                        const std::vector<int32_t> &right_indices);

} // namespace duckdb::rootlake
