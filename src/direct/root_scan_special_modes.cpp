#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb
{

void RootScanExecutor::ProcessHistogramMode(ClientContext& context, const RootScanBindData& bind_data,
                                            RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                            DataChunk& output) const
{
    const auto* histogram_mode = bind_data.HistogramMode();
    if (!histogram_mode || !histogram_mode->object)
    {
        throw InternalException("ROOT histogram object is unavailable");
    }
    idx_t output_count = 0;
    std::vector<rootlake::RootScalarActual> row_values;
    while (output_count < STANDARD_VECTOR_SIZE)
    {
        if (lstate.local_current_row >= lstate.local_end_row)
        {
            if (!ClaimEntryBatch(gstate, lstate))
            {
                break;
            }
        }
        const auto row = lstate.local_current_row++;
        rootlake::MaterializeRootHistogramRow(histogram_mode->binding, *histogram_mode->object, row, row_values);
        bool passes = true;
        if (gstate.filters)
        {
            for (const auto& filter : *gstate.filters)
            {
                if (filter.GetIndex().GetIndex() >= gstate.scan_column_ids.size())
                {
                    continue;
                }
                const auto column = gstate.scan_column_ids[filter.GetIndex().GetIndex()];
                rootlake::RootScalarActual actual;
                if (column == COLUMN_IDENTIFIER_ROW_ID)
                {
                    actual = rootlake::RootScalarActual ::Unsigned(row, LogicalType(LogicalTypeId::UBIGINT));
                }
                else if (column < row_values.size())
                {
                    actual = row_values[column];
                }
                else
                {
                    actual = rootlake::RootScalarActual ::Null(LogicalType::SQLNULL);
                }
                if (!lstate.filter_evaluator.Evaluate(context, filter.Filter(), actual))
                {
                    passes = false;
                    break;
                }
            }
        }
        if (!passes)
        {
            continue;
        }
        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            const auto column = gstate.output_column_ids[output_index];
            auto& vector = output.data[output_index];
            if (column == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetDataMutable<int64_t>(vector)[output_count] = static_cast<int64_t>(row);
                FlatVector::ValidityMutable(vector).SetValid(output_count);
            }
            else if (column < row_values.size())
            {
                rootlake::WriteRootHistogramActual(vector, output_count, row_values[column]);
            }
            else
            {
                FlatVector::ValidityMutable(vector).SetInvalid(output_count);
            }
        }
        ++output_count;
    }
    output.SetCardinalityUnsafe(output_count);
}

void RootScanExecutor::ProcessBrowseMode(ClientContext& context, const RootScanBindData& bind_data,
                                         RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                         DataChunk& output) const
{
    const auto* browse_mode = bind_data.BrowseMode();
    if (!browse_mode)
    {
        throw InternalException("read_root browse execution has no browse plan");
    }
    const auto& children = browse_mode->children;
    const std::lock_guard<std::mutex> lock(gstate.coordination_mutex);
    const size_t start = gstate.browse_offset;
    if (start >= children.size())
    {
        output.SetCardinalityUnsafe(0);
        return;
    }
    size_t count = 0;
    size_t i = start;
    for (; i < children.size() && count < STANDARD_VECTOR_SIZE; ++i)
    {
        bool passes = true;
        if (gstate.filters)
        {
            for (const auto& filter : *gstate.filters)
            {
                if (filter.GetIndex().GetIndex() >= gstate.scan_column_ids.size())
                {
                    continue;
                }
                const auto column = gstate.scan_column_ids[filter.GetIndex().GetIndex()];
                const auto actual = column == COLUMN_IDENTIFIER_ROW_ID
                                        ? rootlake::RootScalarActual::Event(i)
                                        : (column == 0 ? rootlake::RootScalarActual::String(children[i])
                                                       : rootlake::RootScalarActual::Null(LogicalType::SQLNULL));
                if (!lstate.filter_evaluator.Evaluate(context, filter.Filter(), actual))
                {
                    passes = false;
                    break;
                }
            }
        }
        if (!passes)
        {
            continue;
        }
        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            auto& vector = output.data[output_index];
            const auto column = gstate.output_column_ids[output_index];
            if (column == 0)
            {
                FlatVector::GetDataMutable<string_t>(vector)[count] = StringVector::AddString(vector, children[i]);
                FlatVector::ValidityMutable(vector).SetValid(count);
            }
            else if (column == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetDataMutable<int64_t>(vector)[count] = static_cast<int64_t>(i);
                FlatVector::ValidityMutable(vector).SetValid(count);
            }
            else
            {
                FlatVector::ValidityMutable(vector).SetInvalid(count);
            }
        }
        ++count;
    }
    gstate.browse_offset = i;
    output.SetCardinalityUnsafe(count);
}

rootlake::RootScalarActual PrimitiveScalarActual(const LogicalType& logical_type,
                                                 const rootlake::RootPrimitiveValue& value)
{
    switch (value.kind)
    {
    case rootlake::RootPrimitiveKind::SIGNED:
        return rootlake::RootScalarActual::Signed(value.signed_value, logical_type);
    case rootlake::RootPrimitiveKind::UNSIGNED:
        return rootlake::RootScalarActual::Unsigned(value.unsigned_value, logical_type);
    case rootlake::RootPrimitiveKind::FLOATING:
        return rootlake::RootScalarActual::Numeric(logical_type, value.floating_value);
    }
    return rootlake::RootScalarActual::Null(logical_type);
}

} // namespace duckdb
