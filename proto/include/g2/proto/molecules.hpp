// The molecules of the G2's USB protocol: the commands, requests and dumps a
// message carries (re/notes/usb-protocol.md §4.1, §7). A molecule is an id byte
// and a body written MSB-first with byte-aligned byte fields (CBitStream);
// molecules follow each other at byte granularity.
//
// Each id has a typed struct below. Bodies have no length field, so decoding
// needs every id's layout; an id this codec does not know swallows the rest of
// the message as `Raw` (Clavia would drop the connection, §4.1). Decoding
// never throws: a malformed molecule also becomes `Raw` with the remaining
// bytes, so encode(decode(x)) == x for any input.
#pragma once

#include "g2/file.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace g2::proto {

using u8 = std::uint8_t;

// Molecule ids (Clavia CM* class, Verhue constant in the spec's table).
namespace id {
inline constexpr u8 SynthDataRequest = 0x02, SynthData = 0x03, VoicesRequest = 0x04, Voices = 0x05;
inline constexpr u8 SlotSelectionRequest = 0x06, SlotSelection = 0x07, SlotFocusRequest = 0x08, SlotFocus = 0x09;
inline constexpr u8 FlashLoad = 0x0A, FlashStore = 0x0B, FlashDelete = 0x0C, FlashResult = 0x0D;
inline constexpr u8 FlashDeleteRange = 0x0E, KeyboardFocus = 0x0F, PerfHeaderRequest = 0x10, PerfHeader = 0x11;
inline constexpr u8 FlashDeleteRangeDump = 0x12, FlashUsage = 0x13, FlashDataRequest = 0x14, FlashDeleteDump = 0x15;
inline constexpr u8 FlashData = 0x16, FlashDumpRequest = 0x17, FlashDumpMarker = 0x18, FlashRawData = 0x19;
inline constexpr u8 CompletePerformance = 0x1A, GlobalKnobAssign = 0x1C, GlobalKnobDeassign = 0x1D;
inline constexpr u8 GlobalPageFocus = 0x1E, PerformanceRelease = 0x1F, PatchHeaderRequest = 0x20, PatchHeader = 0x21;
inline constexpr u8 CtrlAssign = 0x22, CtrlDeassign = 0x23, KnobAssign = 0x25, KnobDeassign = 0x26;
inline constexpr u8 PatchName = 0x27, PatchNameRequest = 0x28, PerfName = 0x29, Uprate = 0x2A, ModeChange = 0x2B;
inline constexpr u8 PerfNameRequest = 0x2C, PageFocus = 0x2D, ParamFocusRequest = 0x2E, ParamFocus = 0x2F;
inline constexpr u8 ModuleNew = 0x30, ModuleRecolor = 0x31, ModuleDelete = 0x32, ModuleName = 0x33, ModuleMove = 0x34;
inline constexpr u8 SessionRequest = 0x35, SessionDump = 0x36, DumpDestination = 0x37, PatchRelease = 0x38;
inline constexpr u8 Leds = 0x39, Meters = 0x3A, ClockInfoRequest = 0x3B, CompletePatchRequest = 0x3C;
inline constexpr u8 DumpOneRequest = 0x3D, PerformanceMode = 0x3E, PerfHeaderParam = 0x3F, ParamChange = 0x40;
inline constexpr u8 CustomData = 0x42, MorphChange = 0x43, VariationCopy = 0x44;
inline constexpr u8 ModuleList = 0x4A, ModuleListRequest = 0x4B, ParamListRequest = 0x4C, ParamList = 0x4D;
inline constexpr u8 ModuleNamesRequest = 0x4E, CustomDataRequest = 0x4F, CableConnect = 0x50, CableDelete = 0x51;
inline constexpr u8 CableList = 0x52, CableListRequest = 0x53, CableRecolor = 0x54, SendCtrlSnap = 0x55;
inline constexpr u8 PlayNote = 0x56, PageFocusRequest = 0x58, GlobalPageFocusRequest = 0x59, ModuleNames = 0x5A;
inline constexpr u8 CustomDataDump = 0x5B, ClockInfo = 0x5D, GlobalKnobMapRequest = 0x5E, GlobalKnobMap = 0x5F;
inline constexpr u8 CtrlMap = 0x60, CtrlMapRequest = 0x61, KnobMap = 0x62, KnobMapRequest = 0x63;
inline constexpr u8 MorphMap = 0x65, MorphMapRequest = 0x66, CurrentNotesRequest = 0x68, CurrentNotes = 0x69;
inline constexpr u8 VariationSelect = 0x6A, TextpadRequest = 0x6E, Textpad = 0x6F, FlushBlink = 0x70;
inline constexpr u8 PatchLoadRequest = 0x71, PatchLoad = 0x72, KnobMapChanged = 0x74, GlobalKnobMapChanged = 0x75;
inline constexpr u8 EditorSync = 0x7D, Exception = 0x7E, Ack = 0x7F, MidiLearn = 0x80, MidiLearnRequest = 0x81;
inline constexpr u8 MutaLock = 0x90;
} // namespace id

