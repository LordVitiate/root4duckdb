#include "root4duckdb/histogram/root_histogram_internal.hpp"

namespace duckdb::rootlake::histogram_detail {

void AddColumn(RootHistogramSchema& schema, const char* name, LogicalTypeId type) {
    schema.names.emplace_back(name);
    schema.types.emplace_back(LogicalType(type));
}

RootHistogramSchema BinSchema() {
    RootHistogramSchema s;

    AddColumn(s, "object_path", LogicalTypeId::VARCHAR);
    AddColumn(s, "object_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "object_title", LogicalTypeId::VARCHAR);
    AddColumn(s, "class_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "dimension", LogicalTypeId::INTEGER);

    AddColumn(s, "global_bin", LogicalTypeId::INTEGER);
    AddColumn(s, "x_bin", LogicalTypeId::INTEGER);
    AddColumn(s, "y_bin", LogicalTypeId::INTEGER);
    AddColumn(s, "z_bin", LogicalTypeId::INTEGER);

    AddColumn(s, "is_underflow", LogicalTypeId::BOOLEAN);
    AddColumn(s, "is_overflow", LogicalTypeId::BOOLEAN);

    AddColumn(s, "x_low", LogicalTypeId::DOUBLE);
    AddColumn(s, "x_high", LogicalTypeId::DOUBLE);
    AddColumn(s, "x_center", LogicalTypeId::DOUBLE);
    AddColumn(s, "x_width", LogicalTypeId::DOUBLE);
    AddColumn(s, "x_label", LogicalTypeId::VARCHAR);

    AddColumn(s, "y_low", LogicalTypeId::DOUBLE);
    AddColumn(s, "y_high", LogicalTypeId::DOUBLE);
    AddColumn(s, "y_center", LogicalTypeId::DOUBLE);
    AddColumn(s, "y_width", LogicalTypeId::DOUBLE);
    AddColumn(s, "y_label", LogicalTypeId::VARCHAR);

    AddColumn(s, "z_low", LogicalTypeId::DOUBLE);
    AddColumn(s, "z_high", LogicalTypeId::DOUBLE);
    AddColumn(s, "z_center", LogicalTypeId::DOUBLE);
    AddColumn(s, "z_width", LogicalTypeId::DOUBLE);
    AddColumn(s, "z_label", LogicalTypeId::VARCHAR);

    AddColumn(s, "content", LogicalTypeId::DOUBLE);
    AddColumn(s, "error", LogicalTypeId::DOUBLE);
    AddColumn(s, "error_low", LogicalTypeId::DOUBLE);
    AddColumn(s, "error_high", LogicalTypeId::DOUBLE);
    AddColumn(s, "sumw2_raw", LogicalTypeId::DOUBLE);

    AddColumn(s, "profile_entries", LogicalTypeId::DOUBLE);
    AddColumn(s, "profile_effective_entries", LogicalTypeId::DOUBLE);
    AddColumn(s, "profile_bin_sumw2_raw", LogicalTypeId::DOUBLE);
    AddColumn(s, "profile_error_option", LogicalTypeId::VARCHAR);

    AddColumn(s, "entries", LogicalTypeId::DOUBLE);
    AddColumn(s, "effective_entries", LogicalTypeId::DOUBLE);
    AddColumn(s, "sum_weights", LogicalTypeId::DOUBLE);

    AddColumn(s, "x_underflow", LogicalTypeId::BOOLEAN);
    AddColumn(s, "x_overflow", LogicalTypeId::BOOLEAN);
    AddColumn(s, "y_underflow", LogicalTypeId::BOOLEAN);
    AddColumn(s, "y_overflow", LogicalTypeId::BOOLEAN);
    AddColumn(s, "z_underflow", LogicalTypeId::BOOLEAN);
    AddColumn(s, "z_overflow", LogicalTypeId::BOOLEAN);

    return s;
}

RootHistogramSchema AxisSchema() {
    RootHistogramSchema s;

    AddColumn(s, "object_path", LogicalTypeId::VARCHAR);
    AddColumn(s, "object_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "class_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "dimension", LogicalTypeId::INTEGER);

    AddColumn(s, "axis_idx", LogicalTypeId::INTEGER);
    AddColumn(s, "axis_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "title", LogicalTypeId::VARCHAR);
    AddColumn(s, "nbins", LogicalTypeId::INTEGER);
    AddColumn(s, "min", LogicalTypeId::DOUBLE);
    AddColumn(s, "max", LogicalTypeId::DOUBLE);
    AddColumn(s, "first_bin", LogicalTypeId::INTEGER);
    AddColumn(s, "last_bin", LogicalTypeId::INTEGER);

    AddColumn(s, "variable_bins", LogicalTypeId::BOOLEAN);
    AddColumn(s, "time_display", LogicalTypeId::BOOLEAN);
    AddColumn(s, "time_format", LogicalTypeId::VARCHAR);
    AddColumn(s, "labels_present", LogicalTypeId::BOOLEAN);

    AddColumn(s, "axis_color", LogicalTypeId::INTEGER);
    AddColumn(s, "label_color", LogicalTypeId::INTEGER);
    AddColumn(s, "label_font", LogicalTypeId::INTEGER);
    AddColumn(s, "label_offset", LogicalTypeId::DOUBLE);
    AddColumn(s, "label_size", LogicalTypeId::DOUBLE);

    AddColumn(s, "title_color", LogicalTypeId::INTEGER);
    AddColumn(s, "title_font", LogicalTypeId::INTEGER);
    AddColumn(s, "title_offset", LogicalTypeId::DOUBLE);
    AddColumn(s, "title_size", LogicalTypeId::DOUBLE);

    AddColumn(s, "tick_length", LogicalTypeId::DOUBLE);
    AddColumn(s, "ndivisions", LogicalTypeId::INTEGER);

    return s;
}

RootHistogramSchema MetaSchema() {
    RootHistogramSchema s;

    AddColumn(s, "object_path", LogicalTypeId::VARCHAR);
    AddColumn(s, "object_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "object_title", LogicalTypeId::VARCHAR);
    AddColumn(s, "class_name", LogicalTypeId::VARCHAR);
    AddColumn(s, "dimension", LogicalTypeId::INTEGER);
    AddColumn(s, "is_profile", LogicalTypeId::BOOLEAN);

    AddColumn(s, "entries", LogicalTypeId::DOUBLE);
    AddColumn(s, "effective_entries", LogicalTypeId::DOUBLE);
    AddColumn(s, "sum_weights", LogicalTypeId::DOUBLE);
    AddColumn(s, "n_cells", LogicalTypeId::UBIGINT);

    AddColumn(s, "has_sumw2", LogicalTypeId::BOOLEAN);
    AddColumn(s, "sumw2_size", LogicalTypeId::INTEGER);
    AddColumn(s, "norm_factor", LogicalTypeId::DOUBLE);

    AddColumn(s, "minimum", LogicalTypeId::DOUBLE);
    AddColumn(s, "maximum", LogicalTypeId::DOUBLE);
    AddColumn(s, "minimum_stored", LogicalTypeId::DOUBLE);
    AddColumn(s, "maximum_stored", LogicalTypeId::DOUBLE);

    AddColumn(s, "mean_x", LogicalTypeId::DOUBLE);
    AddColumn(s, "mean_error_x", LogicalTypeId::DOUBLE);
    AddColumn(s, "stddev_x", LogicalTypeId::DOUBLE);
    AddColumn(s, "stddev_error_x", LogicalTypeId::DOUBLE);
    AddColumn(s, "skewness_x", LogicalTypeId::DOUBLE);
    AddColumn(s, "kurtosis_x", LogicalTypeId::DOUBLE);

    AddColumn(s, "mean_y", LogicalTypeId::DOUBLE);
    AddColumn(s, "mean_error_y", LogicalTypeId::DOUBLE);
    AddColumn(s, "stddev_y", LogicalTypeId::DOUBLE);
    AddColumn(s, "stddev_error_y", LogicalTypeId::DOUBLE);
    AddColumn(s, "skewness_y", LogicalTypeId::DOUBLE);
    AddColumn(s, "kurtosis_y", LogicalTypeId::DOUBLE);

    AddColumn(s, "mean_z", LogicalTypeId::DOUBLE);
    AddColumn(s, "mean_error_z", LogicalTypeId::DOUBLE);
    AddColumn(s, "stddev_z", LogicalTypeId::DOUBLE);
    AddColumn(s, "stddev_error_z", LogicalTypeId::DOUBLE);
    AddColumn(s, "skewness_z", LogicalTypeId::DOUBLE);
    AddColumn(s, "kurtosis_z", LogicalTypeId::DOUBLE);

    AddColumn(s, "bin_error_option", LogicalTypeId::INTEGER);
    AddColumn(s, "stat_overflows", LogicalTypeId::BOOLEAN);
    AddColumn(s, "can_extend_all_axes", LogicalTypeId::BOOLEAN);

    AddColumn(s, "option", LogicalTypeId::VARCHAR);
    AddColumn(s, "draw_option", LogicalTypeId::VARCHAR);

    AddColumn(s, "line_color", LogicalTypeId::INTEGER);
    AddColumn(s, "line_style", LogicalTypeId::INTEGER);
    AddColumn(s, "line_width", LogicalTypeId::INTEGER);

    AddColumn(s, "fill_color", LogicalTypeId::INTEGER);
    AddColumn(s, "fill_style", LogicalTypeId::INTEGER);

    AddColumn(s, "marker_color", LogicalTypeId::INTEGER);
    AddColumn(s, "marker_style", LogicalTypeId::INTEGER);
    AddColumn(s, "marker_size", LogicalTypeId::DOUBLE);

    AddColumn(s, "buffer_size", LogicalTypeId::INTEGER);
    AddColumn(s, "buffer_length", LogicalTypeId::INTEGER);

    AddColumn(s, "unique_id", LogicalTypeId::UINTEGER);
    AddColumn(s, "object_size_bytes", LogicalTypeId::INTEGER);

    AddColumn(s, "function_count", LogicalTypeId::INTEGER);
    AddColumn(s, "functions", LogicalTypeId::VARCHAR);

    AddColumn(s, "profile_error_option", LogicalTypeId::VARCHAR);

    AddColumn(s, "x_nbins", LogicalTypeId::INTEGER);
    AddColumn(s, "y_nbins", LogicalTypeId::INTEGER);
    AddColumn(s, "z_nbins", LogicalTypeId::INTEGER);

    return s;
}

} // namespace duckdb::rootlake::histogram_detail
