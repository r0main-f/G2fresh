// How cables are shown, as Clavia's editor v1.62 does it (fidelity audit C7-C9): the cable style option, Shake, Hide
// All and its keys. Statements from the decompiled code.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CableView.h"

#include <array>

using namespace g2ui;

TEST_CASE("cable style: four styles, Curved 3D unless a valid other one is stored")
{
    // CCableRenderer::MakeNewCable @0001a706: 0 Straight 3D, 1 Curved 3D, 2 Straight Thin, 3 Curved Thin.
    // CPrefManager @0006efea defaults to 1; ReadPrefs @0006e600 / SetCableStyle @0006e5bc read an unknown value as 1.
    CHECK(cableStyleFromSetting(0) == CableStyle::Straight3D);
    CHECK(cableStyleFromSetting(1) == CableStyle::Curved3D);
    CHECK(cableStyleFromSetting(2) == CableStyle::StraightThin);
    CHECK(cableStyleFromSetting(3) == CableStyle::CurvedThin);
    CHECK(cableStyleFromSetting(-1) == CableStyle::Curved3D);
    CHECK(cableStyleFromSetting(7) == CableStyle::Curved3D);
    CHECK(kDefaultCableStyle == CableStyle::Curved3D);
}

TEST_CASE("cable style: curved styles can be shaken; 3D cables are 3 px, thin ones 1.01 px")
{
    // CCableRenderer::IsCurrentStyleShakeable @00019a16; k3DCableWidth, kSlimCableWidth (GetCableWidth @001a875a).
    CHECK(isCurved(CableStyle::Curved3D));
    CHECK(isCurved(CableStyle::CurvedThin));
    CHECK_FALSE(isCurved(CableStyle::Straight3D));
    CHECK_FALSE(isCurved(CableStyle::StraightThin));
    CHECK(cableWidth(CableStyle::Curved3D) == Catch::Approx(3.0f));
    CHECK(cableWidth(CableStyle::Straight3D) == Catch::Approx(3.0f));
    CHECK(cableWidth(CableStyle::CurvedThin) == Catch::Approx(1.01f));
    CHECK(cableWidth(CableStyle::StraightThin) == Catch::Approx(1.01f));
}

TEST_CASE("hide all: a cable shows when cables are shown and so is its colour; a highlighted one always")
{
    // CPatch::HandlePatchHeaderDump @000dab9c: AreCablesVisible && IsCabelColorVisible(c); CCableRenderer::Render
    // @0001a1ce draws a highlighted cable whatever its visibility.
    CHECK(cableShown(true, true, false));
    CHECK_FALSE(cableShown(false, true, false));
    CHECK_FALSE(cableShown(true, false, false));
    CHECK(cableShown(false, false, true));
}

TEST_CASE("space hides or shows all cables; Control with it shakes them")
{
    // CPatchView::InitializeShortcutKeys @000fafd8: Space -> KeyToggleShowCables, Control+Space -> KeyShakeCables
    // (the physical Control key, CShortcutKeys::GetMethod @001c81e2); two modifiers read as none.
    CHECK(spaceKey({}) == SpaceAction::HideAll);
    CHECK(spaceKey({.control = true}) == SpaceAction::Shake);
    CHECK(spaceKey({.command = true}) == SpaceAction::None);
    CHECK(spaceKey({.option = true}) == SpaceAction::None);
    CHECK(spaceKey({.shift = true}) == SpaceAction::None);
    CHECK(spaceKey({.control = true, .shift = true}) == SpaceAction::HideAll);
}

TEST_CASE("cable opacity: G2fresh's own setting, opaque unless one of its steps is chosen")
{
    // An addition: the original draws its cables opaque (CCableRenderer::Render @0001a1ce).
    CHECK(kCableOpacities == std::array<int, 4>{100, 75, 50, 25});
    CHECK(cableOpacityFromSetting(100) == 100);
    CHECK(cableOpacityFromSetting(50) == 50);
    CHECK(cableOpacityFromSetting(60) == 100);
    CHECK(cableOpacityFromSetting(0) == 100);
    CHECK(cableAlpha(25) == Catch::Approx(0.25f));
    CHECK(cableAlpha(100) == Catch::Approx(1.0f));
}

TEST_CASE("modern cables: the area's seed varies their sag and lean, so Shake shows there too")
{
    // G2fresh's own hanging cable (the Modern look): Shake gives a new seed (CCableRenderer::Shake @0001a8ec), which
    // here changes the sag between 75% and 125% of its base and the lean of its arms.
    const ModernCurve base = modernCurve(200.0f, 0);
    CHECK(base.sag > 0.0f);
    bool differs = false;
    for (int seed = 0; seed < 256; ++seed) {
        const auto c = modernCurve(200.0f, seed);
        CHECK(c.sag >= 0.75f * 56.0f - 0.01f); // base sag at 200 px: min(90, 12 + 0.22 * 200) = 56
        CHECK(c.sag <= 1.25f * 56.0f + 0.01f);
        CHECK(c.lean >= 0.05f);
        CHECK(c.lean <= 0.25f);
        differs = differs || c.sag != base.sag || c.lean != base.lean;
    }
    CHECK(differs);
    CHECK(modernCurve(200.0f, 17).sag == modernCurve(200.0f, 17).sag); // the same seed, the same cable
    CHECK(modernCurve(1000.0f, 0).sag <= 1.25f * 90.0f + 0.01f);      // the base sag stops at 90 px
}