// A request or notification without a body (02, 04, 06, 08, 10, 20, 28, 2C,
// 2E, 3B, 3C, 3D, 55, 58, 59, 5E, 61, 63, 66, 68, 6E, 70, 81; 74, 75).
struct Request {
    u8 id = 0;
};
// A request for one location of a patch (4B, 4C, 4E, 4F, 53 `loc`; 71 `isVA`).
struct LocationRequest {
    u8 id = 0;
    u8 location = 1; // 0 FX, 1 VA, 2 patch settings
};

// 03 CMSynthDataDump: the synth settings (§7.1, CSynthMap::WriteStream 0x120410).
struct SynthSettings {
    std::string name;
    bool perfMode = false;
    u8 patchSortMode = 0, perfSortMode = 0; // 7 bits each
    u8 focusBank = 0, focusProg = 0;
    bool memoryProtect = false;
    std::array<u8, 4> midiChannels{0, 1, 2, 3}; // 0..15, 16 = off
    u8 globalChannel = 15, sysExId = 16;          // 16 = off / all
    bool localOn = true;
    u8 programChangeMode = 1, controllerMode = 1; // bits rx << 1 | tx (Verhue)
    bool sendArp = false, sendClock = false, ignoreExternalClock = false;
    std::int8_t masterTune = 0; // cents
    bool octaveShiftEnable = false;
    std::int8_t octaveShift = 0, transpose = 0;
    u8 vibratoRate = 0;         // "unknown" in Verhue
    bool sustainPolarity = false;
    u8 controlPedalGain = 0;
    std::array<u8, 16> reserved{}; // written as zero
};
struct SynthData {
    SynthSettings settings;
};

// 05 CMNumberOfVoicesDump: voices assigned to slots A..D.
struct Voices {
    std::array<u8, 4> voices{};
};

// 07 CMSlotSelectionDump / 0F CMKeyboardFocusDump: one flag per slot A..D
// (pad4, b1 A, b1 B, b1 C, b1 D).
struct SlotFlags {
    u8 id = 0;
    std::array<bool, 4> slots{};
};

// 09 CMSlotFocusDump: the focused slot 0..3, or 4 = none.
struct SlotFocus {
    u8 slot = 0;
};

// Flash commands with (slot or type, bank, prog): 0A load, 0B store, 14 name
// list request, 15 delete dump, 17 dump request. `slot` is 0..3 or 4 (the
// performance) for 0A/0B, the type (0 patch, 1 performance) otherwise.
struct FlashCommand {
    u8 id = 0;
    u8 slot = 0, bank = 0, prog = 0;
};
// 0C CMFlashDeleteCommand.
struct FlashDelete {
    u8 type = 0, bank = 0, prog = 0, origin = 0;
};
// 0D CMFlashCommandResult: result 0 = ok.
struct FlashResult {
    u8 type = 0, bank = 0, prog = 0, origin = 0, result = 0;
};
// 0E CMFlashDeleteRangeCommand (with origin) / 12 CMFlashDeleteRangeDump (without).
struct FlashDeleteRange {
    u8 id = 0;
    u8 type = 0, bank1 = 0, prog1 = 0, bank2 = 0, prog2 = 0;
    u8 origin = 0; // 0E only
};
// 13 CMFlashUsageDump: lo | hi << 7, at most 1000 (usage in per mille [I]).
struct FlashUsage {
    std::uint16_t value = 0;
};
// 16 CMFlashDataDump: a chunk of a bank's name list.
struct FlashItem {
    enum class Kind : u8 { SetEntry = 3, SetProg = 1, Empty = 2, EndOfList = 4, EndOfChunk = 5, Name = 0 };
    Kind kind = Kind::Name;
    u8 bank = 0, prog = 0;  // SetEntry: bank, prog; SetProg: prog
    std::string name;       // Name
    u8 category = 0;        // Name
};
struct FlashData {
    u8 flag = 0, type = 0;
    std::vector<FlashItem> items; // ends with EndOfList or EndOfChunk
};
// 18 CMFlashDumpMarker: code 0 ok, 2 header (with the name), 3 memory full,
// 4 empty, 5 corrupt, others protected.
struct FlashDumpMarker {
    u8 code = 0, type = 0, bank = 0, prog = 0;
    std::string name; // code 2 only
};
// 19 CMFlashDumpRawData: the binary part of a .pch2/.prf2 file (from the
// version byte on, without the version itself); size 0xFFFF = no data
// (CMFlashDumpRawData::ReadStream 0x10f52: `size` counts the data bytes).
struct FlashRawData {
    u8 type = 0, bank = 0, prog = 0;
    std::string name;
    u8 version = 0;
    std::optional<std::vector<u8>> data;
};

