#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class TBranch;
class TBasket;

namespace duckdb::rootlake {

/// Physical metadata for the currently loaded basket.
struct SerializedBasketInfo {
    int32_t basket_number = -1;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
    uint64_t physical_offset = 0;
    uint32_t key_length = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
};

/// Worker-local cache for one physical ROOT branch basket.
///
/// Multiple logical projections of the same ROOT object commonly resolve to
/// the same physical TBranch. Sharing this cache prevents each projection
/// from independently dropping, loading and decompressing the same basket.
/// The cache is intentionally worker-local: TBranch/TBasket state is never
/// shared across DuckDB workers.
class SerializedBasketCache {
  public:
    SerializedBasketCache();
    ~SerializedBasketCache() noexcept;
    SerializedBasketCache(const SerializedBasketCache&) = delete;
    SerializedBasketCache& operator=(const SerializedBasketCache&) = delete;

    void Bind(TBranch* branch);
    void Reset() noexcept;

    bool EntrySlice(uint64_t entry, uint64_t max_entry_bytes, const uint8_t*& begin, size_t& size,
                    bool& loaded_new_basket, uint64_t& compressed_bytes, std::string& failure_reason);
    bool CurrentBasketInfo(SerializedBasketInfo& info) const;
    TBranch* Branch() const noexcept;

  private:
    bool LoadBasket(uint64_t entry, bool& loaded_new_basket, uint64_t& compressed_bytes,
                    std::string& failure_reason);

    TBranch* branch = nullptr;
    TBasket* basket = nullptr;
    bool basket_prepared = false;
    int current_basket = -1;
    uint64_t current_basket_entry_begin = 0;
    uint64_t current_basket_entry_end = 0;
};

} // namespace duckdb::rootlake
