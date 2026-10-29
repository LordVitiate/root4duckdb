#include "root4duckdb/reader/root_semantic_types.hpp"

#include <cstring>

namespace duckdb::rootlake
{

namespace
{

template <class T> T LoadRootPrimitive(const void* pointer) noexcept
{
    T value{};
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}

} // namespace

RootPrimitiveValue RootPrimitiveValue::FromPointer(void* pointer, const std::string& raw_type)
{
    if (!pointer)
    {
        return Floating(0.0);
    }
    const auto type = PrimitiveBaseType(raw_type);
    if (type == "Bool_t" || type == "bool" || type == "O")
    {
        return Unsigned(LoadRootPrimitive<uint8_t>(pointer) != 0 ? 1 : 0);
    }
    if (type == "Char_t" || type == "char" || type == "b")
    {
        return Signed(LoadRootPrimitive<int8_t>(pointer));
    }
    if (type == "UChar_t" || type == "unsigned char" || type == "B")
    {
        return Unsigned(LoadRootPrimitive<uint8_t>(pointer));
    }
    if (type == "Short_t" || type == "short" || type == "S")
    {
        return Signed(LoadRootPrimitive<int16_t>(pointer));
    }
    if (type == "UShort_t" || type == "unsigned short" || type == "s")
    {
        return Unsigned(LoadRootPrimitive<uint16_t>(pointer));
    }
    if (type == "Int_t" || type == "int" || type == "I")
    {
        return Signed(LoadRootPrimitive<int32_t>(pointer));
    }
    if (type == "UInt_t" || type == "unsigned int" || type == "i")
    {
        return Unsigned(LoadRootPrimitive<uint32_t>(pointer));
    }
    if (type == "Long_t" || type == "long")
    {
        return Signed(static_cast<int64_t>(LoadRootPrimitive<long>(pointer)));
    }
    if (type == "ULong_t" || type == "unsigned long")
    {
        return Unsigned(static_cast<uint64_t>(LoadRootPrimitive<unsigned long>(pointer)));
    }
    if (type == "Long64_t" || type == "long long" || type == "L")
    {
        return Signed(LoadRootPrimitive<int64_t>(pointer));
    }
    if (type == "ULong64_t" || type == "unsigned long long" || type == "l")
    {
        return Unsigned(LoadRootPrimitive<uint64_t>(pointer));
    }
    if (type == "Float_t" || type == "float" || type == "F")
    {
        return Floating(LoadRootPrimitive<float>(pointer));
    }
    if (type == "Double_t" || type == "double" || type == "D")
    {
        return Floating(LoadRootPrimitive<double>(pointer));
    }
    throw NotImplementedException("Unsupported primitive ROOT type: " + raw_type);
}

RootPrimitiveValue RootPrimitiveValue::FromDouble(double value, const std::string& raw_type)
{
    switch (RootTypeToLogicalType(raw_type).id())
    {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
        return Unsigned(Floating(value).AsUnsigned());
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
        return Signed(Floating(value).AsSigned());
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        return Floating(value);
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UBIGINT:
        throw InternalException("Lossy double-to-64-bit ROOT primitive conversion was attempted");
    default:
        throw NotImplementedException("Unsupported ROOT primitive type: " + raw_type);
    }
}

size_t ReadResult::size() const
{
    return strings.size();
}

bool ReadResult::empty() const
{
    return strings.empty();
}

void ReadResult::Clear()
{
    strings.clear();
    numbers.clear();
    is_string_flag.clear();
    entry_ids.clear();
    vector_indices.clear();
    vector_names.clear();
    source_path.clear();
}

void ReadResult::AddString(const std::string& value, int64_t entry_id, const std::vector<int32_t>& indices,
                           const std::vector<std::string>& index_names)
{
    strings.push_back(value);
    numbers.emplace_back();
    is_string_flag.push_back(true);
    entry_ids.push_back(entry_id);
    vector_indices.emplace_back(indices.begin(), indices.end());
    if (vector_names.empty())
    {
        vector_names = index_names;
    }
}

void ReadResult::AddNumber(const RootPrimitiveValue& value, int64_t entry_id, const std::vector<int32_t>& indices,
                           const std::vector<std::string>& index_names)
{
    strings.emplace_back();
    numbers.push_back(value);
    is_string_flag.push_back(false);
    entry_ids.push_back(entry_id);
    vector_indices.emplace_back(indices.begin(), indices.end());
    if (vector_names.empty())
    {
        vector_names = index_names;
    }
}

void ReadResult::AddNumber(double value, int64_t entry_id, const std::vector<int32_t>& indices,
                           const std::vector<std::string>& index_names)
{
    AddNumber(RootPrimitiveValue::Floating(value), entry_id, indices, index_names);
}

} // namespace duckdb::rootlake
