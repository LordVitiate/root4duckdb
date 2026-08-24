#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace duckdb::rootlake {

bool IsSerializedNestedProjection(SerializedProjectionKind kind) {
    return kind == SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR ||
           kind == SerializedProjectionKind::SELECTED_SUBTREE;
}

const char* SerializedProjectionName(SerializedProjectionKind kind) {
    switch (kind) {
    case SerializedProjectionKind::FIXED_MEMBER:
        return "fixed_member";
    case SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR:
        return "nested_primitive_vector";
    case SerializedProjectionKind::SELECTED_SUBTREE:
        return "selected_subtree";
    case SerializedProjectionKind::LEAF_BRANCH:
        return "leaf_branch";
    case SerializedProjectionKind::ROOT_SELECTED_SUBTREE:
        return "root_selected_subtree";
    case SerializedProjectionKind::COLLECTION_BRANCH:
        return "collection_branch";
    }
    return "unknown";
}

bool ResolveSerializedVersionedMember(const SerializedReadPlan& plan, int32_t element_version,
                                      std::vector<int>& prefix_element_ids, std::string& failure_reason) {
    auto* streamer = plan.outer_element_class->GetStreamerInfo(element_version);
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements) {
        failure_reason =
            "ROOT has no TStreamerInfo for serialized outer element version " + std::to_string(element_version);
        return false;
    }

    TStreamerElement* target = nullptr;
    int target_index = -1;
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
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
                         "element version " +
                         std::to_string(element_version);
        return false;
    }
    if (target->IsaPointer() ||
        (plan.projection_kind == SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR &&
         (target->GetArrayDim() != 0 || target->GetStreamer()))) {
        failure_reason = "projected member has an unsupported layout in serialized outer "
                         "element version " +
                         std::to_string(element_version);
        return false;
    }

    const std::string target_type = target->GetTypeName() ? target->GetTypeName() : "";
    const auto arguments = TemplateArguments(target_type);
    auto* target_class = target->GetClassPointer();
    auto* target_proxy = target_class ? target_class->GetCollectionProxy() : nullptr;
    if (plan.projection_kind == SerializedProjectionKind::NESTED_PRIMITIVE_VECTOR) {
        if (!IsContiguousVectorType(target_type) || arguments.empty() || !target_proxy ||
            target_proxy->HasPointers()) {
            failure_reason = "projected nested vector is not an inline collection in serialized outer element "
                             "version " +
                             std::to_string(element_version);
            return false;
        }
        const auto scalar_type = PrimitiveBaseType(arguments.front());
        const auto scalar_kind = ClassifySerializedPrimitive(scalar_type);
        if (target_proxy->GetValueClass() || scalar_kind == SerializedPrimitiveKind::UNKNOWN ||
            scalar_kind != ClassifySerializedPrimitive(plan.value_type) ||
            PrimitiveTypeSize(scalar_type) != plan.value_bytes) {
            failure_reason = "projected nested vector primitive type changed in serialized "
                             "outer element version " +
                             std::to_string(element_version) + " (file=" + scalar_type +
                             ", dictionary=" + plan.value_type + ")";
            return false;
        }
    } else if (plan.projection_kind == SerializedProjectionKind::SELECTED_SUBTREE) {
        // The selected member can be either an inline object or an inline
        // vector<object>.  Versioned resolution must preserve that exact
        // persistent shape; otherwise the stored traversal offsets are not
        // valid for this file version.
        const bool planned_is_primitive =
            plan.projection_levels.size() > 1 && plan.projection_levels[1].is_primitive;
        if (planned_is_primitive) {
            const auto selected_primitive_type =
                StreamerPrimitiveType(target->GetType(), target->GetTypeName());
            const auto planned_type = PrimitiveBaseType(plan.projection_levels[1].type);
            if (PrimitiveBaseType(selected_primitive_type) != planned_type ||
                target->GetArrayDim() !=
                    static_cast<int>(plan.projection_levels[1].array_dimensions.size())) {
                failure_reason = "projected selected primitive changed type or shape in serialized outer element "
                                 "version " +
                                 std::to_string(element_version);
                return false;
            }
            prefix_element_ids.clear();
            prefix_element_ids.reserve(static_cast<size_t>(target_index));
            for (int i = 0; i < target_index; ++i) {
                prefix_element_ids.push_back(i);
            }
            return true;
        }
        const bool target_is_collection = target_proxy != nullptr;
        const bool planned_is_collection =
            plan.projection_levels.size() > 1 && plan.projection_levels[1].is_container;
        if (target_is_collection != planned_is_collection ||
            (target_is_collection && target_proxy->HasPointers())) {
            failure_reason = "projected selected subtree changed shape in serialized outer element version " +
                             std::to_string(element_version);
            return false;
        }
        if (target_is_collection) {
            auto* planned_value_class = plan.projection_levels[1].element_class;
            auto* target_value_class = target_proxy->GetValueClass();
            if ((planned_value_class && target_value_class != planned_value_class) ||
                (!planned_value_class && target_value_class)) {
                failure_reason = "projected selected subtree collection value changed in serialized outer element "
                                 "version " +
                                 std::to_string(element_version);
                return false;
            }
            if (!planned_value_class) {
                const auto file_value_type = arguments.empty() ? std::string() : PrimitiveBaseType(arguments.front());
                if (ClassifySerializedPrimitive(file_value_type) !=
                        ClassifySerializedPrimitive(plan.value_type) ||
                    PrimitiveTypeSize(file_value_type) != plan.value_bytes) {
                    failure_reason = "projected primitive collection value changed in serialized outer element "
                                     "version " +
                                     std::to_string(element_version);
                    return false;
                }
            }
        }
        const auto* planned_class = plan.projection_levels.size() > 1 ? plan.projection_levels[1].klass : nullptr;
        if (!target_class || !planned_class || std::string(target_class->GetName()) != planned_class->GetName()) {
            failure_reason = "projected selected subtree changed class in serialized outer element version " +
                             std::to_string(element_version);
            return false;
        }
    } else {
        failure_reason = "serialized nested schema resolver received an invalid projection";
        return false;
    }

    prefix_element_ids.clear();
    prefix_element_ids.reserve(static_cast<size_t>(target_index));
    for (int i = 0; i < target_index; ++i) {
        prefix_element_ids.push_back(i);
    }
    return true;
}

