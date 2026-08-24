#include "root4duckdb/reader/root_offset_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/core/root_lake_common.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace duckdb::rootlake {
namespace {

struct StreamerFieldMatch {
    TStreamerElement* element = nullptr;
    int64_t offset = 0;
};

std::optional<StreamerFieldMatch> FindStreamerField(TClass* klass, const std::string& field,
                                                    std::vector<std::string>& visited) {
    if (!klass) {
        return std::nullopt;
    }
    const std::string class_name = klass->GetName();
    if (std::find(visited.begin(), visited.end(), class_name) != visited.end()) {
        return std::nullopt;
    }
    visited.push_back(class_name);

    RootDebug("PATH.FIND_FIELD", "class=" + class_name + " field=" + field);
    auto* streamer = klass->GetStreamerInfo();
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element || element->IsBase() || field != element->GetName()) {
            continue;
        }
        StreamerFieldMatch result;
        result.element = element;
        result.offset = streamer->GetElementOffset(i);
        visited.pop_back();
        return result;
    }
    for (int i = 0; elements && i < elements->GetEntries(); ++i) {
        auto* base = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!base || !base->IsBase()) {
            continue;
        }
        auto* base_class = base->GetClassPointer();
        if (!base_class) {
            base_class = TClass::GetClass(base->GetTypeName());
        }
        auto nested = FindStreamerField(base_class, field, visited);
        if (!nested) {
            continue;
        }
        nested->offset += streamer->GetElementOffset(i);
        visited.pop_back();
        return nested;
    }
    visited.pop_back();
    return std::nullopt;
}

void CollectImmediateChildren(TClass* klass, const std::string& prefix, SemanticPathSelection& selection,
                              std::set<std::string>& active_bases) {
    if (!klass) {
        return;
    }
    const std::string class_name = klass->GetName();
    if (!active_bases.insert(class_name).second) {
        return;
    }

    auto* streamer = klass->GetStreamerInfo();
    auto* elements = streamer ? streamer->GetElements() : nullptr;
    if (!elements) {
        active_bases.erase(class_name);
        return;
    }
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto* base = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!base || !base->IsBase()) {
            continue;
        }
        auto* base_class = base->GetClassPointer();
        if (!base_class) {
            base_class = TClass::GetClass(base->GetTypeName());
        }
        CollectImmediateChildren(base_class, prefix, selection, active_bases);
    }
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element || element->IsBase()) {
            continue;
        }
        const std::string full_path = prefix + element->GetName();
        const std::string type = element->GetTypeName();
        if (IsPrimitiveType(type) || IsStringType(type)) {
            selection.primitive_paths.push_back(full_path);
        } else if (element->GetClassPointer() || element->IsaPointer()) {
            selection.child_paths.insert(full_path + "/");
        }
    }
    active_bases.erase(class_name);
}

