#pragma once

#include "root_headers.hpp"
#include "root_filter.hpp"

#include "duckdb/common/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb::rootlake {

enum class RootHistogramView {
    BINS,
    AXES,
    META
};

struct RootHistogramSchema {
    std::vector<std::string> names;
    std::vector<LogicalType> types;
};

struct RootHistogramBinding {
    std::string object_path;
    std::string class_name;
    RootHistogramView view = RootHistogramView::BINS;
    int32_t dimension = 0;
    bool is_profile = false;
    uint64_t row_count = 0;
    RootHistogramSchema schema;
};

bool TryBindRootHistogram(
    TFile &file,
    const std::string &requested_path,
    RootHistogramBinding &binding,
    std::unique_ptr<TH1> &histogram);

void MaterializeRootHistogramRow(
    const RootHistogramBinding &binding,
    TH1 &histogram,
    uint64_t row,
    std::vector<RootScalarActual> &values);

void WriteRootHistogramActual(
    Vector &vector,
    idx_t row,
    const RootScalarActual &actual);

const char *RootHistogramViewName(
    RootHistogramView view);

} // namespace duckdb::rootlake
