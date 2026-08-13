#include "root4duckdb/serialized/root_serialized_codec.hpp"

#include "root4duckdb/serialized/root_serialized_codec_utils.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace duckdb::rootlake {

namespace {

bool ValidatePrimitiveLayout(const SerializedEntryLayout& layout, size_t expected_index_depth,
                             SerializedPrimitiveKind& primitive_kind, std::string& failure_reason) {
    if (!layout.value_bytes || !layout.fixed_array_length || !layout.index_depth) {
        failure_reason = "serialized entry layout is incomplete";
        return false;
    }
    primitive_kind = layout.primitive_kind == SerializedPrimitiveKind::UNKNOWN
                         ? ClassifySerializedPrimitive(layout.value_type)
                         : layout.primitive_kind;
    if (primitive_kind == SerializedPrimitiveKind::UNKNOWN ||
        serialized_codec::PrimitiveWidth(primitive_kind) != layout.value_bytes) {
        failure_reason = "serialized primitive type and width are inconsistent";
        return false;
    }
    if (layout.index_depth != expected_index_depth) {
        failure_reason = "serialized entry index shape is inconsistent";
        return false;
    }
    return true;
}

void PushArrayCoordinates(uint64_t flat, const std::vector<uint32_t>& dimensions, std::vector<int32_t>& indices) {
    if (dimensions.empty()) {
        indices.push_back(static_cast<int32_t>(flat));
        return;
    }
    const auto old_size = indices.size();
    indices.resize(old_size + dimensions.size());
    for (size_t reverse = dimensions.size(); reverse > 0; --reverse) {
        const size_t dim = reverse - 1;
        indices[old_size + dim] = static_cast<int32_t>(flat % dimensions[dim]);
        flat /= dimensions[dim];
    }
}

} // namespace

SerializedPrimitiveKind ClassifySerializedPrimitive(const std::string& raw_type) {
    const auto type = serialized_codec::NormalizePrimitiveType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O") {
        return SerializedPrimitiveKind::BOOL;
    }
    if (type == "Char_t" || type == "char" || type == "b") {
        return SerializedPrimitiveKind::INT8;
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B") {
        return SerializedPrimitiveKind::UINT8;
    }
    if (type == "Short_t" || type == "short" || type == "S") {
        return SerializedPrimitiveKind::INT16;
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s") {
        return SerializedPrimitiveKind::UINT16;
    }
    if (type == "Int_t" || type == "int" || type == "I") {
        return SerializedPrimitiveKind::INT32;
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i") {
        return SerializedPrimitiveKind::UINT32;
    }
    if (type == "Long64_t" || type == "long long" || type == "L") {
        return SerializedPrimitiveKind::INT64;
    }
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l") {
        return SerializedPrimitiveKind::UINT64;
    }
    if (type == "Float_t" || type == "float" || type == "F") {
        return SerializedPrimitiveKind::FLOAT32;
    }
    if (type == "Double_t" || type == "double" || type == "D") {
        return SerializedPrimitiveKind::FLOAT64;
    }
    return SerializedPrimitiveKind::UNKNOWN;
}

