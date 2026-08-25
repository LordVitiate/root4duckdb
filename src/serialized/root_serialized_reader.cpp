#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include "root4duckdb/serialized/root_serialized_codec_utils.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace duckdb::rootlake {

SerializedBasketReader::SerializedBasketReader() = default;
SerializedBasketReader::~SerializedBasketReader() noexcept {
    Reset();
}

SerializedBasketReader::SerializedBasketReader(SerializedBasketReader&& other) noexcept {
    *this = std::move(other);
}

SerializedBasketReader& SerializedBasketReader::operator=(SerializedBasketReader&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Reset();
    branch = other.branch;
    basket = other.basket;
    basket_prepared = other.basket_prepared;
    plan = std::move(other.plan);
    layout = std::move(other.layout);
    current_basket = other.current_basket;
    current_basket_entry_begin = other.current_basket_entry_begin;
    current_basket_entry_end = other.current_basket_entry_end;
    max_entry_bytes = other.max_entry_bytes;
    max_values_per_entry = other.max_values_per_entry;
    outer_collection_scratch = other.outer_collection_scratch;
    owns_outer_collection_scratch = other.owns_outer_collection_scratch;
    leaf = other.leaf;
    leaf_scratch = std::move(other.leaf_scratch);
    leaf_scratch_capacity = other.leaf_scratch_capacity;
    leaf_make_class = other.leaf_make_class;
    shared_basket_cache = std::move(other.shared_basket_cache);
    resolved_element_version = other.resolved_element_version;
    resolved_prefix_element_ids = std::move(other.resolved_prefix_element_ids);
    cached_action_sequence = std::move(other.cached_action_sequence);
    observed_memberwise_header = other.observed_memberwise_header;
    counters = other.counters;

    other.branch = nullptr;
    other.basket = nullptr;
    other.basket_prepared = false;
    other.outer_collection_scratch = nullptr;
    other.owns_outer_collection_scratch = false;
    other.leaf = nullptr;
    other.leaf_scratch_capacity = 0;
    other.leaf_make_class = false;
    other.resolved_element_version = -1;
    other.observed_memberwise_header = 0;
    other.counters = {};
    return *this;
}

bool SerializedBasketReader::IsBound() const {
    return branch != nullptr && plan.Supported();
}

const SerializedReadPlan& SerializedBasketReader::Plan() const {
    return plan;
}

const SerializedReadCounters& SerializedBasketReader::Counters() const {
    return counters;
}

void SerializedBasketReader::Bind(TBranch* branch_p, SerializedReadPlan plan_p, uint64_t max_entry_bytes_p,
                                  uint64_t max_values_per_entry_p,
                                  std::shared_ptr<SerializedBasketCache> shared_basket_cache_p) {
    Reset();
    branch = branch_p;
    shared_basket_cache = std::move(shared_basket_cache_p);
    if (shared_basket_cache) {
        shared_basket_cache->Bind(branch);
    }
    plan = std::move(plan_p);
    layout = {};
    layout.value_type = plan.value_type;
    layout.primitive_kind = ClassifySerializedPrimitive(plan.value_type);
    layout.bytes_before_value_per_element = plan.bytes_before_value_per_element;
    layout.value_bytes = plan.value_bytes;
    layout.fixed_array_length = plan.fixed_array_length;
    layout.array_dimensions = plan.array_dimensions;
    layout.index_depth = plan.index_depth;
    max_entry_bytes = std::max<uint64_t>(12, max_entry_bytes_p);
    max_values_per_entry = std::max<uint64_t>(1, max_values_per_entry_p);
    const bool uses_root_scratch =
        plan.Is(SerializedProjectionKind::ROOT_SELECTED_SUBTREE) ||
        plan.Is(SerializedProjectionKind::COLLECTION_BRANCH);
    auto* scratch_class = uses_root_scratch ? plan.scratch_class : plan.outer_container_class;
    if ((IsSerializedNestedProjection(plan.Kind()) ||
         uses_root_scratch) &&
        scratch_class) {
        outer_collection_scratch = scratch_class->New();
        owns_outer_collection_scratch = outer_collection_scratch != nullptr;
    }
    if (plan.Is(SerializedProjectionKind::LEAF_BRANCH) && branch) {
        auto* leaves = branch->GetListOfLeaves();
        leaf = leaves && leaves->GetEntries() == 1 ? dynamic_cast<TLeaf*>(leaves->At(0)) : nullptr;
        if (auto* branch_element = dynamic_cast<TBranchElement*>(branch)) {
            // A split TBranchElement normally writes through its parent object.
            // Decomposed mode makes this one physical leaf write directly into
            // our private scratch instead, without constructing the root event.
            leaf_make_class = branch_element->SetMakeClass(true);
            branch_element->ResetAddress();
        } else {
            branch->ResetAddress();
        }
    }
    RootDebug("SERIALIZED.BIND", "path=" + plan.logical_path +
                                     " branch=" + (branch && branch->GetName() ? branch->GetName() : "none") +
                                     " supported=" + (plan.Supported() ? "true" : "false") +
                                     " type=" + plan.value_type +
                                     " projection=" + SerializedProjectionName(plan.Kind()) +
                                     " prefix_bytes=" + std::to_string(plan.bytes_before_value_per_element));
}

