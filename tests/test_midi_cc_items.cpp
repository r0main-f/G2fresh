// A parameter's MIDI controller, as Clavia's editor v1.62 offers it: the parameter pop-up's MIDI Controller submenu
// (CControlMenu::CControlMenu @0009026e, InitMenus @0008fcbe: "Assign to CC# n" for the last CC the synth received or
// "No MIDI CC received", "Assign..." ticked when the parameter has one, "Deassign"), the Assign dialog's list
// (CDialogMIDIControl::InitDialog @00031476) and the L key (CPatchView::KeyLearnMidi @000f99fe).
#include <catch2/catch_test_macros.hpp>

#include "MidiCcItems.h"
#include "PatchKeys.h"

#include "g2/edit.hpp"

using namespace g2ui;

TEST_CASE("MIDI controller submenu: the last CC received, Assign..., Deassign")
{
    auto items = midiCcItems(std::nullopt, std::nullopt);
    REQUIRE(items.size() == 3);
    CHECK(items[0].label == "No MIDI CC received");
    CHECK_FALSE(items[0].enabled);
    CHECK(items[1].label == "Assign...");
    CHECK_FALSE(items[1].ticked);
    CHECK(items[2].label == "Deassign");
    CHECK_FALSE(items[2].enabled);
    items = midiCcItems(std::uint8_t{21}, std::uint8_t{74});
    CHECK(items[0].label == "Assign to CC# 74");
    CHECK(items[0].enabled);
    CHECK(items[1].ticked);
    CHECK(items[2].enabled);
    CHECK_FALSE(midiCcItems(std::nullopt, std::uint8_t{7})[0].enabled); // the patch volume's CC
}

TEST_CASE("MIDI controller dialog: every CC a parameter can take, and what holds it")
{
    auto p = g2::Patch::makeDefault();
    const auto osc = g2::edit::addModule(p, g2::Location::Va, 7, 0, 0); // OscB
    g2::edit::assignMidiCc(p, 21, g2::Location::Va, osc, 0);
    const auto rows = midiCcRows(p);
    for (const auto& r : rows) {
        CHECK(g2::edit::isValidMidiCc(r.cc));
        CHECK_FALSE(g2::edit::isPreAssignedMidiCc(r.cc));
    }
    const auto it = std::find_if(rows.begin(), rows.end(), [](const MidiCcRow& r) { return r.cc == 21; });
    REQUIRE(it != rows.end());
    CHECK(it->module == p.va.find(osc)->name);
    CHECK(it->param == g2::db::find(7)->params[0].name);
    const auto free = std::find_if(rows.begin(), rows.end(), [](const MidiCcRow& r) { return r.cc == 22; });
    REQUIRE(free != rows.end());
    CHECK(free->module.empty());
}

TEST_CASE("patch keys: L learns the last MIDI CC")
{
    // InitializeShortcutKeys @000fafd8 binds KeyLearnMidi as the variations' keys.
    CHECK(patchKey('l', false, false, false).kind == PatchKey::Kind::LearnMidi);
    CHECK(patchKey('L', true, false, false).kind == PatchKey::Kind::None);
}

TEST_CASE("patch keys: M opens the focused parameter's MIDI controllers")
{
    // InitializeShortcutKeys @000fafd8: KeyAssignMidi (CDialogMIDIControl; manual p90 "(M key)").
    CHECK(patchKey('m', false, false, false).kind == PatchKey::Kind::AssignMidi);
    CHECK(patchKey('m', false, true, false).kind == PatchKey::Kind::None);
}
