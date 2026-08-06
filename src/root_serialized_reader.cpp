#include "root_serialized_reader.hpp"

#include "root_debug.hpp"

#include "TBranch.h"
#include "TBasket.h"
#include "TBuffer.h"

#include <algorithm>
#include <utility>

namespace duckdb::rootlake {

void SerializedBasketReader::Bind(TBranch *branch_p, SerializedReadPlan plan_p,
                                  uint64_t max_entry_bytes_p,
                                  uint64_t max_values_per_entry_p) {
    branch = branch_p;
    basket = nullptr;
    basket_prepared = false;
    plan = std::move(plan_p);
    layout = {};
    layout.value_type = plan.value_type;
    layout.primitive_kind = ClassifySerializedPrimitive(plan.value_type);
    layout.bytes_before_value_per_element = plan.bytes_before_value_per_element;
    layout.value_bytes = plan.value_bytes;
    layout.fixed_array_length = plan.fixed_array_length;
    layout.array_dimensions = plan.array_dimensions;
    layout.index_depth = plan.index_depth;
    current_basket = -1;
    current_basket_entry_begin = 0;
    current_basket_entry_end = 0;
    max_entry_bytes = std::max<uint64_t>(12, max_entry_bytes_p);
    max_values_per_entry = std::max<uint64_t>(1, max_values_per_entry_p);
    observed_memberwise_header = 0;
    counters = {};
    RootDebug("SERIALIZED.BIND",
              "path=" + plan.logical_path +
              " branch=" + (branch && branch->GetName() ? branch->GetName() : "none") +
              " supported=" + (plan.supported ? "true" : "false") +
              " type=" + plan.value_type +
              " prefix_bytes=" + std::to_string(plan.bytes_before_value_per_element));
}

bool SerializedBasketReader::LoadBasket(uint64_t entry, std::string &failure_reason) {
    if (!IsBound()) {
        failure_reason = "serialized reader is not bound";
        return false;
    }
    const int basket_count = branch->GetWriteBasket() + 1;
    auto *basket_entries = branch->GetBasketEntry();
    if (basket_count <= 0 || !basket_entries) {
        failure_reason = "physical branch has no basket entry map";
        return false;
    }
    const auto *upper = std::upper_bound(basket_entries, basket_entries + basket_count,
                                         static_cast<Long64_t>(entry));
    int basket_number = static_cast<int>(upper - basket_entries) - 1;
    if (basket_number < 0) basket_number = 0;
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
    if (basket_number == current_basket && basket) return true;

    // The old pointer is no longer used. Drop completed baskets before loading
    // the next one; dropping after GetBasket could invalidate the fresh pointer
    // on ROOT versions where GetBasket does not update fReadBasket.
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
    ++counters.baskets;
    auto *basket_bytes = branch->GetBasketBytes();
    counters.compressed_bytes += basket_bytes && basket_bytes[basket_number] > 0
                                     ? static_cast<uint64_t>(basket_bytes[basket_number])
                                     : static_cast<uint64_t>(std::max(0, basket->GetNbytes()));
    RootDebug("SERIALIZED.BASKET",
              "path=" + plan.logical_path +
              " basket=" + std::to_string(current_basket) +
              " entries=" + std::to_string(current_basket_entry_begin) + ".." +
              std::to_string(current_basket_entry_end) +
              " compressed_bytes=" + std::to_string(
                  basket_bytes && basket_bytes[basket_number] > 0
                      ? basket_bytes[basket_number] : std::max(0, basket->GetNbytes())));
    return true;
}

bool SerializedBasketReader::EntrySlice(uint64_t entry, const uint8_t *&begin, size_t &size,
                                        std::string &failure_reason) {
    if (!LoadBasket(entry, failure_reason)) return false;
    if (!basket_prepared) {
        basket->PrepareBasket(static_cast<Long64_t>(entry));
        basket_prepared = true;
    }
    auto *buffer = basket->GetBufferRef();
    auto *offsets = basket->GetEntryOffset();
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
    const int end_offset = local + 1 < static_cast<uint64_t>(basket->GetNevBuf())
                               ? offsets[local + 1] : basket->GetLast();
    if (begin_offset < 0 || end_offset < begin_offset || end_offset > buffer->BufferSize()) {
        failure_reason = "invalid decompressed entry offsets";
        return false;
    }
    size = static_cast<size_t>(end_offset - begin_offset);
    if (size > max_entry_bytes) {
        failure_reason = "serialized entry exceeds configured safety limit";
        return false;
    }
    begin = reinterpret_cast<const uint8_t *>(buffer->Buffer()) + begin_offset;
    return true;
}

bool SerializedBasketReader::Decode(uint64_t entry, std::vector<double> &values,
                                    std::vector<int32_t> &flat_indices,
                                    std::string &failure_reason,
                                    bool collect_indices) {
    values.clear();
    flat_indices.clear();
    const uint8_t *bytes = nullptr;
    size_t entry_size = 0;
    if (!EntrySlice(entry, bytes, entry_size, failure_reason)) {
        RootDebug("SERIALIZED.DECODE_FAILURE",
                  "path=" + plan.logical_path + " entry=" + std::to_string(entry) +
                  " reason=" + failure_reason);
        return false;
    }
    if (!DecodeSerializedVectorEntry(bytes, entry_size, layout, max_values_per_entry,
                                     observed_memberwise_header, values, flat_indices,
                                     failure_reason, collect_indices)) {
        RootDebug("SERIALIZED.DECODE_FAILURE",
                  "path=" + plan.logical_path + " entry=" + std::to_string(entry) +
                  " reason=" + failure_reason);
        return false;
    }
    ++counters.entries;
    counters.values += values.size();
    counters.serialized_bytes += entry_size;
    return true;
}

bool SerializedBasketReader::CurrentBasketInfo(SerializedBasketInfo &info) const {
    if (!branch || !basket || current_basket < 0) return false;
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
