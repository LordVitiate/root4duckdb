#pragma once

#include "root_lake_common.hpp"
#include "root_serialized_codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

class TBranch;
class TBasket;

namespace duckdb::rootlake {

enum class RootReaderMode : uint8_t {
    AUTO = 0,
    SERIALIZED = 1,
    OBJECT = 2
};

RootReaderMode ParseRootReaderMode(std::string mode);
const char *RootReaderModeName(RootReaderMode mode);

struct SerializedReadPlan {
    bool supported = false;
    std::string reason;
    std::string logical_path;
    std::string root_class;
    std::string container_name;
    std::string element_class;
    std::string value_type;
    std::string physical_branch_name;
    std::string schema_fingerprint;
    uint32_t streamer_version = 0;
    uint32_t bytes_before_value_per_element = 0;
    uint32_t value_bytes = 0;
    uint64_t fixed_array_length = 1;
    std::vector<uint32_t> array_dimensions;
    idx_t index_depth = 0;
};

// Build a deliberately conservative projection plan for ROOT's member-wise
// std::vector<object> representation. Unsupported layouts remain correct by
// using the universal object reader.
SerializedReadPlan BuildSerializedReadPlan(TClass *root_class,
                                           const ParsedPath &path,
                                           TBranch *physical_branch);

struct SerializedReadCounters {
    uint64_t entries = 0;
    uint64_t values = 0;
    uint64_t baskets = 0;
    uint64_t compressed_bytes = 0;
    uint64_t serialized_bytes = 0;
};

struct SerializedBasketInfo {
    int32_t basket_number = -1;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
    uint64_t physical_offset = 0;
    uint32_t key_length = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
};

class SerializedBasketReader {
public:
    SerializedBasketReader() = default;
    SerializedBasketReader(const SerializedBasketReader &) = delete;
    SerializedBasketReader &operator=(const SerializedBasketReader &) = delete;
    SerializedBasketReader(SerializedBasketReader &&) noexcept = default;
    SerializedBasketReader &operator=(SerializedBasketReader &&) noexcept = default;

    void Bind(TBranch *branch, SerializedReadPlan plan,
              uint64_t max_entry_bytes = 64ULL * 1024ULL * 1024ULL,
              uint64_t max_values_per_entry = 10ULL * 1024ULL * 1024ULL);

    bool Decode(uint64_t entry, std::vector<double> &values,
                std::vector<int32_t> &flat_indices, std::string &failure_reason,
                bool collect_indices = true);

    bool IsBound() const { return branch != nullptr && plan.supported; }
    const SerializedReadPlan &Plan() const { return plan; }
    const SerializedReadCounters &Counters() const { return counters; }
    bool CurrentBasketInfo(SerializedBasketInfo &info) const;

private:
    bool LoadBasket(uint64_t entry, std::string &failure_reason);
    bool EntrySlice(uint64_t entry, const uint8_t *&begin, size_t &size,
                    std::string &failure_reason);

    TBranch *branch = nullptr;
    TBasket *basket = nullptr;
    bool basket_prepared = false;
    SerializedReadPlan plan;
    SerializedEntryLayout layout;
    int current_basket = -1;
    uint64_t current_basket_entry_begin = 0;
    uint64_t current_basket_entry_end = 0;
    uint64_t max_entry_bytes = 0;
    uint64_t max_values_per_entry = 0;
    uint32_t observed_memberwise_header = 0;
    SerializedReadCounters counters;
};

// Fallback warnings are emitted once per process/schema/path/reason. They can
// be disabled with ROOT4DUCKDB_FALLBACK_WARNINGS=0.
void WarnRootFallbackOnce(const std::string &logical_path,
                          const std::string &schema_fingerprint,
                          const std::string &reason);

} // namespace duckdb::rootlake
