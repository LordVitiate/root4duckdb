#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb
{

namespace
{

struct CachedIndexSource
{
    idx_t column;
    size_t position;
};

std::string WithoutIndexSuffix(std::string name)
{
    if (name.size() > 4 && name.substr(name.size() - 4) == "_idx")
    {
        name.resize(name.size() - 4);
    }
    return name;
}

// Resolve index names once per cached result page, not once per output row.
std::optional<CachedIndexSource> FindCachedIndexSource(const RootScanBindData& bind_data,
                                                       const RootScanLocalState& lstate, idx_t column_id,
                                                       std::optional<size_t> row)
{
    if (column_id >= bind_data.columns.size())
    {
        return std::nullopt;
    }
    const auto& column = bind_data.columns[column_id];
    const auto name = WithoutIndexSuffix(column.name);
    for (idx_t candidate_id = 0; candidate_id < bind_data.columns.size(); ++candidate_id)
    {
        const auto& candidate = bind_data.columns[candidate_id];
        if (candidate.is_virtual_index || candidate.levels.empty() ||
            candidate.root_class_name != column.root_class_name || candidate_id >= lstate.cached_results.size())
        {
            continue;
        }
        const auto& result = lstate.cached_results[candidate_id];
        if (result.empty() || (row && (*row >= result.size() || *row >= result.vector_indices.size())))
        {
            continue;
        }
        for (size_t pos = 0; pos < result.vector_names.size(); ++pos)
        {
            if (WithoutIndexSuffix(result.vector_names[pos]) == name &&
                (!row || pos < result.vector_indices[*row].size()))
            {
                return CachedIndexSource{candidate_id, pos};
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool RootScanExecutor::WriteNumericValue(Vector& vector, idx_t row, const rootlake::RootPrimitiveValue& value) const
{
    switch (vector.GetType().id())
    {
    case LogicalTypeId::TINYINT:
        FlatVector::GetDataMutable<int8_t>(vector)[row] = value.AsInteger<int8_t>();
        break;
    case LogicalTypeId::UTINYINT:
        FlatVector::GetDataMutable<uint8_t>(vector)[row] = value.AsInteger<uint8_t>();
        break;
    case LogicalTypeId::SMALLINT:
        FlatVector::GetDataMutable<int16_t>(vector)[row] = value.AsInteger<int16_t>();
        break;
    case LogicalTypeId::USMALLINT:
        FlatVector::GetDataMutable<uint16_t>(vector)[row] = value.AsInteger<uint16_t>();
        break;
    case LogicalTypeId::INTEGER:
        FlatVector::GetDataMutable<int32_t>(vector)[row] = value.AsInteger<int32_t>();
        break;
    case LogicalTypeId::UINTEGER:
        FlatVector::GetDataMutable<uint32_t>(vector)[row] = value.AsInteger<uint32_t>();
        break;
    case LogicalTypeId::BIGINT:
        FlatVector::GetDataMutable<int64_t>(vector)[row] = value.AsSigned();
        break;
    case LogicalTypeId::UBIGINT:
        FlatVector::GetDataMutable<uint64_t>(vector)[row] = value.AsUnsigned();
        break;
    case LogicalTypeId::FLOAT:
        FlatVector::GetDataMutable<float>(vector)[row] = value.AsFloat();
        break;
    case LogicalTypeId::DOUBLE:
        FlatVector::GetDataMutable<double>(vector)[row] = value.AsDouble();
        break;
    case LogicalTypeId::BOOLEAN:
        FlatVector::GetDataMutable<bool>(vector)[row] = value.AsBool();
        break;
    default:
        FlatVector::ValidityMutable(vector).SetInvalid(row);
        return false;
    }
    FlatVector::ValidityMutable(vector).SetValid(row);
    return true;
}

std::optional<int32_t> RootScanExecutor::ResolveCachedIndexValue(const RootScanBindData& bind_data,
                                                                 const RootScanLocalState& lstate, idx_t col_idx,
                                                                 size_t elem_idx) const
{
    if (col_idx < lstate.cached_index_sources.size() && lstate.cached_index_sources[col_idx])
    {
        const auto [source_column, source_position] = *lstate.cached_index_sources[col_idx];
        if (source_column < lstate.cached_results.size())
        {
            const auto& result = lstate.cached_results[source_column];
            if (elem_idx < result.size() && elem_idx < result.vector_indices.size() &&
                source_position < result.vector_indices[elem_idx].size())
            {
                return result.vector_indices[elem_idx][source_position];
            }
        }
    }
    // Sparse/mismatched sibling pages may require a different valid source
    // for this row. Preserve the original per-row search in that rare case.
    const auto fallback = FindCachedIndexSource(bind_data, lstate, col_idx, elem_idx);
    if (!fallback)
    {
        return std::nullopt;
    }
    return lstate.cached_results[fallback->column].vector_indices[elem_idx][fallback->position];
}

rootlake::RootScalarActual RootScanExecutor::CachedScalar(const RootScanBindData& bind_data,
                                                          const RootScanLocalState& lstate, idx_t col_idx,
                                                          uint64_t entry, size_t elem_idx) const
{
    if (col_idx == COLUMN_IDENTIFIER_ROW_ID)
    {
        return rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));
    }
    if (col_idx >= bind_data.columns.size())
    {
        return rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
    }
    const auto& column = bind_data.columns[col_idx];
    if (column.name == "entry_id" && column.levels.empty())
    {
        return rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));
    }
    if (col_idx == bind_data.source_id_column)
    {
        return rootlake::RootScalarActual::Event(lstate.file_task.source_id);
    }
    if (col_idx == bind_data.source_path_column)
    {
        return rootlake::RootScalarActual::String(lstate.file_task.path);
    }
    if (column.is_virtual_index)
    {
        return rootlake::RootScalarActual::Index(ResolveCachedIndexValue(bind_data, lstate, col_idx, elem_idx));
    }
    const auto logical_type = rootlake::RootTypeToScanLogicalType(column.root_type, column.is_string, true);
    if (col_idx >= lstate.cached_results.size())
    {
        return rootlake::RootScalarActual::Null(logical_type);
    }
    const auto& result = lstate.cached_results[col_idx];
    if (elem_idx >= result.size())
    {
        return rootlake::RootScalarActual::Null(logical_type);
    }
    if (result.is_string_flag[elem_idx])
    {
        return rootlake::RootScalarActual::String(result.strings[elem_idx]);
    }
    return PrimitiveScalarActual(logical_type, result.numbers[elem_idx]);
}

bool RootScanExecutor::PassesCachedFilters(ClientContext& context, const RootScanBindData& bind_data,
                                           const RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                           uint64_t entry, size_t elem_idx, bool entry_only) const
{
    const auto& bindings = entry_only ? gstate.entry_filters : gstate.element_filters;
    return rootlake::EvaluateRootRowFilters(context, lstate.filter_evaluator, bindings,
                                            [&](idx_t column_id)
                                            {
                                                return CachedScalar(bind_data, lstate, column_id, entry, elem_idx);
                                            });
}

bool RootScanExecutor::PassesPrimitiveTreeFilters(ClientContext& context, const RootScanBindData& bind_data,
                                                  const RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                                  uint64_t entry) const
{
    if (!gstate.filters)
    {
        return true;
    }
    for (const auto& filter : *gstate.filters)
    {
        if (filter.GetIndex().GetIndex() >= gstate.scan_column_ids.size())
        {
            continue;
        }
        const auto column_index = gstate.scan_column_ids[filter.GetIndex().GetIndex()];
        auto actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        if (column_index == 0 || column_index == COLUMN_IDENTIFIER_ROW_ID)
        {
            actual = rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));
        }
        else if (column_index == bind_data.source_id_column)
        {
            actual = rootlake::RootScalarActual::Event(lstate.file_task.source_id);
        }
        else if (column_index == bind_data.source_path_column)
        {
            actual = rootlake::RootScalarActual::String(lstate.file_task.path);
        }
        else if (column_index < bind_data.columns.size() && column_index < lstate.primitive_tree_leaves.size())
        {
            const auto& column = bind_data.columns[column_index];
            auto* leaf = lstate.primitive_tree_leaves[column_index];
            if (leaf && leaf->GetValuePointer())
            {
                const auto value =
                    rootlake::RootPrimitiveValue ::FromPointer(leaf->GetValuePointer(), column.root_type);
                const auto logical_type = rootlake ::RootTypeToScanLogicalType(column.root_type, false, true);
                switch (value.kind)
                {
                case rootlake ::RootPrimitiveKind ::SIGNED:
                    actual = rootlake ::RootScalarActual ::Signed(value.signed_value, logical_type);
                    break;
                case rootlake ::RootPrimitiveKind ::UNSIGNED:
                    actual = rootlake ::RootScalarActual ::Unsigned(value.unsigned_value, logical_type);
                    break;
                case rootlake ::RootPrimitiveKind ::FLOATING:
                    actual = rootlake ::RootScalarActual ::Numeric(logical_type, value.floating_value);
                    break;
                }
            }
        }
        if (!lstate.filter_evaluator.Evaluate(context, filter.Filter(), actual))
        {
            return false;
        }
    }
    return true;
}

