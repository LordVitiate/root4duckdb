#include "LegacyTestEvent.hpp"

#include "TFile.h"
#include "TTree.h"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const fs::path output(argv[1]);
    TFile file(output.string().c_str(), "RECREATE");
    if (file.IsZombie()) {
        throw std::runtime_error("Cannot create legacy fixture ROOT file");
    }
    TTree tree("Events", "ROOT4DuckDB schema-evolution fixture");
    auto* event = new TestEvent();
    auto* branch = tree.Branch("TestEvent", "TestEvent", &event, 32000, 1);
    if (!branch) {
        delete event;
        return 1;
    }
    branch->SetBasketSize(256);
    event->vecHit = {TestHit(1.0), TestHit(2.0)};
    tree.Fill();
    event->vecHit = {TestHit(3.0)};
    tree.Fill();
    tree.Write();
    file.Close();
    delete event;
    return 0;
}