// 1C CMGlobalKnobAssign (b2 assignType, b2 slot, b2 loc, pad2, module, param, u16 knob).
struct GlobalKnobAssign {
    u8 assignType = 0, slot = 0, location = 0, module = 0, param = 0;
    std::uint16_t knob = 0; // 0..119
};
// 1D CMGlobalKnobDeassign / 26 CMKnobDeassign: u16 knob.
struct KnobDeassign {
    u8 id = 0;
    std::uint16_t knob = 0;
};
// 1E CMGlobalParameterPageFocus / 2D CMParameterPageFocus: u8 page.
struct PageFocus {
    u8 id = 0;
    u8 page = 0;
};
// 1F CMPerformanceRelease: the synth has a new performance (§5.3).
struct PerformanceRelease {
    u8 session = 0;
};

// A .pch2 section as a molecule (11, 21, 4A, 4D, 52, 5A, 5B, 5F, 60, 62, 65,
// 69, 6F): id, u16 length, payload, as in files (re/notes/pch2-format.md).
struct SectionDump {
    file::Section section;
};

// 22 CMCtrlAssign: a MIDI CC to a parameter.
struct CtrlAssign {
    u8 location = 0, module = 0, param = 0, cc = 0;
};
// 23 CMCtrlDeassign.
struct CtrlDeassign {
    u8 cc = 0;
};
// 25 CMKnobAssign (module, param, b2 loc, b2 assignType, pad4, u16 knob).
struct KnobAssign {
    u8 module = 0, param = 0, location = 0, assignType = 0;
    std::uint16_t knob = 0; // 0..119
};
// 27 CMPatchNameDump / 29 CMPerformanceNameDump: str16.
struct NameDump {
    u8 id = 0;
    std::string name;
};
// 2A CMModuleABCode: a module's rate (uprate).
struct Uprate {
    u8 location = 0, module = 0, uprate = 0;
};
// 2B CMDSPPartChange: a module's mode ("part") value.
struct ModeChange {
    u8 location = 0, module = 0, mode = 0, value = 0;
};
// 2F CMParamFocusDump: the selected parameter (flag 0 when the editor sends it).
struct ParamFocus {
    u8 flag = 0, location = 0, module = 0, param = 0;
};
// 30 CMModuleNew: a module with its modes (as many as its type has) and name.
struct ModuleNew {
    u8 type = 0, location = 0, index = 0, col = 0, row = 0, color = 0, uprate = 0, locked = 0;
    std::vector<u8> modes;
    std::string name;
};
// 31 CMModuleRecolor.
struct ModuleRecolor {
    u8 location = 0, module = 0, color = 0;
};
// 32 CMModuleDelete.
struct ModuleDelete {
    u8 location = 0, module = 0;
};
// 33 CMModuleName.
struct ModuleRename {
    u8 location = 0, module = 0;
    std::string name;
};
// 34 CMModuleMove.
struct ModuleMove {
    u8 location = 0, module = 0, col = 0, row = 0;
};
// 35 CMSessionNumberRequest (slot 0..3, 4 = performance).
struct SessionRequest {
    u8 slot = 0;
};
// 36 CMSessionNumberDump / 38 CMSessionPatchRelease: (slot, session).
struct SessionNumber {
    u8 id = 0;
    u8 slot = 0, session = 0;
};
// 37 CMDumpBubbleDestination: heads an upload (always 00 00 00 name).
struct DumpDestination {
    u8 target = 0, bank = 0, prog = 0;
    std::string name;
};
// 39 / 3A: LED and meter data (§10): start index, then the rest of the message.
struct Blink {
    u8 id = 0;
    u8 start = 0;
    std::vector<u8> data;
};
// 3E CMPerformanceModeChange.
struct PerformanceMode {
    u8 perfMode = 0, flag = 0;
};
// 3F CMPerformanceHeaderParam: scope (0xFF = global), param, and a value when
// scope > 3 and param < 3 (0 clock run, 1 BPM, 2 keyboard range [I]).
struct PerfHeaderParam {
    u8 scope = 0, param = 0;
    std::optional<u8> value;
};
// 40 CMParamChange: variation 0..7, 8 = init.
struct ParamChange {
    u8 location = 0, module = 0, param = 0, value = 0, variation = 0;
};
// 42 CMCustomData: a module's custom data records.
struct CustomData {
    u8 location = 0, module = 0;
    std::vector<u8> bytes;
};
// 43 CMMorphChange: |range| and a sign byte on the wire.
struct MorphChange {
    u8 location = 0, module = 0, param = 0, morph = 0;
    int range = 0; // -127..127
    u8 variation = 0;
};
// 44 CMParamSettingCopy (8 = init variation).
struct VariationCopy {
    u8 from = 0, to = 0;
};
// 50 CMCableConnect / 51 CMCableDelete / 54 CMCableRecolor.
struct CableEdit {
    u8 id = 0;
    bool va = true;
    u8 color = 0; // 50, 54
    u8 fromModule = 0, fromConn = 0;
    bool fromIsOutput = true;
    u8 toModule = 0, toConn = 0;
    bool toIsOutput = false;
};
// 56 CMPlayNote: no velocity, no channel (§12).
struct PlayNote {
    bool off = false;
    u8 note = 60;
};
// 5D CMSysClockInfo.
struct ClockInfo {
    u8 flag = 0;
    std::uint16_t bpm = 120;
};
// 6A CMParamSettingFocus: the selected variation.
struct VariationSelect {
    u8 variation = 0;
};
// 72 CMPatchLoad: DSP resource counters of one area (27 bytes after isVA:
// u14, u14, u8, u14, 6 x u14, u14, u16, u16, u14; u14 = 7-bit BE pair).
struct PatchLoad {
    u8 isVA = 1;
    std::array<u8, 27> counters{};
};
// 7D CMSynthUnlockEditorSync: 1 = editor sync begins, 0 = done.
struct EditorSync {
    bool begin = true;
};
// 7E kMesException: the synth reports an error (§11.2) and nothing follows.
struct Exception {
    u8 code = 0;
};
// 7F kMesAck: end of a reply; nothing follows.
struct Ack {};
// 80 CMMidiLearn: slot 0..4, the CC number last received.
struct MidiLearn {
    u8 slot = 0, cc = 0;
};
// 90 CMMutaLock.
struct MutaLock {
    u8 location = 0, module = 0, locked = 0;
};
// 1A CMCompletePerformanceDump: the performance name, header section, the 4
// patches' 18 sections each, and the global knob map (§8.1).
struct CompletePerformance {
    std::string name;
    std::vector<file::Section> sections; // 11, 4 x 18 patch sections, 5F
};

