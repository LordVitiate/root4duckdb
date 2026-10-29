#include "root4duckdb/dataset/root_dataset_pruning.hpp"

#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb::rootlake
{

static std::optional<double> FilterConstantAsPhysicalDouble(const Value& value, const LogicalType& physical_type)
{
    if (value.IsNull() || value.type() != physical_type)
    {
        // Metadata pruning may not cast away fractional or out-of-range constants.
        return std::nullopt;
    }
    try
    {
        const auto casted = value.DefaultCastAs(physical_type);
        double result = 0;
        switch (physical_type.id())
        {
        case LogicalTypeId::BOOLEAN:
            result = casted.GetValue<bool>() ? 1.0 : 0.0;
            break;
        case LogicalTypeId::TINYINT:
            result = static_cast<double>(casted.GetValue<int8_t>());
            break;
        case LogicalTypeId::UTINYINT:
            result = static_cast<double>(casted.GetValue<uint8_t>());
            break;
        case LogicalTypeId::SMALLINT:
            result = static_cast<double>(casted.GetValue<int16_t>());
            break;
        case LogicalTypeId::USMALLINT:
            result = static_cast<double>(casted.GetValue<uint16_t>());
            break;
        case LogicalTypeId::INTEGER:
            result = static_cast<double>(casted.GetValue<int32_t>());
            break;
        case LogicalTypeId::UINTEGER:
            result = static_cast<double>(casted.GetValue<uint32_t>());
            break;
        case LogicalTypeId::FLOAT:
            result = static_cast<double>(casted.GetValue<float>());
            break;
        case LogicalTypeId::DOUBLE:
            result = casted.GetValue<double>();
            break;
        default:
            return std::nullopt;
        }
        if (!std::isfinite(result))
        {
            return std::nullopt;
        }
        return result;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

static std::string NumberSQL(double value)
{
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return ss.str();
}

static std::optional<std::string> FilterConstantSQL(const Value& value, const LogicalType& physical_type)
{
    if (value.IsNull() || value.type() != physical_type)
    {
        return std::nullopt;
    }
    try
    {
        const auto casted = value.DefaultCastAs(physical_type);
        switch (physical_type.id())
        {
        case LogicalTypeId::BOOLEAN:
            return casted.GetValue<bool>() ? std::string("1") : std::string("0");
        case LogicalTypeId::TINYINT:
            return std::to_string(static_cast<int64_t>(casted.GetValue<int8_t>()));
        case LogicalTypeId::SMALLINT:
            return std::to_string(static_cast<int64_t>(casted.GetValue<int16_t>()));
        case LogicalTypeId::INTEGER:
            return std::to_string(static_cast<int64_t>(casted.GetValue<int32_t>()));
        case LogicalTypeId::BIGINT:
            return std::to_string(casted.GetValue<int64_t>());
        case LogicalTypeId::UTINYINT:
            return std::to_string(static_cast<uint64_t>(casted.GetValue<uint8_t>()));
        case LogicalTypeId::USMALLINT:
            return std::to_string(static_cast<uint64_t>(casted.GetValue<uint16_t>()));
        case LogicalTypeId::UINTEGER:
            return std::to_string(static_cast<uint64_t>(casted.GetValue<uint32_t>()));
        case LogicalTypeId::UBIGINT:
            return std::to_string(casted.GetValue<uint64_t>());
        case LogicalTypeId::FLOAT:
        {
            const auto number = static_cast<double>(casted.GetValue<float>());
            if (!std::isfinite(number))
            {
                return std::nullopt;
            }
            return NumberSQL(number);
        }
        case LogicalTypeId::DOUBLE:
        {
            const auto number = casted.GetValue<double>();
            if (!std::isfinite(number))
            {
                return std::nullopt;
            }
            return NumberSQL(number);
        }
        default:
            return std::nullopt;
        }
    }
    catch (...)
    {
        return std::nullopt;
    }
}

static std::optional<Value> ConstantExpressionValue(const Expression& expression)
{
    if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT)
    {
        return expression.Cast<BoundConstantExpression>().GetValue();
    }
    if (expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
        expression.GetExpressionType() == ExpressionType::OPERATOR_CAST)
    {
        const auto& cast = expression.Cast<BoundFunctionExpression>();
        if (cast.GetChildren().size() != 1)
        {
            return std::nullopt;
        }
        const auto value = ConstantExpressionValue(*cast.GetChildren()[0]);
        if (!value)
        {
            return std::nullopt;
        }
        try
        {
            return value->DefaultCastAs(expression.GetReturnType());
        }
        catch (const Exception&)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static bool ColumnComparison(const Expression& expression, ExpressionType& type, Value& constant)
{
    if (expression.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
        !RootIsComparison(expression.GetExpressionType()))
    {
        return false;
    }
    const auto& args = expression.Cast<BoundFunctionExpression>().GetChildren();
    if (args.size() != 2)
    {
        return false;
    }
    type = expression.GetExpressionType();
    if (RootIsColumnReference(*args[0]))
    {
        const auto value = ConstantExpressionValue(*args[1]);
        if (!value || value->IsNull())
        {
            return false;
        }
        constant = *value;
        return true;
    }
    if (RootIsColumnReference(*args[1]))
    {
        const auto value = ConstantExpressionValue(*args[0]);
        if (!value || value->IsNull())
        {
            return false;
        }
        type = RootReverseComparison(type);
        constant = *value;
        return true;
    }
    return false;
}

static bool InConstants(const Expression& expression, std::vector<Value>& values)
{
    if (expression.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
        expression.GetExpressionType() != ExpressionType::COMPARE_IN)
    {
        return false;
    }
    const auto& children = expression.Cast<BoundOperatorExpression>().GetChildren();
    if (children.size() < 2 || !RootIsColumnReference(*children[0]))
    {
        return false;
    }
    for (idx_t i = 1; i < children.size(); ++i)
    {
        auto constant = ConstantExpressionValue(*children[i]);
        if (!constant)
        {
            return false;
        }
        if (!constant->IsNull())
        {
            values.push_back(std::move(*constant));
        }
    }
    return true;
}

static std::string ComparisonZonemapClause(ExpressionType type, const Value& value, const std::string& min_expr,
                                           const std::string& max_expr, const LogicalType& physical_type)
{
    auto literal = FilterConstantSQL(value, physical_type);
    if (!literal)
    {
        return {};
    }
    switch (type)
    {
    case ExpressionType::COMPARE_EQUAL:
        return min_expr + " <= " + *literal + " AND " + max_expr + " >= " + *literal;
    case ExpressionType::COMPARE_LESSTHAN:
        return min_expr + " < " + *literal;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return min_expr + " <= " + *literal;
    case ExpressionType::COMPARE_GREATERTHAN:
        return max_expr + " > " + *literal;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return max_expr + " >= " + *literal;
    default:
        return {};
    }
}

static std::string JoinClauses(const std::vector<std::string>& clauses, const std::string& op)
{
    std::string result;
    for (const auto& clause : clauses)
    {
        if (!result.empty())
        {
            result += op;
        }
        result += "(" + clause + ")";
    }
    return result;
}

static std::string ZonemapExpressionClause(const Expression& expr, const std::string& min_expr,
                                           const std::string& max_expr, const LogicalType& physical_type)
{
    ExpressionType type;
    Value constant;
    if (ColumnComparison(expr, type, constant))
    {
        return ComparisonZonemapClause(type, constant, min_expr, max_expr, physical_type);
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
        expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN)
    {
        const auto& between = expr.Cast<BoundFunctionExpression>();
        const auto& args = between.GetChildren();
        if (args.size() != 3 || !RootIsColumnReference(*args[0]))
        {
            return {};
        }
        const auto lower = ConstantExpressionValue(*args[1]);
        const auto upper = ConstantExpressionValue(*args[2]);
        if (!lower || !upper)
        {
            return {};
        }
        const auto first = ComparisonZonemapClause(ExpressionType::COMPARE_GREATERTHANOREQUALTO, *lower, min_expr,
                                                   max_expr, physical_type);
        const auto second = ComparisonZonemapClause(ExpressionType::COMPARE_LESSTHANOREQUALTO, *upper, min_expr,
                                                    max_expr, physical_type);
        return first.empty() || second.empty() ? std::string() : "(" + first + ") AND (" + second + ")";
    }
    std::vector<Value> values;
    if (InConstants(expr, values))
    {
        std::vector<std::string> clauses;
        for (const auto& value : values)
        {
            auto clause =
                ComparisonZonemapClause(ExpressionType::COMPARE_EQUAL, value, min_expr, max_expr, physical_type);
            if (clause.empty())
            {
                return {};
            }
            clauses.push_back(std::move(clause));
        }
        return clauses.empty() ? "FALSE" : JoinClauses(clauses, " OR ");
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION)
    {
        const auto& conjunction = expr.Cast<BoundConjunctionExpression>();
        const auto is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
        const auto is_or = expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR;
        if (!is_and && !is_or)
        {
            return {};
        }
        std::vector<std::string> clauses;
        for (const auto& child : conjunction.GetChildren())
        {
            const auto clause = ZonemapExpressionClause(*child, min_expr, max_expr, physical_type);
            if (clause.empty())
            {
                if (is_or)
                {
                    return {};
                }
                continue;
            }
            clauses.push_back(clause);
        }
        return JoinClauses(clauses, is_and ? " AND " : " OR ");
    }
    return {};
}

std::string ZonemapClause(const TableFilter& filter, const std::string& min_expr, const std::string& max_expr,
                          const LogicalType& physical_type)
{
    const auto* expression = RootFilterMetadataExpression(filter);
    return expression ? ZonemapExpressionClause(*expression, min_expr, max_expr, physical_type) : std::string{};
}

static bool BloomMayContain(const string& bytes, double value)
{
    return RootBloomFilter::MayContain(bytes, value);
}

static bool ExtractEqualityConstants(const Expression& expr, std::vector<Value>& values)
{
    ExpressionType type;
    Value constant;
    if (ColumnComparison(expr, type, constant) && type == ExpressionType::COMPARE_EQUAL)
    {
        values.push_back(std::move(constant));
        return true;
    }
    if (InConstants(expr, values))
    {
        return true;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
        expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR)
    {
        const auto& conjunction = expr.Cast<BoundConjunctionExpression>();
        if (conjunction.GetChildren().empty())
        {
            return false;
        }
        for (const auto& child : conjunction.GetChildren())
        {
            if (!ExtractEqualityConstants(*child, values))
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool BloomMayContainFilter(const TableFilter& filter, const string& bytes, const LogicalType& physical_type)
{
    if (bytes.empty())
    {
        return true;
    }
    const auto* expression = RootFilterMetadataExpression(filter);
    if (!expression)
    {
        return true;
    }
    std::vector<Value> constants;
    if (!ExtractEqualityConstants(*expression, constants))
    {
        return true;
    }
    for (const auto& constant : constants)
    {
        const auto value = FilterConstantAsPhysicalDouble(constant, physical_type);
        if (!value || BloomMayContain(bytes, *value))
        {
            return true;
        }
    }
    return false;
}

bool FilterNeedsBloom(const TableFilter& filter)
{
    const auto* expression = RootFilterMetadataExpression(filter);
    if (!expression)
    {
        return false;
    }
    std::vector<Value> constants;
    return ExtractEqualityConstants(*expression, constants) && !constants.empty();
}

void MergeEventRangeIntoGlobal(DatasetGlobalState& global, const RootUnsignedFilterRange& range)
{
    if (!range.known)
    {
        return;
    }
    if (range.impossible)
    {
        global.has_event_range = true;
        global.event_range_impossible = true;
        return;
    }
    if (!global.has_event_range)
    {
        global.has_event_range = true;
        global.event_lower = range.lower;
        global.event_upper = range.upper;
        return;
    }
    global.event_lower = std::max(global.event_lower, range.lower);
    global.event_upper = std::min(global.event_upper, range.upper);
    global.event_range_impossible = global.event_lower > global.event_upper;
}

static std::string ExactStringExpressionClause(const Expression& expr, const std::string& column_sql)
{
    ExpressionType type;
    Value constant;
    if (ColumnComparison(expr, type, constant) && type == ExpressionType::COMPARE_EQUAL)
    {
        return column_sql + " = " + SqlLiteral(constant.ToString());
    }
    std::vector<Value> values;
    if (InConstants(expr, values))
    {
        if (values.empty())
        {
            return "FALSE";
        }
        std::string result = column_sql + " IN (";
        for (idx_t i = 0; i < values.size(); ++i)
        {
            if (i)
            {
                result += ", ";
            }
            result += SqlLiteral(values[i].ToString());
        }
        return result + ")";
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION)
    {
        const auto& conjunction = expr.Cast<BoundConjunctionExpression>();
        const auto is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
        const auto is_or = expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR;
        if (!is_and && !is_or)
        {
            return {};
        }
        std::vector<std::string> clauses;
        for (const auto& child : conjunction.GetChildren())
        {
            auto clause = ExactStringExpressionClause(*child, column_sql);
            if (clause.empty())
            {
                if (is_or)
                {
                    return {};
                }
                continue;
            }
            clauses.push_back(std::move(clause));
        }
        return JoinClauses(clauses, is_and ? " AND " : " OR ");
    }
    return {};
}

std::string ExactStringFilterClause(const TableFilter& filter, const std::string& column_sql)
{
    const auto* expression = RootFilterMetadataExpression(filter);
    return expression ? ExactStringExpressionClause(*expression, column_sql) : std::string{};
}

bool RejectsAllMaterializedRows(const TableFilter&)
{
    // A filter's column is not available here; nullability varies by ROOT column.
    // Do not reject rows on an IS NULL expression without proving the column non-nullable.
    return false;
}

std::string IdListSQL(const std::vector<SchemaBinding>& schemas, bool column_ids)
{
    std::string out = "(";
    for (idx_t i = 0; i < schemas.size(); ++i)
    {
        if (i)
        {
            out += ',';
        }
        out += SqlLiteral(column_ids ? schemas[i].column_id : schemas[i].schema_id);
    }
    out += ')';
    return out;
}

bool PredicateMetadataMayMatch(const PathPredicateBinding& predicate, const Value& min_value, const Value& max_value,
                               uint64_t nan_count, uint64_t pos_inf_count, uint64_t neg_inf_count,
                               const Value& bloom_value)
{

    // Current index-format min/max/Bloom transport is DOUBLE.
    // Never prune 64-bit integer predicates through it.
    if (!UsesDoubleBackedValueMetadata(predicate.value_type))
    {
        return true;
    }
    const bool has_finite = !min_value.IsNull() && !max_value.IsNull();
    const double min_number = has_finite ? min_value.GetValue<double>() : 0.0;
    const double max_number = has_finite ? max_value.GetValue<double>() : 0.0;
    const string* bloom = nullptr;
    string bloom_storage;
    if (!bloom_value.IsNull())
    {
        bloom_storage = StringValue::Get(bloom_value);
        bloom = &bloom_storage;
    }
    auto constant = [&](idx_t index)
    {
        return predicate.values[index].AsDouble();
    };
    auto equality_possible = [&](double value)
    {
        if (!std::isfinite(value) || !has_finite || value < min_number || value > max_number)
        {
            return false;
        }
        return !bloom || BloomMayContain(*bloom, value);
    };
    switch (predicate.op)
    {
    case PathPredicateOp::EQ:
        return equality_possible(constant(0));
    case PathPredicateOp::NE:
        if (nan_count || pos_inf_count || neg_inf_count)
        {
            return true;
        }
        return !has_finite || min_number != constant(0) || max_number != constant(0);
    case PathPredicateOp::LT:
        return neg_inf_count || (has_finite && min_number < constant(0));
    case PathPredicateOp::LE:
        return neg_inf_count || (has_finite && min_number <= constant(0));
    case PathPredicateOp::GT:
        return pos_inf_count || (has_finite && max_number > constant(0));
    case PathPredicateOp::GE:
        return pos_inf_count || (has_finite && max_number >= constant(0));
    case PathPredicateOp::BETWEEN:
        return has_finite && max_number >= constant(0) && min_number <= constant(1);
    case PathPredicateOp::IN:
        for (const auto& value : predicate.values)
        {
            if (equality_possible(value.AsDouble()))
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

} // namespace duckdb::rootlake