void SerializedBasketReader::ReleaseBindings() noexcept {
    try {
        if (leaf && branch) {
            branch->ResetAddress();
        }
        if (leaf_make_class && branch) {
            if (auto* branch_element = dynamic_cast<TBranchElement*>(branch)) {
                branch_element->SetMakeClass(false);
            }
        }
    } catch (...) {
        // ROOT cleanup is an ABI boundary and must not escape noexcept code.
    }
    leaf_make_class = false;
    leaf_scratch.clear();
    leaf_scratch_capacity = 0;
}

void SerializedBasketReader::Reset() noexcept {
    ReleaseBindings();
    const bool uses_root_scratch =
        plan.Is(SerializedProjectionKind::ROOT_SELECTED_SUBTREE) ||
        plan.Is(SerializedProjectionKind::COLLECTION_BRANCH);
    auto* scratch_class = uses_root_scratch ? plan.scratch_class : plan.outer_container_class;
    auto* scratch = owns_outer_collection_scratch ? outer_collection_scratch : nullptr;
    branch = nullptr;
    basket = nullptr;
    basket_prepared = false;
    plan = {};
    layout = {};
    current_basket = -1;
    current_basket_entry_begin = 0;
    current_basket_entry_end = 0;
    max_entry_bytes = 0;
    max_values_per_entry = 0;
    outer_collection_scratch = nullptr;
    owns_outer_collection_scratch = false;
    leaf = nullptr;
    shared_basket_cache.reset();
    resolved_element_version = -1;
    resolved_prefix_element_ids.clear();
    cached_action_sequence.reset();
    observed_memberwise_header = 0;
    counters = {};

    try {
        if (scratch && scratch_class) {
            scratch_class->Destructor(scratch, kFALSE);
        }
    } catch (...) {
        // DCL57-CPP / ERR59-CPP: never throw across destruction or ROOT ABI.
    }
}

bool SerializedBasketReader::EnsureLeafScratch(uint64_t value_count, std::string& failure_reason) {
    if (!branch || !leaf || !plan.value_bytes) {
        failure_reason = "serialized primitive branch has no scratch type metadata";
        return false;
    }
    if (dynamic_cast<TBranchElement*>(branch) && !leaf_make_class) {
        failure_reason = "ROOT cannot decompose the primitive TBranchElement into private scratch";
        return false;
    }
    if (value_count > max_values_per_entry ||
        value_count > std::numeric_limits<uint64_t>::max() / plan.value_bytes) {
        failure_reason = "primitive physical branch value count exceeds configured safety limit";
        return false;
    }
    const uint64_t byte_count = std::max<uint64_t>(1, value_count * plan.value_bytes);
    const uint64_t alignment = sizeof(std::max_align_t);
    const uint64_t word_count = (byte_count + alignment - 1) / alignment;
    if (word_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        failure_reason = "primitive physical branch scratch size exceeds host limits";
        return false;
    }
    if (value_count > leaf_scratch_capacity) {
        leaf_scratch.resize(static_cast<size_t>(word_count));
        leaf_scratch_capacity = value_count;
    }
    branch->SetAddress(leaf_scratch.data());
    return true;
}

