// What an editor knows about a synth, and what the emulator holds: the
// performance, the four slots' patches, the synth settings and the small
// pieces of panel state the protocol carries. Molecules apply to it the same
// way on both sides (re/notes/usb-protocol.md §7, §9, §10).
#pragma once

#include "g2/patch.hpp"
#include "g2/proto/frame.hpp"
#include "g2/proto/molecules.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace g2::proto {

inline constexpr int kSlots = 4;
inline constexpr int kLedsPerSlot = 40; // 0x39 and 0x3A entries per slot

struct SlotState {
    Patch patch;
    std::string name;            // the patch name (27)
    u8 session = 0;
    bool sessionKnown = false;
    u8 page = 0;                 // parameter page focus (2D)
    ParamFocus focus;            // selected parameter (2F)
    std::array<std::optional<PatchLoad>, 2> load; // 72 for FX (0) and VA (1)
    std::array<u8, kLedsPerSlot> leds{};          // 0x39: 0..3 per LED
    std::array<std::uint16_t, kLedsPerSlot> meters{}; // 0x3A: one word per entry
};

// One name of a flash bank list (16).
struct FlashName {
    u8 bank = 0, prog = 0;
    std::string name;
    u8 category = 0;
};

struct SynthState {
    VersionInfo version;
    SynthSettings settings;
    std::array<u8, 4> voices{};
    ClockInfo clock;
    std::optional<MidiLearn> midiLearn;

    std::string perfName;
    file::PerfHeader perfHeader;
    std::vector<std::optional<file::KnobAssign>> globalKnobs =
        std::vector<std::optional<file::KnobAssign>>(kKnobCount); // 5F
    u8 perfSession = 0;
    bool perfSessionKnown = false;
    u8 slotFocus = 0;        // 09: 0..3, 4 = none
    u8 globalPage = 0;       // 1E
    std::array<bool, 4> slotEnabled{true, true, true, true};  // 07
    std::array<bool, 4> keyboardEnabled{true, false, false, false}; // 0F

    std::array<SlotState, kSlots> slots;
    std::array<std::vector<FlashName>, 2> flash; // name lists: [0] patches, [1] performances
    std::optional<std::uint16_t> flashUsage;     // 13

    // The performance as a file model (header, the slots' patches, global knobs).
    Performance performance() const;
    // Replaces everything a performance holds.
    void setPerformance(const Performance& perf, const std::string& name);
};

// Applies a patch-level molecule (destination 2) to a patch: parameter, morph,
// variation, mode, knob / controller assignment, module and cable edits, custom
// data, and sections sent as edits. Sections that list modules (4A, 4D, 52,
// 5A, 5B) merge into the patch, as an editor's paste sends only the pasted
// modules (Verhue's param paste: one 4D with one module and variation); the
// others (21, 60, 62, 65, 69, 6F) replace the patch's. Returns false for
// molecules that do not edit a patch. Edits naming a module or parameter the
// patch does not have are ignored (they return true).
bool applyToPatch(Patch& patch, const Molecule& m);
// Applies a molecule to a slot: applyToPatch, plus the slot's name (27), page
// (2D), parameter focus (2F), load report (72) and LEDs (39 / 3A).
bool applyToSlot(SlotState& slot, const Molecule& m);
// Applies a synth- or performance-level molecule (03, 05, 07, 09, 0F, 11, 13,
// 1C, 1D, 1E, 29, 3F, 5D, 5F, 80). Returns false for others.
bool applyToSynth(SynthState& state, const Molecule& m);

// LED decoding (§10). 0x39: 2 bits per LED, 4 per byte, LSB first, from `start`.
void decodeLeds(const Blink& b, std::array<u8, kLedsPerSlot>& leds);
std::vector<u8> encodeLeds(std::span<const u8> leds); // from index 0
// 0x3A: one 16-bit word per entry from `start`. The byte order is an open
// question (§14.4): Clavia reads a raw little-endian i386 ushort, Verhue takes
// the second byte as the value. Read here as big-endian, like the golden
// vectors' generator [I]; see the hardware checklist.
void decodeMeters(const Blink& b, std::array<std::uint16_t, kLedsPerSlot>& meters);
std::vector<u8> encodeMeters(std::span<const std::uint16_t> meters);

} // namespace g2::proto
