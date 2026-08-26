#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

namespace duckdb::rootlake {

namespace {

bool EnvironmentFlagEnabled(const char* name, bool default_value) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value != "0" && value != "false" && value != "off" && value != "no";
}

uint64_t FixedArrayLength(TStreamerElement* element, std::vector<uint32_t>* dimensions = nullptr) {
    if (!element) {
        return 0;
    }
    const int rank = element->GetArrayDim();
    if (rank <= 0) {
        return 1;
    }
    uint64_t length = 1;
    for (int dim = 0; dim < rank; ++dim) {
        const int extent = element->GetMaxIndex(dim);
        if (extent <= 0 || length > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(extent)) {
            return 0;
        }
        if (dimensions) {
            dimensions->push_back(static_cast<uint32_t>(extent));
        }
        length *= static_cast<uint64_t>(extent);
    }
    return length;
}

bool FixedPrimitiveWidth(TStreamerElement* element, uint32_t& width, std::string& reason) {
    if (!element) {
        reason = "null streamer element";
        return false;
    }
    if (element->IsaPointer()) {
        reason = "pointer before projected member";
        return false;
    }
    int type_code = element->GetType();
    while (type_code >= 20 && type_code < 60) {
        type_code -= 20;
    }
    const std::string raw_type = element->GetTypeName() ? element->GetTypeName() : "";
    const std::string type =
        StreamerPrimitiveType(element->GetType(), element->GetTypeName() ? element->GetTypeName() : "");
    if (type_code == 9 || type_code == 19 || raw_type == "Float16_t" || raw_type == "Double32_t") {
        reason = "compressed floating streamer element before projected member";
        return false;
    }
    const auto primitive = PrimitiveTypeSize(type);
    const auto length = FixedArrayLength(element);
    if (!primitive || !length || length > std::numeric_limits<uint32_t>::max() / primitive) {
        reason = "variable-width or unsupported streamer element before projected member: " +
                 std::string(element->GetName());
        return false;
    }
    width = primitive * static_cast<uint32_t>(length);
    return true;
}

bool StreamerContainsField(TClass* klass, const std::string& field, std::set<std::string>& active) {
    if (!klass || !active.insert(klass->GetName()).second) {
        return false;
    }
    auto* streamer = klass->GetStreamerInfo();
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (element && !element->IsBase() && field == element->GetName()) {
            active.erase(klass->GetName());
            return true;
        }
    }
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element || !element->IsBase()) {
            continue;
        }
        auto* base = element->GetClassPointer();
        if (!base) {
            base = TClass::GetClass(element->GetTypeName());
        }
        if (StreamerContainsField(base, field, active)) {
            active.erase(klass->GetName());
            return true;
        }
    }
    active.erase(klass->GetName());
    return false;
}

int RootSelectedElement(TClass* root_class, const std::string& field) {
    auto* streamer = root_class ? root_class->GetStreamerInfo() : nullptr;
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (element && !element->IsBase() && field == element->GetName()) {
            return i;
        }
    }
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element || !element->IsBase()) {
            continue;
        }
        auto* base = element->GetClassPointer();
        if (!base) {
            base = TClass::GetClass(element->GetTypeName());
        }
        std::set<std::string> active;
        if (StreamerContainsField(base, field, active)) {
            return i;
        }
    }
    return -1;
}

bool IsTopObjectBranch(TBranch* branch, TClass* root_class) {
    auto* element = dynamic_cast<TBranchElement*>(branch);
    if (!element || !root_class) {
        return false;
    }
    const std::string branch_class = element->GetClassName() ? element->GetClassName() : "";
    // Child TBranchElement objects may still report the owning root class.
    // GetMother() is the stable distinction: a top-level branch is its own
    // mother, while every split child points back to the top-level branch.
    return branch->GetMother() == branch && branch_class == root_class->GetName();
}

