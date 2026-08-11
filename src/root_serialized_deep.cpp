#include "root_serialized_reader.hpp"

#include "root_debug.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace duckdb::rootlake {

bool IsSerializedNestedProjection(SerializedProjectionKind kind) {
    return kind == SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR ||
           kind == SerializedProjectionKind::NESTED_OBJECT_VECTOR;
}

const char *SerializedProjectionName(SerializedProjectionKind kind) {
    switch (kind) {
    case SerializedProjectionKind::FIXED_MEMBER: return "fixed_member";
    case SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR:
        return "nested_primitive_vector";
    case SerializedProjectionKind::NESTED_OBJECT_VECTOR:
        return "nested_object_vector";
    }
    return "unknown";
}

bool ResolveSerializedVersionedMember(const SerializedReadPlan &plan,
                                      int32_t element_version,
                                      std::vector<int> &prefix_element_ids,
                                      std::string &failure_reason) {
    auto *streamer = plan.outer_element_class->GetStreamerInfo(element_version);
    auto *elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements) {
        failure_reason = "ROOT has no TStreamerInfo for serialized outer element version " +
                         std::to_string(element_version);
        return false;
    }

    TStreamerElement *target = nullptr;
    int target_index = -1;
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element) {
            failure_reason = "serialized outer element version has invalid streamer metadata";
            return false;
        }
        if (!element->IsBase() && plan.projected_member_name == element->GetName()) {
            target = element;
            target_index = i;
            break;
        }
    }
    if (!target) {
        failure_reason = "projected nested vector member is absent from serialized outer "
                         "element version " + std::to_string(element_version);
        return false;
    }
    if (target->IsaPointer() || target->GetArrayDim() != 0 ||
        (plan.projection_kind == SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR &&
         target->GetStreamer())) {
        failure_reason = "projected member has an unsupported layout in serialized outer "
                         "element version " + std::to_string(element_version);
        return false;
    }

    const std::string target_type = target->GetTypeName() ? target->GetTypeName() : "";
    const auto arguments = TemplateArguments(target_type);
    if (!IsContiguousVectorType(target_type) || arguments.empty()) {
        failure_reason = "projected member is not std::vector in serialized outer element "
                         "version " + std::to_string(element_version);
        return false;
    }
    auto *target_class = target->GetClassPointer();
    auto *target_proxy = target_class ? target_class->GetCollectionProxy() : nullptr;
    if (!target_proxy || target_proxy->HasPointers()) {
        failure_reason = "projected nested vector is not an inline collection in serialized "
                         "outer element version " + std::to_string(element_version);
        return false;
    }
    if (plan.projection_kind == SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR) {
        const auto scalar_type = PrimitiveBaseType(arguments.front());
        const auto scalar_kind = ClassifySerializedPrimitive(scalar_type);
        if (target_proxy->GetValueClass() ||
            scalar_kind == SerializedPrimitiveKind::UNKNOWN ||
            scalar_kind != ClassifySerializedPrimitive(plan.value_type) ||
            PrimitiveTypeSize(scalar_type) != plan.value_bytes) {
            failure_reason = "projected nested vector primitive type changed in serialized "
                             "outer element version " + std::to_string(element_version) +
                             " (file=" + scalar_type + ", dictionary=" + plan.value_type + ")";
            return false;
        }
    } else if (plan.projection_kind == SerializedProjectionKind::NESTED_OBJECT_VECTOR) {
        if (!target_proxy->GetValueClass()) {
            failure_reason = "projected nested vector no longer contains inline objects in "
                             "serialized outer element version " +
                             std::to_string(element_version);
            return false;
        }
    } else {
        failure_reason = "serialized nested schema resolver received an invalid projection";
        return false;
    }

    prefix_element_ids.clear();
    prefix_element_ids.reserve(static_cast<size_t>(target_index));
    for (int i = 0; i < target_index; ++i) prefix_element_ids.push_back(i);
    return true;
}

