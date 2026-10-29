#include "root4duckdb/index/root_filter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/table_filter.hpp"

#include <algorithm>
#include <limits>

namespace duckdb::rootlake
{

static const ExpressionFilter& RequireExpressionFilter(const TableFilter& filter)
{
    if (filter.filter_type != TableFilterType::EXPRESSION_FILTER)
    {
        throw InternalException("ROOT scalar filter requires DuckDB 2.0 ExpressionFilter");
    }
    return filter.Cast<ExpressionFilter>();
}

const Expression* RootFilterMetadataExpression(const TableFilter& filter)
{
    const auto& root = RequireExpressionFilter(filter);
    const Expression* expression = root.expr.get();
    // In DuckDB 2.0 a non-dense IN reaches scans wrapped in a metadata-only
    // optional filter. Its real expression lives in FunctionData, not in
    // the function's visible argument list.
    // Unwrap for conservative metadata pruning ONLY. Row evaluation retains
    // DuckDB's optional-filter semantics: optional filters always pass.
    for (idx_t depth = 0; expression && depth < 16; ++depth)
    {
        if (expression->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION)
        {
            return expression;
        }
        const auto& function = expression->Cast<BoundFunctionExpression>();
        if (function.Function().GetName() == OptionalFilterScalarFun::NAME)
        {
            if (!function.BindInfo())
            {
                return nullptr;
            }
            expression = function.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr.get();
        }
        else if (function.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME)
        {
            if (!function.BindInfo())
            {
                return nullptr;
            }
            expression = function.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get();
        }
        else
        {
            return expression;
        }
    }
    return nullptr;
}

bool RootIsColumnReference(const Expression& expression)
{
    return expression.GetExpressionClass() == ExpressionClass::BOUND_REF ||
           expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

bool RootIsComparison(ExpressionType type)
{
    switch (type)
    {
    case ExpressionType::COMPARE_EQUAL:
    case ExpressionType::COMPARE_NOTEQUAL:
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return true;
    default:
        return false;
    }
}

bool RootIsSimpleConstantComparisonFilter(const TableFilter& filter)
{
    if (filter.filter_type != TableFilterType::EXPRESSION_FILTER)
    {
        return false;
    }
    const auto& expression = filter.Cast<ExpressionFilter>().expr;
    if (!expression || expression->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
        !RootIsComparison(expression->GetExpressionType()))
    {
        return false;
    }
    const auto& children = expression->Cast<BoundFunctionExpression>().GetChildren();
    if (children.size() != 2 || !children[0] || !children[1])
    {
        return false;
    }
    return (RootIsColumnReference(*children[0]) &&
            children[1]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) ||
           (RootIsColumnReference(*children[1]) &&
            children[0]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);
}

ExpressionType RootReverseComparison(ExpressionType type)
{
    switch (type)
    {
    case ExpressionType::COMPARE_LESSTHAN:
        return ExpressionType::COMPARE_GREATERTHAN;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
    case ExpressionType::COMPARE_GREATERTHAN:
        return ExpressionType::COMPARE_LESSTHAN;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return ExpressionType::COMPARE_LESSTHANOREQUALTO;
    default:
        return type;
    }
}

class RootFilterEvaluator::Impl
{
  public:
    bool Evaluate(ClientContext& context, const TableFilter& filter, const RootScalarActual& actual)
    {
        const auto& expression = RequireExpressionFilter(filter);
        auto executor = expression_executors.find(&expression);
        if (executor == expression_executors.end())
        {
            executor =
                expression_executors.emplace(&expression, make_uniq<ExpressionExecutor>(context, *expression.expr))
                    .first;
        }
        DataChunk input;
        input.data.emplace_back(actual.ToValue(), count_t(1));
        SelectionVector selection(1);
        return executor->second->SelectExpression(input, selection) > 0;
    }

  private:
    std::unordered_map<const ExpressionFilter*, unique_ptr<ExpressionExecutor>> expression_executors;
};

RootFilterEvaluator::RootFilterEvaluator() : impl(std::make_unique<Impl>())
{
}
RootFilterEvaluator::~RootFilterEvaluator() = default;
RootFilterEvaluator::RootFilterEvaluator(RootFilterEvaluator&&) noexcept = default;
RootFilterEvaluator& RootFilterEvaluator::operator=(RootFilterEvaluator&&) noexcept = default;

bool RootFilterEvaluator::Evaluate(ClientContext& context, const TableFilter& filter, const RootScalarActual& actual)
{
    return impl->Evaluate(context, filter, actual);
}

namespace
{

static std::optional<uint64_t> FilterConstantAsUInt64(const Value& value)
{
    if (value.IsNull() || !value.type().IsIntegral())
    {
        // A lossy floating-point to unsigned cast must not narrow entry ranges.
        return std::nullopt;
    }
    try
    {
        return value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
    }
    catch (const Exception&)
    {
        return std::nullopt;
    }
}

static RootUnsignedFilterRange ComparisonRange(ExpressionType type, const Value& constant)
{
    RootUnsignedFilterRange result;
    const auto value = FilterConstantAsUInt64(constant);
    if (!value)
    {
        return result;
    }
    result.known = true;
    switch (type)
    {
    case ExpressionType::COMPARE_EQUAL:
        result.lower = result.upper = *value;
        break;
    case ExpressionType::COMPARE_LESSTHAN:
        if (*value == 0)
        {
            result.impossible = true;
        }
        else
        {
            result.upper = *value - 1;
        }
        break;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        result.upper = *value;
        break;
    case ExpressionType::COMPARE_GREATERTHAN:
        if (*value == std::numeric_limits<uint64_t>::max())
        {
            result.impossible = true;
        }
        else
        {
            result.lower = *value + 1;
        }
        break;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        result.lower = *value;
        break;
    default:
        result.known = false;
        break;
    }
    return result;
}

static RootUnsignedFilterRange IntersectRanges(const RootUnsignedFilterRange& left,
                                               const RootUnsignedFilterRange& right)
{
    if (!left.known)
    {
        return right;
    }
    if (!right.known)
    {
        return left;
    }
    RootUnsignedFilterRange result;
    result.known = true;
    result.impossible = left.impossible || right.impossible;
    if (!result.impossible)
    {
        result.lower = std::max(left.lower, right.lower);
        result.upper = std::min(left.upper, right.upper);
        result.impossible = result.lower > result.upper;
    }
    return result;
}

static RootUnsignedFilterRange UnionRanges(const RootUnsignedFilterRange& left, const RootUnsignedFilterRange& right)
{
    RootUnsignedFilterRange result;
    if (!left.known || !right.known)
    {
        return result;
    }
    result.known = true;
    if (left.impossible)
    {
        return right;
    }
    if (right.impossible)
    {
        return left;
    }
    result.lower = std::min(left.lower, right.lower);
    result.upper = std::max(left.upper, right.upper);
    return result;
}

static RootUnsignedFilterRange ExtractUnsignedExpressionRange(const Expression& expression)
{
    RootUnsignedFilterRange unknown;
    if (expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
        RootIsComparison(expression.GetExpressionType()))
    {
        const auto& args = expression.Cast<BoundFunctionExpression>().GetChildren();
        if (args.size() != 2)
        {
            return unknown;
        }
        const Expression* constant = nullptr;
        auto type = expression.GetExpressionType();
        if (RootIsColumnReference(*args[0]) && args[1]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT)
        {
            constant = args[1].get();
        }
        else if (RootIsColumnReference(*args[1]) && args[0]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT)
        {
            constant = args[0].get();
            type = RootReverseComparison(type);
        }
        return constant ? ComparisonRange(type, constant->Cast<BoundConstantExpression>().GetValue()) : unknown;
    }

    if (expression.GetExpressionType() == ExpressionType::COMPARE_BETWEEN &&
        expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION)
    {
        const auto& between = expression.Cast<BoundFunctionExpression>();
        const auto& args = between.GetChildren();
        if (args.size() != 3 || !RootIsColumnReference(*args[0]))
        {
            return unknown;
        }
        const auto& lower = *args[1];
        const auto& upper = *args[2];
        if (lower.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
            upper.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT)
        {
            return unknown;
        }
        return IntersectRanges(ComparisonRange(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                                               lower.Cast<BoundConstantExpression>().GetValue()),
                               ComparisonRange(ExpressionType::COMPARE_LESSTHANOREQUALTO,
                                               upper.Cast<BoundConstantExpression>().GetValue()));
    }

    if (expression.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
        expression.GetExpressionType() == ExpressionType::COMPARE_IN)
    {
        const auto& children = expression.Cast<BoundOperatorExpression>().GetChildren();
        if (children.empty() || !RootIsColumnReference(*children[0]))
        {
            return unknown;
        }
        RootUnsignedFilterRange result;
        result.known = true;
        result.impossible = true;
        for (idx_t i = 1; i < children.size(); ++i)
        {
            if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT)
            {
                return unknown;
            }
            const auto& constant = children[i]->Cast<BoundConstantExpression>().GetValue();
            if (constant.IsNull())
            {
                continue;
            }
            const auto candidate = ComparisonRange(ExpressionType::COMPARE_EQUAL, constant);
            if (!candidate.known)
            {
                return unknown;
            }
            result = UnionRanges(result, candidate);
        }
        return result;
    }

    if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION)
    {
        const auto& conjunction = expression.Cast<BoundConjunctionExpression>();
        const auto type = conjunction.GetExpressionType();
        if (type != ExpressionType::CONJUNCTION_AND && type != ExpressionType::CONJUNCTION_OR)
        {
            return unknown;
        }
        RootUnsignedFilterRange combined;
        bool first = true;
        for (const auto& child : conjunction.GetChildren())
        {
            if (!child)
            {
                return unknown;
            }
            const auto candidate = ExtractUnsignedExpressionRange(*child);
            if (first)
            {
                combined = candidate;
                first = false;
            }
            else
            {
                combined = type == ExpressionType::CONJUNCTION_AND ? IntersectRanges(combined, candidate)
                                                                   : UnionRanges(combined, candidate);
            }
        }
        return first ? unknown : combined;
    }
    return unknown;
}

} // namespace

RootUnsignedFilterRange ExtractRootUnsignedRange(const TableFilter& filter)
{
    const auto* expression = RootFilterMetadataExpression(filter);
    // An unrecognized expression disables range pruning; it never rejects rows.
    return expression ? ExtractUnsignedExpressionRange(*expression) : RootUnsignedFilterRange{};
}

} // namespace duckdb::rootlake
