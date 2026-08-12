#pragma once

#include "duckdb/common/types/value.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace duckdb {
class ClientContext;
class ExpressionExecutor;
class ExpressionFilter;
class TableFilter;

namespace rootlake {

struct RootScalarActual {
    LogicalType type;
    bool is_null = false;
    double numeric = 0;
    uint64_t unsigned_value = 0;
    int64_t signed_value = 0;
    std::string string_value;

    static RootScalarActual Null(const LogicalType &type);
    static RootScalarActual Event(uint64_t value);
    static RootScalarActual Signed(int64_t value, const LogicalType &type = LogicalType::BIGINT);
    static RootScalarActual Unsigned(uint64_t value, const LogicalType &type = LogicalType::UBIGINT);
    static RootScalarActual Index(std::optional<int32_t> value);
    static RootScalarActual String(std::string value);
    static RootScalarActual Numeric(const LogicalType &type, double value);
    Value ToValue() const;
};

struct RootUnsignedFilterRange {
    bool known = false;
    bool impossible = false;
    uint64_t lower = 0;
    uint64_t upper = std::numeric_limits<uint64_t>::max();
};

RootUnsignedFilterRange ExtractRootUnsignedRange(const TableFilter &filter);

class RootFilterEvaluator {
public:
    RootFilterEvaluator();
    ~RootFilterEvaluator();
    RootFilterEvaluator(RootFilterEvaluator &&) noexcept;
    RootFilterEvaluator &operator=(RootFilterEvaluator &&) noexcept;

    bool Evaluate(ClientContext &context, const TableFilter &filter, const RootScalarActual &actual);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rootlake
} // namespace duckdb