bool ConfigureSerializedDeepProjection(const ParsedPath& path, const std::vector<PathLevel>& levels,
                                       TClass* outer_element_class, SerializedReadPlan& plan,
                                       std::string& failure_reason) {
    // Select the first non-primitive member below vector<object>, then let the
    // offset walker recurse through the rest of the already-resolved path.
    // This supports arbitrary combinations such as:
    //   vector<object>/object/.../primitive
    //   vector<object>/vector<object>/.../primitive
    //   vector<object>/vector<object>/.../vector<primitive>/value
    if (!outer_element_class || levels.size() < 2 || path.fields.size() != levels.size() ||
        !levels.back().is_primitive || levels.back().is_pointer || levels[1].is_pointer ||
        (levels.size() > 2 && (levels[1].is_primitive || levels[1].is_string || !levels[1].klass))) {
        failure_reason = "serialized selected-subtree projection requires "
                         "vector<object>/<inline object or vector<object>>/.../primitive";
        return false;
    }

    auto* streamer = outer_element_class->GetStreamerInfo();
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    TStreamerElement* selected_member = nullptr;
    int selected_member_index = -1;
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element) {
            failure_reason = "unexpected non-streamer element";
            return false;
        }
        if (!element->IsBase() && path.fields[1] == element->GetName()) {
            selected_member = element;
            selected_member_index = i;
            break;
        }
    }
    if (!selected_member || selected_member->IsaPointer()) {
        failure_reason = "selected subtree member is absent or a pointer";
        return false;
    }
    auto* selected_class = selected_member->GetClassPointer();
    if (!levels[1].is_primitive && !selected_class) {
        failure_reason = "selected subtree member has no ROOT class metadata";
        return false;
    }
    auto* selected_proxy = selected_class ? selected_class->GetCollectionProxy() : nullptr;
    if (levels[1].is_container) {
        if (!selected_proxy || selected_proxy->HasPointers()) {
            failure_reason = "selected subtree collection has no safe inline collection proxy";
            return false;
        }
        auto* selected_value_class = selected_proxy->GetValueClass();
        if (levels[1].element_class) {
            if (selected_value_class != levels[1].element_class) {
                failure_reason = "selected subtree collection value class is inconsistent";
                return false;
            }
        } else if (selected_value_class || levels.size() != 3 || path.fields.back() != "value" ||
                   !levels.back().is_primitive || levels.back().is_container) {
            failure_reason = "selected subtree primitive collection has an unsupported semantic shape";
            return false;
        }
    } else if (!levels[1].is_primitive && selected_proxy) {
        failure_reason = "selected subtree object unexpectedly resolves as a collection";
        return false;
    }

    const auto scalar_type = PrimitiveBaseType(levels.back().type);
    const auto scalar_width = PrimitiveTypeSize(scalar_type);
    if (!scalar_width || ClassifySerializedPrimitive(scalar_type) == SerializedPrimitiveKind::UNKNOWN) {
        failure_reason = "selected subtree primitive type is unsupported: " + scalar_type;
        return false;
    }

    plan.supported = true;
    plan.reason.clear();
    plan.projection_kind = SerializedProjectionKind::SELECTED_SUBTREE;
    plan.container_name = levels[0].name;
    plan.element_class = outer_element_class->GetName();
    plan.projected_member_name = path.fields[1];
    plan.value_type = scalar_type;
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.outer_container_class = levels[0].klass;
    plan.outer_element_class = outer_element_class;
    for (int i = 0; i < selected_member_index; ++i) {
        plan.prefix_element_ids.push_back(i);
    }
    plan.projection_levels = levels;
    // The traversal starts at a standalone outer collection rather than at
    // the reconstructed root object, so its first offset is always zero.
    plan.projection_levels.front().offset_in_parent = 0;
    plan.projection_levels.front().cumulative_offset = 0;
    plan.streamer_version =
        static_cast<uint32_t>(std::max(0, static_cast<int>(outer_element_class->GetClassVersion())));
    plan.value_bytes = scalar_width;
    plan.fixed_array_length = 1;
    plan.index_depth = IndexDepth(levels);
    return true;
}

