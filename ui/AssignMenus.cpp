#include "AssignMenus.h"

#include <algorithm>

namespace g2ui::assignmenus {

juce::PopupMenu knobMenu(const g2::Patch& patch, g2::Location loc, std::uint8_t module, std::uint8_t param)
{
    juce::PopupMenu knobs;
    const auto knob = g2::edit::knobOf(patch, loc, module, param);
    // The original's Knob menu: Page A1 .. Page E3, eight knobs each (SMNU, ids 2010-2024).
    for (int page = 0; page < 15; ++page) {
        juce::PopupMenu pageMenu;
        for (int k = 0; k < g2::edit::kKnobsPerPage; ++k) {
            const int index = page * g2::edit::kKnobsPerPage + k;
            juce::String text = "Knob " + juce::String(k + 1);
            if (const auto& a = patch.knobs[static_cast<std::size_t>(index)]; a && index != knob.value_or(-1))
                text << "  (in use)";
            pageMenu.addItem(kKnobBase + index, text, true, knob == index);
        }
        const auto name = "Page " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + page / 3)) + juce::String(page % 3 + 1);
        knobs.addSubMenu(name, pageMenu, true, nullptr, knob && *knob / g2::edit::kKnobsPerPage == page);
    }
    knobs.addSeparator();
    knobs.addItem(kClearKnob, "No knob", knob.has_value());
    return knobs;
}

juce::PopupMenu ccMenu(const g2::Patch& patch, g2::Location loc, std::uint8_t module, std::uint8_t param)
{
    // None for a parameter holding CC 7 or 17 or not MIDI-assignable
    // (CControlMenu removes the item).
    juce::PopupMenu ccs;
    if (!g2::edit::canAssignMidiCc(patch, loc, module, param))
        return ccs;
    const auto cc = g2::edit::midiCcOf(patch, loc, module, param);
    for (int group = 0; group < 8; ++group) {
        juce::PopupMenu groupMenu;
        for (int n = group * 16; n < std::min(group * 16 + 16, 120); ++n) {
            if (!g2::edit::isValidMidiCc(static_cast<std::uint8_t>(n)) || g2::edit::isPreAssignedMidiCc(static_cast<std::uint8_t>(n)))
                continue;
            const bool used = std::any_of(patch.controllers.begin(), patch.controllers.end(),
                                          [&](const g2::CtrlAssign& a) { return a.cc == n; });
            groupMenu.addItem(kCcBase + n, "CC " + juce::String(n) + (used && cc != n ? "  (in use)" : ""), true, cc == n);
        }
        ccs.addSubMenu("CC " + juce::String(group * 16) + "-" + juce::String(std::min(group * 16 + 15, 119)), groupMenu);
    }
    ccs.addSeparator();
    ccs.addItem(kClearCc, "No MIDI controller", cc.has_value());
    return ccs;
}

void apply(PatchDocument& doc, int r, g2::Location loc, std::uint8_t module, std::uint8_t param)
{
    if (r >= kKnobBase && r < kKnobBase + g2::kKnobCount) {
        doc.perform("Assign knob", [&](g2::Patch& p) { g2::edit::assignKnob(p, r - kKnobBase, loc, module, param); });
    } else if (r == kClearKnob) {
        doc.perform("Remove knob", [&](g2::Patch& p) {
            if (const auto k = g2::edit::knobOf(p, loc, module, param))
                g2::edit::clearKnob(p, *k);
        });
    } else if (r >= kCcBase && r < kCcBase + 128) {
        const auto n = static_cast<std::uint8_t>(r - kCcBase);
        doc.perform("Assign MIDI controller", [&](g2::Patch& p) { g2::edit::assignMidiCc(p, n, loc, module, param); });
    } else if (r == kClearCc) {
        doc.perform("Remove MIDI controller", [&](g2::Patch& p) {
            if (const auto c = g2::edit::midiCcOf(p, loc, module, param))
                g2::edit::clearMidiCc(p, *c);
        });
    }
}

} // namespace g2ui::assignmenus
