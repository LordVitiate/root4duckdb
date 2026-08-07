#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb::rootlake {

// Versioned, adaptive Bloom filter used by index format 12.  The serialized
// form is self-describing so a reader never has to guess the hash count or bit
// width from the BLOB length.
class RootBloomBuilder {
public:
    RootBloomBuilder(uint32_t max_payload_bytes, double false_positive_rate);

    void Add(double value);
    std::string SerializeAndRelease();
    uint64_t MemoryUsage() const;

private:
    uint32_t max_payload_bytes;
    double false_positive_rate;
    std::vector<uint64_t> hashes;
};

class RootBloomFilter {
public:
    // Missing, malformed and future filter formats deliberately return true:
    // Bloom metadata is an optional pruning hint and must never reject data.
    static bool MayContain(const std::string &bytes, double value);
};

} // namespace duckdb::rootlake
