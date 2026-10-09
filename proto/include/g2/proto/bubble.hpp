// Bubbles: the unit of editor -> synth traffic (re/notes/usb-protocol.md §4.2,
// §5.1). A bubble is a list of molecules for one destination (a patch slot, or
// the synth/performance level) under one session number; it becomes one or
// more bulk-OUT frames.
#pragma once

#include "g2/proto/frame.hpp"
#include "g2/proto/molecules.hpp"

#include <optional>
#include <vector>

namespace g2::proto {

struct Bubble {
    u8 slot = kSlotSynth;      // 0..3 patch slot A..D, 4 synth / performance
    std::optional<u8> session; // nullopt: void session (0x40 | molecule count)
    bool realtime = false;     // fire-and-forget, never answered
    std::vector<Molecule> molecules;

    // The kinds of §5.1.
    static Bubble synth(std::vector<Molecule> molecules);                     // S: 0x2C, void
    static Bubble performance(u8 perfSession, std::vector<Molecule> molecules); // P: 0x2C, perf session
    static Bubble patch(u8 slot, u8 slotSession, std::vector<Molecule> molecules); // T: 0x28|slot
    static Bubble patchVoid(u8 slot, std::vector<Molecule> molecules);        // T-void: patch upload
    static Bubble realtimeFor(u8 slot, u8 slotSession, std::vector<Molecule> molecules); // RT: 0x38|slot
};

// The bulk-OUT frames of a bubble (CBubblePort::GenerateMessages 0x11828a,
// GenerateRealtimeMessages 0x118536):
// - realtime: one frame per molecule, hdr 0x38|slot, the bubble's session;
// - otherwise the molecules back to back, split at molecule boundaries into
//   frames of at most 0xFFFF bytes, Begin (0x20) on the first and End (0x08)
//   on the last; a void session byte counts the molecules ending in each frame.
// Editor-only molecules (CMMutaLock's local twins) do not exist here.
std::vector<std::vector<u8>> framesFor(const Bubble& bubble);

} // namespace g2::proto
