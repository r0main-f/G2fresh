// Synth > Synth Settings... (Cmd/Ctrl-G): the synth's system settings, as the original editor's dialog
// (CDialogSynthSettings, SynthSettingsForm.h) has them, in G2fresh's look. A modal window over the connected synth:
// each change goes to the synth at once, OK keeps them, Cancel (or Escape, or the close button) sends back the settings
// the window opened with. Changes made on the synth itself (its System menu) show in the window as they come; it
// closes when the connection goes.
#pragma once

#include "SynthSync.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace g2ui {

// Opens the window, modal, centred on `parent`; nullptr if the synth is not ready (connected and read). The caller
// owns it (so it goes with the caller) and is told, through `closed`, when it may delete it.
std::unique_ptr<juce::Component> openSynthSettings(SynthSync& synth, juce::Component* parent, std::function<void()> closed);
// The window's content alone, for a synth that is ready (g2render draws it).
std::unique_ptr<juce::Component> makeSynthSettingsPanel(SynthSync& synth);

} // namespace g2ui
