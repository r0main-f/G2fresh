// G2fresh's own icons for the Modern look, drawn as vectors. Where the
// original editor shows a small picture (a waveform on an oscillator's
// buttons, a filter slope, an envelope shape, a switch or amplifier symbol
// on a module face...), the Modern look draws the same meaning in the same
// place, our way. Controls and decorations are recognised by module type and
// element (parameter, mode or panel element id), never by the original's
// pixels.
#pragma once

#include "Skin.h"

#include "g2/module_db.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <span>

namespace g2ui::icons {

enum class Icon : std::uint8_t {
    None,
    // Waveforms, one period left to right.
    Sine, Tri, Saw, Square, Pulse25, Pulse10, DualSaw, DoubleSaw, SymPulse, RndStep, Rnd, RndPulses,
    RoundPulses, CosBell, TriBell, SawTri, Trapezoid, Sine2, Sine3, Sine4,
    // Rectifier outputs and shaper transfer curves.
    HalfPos, HalfNeg, FullPos, FullNeg, CurveInv3, CurveInv2, CurveX2, CurveX3,
    // Filter responses (Option::amount: slope in dB/octave).
    LowPass, HighPass,
    // Envelope attack/decay curves.
    EnvLogExp, EnvLinExp, EnvExpExp, EnvLinLin,
    // Logic: input (faint) over output timing, flip-flops and gates.
    PosEdge, NegEdge, DelayRise, DelayFall, DelayBoth, ClkGated, ClkToggle, FlipFlopD, FlipFlopSR,
    GateAnd, GateNand, GateOr, GateNor, GateXor, GateXnor,
    // Buttons.
    Power, OneShot, Loop, Bipolar, Positive, Negative, BlackKey, WhiteKey,
};

// One value of a control that the original shows as a picture: the icon
// (None: show the label instead) and a label for menus and tooltips.
struct Option {
    Icon icon = Icon::None;
    const char* label = "";
    int amount = 0;           // LowPass / HighPass: dB per octave
    bool unavailable = false; // drawn struck through (the original crosses it out)
    bool textOnFace = false;  // the control shows the label, its menu the icon
};

// The pictures of a control, one per value; empty if it shows text.
std::span<const Option> options(const g2::db::ModuleDef& def, const PanelElement& e);
// The icon of a ButtonText that shows one (bypass/mute, KeyQuant's keys), else None.
Icon buttonIcon(const g2::db::ModuleDef& def, const PanelElement& e);

// Draws `icon` fitted in `box` with strokes in `colour`.
void draw(juce::Graphics& g, Icon icon, juce::Rectangle<float> box, juce::Colour colour, int amount = 0);
// An option in `box`: its icon, or its label as text.
void drawOption(juce::Graphics& g, const Option& o, juce::Rectangle<float> box, juce::Colour colour);
// A menu item picture of an option (nullptr for a text-only option).
std::unique_ptr<juce::Drawable> menuImage(const Option& o, juce::Colour colour);

// Draws a panel `Bitmap` element (a signal-flow symbol or a section title)
// of module `def`; false if it isn't one we draw.
bool paintDecoration(juce::Graphics& g, const g2::db::ModuleDef& def, const PanelElement& e, juce::Colour line,
                     juce::Colour ink);

} // namespace g2ui::icons