bool ConfigureSerializedDeepProjection(const ParsedPath &path,
                                       const std::vector<PathLevel> &levels,
                                       TClass *outer_element_class,
                                       SerializedReadPlan &plan,
                                       std::string &failure_reason) {
    // vector<object>/vector<object>/vector<primitive>/value. ROOT consumes the
    // first nested object-vector member with its version-aware action. Only
    // that subtree is materialized before the offset walker projects values.
    if (!outer_element_class || levels.size() != 4 || path.fields.size() != 4 ||
        path.fields[3] != "value" || !levels[1].is_container ||
        !IsContiguousVectorType(levels[1].type) || levels[1].is_pointer ||
        !levels[1].element_class || !levels[2].is_container ||
        !IsContiguousVectorType(levels[2].type) || levels[2].is_pointer ||
        levels[2].element_class || !levels[3].is_primitive ||
        levels[3].is_pointer || levels[3].is_container) {
        failure_reason = "serialized deep projection requires "
                         "vector<object>/vector<object>/vector<primitive>/value";
        return false;
    }

    auto *streamer = outer_element_class->GetStreamerInfo();
    auto *elements = streamer ? streamer->GetElements() : nullptr;
    TStreamerElement *object_vector = nullptr;
    int object_vector_index = -1;
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element) {
            failure_reason = "unexpected non-streamer element";
            return false;
        }
        if (!element->IsBase() && path.fields[1] == element->GetName()) {
            object_vector = element;
            object_vector_index = i;
            break;
        }
    }
    if (!object_vector || object_vector->IsaPointer() ||
        object_vector->GetArrayDim() != 0) {
        failure_reason = "nested object vector is absent, a pointer or a fixed array";
        return false;
    }
    auto *object_vector_class = object_vector->GetClassPointer();
    auto *object_vector_proxy = object_vector_class
                                    ? object_vector_class->GetCollectionProxy() : nullptr;
    if (!object_vector_proxy || object_vector_proxy->HasPointers() ||
        object_vector_proxy->GetValueClass() != levels[1].element_class) {
        failure_reason = "nested object vector does not contain inline dictionary objects";
        return false;
    }

    auto *nested_streamer = levels[1].element_class->GetStreamerInfo();
    auto *nested_elements = nested_streamer ? nested_streamer->GetElements() : nullptr;
    TStreamerElement *primitive_vector = nullptr;
    for (int i = 0; nested_elements && i < nested_elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(nested_elements->At(i));
        if (!element) {
            failure_reason = "unexpected nested non-streamer element";
            return false;
        }
        if (!element->IsBase() && path.fields[2] == element->GetName()) {
            primitive_vector = element;
            break;
        }
    }
    if (!primitive_vector || primitive_vector->IsaPointer() ||
        primitive_vector->GetArrayDim() != 0) {
        failure_reason = "nested primitive vector is absent, a pointer or a fixed array";
        return false;
    }
    auto *primitive_vector_class = primitive_vector->GetClassPointer();
    auto *primitive_vector_proxy = primitive_vector_class
                                       ? primitive_vector_class->GetCollectionProxy() : nullptr;
    if (!primitive_vector_proxy || primitive_vector_proxy->GetValueClass() ||
        primitive_vector_proxy->HasPointers()) {
        failure_reason = "terminal nested vector does not contain inline primitive values";
        return false;
    }

    const auto scalar_type = PrimitiveBaseType(levels[3].type);
    const auto scalar_width = PrimitiveTypeSize(scalar_type);
    if (!scalar_width ||
        ClassifySerializedPrimitive(scalar_type) == SerializedPrimitiveKind::UNKNOWN) {
        failure_reason = "deep nested vector primitive type is unsupported: " + scalar_type;
        return false;
    }

    plan.supported = true;
    plan.reason.clear();
    plan.projection_kind = SerializedProjectionKind::NESTED_OBJECT_VECTOR;
    plan.container_name = levels[0].name;
    plan.element_class = outer_element_class->GetName();
    plan.projected_member_name = path.fields[1];
    plan.value_type = scalar_type;
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.outer_container_class = levels[0].klass;
    plan.outer_element_class = outer_element_class;
    plan.outer_container_offset = levels[0].offset_in_parent;
    for (int i = 0; i < object_vector_index; ++i) plan.prefix_element_ids.push_back(i);
    plan.projection_levels = levels;
    plan.streamer_version = static_cast<uint32_t>(
        std::max(0, static_cast<int>(outer_element_class->GetClassVersion())));
    plan.value_bytes = scalar_width;
    plan.fixed_array_length = 1;
    plan.index_depth = 3;
    return true;
}

