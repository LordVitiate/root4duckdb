#include "root4duckdb/index/root_filter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

#include <limits>
#include <mutex>

namespace duckdb::rootlake {

namespace {

static bool CompareValues(ExpressionType type, const Value& actual, const Value& expected) {
    if (actual.IsNull() || expected.IsNull()) {
        return false;
    }
    switch (type) {
    case ExpressionType::COMPARE_EQUAL:
        return ValueOperations::Equals(actual, expected);
    case ExpressionType::COMPARE_NOTEQUAL:
        return ValueOperations::NotEquals(actual, expected);
    case ExpressionType::COMPARE_LESSTHAN:
        return ValueOperations::LessThan(actual, expected);
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return ValueOperations::LessThanEquals(actual, expected);
    case ExpressionType::COMPARE_GREATERTHAN:
        return ValueOperations::GreaterThan(actual, expected);
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return ValueOperations::GreaterThanEquals(actual, expected);
    default:
        throw NotImplementedException("Unsupported ROOT pushed comparison");
    }
}

template <class T> static bool CompareNative(ExpressionType type, const T& actual, const T& expected) {
    switch (type) {
    case ExpressionType::COMPARE_EQUAL:
        return Equals::Operation<T>(actual, expected);
    case ExpressionType::COMPARE_NOTEQUAL:
        return NotEquals::Operation<T>(actual, expected);
    case ExpressionType::COMPARE_LESSTHAN:
        return LessThan::Operation<T>(actual, expected);
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return LessThanEquals::Operation<T>(actual, expected);
    case ExpressionType::COMPARE_GREATERTHAN:
        return GreaterThan::Operation<T>(actual, expected);
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return GreaterThanEquals::Operation<T>(actual, expected);
    default:
        throw NotImplementedException("Unsupported ROOT pushed comparison");
    }
}

template <class T>
static std::optional<bool> TryTypedComparison(ExpressionType type, const T& actual, const Value& expected) {
    if (expected.IsNull()) {
        return false;
    }
    try {
        return CompareNative(type, actual, expected.GetValue<T>());
    } catch (const Exception&) {
        return std::nullopt;
    }
}

static std::optional<bool> TryFastComparison(ExpressionType type, const RootScalarActual& actual,
                                             const Value& expected) {
    if (actual.is_null || expected.IsNull()) {
        return false;
    }
    switch (actual.type.id()) {
    case LogicalTypeId::BOOLEAN:
        return TryTypedComparison(type, actual.numeric != 0, expected);
    case LogicalTypeId::TINYINT:
        return TryTypedComparison(type, static_cast<int8_t>(actual.signed_value), expected);
    case LogicalTypeId::UTINYINT:
        return TryTypedComparison(type, static_cast<uint8_t>(actual.unsigned_value), expected);
    case LogicalTypeId::SMALLINT:
        return TryTypedComparison(type, static_cast<int16_t>(actual.signed_value), expected);
    case LogicalTypeId::USMALLINT:
        return TryTypedComparison(type, static_cast<uint16_t>(actual.unsigned_value), expected);
    case LogicalTypeId::INTEGER:
        return TryTypedComparison(type, static_cast<int32_t>(actual.signed_value), expected);
    case LogicalTypeId::UINTEGER:
        return TryTypedComparison(type, static_cast<uint32_t>(actual.unsigned_value), expected);
    case LogicalTypeId::BIGINT:
        return TryTypedComparison(type, actual.signed_value, expected);
    case LogicalTypeId::UBIGINT:
        return TryTypedComparison(type, actual.unsigned_value, expected);
    case LogicalTypeId::FLOAT:
        return TryTypedComparison(type, static_cast<float>(actual.numeric), expected);
    case LogicalTypeId::DOUBLE:
        return TryTypedComparison(type, actual.numeric, expected);
    default:
        return std::nullopt;
    }
}

} // namespace

class RootFilterEvaluator::Impl {
  public:
    bool Evaluate(ClientContext& context, const TableFilter& filter, const RootScalarActual& actual) {
        switch (filter.filter_type) {
        case TableFilterType::IS_NULL:
            return actual.is_null;
        case TableFilterType::IS_NOT_NULL:
            return !actual.is_null;
        case TableFilterType::CONSTANT_COMPARISON: {
            const auto& constant = filter.Cast<ConstantFilter>();
            auto fast = TryFastComparison(constant.comparison_type, actual, constant.constant);
            return fast.has_value() ? *fast
                                    : CompareValues(constant.comparison_type, actual.ToValue(), constant.constant);
        }
        case TableFilterType::IN_FILTER: {
            if (actual.is_null) {
                return false;
            }
            for (const auto& constant : filter.Cast<InFilter>().values) {
                auto fast = TryFastComparison(ExpressionType::COMPARE_EQUAL, actual, constant);
                if ((fast.has_value() && *fast) ||
                    (!fast.has_value() && !constant.IsNull() && ValueOperations::Equals(actual.ToValue(), constant))) {
                    return true;
                }
            }
            return false;
        }
        case TableFilterType::OPTIONAL_FILTER: {
            const auto& optional = filter.Cast<OptionalFilter>();
            return !optional.child_filter || Evaluate(context, *optional.child_filter, actual);
        }
        case TableFilterType::DYNAMIC_FILTER: {
            const auto& dynamic = filter.Cast<DynamicFilter>();
            if (!dynamic.filter_data) {
                return true;
            }
            lock_guard<mutex> guard(dynamic.filter_data->lock);
            return !dynamic.filter_data->initialized || !dynamic.filter_data->filter ||
                   Evaluate(context, *dynamic.filter_data->filter, actual);
        }
        case TableFilterType::EXPRESSION_FILTER: {
            const auto& expression = filter.Cast<ExpressionFilter>();
            auto executor = expression_executors.find(&expression);
            if (executor == expression_executors.end()) {
                executor =
                    expression_executors.emplace(&expression, make_uniq<ExpressionExecutor>(context, *expression.expr))
                        .first;
            }
            return expression.EvaluateWithConstant(*executor->second, actual.ToValue());
        }
        case TableFilterType::CONJUNCTION_AND:
            for (const auto& child : filter.Cast<ConjunctionAndFilter>().child_filters) {
                if (!Evaluate(context, *child, actual)) {
                    return false;
                }
            }
            return true;
        case TableFilterType::CONJUNCTION_OR:
            for (const auto& child : filter.Cast<ConjunctionOrFilter>().child_filters) {
                if (Evaluate(context, *child, actual)) {
                    return true;
                }
            }
            return false;
        default:
            throw NotImplementedException("Unsupported pushed ROOT table filter for a scalar column");
        }
    }