bool ConfigureLeafProjection(const ParsedPath& path, const std::vector<PathLevel>& levels, TBranch* branch,
                             SerializedReadPlan& plan) {
    auto* children = branch ? branch->GetListOfBranches() : nullptr;
    auto* leaves = branch ? branch->GetListOfLeaves() : nullptr;
    if (!branch || (children && children->GetEntries() != 0) || !leaves || leaves->GetEntries() != 1 ||
        levels.empty() || !levels.back().is_primitive || levels.back().is_pointer) {
        return false;
    }
    auto* leaf = dynamic_cast<TLeaf*>(leaves->At(0));
    const std::string leaf_type = leaf && leaf->GetTypeName() ? leaf->GetTypeName() : "";
    // ROOT exposes compressed floating leaves by their persistent typedefs,
    // while PathResolver deliberately maps them to their decoded in-memory
    // Float_t/Double_t types. MakeClass reads them into those native scratch
    // types, so they are safe leaf projections even though their serialized
    // width is variable and must never be used for prefix arithmetic.
    const bool compressed_float_leaf = leaf_type == "Float16_t" || leaf_type == "Double32_t";
    if (!leaf || (!IsPrimitiveType(leaf_type) && !compressed_float_leaf)) {
        return false;
    }
    size_t container_depth = 0;
    for (const auto& level : levels) {
        container_depth += level.is_container ? 1U : 0U;
    }
    if (container_depth > 1) {
        return false;
    }
    const auto value_type = PrimitiveBaseType(levels.back().type);
    if (!PrimitiveTypeSize(value_type)) {
        return false;
    }
    plan.Select(SerializedProjectionKind::LEAF_BRANCH);
    plan.value_type = value_type;
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.projection_levels = levels;
    plan.value_bytes = PrimitiveTypeSize(value_type);
    plan.index_depth = IndexDepth(levels);
    return true;
}

bool ConfigureCollectionBranchProjection(TClass* root_class, const ParsedPath& path,
                                         const std::vector<PathLevel>& levels, TBranch* branch,
                                         SerializedReadPlan& plan) {
    auto* branch_element = dynamic_cast<TBranchElement*>(branch);
    auto* children = branch ? branch->GetListOfBranches() : nullptr;
    if (!root_class || !branch_element || (children && children->GetEntries() != 0) || levels.size() != 2 ||
        !levels[0].is_container || levels[0].is_pointer || levels[0].element_class ||
        !levels[1].is_primitive || levels[1].is_pointer || levels[1].is_container ||
        path.fields.size() != 2 || path.fields[1] != "value" ||
        !BranchNameEndsWithToken(branch->GetName() ? branch->GetName() : "", levels[0].name)) {
        return false;
    }
    const auto container_kind = TemplatePrimaryName(levels[0].type);
    const bool primitive_vector = container_kind == "vector";
    const bool primitive_set = container_kind == "set" || container_kind == "multiset" ||
                               container_kind == "unordered_set" || container_kind == "unordered_multiset";
    if (!primitive_vector && !primitive_set) {
        return false;
    }
    auto* proxy = branch_element->GetCollectionProxy();
    if (!proxy && levels[0].klass) {
        proxy = levels[0].klass->GetCollectionProxy();
    }
    if (!proxy || proxy->GetValueClass() || proxy->HasPointers()) {
        return false;
    }
    const auto value_type = PrimitiveBaseType(levels[1].type);
    // std::vector<bool> is bit-packed and is not compatible with the ordinary
    // primitive collection traversal used by OffsetValueReader.
    if ((primitive_vector && (value_type == "bool" || value_type == "Bool_t")) ||
        !PrimitiveTypeSize(value_type) ||
        ClassifySerializedPrimitive(value_type) == SerializedPrimitiveKind::UNKNOWN) {
        return false;
    }
    auto* streamer = branch_element->GetInfo();
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    const int selected_id = branch_element->GetID();
    auto* selected_element = elements && selected_id >= 0 && selected_id < elements->GetEntries()
                                 ? dynamic_cast<TStreamerElement*>(elements->At(selected_id))
                                 : nullptr;
    if (!selected_element || levels[0].name != selected_element->GetName()) {
        return false;
    }

    plan.Select(SerializedProjectionKind::COLLECTION_BRANCH);
    plan.value_type = value_type;
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.scratch_class = root_class;
    plan.projected_member_name = selected_element->GetName();
    plan.root_action_ids = {selected_id};
    plan.projection_levels = levels;
    // FillLeavesMember writes only this branch's selected action to its
    // basket.  Decode that same action at byte zero into a private root-class
    // scratch object; the original PathLevel offsets therefore stay intact.
    plan.value_bytes = PrimitiveTypeSize(value_type);
    plan.index_depth = IndexDepth(levels);
    return true;
}

