#include "root4duckdb/histogram/root_histogram_internal.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb::rootlake::histogram_detail {

RootScalarActual Null(const LogicalType& type) {
    return RootScalarActual::Null(type);
}

RootScalarActual Str(const std::string& value) {
    return RootScalarActual::String(value);
}

RootScalarActual Int(int64_t value) {
    return RootScalarActual::Signed(value, LogicalType(LogicalTypeId::INTEGER));
}

RootScalarActual UInt(uint64_t value) {
    return RootScalarActual::Unsigned(value, LogicalType(LogicalTypeId::UINTEGER));
}

RootScalarActual UBig(uint64_t value) {
    return RootScalarActual::Unsigned(value, LogicalType(LogicalTypeId::UBIGINT));
}

RootScalarActual Bool(bool value) {
    return RootScalarActual::Numeric(LogicalType(LogicalTypeId::BOOLEAN), value ? 1.0 : 0.0);
}

RootScalarActual Double(double value) {
    return RootScalarActual::Numeric(LogicalType(LogicalTypeId::DOUBLE), value);
}

bool IsProfile(const TH1& histogram) {
    return dynamic_cast<const TProfile*>(&histogram) || dynamic_cast<const TProfile2D*>(&histogram) ||
           dynamic_cast<const TProfile3D*>(&histogram);
}

