// The original editor's special panel controls (PANL `Graph` elements that
// are interactive instead of graphs), drawn in both looks:
//   22  Vocoder band presets "-2" "-1" "0" "+1" "+2" "Inv" "Rnd"
//   25  SeqNote zoom switch (3, 2 or 1 octaves) with its key ruler
//   26  SeqNote octave offset ("C0".."C9") with left/right buttons
//   46  DrumSynth preset name with up/down buttons
// and the SeqNote step sliders, whose visible note window follows the zoom
// and offset. The editing itself is g2::special (core/include/g2/special.hpp).
#pragma once

#include "ModulePainter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace g2ui {

class SpecialControls {
public:
    static bool isSpecial(const PanelElement& e);
    static bool isSeqSlider(const PanelElement& e) { return e.kind == "Knob" && e.type == "SeqSlider"; }

    // The part of a special control under a point (relative to the element).
    enum class Part { None, VocoderButton, Zoom, OffsetLeft, OffsetRight, DrumName, DrumUp, DrumDown };
    struct Hit {
        Part part = Part::None;
        int index = 0; // VocoderButton: 0..6 (g2::special::VocoderOp)
    };
    static Hit partAt(const PanelElement& e, juce::Point<int> local);

    // Status-bar text for the part under the mouse.
    static juce::String describe(const ModuleContext& c, const PanelElement& e, Hit hit);

    static void paint(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, bool highlighted);
    static void paintSeqSlider(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, bool highlighted);

    // The value a click at row `localY` of a step slider sets.
    static int seqSliderValueAt(const ModuleContext& c, int localY);
    // Rows of a step slider per note at the module's zoom.
    static int seqSliderPixelsPerValue(const ModuleContext& c);

    // The vocoder buttons' bounds, relative to the element.
    static juce::Rectangle<int> vocoderButton(int index);
};

} // namespace g2ui
