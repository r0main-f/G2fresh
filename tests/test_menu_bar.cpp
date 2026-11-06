// The menu bar (MenuBar.h): the original editor's menus (SMNU 128), and a Synth menu for a G2 on USB or the
// Emulated G2.
#include <catch2/catch_test_macros.hpp>

#include "LiveShow.h"
#include "MenuBar.h"

#include <algorithm>
#include <map>
#include <set>

using namespace g2ui;
using namespace g2ui::menuid;
using g2ui::menubar::kSeparator;

namespace {

bool has(const std::vector<int>& items, int id)
{
    return std::find(items.begin(), items.end(), id) != items.end();
}

} // namespace

TEST_CASE("The menu bar has the original's menus, in its order")
{
    // The original's menu script: File, Edit, Patch, Performance, Synth, Setup, Tools (Window and Help on Windows).
    // G2fresh adds View after Edit, as Mac apps have it, and Help for the USB log.
    CHECK(menubar::names(true)
          == std::vector<std::string>{"File", "Edit", "View", "Patch", "Performance", "Synth", "Setup", "Tools", "Help"});
    CHECK(menubar::names(false) == std::vector<std::string>{"File", "Edit", "View", "Patch", "Performance", "Setup", "Tools"});
}

TEST_CASE("Each menu holds its own commands, and every command is in one menu")
{
    using menubar::items;
    // Edit: only the editing commands, as the original's Edit menu.
    CHECK(items("Edit")
          == std::vector<int>{kUndo, kRedo, kSeparator, kCut, kCopy, kPaste, kDuplicate, kPasteParams, kDelete, kSelectAll});
    // Patch: the original's Patch Settings and Textpad (SMNU 1005, 1002) and Delete Unused Modules (305), with the
    // patch's own commands that were in Edit and View.
    for (const int id : {int(kShowSettings), int(kPatchNotes), int(kRename), int(kCopyVariationBase), int(kRecallInit),
                         int(kInsertPatch), int(kSaveSelection), int(kDeleteUnused), int(kRemoveBendPoints)})
        CHECK(has(items("Patch"), id));
    CHECK(items("Performance") == std::vector<int>{kPerformanceSettings}); // SMNU 350
    // Tools: the original's Patch Mutator (1090) and MIDI assignment (1086-1088), with Randomize and Mutate.
    CHECK(items("Tools")
          == std::vector<int>{kMutator, kRandomize, kMutate, kSeparator, kAssignMidiSelection, kDeassignMidiSelection,
                              kDeassignAllMidi});
    // Synth: which synth, then what goes to and from it; the Emulated G2's own options in one submenu.
    CHECK(items("Synth")
          == std::vector<int>{kSynthG2, kSynthEmulated, kSynthDisconnect, kSeparator, kSynthSettings, kSeparator,
                              kSendPatchBase, kSendPerformance, kGetPatchBase, kGetPerformance, kUnbind, kSynthMemory,
                              kSeparator, kEmulatedMenu});
    CHECK(items("Emulated G2")
          == std::vector<int>{kEmulatorFirmware, kSeparator, kEmulatorBufferBase, kEmulatorExpansion, kSeparator,
                              kHostClock, kMixOut34});
    CHECK(items("Setup")
          == std::vector<int>{kAudioSettings, kMidiOutOff, kSeparator, kWheelEditsKnobs, kDoubleClickMorph});
    CHECK(items("Help") == std::vector<int>{kShowUsbLog, kFullUsbLog});

    // Every command the menus had is in exactly one menu, but the Virtual G2 (a stand-in without sound, for
    // developers: g2bridge --emulator).
    const std::set<int> all{kNew, kNewPerformance, kOpen, kNewToBase, kOpenToBase, kRecentBase, kPatchLibrary, kSave,
                            kSaveAs, kSaveFromBase, kSaveInitPatch1, kSaveInitPatch2, kPerformanceSettings, kUndo, kRedo,
                            kCut, kCopy, kPaste, kDuplicate, kPasteParams, kDelete, kSelectAll, kSaveSelection,
                            kInsertPatch, kCopyVariationBase, kRecallInit, kRandomize, kMutate, kMutator, kRename,
                            kPatchNotes, kDeleteUnused, kAssignMidiSelection, kDeassignMidiSelection, kDeassignAllMidi,
                            kLiveMode, kZoomIn, kZoomOut, kZoomReset, kZoomFit, kShowSettings, kCablesBase,
                            kHideAllCables, kShakeCables, kCableStyleBase, kCableOpacityBase, kRemoveBendPoints,
                            kClassicLook, kAnimateCables, kWheelEditsKnobs, kDoubleClickMorph, kMorphGroupColours,
                            kShowLoadEstimate, kAudioSettings, kMidiOutOff, kSynthG2, kSynthEmulated, kEmulatorFirmware,
                            kHostClock, kMixOut34, kEmulatorBufferBase, kEmulatorExpansion, kSynthDisconnect,
                            kSynthSettings, kSendPerformance, kSendPatchBase, kGetPatchBase, kGetPerformance,
                            kSynthMemory, kUnbind, kShowUsbLog, kFullUsbLog};
    std::map<int, int> seen;
    for (const auto& menu : menubar::names(true))
        for (const int id : items(menu))
            ++seen[id];
    for (const int id : items("Emulated G2"))
        ++seen[id];
    for (const int id : all) {
        INFO("command " << id);
        CHECK(seen[id] == 1);
    }
    CHECK(seen.count(kSynthVirtual) == 0);
}

TEST_CASE("The Live view is greyed while a G2 on USB is connected")
{
    // The user's decision (2026-10-19): the Live panel and keyboard are the Emulated G2's; a real G2 has its own.
    // Not connected, showing it starts the Emulated G2 (LiveShow.h).
    CHECK(liveViewAvailable(true, false));
    CHECK_FALSE(liveViewAvailable(true, true));
    CHECK_FALSE(liveViewAvailable(false, false)); // a build without the emulator
}
