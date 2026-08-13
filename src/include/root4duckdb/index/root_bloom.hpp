#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake {

/// Builds the self-describing Bloom payload used by index format 12.
class RootBloomBuilder {
  public:
    /// Configures the bounded adaptive Bloom payload.
    RootBloomBuilder(uint32_t max_payload_bytes, double false_positive_rate);

    /// Adds one finite numeric value to the pending payload.
    void Add(double value);
    /// Serializes the payload and releases buffered hashes.
    std::string SerializeAndRelease();
    /// Reports memory retained by buffered hashes.
    uint64_t MemoryUsage() const;

  private:
    uint32_t max_payload_bytes;
    double false_positive_rate;
    std::vector<uint64_t> hashes;
};

/// Reads optional Bloom metadata without risking false rejection.
class RootBloomFilter {
  public:
    /// Returns true for matches and for unsupported payloads.
    static bool MayContain(const std::string& bytes, double value);
};

} // namespace duckdb::rootlake
