#include "root4duckdb/dataset/root_dataset_pruning.hpp"

namespace duckdb::rootlake
{

optional_ptr<TableFilter> FilterForFullColumn(const DatasetGlobalState& global, column_t full_column)
{
    if (!global.filters)
    {
        return nullptr;
    }
    for (const auto& entry : global.filters->filters)
    {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size())
        {
            continue;
        }
        if (global.scan_column_ids[scan_position] == full_column)
        {
            return entry.second.get();
        }
    }
    return nullptr;
}

bool PassesFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                   const DatasetGlobalState& global, uint64_t event_fk, double numeric_value, const int32_t* indices,
                   idx_t index_count)
{
    if (!global.filters)
    {
        return true;
    }
    for (const auto& entry : global.filters->filters)
    {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size())
        {
            continue;
        }
        const column_t column = global.scan_column_ids[scan_position];
        RootScalarActual actual;
        if (column == 0)
        {
            actual = RootScalarActual::Event(event_fk);
        }
        else if (column >= 1 && static_cast<idx_t>(column) < bind.value_column)
        {
            const idx_t index_position = static_cast<idx_t>(column) - 1;
            actual = RootScalarActual::Index(
                index_position < index_count ? std::optional<int32_t>(indices[index_position]) : std::nullopt);
        }
        else if (static_cast<idx_t>(column) == bind.value_column)
        {
            actual = RootScalarActual::Numeric(bind.value_type, numeric_value);
        }
        else if (static_cast<idx_t>(column) == bind.source_id_column)
        {
            actual = RootScalarActual::String(local.value_source_id);
        }
        else if (static_cast<idx_t>(column) == bind.entry_id_column)
        {
            actual = RootScalarActual::Event(local.value_entry_id);
        }
        else
        {
            actual = RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!local.filter_evaluator.Evaluate(context, *entry.second, actual))
        {
            return false;
        }
    }
    return true;
}

static RootScalarActual DatasetPrimitiveActual(const LogicalType& type, const RootPrimitiveValue& value)
{
    switch (value.kind)
    {
    case RootPrimitiveKind::SIGNED:
        return RootScalarActual::Signed(value.signed_value, type);
    case RootPrimitiveKind::UNSIGNED:
        return RootScalarActual::Unsigned(value.unsigned_value, type);
    case RootPrimitiveKind::FLOATING:
        return RootScalarActual::Numeric(type, value.floating_value);
    }
    return RootScalarActual::Null(type);
}

bool PassesTypedFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                        const DatasetGlobalState& global, uint64_t event_fk, const RootPrimitiveValue& numeric_value,
                        const int32_t* indices, idx_t index_count)
{

    if (!global.filters)
    {
        return true;
    }
    for (const auto& entry : global.filters->filters)
    {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size())
        {
            continue;
        }
        const column_t column = global.scan_column_ids[scan_position];
        RootScalarActual actual;
        if (column == 0)
        {
            actual = RootScalarActual::Event(event_fk);
        }
        else if (column >= 1 && static_cast<idx_t>(column) < bind.value_column)
        {
            const idx_t index_position = static_cast<idx_t>(column) - 1;
            actual = RootScalarActual::Index(
                index_position < index_count ? std::optional<int32_t>(indices[index_position]) : std::nullopt);
        }
        else if (static_cast<idx_t>(column) == bind.value_column)
        {
            actual = DatasetPrimitiveActual(bind.value_type, numeric_value);
        }
        else if (static_cast<idx_t>(column) == bind.source_id_column)
        {
            actual = RootScalarActual::String(local.value_source_id);
        }
        else if (static_cast<idx_t>(column) == bind.entry_id_column)
        {
            actual = RootScalarActual::Event(local.value_entry_id);
        }
        else
        {
            actual = RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!local.filter_evaluator.Evaluate(context, *entry.second, actual))
        {
            return false;
        }
    }
    return true;
}

} // namespace duckdb::rootlake