bool DecodeSerializedVectorEntry(const uint8_t* bytes, size_t entry_size, const SerializedEntryLayout& layout,
                                 uint64_t max_values_per_entry, uint32_t& observed_memberwise_header,
                                 std::vector<double>& values, std::vector<int32_t>& flat_indices,
                                 std::string& failure_reason, bool collect_indices) {
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    if (!bytes || entry_size < 12) {
        failure_reason = "serialized vector entry is shorter than its header";
        return false;
    }
    const size_t expected_index_depth =
        1 +
        (layout.fixed_array_length > 1 ? (layout.array_dimensions.empty() ? 1 : layout.array_dimensions.size()) : 0);
    SerializedPrimitiveKind primitive_kind = SerializedPrimitiveKind::UNKNOWN;
    if (!ValidatePrimitiveLayout(layout, expected_index_depth, primitive_kind, failure_reason)) {
        return false;
    }
    uint64_t dimensions_product = 1;
    for (const auto dimension : layout.array_dimensions) {
        if (!dimension || dimension > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            dimensions_product > std::numeric_limits<uint64_t>::max() / dimension) {
            failure_reason = "serialized fixed-array dimensions are invalid";
            return false;
        }
        dimensions_product *= dimension;
    }
    if (!layout.array_dimensions.empty() && dimensions_product != layout.fixed_array_length) {
        failure_reason = "serialized fixed-array dimensions do not match its length";
        return false;
    }
    const uint32_t byte_count = serialized_codec::ReadBE32(bytes);
    if ((byte_count & serialized_codec::ROOT_BYTE_COUNT_MASK) == 0) {
        failure_reason = "serialized vector entry has no ROOT byte-count marker";
        return false;
    }
    const uint64_t declared_size = static_cast<uint64_t>(byte_count & serialized_codec::ROOT_BYTE_COUNT_VALUE_MASK) + 4;
    if (declared_size != entry_size) {
        failure_reason = "serialized vector byte-count does not match entry offsets";
        return false;
    }
    const uint32_t memberwise_header = serialized_codec::ReadBE32(bytes + 4);
    if (!memberwise_header) {
        failure_reason = "serialized vector has an empty member-wise header";
        return false;
    }
    if (!observed_memberwise_header) {
        observed_memberwise_header = memberwise_header;
    }
    if (memberwise_header != observed_memberwise_header) {
        failure_reason = "member-wise streamer header changed inside one physical branch";
        return false;
    }
    const uint64_t count = serialized_codec::ReadBE32(bytes + 8);
    if (count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || count > max_values_per_entry ||
        count > max_values_per_entry / layout.fixed_array_length) {
        failure_reason = "serialized vector element count exceeds configured safety limit";
        return false;
    }
    if ((layout.bytes_before_value_per_element &&
         count > std::numeric_limits<uint64_t>::max() / layout.bytes_before_value_per_element) ||
        count > std::numeric_limits<uint64_t>::max() / layout.fixed_array_length) {
        failure_reason = "serialized vector size arithmetic overflow";
        return false;
    }
    const uint64_t prefix_bytes = count * layout.bytes_before_value_per_element;
    const uint64_t value_count = count * layout.fixed_array_length;
    if (value_count > std::numeric_limits<uint64_t>::max() / layout.value_bytes ||
        value_count > std::numeric_limits<size_t>::max() / layout.index_depth) {
        failure_reason = "serialized projected member size arithmetic overflow";
        return false;
    }
    const uint64_t projected_bytes = value_count * layout.value_bytes;
    if (prefix_bytes > entry_size - 12 || projected_bytes > entry_size - 12 - prefix_bytes) {
        failure_reason = "projected member range exceeds serialized entry";
        return false;
    }
    const uint8_t* projected = bytes + 12 + prefix_bytes;
    values.reserve(static_cast<size_t>(value_count));
    if (collect_indices) {
        flat_indices.reserve(static_cast<size_t>(value_count * layout.index_depth));
    }
    for (uint64_t element = 0; element < count; ++element) {
        for (uint64_t array_index = 0; array_index < layout.fixed_array_length; ++array_index) {
            const uint64_t flat = element * layout.fixed_array_length + array_index;
            double value = 0;
            if (!serialized_codec::DecodePrimitive(projected + flat * layout.value_bytes, primitive_kind, value)) {
                failure_reason = "unsupported primitive decoder for " + layout.value_type;
                values.clear();
                flat_indices.clear();
                return false;
            }
            values.push_back(value);
            if (collect_indices) {
                flat_indices.push_back(static_cast<int32_t>(element));
                if (layout.fixed_array_length > 1) {
                    PushArrayCoordinates(array_index, layout.array_dimensions, flat_indices);
                }
            }
        }
    }
    return true;
}

bool EqualDecodedValues(const std::vector<double>& left, const std::vector<int32_t>& left_indices,
                        const std::vector<double>& right, const std::vector<int32_t>& right_indices) {
    if (left_indices != right_indices || left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i] == right[i]) {
            continue;
        }
        if (std::isnan(left[i]) && std::isnan(right[i])) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace duckdb::rootlake
