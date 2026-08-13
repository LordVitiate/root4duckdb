#pragma once

#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/index/root_filter.hpp"

#include "duckdb/common/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb::rootlake {

/// Canonical relational view of a ROOT histogram.
enum class RootHistogramView { BINS, AXES, META };

/// Ordered names and types returned by one view.
struct RootHistogramSchema {
    std::vector<std::string> names;
    std::vector<LogicalType> types;
};

/// Bound ROOT object and its canonical SQL schema.
struct RootHistogramBinding {
    std::string object_path;
    std::string class_name;
    RootHistogramView view = RootHistogramView::BINS;
    int32_t dimension = 0;
    bool is_profile = false;
    uint64_t row_count = 0;
    RootHistogramSchema schema;
};

/// Resolves and owns a supported histogram object.
bool TryBindRootHistogram(TFile& file, const std::string& requested_path, RootHistogramBinding& binding,
                          std::unique_ptr<TH1>& histogram);

/// Materializes one canonical histogram row.
void MaterializeRootHistogramRow(const RootHistogramBinding& binding, TH1& histogram, uint64_t row,
                                 std::vector<RootScalarActual>& values);

/// Writes one typed histogram scalar into a DuckDB vector.
void WriteRootHistogramActual(Vector& vector, idx_t row, const RootScalarActual& actual);

/// Returns the stable SQL view name.
const char* RootHistogramViewName(RootHistogramView view);

} // namespace duckdb::rootlake
