#include "root4duckdb/dataset/root_dataset_pruning.hpp"

namespace duckdb::rootlake {

static std::optional<double> FilterConstantAsPhysicalDouble(const Value& value, const LogicalType& physical_type) {
    if (value.IsNull()) {
        return std::nullopt;
    }
    try {
        const auto casted = value.DefaultCastAs(physical_type);
        double result = 0;
        switch (physical_type.id()) {
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
        if (!std::isfinite(result)) {
            return std::nullopt;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

static std::string NumberSQL(double value) {
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return ss.str();
}

static std::optional<std::string> FilterConstantSQL(const Value& value, const LogicalType& physical_type) {
    if (value.IsNull()) {
        return std::nullopt;
    }
    try {
        const auto casted = value.DefaultCastAs(physical_type);
        switch (physical_type.id()) {
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
        case LogicalTypeId::FLOAT: {
            const auto number = static_cast<double>(casted.GetValue<float>());
            if (!std::isfinite(number)) {
                return std::nullopt;
            }
            return NumberSQL(number);
        }
        case LogicalTypeId::DOUBLE: {
            const auto number = casted.GetValue<double>();
            if (!std::isfinite(number)) {
                return std::nullopt;
            }
            return NumberSQL(number);
        }
        default:
            return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
}

std::string ZonemapClause(const TableFilter& filter, const std::string& min_expr, const std::string& max_expr,
                          const LogicalType& physical_type) {
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto& constant = filter.Cast<ConstantFilter>();
        auto literal = FilterConstantSQL(constant.constant, physical_type);
        if (!literal) {
            return {};
        }
        switch (constant.comparison_type) {
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
    case TableFilterType::IN_FILTER: {
        const auto& in_filter = filter.Cast<InFilter>();
        std::vector<std::string> clauses;
        for (const auto& constant : in_filter.values) {
            auto literal = FilterConstantSQL(constant, physical_type);
            if (!literal) {
                return {};
            }
            clauses.push_back("(" + min_expr + " <= " + *literal + " AND " + max_expr + " >= " + *literal + ")");
        }
        std::string out;
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) {
                out += " OR ";
            }
            out += clauses[i];
        }
        return out;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto& optional = filter.Cast<OptionalFilter>();
        return optional.child_filter ? ZonemapClause(*optional.child_filter, min_expr, max_expr, physical_type)
                                     : std::string();
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto& dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) {
            return {};
        }
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) {
            return {};
        }
        return ZonemapClause(*dynamic.filter_data->filter, min_expr, max_expr, physical_type);
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto& conjunction = filter.Cast<ConjunctionAndFilter>();
        std::vector<std::string> clauses;
        for (const auto& child : conjunction.child_filters) {
            auto clause = ZonemapClause(*child, min_expr, max_expr, physical_type);
            if (!clause.empty()) {
                clauses.push_back("(" + clause + ")");
            }
        }
        std::string out;
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) {
                out += " AND ";
            }
            out += clauses[i];
        }
        return out;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto& conjunction = filter.Cast<ConjunctionOrFilter>();
        std::vector<std::string> clauses;
        for (const auto& child : conjunction.child_filters) {
            auto clause = ZonemapClause(*child, min_expr, max_expr, physical_type);
            if (clause.empty()) {
                return {};
            }
            clauses.push_back("(" + clause + ")");
        }
        std::string out;
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) {
                out += " OR ";
            }
            out += clauses[i];
        }
        return out;
    }
    default:
        return {};
    }
}

optional_ptr<TableFilter> FilterForFullColumn(const DatasetGlobalState& global, column_t full_column) {
    if (!global.filters) {
        return nullptr;
    }
    for (const auto& entry : global.filters->filters) {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size()) {
            continue;
        }
        if (global.scan_column_ids[scan_position] == full_column) {
            return entry.second.get();
        }
    }
    return nullptr;
}

