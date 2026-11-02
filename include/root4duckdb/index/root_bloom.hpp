#ifndef ROOT4DUCKDB_INDEX_ROOT_BLOOM_HPP
#define ROOT4DUCKDB_INDEX_ROOT_BLOOM_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake
{

/// Builds the self-describing Bloom payload used by index format 12.
class RootBloomBuilder
{
  public:
    /// Configures the bounded adaptive Bloom payload.
    RootBloomBuilder(uint32_t max_payload_bytes_p, double false_positive_rate_p);

    /// Adds one finite numeric value, switching to a bounded streaming filter when needed.
    void Add(double value);
    /// Serializes the version-1 payload and releases temporary storage.
    std::string SerializeAndRelease();
    /// Reports retained heap storage.
    uint64_t MemoryUsage() const;

  private:
    uint32_t max_payload_bytes;
    double false_positive_rate;
    std::vector<uint64_t> hashes;
    std::string streaming_bits;
    uint64_t value_count = 0;
    uint8_t streaming_hash_count = 0;
};

/// Reads optional Bloom metadata without risking false rejection.
class RootBloomFilter
{
  public:
    /// Returns true for matches and for unsupported payloads.
    static bool MayContain(const std::string& bytes, double value);
};

} // namespace duckdb::rootlake

#endif
