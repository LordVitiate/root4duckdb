#include "root4duckdb/dataset/root_dataset_pruning.hpp"

namespace duckdb::rootlake {

idx_t DatasetGlobalState::MaxThreads() const {
    if (metadata_count_only) {
        return 1;
    }
    return std::max<idx_t>(
        1, std::min<idx_t>(worker_limit, std::min<idx_t>(task_groups.size(),
                                                         static_cast<idx_t>(GlobalTableFunctionState::MAX_THREADS))));
}

bool RequiresTypedDatasetValue(const LogicalType& type) {
    return type.id() == LogicalTypeId::BIGINT || type.id() == LogicalTypeId::UBIGINT;
}

bool UsesDoubleBackedValueMetadata(const LogicalType& type) {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        return true;
    default:
        return false;
    }
}

static PathPredicateOp ParsePathPredicateOp(std::string op) {
    std::transform(op.begin(), op.end(), op.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (op == "=" || op == "==" || op == "eq") {
        return PathPredicateOp::EQ;
    }
    if (op == "!=" || op == "<>" || op == "ne") {
        return PathPredicateOp::NE;
    }
    if (op == "<" || op == "lt") {
        return PathPredicateOp::LT;
    }
    if (op == "<=" || op == "le") {
        return PathPredicateOp::LE;
    }
    if (op == ">" || op == "gt") {
        return PathPredicateOp::GT;
    }
    if (op == ">=" || op == "ge") {
        return PathPredicateOp::GE;
    }
    if (op == "between") {
        return PathPredicateOp::BETWEEN;
    }
    if (op == "in") {
        return PathPredicateOp::IN;
    }
    throw InvalidInputException("Unsupported path predicate operator: " + op);
}

static RootPrimitiveValue PathPredicateJsonValue(const nlohmann::json& value, const LogicalType& type) {

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        if (value.is_boolean()) {
            return RootPrimitiveValue::Unsigned(value.get<bool>() ? 1 : 0);
        }
        if (value.is_number_integer()) {
            const auto number = value.get<int64_t>();
            if (number == 0 || number == 1) {
                return RootPrimitiveValue::Unsigned(static_cast<uint64_t>(number));
            }
        }
        throw InvalidInputException("Boolean path predicate requires true/false or 0/1");

    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
        if (value.is_number_unsigned()) {
            const auto number = value.get<uint64_t>();

            if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                throw InvalidInputException("Signed path predicate constant is too large");
            }

            return RootPrimitiveValue::Signed(static_cast<int64_t>(number));
        }

        if (!value.is_number_integer()) {
            throw InvalidInputException("Signed integer path predicate requires "
                                        "an integer constant");
        }

        return RootPrimitiveValue::Signed(value.get<int64_t>());

    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
        if (value.is_number_unsigned()) {
            return RootPrimitiveValue::Unsigned(value.get<uint64_t>());
        }

        if (value.is_number_integer()) {
            const auto number = value.get<int64_t>();

            if (number < 0) {
                throw InvalidInputException("Unsigned path predicate cannot use "
                                            "a negative constant");
            }

            return RootPrimitiveValue::Unsigned(static_cast<uint64_t>(number));
        }

        throw InvalidInputException("Unsigned integer path predicate requires "
                                    "an integer constant");

    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
        if (!value.is_number()) {
            throw InvalidInputException("Floating path predicate requires "
                                        "a numeric constant");
        }

        return RootPrimitiveValue::Floating(value.get<double>());

    default:
        throw NotImplementedException("Unsupported path predicate type " + type.ToString());
    }
}