bool PassesFilters(ClientContext& context, DatasetLocalState& local, const DatasetBindData& bind,
                   const DatasetGlobalState& global, uint64_t event_fk, double numeric_value, const int32_t* indices,
                   idx_t index_count) {
    if (!global.filters) {
        return true;
    }
    for (const auto& entry : global.filters->filters) {
        const auto scan_position = entry.first;
        if (scan_position >= global.scan_column_ids.size()) {
            continue;
        }
        const column_t column = global.scan_column_ids[scan_position];
        RootScalarActual actual;
        if (column == 0) {
            actual = RootScalarActual::Event(event_fk);
        } else if (column >= 1 && static_cast<idx_t>(column) < bind.value_column) {
            const idx_t index_position = static_cast<idx_t>(column) - 1;
            actual = RootScalarActual::Index(
                index_position < index_count ? std::optional<int32_t>(indices[index_position]) : std::nullopt);
        } else if (static_cast<idx_t>(column) == bind.value_column) {
            actual = RootScalarActual::Numeric(bind.value_type, numeric_value);
        } else if (static_cast<idx_t>(column) == bind.source_id_column) {
            actual = RootScalarActual::String(local.value_source_id);
        } else if (static_cast<idx_t>(column) == bind.entry_id_column) {
            actual = RootScalarActual::Event(local.value_entry_id);
        } else {
            actual = RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!local.filter_evaluator.Evaluate(context, *entry.second, actual)) {
            return false;
        }
    }
    return true;
}

