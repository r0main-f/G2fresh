#pragma once
// The patch window's menu item ids. Each command and range has its own numbers: the menu handler tells them apart by
// range (MainView::menuItemSelected). Plain C++.

namespace g2ui::menuid {

enum MenuId {
    kNew = 1, kOpen, kSave, kSaveAs, kClearRecent, kNewPerformance, kPerformanceSettings, kPatchLibrary,
    kUndo, kRedo, kDelete, kRename, kCut, kCopy, kPaste, kDuplicate, kSelectAll, kPatchNotes,
    kRandomize, kMutate, kMutator,
    kZoomIn, kZoomOut, kZoomReset, kShowSettings, kClassicLook, kAnimateCables,
    kAudioSettings, kRemoveBendPoints, kLiveMode, kHideAllCables, kShakeCables, kZoomFit, kSaveSelection, kInsertPatch,
    kRecallInit, kDeleteUnused, kPasteParams, kSaveInitPatch1, kSaveInitPatch2,
    kNewToBase = 900, kOpenToBase = 910, kSaveFromBase = 920, // + slot
    kAssignMidiSelection = 930, kDeassignMidiSelection, kDeassignAllMidi,
    kRecentBase = 100,       // + index into the recent files list
    kCopyVariationBase = 200, // + target variation (8 = init)
    kCablesBase = 300,        // + cable colour
    kCableStyleBase = 310,    // + CableStyle
    kCableOpacityBase = 320,  // + index into kCableOpacities
    kMidiOutOff = 400, kMidiOutBase = 401, // + index into the device list
    kMidiChannelBase = 600,   // + channel (0: as played)
    kSynthG2 = 700, kSynthVirtual, kSynthDisconnect, kSendPerformance, kGetPerformance, kUnbind, kSynthMemory, kShowUsbLog, kFullUsbLog,
    kSynthSettings,
    kSendPatchBase = 710,     // + slot
    kGetPatchBase = 720,      // + slot
    kSynthEmulated = 730, kEmulatorFirmware, kHostClock, kMixOut34, kEmulatorExpansion,
    kEmulatorBufferBase = 740, // + buffer (EmulatorHost::emulatorBuffer)
    // Opt-in deviations from the original editor (View menu).
    kWheelEditsKnobs = 800, kDoubleClickMorph, kDoubleClickReset, kDoubleClickNothing, kShowLoadEstimate,
    kMorphGroupColours,
};

// The Open Recent entries: as many as the list keeps (MainView: 12).
inline constexpr int kRecentMax = 12;
constexpr bool isRecentFileItem(int id) { return id >= kRecentBase && id < kRecentBase + kRecentMax; }

} // namespace g2ui::menuid
