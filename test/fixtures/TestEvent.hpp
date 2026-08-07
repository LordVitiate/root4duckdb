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

    TestHit() = default;
    explicit TestHit(Double_t value) : u(static_cast<Float_t>(value)) {
        uv[0] = static_cast<Float_t>(value);
        uv[1] = static_cast<Float_t>(value + 0.5);
    }

    ClassDef(TestHit, 2);
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
