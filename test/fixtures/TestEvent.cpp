#include "TestEvent.hpp"

#include "TBuffer.h"

TestHit::TestHit(Double_t value) : u(static_cast<Float_t>(value)) {
    uv[0] = static_cast<Float_t>(value);
    uv[1] = static_cast<Float_t>(value + 0.5);
}

void TestCustom::Streamer(TBuffer& buffer) {
    UInt_t start = 0;
    UInt_t count = 0;
    if (buffer.IsReading()) {
        const auto version = buffer.ReadVersion(&start, &count);
        buffer >> tag;
        buffer >> score;
        buffer.CheckByteCount(start, count, TestCustom::IsA());
        (void)version;
        return;
    }
    start = buffer.WriteVersion(TestCustom::IsA(), kTRUE);
    buffer << tag;
    buffer << score;
    buffer.SetByteCount(start, kTRUE);
}

ClassImp(TestBase);
ClassImp(TestHit);
ClassImp(TestGroup);
ClassImp(TestLayer);
ClassImp(TestPoint);
ClassImp(TestCustom);
ClassImp(TestEvent);
