#include "root4duckdb/serialized/root_serialized_basket_cache.hpp"

#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/serialized/root_serialized_basket_utils.hpp"

#include <algorithm>
#include <stdexcept>

namespace duckdb::rootlake
{

SerializedBasketCache::SerializedBasketCache() = default;
SerializedBasketCache::~SerializedBasketCache() noexcept = default;

void SerializedBasketCache::Bind(TBranch* branch_p)
{
    if (!branch_p)
    {
        throw std::invalid_argument("serialized basket cache requires a physical ROOT branch");
    }
    if (branch && branch != branch_p)
    {
        throw std::invalid_argument("serialized basket cache cannot be rebound to another ROOT branch");
    }
    branch = branch_p;
}

void SerializedBasketCache::Reset() noexcept
{
    branch = nullptr;
    basket = nullptr;
    basket_prepared = false;
    current_basket = -1;
    current_basket_entry_begin = 0;
    current_basket_entry_end = 0;
}

TBranch* SerializedBasketCache::Branch() const noexcept
{
    return branch;
}

bool SerializedBasketCache::LoadBasket(uint64_t entry, bool& loaded_new_basket, uint64_t& compressed_bytes,
                                       std::string& failure_reason)
{
    loaded_new_basket = false;
    compressed_bytes = 0;
    if (!branch)
    {
        failure_reason = "serialized basket cache is not bound";
        return false;
    }
    if (basket && entry >= current_basket_entry_begin && entry < current_basket_entry_end)
    {
        return true;
    }
    SerializedBasketRange range;
    if (!ResolveSerializedBasket(branch, entry, range, failure_reason))
    {
        return false;
    }
    const auto basket_number = range.number;
    const auto basket_begin = range.entry_begin;
    const auto basket_end = range.entry_end;
    if (basket_number == current_basket && basket)
    {
        return true;
    }
    basket = nullptr;
    basket_prepared = false;
    branch->DropBaskets();
    basket = branch->GetBasket(basket_number);
    if (!basket || basket->IsZombie())
    {
        failure_reason = "ROOT failed to load/decompress physical basket";
        basket = nullptr;
        return false;
    }
    current_basket = basket_number;
    current_basket_entry_begin = basket_begin;
    current_basket_entry_end = basket_end;
    loaded_new_basket = true;
    auto* basket_bytes = branch->GetBasketBytes();
    compressed_bytes = basket_bytes && basket_bytes[basket_number] > 0
                           ? static_cast<uint64_t>(basket_bytes[basket_number])
                           : static_cast<uint64_t>(std::max(0, basket->GetNbytes()));
    return true;
}

bool SerializedBasketCache::EntrySlice(uint64_t entry, uint64_t max_entry_bytes, const uint8_t*& begin, size_t& size,
                                       bool& loaded_new_basket, uint64_t& compressed_bytes, std::string& failure_reason)
{
    begin = nullptr;
    size = 0;
    if (!LoadBasket(entry, loaded_new_basket, compressed_bytes, failure_reason))
    {
        return false;
    }
    return SliceSerializedBasket(basket, entry, current_basket_entry_begin, max_entry_bytes, basket_prepared, begin,
                                 size, failure_reason);
}

bool SerializedBasketCache::CurrentBasketInfo(SerializedBasketInfo& info) const
{
    if (!branch || !basket || current_basket < 0)
    {
        return false;
    }
    info.basket_number = current_basket;
    info.entry_begin = current_basket_entry_begin;
    info.entry_end = current_basket_entry_end;
    info.physical_offset = static_cast<uint64_t>(std::max<Long64_t>(0, basket->GetSeekKey()));
    info.key_length = static_cast<uint32_t>(std::max(0, basket->GetKeylen()));
    info.compressed_size = static_cast<uint32_t>(std::max(0, basket->GetNbytes()));
    info.uncompressed_size = static_cast<uint32_t>(std::max(0, basket->GetObjlen()));
    return true;
}

} // namespace duckdb::rootlake
