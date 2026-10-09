#ifndef ROOT4DUCKDB_TEST_LEGACYTESTEVENT_HPP
#define ROOT4DUCKDB_TEST_LEGACYTESTEVENT_HPP

#include "Rtypes.h"

#include <vector>

class TestHit
{
  public:
    Int_t legacy_prefix = 0;
    Float_t u = 0.0F;
    Float_t uv[2] = {0.0F, 0.0F};

    TestHit() = default;
    explicit TestHit(Double_t value);

    ClassDef(TestHit, 5);
};

class TestEvent
{
  public:
    std::vector<TestHit> vecHit;

    ClassDef(TestEvent, 6);
};

#endif