bool ConfigureInlineObjectBranchProjection(TClass* root_class, const ParsedPath& path,
                                           const std::vector<PathLevel>& levels, TBranch* branch,
                                           SerializedReadPlan& plan) {
    auto* branch_element = dynamic_cast<TBranchElement*>(branch);
    auto* children = branch ? branch->GetListOfBranches() : nullptr;
    if (!root_class || !branch_element || branch->GetMother() == branch ||
        (children && children->GetEntries() != 0) || levels.size() < 2 || path.fields.size() != levels.size() ||
        levels.front().is_primitive || levels.front().is_string || levels.front().is_container ||
        levels.front().is_pointer || !levels.front().klass || !levels.back().is_primitive ||
        levels.back().is_pointer ||
        !BranchNameEndsWithToken(branch->GetName() ? branch->GetName() : "", levels.front().name)) {
        return false;
    }
    if (root_class->HasCustomStreamerMember() || root_class->GetStreamer() ||
        levels.front().klass->HasCustomStreamerMember() || levels.front().klass->GetStreamer() ||
        std::any_of(levels.begin(), levels.end(), [](const PathLevel& level) { return level.is_pointer; })) {
        return false;
    }
    const auto value_type = PrimitiveBaseType(levels.back().type);
    if (!PrimitiveTypeSize(value_type) ||
        ClassifySerializedPrimitive(value_type) == SerializedPrimitiveKind::UNKNOWN) {
        return false;
    }
    auto* streamer = branch_element->GetInfo();
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    const int selected_id = branch_element->GetID();
    auto* selected_element = elements && selected_id >= 0 && selected_id < elements->GetEntries()
                                 ? dynamic_cast<TStreamerElement*>(elements->At(selected_id))
                                 : nullptr;
    if (!selected_element || selected_element->IsaPointer() ||
        levels.front().name != selected_element->GetName()) {
        return false;
    }
    auto* selected_class = selected_element->GetClassPointer();
    if (!selected_class || std::string(selected_class->GetName()) != levels.front().klass->GetName()) {
        return false;
    }

    plan.Select(SerializedProjectionKind::ROOT_SELECTED_SUBTREE);
    plan.value_type = value_type;
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.scratch_class = root_class;
    plan.projected_member_name = selected_element->GetName();
    plan.root_action_ids = {selected_id};
    plan.projection_levels = levels;
    plan.streamer_version = static_cast<uint32_t>(std::max(0, static_cast<int>(root_class->GetClassVersion())));
    plan.value_bytes = PrimitiveTypeSize(value_type);
    plan.index_depth = IndexDepth(levels);
    return true;
}

bool ConfigureRootProjection(TClass* root_class, const ParsedPath& path, const std::vector<PathLevel>& levels,
                             SerializedReadPlan& plan, std::string& reason) {
    if (!root_class || levels.empty() || levels.front().is_pointer || !levels.back().is_primitive) {
        reason = "unsplit root projection requires a non-pointer numeric path";
        return false;
    }
    if (root_class->HasCustomStreamerMember() || root_class->GetStreamer()) {
        reason = "unsplit top-level class uses a custom streamer";
        return false;
    }
    const int selected = RootSelectedElement(root_class, path.fields.front());
    if (selected < 0) {
        reason = "top-level selected member is absent from ROOT streamer actions";
        return false;
    }
    plan.Select(SerializedProjectionKind::ROOT_SELECTED_SUBTREE);
    plan.value_type = PrimitiveBaseType(levels.back().type);
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.scratch_class = root_class;
    plan.projection_levels = levels;
    auto* root_streamer = root_class->GetStreamerInfo();
    auto* root_elements = root_streamer ? root_streamer->GetElements() : nullptr;
    auto* selected_element = root_elements && selected < root_elements->GetEntries()
                                 ? dynamic_cast<TStreamerElement*>(root_elements->At(selected))
                                 : nullptr;
    if (!selected_element) {
        reason = "top-level selected ROOT action has no persistent element metadata";
        return false;
    }
    plan.projected_member_name = selected_element->GetName();
    plan.root_action_ids.reserve(static_cast<size_t>(selected + 1));
    for (int i = 0; i <= selected; ++i) {
        plan.root_action_ids.push_back(i);
    }
    plan.streamer_version = static_cast<uint32_t>(std::max(0, static_cast<int>(root_class->GetClassVersion())));
    plan.value_bytes = PrimitiveTypeSize(plan.value_type);
    plan.index_depth = IndexDepth(levels);
    return plan.value_bytes != 0;
}

} // namespace

