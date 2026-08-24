#include "LegacyTestEvent.hpp"

TestHit::TestHit(Double_t value) : legacy_prefix(1234), u(static_cast<Float_t>(value)) {
    uv[0] = static_cast<Float_t>(value);
    uv[1] = static_cast<Float_t>(value + 0.5);
}

ClassImp(TestHit);
ClassImp(TestEvent);
