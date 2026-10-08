#ifndef ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_BASKET_CACHE_HPP
#define ROOT4DUCKDB_SERIALIZED_ROOT_SERIALIZED_BASKET_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

class TBranch;
class TBasket;

namespace duckdb::rootlake
{

/// Physical metadata for the currently loaded basket.
struct SerializedBasketInfo
{
    int32_t basket_number = -1;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
    uint64_t physical_offset = 0;
    uint32_t key_length = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
};

/// Worker-local physical basket cache shared by projections of one branch.
class SerializedBasketCache
{
  public:
    SerializedBasketCache();
    ~SerializedBasketCache() noexcept;
    SerializedBasketCache(const SerializedBasketCache&) = delete;
    SerializedBasketCache& operator=(const SerializedBasketCache&) = delete;

    void Bind(TBranch* branch_p);
    void Reset() noexcept;

    bool EntrySlice(uint64_t entry, uint64_t max_entry_bytes, const uint8_t*& begin, size_t& size,
                    bool& loaded_new_basket, uint64_t& compressed_bytes, std::string& failure_reason);
    bool CurrentBasketInfo(SerializedBasketInfo& info) const;
    TBranch* Branch() const noexcept;

  private:
    bool LoadBasket(uint64_t entry, bool& loaded_new_basket, uint64_t& compressed_bytes, std::string& failure_reason);

    TBranch* branch = nullptr;
    TBasket* basket = nullptr;
    bool basket_prepared = false;
    int current_basket = -1;
    uint64_t current_basket_entry_begin = 0;
    uint64_t current_basket_entry_end = 0;
};

} // namespace duckdb::rootlake

#endif
