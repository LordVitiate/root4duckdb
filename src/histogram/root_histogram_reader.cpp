#include "root4duckdb/histogram/root_histogram_reader.hpp"

#include "root4duckdb/histogram/root_histogram_internal.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb::rootlake {
namespace {

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string NormalizeObjectPath(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    if (path.front() == '/') {
        return path.substr(1);
    }

    return path;
}

} // namespace

const char* RootHistogramViewName(RootHistogramView view) {
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

bool TryBindRootHistogram(TFile& file, const std::string& requested_path, RootHistogramBinding& binding,
                          std::unique_ptr<TH1>& histogram) {
    std::string path = NormalizeObjectPath(requested_path);

    if (path.empty()) {
        return false;
    }

    RootHistogramView view = RootHistogramView::BINS;

    TObject* object = file.Get(path.c_str());

    auto* root_histogram = dynamic_cast<TH1*>(object);

    if (!root_histogram) {
        struct Suffix {
            const char* text;
            RootHistogramView view;
        };

        static const Suffix suffixes[] = {
            {"/bins", RootHistogramView::BINS}, {"/axes", RootHistogramView::AXES}, {"/meta", RootHistogramView::META}};

        for (const auto& suffix : suffixes) {
            const std::string suffix_text = suffix.text;

            if (!EndsWith(path, suffix_text) || path.size() <= suffix_text.size()) {
                continue;
            }

            const auto object_path = path.substr(0, path.size() - suffix_text.size());

            object = file.Get(object_path.c_str());

            root_histogram = dynamic_cast<TH1*>(object);

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
        throw NotImplementedException("TH2Poly requires a polygon-bin adapter; "
                                      "rectangular TH1/TH2/TH3 flattening is not valid");
    }

    auto* clone = dynamic_cast<TH1*>(root_histogram->Clone());

    if (!clone) {
        throw IOException("Failed to clone ROOT histogram " + requested_path);
    }

    clone->SetDirectory(nullptr);

    histogram.reset(clone);

    binding.object_path = "/" + path;

    binding.class_name = clone->ClassName();

    binding.view = view;

    binding.dimension = clone->GetDimension();

    binding.is_profile = histogram_detail::IsProfile(*clone);

    switch (view) {
    case RootHistogramView::BINS:
        binding.schema = histogram_detail::BinSchema();
        binding.row_count = static_cast<uint64_t>(clone->GetNcells());
        break;

    case RootHistogramView::AXES:
        binding.schema = histogram_detail::AxisSchema();
        binding.row_count = static_cast<uint64_t>(binding.dimension);
        break;

    case RootHistogramView::META:
        binding.schema = histogram_detail::MetaSchema();
        binding.row_count = 1;
        break;
    }

    return true;
}

} // namespace duckdb::rootlake
