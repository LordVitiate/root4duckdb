#include "root4duckdb/direct/root_scan_internal.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <charconv>
#include <type_traits>

namespace duckdb
{
namespace
{

template <class T> column_t RootPruningPrimaryColumnId(const T& value)
{
    if constexpr (std::is_integral_v<std::decay_t<T>>)
    {
        return static_cast<column_t>(value);
    }
    else
    {
        return value.GetPrimaryIndex();
    }
}

const Expression* RootPruningStripConstantCasts(const Expression* expression)
{
    while (expression && expression->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
           expression->GetExpressionType() == ExpressionType::OPERATOR_CAST)
    {
        const auto& args = expression->Cast<BoundFunctionExpression>().GetChildren();
        if (args.size() != 1 || args[0]->GetReturnType() != expression->GetReturnType())
        {
            // Casting can change the comparison; omit the pruning hint rather than guess.
            return nullptr;
        }
        expression = args[0].get();
    }
    return expression;
}

bool RootPruningIsEntryId(const LogicalGet& get, const Expression* expression)
{
    // Strip constant casts only; casts on entry_id may change comparison semantics.
    if (!expression || expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF)
    {
        return false;
    }
    const auto& ref = expression->Cast<BoundColumnRefExpression>();
    if (ref.Binding().table_index != get.table_index)
    {
        return false;
    }
    const idx_t projected_column = ref.Binding().column_index;
    const auto& column_ids = get.GetColumnIds();
    if (projected_column >= column_ids.size())
    {
        return false;
    }
    // read_root's public entry_id is physical output column zero.
    return RootPruningPrimaryColumnId(column_ids[projected_column]) == 0;
}

struct RootPruningInteger
{
    bool negative = false;
    uint64_t magnitude = 0;
};

bool RootPruningParseInteger(const Expression* expression, RootPruningInteger& value)
{
    expression = RootPruningStripConstantCasts(expression);
    if (!expression || expression->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT)
    {
        return false;
    }
    const auto& constant = expression->Cast<BoundConstantExpression>();
    if (constant.GetValue().IsNull())
    {
        return false;
    }
    const std::string text = constant.GetValue().ToString();
    if (text.empty())
    {
        return false;
    }
    size_t offset = 0;
    if (text[0] == '+' || text[0] == '-')
    {
        value.negative = text[0] == '-';
        offset = 1;
    }
    if (offset == text.size())
    {
        return false;
    }
    uint64_t parsed = 0;
    const char* begin = text.data() + offset;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end)
    {
        return false;
    }
    value.magnitude = parsed;
    return true;
}

struct RootPruningInterval
{
    bool known = false;
    bool impossible = false;
    uint64_t lower = 0;
    uint64_t upper = std::numeric_limits<uint64_t>::max();
};

RootPruningInterval RootPruningUnknown()
{
    return {};
}

RootPruningInterval RootPruningImpossible()
{
    RootPruningInterval result;
    result.known = true;
    result.impossible = true;
    return result;
}

RootPruningInterval RootPruningAll()
{
    RootPruningInterval result;
    result.known = true;
    return result;
}

RootPruningInterval RootPruningFromComparison(ExpressionType type, const RootPruningInteger& constant)
{
    RootPruningInterval result = RootPruningAll();
    if (constant.negative)
    {
        switch (type)
        {
        case ExpressionType::COMPARE_EQUAL:
        case ExpressionType::COMPARE_LESSTHAN:
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            return RootPruningImpossible();
        case ExpressionType::COMPARE_GREATERTHAN:
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            return result;
        default:
            return RootPruningUnknown();
        }
    }
    const uint64_t value = constant.magnitude;
    switch (type)
    {
    case ExpressionType::COMPARE_EQUAL:
        result.lower = value;
        result.upper = value;
        break;
    case ExpressionType::COMPARE_GREATERTHAN:
        if (value == std::numeric_limits<uint64_t>::max())
        {
            return RootPruningImpossible();
        }
        result.lower = value + 1;
        break;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        result.lower = value;
        break;
    case ExpressionType::COMPARE_LESSTHAN:
        if (value == 0)
        {
            return RootPruningImpossible();
        }
        result.upper = value - 1;
        break;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        result.upper = value;
        break;
    default:
        return RootPruningUnknown();
    }
    return result;
}

RootPruningInterval RootPruningIntersect(const RootPruningInterval& left, const RootPruningInterval& right)
{
    if (!left.known)
    {
        return right;
    }
    if (!right.known)
    {
        return left;
    }
    if (left.impossible || right.impossible)
    {
        return RootPruningImpossible();
    }
    RootPruningInterval result = RootPruningAll();
    result.lower = std::max(left.lower, right.lower);
    result.upper = std::min(left.upper, right.upper);
    if (result.lower > result.upper)
    {
        return RootPruningImpossible();
    }
    return result;
}

RootPruningInterval RootPruningUnionHull(const RootPruningInterval& left, const RootPruningInterval& right)
{
    // OR pruning is safe only when every disjunct is understood. The physical
    // scan can use the hull; DuckDB keeps the exact OR predicate above it.
    if (!left.known || !right.known)
    {
        return RootPruningUnknown();
    }
    if (left.impossible)
    {
        return right;
    }
    if (right.impossible)
    {
        return left;
    }
    RootPruningInterval result = RootPruningAll();
    result.lower = std::min(left.lower, right.lower);
    result.upper = std::max(left.upper, right.upper);
    return result;
}

RootPruningInterval RootPruningExtract(const LogicalGet& get, const Expression& expression);

RootPruningInterval RootPruningComparison(const LogicalGet& get, const BoundFunctionExpression& comparison)
{
    const auto& args = comparison.GetChildren();
    if (args.size() != 2)
    {
        return RootPruningUnknown();
    }
    const Expression* column = args[0].get();
    const Expression* constant = args[1].get();
    auto type = comparison.GetExpressionType();
    RootPruningInteger parsed;
    if (!RootPruningIsEntryId(get, column) || !RootPruningParseInteger(constant, parsed))
    {
        column = args[1].get();
        constant = args[0].get();
        type = rootlake::RootReverseComparison(type);
        if (!RootPruningIsEntryId(get, column) || !RootPruningParseInteger(constant, parsed))
        {
            return RootPruningUnknown();
        }
    }
    return RootPruningFromComparison(type, parsed);
}

RootPruningInterval RootPruningBetween(const LogicalGet& get, const BoundFunctionExpression& between)
{
    const auto& args = between.GetChildren();
    if (args.size() != 3 || !RootPruningIsEntryId(get, args[0].get()))
    {
        return RootPruningUnknown();
    }
    RootPruningInteger lower;
    RootPruningInteger upper;
    if (!RootPruningParseInteger(args[1].get(), lower) || !RootPruningParseInteger(args[2].get(), upper))
    {
        return RootPruningUnknown();
    }
    // Inclusive bounds are safe for both inclusive and exclusive BETWEEN variants.
    const auto first = RootPruningFromComparison(ExpressionType::COMPARE_GREATERTHANOREQUALTO, lower);
    const auto last = RootPruningFromComparison(ExpressionType::COMPARE_LESSTHANOREQUALTO, upper);
    return RootPruningIntersect(first, last);
}

RootPruningInterval RootPruningExtract(const LogicalGet& get, const Expression& expression)
{
    if (expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION)
    {
        const auto& function = expression.Cast<BoundFunctionExpression>();
        if (rootlake::RootIsComparison(expression.GetExpressionType()))
        {
            return RootPruningComparison(get, function);
        }
        if (expression.GetExpressionType() == ExpressionType::COMPARE_BETWEEN)
        {
            return RootPruningBetween(get, function);
        }
    }
    if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION)
    {
        const auto& conjunction = expression.Cast<BoundConjunctionExpression>();
        const auto type = conjunction.GetExpressionType();
        if (type != ExpressionType::CONJUNCTION_AND && type != ExpressionType::CONJUNCTION_OR)
        {
            return RootPruningUnknown();
        }
        RootPruningInterval result;
        bool first = true;
        for (const auto& child : conjunction.GetChildren())
        {
            if (!child)
            {
                continue;
            }
            const auto child_interval = RootPruningExtract(get, *child);
            if (first)
            {
                result = child_interval;
                first = false;
                continue;
            }
            result = type == ExpressionType::CONJUNCTION_AND ? RootPruningIntersect(result, child_interval)
                                                             : RootPruningUnionHull(result, child_interval);
        }
        return first ? RootPruningUnknown() : result;
    }
    return RootPruningUnknown();
}

} // namespace

