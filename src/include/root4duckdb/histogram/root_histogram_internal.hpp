#pragma once

#include "root4duckdb/histogram/root_histogram_reader.hpp"

namespace duckdb::rootlake::histogram_detail {

/// Builds the canonical histogram-bin schema.
RootHistogramSchema BinSchema();
/// Builds the canonical histogram-axis schema.
RootHistogramSchema AxisSchema();
/// Builds the canonical histogram-metadata schema.
RootHistogramSchema MetaSchema();
/// Detects profile histogram subclasses.
bool IsProfile(const TH1& histogram);
/// Materializes one canonical bin row.
void MaterializeBinRow(const RootHistogramBinding& binding, TH1& histogram, uint64_t row,
                       std::vector<RootScalarActual>& values);
/// Materializes one canonical axis row.
void MaterializeAxisRow(const RootHistogramBinding& binding, TH1& histogram, uint64_t row,
                        std::vector<RootScalarActual>& values);
/// Materializes the histogram metadata row.
void MaterializeMetaRow(const RootHistogramBinding& binding, TH1& histogram, std::vector<RootScalarActual>& values);

} // namespace duckdb::rootlake::histogram_detail
