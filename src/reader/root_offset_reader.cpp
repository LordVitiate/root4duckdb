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

std::string ReadRootString(void* pointer, const std::string& raw_type) {
    if (!pointer) {
        return {};
    }
    try {
        if (TrimType(raw_type) == "TString") {
            auto* value = static_cast<TString*>(pointer);
            return std::string(value->Data(), value->Length());
        }
        return *reinterpret_cast<std::string*>(pointer);
    } catch (...) {
        return TrimType(raw_type) == "TString" ? "<tstring_error>" : "<string_error>";
    }
}

void AppendArrayIndexNames(const PathLevel& level, size_t current_depth, std::vector<std::string>& names) {
    const size_t rank = level.array_dimensions.size() <= 1 ? 1 : level.array_dimensions.size();
    const size_t target_size = current_depth + rank;
    if (level.array_dimensions.size() <= 1) {
        if (names.size() < target_size) {
            names.push_back(level.name + "_idx");
        }
        return;
    }
    for (size_t dim = 0; dim < level.array_dimensions.size(); ++dim) {
        if (names.size() < target_size) {
            names.push_back(level.name + "_dim" + std::to_string(dim) + "_idx");
        }
    }
}

void PushArrayCoordinates(uint64_t flat_index, const std::vector<uint32_t>& dimensions, std::vector<int32_t>& indices) {
    if (dimensions.empty()) {
        indices.push_back(static_cast<int32_t>(flat_index));
        return;
    }
    std::vector<int32_t> coordinates(dimensions.size(), 0);
    for (size_t reverse = dimensions.size(); reverse > 0; --reverse) {
        const size_t dim = reverse - 1;
        coordinates[dim] = static_cast<int32_t>(flat_index % dimensions[dim]);
        flat_index /= dimensions[dim];
    }
    indices.insert(indices.end(), coordinates.begin(), coordinates.end());
}

struct ContainerAccess {
    char* base = nullptr;
    uint32_t stride = 0;
    bool contiguous = false;
};

ContainerAccess PrepareContainerAccess(const PathLevel& level, TVirtualCollectionProxy* proxy, size_t size) {
    ContainerAccess result;
    if (!proxy || !size || !ContiguousVectorPathEnabled() || !IsContiguousVectorType(level.type)) {
        return result;
    }
    const auto inner = TrimType(ExtractInnerType(level.type));
    if (inner.empty() || inner == "bool" || inner == "Bool_t" || IsPointerType(inner)) {
        return result;
    }
    uint32_t stride = PrimitiveTypeSize(inner);
    if (!stride && level.element_class) {
        stride = static_cast<uint32_t>(level.element_class->Size());
    }
    if (!stride) {
        return result;
    }
    auto* first = static_cast<char*>(proxy->At(0));
    if (!first) {
        return result;
    }
    if (size > 1) {
        auto* second = static_cast<char*>(proxy->At(1));
        if (!second || second != first + stride) {
            return result;
        }
    }
    result.base = first;
    result.stride = stride;
    result.contiguous = true;
    return result;
}

struct DirectSink {
    int64_t max_values;
    int64_t event_id;
    ReadResult& result;

    bool Full() const {
        return max_values >= 0 && result.size() >= static_cast<size_t>(max_values);
    }
    bool RequiresContainerDictionary() const {
        return true;
    }
    void Number(void* pointer, const std::string& type, const std::vector<int32_t>& indices,
                const std::vector<std::string>& names) {
        result.AddNumber(RootPrimitiveValue::FromPointer(pointer, type), event_id, indices, names);
    }
    void String(void* pointer, const std::string& type, const std::vector<int32_t>& indices,
                const std::vector<std::string>& names) {
        result.AddString(ReadRootString(pointer, type), event_id, indices, names);
    }
};

struct NumericOnlySink {
    std::vector<double>& result;

    bool Full() const {
        return false;
    }
    bool RequiresContainerDictionary() const {
        return false;
    }
    void Number(void* pointer, const std::string& type, const std::vector<int32_t>&, const std::vector<std::string>&) {
        result.push_back(ReadPrimitiveAsDouble(pointer, type));
    }
    void String(void*, const std::string&, const std::vector<int32_t>&, const std::vector<std::string>&) {
    }
};

struct FlatNumericSink {
    idx_t index_depth;
    std::vector<double>& values;
    std::vector<int32_t>& indices;

    bool Full() const {
        return false;
    }
    bool RequiresContainerDictionary() const {
        return false;
    }
    void Number(void* pointer, const std::string& type, const std::vector<int32_t>& current_indices,
                const std::vector<std::string>&) {
        if (current_indices.size() != index_depth) {
            throw IOException("ROOT container depth mismatch while executing indexed access plan: expected " +
                              std::to_string(index_depth) + ", got " + std::to_string(current_indices.size()));
        }
        values.push_back(ReadPrimitiveAsDouble(pointer, type));
        indices.insert(indices.end(), current_indices.begin(), current_indices.end());
    }
    void String(void*, const std::string&, const std::vector<int32_t>&, const std::vector<std::string>&) {
    }
};

