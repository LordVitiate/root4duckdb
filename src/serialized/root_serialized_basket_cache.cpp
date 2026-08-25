#include "root4duckdb/serialized/root_serialized_basket_cache.hpp"

#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <stdexcept>

namespace duckdb::rootlake {

SerializedBasketCache::SerializedBasketCache() = default;
SerializedBasketCache::~SerializedBasketCache() noexcept = default;

void SerializedBasketCache::Bind(TBranch* branch_p) {
    if (!branch_p) {
        throw std::invalid_argument("serialized basket cache requires a physical ROOT branch");
    }
    if (branch && branch != branch_p) {
        throw std::invalid_argument("serialized basket cache cannot be rebound to another ROOT branch");
    }
    branch = branch_p;
}

void SerializedBasketCache::Reset() noexcept {
    branch = nullptr;
    basket = nullptr;
    basket_prepared = false;
    current_basket = -1;
    current_basket_entry_begin = 0;
    current_basket_entry_end = 0;
}

TBranch* SerializedBasketCache::Branch() const noexcept {
    return branch;
}

bool SerializedBasketCache::LoadBasket(uint64_t entry, bool& loaded_new_basket, uint64_t& compressed_bytes,
                                       std::string& failure_reason) {
    loaded_new_basket = false;
    compressed_bytes = 0;
    if (!branch) {
        failure_reason = "serialized basket cache is not bound";
        return false;
    }
    if (basket && entry >= current_basket_entry_begin && entry < current_basket_entry_end) {
        return true;
    }

    const int basket_count = branch->GetWriteBasket() + 1;
    auto* basket_entries = branch->GetBasketEntry();
    if (basket_count <= 0 || !basket_entries) {
        failure_reason = "physical branch has no basket entry map";
        return false;
    }

    const auto* upper = std::upper_bound(basket_entries, basket_entries + basket_count, static_cast<Long64_t>(entry));
    int basket_number = static_cast<int>(upper - basket_entries) - 1;
    if (basket_number < 0) {
        basket_number = 0;
    }
    if (basket_number >= basket_count) {
        failure_reason = "entry is outside physical basket map";
        return false;
    }

    const uint64_t basket_begin = static_cast<uint64_t>(std::max<Long64_t>(0, basket_entries[basket_number]));
    const uint64_t basket_end = basket_number + 1 < basket_count
                                    ? static_cast<uint64_t>(std::max<Long64_t>(0, basket_entries[basket_number + 1]))
                                    : static_cast<uint64_t>(std::max<Long64_t>(0, branch->GetEntries()));
    if (entry < basket_begin || entry >= basket_end) {
        failure_reason = "entry does not belong to resolved physical basket";
        return false;
    }
    if (basket_number == current_basket && basket) {
        return true;
    }

    basket = nullptr;
    basket_prepared = false;
    branch->DropBaskets();
    basket = branch->GetBasket(basket_number);
    if (!basket || basket->IsZombie()) {
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
                                       bool& loaded_new_basket, uint64_t& compressed_bytes,
                                       std::string& failure_reason) {
    begin = nullptr;
    size = 0;
    if (!LoadBasket(entry, loaded_new_basket, compressed_bytes, failure_reason)) {
        return false;
    }
    if (!basket_prepared) {
        basket->PrepareBasket(static_cast<Long64_t>(entry));
        basket_prepared = true;
    }

    auto* buffer = basket->GetBufferRef();
    auto* offsets = basket->GetEntryOffset();
    if (!buffer || !offsets) {
        failure_reason = "basket has no decompressed buffer or entry-offset table";
        return false;
    }
    if (basket->GetDisplacement()) {
        failure_reason = "basket uses unsupported entry displacement";
        return false;
    }

    const auto local = entry - current_basket_entry_begin;
    if (local >= static_cast<uint64_t>(std::max(0, basket->GetNevBuf()))) {
        failure_reason = "local entry exceeds basket entry count";
        return false;
    }
    const int begin_offset = offsets[local];
    const int end_offset =
        local + 1 < static_cast<uint64_t>(basket->GetNevBuf()) ? offsets[local + 1] : basket->GetLast();
    if (begin_offset < 0 || end_offset < begin_offset || end_offset > buffer->BufferSize()) {
        failure_reason = "invalid decompressed entry offsets";
        return false;
    }

    size = static_cast<size_t>(end_offset - begin_offset);
    if (size > max_entry_bytes) {
        failure_reason = "serialized entry exceeds configured safety limit";
        return false;
    }
    begin = reinterpret_cast<const uint8_t*>(buffer->Buffer()) + begin_offset;
    return true;
}

bool SerializedBasketCache::CurrentBasketInfo(SerializedBasketInfo& info) const {
    if (!branch || !basket || current_basket < 0) {
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
