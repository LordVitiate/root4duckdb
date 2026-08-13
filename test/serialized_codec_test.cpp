#include "root4duckdb/serialized/root_serialized_codec.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using duckdb::rootlake::DecodeSerializedNestedPrimitiveVectorColumn;
using duckdb::rootlake::DecodeSerializedVectorEntry;
using duckdb::rootlake::EqualDecodedValues;
using duckdb::rootlake::SerializedEntryLayout;

static void PutBE32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value >> 24U);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16U);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8U);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

static void PutBE16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value >> 8U);
    bytes[offset + 1] = static_cast<uint8_t>(value);
}

static void PutBE64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    PutBE32(bytes, offset, static_cast<uint32_t>(value >> 32U));
    PutBE32(bytes, offset + 4, static_cast<uint32_t>(value));
}

static std::vector<uint8_t> FloatEntry(uint32_t count, uint32_t prefix_per_element, const std::vector<float>& values,
                                       uint32_t header = 0x4009001eU) {
    const size_t size = 12 + static_cast<size_t>(count) * prefix_per_element + values.size() * 4;
    std::vector<uint8_t> bytes(size, 0xa5);
    PutBE32(bytes, 0, 0x40000000U | static_cast<uint32_t>(size - 4));
    PutBE32(bytes, 4, header);
    PutBE32(bytes, 8, count);
    size_t offset = 12 + static_cast<size_t>(count) * prefix_per_element;
    for (const auto value : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        PutBE32(bytes, offset, bits);
        offset += 4;
    }
    return bytes;
}

static std::vector<uint8_t> NestedShortColumn(size_t column_offset) {
    // Three outer objects: {-1, 7}, {}, {8, 9, 10}. The bytes before and after
    // the framed target column stand in for other member-wise columns.
    const size_t column_size = 6 + (4 + 4) + 4 + (4 + 6);
    std::vector<uint8_t> bytes(column_offset + column_size + 3, 0xa5);
    PutBE32(bytes, column_offset, 0x40000000U | static_cast<uint32_t>(column_size - 4));
    PutBE16(bytes, column_offset + 4, 9);
    size_t cursor = column_offset + 6;
    PutBE32(bytes, cursor, 2);
    cursor += 4;
    PutBE16(bytes, cursor, 0xffffU);
    PutBE16(bytes, cursor + 2, 7);
    cursor += 4;
    PutBE32(bytes, cursor, 0);
    cursor += 4;
    PutBE32(bytes, cursor, 3);
    cursor += 4;
    PutBE16(bytes, cursor, 8);
    PutBE16(bytes, cursor + 2, 9);
    PutBE16(bytes, cursor + 4, 10);
    return bytes;
}

