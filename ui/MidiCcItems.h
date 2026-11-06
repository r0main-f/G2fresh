#pragma once
// A parameter's MIDI controller, as Clavia's editor v1.62 offers it. The parameter pop-up's MIDI Controller submenu
// (CControlMenu::CControlMenu @0009026e, InitMenus @0008fcbe): "Assign to CC# n" for the last CC the synth received
// (CPatch::GetLastUsedMidiController; "No MIDI CC received" before one), "Assign..." (ticked when the parameter has a
// CC) opening the list of controllers (CDialogMIDIControl::InitDialog @00031476), and "Deassign". Plain C++.

#include "g2/edit.hpp"
#include "g2/module_db.hpp"
#include "g2/patch.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace g2ui {

struct MidiCcItem {
    std::string label;
    bool enabled = true;
    bool ticked = false;
};

// `assigned`: the parameter's CC; `lastReceived`: the last CC the synth received, if any. Assigning the last one needs
// a CC a parameter can take.
inline std::vector<MidiCcItem> midiCcItems(std::optional<std::uint8_t> assigned, std::optional<std::uint8_t> lastReceived)
{
    std::vector<MidiCcItem> items;
    if (lastReceived)
        items.push_back({"Assign to CC# " + std::to_string(*lastReceived),
                         g2::edit::isValidMidiCc(*lastReceived) && !g2::edit::isPreAssignedMidiCc(*lastReceived)});
    else
        items.push_back({"No MIDI CC received", false});
    items.push_back({"Assign...", true, assigned.has_value()});
    items.push_back({"Deassign", assigned.has_value()});
    return items;
}

// The Assign dialog's list: every controller a parameter can take (valid, not pre-assigned), with the module and
// parameter holding it ("" when free).
struct MidiCcRow {
    std::uint8_t cc = 0;
    std::string module, param;
};
inline std::vector<MidiCcRow> midiCcRows(const g2::Patch& patch)
{
    std::vector<MidiCcRow> rows;
    for (int n = 0; n < 120; ++n) {
        const auto cc = static_cast<std::uint8_t>(n);
        if (!g2::edit::isValidMidiCc(cc) || g2::edit::isPreAssignedMidiCc(cc))
            continue;
        MidiCcRow row{cc, {}, {}};
        for (const auto& a : patch.controllers)
            if (a.cc == cc) {
                const auto loc = static_cast<g2::Location>(a.location);
                if (const auto* m = loc == g2::Location::Settings ? nullptr : patch.area(loc).find(a.module)) {
                    row.module = m->name;
                    if (const auto* def = g2::db::find(m->type); def && a.param < def->params.size())
                        row.param = def->params[a.param].name;
                } else {
                    row.module = "Patch settings";
                }
            }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace g2ui
