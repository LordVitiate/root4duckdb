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

std::string ChildKind(const PathLevel& level) {
    if (level.is_fixed_array) {
        return "FIXED_ARRAY";
    }
    if (level.is_container) {
        return "CONTAINER";
    }
    if (level.is_string) {
        return "STRING";
    }
    if (level.is_primitive) {
        return "PRIMITIVE";
    }
    return "OBJECT";
}

void CollectImmediateMemberNames(TClass* klass, std::vector<std::string>& names, std::set<std::string>& seen_names,
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

    // Match FindStreamerField(): a derived member shadows an inherited one.
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto* element = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!element || element->IsBase()) {
            continue;
        }
        const std::string name = element->GetName();
        if (seen_names.insert(name).second) {
            names.push_back(name);
        }
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
        CollectImmediateMemberNames(base_class, names, seen_names, active_bases);
    }
    active_bases.erase(class_name);
}

SemanticPathChild MakeChild(const std::string& path, const std::vector<PathLevel>& levels) {
    SemanticPathChild child;
    child.path = path;
    const auto slash = path.find_last_of('/');
    child.name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (levels.empty()) {
        child.kind = "UNKNOWN";
        return child;
    }
    const auto& level = levels.back();
    child.root_type = level.type;
    child.kind = ChildKind(level);
    child.is_primitive = level.is_primitive;
    child.is_string = level.is_string;
    child.is_container = level.is_container;
    child.is_fixed_array = level.is_fixed_array;
    child.is_pointer = level.is_pointer;
    return child;
}

void AppendSyntheticChild(TClass* root_class, const std::string& path, std::vector<SemanticPathChild>& children) {
    const auto parsed = ParsePathPrefix(path);
    const auto levels = PathResolver::TryResolve(root_class, parsed.fields);
    if (!levels.empty()) {
        children.push_back(MakeChild(path, levels));
    }
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

bool DescribeSemanticPath(TClass* root_class, const ParsedPath& path, const std::string& raw_path,
                          std::vector<SemanticPathChild>& children) {
    children.clear();
    if (!root_class) {
        return false;
    }

    const std::string canonical = NormalizePath(raw_path);
    std::vector<PathLevel> target_levels;
    TClass* target_class = root_class;

    if (!path.fields.empty()) {
        target_levels = PathResolver::TryResolve(root_class, path.fields);
        if (target_levels.empty()) {
            return false;
        }
        const auto& terminal = target_levels.back();

        // A scalar is a valid metadata location but has no next level.
        if ((terminal.is_primitive || terminal.is_string) && !terminal.is_fixed_array && !terminal.is_container) {
            return true;
        }

        if (terminal.is_fixed_array) {
            if (terminal.is_primitive || terminal.is_string) {
                SemanticPathChild child = MakeChild(canonical, target_levels);
                child.path = canonical + "/value";
                child.name = "value";
                child.kind = terminal.is_string ? "STRING" : "PRIMITIVE";
                child.is_fixed_array = false;
                children.push_back(std::move(child));
                return true;
            }
            target_class = terminal.klass;
        } else if (terminal.is_container) {
            if (IsAssociativeContainerType(terminal.type)) {
                AppendSyntheticChild(root_class, canonical + "/key", children);
                AppendSyntheticChild(root_class, canonical + "/value", children);
                return true;
            }

            const auto inner = terminal.element_class ? std::string(terminal.element_class->GetName())
                                                      : ExtractInnerType(terminal.type);
            if (IsPrimitiveType(inner) || IsStringType(inner) ||
                (terminal.element_class && terminal.element_class->GetCollectionProxy())) {
                AppendSyntheticChild(root_class, canonical + "/value", children);
                return true;
            }
            target_class = terminal.element_class;
        } else {
            target_class = terminal.klass;
        }
    }

    if (!target_class) {
        return false;
    }

    std::vector<std::string> member_names;
    std::set<std::string> seen_names;
    std::set<std::string> active_bases;
    CollectImmediateMemberNames(target_class, member_names, seen_names, active_bases);
    for (const auto& name : member_names) {
        const auto child_path = canonical + "/" + name;
        const auto child_parsed = ParsePathPrefix(child_path);
        const auto levels = PathResolver::TryResolve(root_class, child_parsed.fields);
        if (!levels.empty()) {
            children.push_back(MakeChild(child_path, levels));
        }
    }
    return true;
}

bool SelectSemanticPath(TClass* root_class, const ParsedPath& path, const std::string& raw_path,
                        SemanticPathSelection& selection) {
    selection = {};
    if (!root_class) {
        return false;
    }

    const std::string canonical = NormalizePath(raw_path);
    std::vector<PathLevel> target_levels;
    if (!path.fields.empty()) {
        target_levels = PathResolver::TryResolve(root_class, path.fields);
        if (target_levels.empty()) {
            return false;
        }
        const auto& terminal = target_levels.back();
        if ((terminal.is_primitive || terminal.is_string) && !terminal.is_fixed_array && !terminal.is_container) {
            throw InvalidInputException("read_root path_prefix must select an object or collection, not scalar leaf '" +
                                        canonical + "'; select its parent object and project column '" + terminal.name +
                                        "' instead");
        }

        // A fixed array is itself the selected collection. Keep the existing
        // internal address so flattened array indices preserve their contract.
        if (terminal.is_fixed_array && (terminal.is_primitive || terminal.is_string)) {
            selection.bind_prefix = canonical;
            selection.primitive_paths.push_back(canonical);
            return true;
        }
    }

    std::vector<SemanticPathChild> children;
    if (!DescribeSemanticPath(root_class, path, canonical, children)) {
        return false;
    }

    selection.bind_prefix = canonical + "/";
    const auto target_signature = IndexSignature(target_levels);
    for (const auto& child : children) {
        if ((child.is_primitive || child.is_string) && !child.is_container && !child.is_fixed_array) {
            const auto child_parsed = ParsePathPrefix(child.path);
            const auto child_levels = PathResolver::TryResolve(root_class, child_parsed.fields);
            if (!child_levels.empty() && IndexSignature(child_levels) == target_signature) {
                selection.primitive_paths.push_back(child.path);
                continue;
            }
        }
        selection.child_paths.insert(child.path);
    }
    return true;
}

} // namespace duckdb::rootlake