bool ConsumeSerializedSelectedMembers(TBufferFile &buffer,
                                      const SerializedReadPlan &plan,
                                      int32_t element_version,
                                      uint64_t outer_count,
                                      void *outer_collection_scratch,
                                      const std::vector<int> &prefix_element_ids,
                                      std::string &failure_reason) {
    std::vector<int> action_element_ids = prefix_element_ids;
    if (plan.projection_kind == SerializedProjectionKind::NESTED_OBJECT_VECTOR) {
        // Prefix ids are consecutive; the selected object-vector is next.
        action_element_ids.push_back(static_cast<int>(prefix_element_ids.size()));
    }
    if (action_element_ids.empty()) return true;
    if (!outer_collection_scratch) {
        failure_reason = "serialized nested projection has no safe scratch object";
        return false;
    }
    auto *proxy = plan.outer_container_class->GetCollectionProxy();
    if (!proxy || proxy->GetValueClass() != plan.outer_element_class ||
        proxy->HasPointers()) {
        failure_reason = "serialized outer vector collection proxy is inconsistent";
        return false;
    }
    auto *all_actions = proxy->GetReadMemberWiseActions(element_version);
    if (!all_actions) {
        failure_reason = "ROOT has no read actions for serialized outer element version";
        return false;
    }
    std::unique_ptr<TStreamerInfoActions::TActionSequence> selected_actions(
        all_actions->CreateSubSequence(action_element_ids, 0));
    if (!selected_actions) {
        failure_reason = "ROOT could not build serialized selected action sequence";
        return false;
    }

    char start_buffer[TVirtualCollectionProxy::fgIteratorArenaSize];
    char end_buffer[TVirtualCollectionProxy::fgIteratorArenaSize];
    void *begin = start_buffer;
    void *end = end_buffer;
    const auto create_iterators = proxy->GetFunctionCreateIterators(kTRUE);
    const auto delete_iterators = proxy->GetFunctionDeleteTwoIterators(kTRUE);
    if (!create_iterators || !delete_iterators) {
        failure_reason = "ROOT collection proxy has no read iterator functions";
        return false;
    }

    TVirtualCollectionProxy::TPushPop proxy_guard(proxy, outer_collection_scratch);
    void *alternative = proxy->Allocate(static_cast<UInt_t>(outer_count), kTRUE);
    if (!alternative) {
        failure_reason = "ROOT could not size serialized projection scratch collection";
        return false;
    }
    try {
        create_iterators(alternative, &begin, &end, proxy);
        buffer.ApplySequence(*selected_actions, begin, end);
        if (begin != static_cast<void *>(start_buffer)) {
            delete_iterators(begin, end);
            begin = start_buffer;
        }
        proxy->Commit(alternative);
    } catch (...) {
        if (begin != static_cast<void *>(start_buffer)) delete_iterators(begin, end);
        proxy->Commit(alternative);
        throw;
    }
    return true;
}

bool CollectSerializedNestedObjectProjection(
    const SerializedReadPlan &plan, void *root_object_scratch,
    uint64_t max_values_per_entry, std::vector<double> &values,
    std::vector<int32_t> &flat_indices, std::string &failure_reason,
    bool collect_indices) {
    values.clear();
    flat_indices.clear();
    try {
        if (collect_indices) {
            OffsetValueReader::CollectFlat(root_object_scratch, plan.projection_levels,
                                           plan.index_depth, values, flat_indices);
        } else {
            OffsetValueReader::CollectValues(root_object_scratch,
                                             plan.projection_levels, values);
        }
    } catch (const std::exception &ex) {
        values.clear();
        flat_indices.clear();
        failure_reason = std::string("serialized nested object traversal failed: ") + ex.what();
        return false;
    }
    if (values.size() > max_values_per_entry) {
        values.clear();
        flat_indices.clear();
        failure_reason = "serialized nested object value count exceeds configured safety limit";
        return false;
    }
    RootDebug("SERIALIZED.NESTED_OBJECT",
              "path=" + plan.logical_path + " values=" + std::to_string(values.size()));
    return true;
}

} // namespace duckdb::rootlake
