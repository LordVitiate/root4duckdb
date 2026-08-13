#include "TestEvent.hpp"

TestHit::TestHit(Double_t value) : u(static_cast<Float_t>(value)) {
    uv[0] = static_cast<Float_t>(value);
    uv[1] = static_cast<Float_t>(value + 0.5);
}

ClassImp(TestBase);
ClassImp(TestHit);
ClassImp(TestEvent);
