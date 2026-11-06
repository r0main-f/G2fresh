#pragma once
// The hint boxes F5 to F8 show on the patch while the key is held, as Clavia's editor v1.62 (CPatchView::KeyHintView*,
// KeyBringDownHintViews on release; manual p115): F5 every parameter's value (CPatchBackground::HintViewParamValues
// @000e1f4c), F6 the morph groups a parameter is in (HintViewParamMorphGroupes @000e2646), F7 its knob
// (HintViewKnobAssignment @000e1ba8: "A1:3"), F8 its MIDI controller (HintViewMIDICtrlAssignment @000e1a1c:
// MIDICtrl::GetNonDescriptiveName, "Ctrl 21"). Ctrl+F8's controller values are not built. Plain C++.

#include "g2/edit.hpp"
#include "g2/patch.hpp"

#include <optional>
#include <string>

namespace g2ui {

enum class HintMode { None, Values, MorphGroups, Knobs, MidiCcs };

// `f`: the function key's number (5 for F5).
inline HintMode hintModeForKey(int f)
{
    switch (f) {
    case 5: return HintMode::Values;
    case 6: return HintMode::MorphGroups;
    case 7: return HintMode::Knobs;
    case 8: return HintMode::MidiCcs;
    default: return HintMode::None;
    }
}

// The box for one parameter, or none.
inline std::optional<std::string> hintText(const g2::Patch& patch, HintMode mode, g2::Location loc, std::uint8_t module,
                                           std::uint8_t param, std::uint8_t variation)
{
    switch (mode) {
    case HintMode::Values:
        return g2::edit::paramText(patch, loc, module, param, variation);
    case HintMode::MorphGroups: {
        std::string text;
        for (int group = 0; group < g2::kMorphGroups; ++group)
            if (g2::edit::morphRange(patch, variation, loc, module, param, static_cast<std::uint8_t>(group)))
                text += (text.empty() ? "" : "\n") + g2::edit::morphLabel(patch, group);
        return text.empty() ? std::nullopt : std::optional<std::string>(text);
    }
    case HintMode::Knobs: {
        const auto knob = g2::edit::knobOf(patch, loc, module, param);
        if (!knob)
            return std::nullopt;
        const int page = *knob / g2::edit::kKnobsPerPage;
        return std::string(1, static_cast<char>('A' + page / 3)) + std::to_string(page % 3 + 1) + ":"
             + std::to_string(*knob % g2::edit::kKnobsPerPage + 1);
    }
    case HintMode::MidiCcs: {
        const auto cc = g2::edit::midiCcOf(patch, loc, module, param);
        return cc ? std::optional<std::string>("Ctrl " + std::to_string(*cc)) : std::nullopt;
    }
    case HintMode::None: break;
    }
    return std::nullopt;
}

} // namespace g2ui
