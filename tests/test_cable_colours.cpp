// The original's cable colours (Clavia's editor v1.62, the CRGBColor table in Global.c, enum 0..6, and the
// highlight colour of a held jack's chain), which the Classic look draws.
#include <catch2/catch_test_macros.hpp>

#include "CableColours.h"

using namespace g2ui;

TEST_CASE("original cable colours: red, blue, yellow, orange, green, purple, white and the highlight")
{
    CHECK(originalCableRgb(g2::CableColor::Red) == Rgb{255, 90, 90});
    CHECK(originalCableRgb(g2::CableColor::Blue) == Rgb{100, 100, 255});
    CHECK(originalCableRgb(g2::CableColor::Yellow) == Rgb{230, 230, 80});
    CHECK(originalCableRgb(g2::CableColor::Orange) == Rgb{255, 192, 80});
    CHECK(originalCableRgb(g2::CableColor::Green) == Rgb{80, 210, 80});
    CHECK(originalCableRgb(g2::CableColor::Purple) == Rgb{200, 0, 230});
    CHECK(originalCableRgb(g2::CableColor::White) == Rgb{255, 255, 255});
    CHECK(kOriginalHighlightRgb == Rgb{155, 255, 255});
}