RootReaderMode ParseRootReaderMode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode.empty() || mode == "auto") {
        return RootReaderMode::AUTO;
    }
    if (mode == "serialized" || mode == "raw" || mode == "vectorized") {
        return RootReaderMode::SERIALIZED;
    }
    if (mode == "object" || mode == "fallback" || mode == "universal") {
        return RootReaderMode::OBJECT;
    }
    throw InvalidInputException("reader_mode must be one of: auto, serialized, object");
}

const char* RootReaderModeName(RootReaderMode mode) {
    switch (mode) {
    case RootReaderMode::AUTO:
        return "auto";
    case RootReaderMode::SERIALIZED:
        return "serialized";
    case RootReaderMode::OBJECT:
        return "object";
    }
    return "unknown";
}

SerializedReadPlan BuildSerializedReadPlan(TClass* root_class, const ParsedPath& path, TBranch* physical_branch) {
    SerializedReadPlan plan;
    plan.logical_path = "/" + path.root_class + "/" + JoinStrings(path.fields, "/");
    plan.root_class = path.root_class;
    plan.physical_branch_name = physical_branch && physical_branch->GetName() ? physical_branch->GetName() : "";
    auto reject = [&](std::string reason) {
        plan.Reject(std::move(reason));
        return plan;
    };

    if (!root_class || !physical_branch) {
        return reject("missing ROOT class or physical branch");
    }
    std::vector<PathLevel> levels;
    try {
        levels = PathResolver::Resolve(root_class, path.fields);
    } catch (const std::exception& ex) {
        return reject(std::string("cannot resolve serialized path: ") + ex.what());
    }
    if (IsTopObjectBranch(physical_branch, root_class)) {
        std::string reason;
        if (ConfigureRootProjection(root_class, path, levels, plan, reason)) {
            return plan;
        }
        return reject(std::move(reason));
    }
    if (ConfigureLeafProjection(path, levels, physical_branch, plan)) {
        return plan;
    }
    if (ConfigureCollectionBranchProjection(root_class, path, levels, physical_branch, plan)) {
        return plan;
    }
    if (ConfigureInlineObjectBranchProjection(root_class, path, levels, physical_branch, plan)) {
        return plan;
    }
    if (physical_branch->GetListOfBranches() && physical_branch->GetListOfBranches()->GetEntries() > 0) {
        return reject("physical ancestor is split into persistent child branches");
    }
    if (levels.empty() || !levels[0].is_container || !IsContiguousVectorType(levels[0].type) || levels[0].is_pointer ||
        !levels[0].element_class) {
        return reject("outer field is not a supported primitive collection or std::vector<object>");
    }
    if (!BranchNameEndsWithToken(plan.physical_branch_name, levels[0].name)) {
        return reject("physical branch '" + plan.physical_branch_name + "' is not the vector ancestor '" +
                      levels[0].name + "'");
    }
    const auto outer_offset = levels[0].offset_in_parent;
    const int root_size = root_class->Size();
    const int outer_container_size = levels[0].klass ? levels[0].klass->Size() : 0;
    if (outer_offset < 0 || !levels[0].klass || root_size <= 0 || outer_container_size <= 0 ||
        outer_offset > root_size || outer_container_size > root_size - outer_offset) {
        return reject("outer vector memory offset is outside the ROOT object");
    }

    auto* element_class = levels[0].element_class;
    auto* streamer = element_class->GetStreamerInfo();
    if (!streamer) {
        return reject("vector element class has no TStreamerInfo");
    }
    auto* elements = streamer->GetElements();
    if (!elements) {
        return reject("vector element streamer has no elements");
    }

    // vector<object>/vector<primitive>/value. ROOT writes one framed STL
    // member column containing, for each outer object, N followed by N dense
    // primitive values. Members before that column can be variable width, so
    // the read plan records their streamer element ids for ROOT to consume.
    const bool nested_primitive_vector =
        path.fields.size() == 3 && levels.size() == 3 && path.fields[2] == "value" &&
        levels[1].is_container && IsContiguousVectorType(levels[1].type) && !levels[1].is_pointer &&
        !levels[1].element_class && levels[2].is_primitive && !levels[2].is_pointer && !levels[2].is_container;
    if (nested_primitive_vector) {
        TStreamerElement* target = nullptr;
        int target_index = -1;
        for (int i = 0; i < elements->GetEntries(); ++i) {
            auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
            if (!element) {
                return reject("unexpected non-streamer element");
            }
            if (!element->IsBase() && path.fields[1] == element->GetName()) {
                target = element;
                target_index = i;
                break;
            }
        }
        if (!target) {
            return reject("nested vector member is inherited or absent from element streamer");
        }
        if (target->IsaPointer() || target->GetArrayDim() != 0) {
            return reject("nested vector member is a pointer or fixed array");
        }
        if (target->GetStreamer()) {
            return reject("nested vector member has a custom ROOT streamer");
        }
        auto* target_class = target->GetClassPointer();
        auto* target_proxy = target_class ? target_class->GetCollectionProxy() : nullptr;
        if (!target_proxy || target_proxy->GetValueClass() || target_proxy->HasPointers()) {
            return reject("nested vector does not contain inline primitive values");
        }
        const auto scalar_type = PrimitiveBaseType(levels[2].type);
        const auto scalar_width = PrimitiveTypeSize(scalar_type);
        if (!scalar_width || ClassifySerializedPrimitive(scalar_type) == SerializedPrimitiveKind::UNKNOWN) {
            return reject("nested vector primitive type is unsupported: " + scalar_type);
        }

        plan.Select(SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR);
        plan.container_name = levels[0].name;
        plan.element_class = element_class->GetName();
        plan.projected_member_name = path.fields[1];
        plan.value_type = scalar_type;
        plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
        plan.outer_container_class = levels[0].klass;
        plan.outer_element_class = element_class;
        for (int i = 0; i < target_index; ++i) {
            plan.prefix_element_ids.push_back(i);
        }
        plan.streamer_version = static_cast<uint32_t>(std::max(0, static_cast<int>(element_class->GetClassVersion())));
        plan.value_bytes = scalar_width;
        plan.fixed_array_length = 1;
        plan.index_depth = 2;
        return plan;
    }

    if (path.fields.size() >= 3) {
        std::string failure_reason;
        if (ConfigureSerializedDeepProjection(path, levels, element_class, plan, failure_reason)) {
            return plan;
        }
        return reject(std::move(failure_reason));
    }

    if (path.fields.size() != 2 || levels.size() != 2) {
        return reject("serialized reader requires vector<object>/primitive-member or "
                      "vector<object>/vector<primitive>/value or "
                      "vector<object>/<inline subtree>/.../primitive");
    }
    if (!levels[1].is_primitive || levels[1].is_pointer || levels[1].is_container) {
        return reject("terminal field is not a fixed primitive member");
    }

    auto selected_subtree_or_reject = [&](std::string fixed_reason) {
        std::string selected_reason;
        if (ConfigureSerializedDeepProjection(path, levels, element_class, plan, selected_reason)) {
            return plan;
        }
        return reject(std::move(fixed_reason) + "; selected action projection: " + selected_reason);
    };

    uint64_t prefix_width = 0;
    TStreamerElement* target = nullptr;
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element) {
            return reject("unexpected non-streamer element");
        }
        if (!element->IsBase() && path.fields[1] == element->GetName()) {
            target = element;
            break;
        }
        if (element->IsBase()) {
            std::string base_type = TrimType(element->GetTypeName() ? element->GetTypeName() : "");
            if (base_type.empty() || base_type == "BASE") {
                base_type = TrimType(element->GetName() ? element->GetName() : "");
            }
            // TObject contributes its version marker, unique id and bits in
            // the observed member-wise representation. Per-file validation is
            // mandatory before this constant is trusted.
            if (base_type == "TObject") {
                prefix_width += 10;
                continue;
            }
            return selected_subtree_or_reject("unsupported base class before projected member: " + base_type);
        }
        uint32_t element_width = 0;
        std::string reason;
        if (!FixedPrimitiveWidth(element, element_width, reason)) {
            return selected_subtree_or_reject(reason);
        }
        prefix_width += element_width;
        if (prefix_width > std::numeric_limits<uint32_t>::max()) {
            return reject("serialized member prefix exceeds 32-bit offset");
        }
    }
    if (!target) {
        return selected_subtree_or_reject("terminal member is inherited or absent from element streamer");
    }

    uint32_t target_width = 0;
    std::string target_reason;
    if (!FixedPrimitiveWidth(target, target_width, target_reason)) {
        return selected_subtree_or_reject(target_reason);
    }
    std::vector<uint32_t> dimensions;
    const auto array_length = FixedArrayLength(target, &dimensions);
    const auto persistent_target_type =
        StreamerPrimitiveType(target->GetType(), target->GetTypeName() ? target->GetTypeName() : "");
    const auto scalar_width = PrimitiveTypeSize(persistent_target_type);
    if (!array_length || !scalar_width || target_width != array_length * scalar_width) {
        return reject("terminal member has unsupported persistent width");
    }

    plan.Select(SerializedProjectionKind::FIXED_MEMBER);
    plan.container_name = levels[0].name;
    plan.element_class = element_class->GetName();
    plan.projected_member_name = target->GetName();
    plan.value_type = PrimitiveBaseType(persistent_target_type);
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.outer_container_class = levels[0].klass;
    plan.outer_element_class = element_class;
    plan.streamer_version = static_cast<uint32_t>(std::max(0, static_cast<int>(element_class->GetClassVersion())));
    plan.bytes_before_value_per_element = static_cast<uint32_t>(prefix_width);
    plan.value_bytes = scalar_width;
    plan.fixed_array_length = array_length;
    plan.array_dimensions = std::move(dimensions);
    plan.index_depth = 1 + (array_length > 1 ? std::max<idx_t>(1, plan.array_dimensions.size()) : 0);
    return plan;
}

