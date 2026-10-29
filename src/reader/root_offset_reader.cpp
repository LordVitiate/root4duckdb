#include "root4duckdb/reader/root_offset_reader.hpp"

#include "root4duckdb/core/root_debug.hpp"
#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/core/root_lake_common.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace duckdb::rootlake
{
namespace
{

std::string ReadRootString(void* pointer, const std::string& raw_type)
{
    if (!pointer)
    {
        return {};
    }
    if (TrimType(raw_type) == "TString")
    {
        const auto* value = static_cast<const TString*>(pointer);
        const auto length = value->Length();
        if (length < 0)
        {
            throw IOException("ROOT TString reported a negative length");
        }
        return std::string(value->Data(), static_cast<std::string::size_type>(length));
    }
    return *static_cast<const std::string*>(pointer);
}

int32_t CheckedContainerIndex(size_t index)
{
    if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    {
        throw IOException("ROOT container index exceeds the supported 32-bit SQL index range");
    }
    return static_cast<int32_t>(index);
}

void AppendArrayIndexNames(const PathLevel& level, size_t current_depth, std::vector<std::string>& names)
{
    const size_t rank = level.array_dimensions.size() <= 1 ? 1 : level.array_dimensions.size();
    const size_t target_size = current_depth + rank;
    if (level.array_dimensions.size() <= 1)
    {
        if (names.size() < target_size)
        {
            names.push_back(level.name + "_idx");
        }
        return;
    }
    for (size_t dim = 0; dim < level.array_dimensions.size(); ++dim)
    {
        if (names.size() < target_size)
        {
            names.push_back(level.name + "_dim" + std::to_string(dim) + "_idx");
        }
    }
}

void PushArrayCoordinates(uint64_t flat_index, const std::vector<uint32_t>& dimensions, std::vector<int32_t>& indices)
{
    if (dimensions.empty())
    {
        if (flat_index > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
        {
            throw IOException("ROOT fixed-array index exceeds the supported 32-bit SQL index range");
        }
        indices.push_back(static_cast<int32_t>(flat_index));
        return;
    }
    const auto base = indices.size();
    indices.resize(base + dimensions.size());
    for (size_t reverse = dimensions.size(); reverse > 0; --reverse)
    {
        const size_t dim = reverse - 1;
        if (dimensions[dim] == 0)
        {
            throw IOException("ROOT fixed array has a zero dimension");
        }
        const uint64_t coordinate = flat_index % dimensions[dim];
        if (coordinate > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
        {
            throw IOException("ROOT fixed-array coordinate exceeds the supported 32-bit SQL index range");
        }
        indices[base + dim] = static_cast<int32_t>(coordinate);
        flat_index /= dimensions[dim];
    }
}

struct ContainerAccess
{
    char* base = nullptr;
    uint32_t stride = 0;
    bool contiguous = false;
};

ContainerAccess PrepareContainerAccess(const PathLevel& level, TVirtualCollectionProxy* proxy, size_t size)
{
    ContainerAccess result;
    if (!proxy || !size || !ContiguousVectorPathEnabled() || !IsContiguousVectorType(level.type))
    {
        return result;
    }
    const auto inner = TrimType(ExtractInnerType(level.type));
    if (inner.empty() || inner == "bool" || inner == "Bool_t" || IsPointerType(inner))
    {
        return result;
    }
    uint32_t stride = PrimitiveTypeSize(inner);
    if (!stride && level.element_class)
    {
        stride = static_cast<uint32_t>(level.element_class->Size());
    }
    if (!stride)
    {
        return result;
    }
    auto* first = static_cast<char*>(proxy->At(0));
    if (!first)
    {
        return result;
    }
    if (size > 1)
    {
        const auto* second = static_cast<const char*>(proxy->At(1));
        if (!second || second != first + stride)
        {
            return result;
        }
    }
    result.base = first;
    result.stride = stride;
    result.contiguous = true;
    RootDebug("VECTOR.CONTIGUOUS",
              "type=" + level.type + " size=" + std::to_string(size) + " stride=" + std::to_string(stride));
    return result;
}

class DirectSink
{
  public:
    DirectSink(int64_t max_values_p, int64_t entry_id_p, ReadResult& result_p)
        : max_values(max_values_p), entry_id(entry_id_p), result(result_p)
    {
    }
    size_t StartIndex(const PathLevel&, const std::vector<int32_t>&) const noexcept
    {
        return 0;
    }
    bool Full() const
    {
        return max_values >= 0 && result.size() >= static_cast<size_t>(max_values);
    }
    bool RequiresContainerDictionary() const
    {
        return true;
    }
    void Number(void* pointer, const std::string& type, const std::vector<int32_t>& indices,
                const std::vector<std::string>& names)
    {
        result.AddNumber(RootPrimitiveValue::FromPointer(pointer, type), entry_id, indices, names);
    }
    void String(void* pointer, const std::string& type, const std::vector<int32_t>& indices,
                const std::vector<std::string>& names)
    {
        result.AddString(ReadRootString(pointer, type), entry_id, indices, names);
    }

  private:
    int64_t max_values;
    int64_t entry_id;
    ReadResult& result;
};

class FlatTypedSink
{
  public:
    FlatTypedSink(idx_t index_depth_p, int64_t max_values_p, std::vector<RootPrimitiveValue>& values_p,
                  std::vector<int32_t>& flat_indices_p)
        : index_depth(index_depth_p), max_values(max_values_p), values(values_p), flat_indices(flat_indices_p)
    {
    }
    size_t StartIndex(const PathLevel&, const std::vector<int32_t>&) const noexcept
    {
        return 0;
    }
    bool Full() const
    {
        return max_values >= 0 && values.size() >= static_cast<size_t>(max_values);
    }
    bool RequiresContainerDictionary() const
    {
        return true;
    }
    void Number(void* pointer, const std::string& type, const std::vector<int32_t>& current_indices,
                const std::vector<std::string>&)
    {
        if (current_indices.size() != index_depth)
        {
            throw IOException("ROOT container depth mismatch while executing indexed access plan: expected " +
                              std::to_string(index_depth) + ", got " + std::to_string(current_indices.size()));
        }
        values.push_back(RootPrimitiveValue::FromPointer(pointer, type));
        flat_indices.insert(flat_indices.end(), current_indices.begin(), current_indices.end());
    }
    void String(void*, const std::string&, const std::vector<int32_t>&, const std::vector<std::string>&)
    {
    }

  private:
    idx_t index_depth;
    int64_t max_values;
    std::vector<RootPrimitiveValue>& values;
    std::vector<int32_t>& flat_indices;
};

// The resume position is a flattened index tuple, not a ROOT object pointer.
// Recreate TPushPop frames on every page; no proxy iterator survives a call.
class CursorPageSink
{
  public:
    CursorPageSink(size_t limit_p, int64_t entry_id_p, const std::vector<int32_t>& previous_p, bool skip_previous_p,
                   ReadResult& output_p)
        : limit(limit_p), entry_id(entry_id_p), previous(previous_p), skip_previous(skip_previous_p), output(output_p)
    {
    }

    bool Full() const noexcept
    {
        return output.size() >= limit;
    }
    bool RequiresContainerDictionary() const noexcept
    {
        return true;
    }

    size_t StartIndex(const PathLevel& level, const std::vector<int32_t>& prefix) const
    {
        if (!skip_previous || prefix.size() > previous.size() ||
            !std::equal(prefix.begin(), prefix.end(), previous.begin()))
        {
            return 0;
        }
        const size_t rank = level.is_fixed_array ? std::max<size_t>(1, level.array_dimensions.size()) : 1;
        if (previous.size() - prefix.size() < rank)
        {
            return 0;
        }
        if (!level.is_fixed_array || level.array_dimensions.size() <= 1)
        {
            if (previous[prefix.size()] < 0)
            {
                throw IOException("ROOT cursor has a negative container index");
            }
            return static_cast<size_t>(previous[prefix.size()]);
        }
        size_t index = 0;
        for (size_t dim = 0; dim < rank; ++dim)
        {
            const auto coordinate = previous[prefix.size() + dim];
            const auto dimension = level.array_dimensions[dim];
            if (coordinate < 0 || dimension == 0 || static_cast<uint64_t>(coordinate) >= dimension ||
                index > (std::numeric_limits<size_t>::max() - static_cast<size_t>(coordinate)) / dimension)
            {
                throw IOException("ROOT cursor has an invalid flattened array position");
            }
            index = index * dimension + static_cast<size_t>(coordinate);
        }
        return index;
    }

    void Number(void* pointer, const std::string& type, const std::vector<int32_t>& indices,
                const std::vector<std::string>& names)
    {
        if (Skip(indices))
        {
            return;
        }
        output.AddNumber(RootPrimitiveValue::FromPointer(pointer, type), entry_id, indices, names);
        last_indices = indices;
    }
    void String(void* pointer, const std::string& type, const std::vector<int32_t>& indices,
                const std::vector<std::string>& names)
    {
        if (Skip(indices))
        {
            return;
        }
        output.AddString(ReadRootString(pointer, type), entry_id, indices, names);
        last_indices = indices;
    }
    [[nodiscard]] const std::vector<int32_t>& LastIndices() const noexcept
    {
        return last_indices;
    }

  private:
    bool Skip(const std::vector<int32_t>& indices)
    {
        if (!skip_previous)
        {
            return false;
        }
        if (indices == previous)
        {
            skip_previous = false;
            return true;
        }
        // A restored cursor must encounter its previous value before moving past it.
        throw IOException("ROOT cursor could not restore its previous logical index");
    }

    size_t limit;
    int64_t entry_id;
    const std::vector<int32_t>& previous;
    bool skip_previous;
    ReadResult& output;
    std::vector<int32_t> last_indices;
};

template <class SINK>
void CollectValuesRecursive(void* current, const std::vector<PathLevel>& levels, size_t level_index,
                            std::vector<int32_t>& indices, std::vector<std::string>& index_names, SINK& sink)
{
    if (!current || level_index >= levels.size() || sink.Full())
    {
        return;
    }
    const auto& level = levels[level_index];
    auto* field = static_cast<char*>(current) + level.offset_in_parent;
    if (level.is_pointer)
    {
        field = field ? *reinterpret_cast<char**>(field) : nullptr;
        if (!field)
        {
            return;
        }
    }
    const bool last = level_index + 1 == levels.size();
    if (level.is_fixed_array)
    {
        AppendArrayIndexNames(level, indices.size(), index_names);
        const size_t pushed = std::max<size_t>(1, level.array_dimensions.size());
        for (uint64_t i = sink.StartIndex(level, indices); i < level.fixed_array_length && !sink.Full(); ++i)
        {
            auto* element = field + i * level.element_size;
            PushArrayCoordinates(i, level.array_dimensions, indices);
            if (last)
            {
                if (level.is_primitive)
                {
                    sink.Number(element, level.type, indices, index_names);
                }
            }
            else
            {
                CollectValuesRecursive(element, levels, level_index + 1, indices, index_names, sink);
            }
            indices.resize(indices.size() - pushed);
        }
        return;
    }
    if (last && level.is_container)
    {
        auto* proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
        if (!proxy)
        {
            return;
        }
        const TVirtualCollectionProxy::TPushPop guard(proxy, field);
        const auto inner = ExtractInnerType(level.type);
        const auto size = proxy->Size();
        if (index_names.size() < indices.size() + 1)
        {
            index_names.push_back(level.name + "_idx");
        }
        const auto access = PrepareContainerAccess(level, proxy, size);
        for (size_t i = sink.StartIndex(level, indices); i < size && !sink.Full(); ++i)
        {
            const auto container_index = CheckedContainerIndex(i);
            auto* element = access.contiguous ? static_cast<void*>(access.base + i * access.stride)
                                              : proxy->At(static_cast<UInt_t>(container_index));
            if (!element)
            {
                continue;
            }
            indices.push_back(container_index);
            if (IsPrimitiveType(inner))
            {
                sink.Number(element, inner, indices, index_names);
            }
            else if (IsStringType(inner))
            {
                sink.String(element, inner, indices, index_names);
            }
            indices.pop_back();
        }
        return;
    }
    if (last)
    {
        if (level.is_primitive)
        {
            sink.Number(field, level.type, indices, index_names);
        }
        else if (level.is_string)
        {
            sink.String(field, level.type, indices, index_names);
        }
        return;
    }
    if (level.is_container)
    {
        if (sink.RequiresContainerDictionary() && (!level.klass || !level.klass->HasDictionary()))
        {
            return;
        }
        auto* proxy = level.klass ? level.klass->GetCollectionProxy() : nullptr;
        if (!proxy)
        {
            return;
        }
        const TVirtualCollectionProxy::TPushPop guard(proxy, field);
        const auto size = proxy->Size();
        if (index_names.size() < indices.size() + 1)
        {
            index_names.push_back(level.name + "_idx");
        }
        const auto access = PrepareContainerAccess(level, proxy, size);
        for (size_t i = sink.StartIndex(level, indices); i < size && !sink.Full(); ++i)
        {
            const auto container_index = CheckedContainerIndex(i);
            auto* element = access.contiguous ? static_cast<void*>(access.base + i * access.stride)
                                              : proxy->At(static_cast<UInt_t>(container_index));
            if (!element)
            {
                continue;
            }
            indices.push_back(container_index);
            CollectValuesRecursive(element, levels, level_index + 1, indices, index_names, sink);
            indices.pop_back();
        }
        return;
    }
    if (level.klass)
    {
        CollectValuesRecursive(field, levels, level_index + 1, indices, index_names, sink);
    }
}

template <class SINK> void RunTraversal(void* root_object, const std::vector<PathLevel>& levels, SINK& sink)
{
    std::vector<int32_t> indices;
    std::vector<std::string> index_names;
    CollectValuesRecursive(root_object, levels, 0, indices, index_names, sink);
}

} // namespace

void OffsetValueReader::CollectTypedFlat(void* root_object, const std::vector<PathLevel>& levels, idx_t index_depth,
                                         int64_t max_values, std::vector<RootPrimitiveValue>& values,
                                         std::vector<int32_t>& flat_indices)
{
    FlatTypedSink sink{index_depth, max_values, values, flat_indices};
    RunTraversal(root_object, levels, sink);
}

void OffsetValueReader::CollectDirect(void* root_object, const std::vector<PathLevel>& levels, int64_t max_values,
                                      int64_t entry_id, ReadResult& out)
{
    DirectSink sink{max_values, entry_id, out};
    RunTraversal(root_object, levels, sink);
}

void OffsetValueCursor::Open(void* root_object, const std::vector<PathLevel>& levels, int64_t entry_id)
{
    Reset();
    root_object_ = root_object;
    levels_ = &levels;
    entry_id_ = entry_id;
    complete_ = !root_object || levels.empty();
}

bool OffsetValueCursor::ReadPage(idx_t maximum_rows, ReadResult& result)
{
    result.Clear();
    if (maximum_rows == 0)
    {
        throw InvalidInputException("ROOT cursor page size must be positive");
    }
    if (complete_)
    {
        return false;
    }
    CursorPageSink sink{static_cast<size_t>(maximum_rows), entry_id_, resume_indices_, previous_emitted_, result};
    RunTraversal(root_object_, *levels_, sink);
    if (result.empty())
    {
        complete_ = true;
        return false;
    }
    resume_indices_ = sink.LastIndices();
    previous_emitted_ = true;
    if (!sink.Full())
    {
        complete_ = true;
    }
    return true;
}

bool OffsetValueCursor::HasMore() const noexcept
{
    return !complete_;
}

void OffsetValueCursor::Reset() noexcept
{
    root_object_ = nullptr;
    levels_ = nullptr;
    resume_indices_.clear();
    entry_id_ = 0;
    previous_emitted_ = false;
    complete_ = true;
}

} // namespace duckdb::rootlake
