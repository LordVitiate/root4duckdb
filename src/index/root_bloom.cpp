#include "root4duckdb/index/root_bloom.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>

namespace duckdb::rootlake
{

namespace
{

static constexpr char BLOOM_MAGIC[] = {'R', '4', 'B', 'F'};
static constexpr uint8_t BLOOM_VERSION = 1;
static constexpr uint16_t BLOOM_HEADER_SIZE = 24;

static uint64_t CanonicalDoubleHash(double value)
{
    if (value == 0.0)
    {
        value = 0.0;
    }
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits ^= bits >> 33U;
    bits *= 0xFF51AFD7ED558CCDULL;
    bits ^= bits >> 33U;
    bits *= 0xC4CEB9FE1A85EC53ULL;
    bits ^= bits >> 33U;
    return bits;
}

static uint64_t Mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

static void AppendUInt16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

static void AppendUInt64(std::string& out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

static uint16_t ReadUInt16(const std::string& bytes, size_t offset)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset])) |
           static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset + 1])) << 8;
}

static uint64_t ReadUInt64(const std::string& bytes, size_t offset)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

} // namespace

RootBloomBuilder::RootBloomBuilder(uint32_t max_payload_bytes_p, double false_positive_rate_p)
    : max_payload_bytes(max_payload_bytes_p), false_positive_rate(std::clamp(false_positive_rate_p, 1e-9, 0.5))
{
}

static void SetBloomBits(std::string& payload, uint64_t hash, uint8_t hash_count)
{
    const auto bit_count = static_cast<uint64_t>(payload.size()) * 8ULL;
    const auto second_hash = Mix64(hash ^ 0x9E3779B97F4A7C15ULL) | 1ULL;
    for (uint8_t i = 0; i < hash_count; ++i)
    {
        const auto bit = (hash + static_cast<uint64_t>(i) * second_hash) % bit_count;
        auto& byte = payload[static_cast<size_t>(bit / 8)];
        byte = static_cast<char>(static_cast<uint8_t>(byte) | static_cast<uint8_t>(1U << (bit % 8)));
    }
}

void RootBloomBuilder::Add(double value)
{
    if (max_payload_bytes == 0 || !std::isfinite(value))
    {
        return;
    }
    const auto hash = CanonicalDoubleHash(value);
    if (streaming_bits.empty())
    {
        const auto hash_limit = std::max<uint64_t>(1, max_payload_bytes / sizeof(uint64_t));
        if (hashes.size() < hash_limit)
        {
            hashes.push_back(hash);
            ++value_count;
            return;
        }
        streaming_bits.assign(max_payload_bytes, '\0');
        const auto ideal_hashes = std::llround(-std::log(false_positive_rate) / std::log(2.0));
        streaming_hash_count = static_cast<uint8_t>(std::clamp<long long>(ideal_hashes, 1, 16));
        for (const auto buffered_hash : hashes)
        {
            SetBloomBits(streaming_bits, buffered_hash, streaming_hash_count);
        }
        std::vector<uint64_t>().swap(hashes);
    }
    SetBloomBits(streaming_bits, hash, streaming_hash_count);
    ++value_count;
}

uint64_t RootBloomBuilder::MemoryUsage() const
{
    return static_cast<uint64_t>(hashes.capacity()) * sizeof(uint64_t) +
           (streaming_bits.empty() ? 0ULL : static_cast<uint64_t>(streaming_bits.capacity()));
}

std::string RootBloomBuilder::SerializeAndRelease()
{
    if (value_count == 0)
    {
        return {};
    }
    std::string payload;
    uint8_t hash_count = streaming_hash_count;
    if (!streaming_bits.empty())
    {
        payload.swap(streaming_bits);
    }
    else
    {
        const auto count = static_cast<double>(value_count);
        const auto denominator = std::log(2.0) * std::log(2.0);
        const auto ideal_bits = static_cast<uint64_t>(std::ceil(-count * std::log(false_positive_rate) / denominator));
        const auto maximum_bits = static_cast<uint64_t>(max_payload_bytes) * 8ULL;
        const auto bit_count = std::max<uint64_t>(8, std::min<uint64_t>(ideal_bits, maximum_bits));
        const auto payload_bytes = static_cast<size_t>((bit_count + 7) / 8);
        const auto ideal_hashes =
            static_cast<uint32_t>(std::llround((static_cast<double>(payload_bytes * 8ULL) / count) * std::log(2.0)));
        hash_count = static_cast<uint8_t>(std::clamp<uint32_t>(ideal_hashes, 1, 16));
        payload.assign(payload_bytes, '\0');
        for (const auto hash : hashes)
        {
            SetBloomBits(payload, hash, hash_count);
        }
    }
    std::vector<uint64_t>().swap(hashes);
    std::string out;
    out.reserve(BLOOM_HEADER_SIZE + payload.size());
    out.append(BLOOM_MAGIC, sizeof(BLOOM_MAGIC));
    out.push_back(static_cast<char>(BLOOM_VERSION));
    out.push_back(static_cast<char>(hash_count));
    AppendUInt16(out, BLOOM_HEADER_SIZE);
    AppendUInt64(out, static_cast<uint64_t>(payload.size()) * 8ULL);
    AppendUInt64(out, value_count);
    out.append(payload);
    value_count = 0;
    return out;
}

bool RootBloomFilter::MayContain(const std::string& bytes, double value)
{
    if (bytes.empty() || !std::isfinite(value))
    {
        return true;
    }
    if (bytes.size() < BLOOM_HEADER_SIZE || std::memcmp(bytes.data(), BLOOM_MAGIC, sizeof(BLOOM_MAGIC)) != 0 ||
        static_cast<uint8_t>(bytes[4]) != BLOOM_VERSION)
    {
        return true;
    }
    const auto hash_count = static_cast<uint8_t>(bytes[5]);
    const auto header_size = ReadUInt16(bytes, 6);
    const auto bit_count = ReadUInt64(bytes, 8);
    if (hash_count == 0 || hash_count > 16 || header_size < BLOOM_HEADER_SIZE || header_size > bytes.size() ||
        bit_count == 0 || bit_count > static_cast<uint64_t>(bytes.size() - header_size) * 8ULL)
    {
        return true;
    }
    const auto h1 = CanonicalDoubleHash(value);
    const auto h2 = Mix64(h1 ^ 0x9E3779B97F4A7C15ULL) | 1ULL;
    for (uint8_t i = 0; i < hash_count; ++i)
    {
        const auto bit = (h1 + static_cast<uint64_t>(i) * h2) % bit_count;
        const auto byte = static_cast<uint8_t>(bytes[header_size + bit / 8]);
        if ((byte & static_cast<uint8_t>(1U << (bit % 8))) == 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace duckdb::rootlake
