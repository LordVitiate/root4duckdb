#ifndef ROOT4DUCKDB_INDEX_ROOT_FILTER_HPP
#define ROOT4DUCKDB_INDEX_ROOT_FILTER_HPP

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/table_filter_set.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb
{
class ClientContext;
class ExpressionExecutor;
class Expression;
class ExpressionFilter;
class TableFilter;

namespace rootlake
{

bool RootIsColumnReference(const Expression& expression);
bool RootIsComparison(ExpressionType type);
/// True only for a direct reference-versus-constant comparison; never unwrap optional filters.
bool RootIsSimpleConstantComparisonFilter(const TableFilter& filter);
ExpressionType RootReverseComparison(ExpressionType type);

/// One filter binding whose owner is the copied scan-level TableFilterSet.
struct RootFilterBinding
{
    idx_t column_id;
    const TableFilter* filter;
};

template <typename Column, typename IsEntryScoped>
void PartitionRootRowFilters(const TableFilterSet& filters, const std::vector<Column>& scan_columns,
                             IsEntryScoped&& is_entry_scoped, std::vector<RootFilterBinding>& entry_filters,
                             std::vector<RootFilterBinding>& element_filters);

/// Exact scalar supplied to DuckDB table-filter evaluation.
struct RootScalarActual
{
    LogicalType type;
    bool is_null = false;
    double numeric = 0;
    uint64_t unsigned_value = 0;
    int64_t signed_value = 0;
    std::string string_value;

    static RootScalarActual Null(const LogicalType& type);
    static RootScalarActual Event(uint64_t value);
    static RootScalarActual Signed(int64_t value, const LogicalType& logical_type = LogicalType::BIGINT);
    static RootScalarActual Unsigned(uint64_t value, const LogicalType& logical_type = LogicalType::UBIGINT);
    static RootScalarActual Index(std::optional<int32_t> value);
    static RootScalarActual String(std::string value);
    static RootScalarActual Numeric(const LogicalType& logical_type, double value);
    Value ToValue() const;
};

/// Conservative inclusive range extracted from an unsigned filter.
struct RootUnsignedFilterRange
{
    bool known = false;
    bool impossible = false;
    uint64_t lower = 0;
    uint64_t upper = std::numeric_limits<uint64_t>::max();
};

/// Unwraps DuckDB's metadata-only optional table filters without executing them.
/// Returns nullptr when no safe expression is available.
const Expression* RootFilterMetadataExpression(const TableFilter& filter);

/// Extracts event/source bounds without rejecting uncertain filters.
RootUnsignedFilterRange ExtractRootUnsignedRange(const TableFilter& filter);

/// Evaluates DuckDB filters against scalar ROOT values.
class RootFilterEvaluator
{
  public:
    RootFilterEvaluator();
    RootFilterEvaluator(const RootFilterEvaluator&) = delete;
    RootFilterEvaluator& operator=(const RootFilterEvaluator&) = delete;
    ~RootFilterEvaluator();
    RootFilterEvaluator(RootFilterEvaluator&&) noexcept;
    RootFilterEvaluator& operator=(RootFilterEvaluator&&) noexcept;

    /// Evaluates one bound filter against one exact scalar.
    bool Evaluate(ClientContext& context, const TableFilter& filter, const RootScalarActual& actual);

  private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

template <typename ResolveScalar>
bool EvaluateRootRowFilters(ClientContext& context, RootFilterEvaluator& evaluator,
                            const std::vector<RootFilterBinding>& filters, ResolveScalar&& resolve);

} // namespace rootlake
} // namespace duckdb

#include "root4duckdb/index/root_filter.tpp"

#endif
