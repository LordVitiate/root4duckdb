#include "TestEvent.hpp"

#include "TFile.h"
#include "TTree.h"

#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void WriteFixture(const fs::path& path, Int_t run_base, const std::vector<std::vector<double>>& events,
                         Int_t split_level = 99) {
    TFile file(path.string().c_str(), "RECREATE");
    if (file.IsZombie()) {
        throw std::runtime_error("Cannot create fixture ROOT file: " + path.string());
    }

    TTree tree("Events", "ROOT4DuckDB integration fixture");
    auto* event = new TestEvent();
    auto* branch = tree.Branch("TestEvent", "TestEvent", &event, 32000, split_level);
    if (!branch) {
        delete event;
        throw std::runtime_error("Cannot create TestEvent object branch");
    }
    branch->SetBasketSize(256);

    Int_t event_index = 0;
    for (const auto& values : events) {
        event->run = run_base + event_index;
        event->flags = static_cast<UChar_t>(event_index % 3);
        event->signed_code = static_cast<Char_t>(event_index - 1);
        event->big_signed = static_cast<Long64_t>(9007199254740993LL + event_index);
        event->big_unsigned = static_cast<ULong64_t>(9007199254741993ULL + static_cast<ULong64_t>(event_index));
        event->compressed_float = static_cast<Float_t>(1.25 + event_index);
        event->compressed_double = 100.125 + event_index;
        event->inherited = 9000 + event_index;
        event->point.x = 1000.0 + event_index;
        event->point.y = 2000.0 + event_index;
        event->custom.tag = 700 + event_index;
        event->custom.score = 3000.5 + event_index;
        event->vertex[0] = static_cast<Float_t>(event_index * 10.0 + 0.0);
        event->vertex[1] = static_cast<Float_t>(event_index * 10.0 + 1.0);
        event->vertex[2] = static_cast<Float_t>(event_index * 10.0 + 2.0);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 3; ++col) {
                event->matrix[row][col] = static_cast<Float_t>(event_index * 10.0 + row * 3 + col);
            }
        }
        event->mapScore.clear();
        event->mapScore.emplace(static_cast<Double_t>(event_index) + 0.25, static_cast<Double_t>(event->run));
        event->mapScore.emplace(static_cast<Double_t>(event_index) + 0.75, static_cast<Double_t>(event->run) + 0.5);
        event->setCode = {event_index, event_index + 10};
        event->nestedPairs = {{{static_cast<Double_t>(event_index) + 1.5, 100 + event_index},
                               {static_cast<Double_t>(event_index) + 2.5, 200 + event_index}},
                              {{static_cast<Double_t>(event_index) + 3.5, 300 + event_index}}};
        event->vecLayer.clear();
        event->vecLayer.resize(2);
        for (size_t layer_index = 0; layer_index < event->vecLayer.size(); ++layer_index) {
            auto& layer = event->vecLayer[layer_index];
            layer.layer_id = static_cast<Int_t>(layer_index);
            layer.groups.resize(layer_index + 1);
            for (size_t group_index = 0; group_index < layer.groups.size(); ++group_index) {
                auto& group = layer.groups[group_index];
                group.group_id = static_cast<Int_t>(group_index);
                group.hits.emplace_back(static_cast<Double_t>(event_index * 100 + layer_index * 10 + group_index));
                group.hits.back().refs = {static_cast<Short_t>(event_index + layer_index + group_index),
                                          static_cast<Short_t>(10 + event_index + layer_index + group_index)};
            }
        }
        ++event_index;
        event->vecHit.clear();
        size_t hit_index = 0;
        for (double value : values) {
            event->vecHit.emplace_back(value);
            auto& hit = event->vecHit.back();
            hit.kind = (hit_index % 2) == 0 ? TestKind::PRIMARY : TestKind::SECONDARY;
            hit.exact_id = static_cast<Long64_t>(9007199254740993LL + static_cast<Long64_t>(hit_index));
            hit.exact_uid =
                static_cast<ULong64_t>(9007199254741993ULL + static_cast<ULong64_t>(hit_index));
            hit.compressed = static_cast<Float_t>(value);
            hit.compressed_wide = value + 0.125;
            if (value == value) {
                hit.weights = {static_cast<Float_t>(value + 0.25), static_cast<Float_t>(value + 0.75)};
                hit.labels = {static_cast<Int_t>(value), static_cast<Int_t>(value + 100.0)};
                if ((hit_index % 2) == 0) {
                    hit.refs = {static_cast<Short_t>(value), static_cast<Short_t>(value + 10.0)};
                } else {
                    hit.refs = {static_cast<Short_t>(value)};
                }
            }
            ++hit_index;
        }
        tree.Fill();
    }

    tree.Write();
    file.Close();
    delete event;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: make_fixture <output-directory>\n";
        return 2;
    }
    try {
        const fs::path output(argv[1]);
        fs::create_directories(output);
        WriteFixture(output / "a.root", 100, {{5.0, 12.0, 7.0, 18.0}, {}, {11.0}});
        WriteFixture(output / "b.root", 200, {{9.0, 20.0}, {10.0, 10.5}});
        WriteFixture(output / "c.root", 300, {{std::numeric_limits<double>::quiet_NaN(), 0.1}});
        WriteFixture(output / "packed.root", 400, {{1.0, 2.0}, {3.0}}, 0);
        // split_level=1 keeps TestHit fields inside the persistent vecHit ancestor
        // branch. This reproduces Phast layouts where no physical leaf exists for u/uv.
        WriteFixture(output / "ancestor.root", 500, {{1.0, 2.0}, {3.0}}, 1);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