void RootScanExecutor::ProcessPrimitiveTree(ClientContext& context, const RootScanBindData& bind_data,
                                            RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                            DataChunk& output) const
{
    auto* tree = lstate.root_file.GetTTree();
    if (!tree)
    {
        output.SetCardinalityUnsafe(0);
        return;
    }
    idx_t output_count = 0;
    while (output_count < STANDARD_VECTOR_SIZE)
    {
        if (lstate.local_current_row >= lstate.local_end_row)
        {
            if (gstate.file_scheduler)
            {
                break;
            }
            if (!ClaimEntryBatch(gstate, lstate))
            {
                break;
            }
        }
        const auto entry = lstate.local_current_row++;
        if (lstate.primitive_tree_requires_read)
        {
            if (tree->GetEntry(static_cast<Long64_t>(entry)) < 0)
            {
                throw IOException("ROOT TTree::GetEntry failed in " + lstate.file_task.path + " at entry " +
                                  std::to_string(entry));
            }
        }
        if (!PassesPrimitiveTreeFilters(context, bind_data, gstate, lstate, entry))
        {
            continue;
        }
        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            const auto column_index = gstate.output_column_ids[output_index];
            auto& vector = output.data[output_index];
            if (column_index == 0 || column_index == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetDataMutable<int64_t>(vector)[output_count] = static_cast<int64_t>(entry);
                FlatVector::ValidityMutable(vector).SetValid(output_count);
                continue;
            }
            if (column_index == bind_data.source_id_column)
            {
                FlatVector::GetDataMutable<uint64_t>(vector)[output_count] = lstate.file_task.source_id;
                FlatVector::ValidityMutable(vector).SetValid(output_count);
                continue;
            }
            if (column_index == bind_data.source_path_column)
            {
                FlatVector::GetDataMutable<string_t>(vector)[output_count] =
                    StringVector::AddString(vector, lstate.file_task.path);
                FlatVector::ValidityMutable(vector).SetValid(output_count);
                continue;
            }
            if (column_index >= bind_data.columns.size() || column_index >= lstate.primitive_tree_leaves.size())
            {
                FlatVector::ValidityMutable(vector).SetInvalid(output_count);
                continue;
            }
            auto* leaf = lstate.primitive_tree_leaves[column_index];
            const auto& column = bind_data.columns[column_index];
            if (!leaf || !leaf->GetValuePointer())
            {
                FlatVector::ValidityMutable(vector).SetInvalid(output_count);
                continue;
            }
            const auto value = rootlake::RootPrimitiveValue ::FromPointer(leaf->GetValuePointer(), column.root_type);
            WriteNumericValue(vector, output_count, value);
        }
        ++output_count;
    }
    output.SetCardinalityUnsafe(output_count);
}

