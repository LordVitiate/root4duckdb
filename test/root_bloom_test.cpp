#include "root4duckdb/index/root_bloom.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using duckdb::rootlake::RootBloomBuilder;
using duckdb::rootlake::RootBloomFilter;

int main()
{
    for (const uint32_t bytes : {0U, 1U, 256U, 65536U})
    {
        RootBloomBuilder builder(bytes, 0.01);
        const std::vector<double> values = {0.0, -0.0, -12.5, 42.125, 1000000000.0};
        for (const auto value : values)
        {
            builder.Add(value);
        }
        builder.Add(std::numeric_limits<double>::quiet_NaN());
        for (uint32_t i = 0; i < 200000; ++i)
        {
            builder.Add(static_cast<double>(i % 99421));
            if (bytes != 0)
            {
                assert(builder.MemoryUsage() <= 2ULL * bytes + 128ULL);
            }
        }
        const auto payload = builder.SerializeAndRelease();
        assert(builder.MemoryUsage() == 0);
        assert(payload.size() <= static_cast<size_t>(bytes) + 24);
        for (const auto value : values)
        {
            assert(RootBloomFilter::MayContain(payload, value));
        }
        for (uint32_t i = 0; i < 200000; ++i)
        {
            assert(RootBloomFilter::MayContain(payload, static_cast<double>(i % 99421)));
        }
        assert(RootBloomFilter::MayContain(payload, std::numeric_limits<double>::quiet_NaN()));
        builder.Add(123.5);
        assert(RootBloomFilter::MayContain(builder.SerializeAndRelease(), 123.5));
    }
    RootBloomBuilder small(65536, 0.01);
    for (int i = 0; i < 100; ++i)
    {
        small.Add(static_cast<double>(i) + 0.125);
    }
    const auto payload = small.SerializeAndRelease();
    assert(payload.size() < 65536);
    for (int i = 0; i < 100; ++i)
    {
        assert(RootBloomFilter::MayContain(payload, static_cast<double>(i) + 0.125));
    }
    assert(RootBloomFilter::MayContain("invalid", 1.0));
}