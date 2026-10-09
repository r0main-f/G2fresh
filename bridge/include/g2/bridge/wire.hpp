// The messages between g2bridge and its editors.
//
// Framing: [u32 little-endian length][type u8][payload], length counting the
// type and payload, at most kMaxMessage. Integers in payloads are little-
// endian; `bytes` and `string` are a u32 length then the data; molecules are
// their USB encoding (g2::proto::encode). A receiver ignores message types and
// trailing payload bytes it does not know, so the format can grow; a new
// incompatible format changes kProtocolVersion.
//
//   editor -> bridge
//     Hello     "G2BR" u16 version
//     Op        u8 op, then the call's arguments (see Op)
//   bridge -> editor
//     Welcome   u16 version, u32 client id
//     Status    u8 status, u8 synced, string status line
//     Synth     the synth-level state (snapshot; replaces everything but the slots)
//     Slot      u8 slot, then one slot's state (snapshot)
//     Applied   u8 target, molecule: applied to the bridge's mirror (apply it too)
//     Event     u8 kind, u8 slot, u8 value, bytes molecule: a listener callback
//
// After Hello, the bridge sends Welcome, a full snapshot (Synth, then Slot for
// A..D) and Status. Afterwards every change to its mirror arrives as Applied
// (molecule by molecule) or as a new snapshot (a slot downloaded, a new
// connection), in the order the bridge's Client made it, and listener
// callbacks as Event. An editor's own edits come back to it as Applied too
// (its mirror follows the bridge); events for them go to the other editors.
#pragma once

#include "g2/proto/state.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace g2::bridge {

using u8 = std::uint8_t;

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint32_t kMaxMessage = 64u << 20;

enum class MessageType : u8 {
    Hello = 0x01,
    Op = 0x02,
    Welcome = 0x81,
    Status = 0x82,
    Synth = 0x83,
    Slot = 0x84,
    Applied = 0x85,
    Event = 0x86,
};

// SynthLink calls, in Op messages (arguments in this order, slots as u8).
enum class Op : u8 {
    Restart = 1,
    Resync = 2,
    ResyncSlot = 3,        // slot
    SendPatch = 4,         // slot, bytes .pch2, string name
    SendPerformance = 5,   // bytes .prf2, string name
    SetParam = 6,          // slot, location, module, param, value, variation
    SelectParam = 7,       // slot, location, module, param
    SetMorph = 8,          // slot, location, module, param, group, i16 range, variation, dragging
    SelectVariation = 9,   // slot, variation
    CopyVariation = 10,    // slot, from, to
    SetMode = 11,          // slot, location, module, mode, value
    AssignKnob = 12,       // slot, u16 knob, location, module, param
    DeassignKnob = 13,     // slot, u16 knob
    AssignMidiCc = 14,     // slot, cc, location, module, param
    DeassignMidiCc = 15,   // slot, cc
    SelectSlot = 16,       // slot
    LoadFromFlash = 17,    // slot, bank, prog
    StoreToFlash = 18,     // slot, bank, prog
    PlayNote = 19,         // note, on
    Edit = 20,             // slot, bytes molecules
};

// Listener callbacks, in Event messages.
enum class EventKind : u8 {
    SlotChanged = 1,
    SynthChanged = 2,
    PerformanceChanged = 3,
    FlashNamesChanged = 4, // value: type
    ParamChanged = 5,      // molecule: ParamChange
    MorphChanged = 6,      // molecule: MorphChange
    VariationChanged = 7,  // value: variation
    ParamFocused = 8,      // molecule: ParamFocus
    PatchEdited = 9,       // molecule
    LedsChanged = 10,
    MidiLearned = 11,      // molecule: MidiLearn
    Error = 12,            // value: exception code
};

// ---- Encoding ----------------------------------------------------------------

class Writer {
public:
    explicit Writer(MessageType type);
    Writer& u8(std::uint8_t v);
    Writer& u16(std::uint16_t v);
    Writer& u32(std::uint32_t v);
    Writer& bytes(std::span<const std::uint8_t> data);
    Writer& string(const std::string& s);
    Writer& molecule(const proto::Molecule& m); // as bytes
    // The framed message.
    std::vector<std::uint8_t> finish();

private:
    std::vector<std::uint8_t> out_;
};

// Reads a payload; reading past the end gives zeros / empty values and sets
// failed() instead of throwing.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> payload) : data_(payload) {}
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::span<const std::uint8_t> bytes();
    std::string string();
    std::optional<proto::Molecule> molecule();
    bool failed() const { return failed_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Cuts a byte stream into messages.
class MessageBuffer {
public:
    struct Message {
        MessageType type;
        std::vector<std::uint8_t> payload;
    };
    void append(std::span<const std::uint8_t> data);
    // The next complete message, or nullopt. Sets corrupt() (and returns
    // nullopt from then on) for a length over kMaxMessage or zero.
    std::optional<Message> next();
    bool corrupt() const { return corrupt_; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool corrupt_ = false;
};

// ---- State snapshots ---------------------------------------------------------

std::vector<std::uint8_t> synthSnapshot(const proto::SynthState& s);
std::vector<std::uint8_t> slotSnapshot(const proto::SynthState& s, int slot);
// Apply a snapshot's payload. Return false if it cannot be read.
bool applySynthSnapshot(proto::SynthState& s, std::span<const std::uint8_t> payload);
bool applySlotSnapshot(proto::SynthState& s, std::span<const std::uint8_t> payload);

// Applies a molecule the bridge's Client applied to its mirror (see
// Client::Observer::applied): slot targets with applyToSlot, the synth level
// with applyToSynth plus session numbers, PerformanceRelease and MidiLearn.
void applyMirrored(proto::SynthState& s, u8 target, const proto::Molecule& m);

} // namespace g2::bridge