  private:
    std::unordered_map<const ExpressionFilter*, unique_ptr<ExpressionExecutor>> expression_executors;
};

RootFilterEvaluator::RootFilterEvaluator() : impl(std::make_unique<Impl>()) {
}
RootFilterEvaluator::~RootFilterEvaluator() = default;
RootFilterEvaluator::RootFilterEvaluator(RootFilterEvaluator&&) noexcept = default;
RootFilterEvaluator& RootFilterEvaluator::operator=(RootFilterEvaluator&&) noexcept = default;

bool RootFilterEvaluator::Evaluate(ClientContext& context, const TableFilter& filter, const RootScalarActual& actual) {
    return impl->Evaluate(context, filter, actual);
}

namespace {

static std::optional<uint64_t> FilterConstantAsUInt64(const Value& value) {
    if (value.IsNull()) {
        return std::nullopt;
    }
    try {
        return value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

RootUnsignedFilterRange ExtractRootUnsignedRange(const TableFilter& filter) {
    RootUnsignedFilterRange result;
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto& constant = filter.Cast<ConstantFilter>();
        const auto value = FilterConstantAsUInt64(constant.constant);
        if (!value) {
            return result;
        }
        result.known = true;
        switch (constant.comparison_type) {
        case ExpressionType::COMPARE_EQUAL:
            result.lower = result.upper = *value;
            break;
        case ExpressionType::COMPARE_LESSTHAN:
            if (*value == 0) {
                result.impossible = true;
            } else {
                result.upper = *value - 1;
            }
            break;
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            result.upper = *value;
            break;
        case ExpressionType::COMPARE_GREATERTHAN:
            if (*value == std::numeric_limits<uint64_t>::max()) {
                result.impossible = true;
            } else {
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
    case TableFilterType::IN_FILTER: {
        const auto& in_filter = filter.Cast<InFilter>();
        if (in_filter.values.empty()) {
            result.known = true;
            result.impossible = true;
            return result;
        }
        uint64_t lower = std::numeric_limits<uint64_t>::max();
        uint64_t upper = 0;
        for (const auto& constant : in_filter.values) {
            const auto value = FilterConstantAsUInt64(constant);
            if (!value) {
                return result;
            }
            lower = std::min(lower, *value);
            upper = std::max(upper, *value);
        }
        result.known = true;
        result.lower = lower;
        result.upper = upper;
        return result;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto& optional = filter.Cast<OptionalFilter>();
        return optional.child_filter ? ExtractRootUnsignedRange(*optional.child_filter) : result;
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto& dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) {
            return result;
        }
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) {
            return result;
        }
        return ExtractRootUnsignedRange(*dynamic.filter_data->filter);
    }
    case TableFilterType::CONJUNCTION_AND: {
        bool found = false;
        for (const auto& child : filter.Cast<ConjunctionAndFilter>().child_filters) {
            auto child_range = ExtractRootUnsignedRange(*child);
            if (!child_range.known) {
                continue;
            }
            found = true;
            if (child_range.impossible) {
                result.known = true;
                result.impossible = true;
                return result;
            }
            result.lower = std::max(result.lower, child_range.lower);
            result.upper = std::min(result.upper, child_range.upper);
        }
        result.known = found;
        result.impossible = found && result.lower > result.upper;
        return result;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto& children = filter.Cast<ConjunctionOrFilter>().child_filters;
        if (children.empty()) {
            return result;
        }
        uint64_t lower = std::numeric_limits<uint64_t>::max();
        uint64_t upper = 0;
        bool any_possible = false;
        for (const auto& child : children) {
            auto child_range = ExtractRootUnsignedRange(*child);
            if (!child_range.known) {
                return result;
            }
            if (child_range.impossible) {
                continue;
            }
            any_possible = true;
            lower = std::min(lower, child_range.lower);
            upper = std::max(upper, child_range.upper);
        }
        result.known = true;
        result.impossible = !any_possible;
        result.lower = any_possible ? lower : 0;
        result.upper = any_possible ? upper : 0;
        return result;
    }
    default:
        return result;
    }
}

} // namespace duckdb::rootlake
