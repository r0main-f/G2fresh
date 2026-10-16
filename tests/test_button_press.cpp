// When a panel button acts: on mouse-down, as Clavia's editor v1.62 does. CPnlButtonABC::OnClick @000c3520 ->
// OnClickHandler @000c341a asks its panel whether the button is on and requests the other value at once; Release
// @000c355e only ends the press (CPanel::CtrlRelease). Knobs act by dragging; a mode selector opens its menu.
#include <catch2/catch_test_macros.hpp>

#include "ButtonPress.h"

using namespace g2ui;

TEST_CASE("panel buttons act when the mouse goes down")
{
    for (const char* kind : {"ButtonText", "TextEdit", "ButtonFlat", "LevelShift", "ButtonRadio", "ButtonRadioEdit", "ButtonIncDec"})
        CHECK(actsOnPress(kind));
    CHECK_FALSE(actsOnPress("Knob"));
    CHECK_FALSE(actsOnPress("PartSelector"));
    CHECK_FALSE(actsOnPress("Input"));
}
