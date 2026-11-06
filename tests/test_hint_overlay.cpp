// The F5-F8 hint boxes, as Clavia's editor v1.62 shows them while the key is held (CPatchView::KeyHintView*, bound
// in InitializeShortcutKeys @000fafd8, KeyBringDownHintViews on release; CPatchBackground::HintViewParamValues
// @000e1f4c, HintViewParamMorphGroupes @000e2646, HintViewKnobAssignment @000e1ba8, HintViewMIDICtrlAssignment
// @000e1a1c; manual p115).
#include <catch2/catch_test_macros.hpp>

#include "HintOverlay.h"

#include "g2/edit.hpp"

using namespace g2ui;

TEST_CASE("hints: F5 to F8")
{
    CHECK(hintModeForKey(5) == HintMode::Values);
    CHECK(hintModeForKey(6) == HintMode::MorphGroups);
    CHECK(hintModeForKey(7) == HintMode::Knobs);
    CHECK(hintModeForKey(8) == HintMode::MidiCcs);
    CHECK(hintModeForKey(9) == HintMode::None);
}

TEST_CASE("hints: what each box says")
{
    auto p = g2::Patch::makeDefault();
    const auto osc = g2::edit::addModule(p, g2::Location::Va, 7, 0, 0); // OscB
    // F7: page letter and column, then the knob (the knob index >> 3 as page A1.., & 7 as the knob).
    g2::edit::assignKnob(p, 3 * 8 + 4, g2::Location::Va, osc, 0);
    CHECK(hintText(p, HintMode::Knobs, g2::Location::Va, osc, 0, 0) == "B1:5");
    CHECK_FALSE(hintText(p, HintMode::Knobs, g2::Location::Va, osc, 1, 0));
    // F8: MIDICtrl::GetNonDescriptiveName @0014877e.
    g2::edit::assignMidiCc(p, 21, g2::Location::Va, osc, 1);
    CHECK(hintText(p, HintMode::MidiCcs, g2::Location::Va, osc, 1, 0) == "Ctrl 21");
    CHECK_FALSE(hintText(p, HintMode::MidiCcs, g2::Location::Va, osc, 0, 0));
    // F5: the value as the module shows it.
    CHECK(hintText(p, HintMode::Values, g2::Location::Va, osc, 0, 0) == g2::edit::paramText(p, g2::Location::Va, osc, 0, 0));
    // F6: the morph groups the parameter is in, by their names; none: no box.
    CHECK_FALSE(hintText(p, HintMode::MorphGroups, g2::Location::Va, osc, 0, 0));
    g2::edit::setMorph(p, 0, g2::Location::Va, osc, 0, 2, 40);
    CHECK(hintText(p, HintMode::MorphGroups, g2::Location::Va, osc, 0, 0) == g2::edit::morphLabel(p, 2));
}
