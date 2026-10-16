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

TEST_CASE("init patches: two files beside the settings, InitPatch1 and InitPatch2")
{
    // Audit N4: CEditorApp saves the top patch as "InitPatch" + n in the preferences folder (Save InitPatch 1/2) and
    // recalls it from there (the toolbar's Init 1 and Init 2; manual p59, p99). G2fresh adds .pch2.
    CHECK(initPatchFileName(1) == "InitPatch1.pch2");
    CHECK(initPatchFileName(2) == "InitPatch2.pch2");
}

TEST_CASE("patch keys: V shows the Voice Area, F the FX Area")
{
    // InitializeShortcutKeys @000fafd8 binds them as the variations' keys (KeyShowPolyPart, KeyShowMonoPart; manual
    // p61-62, p115), in a patch as in a performance.
    CHECK(patchKey('v', false, false, false).kind == Kind::ShowVoice);
    CHECK(patchKey('V', false, false, true).kind == Kind::ShowVoice);
    CHECK(patchKey('f', false, false, false).kind == Kind::ShowFx);
    CHECK(patchKey('f', true, false, false).kind == Kind::None);
    CHECK(patchKey('v', false, true, false).kind == Kind::None);
}

TEST_CASE("patch keys: R starts or stops a performance's master clock")
{
    // CPatchView::KeyToggleMasterClock @000fc64a: the performance header's Run, only in a performance.
    CHECK(patchKey('r', false, false, true).kind == Kind::ToggleClock);
    CHECK(patchKey('r', false, false, false).kind == Kind::None);
    CHECK(patchKey('r', true, false, true).kind == Kind::None);
}

TEST_CASE("patch keys: the Mutator's, while it is open")
{
    // CPatchView::InitializeShortcutKeys @000fafd8 binds them as the variations' keys; each acts only with the Mutator
    // open (gFloater). The letters are read from the original's key codes (its table runs Q=0x22 .. P, A=0x30 .. L,
    // Z=0x3e .. M, as the known F, V, L, M, R, Z and A-D show).
    CHECK(mutatorKey('u', false, false) == MutatorKey::Mutate);         // KeyMutaMutate (0x28)
    CHECK(mutatorKey('n', false, false) == MutatorKey::Randomize);      // KeyMutaRandomize (0x43)
    CHECK(mutatorKey('i', false, false) == MutatorKey::Interpolate);    // KeyMutaInterpolate (0x29)
    CHECK(mutatorKey('x', false, false) == MutatorKey::Cross);          // KeyMutaCross (0x3f)
    CHECK(mutatorKey('s', false, false) == MutatorKey::StoreChild);     // KeyMutaStoreChild (0x31)
    CHECK(mutatorKey('o', false, false) == MutatorKey::Mother);         // KeyMutaAssignMother (0x2a)
    CHECK(mutatorKey('t', false, false) == MutatorKey::Father);         // KeyMutaAssignFather (0x26)
    CHECK(mutatorKey('E', false, false) == MutatorKey::MutateCurrent);  // KeyMutaMutateCurrent (0x24)
    CHECK(mutatorKey('u', true, false) == MutatorKey::None);
    CHECK(mutatorKey('u', false, true) == MutatorKey::None);
    CHECK(mutatorKey('a', false, false) == MutatorKey::None);
}

TEST_CASE("patch keys: the original's command shortcuts G2fresh has the functions for")
{
    // Manual p116: Ctrl(Mac: Cmd)+P the Patch Settings, +R the Performance Settings, +J the Textpad (Patch Notes),
    // +U upload the active slot from the G2.
    CHECK(commandKey('p', false) == CommandKey::PatchSettings);
    CHECK(commandKey('R', false) == CommandKey::PerformanceSettings);
    CHECK(commandKey('j', false) == CommandKey::Textpad);
    CHECK(commandKey('u', false) == CommandKey::UploadSlot);
    CHECK(commandKey('p', true) == CommandKey::None); // with Shift: not these
    CHECK(commandKey('q', false) == CommandKey::None);
}

TEST_CASE("File > New To, Open To, Save From: a slot of the performance, or of the connected G2")
{
    // Manual p98-99 (SMNU File items 110, 107, 113): New To clears a slot for a new patch, Open To opens a file into a
    // slot, Save From saves a slot's patch. G2fresh has one document: in a performance they act on its slots; with a
    // patch, on the connected G2's (the new or opened patch is sent there; Save From saves what the G2 has).
    CHECK(slotTarget(true, false) == SlotTarget::Performance);
    CHECK(slotTarget(true, true) == SlotTarget::Performance);
    CHECK(slotTarget(false, true) == SlotTarget::Synth);
    CHECK(slotTarget(false, false) == SlotTarget::None);
}