static RootScalarActual DatasetPrimitiveActual(const LogicalType& type, const RootPrimitiveValue& value) {
    switch (value.kind) {
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
                        const int32_t* indices, idx_t index_count) {

    if (!global.filters) {
        return true;
    }

    for (const auto& entry : global.filters->filters) {
        const auto scan_position = entry.first;

        if (scan_position >= global.scan_column_ids.size()) {
            continue;
        }

        const column_t column = global.scan_column_ids[scan_position];

        RootScalarActual actual;

        if (column == 0) {
            actual = RootScalarActual::Event(event_fk);

        } else if (column >= 1 && static_cast<idx_t>(column) < bind.value_column) {

            const idx_t index_position = static_cast<idx_t>(column) - 1;

            actual = RootScalarActual::Index(
                index_position < index_count ? std::optional<int32_t>(indices[index_position]) : std::nullopt);

        } else if (static_cast<idx_t>(column) == bind.value_column) {

            actual = DatasetPrimitiveActual(bind.value_type, numeric_value);

        } else if (static_cast<idx_t>(column) == bind.source_id_column) {

            actual = RootScalarActual::String(local.value_source_id);

        } else if (static_cast<idx_t>(column) == bind.entry_id_column) {

            actual = RootScalarActual::Event(local.value_entry_id);

        } else {
            actual = RootScalarActual::Null(LogicalType::SQLNULL);
        }

        if (!local.filter_evaluator.Evaluate(context, *entry.second, actual)) {
            return false;
        }
    }

    return true;
}

static bool BloomMayContain(const string& bytes, double value) {
    return RootBloomFilter::MayContain(bytes, value);
}

static const Expression* StripExpressionCasts(const Expression* expression) {
    while (expression && expression->expression_class == ExpressionClass::BOUND_CAST) {
        expression = expression->Cast<BoundCastExpression>().child.get();
    }
    return expression;
}

static std::optional<Value> ConstantExpressionValue(const Expression& expression) {
    if (expression.expression_class == ExpressionClass::BOUND_CONSTANT) {
        return expression.Cast<BoundConstantExpression>().value;
    }
    if (expression.expression_class == ExpressionClass::BOUND_CAST) {
        const auto& cast = expression.Cast<BoundCastExpression>();
        auto child = ConstantExpressionValue(*cast.child);
        if (!child) {
            return std::nullopt;
        }
        try {
            return child->DefaultCastAs(expression.return_type);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static bool ExtractEqualityConstants(const Expression& expression, std::vector<Value>& values) {
    if (expression.expression_class == ExpressionClass::BOUND_CONJUNCTION &&
        expression.type == ExpressionType::CONJUNCTION_OR) {
        const auto& conjunction = expression.Cast<BoundConjunctionExpression>();
        if (conjunction.children.empty()) {
            return false;
        }
        for (const auto& child : conjunction.children) {
            if (!ExtractEqualityConstants(*child, values)) {
                return false;
            }
        }
        return true;
    }
    if (expression.expression_class == ExpressionClass::BOUND_COMPARISON &&
        expression.type == ExpressionType::COMPARE_EQUAL) {
        const auto& comparison = expression.Cast<BoundComparisonExpression>();
        auto left = ConstantExpressionValue(*comparison.left);
        auto right = ConstantExpressionValue(*comparison.right);
        if (left && !right) {
            values.push_back(*left);
            return true;
        }
        if (right && !left) {
            values.push_back(*right);
            return true;
        }
        return false;
    }
    if (expression.expression_class == ExpressionClass::BOUND_OPERATOR &&
        expression.type == ExpressionType::COMPARE_IN) {
        const auto& op = expression.Cast<BoundOperatorExpression>();
        if (op.children.size() < 2) {
            return false;
        }
        for (idx_t i = 1; i < op.children.size(); ++i) {
            auto value = ConstantExpressionValue(*op.children[i]);
            if (!value) {
                return false;
            }
            values.push_back(*value);
        }
        return !values.empty();
    }
    return false;
}

bool BloomMayContainFilter(const TableFilter& filter, const string& bytes, const LogicalType& physical_type) {
    if (bytes.empty()) {
        return true;
    }
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto& constant = filter.Cast<ConstantFilter>();
        if (constant.comparison_type != ExpressionType::COMPARE_EQUAL) {
            return true;
        }
        auto value = FilterConstantAsPhysicalDouble(constant.constant, physical_type);
        return !value || BloomMayContain(bytes, *value);
    }
    case TableFilterType::IN_FILTER: {
        const auto& in_filter = filter.Cast<InFilter>();
        for (const auto& constant : in_filter.values) {
            auto value = FilterConstantAsPhysicalDouble(constant, physical_type);
            if (!value) {
                return true;
            }
            if (BloomMayContain(bytes, *value)) {
                return true;
            }
        }
        return false;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto& optional = filter.Cast<OptionalFilter>();
        return !optional.child_filter || BloomMayContainFilter(*optional.child_filter, bytes, physical_type);
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto& dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) {
            return true;
        }
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) {
            return true;
        }
        return BloomMayContainFilter(*dynamic.filter_data->filter, bytes, physical_type);
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto& conjunction = filter.Cast<ConjunctionAndFilter>();
        for (const auto& child : conjunction.child_filters) {
            if (!BloomMayContainFilter(*child, bytes, physical_type)) {
                return false;
            }
        }
        return true;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto& conjunction = filter.Cast<ConjunctionOrFilter>();
        for (const auto& child : conjunction.child_filters) {
            if (BloomMayContainFilter(*child, bytes, physical_type)) {
                return true;
            }
        }
        return false;
    }
    case TableFilterType::EXPRESSION_FILTER: {
        const auto& expression = filter.Cast<ExpressionFilter>();
        std::vector<Value> constants;
        if (!expression.expr || !ExtractEqualityConstants(*expression.expr, constants)) {
            return true;
        }
        for (const auto& constant : constants) {
            auto value = FilterConstantAsPhysicalDouble(constant, physical_type);
            if (!value || BloomMayContain(bytes, *value)) {
                return true;
            }
        }
        return false;
    }
    default:
        return true;
    }
}