TAxis* Axis(TH1& histogram, int axis) {
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

const char* AxisName(int axis) {
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

bool AxisHasLabels(TAxis& axis) {
    for (int bin = 1; bin <= axis.GetNbins(); ++bin) {
        const char* label = axis.GetBinLabel(bin);
        if (label && *label) {
            return true;
        }
    }

    return false;
}

bool ProfileBinData(TH1& histogram, int global_bin, double& entries, double& effective_entries, double& bin_sumw2,
                    bool& has_bin_sumw2, std::string& error_option) {
    if (auto* profile = dynamic_cast<TProfile*>(&histogram)) {
        entries = profile->GetBinEntries(global_bin);
        effective_entries = profile->GetBinEffectiveEntries(global_bin);

        const auto* sumw2 = profile->GetBinSumw2();
        has_bin_sumw2 = sumw2 && global_bin >= 0 && global_bin < sumw2->GetSize();

        if (has_bin_sumw2) {
            bin_sumw2 = sumw2->At(global_bin);
        }

        const auto* option = profile->GetErrorOption();
        error_option = option ? option : "";
        return true;
    }

    if (auto* profile = dynamic_cast<TProfile2D*>(&histogram)) {
        entries = profile->GetBinEntries(global_bin);
        effective_entries = profile->GetBinEffectiveEntries(global_bin);

        const auto* sumw2 = profile->GetBinSumw2();
        has_bin_sumw2 = sumw2 && global_bin >= 0 && global_bin < sumw2->GetSize();

        if (has_bin_sumw2) {
            bin_sumw2 = sumw2->At(global_bin);
        }

        const auto* option = profile->GetErrorOption();
        error_option = option ? option : "";
        return true;
    }

    if (auto* profile = dynamic_cast<TProfile3D*>(&histogram)) {
        entries = profile->GetBinEntries(global_bin);
        effective_entries = profile->GetBinEffectiveEntries(global_bin);

        const auto* sumw2 = profile->GetBinSumw2();
        has_bin_sumw2 = sumw2 && global_bin >= 0 && global_bin < sumw2->GetSize();

        if (has_bin_sumw2) {
            bin_sumw2 = sumw2->At(global_bin);
        }

        const auto* option = profile->GetErrorOption();
        error_option = option ? option : "";
        return true;
    }

    return false;
}

std::string ProfileErrorOption(TH1& histogram) {
    double entries = 0;
    double effective = 0;
    double sumw2 = 0;
    bool has_sumw2 = false;
    std::string option;

    ProfileBinData(histogram, 0, entries, effective, sumw2, has_sumw2, option);

    return option;
}

std::string FunctionList(TH1& histogram) {
    auto* functions = histogram.GetListOfFunctions();

    if (!functions) {
        return {};
    }

    std::ostringstream output;
    TIter next(functions);

    bool first = true;

    while (auto* object = dynamic_cast<TObject*>(next())) {
        if (!first) {
            output << ";";
        }

        first = false;

        output << object->GetName() << ":" << object->ClassName();
    }

    return output.str();
}

int FunctionCount(TH1& histogram) {
    auto* functions = histogram.GetListOfFunctions();
    return functions ? functions->GetSize() : 0;
}

void MaterializeBinRow(const RootHistogramBinding& binding, TH1& histogram, uint64_t row,
                       std::vector<RootScalarActual>& values) {
    values.clear();
    values.reserve(binding.schema.names.size());

    const int global_bin = static_cast<int>(row);

    int x_bin = 0;
    int y_bin = 0;
    int z_bin = 0;

    histogram.GetBinXYZ(global_bin, x_bin, y_bin, z_bin);

    const int dimension = histogram.GetDimension();

    auto* x = histogram.GetXaxis();
    auto* y = histogram.GetYaxis();
    auto* z = histogram.GetZaxis();

    const bool x_underflow = x_bin == 0;
    const bool x_overflow = x_bin == x->GetNbins() + 1;

    const bool y_underflow = dimension >= 2 && y_bin == 0;

    const bool y_overflow = dimension >= 2 && y_bin == y->GetNbins() + 1;

    const bool z_underflow = dimension >= 3 && z_bin == 0;

    const bool z_overflow = dimension >= 3 && z_bin == z->GetNbins() + 1;

    const bool underflow = x_underflow || y_underflow || z_underflow;

    const bool overflow = x_overflow || y_overflow || z_overflow;

    values.push_back(Str(binding.object_path));
    values.push_back(Str(histogram.GetName()));
    values.push_back(Str(histogram.GetTitle()));
    values.push_back(Str(binding.class_name));
    values.push_back(Int(dimension));

    values.push_back(Int(global_bin));
    values.push_back(Int(x_bin));

    values.push_back(dimension >= 2 ? Int(y_bin) : Null(binding.schema.types[7]));

    values.push_back(dimension >= 3 ? Int(z_bin) : Null(binding.schema.types[8]));

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

    values.push_back(Double(histogram.GetBinContent(global_bin)));

    values.push_back(Double(histogram.GetBinError(global_bin)));

    values.push_back(Double(histogram.GetBinErrorLow(global_bin)));

    values.push_back(Double(histogram.GetBinErrorUp(global_bin)));

    if (histogram.GetSumw2N() > global_bin) {
        values.push_back(Double(histogram.GetSumw2()->At(global_bin)));
    } else {
        values.push_back(Null(binding.schema.types[30]));
    }

    double profile_entries = 0;
    double profile_effective = 0;
    double profile_bin_sumw2 = 0;
    bool has_profile_bin_sumw2 = false;
    std::string profile_error_option;

    const bool profile = ProfileBinData(histogram, global_bin, profile_entries, profile_effective, profile_bin_sumw2,
                                        has_profile_bin_sumw2, profile_error_option);

    values.push_back(profile ? Double(profile_entries) : Null(binding.schema.types[31]));

    values.push_back(profile ? Double(profile_effective) : Null(binding.schema.types[32]));

    values.push_back(profile && has_profile_bin_sumw2 ? Double(profile_bin_sumw2) : Null(binding.schema.types[33]));

    values.push_back(profile ? Str(profile_error_option) : Null(binding.schema.types[34]));

    values.push_back(Double(histogram.GetEntries()));

    values.push_back(Double(histogram.GetEffectiveEntries()));

    values.push_back(Double(histogram.GetSumOfWeights()));

    values.push_back(Bool(x_underflow));
    values.push_back(Bool(x_overflow));

    values.push_back(dimension >= 2 ? Bool(y_underflow) : Null(binding.schema.types[40]));

    values.push_back(dimension >= 2 ? Bool(y_overflow) : Null(binding.schema.types[41]));

    values.push_back(dimension >= 3 ? Bool(z_underflow) : Null(binding.schema.types[42]));

    values.push_back(dimension >= 3 ? Bool(z_overflow) : Null(binding.schema.types[43]));
}

void MaterializeAxisRow(const RootHistogramBinding& binding, TH1& histogram, uint64_t row,
                        std::vector<RootScalarActual>& values) {
    values.clear();
    values.reserve(binding.schema.names.size());

    const int axis_index = static_cast<int>(row) + 1;

    auto* axis = Axis(histogram, axis_index);

    if (!axis) {
        throw InternalException("Invalid ROOT histogram axis index");
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

    const auto* bins = axis->GetXbins();

    values.push_back(Bool(bins && bins->GetSize() > 0));

    values.push_back(Bool(axis->GetTimeDisplay()));

    values.push_back(Str(axis->GetTimeFormat()));

    values.push_back(Bool(AxisHasLabels(*axis)));

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

void MaterializeMetaRow(const RootHistogramBinding& binding, TH1& histogram, std::vector<RootScalarActual>& values) {
    values.clear();
    values.reserve(binding.schema.names.size());

    if (binding.schema.types.size() != 58) {
        throw InternalException("ROOT histogram meta schema must contain 58 columns");
    }

    constexpr idx_t PROFILE_ERROR_OPTION_COLUMN = 54;
    constexpr idx_t Y_NBINS_COLUMN = 56;
    constexpr idx_t Z_NBINS_COLUMN = 57;

    const int dimension = histogram.GetDimension();

    values.push_back(Str(binding.object_path));
    values.push_back(Str(histogram.GetName()));
    values.push_back(Str(histogram.GetTitle()));
    values.push_back(Str(binding.class_name));
    values.push_back(Int(dimension));
    values.push_back(Bool(binding.is_profile));

    values.push_back(Double(histogram.GetEntries()));
    values.push_back(Double(histogram.GetEffectiveEntries()));
    values.push_back(Double(histogram.GetSumOfWeights()));
    values.push_back(UBig(static_cast<uint64_t>(histogram.GetNcells())));

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

    values.push_back(Int(static_cast<int>(histogram.GetBinErrorOption())));

    values.push_back(Bool(histogram.GetStatOverflows()));

    values.push_back(Bool(histogram.CanExtendAllAxes()));

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

    values.push_back(binding.is_profile ? Str(ProfileErrorOption(histogram))
                                        : Null(binding.schema.types[PROFILE_ERROR_OPTION_COLUMN]));

    values.push_back(Int(histogram.GetNbinsX()));

    values.push_back(dimension >= 2 ? Int(histogram.GetNbinsY()) : Null(binding.schema.types[Y_NBINS_COLUMN]));

    values.push_back(dimension >= 3 ? Int(histogram.GetNbinsZ()) : Null(binding.schema.types[Z_NBINS_COLUMN]));
}

} // namespace duckdb::rootlake::histogram_detail