bool ResolveSerializedFixedLayout(const SerializedReadPlan& plan, int32_t element_version,
                                  SerializedEntryLayout& resolved_layout, std::string& failure_reason) {
    if (!plan.Is(SerializedProjectionKind::FIXED_MEMBER) || !plan.outer_element_class ||
        plan.projected_member_name.empty() || element_version < 0) {
        failure_reason = "serialized fixed projection has no versioned member metadata";
        return false;
    }
    auto* streamer = plan.outer_element_class->GetStreamerInfo(element_version);
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements) {
        failure_reason = "ROOT has no TStreamerInfo for serialized fixed element version " +
                         std::to_string(element_version);
        return false;
    }

    uint64_t prefix_width = 0;
    TStreamerElement* target = nullptr;
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element) {
            failure_reason = "serialized fixed element version has invalid streamer metadata";
            return false;
        }
        if (!element->IsBase() && plan.projected_member_name == element->GetName()) {
            target = element;
            break;
        }
        if (element->IsBase()) {
            std::string base_type = TrimType(element->GetTypeName() ? element->GetTypeName() : "");
            if (base_type.empty() || base_type == "BASE") {
                base_type = TrimType(element->GetName() ? element->GetName() : "");
            }
            if (base_type != "TObject") {
                failure_reason = "unsupported base class before versioned fixed member: " + base_type;
                return false;
            }
            prefix_width += 10;
            continue;
        }
        uint32_t element_width = 0;
        std::string width_reason;
        if (!FixedPrimitiveWidth(element, element_width, width_reason)) {
            failure_reason = "versioned fixed member prefix is not arithmetic: " + width_reason;
            return false;
        }
        prefix_width += element_width;
        if (prefix_width > std::numeric_limits<uint32_t>::max()) {
            failure_reason = "versioned fixed member prefix exceeds 32-bit offset";
            return false;
        }
    }
    if (!target) {
        failure_reason = "projected fixed member is absent from serialized element version " +
                         std::to_string(element_version);
        return false;
    }

    uint32_t target_width = 0;
    std::string target_reason;
    if (!FixedPrimitiveWidth(target, target_width, target_reason)) {
        failure_reason = "versioned fixed target is not arithmetic: " + target_reason;
        return false;
    }
    std::vector<uint32_t> dimensions;
    const auto array_length = FixedArrayLength(target, &dimensions);
    const auto target_type = StreamerPrimitiveType(
        target->GetType(), target->GetTypeName() ? target->GetTypeName() : "");
    const auto scalar_width = PrimitiveTypeSize(target_type);
    const auto target_kind = ClassifySerializedPrimitive(PrimitiveBaseType(target_type));
    const auto planned_kind = ClassifySerializedPrimitive(plan.value_type);
    if (!array_length || !scalar_width || target_width != array_length * scalar_width ||
        target_kind == SerializedPrimitiveKind::UNKNOWN || target_kind != planned_kind ||
        scalar_width != plan.value_bytes || array_length != plan.fixed_array_length ||
        dimensions != plan.array_dimensions) {
        failure_reason = "projected fixed member changed type or shape in serialized element version " +
                         std::to_string(element_version);
        return false;
    }

    resolved_layout = {};
    resolved_layout.value_type = plan.value_type;
    resolved_layout.primitive_kind = planned_kind;
    resolved_layout.bytes_before_value_per_element = static_cast<uint32_t>(prefix_width);
    resolved_layout.value_bytes = plan.value_bytes;
    resolved_layout.fixed_array_length = plan.fixed_array_length;
    resolved_layout.array_dimensions = plan.array_dimensions;
    resolved_layout.index_depth = static_cast<size_t>(plan.index_depth);
    RootDebug("SERIALIZED.FIXED_SCHEMA", "path=" + plan.logical_path +
                                             " file_version=" + std::to_string(element_version) +
                                             " dictionary_version=" + std::to_string(plan.streamer_version) +
                                             " prefix_bytes=" + std::to_string(prefix_width));
    return true;
}

