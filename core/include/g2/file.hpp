// Lossless codec for G2 patch (.pch2) and performance (.prf2) files, format
// versions 13..23. Every field is kept, including reserved bits, padding and
// the bytes Clavia's writer leaves uninitialised, so that write(read(x)) == x.
// Spec: re/notes/pch2-format.md. Reference implementation: tools/pch2/pch2dump.py.
#pragma once

#include "g2/bitstream.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace g2::file {

using u8 = std::uint8_t;

inline constexpr std::uint16_t kCurrentVersion = 23;

enum SectionId : u8 {
    kPerfHeader = 0x11,
    kPatchHeader = 0x21,
    kModuleList = 0x4A,
    kParamList = 0x4D,
    kCableList = 0x52,
    kModuleNames = 0x5A,
    kCustomData = 0x5B,
    kGlobalKnobMap = 0x5F,
    kCtrlMap = 0x60,
    kKnobMap = 0x62,
    kMorphMap = 0x65,
    kCurrentNotes = 0x69,
    kTextpad = 0x6F,
};

// $21 CPatchHeaderData_11 (15 bytes).
struct PatchHeader {
    // 12 fields left over from the NM1 header, always 0 in v1.62 files.
    std::array<u8, 12> legacy{};
    u8 voiceCount = 1;                // 1..32
    std::uint16_t splitterPos = 600;  // 14 bits
    std::int8_t octaveShift = 0;      // -2..+2
    std::array<bool, 7> cablesVisible{true, true, true, true, true, true, true};
    u8 monoMode = 0;                  // 2 bits
    u8 activeVariation = 0;
    u8 category = 0;
    u8 pad = 0;
    u8 reservedByte = 0;
};

// $4A CModuleData_11.
struct ModuleEntry {
    u8 type = 0;
    u8 index = 0;
    u8 col = 0;      // 7 bits
    u8 row = 0;      // 7 bits
    u8 color = 0;
    u8 uprate = 0;   // 1 bit
    u8 locked = 0;   // 1 bit; Verhue's "IsLed"
    u8 reserved = 0; // 6 bits
    std::vector<u8> modes; // up to 15 values of 6 bits
};
struct ModuleList {
    u8 location = 0;
    std::vector<ModuleEntry> modules;
};

// $69 CCurrentVoiceData_11.
struct Voice {
    u8 note = 64, attackVel = 0, releaseVel = 0;
};
struct CurrentNotes {
    Voice last;
    std::vector<Voice> notes{Voice{}}; // 1..32
};

// $52 CCableData_11.
struct Cable {
    u8 color = 0;        // 3 bits
    u8 fromModule = 0;
    u8 fromConn = 0;     // 6 bits
    u8 fromIsOutput = 1; // 0 = input-to-input link
    u8 toModule = 0;
    u8 toConn = 0;       // 6 bits
};
struct CableList {
    u8 location = 0;
    u8 pad = 0;
    std::vector<Cable> cables;
};

// $4D CModuleParamData_11.
struct ParamVariation {
    u8 variation = 0;
    std::vector<u8> values; // 7 bits each
};
struct ParamModule {
    u8 index = 0;
    u8 paramCount = 0;
    std::vector<ParamVariation> variations;
};
struct ParamList {
    u8 location = 0;
    u8 variationCount = 0;
    std::vector<ParamModule> modules;
};

// $65 CMorphMapData_11.
struct MorphAssign {
    u8 location = 0, module = 0, param = 0, morph = 0;
    std::int8_t range = 0;
};
struct MorphVariation {
    u8 variation = 0;
    std::vector<u8> legacyDials; // morphCount values, always 0
    std::vector<MorphAssign> morphs;
};
struct MorphMap {
    u8 morphCount = 8;
    std::vector<u8> keyboardAssign; // morphCount values
    std::vector<MorphVariation> variations;
};

// $62 / $5F CKnobMapData_11.
struct KnobAssign {
    u8 location = 0, module = 0, assignType = 0, param = 0;
    u8 slot = 0; // global knob map only
};
struct KnobMap {
    std::vector<std::optional<KnobAssign>> knobs; // 120 entries
};

// $60 CCtrlMapData_11.
struct CtrlAssign {
    u8 cc = 0, location = 0, module = 0, param = 0;
};
struct CtrlMap {
    std::vector<CtrlAssign> controllers;
};

// $5B CModuleCustomData_11: per module, a stream of [kind, len, payload] records.
struct CustomModule {
    u8 index = 0;
    std::vector<u8> bytes;
};
struct CustomData {
    u8 location = 0;
    std::vector<CustomModule> modules;
};

// $5A CModuleNameData_11. Names are byte strings (Mac Roman / Latin-1).
struct ModuleName {
    u8 index = 0;
    std::string name; // up to 16 bytes
};
struct ModuleNames {
    u8 location = 0;
    u8 reserved = 0; // 6 bits Clavia's writer skips without writing
    std::vector<ModuleName> names;
};

// $6F CTextpadData_11.
struct Textpad {
    std::string text;
};

// $11 CPerformanceHeader_11.
struct SlotSettings {
    std::string patchName;
    u8 enabled = 1, keyboard = 0, hold = 0, bank = 0, program = 0;
    u8 kbdRangeLower = 0, kbdRangeUpper = 127, midiChannel = 0;
    u8 reserved1 = 0, reserved2 = 0;
};
struct PerfHeader {
    u8 unknown08 = 0;
    u8 focusedSlot = 0; // 6 bits
    u8 globalPages = 0; // 2 bits
    u8 kbdRangeEnabled = 0, masterClockBpm = 120, unknown18 = 0, masterClockRun = 0;
    u8 reserved1 = 0, reserved2 = 0;
    std::array<SlotSettings, 4> slots;
};

// A section whose payload the field codec could not reproduce exactly.
struct RawSection {
    std::vector<u8> payload;
};

using Payload = std::variant<RawSection, PatchHeader, ModuleList, CurrentNotes, CableList,
                             ParamList, MorphMap, KnobMap, CtrlMap, CustomData, ModuleNames,
                             Textpad, PerfHeader>;

struct Section {
    u8 id = 0;
    Payload payload;
    u8 tailPad = 0;          // non-zero bits padding the payload's last byte
    std::vector<u8> trailing; // bytes after the decoded fields
};

enum class FileType : u8 { Patch = 0, Performance = 1 };

struct File {
    std::vector<std::string> textHeader;
    std::uint16_t version = kCurrentVersion;
    FileType type = FileType::Patch;
    std::vector<Section> sections;
    std::vector<u8> unparsedTail;
    std::uint16_t storedCrc = 0;
    bool crcValid = true; // false: write() keeps storedCrc as found
};

// Parses a whole file. Throws FormatError when the container is unreadable
// (text header, lengths, CRC position). Sections that fail to decode become
// RawSection; a bad CRC is reported through crcValid.
File read(std::span<const u8> data);
std::vector<u8> write(const File& file);

// Section payload codec, exposed for tests and the USB layer (which reuses
// the same encodings).
Section decodeSection(u8 id, std::span<const u8> payload);
std::vector<u8> encodeSection(const Section& section);

std::vector<std::string> defaultTextHeader(FileType type);

} // namespace g2::file
