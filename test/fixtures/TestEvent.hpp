#pragma once

#include "Rtypes.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

class TestBase {
  public:
    Int_t inherited = 0;
    ClassDef(TestBase, 1);
};

class TestHit {
  public:
    Float_t u = 0.0F;
    Float_t uv[2] = {0.0F, 0.0F};
    // Variable-width member before refs exercises member-wise prefix handling.
    std::vector<Float_t> weights;
    std::vector<Short_t> refs;

    TestHit() = default;
    explicit TestHit(Double_t value);

    ClassDef(TestHit, 3);
};

class TestEvent : public TestBase {
  public:
    Int_t run = 0;
    UChar_t flags = 0;
    Char_t signed_code = 0;
    Float_t vertex[3] = {0.0F, 0.0F, 0.0F};
    Float_t matrix[2][3] = {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}};
    std::vector<TestHit> vecHit;
    std::map<Double_t, Double_t> mapScore;
    std::set<Int_t> setCode;
    std::vector<std::vector<std::pair<Double_t, Int_t>>> nestedPairs;

    ClassDef(TestEvent, 5);
};