bool ResolveSerializedNestedVersion(const SerializedReadPlan& plan, int32_t element_version,
                                    int32_t& resolved_element_version, std::vector<int>& prefix_element_ids,
                                    std::string& failure_reason) {
    if (element_version < 0) {
        failure_reason = "serialized outer element version is invalid";
        return false;
    }
    if (resolved_element_version == element_version) {
        return true;
    }
    if (!plan.outer_element_class || plan.projected_member_name.empty()) {
        failure_reason = "serialized nested vector plan has no projected member metadata";
        return false;
    }
    if (static_cast<uint32_t>(element_version) == plan.streamer_version) {
        prefix_element_ids = plan.prefix_element_ids;
    } else if (!ResolveSerializedVersionedMember(plan, element_version, prefix_element_ids, failure_reason)) {
        return false;
    }
    resolved_element_version = element_version;
    RootDebug("SERIALIZED.NESTED_SCHEMA", "path=" + plan.logical_path +
                                              " file_version=" + std::to_string(element_version) +
                                              " dictionary_version=" + std::to_string(plan.streamer_version) +
                                              " prefix_elements=" + std::to_string(prefix_element_ids.size()));
    return true;
}

void WarnRootFallbackOnce(const std::string& logical_path, const std::string& schema_fingerprint,
                          const std::string& reason) {
    if (!EnvironmentFlagEnabled("ROOT4DUCKDB_FALLBACK_WARNINGS", true)) {
        return;
    }
    static std::mutex mutex;
    static std::set<std::string> emitted;
    const std::string key = logical_path + "\x1f" + schema_fingerprint + "\x1f" + reason;
    std::lock_guard<std::mutex> guard(mutex);
    if (!emitted.insert(key).second) {
        return;
    }
    std::fprintf(stderr, "[ROOT4DUCKDB][WARN][ROOT_OBJECT_FALLBACK] path=%s schema=%s reason=%s\n",
                 logical_path.c_str(), schema_fingerprint.empty() ? "unknown" : schema_fingerprint.c_str(),
                 reason.c_str());
    std::fflush(stderr);
}

} // namespace duckdb::rootlake