std::vector<PathLevel> ResolvePath(TClass* root_class, const std::vector<std::string>& fields, bool strict_layout) {
    if (!root_class) {
        throw InvalidInputException("ROOT dictionary class is null");
    }
    auto* current_class = root_class;
    if (strict_layout && !current_class->GetStreamerInfo()) {
        throw InvalidInputException("No TStreamerInfo for class " + std::string(root_class->GetName()));
    }

    std::vector<PathLevel> levels;
    int64_t cumulative = 0;
    for (idx_t field_index = 0; field_index < fields.size(); ++field_index) {
        const auto& field = fields[field_index];
        if (field == "value" && !levels.empty() && levels.back().is_container &&
            !IsAssociativeContainerType(levels.back().type)) {
            PathLevel value_level;
            value_level.name = "value";
            value_level.type = levels.back().element_class ? levels.back().element_class->GetName()
                                                           : ExtractInnerType(levels.back().type);
            value_level.offset_in_parent = 0;
            value_level.cumulative_offset = levels.back().cumulative_offset;
            value_level.klass = levels.back().element_class;
            if (!value_level.klass && !value_level.type.empty()) {
                value_level.klass = TClass::GetClass(value_level.type.c_str());
            }
            value_level.is_primitive = IsPrimitiveType(value_level.type);
            value_level.is_string = IsStringType(value_level.type);
            value_level.is_container = value_level.klass && value_level.klass->GetCollectionProxy();
            if (value_level.is_container) {
                value_level.element_class = value_level.klass->GetCollectionProxy()->GetValueClass();
            }
            value_level.element_size = value_level.is_primitive
                                           ? PrimitiveTypeSize(value_level.type)
                                           : (value_level.klass ? static_cast<uint32_t>(value_level.klass->Size()) : 0);
            const bool terminal = field_index + 1 == fields.size();
            if (terminal && !value_level.is_primitive && !value_level.is_string) {
                throw InvalidInputException("Container /value does not terminate in a primitive/string: " +
                                            value_level.type);
            }
            if (!terminal && !value_level.klass) {
                throw InvalidInputException("Cannot descend through container value type: " + value_level.type);
            }
            levels.push_back(value_level);
            cumulative = value_level.is_container ? 0 : value_level.cumulative_offset;
            current_class = value_level.is_container ? value_level.element_class : value_level.klass;
            continue;
        }

        if (!current_class) {
            throw InvalidInputException("Cannot descend through primitive field before '" + field + "'");
        }
        std::string streamer_field = field;
        if (!levels.empty() && levels.back().is_container && IsAssociativeContainerType(levels.back().type)) {
            if (field == "key") {
                streamer_field = "first";
            } else if (field == "value") {
                streamer_field = "second";
            }
        }
        std::vector<std::string> visited;
        auto match = FindStreamerField(current_class, streamer_field, visited);
        if (!match || !match->element) {
            throw InvalidInputException("Field '" + field + "' is absent in ROOT streamer path");
        }

        auto* element = match->element;
        PathLevel level;
        level.name = field;
        level.type = element->GetTypeName();
        level.type = StreamerPrimitiveType(element->GetType(), level.type);
        level.offset_in_parent = match->offset;
        level.cumulative_offset = cumulative + level.offset_in_parent;
        level.is_primitive = IsPrimitiveType(level.type);
        level.is_string = IsStringType(level.type);
        level.is_pointer = element->IsaPointer();
        level.klass = element->GetClassPointer();
        level.is_container = level.klass && level.klass->GetCollectionProxy();
        if (level.is_container) {
            level.element_class = level.klass->GetCollectionProxy()->GetValueClass();
        }

        const int rank = element->GetArrayDim();
        uint64_t array_length = 1;
        for (int dim = 0; dim < rank; ++dim) {
            const int extent = element->GetMaxIndex(dim);
            if (extent <= 0 || (strict_layout &&
                                array_length > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(extent))) {
                array_length = 0;
                level.array_dimensions.clear();
                break;
            }
            level.array_dimensions.push_back(static_cast<uint32_t>(extent));
            array_length *= static_cast<uint64_t>(extent);
        }
        if (strict_layout && (!array_length || level.array_dimensions.empty())) {
            const auto parsed_array = ParseFixedArrayType(level.type);
            if (parsed_array.length) {
                level.array_dimensions = parsed_array.dimensions;
                array_length = parsed_array.length;
                level.type = parsed_array.base_type;
                level.is_primitive = IsPrimitiveType(level.type);
            }
        }
        level.is_fixed_array = !level.array_dimensions.empty() && array_length > 0;
        level.fixed_array_length = level.is_fixed_array ? array_length : 0;
        level.element_size = level.is_primitive ? PrimitiveTypeSize(level.type)
                                                : (level.klass ? static_cast<uint32_t>(level.klass->Size()) : 0);
        if (level.is_fixed_array && !level.element_size) {
            throw NotImplementedException("Cannot determine element size for fixed ROOT array " + field);
        }
        levels.push_back(level);

        if (level.is_pointer) {
            cumulative = 0;
            current_class = level.klass;
        } else if (level.is_container) {
            cumulative = 0;
            current_class = level.element_class;
        } else if (level.is_fixed_array && level.klass) {
            cumulative = 0;
            current_class = level.klass;
        } else if (level.klass) {
            cumulative = level.cumulative_offset;
            current_class = level.klass;
        } else {
            current_class = nullptr;
        }
        if (strict_layout && current_class && !current_class->GetStreamerInfo()) {
            throw InvalidInputException("No TStreamerInfo for nested class " + std::string(current_class->GetName()));
        }
    }
    if (levels.empty()) {
        throw InvalidInputException("Resolved ROOT path is empty");
    }
    return levels;
}

} // namespace