bool FilterNeedsBloom(const TableFilter& filter) {
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON:
        return filter.Cast<ConstantFilter>().comparison_type == ExpressionType::COMPARE_EQUAL;
    case TableFilterType::IN_FILTER:
        return true;
    case TableFilterType::OPTIONAL_FILTER: {
        const auto& optional = filter.Cast<OptionalFilter>();
        return optional.child_filter && FilterNeedsBloom(*optional.child_filter);
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto& dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) {
            return false;
        }
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        return dynamic.filter_data->initialized && dynamic.filter_data->filter &&
               FilterNeedsBloom(*dynamic.filter_data->filter);
    }
    case TableFilterType::CONJUNCTION_AND: {
        for (const auto& child : filter.Cast<ConjunctionAndFilter>().child_filters) {
            if (FilterNeedsBloom(*child)) {
                return true;
            }
        }
        return false;
    }
    case TableFilterType::CONJUNCTION_OR: {
        for (const auto& child : filter.Cast<ConjunctionOrFilter>().child_filters) {
            if (FilterNeedsBloom(*child)) {
                return true;
            }
        }
        return false;
    }
    case TableFilterType::EXPRESSION_FILTER: {
        const auto& expression = filter.Cast<ExpressionFilter>();
        std::vector<Value> constants;
        return expression.expr && ExtractEqualityConstants(*expression.expr, constants);
    }
    default:
        return false;
    }
}

void MergeEventRangeIntoGlobal(DatasetGlobalState& global, const RootUnsignedFilterRange& range) {
    if (!range.known) {
        return;
    }
    if (range.impossible) {
        global.has_event_range = true;
        global.event_range_impossible = true;
        return;
    }
    if (!global.has_event_range) {
        global.has_event_range = true;
        global.event_lower = range.lower;
        global.event_upper = range.upper;
        return;
    }
    global.event_lower = std::max(global.event_lower, range.lower);
    global.event_upper = std::min(global.event_upper, range.upper);
    global.event_range_impossible = global.event_lower > global.event_upper;
}

std::string ExactStringFilterClause(const TableFilter& filter, const std::string& column_sql) {
    switch (filter.filter_type) {
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto& constant = filter.Cast<ConstantFilter>();
        if (constant.comparison_type != ExpressionType::COMPARE_EQUAL || constant.constant.IsNull()) {
            return {};
        }
        return column_sql + " = " + SqlLiteral(constant.constant.ToString());
    }
    case TableFilterType::IN_FILTER: {
        const auto& in_filter = filter.Cast<InFilter>();
        if (in_filter.values.empty()) {
            return "FALSE";
        }
        std::string clause = column_sql + " IN (";
        bool first = true;
        for (const auto& constant : in_filter.values) {
            if (constant.IsNull()) {
                continue;
            }
            if (!first) {
                clause += ", ";
            }
            clause += SqlLiteral(constant.ToString());
            first = false;
        }
        if (first) {
            return "FALSE";
        }
        clause += ")";
        return clause;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto& optional = filter.Cast<OptionalFilter>();
        return optional.child_filter ? ExactStringFilterClause(*optional.child_filter, column_sql) : std::string();
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto& dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) {
            return {};
        }
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) {
            return {};
        }
        return ExactStringFilterClause(*dynamic.filter_data->filter, column_sql);
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto& conjunction = filter.Cast<ConjunctionAndFilter>();
        std::vector<std::string> clauses;
        for (const auto& child : conjunction.child_filters) {
            auto clause = ExactStringFilterClause(*child, column_sql);
            if (!clause.empty()) {
                clauses.push_back(std::move(clause));
            }
        }
        if (clauses.empty()) {
            return {};
        }
        std::string result = "(";
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) {
                result += " AND ";
            }
            result += clauses[i];
        }
        result += ")";
        return result;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto& conjunction = filter.Cast<ConjunctionOrFilter>();
        if (conjunction.child_filters.empty()) {
            return {};
        }
        std::vector<std::string> clauses;
        for (const auto& child : conjunction.child_filters) {
            auto clause = ExactStringFilterClause(*child, column_sql);
            if (clause.empty()) {
                return {};
            }
            clauses.push_back(std::move(clause));
        }
        std::string result = "(";
        for (idx_t i = 0; i < clauses.size(); ++i) {
            if (i) {
                result += " OR ";
            }
            result += clauses[i];
        }
        result += ")";
        return result;
    }
    default:
        return {};
    }
}

