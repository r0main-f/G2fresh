// Small editor dialogs: patch notes and performance settings.
#pragma once

#include "PatchDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui {

void showPatchNotes(PatchDocument& doc, juce::Component* parent);
// The MIDI Controller dialog of a parameter's Assign... (CDialogMIDIControl): every controller a parameter can take,
// with what holds it; the one chosen goes to the parameter, as one undo step.
void showMidiCcDialog(PatchDocument& doc, g2::Location loc, std::uint8_t module, std::uint8_t param, juce::Component* parent);
void showPerformanceSettings(PatchDocument& doc, juce::Component* parent);

} // namespace g2ui