static int ComparePathPredicateValues(const RootPrimitiveValue& left, const RootPrimitiveValue& right,
                                      const LogicalType& type) {

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT: {
        const auto a = left.AsUnsigned();
        const auto b = right.AsUnsigned();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT: {
        const auto a = left.AsSigned();
        const auto b = right.AsSigned();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE: {
        const auto a = left.AsDouble();
        const auto b = right.AsDouble();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    default:
        throw NotImplementedException("Unsupported path predicate type " + type.ToString());
    }
}

void ParsePathPredicates(RootDatasetCatalog& catalog, DatasetBindData& bind, const std::string& raw_json) {

    if (raw_json.empty()) {
        return;
    }

    auto json = nlohmann::json::parse(raw_json);

    if (!json.is_array()) {
        throw InvalidInputException("path_predicates must be a JSON array");
    }

    for (const auto& item : json) {
        if (!item.is_object() || !item.contains("path") || !item.contains("op")) {
            throw InvalidInputException("Each path predicate needs path and op");
        }

        PathPredicateBinding predicate;

        predicate.path = NormalizePath(item.at("path").get<std::string>());

        predicate.op = ParsePathPredicateOp(item.at("op").get<std::string>());

        const auto quantifier = item.value("quantifier", std::string("any"));

        predicate.require_all_values = quantifier == "all";

        if (quantifier != "any" && quantifier != "all") {
            throw InvalidInputException("path predicate quantifier "
                                        "must be any or all");
        }

        // Resolve physical type BEFORE parsing constants.
        predicate.schemas = catalog.LoadPathSchemas(predicate.path, predicate.value_type, predicate.schema_lookup);

        auto add_value = [&](const nlohmann::json& value) {
            predicate.values.push_back(PathPredicateJsonValue(value, predicate.value_type));
        };

        if (predicate.op == PathPredicateOp::IN) {
            if (!item.contains("values") || !item.at("values").is_array()) {
                throw InvalidInputException("IN path predicate needs values array");
            }

            for (const auto& value : item.at("values")) {
                add_value(value);
            }

        } else if (predicate.op == PathPredicateOp::BETWEEN) {

            if (item.contains("values") && item.at("values").is_array() && item.at("values").size() == 2) {

                add_value(item.at("values")[0]);
                add_value(item.at("values")[1]);

            } else if (item.contains("lower") && item.contains("upper")) {

                add_value(item.at("lower"));
                add_value(item.at("upper"));

            } else {
                throw InvalidInputException("BETWEEN path predicate needs "
                                            "lower/upper or two values");
            }

            if (ComparePathPredicateValues(predicate.values[0], predicate.values[1], predicate.value_type) > 0) {
                std::swap(predicate.values[0], predicate.values[1]);
            }

        } else {
            if (!item.contains("value")) {
                throw InvalidInputException("Path predicate needs value");
            }

            add_value(item.at("value"));
        }

        if (predicate.values.empty()) {
            throw InvalidInputException("Path predicate has no constants");
        }

        bind.path_predicates.push_back(std::move(predicate));
    }
}

static bool PathPredicateValueMatches(const PathPredicateBinding& predicate, const RootPrimitiveValue& value) {

    const auto compare = [&](const RootPrimitiveValue& constant) {
        return ComparePathPredicateValues(value, constant, predicate.value_type);
    };

    switch (predicate.op) {
    case PathPredicateOp::EQ:
        return compare(predicate.values[0]) == 0;

    case PathPredicateOp::NE:
        return compare(predicate.values[0]) != 0;

    case PathPredicateOp::LT:
        return compare(predicate.values[0]) < 0;

    case PathPredicateOp::LE:
        return compare(predicate.values[0]) <= 0;

    case PathPredicateOp::GT:
        return compare(predicate.values[0]) > 0;

    case PathPredicateOp::GE:
        return compare(predicate.values[0]) >= 0;

    case PathPredicateOp::BETWEEN:
        return compare(predicate.values[0]) >= 0 && compare(predicate.values[1]) <= 0;

    case PathPredicateOp::IN:
        return std::any_of(predicate.values.begin(), predicate.values.end(),
                           [&](const RootPrimitiveValue& constant) { return compare(constant) == 0; });
    }

    return false;
}

bool PathPredicateEventMatches(const PathPredicateBinding& predicate, const std::vector<RootPrimitiveValue>& values) {

    if (values.empty()) {
        return false;
    }

    if (predicate.require_all_values) {
        return std::all_of(values.begin(), values.end(), [&](const RootPrimitiveValue& value) {
            return PathPredicateValueMatches(predicate, value);
        });
    }

    return std::any_of(values.begin(), values.end(),
                       [&](const RootPrimitiveValue& value) { return PathPredicateValueMatches(predicate, value); });
}

} // namespace duckdb::rootlake
