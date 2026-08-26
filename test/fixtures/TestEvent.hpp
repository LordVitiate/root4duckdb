#pragma once

#include "Rtypes.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

class TBuffer;

class TestBase {
  public:
    Int_t inherited = 0;
    ClassDef(TestBase, 1);
};

enum class TestKind : Int_t { PRIMARY = 0, SECONDARY = 1 };

class TestHit {
  public:
    Float_t u = 0.0F;
    Float_t uv[2] = {0.0F, 0.0F};
    TestKind kind = TestKind::PRIMARY;
    Long64_t exact_id = 0;
    ULong64_t exact_uid = 0;
    Float16_t compressed = 0.0F; //[0,100,16]
    Double32_t compressed_wide = 0.0; //[0,1000,20]
    // Variable-width member before refs exercises member-wise prefix handling.
    std::vector<Float_t> weights;
    std::vector<Short_t> refs;
    std::set<Int_t> labels;

    TestHit() = default;
    explicit TestHit(Double_t value);

    ClassDef(TestHit, 6);
};

class TestGroup {
  public:
    Int_t group_id = 0;
    std::vector<TestHit> hits;

    ClassDef(TestGroup, 1);
};

class TestLayer {
  public:
    Int_t layer_id = 0;
    std::vector<TestGroup> groups;

    ClassDef(TestLayer, 1);
};

class TestPoint {
  public:
    Double_t x = 0.0;
    Double_t y = 0.0;

    ClassDef(TestPoint, 1);
};

class TestCustom {
  public:
    Int_t tag = 0;
    Double_t score = 0.0;

    ClassDef(TestCustom, 1);
};

class TestEvent : public TestBase {
  public:
    Int_t run = 0;
    UChar_t flags = 0;
    Char_t signed_code = 0;
    Long64_t big_signed = 0;
    ULong64_t big_unsigned = 0;
    Float16_t compressed_float = 0.0F; //[0,100,16]
    Double32_t compressed_double = 0.0; //[0,1000,20]
    Float_t vertex[3] = {0.0F, 0.0F, 0.0F};
    Float_t matrix[2][3] = {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}};
    TestPoint point;
    TestCustom custom;
    std::vector<TestHit> vecHit;
    std::vector<TestLayer> vecLayer;
    std::map<Double_t, Double_t> mapScore;
    std::set<Int_t> setCode;
    std::vector<UInt_t> header;
    std::vector<std::vector<std::pair<Double_t, Int_t>>> nestedPairs;

    ClassDef(TestEvent, 8);
};
