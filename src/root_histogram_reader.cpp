#include "include/root_histogram_reader.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb::rootlake {

namespace {

void AddColumn(
    RootHistogramSchema &schema,
    const char *name,
    LogicalTypeId type)
{
    schema.names.emplace_back(name);
    schema.types.emplace_back(LogicalType(type));
}

RootHistogramSchema BinSchema()
{
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

RootHistogramSchema AxisSchema()
{
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

RootHistogramSchema MetaSchema()
{
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

RootScalarActual Null(const LogicalType &type)
{
    return RootScalarActual::Null(type);
}

RootScalarActual Str(const std::string &value)
{
    return RootScalarActual::String(value);
}

RootScalarActual Int(int64_t value)
{
    return RootScalarActual::Signed(
        value, LogicalType(LogicalTypeId::INTEGER));
}

RootScalarActual UInt(uint64_t value)
{
    return RootScalarActual::Unsigned(
        value, LogicalType(LogicalTypeId::UINTEGER));
}

RootScalarActual UBig(uint64_t value)
{
    return RootScalarActual::Unsigned(
        value, LogicalType(LogicalTypeId::UBIGINT));
}

RootScalarActual Bool(bool value)
{
    return RootScalarActual::Numeric(
        LogicalType(LogicalTypeId::BOOLEAN),
        value ? 1.0 : 0.0);
}

RootScalarActual Double(double value)
{
    return RootScalarActual::Numeric(
        LogicalType(LogicalTypeId::DOUBLE),
        value);
}

bool EndsWith(
    const std::string &value,
    const std::string &suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(
               value.size() - suffix.size(),
               suffix.size(),
               suffix) == 0;
}

std::string NormalizeObjectPath(
    const std::string &path)
{
    if (path.empty()) {
        return path;
    }

    if (path.front() == '/') {
        return path.substr(1);
    }

    return path;
}

bool IsProfile(const TH1 &histogram)
{
    return dynamic_cast<const TProfile *>(&histogram) ||
           dynamic_cast<const TProfile2D *>(&histogram) ||
           dynamic_cast<const TProfile3D *>(&histogram);
}

TAxis *Axis(
    TH1 &histogram,
    int axis)
{
    switch (axis) {
    case 1:
        return histogram.GetXaxis();
    case 2:
        return histogram.GetYaxis();
    case 3:
        return histogram.GetZaxis();
    default:
        return nullptr;
    }
}

const char *AxisName(int axis)
{
    switch (axis) {
    case 1:
        return "x";
    case 2:
        return "y";
    case 3:
        return "z";
    default:
        return "";
    }
}

bool AxisHasLabels(TAxis &axis)
{
    for (int bin = 1; bin <= axis.GetNbins(); ++bin) {
        const char *label = axis.GetBinLabel(bin);
        if (label && *label) {
            return true;
        }
    }

    return false;
}

bool ProfileBinData(
    TH1 &histogram,
    int global_bin,
    double &entries,
    double &effective_entries,
    double &bin_sumw2,
    bool &has_bin_sumw2,
    std::string &error_option)
{
    if (auto *profile =
            dynamic_cast<TProfile *>(&histogram)) {
        entries = profile->GetBinEntries(global_bin);
        effective_entries =
            profile->GetBinEffectiveEntries(global_bin);

        const auto *sumw2 = profile->GetBinSumw2();
        has_bin_sumw2 =
            sumw2 &&
            global_bin >= 0 &&
            global_bin < sumw2->GetSize();

        if (has_bin_sumw2) {
            bin_sumw2 = sumw2->At(global_bin);
        }

        const auto *option = profile->GetErrorOption();
        error_option = option ? option : "";
        return true;
    }

    if (auto *profile =
            dynamic_cast<TProfile2D *>(&histogram)) {
        entries = profile->GetBinEntries(global_bin);
        effective_entries =
            profile->GetBinEffectiveEntries(global_bin);

        const auto *sumw2 = profile->GetBinSumw2();
        has_bin_sumw2 =
            sumw2 &&
            global_bin >= 0 &&
            global_bin < sumw2->GetSize();

        if (has_bin_sumw2) {
            bin_sumw2 = sumw2->At(global_bin);
        }

        const auto *option = profile->GetErrorOption();
        error_option = option ? option : "";
        return true;
    }

    if (auto *profile =
            dynamic_cast<TProfile3D *>(&histogram)) {
        entries = profile->GetBinEntries(global_bin);
        effective_entries =
            profile->GetBinEffectiveEntries(global_bin);

        const auto *sumw2 = profile->GetBinSumw2();
        has_bin_sumw2 =
            sumw2 &&
            global_bin >= 0 &&
            global_bin < sumw2->GetSize();

        if (has_bin_sumw2) {
            bin_sumw2 = sumw2->At(global_bin);
        }

        const auto *option = profile->GetErrorOption();
        error_option = option ? option : "";
        return true;
    }

    return false;
}

std::string ProfileErrorOption(TH1 &histogram)
{
    double entries = 0;
    double effective = 0;
    double sumw2 = 0;
    bool has_sumw2 = false;
    std::string option;

    ProfileBinData(
        histogram,
        0,
        entries,
        effective,
        sumw2,
        has_sumw2,
        option);

    return option;
}

std::string FunctionList(TH1 &histogram)
{
    auto *functions = histogram.GetListOfFunctions();

    if (!functions) {
        return {};
    }

    std::ostringstream output;
    TIter next(functions);

    bool first = true;

    while (auto *object =
               dynamic_cast<TObject *>(next())) {
        if (!first) {
            output << ";";
        }

        first = false;

        output
            << object->GetName()
            << ":"
            << object->ClassName();
    }

    return output.str();
}

int FunctionCount(TH1 &histogram)
{
    auto *functions = histogram.GetListOfFunctions();
    return functions ? functions->GetSize() : 0;
}

void MaterializeBinRow(
    const RootHistogramBinding &binding,
    TH1 &histogram,
    uint64_t row,
    std::vector<RootScalarActual> &values)
{
    values.clear();
    values.reserve(binding.schema.names.size());

    const int global_bin =
        static_cast<int>(row);

    int x_bin = 0;
    int y_bin = 0;
    int z_bin = 0;

    histogram.GetBinXYZ(
        global_bin,
        x_bin,
        y_bin,
        z_bin);

    const int dimension =
        histogram.GetDimension();

    auto *x = histogram.GetXaxis();
    auto *y = histogram.GetYaxis();
    auto *z = histogram.GetZaxis();

    const bool x_underflow = x_bin == 0;
    const bool x_overflow =
        x_bin == x->GetNbins() + 1;

    const bool y_underflow =
        dimension >= 2 && y_bin == 0;

    const bool y_overflow =
        dimension >= 2 &&
        y_bin == y->GetNbins() + 1;

    const bool z_underflow =
        dimension >= 3 && z_bin == 0;

    const bool z_overflow =
        dimension >= 3 &&
        z_bin == z->GetNbins() + 1;

    const bool underflow =
        x_underflow ||
        y_underflow ||
        z_underflow;

    const bool overflow =
        x_overflow ||
        y_overflow ||
        z_overflow;

    values.push_back(Str(binding.object_path));
    values.push_back(Str(histogram.GetName()));
    values.push_back(Str(histogram.GetTitle()));
    values.push_back(Str(binding.class_name));
    values.push_back(Int(dimension));

    values.push_back(Int(global_bin));
    values.push_back(Int(x_bin));

    values.push_back(
        dimension >= 2
            ? Int(y_bin)
            : Null(binding.schema.types[7]));

    values.push_back(
        dimension >= 3
            ? Int(z_bin)
            : Null(binding.schema.types[8]));

    values.push_back(Bool(underflow));
    values.push_back(Bool(overflow));

    values.push_back(Double(x->GetBinLowEdge(x_bin)));
    values.push_back(Double(x->GetBinUpEdge(x_bin)));
    values.push_back(Double(x->GetBinCenter(x_bin)));
    values.push_back(Double(x->GetBinWidth(x_bin)));
    values.push_back(Str(x->GetBinLabel(x_bin)));

    if (dimension >= 2) {
        values.push_back(Double(y->GetBinLowEdge(y_bin)));
        values.push_back(Double(y->GetBinUpEdge(y_bin)));
        values.push_back(Double(y->GetBinCenter(y_bin)));
        values.push_back(Double(y->GetBinWidth(y_bin)));
        values.push_back(Str(y->GetBinLabel(y_bin)));
    } else {
        for (idx_t i = 16; i <= 20; ++i) {
            values.push_back(Null(binding.schema.types[i]));
        }
    }

    if (dimension >= 3) {
        values.push_back(Double(z->GetBinLowEdge(z_bin)));
        values.push_back(Double(z->GetBinUpEdge(z_bin)));
        values.push_back(Double(z->GetBinCenter(z_bin)));
        values.push_back(Double(z->GetBinWidth(z_bin)));
        values.push_back(Str(z->GetBinLabel(z_bin)));
    } else {
        for (idx_t i = 21; i <= 25; ++i) {
            values.push_back(Null(binding.schema.types[i]));
        }
    }

    values.push_back(
        Double(histogram.GetBinContent(global_bin)));

    values.push_back(
        Double(histogram.GetBinError(global_bin)));

    values.push_back(
        Double(histogram.GetBinErrorLow(global_bin)));

    values.push_back(
        Double(histogram.GetBinErrorUp(global_bin)));

    if (histogram.GetSumw2N() > global_bin) {
        values.push_back(
            Double(
                histogram.GetSumw2()->At(
                    global_bin)));
    } else {
        values.push_back(
            Null(binding.schema.types[30]));
    }

    double profile_entries = 0;
    double profile_effective = 0;
    double profile_bin_sumw2 = 0;
    bool has_profile_bin_sumw2 = false;
    std::string profile_error_option;

    const bool profile =
        ProfileBinData(
            histogram,
            global_bin,
            profile_entries,
            profile_effective,
            profile_bin_sumw2,
            has_profile_bin_sumw2,
            profile_error_option);

    values.push_back(
        profile
            ? Double(profile_entries)
            : Null(binding.schema.types[31]));

    values.push_back(
        profile
            ? Double(profile_effective)
            : Null(binding.schema.types[32]));

    values.push_back(
        profile && has_profile_bin_sumw2
            ? Double(profile_bin_sumw2)
            : Null(binding.schema.types[33]));

    values.push_back(
        profile
            ? Str(profile_error_option)
            : Null(binding.schema.types[34]));

    values.push_back(
        Double(histogram.GetEntries()));

    values.push_back(
        Double(histogram.GetEffectiveEntries()));

    values.push_back(
        Double(histogram.GetSumOfWeights()));

    values.push_back(Bool(x_underflow));
    values.push_back(Bool(x_overflow));

    values.push_back(
        dimension >= 2
            ? Bool(y_underflow)
            : Null(binding.schema.types[40]));

    values.push_back(
        dimension >= 2
            ? Bool(y_overflow)
            : Null(binding.schema.types[41]));

    values.push_back(
        dimension >= 3
            ? Bool(z_underflow)
            : Null(binding.schema.types[42]));

    values.push_back(
        dimension >= 3
            ? Bool(z_overflow)
            : Null(binding.schema.types[43]));
}

void MaterializeAxisRow(
    const RootHistogramBinding &binding,
    TH1 &histogram,
    uint64_t row,
    std::vector<RootScalarActual> &values)
{
    values.clear();
    values.reserve(binding.schema.names.size());

    const int axis_index =
        static_cast<int>(row) + 1;

    auto *axis =
        Axis(histogram, axis_index);

    if (!axis) {
        throw InternalException(
            "Invalid ROOT histogram axis index");
    }

    values.push_back(Str(binding.object_path));
    values.push_back(Str(histogram.GetName()));
    values.push_back(Str(binding.class_name));
    values.push_back(Int(histogram.GetDimension()));

    values.push_back(Int(axis_index));
    values.push_back(Str(AxisName(axis_index)));
    values.push_back(Str(axis->GetTitle()));
    values.push_back(Int(axis->GetNbins()));
    values.push_back(Double(axis->GetXmin()));
    values.push_back(Double(axis->GetXmax()));
    values.push_back(Int(axis->GetFirst()));
    values.push_back(Int(axis->GetLast()));

    const auto *bins = axis->GetXbins();

    values.push_back(
        Bool(bins && bins->GetSize() > 0));

    values.push_back(
        Bool(axis->GetTimeDisplay()));

    values.push_back(
        Str(axis->GetTimeFormat()));

    values.push_back(
        Bool(AxisHasLabels(*axis)));

    values.push_back(Int(axis->GetAxisColor()));
    values.push_back(Int(axis->GetLabelColor()));
    values.push_back(Int(axis->GetLabelFont()));
    values.push_back(Double(axis->GetLabelOffset()));
    values.push_back(Double(axis->GetLabelSize()));

    values.push_back(Int(axis->GetTitleColor()));
    values.push_back(Int(axis->GetTitleFont()));
    values.push_back(Double(axis->GetTitleOffset()));
    values.push_back(Double(axis->GetTitleSize()));

    values.push_back(Double(axis->GetTickLength()));
    values.push_back(Int(axis->GetNdivisions()));
}

void MaterializeMetaRow(
    const RootHistogramBinding &binding,
    TH1 &histogram,
    std::vector<RootScalarActual> &values)
{
    values.clear();
    values.reserve(binding.schema.names.size());

    if (binding.schema.types.size() != 58) {
        throw InternalException(
            "ROOT histogram meta schema must contain 58 columns");
    }

    constexpr idx_t PROFILE_ERROR_OPTION_COLUMN = 54;
    constexpr idx_t Y_NBINS_COLUMN = 56;
    constexpr idx_t Z_NBINS_COLUMN = 57;

    const int dimension =
        histogram.GetDimension();

    values.push_back(Str(binding.object_path));
    values.push_back(Str(histogram.GetName()));
    values.push_back(Str(histogram.GetTitle()));
    values.push_back(Str(binding.class_name));
    values.push_back(Int(dimension));
    values.push_back(Bool(binding.is_profile));

    values.push_back(Double(histogram.GetEntries()));
    values.push_back(Double(histogram.GetEffectiveEntries()));
    values.push_back(Double(histogram.GetSumOfWeights()));
    values.push_back(UBig(histogram.GetNcells()));

    values.push_back(Bool(histogram.GetSumw2N() > 0));
    values.push_back(Int(histogram.GetSumw2N()));
    values.push_back(Double(histogram.GetNormFactor()));

    values.push_back(Double(histogram.GetMinimum()));
    values.push_back(Double(histogram.GetMaximum()));
    values.push_back(Double(histogram.GetMinimumStored()));
    values.push_back(Double(histogram.GetMaximumStored()));

    values.push_back(Double(histogram.GetMean(1)));
    values.push_back(Double(histogram.GetMeanError(1)));
    values.push_back(Double(histogram.GetStdDev(1)));
    values.push_back(Double(histogram.GetStdDevError(1)));
    values.push_back(Double(histogram.GetSkewness(1)));
    values.push_back(Double(histogram.GetKurtosis(1)));

    if (dimension >= 2) {
        values.push_back(Double(histogram.GetMean(2)));
        values.push_back(Double(histogram.GetMeanError(2)));
        values.push_back(Double(histogram.GetStdDev(2)));
        values.push_back(Double(histogram.GetStdDevError(2)));
        values.push_back(Double(histogram.GetSkewness(2)));
        values.push_back(Double(histogram.GetKurtosis(2)));
    } else {
        for (idx_t i = 23; i <= 28; ++i) {
            values.push_back(Null(binding.schema.types[i]));
        }
    }

    if (dimension >= 3) {
        values.push_back(Double(histogram.GetMean(3)));
        values.push_back(Double(histogram.GetMeanError(3)));
        values.push_back(Double(histogram.GetStdDev(3)));
        values.push_back(Double(histogram.GetStdDevError(3)));
        values.push_back(Double(histogram.GetSkewness(3)));
        values.push_back(Double(histogram.GetKurtosis(3)));
    } else {
        for (idx_t i = 29; i <= 34; ++i) {
            values.push_back(Null(binding.schema.types[i]));
        }
    }

    values.push_back(
        Int(
            static_cast<int>(
                histogram.GetBinErrorOption())));

    values.push_back(
        Bool(histogram.GetStatOverflows()));

    values.push_back(
        Bool(histogram.CanExtendAllAxes()));

    values.push_back(Str(histogram.GetOption()));
    values.push_back(Str(histogram.GetDrawOption()));

    values.push_back(Int(histogram.GetLineColor()));
    values.push_back(Int(histogram.GetLineStyle()));
    values.push_back(Int(histogram.GetLineWidth()));

    values.push_back(Int(histogram.GetFillColor()));
    values.push_back(Int(histogram.GetFillStyle()));

    values.push_back(Int(histogram.GetMarkerColor()));
    values.push_back(Int(histogram.GetMarkerStyle()));
    values.push_back(Double(histogram.GetMarkerSize()));

    values.push_back(Int(histogram.GetBufferSize()));
    values.push_back(Int(histogram.GetBufferLength()));

    values.push_back(UInt(histogram.GetUniqueID()));
    values.push_back(Int(histogram.Sizeof()));

    values.push_back(Int(FunctionCount(histogram)));
    values.push_back(Str(FunctionList(histogram)));

    values.push_back(
        binding.is_profile
            ? Str(ProfileErrorOption(histogram))
            : Null(binding.schema.types[PROFILE_ERROR_OPTION_COLUMN]));

    values.push_back(Int(histogram.GetNbinsX()));

    values.push_back(
        dimension >= 2
            ? Int(histogram.GetNbinsY())
            : Null(binding.schema.types[Y_NBINS_COLUMN]));

    values.push_back(
        dimension >= 3
            ? Int(histogram.GetNbinsZ())
            : Null(binding.schema.types[Z_NBINS_COLUMN]));
}

} // namespace

const char *RootHistogramViewName(
    RootHistogramView view)
{
    switch (view) {
    case RootHistogramView::BINS:
        return "bins";
    case RootHistogramView::AXES:
        return "axes";
    case RootHistogramView::META:
        return "meta";
    }

    return "unknown";
}

bool TryBindRootHistogram(
    TFile &file,
    const std::string &requested_path,
    RootHistogramBinding &binding,
    std::unique_ptr<TH1> &histogram)
{
    std::string path =
        NormalizeObjectPath(requested_path);

    if (path.empty()) {
        return false;
    }

    RootHistogramView view =
        RootHistogramView::BINS;

    TObject *object =
        file.Get(path.c_str());

    auto *root_histogram =
        dynamic_cast<TH1 *>(object);

    if (!root_histogram) {
        struct Suffix {
            const char *text;
            RootHistogramView view;
        };

        static const Suffix suffixes[] = {
            {"/bins", RootHistogramView::BINS},
            {"/axes", RootHistogramView::AXES},
            {"/meta", RootHistogramView::META}
        };

        for (const auto &suffix : suffixes) {
            const std::string suffix_text =
                suffix.text;

            if (!EndsWith(path, suffix_text) ||
                path.size() <= suffix_text.size()) {
                continue;
            }

            const auto object_path =
                path.substr(
                    0,
                    path.size() - suffix_text.size());

            object =
                file.Get(object_path.c_str());

            root_histogram =
                dynamic_cast<TH1 *>(object);

            if (root_histogram) {
                path = object_path;
                view = suffix.view;
                break;
            }
        }
    }

    if (!root_histogram) {
        return false;
    }

    if (root_histogram->InheritsFrom("TH2Poly")) {
        throw NotImplementedException(
            "TH2Poly requires a polygon-bin adapter; "
            "rectangular TH1/TH2/TH3 flattening is not valid");
    }

    auto *clone =
        dynamic_cast<TH1 *>(
            root_histogram->Clone());

    if (!clone) {
        throw IOException(
            "Failed to clone ROOT histogram " +
            requested_path);
    }

    clone->SetDirectory(nullptr);

    histogram.reset(clone);

    binding.object_path =
        "/" + path;

    binding.class_name =
        clone->ClassName();

    binding.view = view;

    binding.dimension =
        clone->GetDimension();

    binding.is_profile =
        IsProfile(*clone);

    switch (view) {
    case RootHistogramView::BINS:
        binding.schema = BinSchema();
        binding.row_count =
            static_cast<uint64_t>(
                clone->GetNcells());
        break;

    case RootHistogramView::AXES:
        binding.schema = AxisSchema();
        binding.row_count =
            static_cast<uint64_t>(
                binding.dimension);
        break;

    case RootHistogramView::META:
        binding.schema = MetaSchema();
        binding.row_count = 1;
        break;
    }

    return true;
}

void MaterializeRootHistogramRow(
    const RootHistogramBinding &binding,
    TH1 &histogram,
    uint64_t row,
    std::vector<RootScalarActual> &values)
{
    if (row >= binding.row_count) {
        throw InternalException(
            "ROOT histogram row outside bound range");
    }

    switch (binding.view) {
    case RootHistogramView::BINS:
        MaterializeBinRow(
            binding,
            histogram,
            row,
            values);
        break;

    case RootHistogramView::AXES:
        MaterializeAxisRow(
            binding,
            histogram,
            row,
            values);
        break;

    case RootHistogramView::META:
        MaterializeMetaRow(
            binding,
            histogram,
            values);
        break;
    }

    if (values.size() !=
        binding.schema.names.size()) {
        throw InternalException(
            "ROOT histogram schema/materialization mismatch");
    }
}

void WriteRootHistogramActual(
    Vector &vector,
    idx_t row,
    const RootScalarActual &actual)
{
    if (actual.is_null) {
        FlatVector::Validity(vector)
            .SetInvalid(row);
        return;
    }

    switch (vector.GetType().id()) {
    case LogicalTypeId::BOOLEAN:
        FlatVector::GetData<bool>(
            vector)[row] =
            actual.numeric != 0;
        break;

    case LogicalTypeId::TINYINT:
        FlatVector::GetData<int8_t>(
            vector)[row] =
            static_cast<int8_t>(
                actual.signed_value);
        break;

    case LogicalTypeId::UTINYINT:
        FlatVector::GetData<uint8_t>(
            vector)[row] =
            static_cast<uint8_t>(
                actual.unsigned_value);
        break;

    case LogicalTypeId::SMALLINT:
        FlatVector::GetData<int16_t>(
            vector)[row] =
            static_cast<int16_t>(
                actual.signed_value);
        break;

    case LogicalTypeId::USMALLINT:
        FlatVector::GetData<uint16_t>(
            vector)[row] =
            static_cast<uint16_t>(
                actual.unsigned_value);
        break;

    case LogicalTypeId::INTEGER:
        FlatVector::GetData<int32_t>(
            vector)[row] =
            static_cast<int32_t>(
                actual.signed_value);
        break;

    case LogicalTypeId::UINTEGER:
        FlatVector::GetData<uint32_t>(
            vector)[row] =
            static_cast<uint32_t>(
                actual.unsigned_value);
        break;

    case LogicalTypeId::BIGINT:
        FlatVector::GetData<int64_t>(
            vector)[row] =
            actual.signed_value;
        break;

    case LogicalTypeId::UBIGINT:
        FlatVector::GetData<uint64_t>(
            vector)[row] =
            actual.unsigned_value;
        break;

    case LogicalTypeId::FLOAT:
        FlatVector::GetData<float>(
            vector)[row] =
            static_cast<float>(
                actual.numeric);
        break;

    case LogicalTypeId::DOUBLE:
        FlatVector::GetData<double>(
            vector)[row] =
            actual.numeric;
        break;

    case LogicalTypeId::VARCHAR:
        FlatVector::GetData<string_t>(
            vector)[row] =
            StringVector::AddString(
                vector,
                actual.string_value);
        break;

    default:
        throw NotImplementedException(
            "Unsupported ROOT histogram SQL type " +
            vector.GetType().ToString());
    }

    FlatVector::Validity(vector)
        .SetValid(row);
}

} // namespace duckdb::rootlake
