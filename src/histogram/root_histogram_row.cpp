#include "root4duckdb/histogram/root_histogram_reader.hpp"

#include "root4duckdb/histogram/root_histogram_internal.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb::rootlake
{

void MaterializeRootHistogramRow(const RootHistogramBinding& binding, TH1& histogram, uint64_t row,
                                 std::vector<RootScalarActual>& values)
{
    if (row >= binding.row_count)
    {
        throw InternalException("ROOT histogram row outside bound range");
    }
    switch (binding.view)
    {
    case RootHistogramView::BINS:
        histogram_detail::MaterializeBinRow(binding, histogram, row, values);
        break;
    case RootHistogramView::AXES:
        histogram_detail::MaterializeAxisRow(binding, histogram, row, values);
        break;
    case RootHistogramView::META:
        histogram_detail::MaterializeMetaRow(binding, histogram, values);
        break;
    }
    if (values.size() != binding.schema.names.size())
    {
        throw InternalException("ROOT histogram schema/materialization mismatch");
    }
}

void WriteRootHistogramActual(Vector& vector, idx_t row, const RootScalarActual& actual)
{
    if (actual.is_null)
    {
        FlatVector::ValidityMutable(vector).SetInvalid(row);
        return;
    }
    switch (vector.GetType().id())
    {
    case LogicalTypeId::BOOLEAN:
        FlatVector::GetDataMutable<bool>(vector)[row] = actual.numeric != 0;
        break;
    case LogicalTypeId::TINYINT:
        FlatVector::GetDataMutable<int8_t>(vector)[row] = static_cast<int8_t>(actual.signed_value);
        break;
    case LogicalTypeId::UTINYINT:
        FlatVector::GetDataMutable<uint8_t>(vector)[row] = static_cast<uint8_t>(actual.unsigned_value);
        break;
    case LogicalTypeId::SMALLINT:
        FlatVector::GetDataMutable<int16_t>(vector)[row] = static_cast<int16_t>(actual.signed_value);
        break;
    case LogicalTypeId::USMALLINT:
        FlatVector::GetDataMutable<uint16_t>(vector)[row] = static_cast<uint16_t>(actual.unsigned_value);
        break;
    case LogicalTypeId::INTEGER:
        FlatVector::GetDataMutable<int32_t>(vector)[row] = static_cast<int32_t>(actual.signed_value);
        break;
    case LogicalTypeId::UINTEGER:
        FlatVector::GetDataMutable<uint32_t>(vector)[row] = static_cast<uint32_t>(actual.unsigned_value);
        break;
    case LogicalTypeId::BIGINT:
        FlatVector::GetDataMutable<int64_t>(vector)[row] = actual.signed_value;
        break;
    case LogicalTypeId::UBIGINT:
        FlatVector::GetDataMutable<uint64_t>(vector)[row] = actual.unsigned_value;
        break;
    case LogicalTypeId::FLOAT:
        FlatVector::GetDataMutable<float>(vector)[row] = static_cast<float>(actual.numeric);
        break;
    case LogicalTypeId::DOUBLE:
        FlatVector::GetDataMutable<double>(vector)[row] = actual.numeric;
        break;
    case LogicalTypeId::VARCHAR:
        FlatVector::GetDataMutable<string_t>(vector)[row] = StringVector::AddString(vector, actual.string_value);
        break;
    default:
        throw NotImplementedException("Unsupported ROOT histogram SQL type " + vector.GetType().ToString());
    }
    FlatVector::ValidityMutable(vector).SetValid(row);
}

} // namespace duckdb::rootlake