std::vector<PathLevel> PathResolver::Resolve(TClass* root_class, const std::vector<std::string>& fields) {
    auto levels = ResolvePath(root_class, fields, true);
    const auto& leaf = levels.back();
    if (!leaf.is_primitive && !leaf.is_string) {
        throw InvalidInputException("ROOT path does not end in a primitive/string value");
    }
    return levels;
}

std::vector<PathLevel> PathResolver::TryResolve(TClass* root_class, const std::vector<std::string>& fields) noexcept {
    try {
        return ResolvePath(root_class, fields, false);
    } catch (...) {
        return {};
    }
}

void AppendLevelIndexNames(const PathLevel& level, std::vector<std::string>& names) {
    if (level.is_container) {
        names.push_back(level.name + "_idx");
    }
    if (!level.is_fixed_array) {
        return;
    }
    if (level.array_dimensions.size() <= 1) {
        names.push_back(level.name + "_idx");
        return;
    }
    for (idx_t dim = 0; dim < level.array_dimensions.size(); ++dim) {
        names.push_back(level.name + "_dim" + std::to_string(dim) + "_idx");
    }
}

std::string IndexSignature(const std::vector<PathLevel>& levels) {
    std::vector<std::string> names;
    for (const auto& level : levels) {
        AppendLevelIndexNames(level, names);
    }
    return JoinStrings(names, ",");
}

idx_t IndexDepth(const std::vector<PathLevel>& levels) {
    idx_t result = 0;
    for (const auto& level : levels) {
        if (level.is_container) {
            ++result;
        }
        if (level.is_fixed_array) {
            result += std::max<idx_t>(1, level.array_dimensions.size());
        }
    }
    return result;
}

bool SelectSemanticPath(TClass* root_class, const ParsedPath& path, const std::string& raw_path,
                        SemanticPathSelection& selection) {
    selection = {};
    if (!root_class) {
        return false;
    }
    std::string canonical = raw_path;
    while (canonical.size() > 1 && canonical.back() == '/') {
        canonical.pop_back();
    }

    if (path.fields.empty()) {
        selection.bind_prefix = canonical + "/";
        std::set<std::string> active_bases;
        CollectImmediateChildren(root_class, selection.bind_prefix, selection, active_bases);
        return !selection.primitive_paths.empty() || !selection.child_paths.empty();
    }

    const auto levels = PathResolver::TryResolve(root_class, path.fields);
    if (levels.empty()) {
        return false;
    }
    const auto& terminal = levels.back();
    if (terminal.is_primitive || terminal.is_string) {
        selection.bind_prefix = canonical;
        selection.primitive_paths.push_back(canonical);
        return true;
    }

    TClass* target_class = nullptr;
    if (terminal.is_container) {
        if (IsAssociativeContainerType(terminal.type)) {
            selection.bind_prefix = canonical + "/";
            selection.primitive_paths.push_back(canonical + "/key");
            selection.primitive_paths.push_back(canonical + "/value");
            return true;
        }
        const auto inner =
            terminal.element_class ? std::string(terminal.element_class->GetName()) : ExtractInnerType(terminal.type);
        if (IsPrimitiveType(inner) || IsStringType(inner)) {
            selection.bind_prefix = canonical + "/";
            selection.primitive_paths.push_back(canonical + "/value");
            return true;
        }
        target_class = terminal.element_class;
    } else {
        target_class = terminal.klass;
    }
    if (!target_class) {
        return false;
    }

    selection.bind_prefix = canonical + "/";
    std::set<std::string> active_bases;
    CollectImmediateChildren(target_class, selection.bind_prefix, selection, active_bases);
    return !selection.primitive_paths.empty() || !selection.child_paths.empty();
}

} // namespace duckdb::rootlake
