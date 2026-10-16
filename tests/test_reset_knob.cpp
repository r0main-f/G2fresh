// The knobs with a reset triangle ("Reset", "Reset/medium" in the panels), as Clavia's editor v1.62 draws and hits
// them (CPnlIndicatorKnob: its constructor @000cc346, MapToTargetArea @000cc4b8, Draw @000cc686, UpdateGraphics
// @000cdc6c; CPnlKnobSliderABC::OnClick @000cb9b0, ResetParamValue @000cb92e, ResetMorphValue @000cb960).
#include <catch2/catch_test_macros.hpp>

#include "IncDec.h"
#include "ResetKnob.h"

using namespace g2ui;

TEST_CASE("reset knob: a strip on top, the knob below it")
{
    // The constructor: medium 6 high, 6 in from each side; small 5 and 5. The control is the knob plus the strip.
    CHECK_FALSE(resetStrip("Small"));
    CHECK_FALSE(resetStrip("Medium"));
    REQUIRE(resetStrip("Reset"));
    CHECK(*resetStrip("Reset") == IncDecRect{5, 0, 21 - 10, 5});
    REQUIRE(resetStrip("Reset/medium"));
    CHECK(*resetStrip("Reset/medium") == IncDecRect{6, 0, 23 - 12, 6});
    CHECK(knobTop("Reset") == 5);
    CHECK(knobTop("Reset/medium") == 6);
    CHECK(knobTop("Medium") == 0);
}

TEST_CASE("reset knob: the triangle's hit area")
{
    // MapToTargetArea: the strip first (area 4), then the knob and its pair below it.
    CHECK(onResetStrip("Reset/medium", 11, 2));
    CHECK_FALSE(onResetStrip("Reset/medium", 2, 2)); // left of the strip
    CHECK_FALSE(onResetStrip("Reset/medium", 11, 6)); // the knob
    CHECK_FALSE(onResetStrip("Medium", 11, 2));
}

TEST_CASE("reset knob: the centre value, and the lit triangle")
{
    // ResetParamValue: (max + 1) >> 1; UpdateGraphics lights the triangle at that value.
    CHECK(resetValue(127) == 64);
    CHECK(resetValue(2) == 1);
    CHECK(resetValue(1) == 1);
    CHECK(resetLit(64, 127));
    CHECK_FALSE(resetLit(63, 127));
}