bool ConsumeSerializedSelectedMembers(TBufferFile& buffer, const SerializedReadPlan& plan, int32_t element_version,
                                      uint64_t outer_count, void* outer_collection_scratch,
                                      const std::vector<int>& prefix_element_ids,
                                      std::unique_ptr<TStreamerInfoActions::TActionSequence>& cached_actions,
                                      std::string& failure_reason) {
    const bool include_selected_member = plan.projection_kind == SerializedProjectionKind::SELECTED_SUBTREE;
    if (prefix_element_ids.empty() && !include_selected_member) {
        return true;
    }
    if (!outer_collection_scratch) {
        failure_reason = "serialized nested projection has no safe scratch object";
        return false;
    }
    auto* proxy = plan.outer_container_class->GetCollectionProxy();
    if (!proxy || proxy->GetValueClass() != plan.outer_element_class || proxy->HasPointers()) {
        failure_reason = "serialized outer vector collection proxy is inconsistent";
        return false;
    }
    if (!cached_actions) {
        std::vector<int> action_element_ids = prefix_element_ids;
        if (include_selected_member) {
            // Prefix ids are consecutive; the selected subtree member is next.
            action_element_ids.push_back(static_cast<int>(prefix_element_ids.size()));
        }
        auto* all_actions = proxy->GetReadMemberWiseActions(element_version);
        if (!all_actions) {
            failure_reason = "ROOT has no read actions for serialized outer element version";
            return false;
        }
        cached_actions.reset(all_actions->CreateSubSequence(action_element_ids, 0));
        if (!cached_actions) {
            failure_reason = "ROOT could not build serialized selected action sequence";
            return false;
        }
    }

    char start_buffer[TVirtualCollectionProxy::fgIteratorArenaSize];
    char end_buffer[TVirtualCollectionProxy::fgIteratorArenaSize];
    void* begin = start_buffer;
    void* end = end_buffer;
    const auto create_iterators = proxy->GetFunctionCreateIterators(kTRUE);
    const auto delete_iterators = proxy->GetFunctionDeleteTwoIterators(kTRUE);
    if (!create_iterators || !delete_iterators) {
        failure_reason = "ROOT collection proxy has no read iterator functions";
        return false;
    }

    TVirtualCollectionProxy::TPushPop proxy_guard(proxy, outer_collection_scratch);
    void* alternative = proxy->Allocate(static_cast<UInt_t>(outer_count), kTRUE);
    if (!alternative) {
        failure_reason = "ROOT could not size serialized projection scratch collection";
        return false;
    }
    try {
        create_iterators(alternative, &begin, &end, proxy);
        buffer.ApplySequence(*cached_actions, begin, end);
        if (begin != static_cast<void*>(start_buffer)) {
            delete_iterators(begin, end);
            begin = start_buffer;
        }
        proxy->Commit(alternative);
    } catch (...) {
        if (begin != static_cast<void*>(start_buffer)) {
            delete_iterators(begin, end);
        }
        proxy->Commit(alternative);
        throw;
    }
    return true;
}