// An id this codec does not know, or a malformed molecule: the rest of the message.
struct Raw {
    u8 id = 0;
    std::vector<u8> body;
};

using Molecule = std::variant<Request, LocationRequest, SynthData, Voices, SlotFlags, SlotFocus, FlashCommand,
                              FlashDelete, FlashResult, FlashDeleteRange, FlashUsage, FlashData, FlashDumpMarker,
                              FlashRawData, GlobalKnobAssign, KnobDeassign, PageFocus, PerformanceRelease, SectionDump,
                              CtrlAssign, CtrlDeassign, KnobAssign, NameDump, Uprate, ModeChange, ParamFocus, ModuleNew,
                              ModuleRecolor, ModuleDelete, ModuleRename, ModuleMove, SessionRequest, SessionNumber,
                              DumpDestination, Blink, PerformanceMode, PerfHeaderParam, ParamChange, CustomData,
                              MorphChange, VariationCopy, CableEdit, PlayNote, ClockInfo, VariationSelect, PatchLoad,
                              EditorSync, Exception, Ack, MidiLearn, MutaLock, CompletePerformance, Raw>;

u8 idOf(const Molecule& m);
// A section molecule from a decoded section.
Molecule sectionMolecule(file::Section section);

// Appends a molecule's bytes. Throws std::invalid_argument if a field does not
// fit its width (a programming error, not a wire condition).
void encode(const Molecule& m, std::vector<u8>& out);
std::vector<u8> encode(std::span<const Molecule> molecules);
// Decodes the molecules of a message body. Never throws (see above). 7F (Ack)
// and 7E (Exception) end a reply for Clavia, which ignores what follows; they
// are decoded like the others here and the client stops at them.
std::vector<Molecule> decode(std::span<const u8> body);

} // namespace g2::proto
