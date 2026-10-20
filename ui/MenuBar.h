#pragma once
// The menu bar (MainView::getMenuBarNames, getMenuForIndex): which menus, and which commands each holds, in order. It
// follows the original editor's menu bar (its menu script, SMNU 128: File, Edit, Patch, Performance, Synth, Setup,
// Tools), with View for what G2fresh shows and how, and Help for the USB log. The Synth menu is for the two ways
// G2fresh is used: a G2 or G2X on USB, or the Emulated G2 (the user's decision, 2026-10-19). Labels, shortcuts and
// states are MainView's. Plain C++.

#include "MenuIds.h"

#include <string>
#include <vector>

namespace g2ui::menubar {

constexpr int kSeparator = 0;

// `synth`: this window talks to a synth (the stand-alone app and the plugin both do).
inline std::vector<std::string> names(bool synth)
{
    std::vector<std::string> n{"File", "Edit", "View", "Patch", "Performance"};
    if (synth)
        n.push_back("Synth");
    n.push_back("Setup");
    n.push_back("Tools");
    if (synth)
        n.push_back("Help");
    return n;
}

// The menu's commands, top to bottom; a submenu by its first id, kSeparator between groups. "Emulated G2" is the
// Synth menu's submenu. A command the window cannot offer (Audio/MIDI Settings in the plugin, Forward MIDI in the
// stand-alone app; Send Patch or Send Performance, by the document) is left out by MainView.
inline std::vector<int> items(const std::string& menu)
{
    using namespace menuid;
    if (menu == "File") // SMNU File: 100, 112, 101, 110, 107, 111 (Recent Files), 103, 104, 113, 108, 109
        return {kNew, kNewPerformance, kOpen, kNewToBase, kOpenToBase, kRecentBase, kPatchLibrary, kSeparator,
                kSave, kSaveAs, kSaveFromBase, kSaveInitPatch1, kSaveInitPatch2};
    if (menu == "Edit") // SMNU Edit: 200-207
        return {kUndo, kRedo, kSeparator, kCut, kCopy, kPaste, kDuplicate, kPasteParams, kDelete, kSelectAll};
    if (menu == "View")
        return {kLiveMode, kSeparator, kZoomIn, kZoomOut, kZoomReset, kZoomFit, kSeparator, kCablesBase,
                kHideAllCables, kShakeCables, kSeparator, kClassicLook, kCableStyleBase, kCableOpacityBase,
                kAnimateCables, kMorphGroupColours, kShowLoadEstimate};
    if (menu == "Patch") // SMNU Patch: 1005, 1002, 305
        return {kShowSettings, kPatchNotes, kRename, kSeparator, kCopyVariationBase, kRecallInit, kSeparator,
                kInsertPatch, kSaveSelection, kDeleteUnused, kRemoveBendPoints};
    if (menu == "Performance") // SMNU Performance: 350
        return {kPerformanceSettings};
    if (menu == "Synth")
        return {kSynthG2, kSynthEmulated, kSynthDisconnect, kSeparator, kSynthSettings, kSeparator, kSendPatchBase,
                kSendPerformance, kGetPatchBase, kGetPerformance, kUnbind, kSynthMemory, kSeparator, kEmulatedMenu};
    if (menu == "Emulated G2")
        return {kEmulatorFirmware, kSeparator, kEmulatorBufferBase, kEmulatorExpansion, kSeparator, kHostClock,
                kMixOut34};
    if (menu == "Setup") // SMNU Setup: Audio/MIDI 502
        return {kAudioSettings, kMidiOutOff, kSeparator, kWheelEditsKnobs, kDoubleClickMorph};
    if (menu == "Tools") // SMNU Tools: 1090, 1086-1088
        return {kMutator, kRandomize, kMutate, kSeparator, kAssignMidiSelection, kDeassignMidiSelection,
                kDeassignAllMidi};
    if (menu == "Help")
        return {kShowUsbLog, kFullUsbLog};
    return {};
}

} // namespace g2ui::menubar
