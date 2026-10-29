#include "root4duckdb/dataset/root_dataset_pruning.hpp"

namespace duckdb::rootlake
{

optional_ptr<const TableFilter> FilterForFullColumn(const DatasetGlobalState& global, column_t full_column)
{
    if (!global.filters)
    {
        return nullptr;
    }
    for (const auto& entry : *global.filters)
    {
        const auto scan_position = entry.GetIndex().GetIndex();
        if (scan_position < global.scan_column_ids.size() && global.scan_column_ids[scan_position] == full_column)
        {
            return &entry.Filter();
        }
    }
    return nullptr;
}

bool PassesDatasetEntryFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                               const DatasetGlobalState& global, uint64_t entry_id)
{
    return EvaluateRootRowFilters(context, local.filter_evaluator, global.entry_filters,
                                  [&](idx_t column) -> RootScalarActual
                                  {
                                      if (column == 0)
                                      {
                                          return RootScalarActual::Event(local.current_task.event_base + entry_id);
                                      }
                                      if (column == bind.entry_id_column)
                                      {
                                          return RootScalarActual::Event(entry_id);
                                      }
                                      if (column == bind.source_id_column)
                                      {
                                          return RootScalarActual::String(local.current_task.file_id);
                                      }
                                      return RootScalarActual::Null(LogicalType::SQLNULL);
                                  });
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

static RootScalarActual DatasetRowActual(const DatasetBindData& bind, const DatasetLocalState& local, idx_t column,
                                         uint64_t event_fk, double value, const RootPrimitiveValue* typed_value,
                                         const int32_t* indices, idx_t index_count)
{
    if (column == 0)
    {
        return RootScalarActual::Event(event_fk);
    }
    if (column >= 1 && column < bind.value_column)
    {
        const idx_t position = column - 1;
        return RootScalarActual::Index(position < index_count ? std::optional<int32_t>(indices[position])
                                                              : std::nullopt);
    }
    if (column == bind.value_column)
    {
        return typed_value ? DatasetPrimitiveActual(bind.value_type, *typed_value)
                           : RootScalarActual::Numeric(bind.value_type, value);
    }
    if (column == bind.source_id_column)
    {
        return RootScalarActual::String(local.value_source_id);
    }
    if (column == bind.entry_id_column)
    {
        return RootScalarActual::Event(local.value_entry_id);
    }
    return RootScalarActual::Null(LogicalType::SQLNULL);
}

bool PassesFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                   const DatasetGlobalState& global, uint64_t event_fk, double numeric_value, const int32_t* indices,
                   idx_t index_count)
{
    return EvaluateRootRowFilters(context, local.filter_evaluator, global.element_filters,
                                  [&](idx_t column)
                                  {
                                      return DatasetRowActual(bind, local, column, event_fk, numeric_value, nullptr,
                                                              indices, index_count);
                                  });
}

bool PassesTypedFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                        const DatasetGlobalState& global, uint64_t event_fk, const RootPrimitiveValue& numeric_value,
                        const int32_t* indices, idx_t index_count)
{
    return EvaluateRootRowFilters(context, local.filter_evaluator, global.element_filters,
                                  [&](idx_t column)
                                  {
                                      return DatasetRowActual(bind, local, column, event_fk, 0, &numeric_value, indices,
                                                              index_count);
                                  });
}

} // namespace duckdb::rootlake