template <class SINK>
void CollectValuesRecursive(void* current, const std::vector<PathLevel>& levels, size_t level_index,
                            std::vector<int32_t>& indices, std::vector<std::string>& index_names, SINK& sink) {
    if (!current || level_index >= levels.size() || sink.Full()) {
        return;
    }
    const auto& level = levels[level_index];
    auto* field = static_cast<char*>(current) + level.offset_in_parent;
    if (level.is_pointer) {
        field = field ? *reinterpret_cast<char**>(field) : nullptr;
        if (!field) {
            return;
        }
    }
    const bool last = level_index + 1 == levels.size();

    if (level.is_fixed_array) {
        AppendArrayIndexNames(level, indices.size(), index_names);
        const size_t pushed = std::max<size_t>(1, level.array_dimensions.size());
        for (uint64_t i = 0; i < level.fixed_array_length && !sink.Full(); ++i) {
            auto* element = field + i * level.element_size;
            PushArrayCoordinates(i, level.array_dimensions, indices);
            if (last) {
                if (level.is_primitive) {
                    sink.Number(element, level.type, indices, index_names);
                }
            } else {
                CollectValuesRecursive(element, levels, level_index + 1, indices, index_names, sink);
            }
            indices.resize(indices.size() - pushed);
        }
        return;
    }

    if (last && level.is_container) {
        auto* proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
        if (!proxy) {
            return;
        }
        TVirtualCollectionProxy::TPushPop guard(proxy, field);
        const auto inner = ExtractInnerType(level.type);
        const auto size = proxy->Size();
        if (index_names.size() < indices.size() + 1) {
            index_names.push_back(level.name + "_idx");
        }
        const auto access = PrepareContainerAccess(level, proxy, size);
        for (size_t i = 0; i < size && !sink.Full(); ++i) {
            auto* element = access.contiguous ? static_cast<void*>(access.base + i * access.stride) : proxy->At(i);
            if (!element) {
                continue;
            }
            indices.push_back(static_cast<int32_t>(i));
            if (IsPrimitiveType(inner)) {
                sink.Number(element, inner, indices, index_names);
            } else if (IsStringType(inner)) {
                sink.String(element, inner, indices, index_names);
            }
            indices.pop_back();
        }
        return;
    }

    if (last) {
        if (level.is_primitive) {
            sink.Number(field, level.type, indices, index_names);
        } else if (level.is_string) {
            sink.String(field, level.type, indices, index_names);
        }
        return;
    }

    if (level.is_container) {
        if (sink.RequiresContainerDictionary() && (!level.klass || !level.klass->HasDictionary())) {
            return;
        }
        auto* proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
        if (!proxy) {
            return;
        }
        TVirtualCollectionProxy::TPushPop guard(proxy, field);
        const auto size = proxy->Size();
        if (index_names.size() < indices.size() + 1) {
            index_names.push_back(level.name + "_idx");
        }
        const auto access = PrepareContainerAccess(level, proxy, size);
        for (size_t i = 0; i < size && !sink.Full(); ++i) {
            auto* element = access.contiguous ? static_cast<void*>(access.base + i * access.stride) : proxy->At(i);
            if (!element) {
                continue;
            }
            indices.push_back(static_cast<int32_t>(i));
            CollectValuesRecursive(element, levels, level_index + 1, indices, index_names, sink);
            indices.pop_back();
        }
        return;
    }

    if (level.klass) {
        CollectValuesRecursive(field, levels, level_index + 1, indices, index_names, sink);
    }
}

template <class SINK> void RunTraversal(void* root_object, const std::vector<PathLevel>& levels, SINK& sink) {
    std::vector<int32_t> indices;
    std::vector<std::string> index_names;
    CollectValuesRecursive(root_object, levels, 0, indices, index_names, sink);
}

} // namespace

void OffsetValueReader::CollectValues(void* root_object, const std::vector<PathLevel>& levels,
                                      std::vector<double>& out) {
    NumericOnlySink sink{out};
    RunTraversal(root_object, levels, sink);
}

void OffsetValueReader::CollectFlat(void* root_object, const std::vector<PathLevel>& levels, idx_t index_depth,
                                    std::vector<double>& values, std::vector<int32_t>& flat_indices) {
    FlatNumericSink sink{index_depth, values, flat_indices};
    RunTraversal(root_object, levels, sink);
}

void OffsetValueReader::CollectDirect(void* root_object, const std::vector<PathLevel>& levels, int64_t max_values,
                                      int64_t event_id, ReadResult& out) {
    DirectSink sink{max_values, event_id, out};
    RunTraversal(root_object, levels, sink);
}

} // namespace duckdb::rootlake
