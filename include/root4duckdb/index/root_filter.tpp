namespace duckdb::rootlake
{

template <typename Column, typename IsEntryScoped>
void PartitionRootRowFilters(const TableFilterSet& filters, const std::vector<Column>& scan_columns,
                             IsEntryScoped&& is_entry_scoped, std::vector<RootFilterBinding>& entry_filters,
                             std::vector<RootFilterBinding>& element_filters)
{
    for (const auto& item : filters)
    {
        const idx_t position = item.GetIndex().GetIndex();
        if (position >= scan_columns.size())
        {
            continue;
        }
        const idx_t column = static_cast<idx_t>(scan_columns[position]);
        const RootFilterBinding binding{column, &item.Filter()};
        auto& destination = RootIsSimpleConstantComparisonFilter(item.Filter()) && is_entry_scoped(column)
                                ? entry_filters
                                : element_filters;
        destination.push_back(binding);
    }
}

template <typename ResolveScalar>
bool EvaluateRootRowFilters(ClientContext& context, RootFilterEvaluator& evaluator,
                            const std::vector<RootFilterBinding>& filters, ResolveScalar&& resolve)
{
    for (const auto& binding : filters)
    {
        if (!evaluator.Evaluate(context, *binding.filter, resolve(binding.column_id)))
        {
            return false;
        }
    }
    return true;
}

} // namespace duckdb::rootlake