bool CollectSerializedSelectedSubtree(const SerializedReadPlan& plan, void* outer_collection_scratch,
                                      uint64_t max_values_per_entry, std::vector<double>& values,
                                      std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                      bool collect_indices) {
    values.clear();
    flat_indices.clear();
    try {
        if (collect_indices) {
            OffsetValueReader::CollectFlat(outer_collection_scratch, plan.projection_levels, plan.index_depth, values,
                                           flat_indices);
        } else {
            OffsetValueReader::CollectValues(outer_collection_scratch, plan.projection_levels, values);
        }
    } catch (const std::exception& ex) {
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
    RootDebug("SERIALIZED.SELECTED_SUBTREE",
              "path=" + plan.logical_path + " values=" + std::to_string(values.size()));
    return true;
}

bool CollectSerializedSelectedSubtree(const SerializedReadPlan& plan, void* outer_collection_scratch,
                                      uint64_t max_values_per_entry, std::vector<RootPrimitiveValue>& values,
                                      std::vector<int32_t>& flat_indices, std::string& failure_reason,
                                      bool collect_indices) {
    values.clear();
    flat_indices.clear();
    try {
        ReadResult result;
        const auto limit = static_cast<int64_t>(
            std::min<uint64_t>(max_values_per_entry, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
        OffsetValueReader::CollectDirect(outer_collection_scratch, plan.projection_levels, limit, 0, result);
        values = std::move(result.numbers);
        if (collect_indices) {
            if (result.vector_indices.size() != values.size()) {
                failure_reason = "serialized selected-subtree index count is inconsistent";
                values.clear();
                return false;
            }
            flat_indices.reserve(values.size() * plan.index_depth);
            for (const auto& row : result.vector_indices) {
                if (row.size() != plan.index_depth) {
                    failure_reason = "serialized selected-subtree index depth is inconsistent";
                    values.clear();
                    flat_indices.clear();
                    return false;
                }
                flat_indices.insert(flat_indices.end(), row.begin(), row.end());
            }
        }
    } catch (const std::exception& ex) {
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
    RootDebug("SERIALIZED.SELECTED_SUBTREE",
              "path=" + plan.logical_path + " values=" + std::to_string(values.size()));
    return true;
}

} // namespace duckdb::rootlake
