#pragma once
// The patch window's single keys, as Clavia's editor v1.62 binds them (CPatchView::InitializeShortcutKeys @000fafd8;
// manual p115-116): 1 to 8 select the variation, A to D a performance's slot, V and F show the Voice or the FX Area
// (SplitBar.h), L learns a MIDI controller and M
// opens the MIDI controller dialog, R runs or stops a performance's
// master clock, on key down and with no modifier.
// Plain C++.

#include <array>
#include <cctype>
#include <string>

namespace g2ui {

struct PatchKey {
    enum class Kind { None, Variation, Slot, ShowVoice, ShowFx, LearnMidi, AssignMidi, ToggleClock };
    Kind kind = Kind::None;
    int index = 0;
};

// `c`: the character typed; `modifier`: any modifier held; `repeat`: the key's auto-repeat; `performance`: the
// document is a performance (only then are there slots to select); `enabled`: which of its slots are enabled (a
// disabled one is not selected, KeySelectSlot @000f992e).
inline PatchKey patchKey(char c, bool modifier, bool repeat, bool performance,
                         std::array<bool, 4> enabled = {true, true, true, true})
{
    if (modifier || repeat)
        return {};
    if (c >= '1' && c <= '8')
        return {PatchKey::Kind::Variation, c - '1'};
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (performance && l >= 'a' && l <= 'd' && enabled[static_cast<std::size_t>(l - 'a')])
        return {PatchKey::Kind::Slot, l - 'a'};
    if (l == 'v')
        return {PatchKey::Kind::ShowVoice};
    if (l == 'f')
        return {PatchKey::Kind::ShowFx};
    if (l == 'l')
        return {PatchKey::Kind::LearnMidi}; // the focused parameter takes the last CC received (KeyLearnMidi @000f99fe)
    if (l == 'm')
        return {PatchKey::Kind::AssignMidi}; // its MIDI controller dialog (KeyAssignMidi @000fa154)
    if (l == 'r' && performance)
        return {PatchKey::Kind::ToggleClock}; // the master clock's Run (KeyToggleMasterClock @000fc64a)
    return {};
}

// The Patch Mutator's keys, while its window is open (CPatchView::KeyMuta*; the letters from the original's key codes):
// U mutates the Mother, N randomizes, I interpolates and X crosses Mother and Father, S stores the focused child in the
// gene bank, O and T make the current variation the Mother or the Father, E mutates the current variation.
enum class MutatorKey { None, Mutate, Randomize, Interpolate, Cross, StoreChild, Mother, Father, MutateCurrent };
inline MutatorKey mutatorKey(char c, bool modifier, bool repeat)
{
    if (modifier || repeat)
        return MutatorKey::None;
    switch (std::tolower(static_cast<unsigned char>(c))) {
    case 'u': return MutatorKey::Mutate;
    case 'n': return MutatorKey::Randomize;
    case 'i': return MutatorKey::Interpolate;
    case 'x': return MutatorKey::Cross;
    case 's': return MutatorKey::StoreChild;
    case 'o': return MutatorKey::Mother;
    case 't': return MutatorKey::Father;
    case 'e': return MutatorKey::MutateCurrent;
    default: return MutatorKey::None;
    }
}

// A click on a performance's slot button (manual p58): it focuses the slot; Shift-click turns it on or off; the morph
// modifier (Option on the Mac, Ctrl elsewhere: the manual's Ctrl) assigns the keyboard to it or not.
enum class SlotClick { Focus, ToggleActive, ToggleKeyboard };
inline SlotClick slotClick(bool shift, bool morphModifier)
{
    if (shift)
        return SlotClick::ToggleActive;
    return morphModifier ? SlotClick::ToggleKeyboard : SlotClick::Focus;
}

// The init patches' files (Save InitPatch 1/2; the toolbar's Init 1 and Init 2): "InitPatch" + n in the settings
// folder, as the original's in its preferences folder (CEditorApp), with .pch2 so that they open elsewhere too.
inline std::string initPatchFileName(int n)
{
    return "InitPatch" + std::to_string(n) + ".pch2";
}

} // namespace g2ui
