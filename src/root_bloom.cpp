#include "include/root_bloom.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>

namespace duckdb::rootlake {

namespace {

static constexpr char BLOOM_MAGIC[] = {'R', '4', 'B', 'F'};
static constexpr uint8_t BLOOM_VERSION = 1;
static constexpr uint16_t BLOOM_HEADER_SIZE = 24;

static uint64_t CanonicalDoubleHash(double value) {
    if (value == 0.0) value = 0.0;
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits ^= bits >> 33U;
    bits *= 0xff51afd7ed558ccdULL;
    bits ^= bits >> 33U;
    bits *= 0xc4ceb9fe1a85ec53ULL;
    bits ^= bits >> 33U;
    return bits;
}

static uint64_t Mix64(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

static void AppendUInt16(std::string &out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

static void AppendUInt64(std::string &out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

static uint16_t ReadUInt16(const std::string &bytes, size_t offset) {
    return static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset])) |
           static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset + 1])) << 8;
}

static uint64_t ReadUInt64(const std::string &bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

} // namespace

RootBloomBuilder::RootBloomBuilder(uint32_t max_payload_bytes_p, double false_positive_rate_p)
    : max_payload_bytes(max_payload_bytes_p),
      false_positive_rate(std::clamp(false_positive_rate_p, 1e-9, 0.5)) {
}

void RootBloomBuilder::Add(double value) {
    if (max_payload_bytes == 0 || !std::isfinite(value)) return;
    hashes.push_back(CanonicalDoubleHash(value));
}

uint64_t RootBloomBuilder::MemoryUsage() const {
    return static_cast<uint64_t>(hashes.capacity()) * sizeof(uint64_t);
}

std::string RootBloomBuilder::SerializeAndRelease() {
    if (max_payload_bytes == 0 || hashes.empty()) return {};
    const auto count = static_cast<double>(hashes.size());
    const auto denominator = std::log(2.0) * std::log(2.0);
    const auto ideal_bits = static_cast<uint64_t>(std::ceil(-count * std::log(false_positive_rate) / denominator));
    const auto maximum_bits = static_cast<uint64_t>(max_payload_bytes) * 8ULL;
    const auto bit_count = std::max<uint64_t>(8, std::min<uint64_t>(ideal_bits, maximum_bits));
    const auto payload_bytes = static_cast<size_t>((bit_count + 7) / 8);
    const auto effective_bits = static_cast<uint64_t>(payload_bytes) * 8ULL;
    const auto ideal_hashes = static_cast<uint32_t>(std::llround(
        (static_cast<double>(effective_bits) / count) * std::log(2.0)));
    const auto hash_count = static_cast<uint8_t>(std::clamp<uint32_t>(ideal_hashes, 1, 16));

    std::string out;
    out.reserve(BLOOM_HEADER_SIZE + payload_bytes);
    out.append(BLOOM_MAGIC, sizeof(BLOOM_MAGIC));
    out.push_back(static_cast<char>(BLOOM_VERSION));
    out.push_back(static_cast<char>(hash_count));
    AppendUInt16(out, BLOOM_HEADER_SIZE);
    AppendUInt64(out, effective_bits);
    AppendUInt64(out, hashes.size());
    out.resize(BLOOM_HEADER_SIZE + payload_bytes, '\0');

    auto set_bit = [&](uint64_t bit) {
        const auto byte_offset = BLOOM_HEADER_SIZE + static_cast<size_t>(bit / 8);
        out[byte_offset] = static_cast<char>(static_cast<uint8_t>(out[byte_offset]) |
                                             static_cast<uint8_t>(1U << (bit % 8)));
    };
    for (const auto h1 : hashes) {
        const auto h2 = Mix64(h1 ^ 0x9e3779b97f4a7c15ULL) | 1ULL;
        for (uint8_t i = 0; i < hash_count; ++i) set_bit((h1 + static_cast<uint64_t>(i) * h2) % effective_bits);
    }
    std::vector<uint64_t>().swap(hashes);
    return out;
}

bool RootBloomFilter::MayContain(const std::string &bytes, double value) {
    if (bytes.empty()) return true;
    if (bytes.size() < BLOOM_HEADER_SIZE ||
        std::memcmp(bytes.data(), BLOOM_MAGIC, sizeof(BLOOM_MAGIC)) != 0 ||
        static_cast<uint8_t>(bytes[4]) != BLOOM_VERSION) {
        return true;
    }
    const auto hash_count = static_cast<uint8_t>(bytes[5]);
    const auto header_size = ReadUInt16(bytes, 6);
    const auto bit_count = ReadUInt64(bytes, 8);
    if (hash_count == 0 || hash_count > 16 || header_size < BLOOM_HEADER_SIZE ||
        header_size > bytes.size() || bit_count == 0 ||
        bit_count > static_cast<uint64_t>(bytes.size() - header_size) * 8ULL) {
        return true;
    }
    const auto h1 = CanonicalDoubleHash(value);
    const auto h2 = Mix64(h1 ^ 0x9e3779b97f4a7c15ULL) | 1ULL;
    for (uint8_t i = 0; i < hash_count; ++i) {
        const auto bit = (h1 + static_cast<uint64_t>(i) * h2) % bit_count;
        const auto byte = static_cast<uint8_t>(bytes[header_size + bit / 8]);
        if ((byte & static_cast<uint8_t>(1U << (bit % 8))) == 0) return false;
    }
    return true;
}

} // namespace duckdb::rootlake