int main() {
    SerializedEntryLayout scalar;
    scalar.value_type = "Float_t";
    scalar.bytes_before_value_per_element = 10;
    scalar.value_bytes = 4;
    scalar.fixed_array_length = 1;
    scalar.index_depth = 1;

    auto entry = FloatEntry(2, 10, {12.0F, 59.5F});
    uint32_t observed_header = 0;
    std::vector<double> values;
    std::vector<int32_t> indices;
    std::string reason;
    assert(
        DecodeSerializedVectorEntry(entry.data(), entry.size(), scalar, 100, observed_header, values, indices, reason));
    assert((values == std::vector<double>{12.0, 59.5}));
    assert((indices == std::vector<int32_t>{0, 1}));
    assert(reason.empty());

    observed_header = 0;
    assert(DecodeSerializedVectorEntry(entry.data(), entry.size(), scalar, 100, observed_header, values, indices,
                                       reason, false));
    assert((values == std::vector<double>{12.0, 59.5}));
    assert(indices.empty());

    auto empty_entry = FloatEntry(0, 10, {});
    observed_header = 0;
    assert(DecodeSerializedVectorEntry(empty_entry.data(), empty_entry.size(), scalar, 100, observed_header, values,
                                       indices, reason));
    assert(values.empty() && indices.empty());

    auto bad_count = entry;
    PutBE32(bad_count, 0, 0x40000001U);
    assert(!DecodeSerializedVectorEntry(bad_count.data(), bad_count.size(), scalar, 100, observed_header, values,
                                        indices, reason));
    assert(!reason.empty());

    auto changed_header = FloatEntry(2, 10, {12.0F, 59.5F}, 0x4009001fU);
    assert(!DecodeSerializedVectorEntry(changed_header.data(), changed_header.size(), scalar, 100, observed_header,
                                        values, indices, reason));

    SerializedEntryLayout fixed_array;
    fixed_array.value_type = "float";
    fixed_array.value_bytes = 4;
    fixed_array.fixed_array_length = 2;
    fixed_array.array_dimensions = {2};
    fixed_array.index_depth = 2;
    auto array_entry = FloatEntry(2, 0, {1.0F, 1.5F, 2.0F, 2.5F});
    observed_header = 0;
    assert(DecodeSerializedVectorEntry(array_entry.data(), array_entry.size(), fixed_array, 100, observed_header,
                                       values, indices, reason));
    assert((indices == std::vector<int32_t>{0, 0, 0, 1, 1, 0, 1, 1}));
    assert(!DecodeSerializedVectorEntry(array_entry.data(), array_entry.size(), fixed_array, 3, observed_header, values,
                                        indices, reason));

    SerializedEntryLayout signed_int;
    signed_int.value_type = "Int_t";
    signed_int.value_bytes = 4;
    signed_int.fixed_array_length = 1;
    signed_int.index_depth = 1;
    std::vector<uint8_t> int_entry(20, 0);
    PutBE32(int_entry, 0, 0x40000000U | 16U);
    PutBE32(int_entry, 4, 0x4009001eU);
    PutBE32(int_entry, 8, 2);
    PutBE32(int_entry, 12, 0xffffffffU);
    PutBE32(int_entry, 16, 42);
    observed_header = 0;
    assert(DecodeSerializedVectorEntry(int_entry.data(), int_entry.size(), signed_int, 100, observed_header, values,
                                       indices, reason));
    assert((values == std::vector<double>{-1.0, 42.0}));

    SerializedEntryLayout double_layout;
    double_layout.value_type = "Double_t";
    double_layout.value_bytes = 8;
    double_layout.fixed_array_length = 1;
    double_layout.index_depth = 1;
    std::vector<uint8_t> double_entry(20, 0);
    PutBE32(double_entry, 0, 0x40000000U | 16U);
    PutBE32(double_entry, 4, 0x4009001eU);
    PutBE32(double_entry, 8, 1);
    double expected_double = -17.25;
    uint64_t double_bits = 0;
    std::memcpy(&double_bits, &expected_double, sizeof(double_bits));
    PutBE64(double_entry, 12, double_bits);
    observed_header = 0;
    assert(DecodeSerializedVectorEntry(double_entry.data(), double_entry.size(), double_layout, 100, observed_header,
                                       values, indices, reason));
    assert((values == std::vector<double>{expected_double}));

    auto bad_dimensions = fixed_array;
    bad_dimensions.array_dimensions = {3};
    assert(!DecodeSerializedVectorEntry(array_entry.data(), array_entry.size(), bad_dimensions, 100, observed_header,
                                        values, indices, reason));

    SerializedEntryLayout nested_short;
    nested_short.value_type = "short";
    nested_short.value_bytes = 2;
    nested_short.fixed_array_length = 1;
    nested_short.index_depth = 2;
    constexpr size_t nested_offset = 17;
    auto nested_entry = NestedShortColumn(nested_offset);
    assert(DecodeSerializedNestedPrimitiveVectorColumn(nested_entry.data(), nested_entry.size(), nested_offset, 3,
                                                       nested_short, 100, values, indices, reason));
    assert((values == std::vector<double>{-1.0, 7.0, 8.0, 9.0, 10.0}));
    assert((indices == std::vector<int32_t>{0, 0, 0, 1, 2, 0, 2, 1, 2, 2}));
    assert(reason.empty());

    assert(DecodeSerializedNestedPrimitiveVectorColumn(nested_entry.data(), nested_entry.size(), nested_offset, 3,
                                                       nested_short, 100, values, indices, reason, false));
    assert((values == std::vector<double>{-1.0, 7.0, 8.0, 9.0, 10.0}));
    assert(indices.empty());

    assert(!DecodeSerializedNestedPrimitiveVectorColumn(nested_entry.data(), nested_entry.size(), nested_offset, 3,
                                                        nested_short, 4, values, indices, reason));
    auto negative_inner_count = nested_entry;
    PutBE32(negative_inner_count, nested_offset + 6, 0xffffffffU);
    assert(!DecodeSerializedNestedPrimitiveVectorColumn(negative_inner_count.data(), negative_inner_count.size(),
                                                        nested_offset, 3, nested_short, 100, values, indices, reason));
    auto truncated_nested = nested_entry;
    PutBE32(truncated_nested, nested_offset, 0x40000000U | static_cast<uint32_t>(nested_entry.size()));
    assert(!DecodeSerializedNestedPrimitiveVectorColumn(truncated_nested.data(), truncated_nested.size(), nested_offset,
                                                        3, nested_short, 100, values, indices, reason));
    auto undersized_nested_frame = nested_entry;
    PutBE32(undersized_nested_frame, nested_offset, 0x40000000U);
    assert(!DecodeSerializedNestedPrimitiveVectorColumn(undersized_nested_frame.data(), undersized_nested_frame.size(),
                                                        nested_offset, 3, nested_short, 100, values, indices, reason));
    auto wrong_nested_shape = nested_short;
    wrong_nested_shape.index_depth = 1;
    assert(!DecodeSerializedNestedPrimitiveVectorColumn(nested_entry.data(), nested_entry.size(), nested_offset, 3,
                                                        wrong_nested_shape, 100, values, indices, reason));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(EqualDecodedValues({nan, 1.0}, {0, 1}, {nan, 1.0}, {0, 1}));
    assert(!EqualDecodedValues({1.0}, {0}, {2.0}, {0}));

    std::cout << "serialized codec: OK\n";
    return 0;
}