bool SerializedBasketReader::LoadBasket(uint64_t entry, std::string& failure_reason) {
    if (!IsBound()) {
        failure_reason = "serialized reader is not bound";
        return false;
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
    auto* basket_bytes = branch->GetBasketBytes();
    counters.compressed_bytes += basket_bytes && basket_bytes[basket_number] > 0
                                     ? static_cast<uint64_t>(basket_bytes[basket_number])
                                     : static_cast<uint64_t>(std::max(0, basket->GetNbytes()));
    RootDebug("SERIALIZED.BASKET",
              "path=" + plan.logical_path + " basket=" + std::to_string(current_basket) +
                  " entries=" + std::to_string(current_basket_entry_begin) + ".." +
                  std::to_string(current_basket_entry_end) + " compressed_bytes=" +
                  std::to_string(basket_bytes && basket_bytes[basket_number] > 0 ? basket_bytes[basket_number]
                                                                                 : std::max(0, basket->GetNbytes())));
    return true;
}

bool SerializedBasketReader::EntrySlice(uint64_t entry, const uint8_t*& begin, size_t& size,
                                        std::string& failure_reason) {
    if (shared_basket_cache) {
        bool loaded_new_basket = false;
        uint64_t compressed_bytes = 0;
        if (!shared_basket_cache->EntrySlice(entry, max_entry_bytes, begin, size, loaded_new_basket,
                                             compressed_bytes, failure_reason)) {
            return false;
        }
        if (loaded_new_basket) {
            ++counters.baskets;
            counters.compressed_bytes += compressed_bytes;
            SerializedBasketInfo info;
            if (shared_basket_cache->CurrentBasketInfo(info)) {
                RootDebug("SERIALIZED.BASKET_SHARED",
                          "path=" + plan.logical_path + " basket=" + std::to_string(info.basket_number) +
                              " entries=" + std::to_string(info.entry_begin) + ".." +
                              std::to_string(info.entry_end) + " compressed_bytes=" +
                              std::to_string(compressed_bytes));
            }
        }
        return true;
    }
    if (!LoadBasket(entry, failure_reason)) {
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

bool SerializedBasketReader::Decode(uint64_t entry, std::vector<double>& values, std::vector<int32_t>& flat_indices,
                                    std::string& failure_reason, bool collect_indices) {
    std::vector<RootPrimitiveValue> exact_values;
    const bool decoded = Decode(entry, exact_values, flat_indices, failure_reason, collect_indices);
    values.clear();
    if (!decoded) {
        return false;
    }
    values.reserve(exact_values.size());
    for (const auto& value : exact_values) {
        values.push_back(value.AsDouble());
    }
    return true;
}

bool SerializedBasketReader::Decode(uint64_t entry, std::vector<RootPrimitiveValue>& values,
                                    std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                    bool collect_indices) {
    values.clear();
    flat_indices.clear();
    if (plan.Is(SerializedProjectionKind::LEAF_BRANCH)) {
        const bool decoded = DecodeLeafBranchEntry(entry, values, flat_indices, failure_reason, collect_indices);
        if (decoded) {
            ++counters.entries;
            counters.values += values.size();
        }
        return decoded;
    }
    const uint8_t* bytes = nullptr;
    size_t entry_size = 0;
    if (!EntrySlice(entry, bytes, entry_size, failure_reason)) {
        RootDebug("SERIALIZED.DECODE_FAILURE",
                  "path=" + plan.logical_path + " entry=" + std::to_string(entry) + " reason=" + failure_reason);
        return false;
    }
    const bool uses_root_actions =
        plan.Is(SerializedProjectionKind::ROOT_SELECTED_SUBTREE) ||
        plan.Is(SerializedProjectionKind::COLLECTION_BRANCH);
    const bool decoded = uses_root_actions
                             ? DecodeRootProjectionEntry(bytes, entry_size, values, flat_indices, failure_reason,
                                                         collect_indices)
                         : IsSerializedNestedProjection(plan.Kind())
                             ? DecodeNestedProjectionEntry(bytes, entry_size, values, flat_indices, failure_reason,
                                                           collect_indices)
                             : DecodeFixedProjectionEntry(bytes, entry_size, values, flat_indices, failure_reason,
                                                          collect_indices);
    if (!decoded) {
        RootDebug("SERIALIZED.DECODE_FAILURE",
                  "path=" + plan.logical_path + " entry=" + std::to_string(entry) + " reason=" + failure_reason);
        return false;
    }
    ++counters.entries;
    counters.values += values.size();
    counters.serialized_bytes += entry_size;
    return true;
}

bool SerializedBasketReader::DecodeNestedProjectionEntry(const uint8_t* bytes, size_t entry_size,
                                                         std::vector<RootPrimitiveValue>& values,
                                                         std::vector<int32_t>& flat_indices,
                                                         std::string& failure_reason, bool collect_indices) {
    values.clear();
    flat_indices.clear();
    failure_reason.clear();
    if (!bytes || entry_size < 12) {
        failure_reason = "serialized vector entry is shorter than its header";
        return false;
    }
    if (!plan.outer_container_class || !plan.outer_element_class) {
        failure_reason = "serialized nested vector plan has no ROOT collection metadata";
        return false;
    }
    if (plan.Is(SerializedProjectionKind::SELECTED_SUBTREE) &&
        (!outer_collection_scratch || plan.projection_levels.empty() || !plan.index_depth)) {
        failure_reason = "serialized selected-subtree projection has no safe traversal metadata";
        return false;
    }
    if (entry_size > static_cast<size_t>(std::numeric_limits<Int_t>::max())) {
        failure_reason = "serialized nested vector entry exceeds ROOT buffer limits";
        return false;
    }

    serialized_codec::CheckedByteCursor header(bytes, entry_size);
    uint32_t raw_byte_count = 0;
    uint32_t memberwise_header = 0;
    if (!header.ReadBE32(raw_byte_count) || !header.ReadBE32(memberwise_header)) {
        failure_reason = "serialized vector entry is shorter than its ROOT header";
        return false;
    }
    if ((raw_byte_count & serialized_codec::ROOT_BYTE_COUNT_MASK) == 0) {
        failure_reason = "serialized vector entry has no ROOT byte-count marker";
        return false;
    }
    const uint64_t declared_size =
        static_cast<uint64_t>(raw_byte_count & serialized_codec::ROOT_BYTE_COUNT_VALUE_MASK) + 4;
    if (declared_size != entry_size) {
        failure_reason = "serialized vector byte-count does not match entry offsets";
        return false;
    }
    if (!memberwise_header) {
        failure_reason = "serialized vector has an empty member-wise header";
        return false;
    }
    if (!observed_memberwise_header) {
        observed_memberwise_header = memberwise_header;
    }
    if (memberwise_header != observed_memberwise_header) {
        failure_reason = "member-wise streamer header changed inside one physical branch";
        return false;
    }

    try {
        TBufferFile buffer(TBuffer::kRead, static_cast<Int_t>(entry_size), const_cast<uint8_t*>(bytes), kFALSE);
        if (branch && branch->GetTree() && branch->GetTree()->GetCurrentFile()) {
            buffer.SetParent(branch->GetTree()->GetCurrentFile());
        }

        UInt_t start = 0;
        UInt_t byte_count = 0;
        const Version_t collection_version = buffer.ReadVersion(&start, &byte_count, plan.outer_container_class);
        if ((collection_version & TBufferFile::kStreamedMemberWise) == 0) {
            failure_reason = "serialized outer vector is not member-wise streamed";
            return false;
        }
        if (static_cast<uint64_t>(start) + byte_count + 4 != entry_size) {
            failure_reason = "ROOT member-wise byte-count differs from basket entry offsets";
            return false;
        }
        const Version_t element_version = buffer.ReadVersionForMemberWise(plan.outer_element_class);
        if (resolved_element_version != element_version) {
            cached_action_sequence.reset();
        }
        if (!ResolveSerializedNestedVersion(plan, element_version, resolved_element_version,
                                            resolved_prefix_element_ids, failure_reason)) {
            return false;
        }
        Int_t signed_outer_count = -1;
        buffer.ReadInt(signed_outer_count);
        if (signed_outer_count < 0 || static_cast<uint64_t>(signed_outer_count) > max_values_per_entry) {
            failure_reason = "serialized outer vector count exceeds configured safety limit";
            return false;
        }
        const uint64_t outer_count = static_cast<uint32_t>(signed_outer_count);
        if (!outer_count) {
            if (static_cast<size_t>(buffer.Length()) != entry_size) {
                failure_reason = "empty serialized outer vector has unexpected member payload";
                return false;
            }
            return true;
        }

        if (!ConsumeSerializedSelectedMembers(buffer, plan, element_version, outer_count, outer_collection_scratch,
                                              resolved_prefix_element_ids, cached_action_sequence, failure_reason)) {
            return false;
        }

        if (plan.Is(SerializedProjectionKind::SELECTED_SUBTREE)) {
            return CollectSerializedSelectedSubtree(plan, outer_collection_scratch, max_values_per_entry, values,
                                                    flat_indices, failure_reason, collect_indices);
        }

        const Int_t root_offset = buffer.Length();
        if (root_offset < 0 || static_cast<size_t>(root_offset) > entry_size) {
            failure_reason = "ROOT serialized prefix actions moved outside the basket entry";
            return false;
        }
        const size_t column_offset = static_cast<size_t>(root_offset);
        RootDebug("SERIALIZED.NESTED_COLUMN",
                  "path=" + plan.logical_path + " outer_count=" + std::to_string(outer_count) + " element_version=" +
                      std::to_string(element_version) + " offset=" + std::to_string(column_offset));
        return DecodeSerializedNestedPrimitiveVectorColumn(bytes, entry_size, column_offset, outer_count, layout,
                                                           max_values_per_entry, values, flat_indices, failure_reason,
                                                           collect_indices);
    } catch (const std::exception& ex) {
        failure_reason = std::string("ROOT failed while consuming serialized member prefix: ") + ex.what();
        return false;
    } catch (...) {
        failure_reason = "ROOT failed while consuming serialized member prefix";
        return false;
    }
}

bool SerializedBasketReader::DecodeFixedProjectionEntry(const uint8_t* bytes, size_t entry_size,
                                                        std::vector<RootPrimitiveValue>& values,
                                                        std::vector<int32_t>& flat_indices,
                                                        std::string& failure_reason, bool collect_indices) {
    values.clear();
    flat_indices.clear();
    if (!bytes || !entry_size || !plan.outer_container_class || !plan.outer_element_class) {
        failure_reason = "serialized fixed projection has no ROOT collection metadata";
        return false;
    }
    if (entry_size > static_cast<size_t>(std::numeric_limits<Int_t>::max())) {
        failure_reason = "serialized fixed projection exceeds ROOT buffer limits";
        return false;
    }
    try {
        TBufferFile buffer(TBuffer::kRead, static_cast<Int_t>(entry_size), const_cast<uint8_t*>(bytes), kFALSE);
        if (branch && branch->GetTree() && branch->GetTree()->GetCurrentFile()) {
            buffer.SetParent(branch->GetTree()->GetCurrentFile());
        }
        UInt_t start = 0;
        UInt_t byte_count = 0;
        const Version_t collection_version =
            buffer.ReadVersion(&start, &byte_count, plan.outer_container_class);
        if ((collection_version & TBufferFile::kStreamedMemberWise) == 0) {
            failure_reason = "serialized fixed projection is not member-wise streamed";
            return false;
        }
        if (static_cast<uint64_t>(start) + byte_count + 4 != entry_size) {
            failure_reason = "ROOT fixed-projection byte-count differs from basket entry offsets";
            return false;
        }
        const Version_t element_version = buffer.ReadVersionForMemberWise(plan.outer_element_class);
        if (element_version < 0) {
            failure_reason = "serialized fixed projection has an invalid element version";
            return false;
        }
        if (resolved_element_version != element_version) {
            SerializedEntryLayout resolved_layout;
            if (!ResolveSerializedFixedLayout(plan, element_version, resolved_layout, failure_reason)) {
                return false;
            }
            layout = std::move(resolved_layout);
            resolved_element_version = element_version;
        }
        Int_t signed_count = -1;
        buffer.ReadInt(signed_count);
        if (signed_count < 0) {
            failure_reason = "serialized fixed projection has a negative collection count";
            return false;
        }
        const Int_t payload_cursor = buffer.Length();
        if (payload_cursor < 0 || static_cast<size_t>(payload_cursor) > entry_size) {
            failure_reason = "ROOT fixed-projection header moved outside the basket entry";
            return false;
        }
        RootDebug("SERIALIZED.FIXED_COLUMN",
                  "path=" + plan.logical_path + " count=" + std::to_string(signed_count) +
                      " element_version=" + std::to_string(element_version) +
                      " offset=" + std::to_string(payload_cursor));
        return DecodeSerializedVectorPayload(bytes, entry_size, static_cast<size_t>(payload_cursor),
                                             static_cast<uint32_t>(signed_count), layout, max_values_per_entry,
                                             values, flat_indices, failure_reason, collect_indices);
    } catch (const std::exception& ex) {
        failure_reason = std::string("ROOT failed while parsing the fixed-projection header: ") + ex.what();
        return false;
    } catch (...) {
        failure_reason = "ROOT failed while parsing the fixed-projection header";
        return false;
    }
}

bool SerializedBasketReader::DecodeRootProjectionEntry(const uint8_t* bytes, size_t entry_size,
                                                       std::vector<RootPrimitiveValue>& values,
                                                       std::vector<int32_t>& flat_indices,
                                                       std::string& failure_reason, bool collect_indices) {
    values.clear();
    flat_indices.clear();
    if (!bytes || !entry_size || !plan.scratch_class || !outer_collection_scratch ||
        plan.root_action_ids.empty() || plan.projection_levels.empty()) {
        failure_reason = "serialized root-class projection has no safe action metadata";
        return false;
    }
    if (entry_size > static_cast<size_t>(std::numeric_limits<Int_t>::max())) {
        failure_reason = "serialized root-class entry exceeds ROOT buffer limits";
        return false;
    }
    try {
        TBufferFile buffer(TBuffer::kRead, static_cast<Int_t>(entry_size), const_cast<uint8_t*>(bytes), kFALSE);
        if (branch && branch->GetTree() && branch->GetTree()->GetCurrentFile()) {
            buffer.SetParent(branch->GetTree()->GetCurrentFile());
        }
        // TBranchElement::FillLeavesMember writes its action sequence directly
        // into the basket entry.  For an unsplit root branch the plan contains
        // prefix+target; for a self-contained split member it contains only
        // that member.  Neither form adds an outer ReadVersion()/byte-count
        // frame. GetInfo() owns the on-file action layout matching bytes[0].
        auto* branch_element = dynamic_cast<TBranchElement*>(branch);
        auto* streamer = branch_element ? branch_element->GetInfo() : nullptr;
        if (!streamer) {
            failure_reason = "ROOT branch has no on-file streamer info for root-class actions";
            return false;
        }
        auto* elements = streamer ? streamer->GetElements() : nullptr;
        const int selected_id = plan.root_action_ids.empty() ? -1 : plan.root_action_ids.back();
        auto* selected_element = elements && selected_id >= 0 && selected_id < elements->GetEntries()
                                     ? dynamic_cast<TStreamerElement*>(elements->At(selected_id))
                                     : nullptr;
        if (!selected_element || plan.projected_member_name != selected_element->GetName()) {
            failure_reason = "serialized root-class selected action changed in the on-file schema";
            return false;
        }
        // This mirrors TBranchElement::SetReadActionSequence for fType 0..2:
        // basket members use the non-collection member-wise action set even
        // when ApplySequence targets a single in-memory object.
        if (!cached_action_sequence) {
            auto* all_actions = streamer->GetReadMemberWiseActions(kFALSE);
            if (!all_actions) {
                failure_reason = "ROOT has no member-wise actions for serialized root-class projection";
                return false;
            }
            cached_action_sequence.reset(all_actions->CreateSubSequence(plan.root_action_ids, 0));
            if (!cached_action_sequence) {
                failure_reason = "ROOT could not build root-class selected action sequence";
                return false;
            }
        }
        buffer.ApplySequence(*cached_action_sequence, outer_collection_scratch);
        const Int_t consumed = buffer.Length();
        if (consumed < 0 || static_cast<size_t>(consumed) > entry_size) {
            failure_reason = "ROOT root-class selected actions moved outside the basket entry";
            return false;
        }
        return CollectSerializedSelectedSubtree(plan, outer_collection_scratch, max_values_per_entry, values,
                                                flat_indices, failure_reason, collect_indices);
    } catch (const std::exception& ex) {
        failure_reason = std::string("ROOT failed while consuming root-class selected actions: ") + ex.what();
        return false;
    } catch (...) {
        failure_reason = "ROOT failed while consuming root-class selected actions";
        return false;
    }
}

bool SerializedBasketReader::DecodeNestedProjectionEntry(const uint8_t* bytes, size_t entry_size,
                                                         std::vector<double>& values,
                                                         std::vector<int32_t>& flat_indices,
                                                         std::string& failure_reason, bool collect_indices) {
    std::vector<RootPrimitiveValue> exact_values;
    const bool decoded = DecodeNestedProjectionEntry(bytes, entry_size, exact_values, flat_indices, failure_reason,
                                                     collect_indices);
    values.clear();
    if (!decoded) {
        return false;
    }
    values.reserve(exact_values.size());
    for (const auto& value : exact_values) {
        values.push_back(value.AsDouble());
    }
    return true;
}

bool SerializedBasketReader::CurrentBasketInfo(SerializedBasketInfo& info) const {
    if (shared_basket_cache) {
        return shared_basket_cache->CurrentBasketInfo(info);
    }
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