void RootScanExecutor::ProcessCachedEntry(ClientContext& context, const RootScanBindData& bind_data,
                                          RootScanGlobalState& gstate, RootScanLocalState& lstate, DataChunk& output,
                                          idx_t& out_count) const
{
    const uint64_t entry = lstate.current_entry;
    size_t max_elements = 0;
    for (const auto& res : lstate.cached_results)
    {
        if (!res.empty())
        {
            max_elements = std::max(max_elements, res.size());
        }
    }
    if (max_elements == 0)
    {
        max_elements = 1;
    }
    // Bind virtual-index sources once per decoded page, not once per SQL chunk.
    if (lstate.cached_index_sources.empty())
    {
        lstate.cached_index_sources.resize(bind_data.columns.size());
        // Resolve each virtual index at most once per decoded page; both projected
        // columns and element-level filters use the same exact structural source.
        const auto resolve_index = [&](idx_t column_id)
        {
            if (column_id >= bind_data.columns.size() || !bind_data.columns[column_id].is_virtual_index ||
                lstate.cached_index_sources[column_id])
            {
                return;
            }
            const auto source = FindCachedIndexSource(bind_data, lstate, column_id, std::nullopt);
            if (source)
            {
                lstate.cached_index_sources[column_id] = std::make_pair(source->column, source->position);
            }
        };
        for (const auto column_id : gstate.output_column_ids)
        {
            resolve_index(column_id);
        }
        for (const auto& binding : gstate.element_filters)
        {
            resolve_index(binding.column_id);
        }
    }
    // Metadata-only filters are invariant for the entire ROOT event/page.
    const bool entry_filters_pass = PassesCachedFilters(context, bind_data, gstate, lstate, entry, 0, true);
    const size_t start_elem = lstate.current_elem_idx;
    size_t next_elem = start_elem;
    for (size_t elem_idx = start_elem; elem_idx < max_elements && out_count < STANDARD_VECTOR_SIZE; ++elem_idx)
    {
        next_elem = elem_idx + 1;
        if (!entry_filters_pass || (!gstate.element_filters.empty() &&
                                    !PassesCachedFilters(context, bind_data, gstate, lstate, entry, elem_idx, false)))
        {
            continue;
        }
        for (idx_t out_idx = 0; out_idx < gstate.output_column_ids.size(); ++out_idx)
        {
            const idx_t col_idx = gstate.output_column_ids[out_idx];
            auto& vec = output.data[out_idx];
            if (col_idx == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetDataMutable<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::ValidityMutable(vec).SetValid(out_count);
                continue;
            }
            if (col_idx >= bind_data.columns.size())
            {
                FlatVector::ValidityMutable(vec).SetInvalid(out_count);
                continue;
            }
            const auto& col = bind_data.columns[col_idx];
            if (col.name == "entry_id" && col.levels.empty())
            {
                FlatVector::GetDataMutable<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::ValidityMutable(vec).SetValid(out_count);
                continue;
            }
            if (col_idx == bind_data.source_id_column)
            {
                FlatVector::GetDataMutable<uint64_t>(vec)[out_count] = lstate.file_task.source_id;
                FlatVector::ValidityMutable(vec).SetValid(out_count);
                continue;
            }
            if (col_idx == bind_data.source_path_column)
            {
                FlatVector::GetDataMutable<string_t>(vec)[out_count] =
                    StringVector::AddString(vec, lstate.file_task.path);
                FlatVector::ValidityMutable(vec).SetValid(out_count);
                continue;
            }
            if (col.is_virtual_index)
            {
                const auto index = ResolveCachedIndexValue(bind_data, lstate, col_idx, elem_idx);
                if (!index)
                {
                    RootDebug("INDEX.NO_REFERENCE",
                              "column=" + col.name + " root_class=" + col.root_class_name +
                                  " projected_columns=" + std::to_string(gstate.output_column_ids.size()));
                    FlatVector::ValidityMutable(vec).SetInvalid(out_count);
                    continue;
                }
                FlatVector::GetDataMutable<int32_t>(vec)[out_count] = *index;
                FlatVector::ValidityMutable(vec).SetValid(out_count);
                continue;
            }
            if (col_idx >= lstate.cached_results.size())
            {
                FlatVector::ValidityMutable(vec).SetInvalid(out_count);
                continue;
            }
            const auto& res = lstate.cached_results[col_idx];
            if (elem_idx >= res.size())
            {
                FlatVector::ValidityMutable(vec).SetInvalid(out_count);
                continue;
            }
            if (res.is_string_flag[elem_idx])
            {
                if (vec.GetType().id() == LogicalTypeId::VARCHAR)
                {
                    FlatVector::GetDataMutable<string_t>(vec)[out_count] =
                        StringVector::AddString(vec, res.strings[elem_idx]);
                    FlatVector::ValidityMutable(vec).SetValid(out_count);
                }
                else
                {
                    FlatVector::ValidityMutable(vec).SetInvalid(out_count);
                }
            }
            else
            {
                const auto& val = res.numbers[elem_idx];
                if (!WriteNumericValue(vec, out_count, val))
                {
                    continue;
                }
            }
        }
        out_count++;
    }
    lstate.current_elem_idx = next_elem;
    if (lstate.current_elem_idx >= max_elements)
    {
        lstate.has_cached_entry = false;
        lstate.cached_results.clear();
        lstate.cached_index_sources.clear();
        if (!lstate.cursor_entry_active)
        {
            lstate.local_current_row++;
        }
        else if (!lstate.cursor_has_more)
        {
            EndEntryCursors(lstate);
            lstate.local_current_row++;
        }
    }
}

} // namespace duckdb
