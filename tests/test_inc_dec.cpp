// The inc/dec pair under a knob, as Clavia's editor v1.62 draws and hits it (CABCKnobGUI::DrawKnobUpDowns
// @0007bfee; the knobs' MapToTargetArea @000cc1d4 small, @000cc0f6 medium, @000cc018 big; the held pair's
// CPnlControl::UpDownOnClickHandler @000c8970 and UpDownIdleHandler @000c8ae4). Positions are relative to the
// knob's place on the panel.
#include <catch2/catch_test_macros.hpp>

#include "IncDec.h"

using namespace g2ui;

TEST_CASE("inc/dec: which knobs have the pair")
{
    CHECK(incDecKnob("Small") == IncDecKnob::Small);
    CHECK(incDecKnob("Medium") == IncDecKnob::Medium);
    CHECK(incDecKnob("Big") == IncDecKnob::Big);
    // Sliders are not knobs here (the original's vertical slider has its own pair, not drawn yet).
    CHECK_FALSE(incDecKnob("Slider"));
    CHECK_FALSE(incDecKnob("SeqSlider"));
}

TEST_CASE("inc/dec: a 21 x 9 pair below the knob, dec on the left, the halves overlapping by a pixel")
{
    const auto small = incDecPair(IncDecKnob::Small);
    CHECK(small.dec == IncDecRect{0, 14, 11, 9});
    CHECK(small.inc == IncDecRect{10, 14, 11, 9});
    CHECK(incDecPair(IncDecKnob::Medium).dec == IncDecRect{1, 16, 11, 9});
    CHECK(incDecPair(IncDecKnob::Medium).inc == IncDecRect{11, 16, 11, 9});
    CHECK(incDecPair(IncDecKnob::Big).dec == IncDecRect{1, 18, 11, 9});
    CHECK(incDecPair(IncDecKnob::Big).inc == IncDecRect{11, 18, 11, 9});
}

TEST_CASE("inc/dec: what a click hits in the knob's control")
{
    // Small: 21 x 23, the pair from y 14, dec where x < 10.
    CHECK(incDecAt(IncDecKnob::Small, 3, 13) == IncDecPart::None); // the knob itself
    CHECK(incDecAt(IncDecKnob::Small, 3, 14) == IncDecPart::Dec);
    CHECK(incDecAt(IncDecKnob::Small, 9, 22) == IncDecPart::Dec);
    CHECK(incDecAt(IncDecKnob::Small, 10, 20) == IncDecPart::Inc);
    CHECK(incDecAt(IncDecKnob::Small, 20, 22) == IncDecPart::Inc);
    CHECK(incDecAt(IncDecKnob::Small, 21, 20) == IncDecPart::None); // outside
    CHECK(incDecAt(IncDecKnob::Small, 5, 23) == IncDecPart::None);
    // Medium: 23 x 25, from y 16, dec where x < 11. Big: 23 x 27, from y 18.
    CHECK(incDecAt(IncDecKnob::Medium, 10, 16) == IncDecPart::Dec);
    CHECK(incDecAt(IncDecKnob::Medium, 11, 24) == IncDecPart::Inc);
    CHECK(incDecAt(IncDecKnob::Medium, 11, 15) == IncDecPart::None);
    CHECK(incDecAt(IncDecKnob::Big, 10, 18) == IncDecPart::Dec);
    CHECK(incDecAt(IncDecKnob::Big, 22, 26) == IncDecPart::Inc);
    CHECK(incDecAt(IncDecKnob::Big, 22, 17) == IncDecPart::None);
    CHECK(incDecControlSize(IncDecKnob::Big) == IncDecRect{0, 0, 23, 27});
}

TEST_CASE("inc/dec: held, it steps again after 400 ms, then every 70 ms")
{
    CHECK(kIncDecFirstRepeatMs == 400);
    CHECK(kIncDecRepeatMs == 70);
}