void RootScanCollectPruningHints(ClientContext&, LogicalGet& get, FunctionData* bind_data,
                                 vector<unique_ptr<Expression>>& filters)
{
    if (!bind_data)
    {
        return;
    }
    auto& bind = bind_data->Cast<RootScanBindData>();
    RootPruningInterval combined;
    bool have_hint = false;
    // Retain DuckDB predicates; this callback only narrows physical entry bounds.
    for (const auto& filter : filters)
    {
        if (!filter)
        {
            continue;
        }
        const auto interval = RootPruningExtract(get, *filter);
        if (!interval.known)
        {
            continue;
        }
        combined = have_hint ? RootPruningIntersect(combined, interval) : interval;
        have_hint = true;
    }
    if (!have_hint)
    {
        return;
    }
    bind.entry_prune_active = true;
    bind.entry_prune_impossible = bind.entry_prune_impossible || combined.impossible;
    if (!combined.impossible)
    {
        bind.entry_prune_lower = std::max(bind.entry_prune_lower, combined.lower);
        bind.entry_prune_upper = std::min(bind.entry_prune_upper, combined.upper);
        if (bind.entry_prune_lower > bind.entry_prune_upper)
        {
            bind.entry_prune_impossible = true;
        }
    }
}

} // namespace duckdb