bool RejectsAllMaterializedRows(const TableFilter& filter) {
    switch (filter.filter_type) {
    case TableFilterType::IS_NULL:
        // read_root_dataset emits only materialized numeric rows. Empty containers
        // emit zero rows rather than a row containing SQL NULL.
        return true;
    case TableFilterType::CONJUNCTION_AND: {
        const auto& conjunction = filter.Cast<ConjunctionAndFilter>();
        for (const auto& child : conjunction.child_filters) {
            if (RejectsAllMaterializedRows(*child)) {
                return true;
            }
        }
        return false;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto& conjunction = filter.Cast<ConjunctionOrFilter>();
        if (conjunction.child_filters.empty()) {
            return false;
        }
        for (const auto& child : conjunction.child_filters) {
            if (!RejectsAllMaterializedRows(*child)) {
                return false;
            }
        }
        return true;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        const auto& optional = filter.Cast<OptionalFilter>();
        return optional.child_filter && RejectsAllMaterializedRows(*optional.child_filter);
    }
    case TableFilterType::DYNAMIC_FILTER: {
        const auto& dynamic = filter.Cast<DynamicFilter>();
        if (!dynamic.filter_data) {
            return false;
        }
        lock_guard<mutex> guard(dynamic.filter_data->lock);
        if (!dynamic.filter_data->initialized || !dynamic.filter_data->filter) {
            return false;
        }
        return RejectsAllMaterializedRows(*dynamic.filter_data->filter);
    }
    default:
        return false;
    }
}

std::string IdListSQL(const std::vector<SchemaBinding>& schemas, bool column_ids) {
    std::string out = "(";
    for (idx_t i = 0; i < schemas.size(); ++i) {
        if (i) {
            out += ',';
        }
        out += SqlLiteral(column_ids ? schemas[i].column_id : schemas[i].schema_id);
    }
    out += ')';
    return out;
}

bool PredicateMetadataMayMatch(const PathPredicateBinding& predicate, const Value& min_value, const Value& max_value,
                               uint64_t nan_count, uint64_t pos_inf_count, uint64_t neg_inf_count,
                               const Value& bloom_value) {

    // Current index-format min/max/Bloom transport is DOUBLE.
    // Never prune 64-bit integer predicates through it.
    if (!UsesDoubleBackedValueMetadata(predicate.value_type)) {
        return true;
    }

    const bool has_finite = !min_value.IsNull() && !max_value.IsNull();

    const double min_number = has_finite ? min_value.GetValue<double>() : 0.0;

    const double max_number = has_finite ? max_value.GetValue<double>() : 0.0;

    const string* bloom = nullptr;
    string bloom_storage;

    if (!bloom_value.IsNull()) {
        bloom_storage = StringValue::Get(bloom_value);

        bloom = &bloom_storage;
    }

    auto constant = [&](idx_t index) { return predicate.values[index].AsDouble(); };

    auto equality_possible = [&](double value) {
        if (!std::isfinite(value) || !has_finite || value < min_number || value > max_number) {
            return false;
        }

        return !bloom || BloomMayContain(*bloom, value);
    };

    switch (predicate.op) {
    case PathPredicateOp::EQ:
        return equality_possible(constant(0));

    case PathPredicateOp::NE:
        if (nan_count || pos_inf_count || neg_inf_count) {
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
        for (const auto& value : predicate.values) {
            if (equality_possible(value.AsDouble())) {
                return true;
            }
        }
        return false;
    }

    return true;
}

} // namespace duckdb::rootlake
