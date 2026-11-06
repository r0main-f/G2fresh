// The Assign (knob) and MIDI Controller submenus of the original's parameter
// popup (CControlMenu), shared by module parameters and the toolbar morph
// dials (CMorphControlMenu builds on CControlMenu).
#pragma once

#include "PatchDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui::assignmenus {

// Menu item ids the two submenus use; callers keep their own items apart.
inline constexpr int kClearKnob = 2, kClearCc = 3, kCcDialog = 4, kKnobBase = 200, kCcBase = 400;
inline bool isAssignItem(int id)
{
    return id == kClearKnob || id == kClearCc || id == kCcDialog || (id >= kKnobBase && id < kKnobBase + g2::kKnobCount)
        || (id >= kCcBase && id < kCcBase + 128);
}

// The 120 knobs by page; "No knob" removes the parameter's.
juce::PopupMenu knobMenu(const g2::Patch& patch, g2::Location loc, std::uint8_t module, std::uint8_t param);
// The original's MIDI Controller submenu (MidiCcItems.h): "Assign to CC# n" for `lastReceived` (the last CC the synth
// received), "Assign..." (the list of controllers), "Deassign". Empty when the parameter shows no MIDI item
// (canAssignMidiCc).
juce::PopupMenu ccMenu(const g2::Patch& patch, g2::Location loc, std::uint8_t module, std::uint8_t param,
                       std::optional<std::uint8_t> lastReceived);
// Applies an item of either submenu as one undo step (Assign... opens the dialog over `parent`).
void apply(PatchDocument& doc, int id, g2::Location loc, std::uint8_t module, std::uint8_t param,
           juce::Component* parent = nullptr);

} // namespace g2ui::assignmenus
