// Small editor dialogs: patch notes and performance settings.
#pragma once

#include "PatchDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui {

void showPatchNotes(PatchDocument& doc, juce::Component* parent);
void showPerformanceSettings(PatchDocument& doc, juce::Component* parent);

} // namespace g2ui
