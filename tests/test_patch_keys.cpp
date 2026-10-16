// The patch window's single keys, as Clavia's editor v1.62 binds them (CPatchView::InitializeShortcutKeys @000fafd8:
// KeySelectVariation1..8 and KeySelectSlotA..D on key down, no modifier; manual p115-116).
#include <catch2/catch_test_macros.hpp>

#include "PatchKeys.h"

#include <array>

using namespace g2ui;
using Kind = PatchKey::Kind;

TEST_CASE("patch keys: 1 to 8 select the variation, alone and once per press")
{
    for (int i = 0; i < 8; ++i) {
        const auto k = patchKey(static_cast<char>('1' + i), false, false, false);
        CHECK(k.kind == Kind::Variation);
        CHECK(k.index == i);
    }
    CHECK(patchKey('9', false, false, false).kind == Kind::None);
    CHECK(patchKey('0', false, false, false).kind == Kind::None);
    CHECK(patchKey('1', true, false, false).kind == Kind::None);  // with a modifier: not this key
    CHECK(patchKey('1', false, true, false).kind == Kind::None);  // the key's auto-repeat
}

TEST_CASE("patch keys: A to D select a performance's slot")
{
    CHECK(patchKey('a', false, false, true).kind == Kind::Slot);
    CHECK(patchKey('a', false, false, true).index == 0);
    CHECK(patchKey('D', false, false, true).index == 3);
    CHECK(patchKey('e', false, false, true).kind == Kind::None);
    CHECK(patchKey('b', false, false, false).kind == Kind::None); // a patch has no slots
    CHECK(patchKey('c', true, false, true).kind == Kind::None);
}

TEST_CASE("patch keys: a disabled slot is not selected")
{
    // KeySelectSlot @000f992e selects a slot only if it is enabled (found by the audit pass of 2026-10-16).
    const std::array<bool, 4> abOnly{true, true, false, false};
    CHECK(patchKey('b', false, false, true, abOnly).kind == Kind::Slot);
    CHECK(patchKey('c', false, false, true, abOnly).kind == Kind::None);
    CHECK(patchKey('d', false, false, true, abOnly).kind == Kind::None);
}

TEST_CASE("slot buttons: a click focuses, Shift-click turns the slot on or off, the morph modifier assigns the keyboard")
{
    // Audit L6, manual p58: "You can activate/deactivate several Slots ... by Shift-clicking on the corresponding Slot
    // buttons ... to change the Keyboard Assign settings, Ctrl-click the Slot button(s)". The manual's Ctrl is Option
    // on the Mac, as in the key table (CShortcutKeys::Add turns modifier 5 into Option).
    CHECK(slotClick(false, false) == SlotClick::Focus);
    CHECK(slotClick(true, false) == SlotClick::ToggleActive);
    CHECK(slotClick(false, true) == SlotClick::ToggleKeyboard);
    CHECK(slotClick(true, true) == SlotClick::ToggleActive);
}
