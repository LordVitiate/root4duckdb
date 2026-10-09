#include "root4duckdb/serialized/root_serialized_basket_utils.hpp"

#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>

namespace duckdb::rootlake
{

bool ResolveSerializedBasket(TBranch* branch, uint64_t entry, SerializedBasketRange& range, std::string& failure_reason)
{
    const int basket_count = branch->GetWriteBasket() + 1;
    auto* basket_entries = branch->GetBasketEntry();
    if (basket_count <= 0 || !basket_entries)
    {
        failure_reason = "physical branch has no basket entry map";
        return false;
    }
    const auto* upper = std::upper_bound(basket_entries, basket_entries + basket_count, static_cast<Long64_t>(entry));
    int basket_number = static_cast<int>(upper - basket_entries) - 1;
    if (basket_number < 0)
    {
        basket_number = 0;
    }
    if (basket_number >= basket_count)
    {
        failure_reason = "entry is outside physical basket map";
        return false;
    }
    const uint64_t basket_begin = static_cast<uint64_t>(std::max<Long64_t>(0, basket_entries[basket_number]));
    const uint64_t basket_end = basket_number + 1 < basket_count
                                    ? static_cast<uint64_t>(std::max<Long64_t>(0, basket_entries[basket_number + 1]))
                                    : static_cast<uint64_t>(std::max<Long64_t>(0, branch->GetEntries()));
    if (entry < basket_begin || entry >= basket_end)
    {
        failure_reason = "entry does not belong to resolved physical basket";
        return false;
    }
    range.number = basket_number;
    range.entry_begin = basket_begin;
    range.entry_end = basket_end;
    return true;
}

bool SliceSerializedBasket(TBasket* basket, uint64_t entry, uint64_t entry_begin, uint64_t max_entry_bytes,
                           bool& prepared, const uint8_t*& begin, size_t& size, std::string& failure_reason)
{
    if (!prepared)
    {
        basket->PrepareBasket(static_cast<Long64_t>(entry));
        prepared = true;
    }
    auto* buffer = basket->GetBufferRef();
    auto* offsets = basket->GetEntryOffset();
    if (!buffer || !offsets)
    {
        failure_reason = "basket has no decompressed buffer or entry-offset table";
        return false;
    }
    if (basket->GetDisplacement())
    {
        failure_reason = "basket uses unsupported entry displacement";
        return false;
    }
    const auto local = entry - entry_begin;
    if (local >= static_cast<uint64_t>(std::max(0, basket->GetNevBuf())))
    {
        failure_reason = "local entry exceeds basket entry count";
        return false;
    }
    const int begin_offset = offsets[local];
    const int end_offset =
        local + 1 < static_cast<uint64_t>(basket->GetNevBuf()) ? offsets[local + 1] : basket->GetLast();
    if (begin_offset < 0 || end_offset < begin_offset || end_offset > buffer->BufferSize())
    {
        failure_reason = "invalid decompressed entry offsets";
        return false;
    }
    size = static_cast<size_t>(end_offset - begin_offset);
    if (size > max_entry_bytes)
    {
        failure_reason = "serialized entry exceeds configured safety limit";
        return false;
    }
    begin = reinterpret_cast<const uint8_t*>(buffer->Buffer()) + begin_offset;
    return true;
}

} // namespace duckdb::rootlake
