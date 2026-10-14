#pragma once
// How fast this computer runs the emulated G2 with a patch playing (re/notes/g2-hardware-and-emulation.md §3.12.5):
// the plugin's test of the computer before the user fits the voice expansion board, and the same measurement as
// tools/firmware/g2emubench.py (g2emurun --kbd PATCH with a chord), in-process.

#include "g2/patch.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"

#include <atomic>
#include <span>
#include <string>

namespace g2emu {

struct SpeedTest {
    std::string error;            // why it did not measure ("stopped": asked to); empty when it did
    double speed = 0;             // emulated seconds per wall second while the chord played (off-line: as fast as it goes)
    double busiestDspThread = 0;  // CPU seconds per emulated second of the busiest DSP thread, its waits left out: above
                                  // 1, that thread alone cannot keep real time (threads > 0)
    int dsps = 0, threads = 0;    // the machine it ran: its DSPs, its DSP threads as resolved
    int voices = 0;               // the voices the OS gave slot A
    float peak = 0;               // the output's peak while it measured: the patch sounded
};

// Boots a machine with `options`, sends `patch` as a performance with slot A on the keyboard (as the editor does),
// holds `notes` on MIDI channel 1 and measures over `seconds` emulated seconds. About 2 s of booting and sending
// before the measurement, at the machine's speed. `stop` (any thread) ends it early, with the error "stopped".
SpeedTest measureSpeed(const Firmware& firmware, const Machine::Options& options, const g2::Patch& patch,
                       std::span<const int> notes, double seconds, const std::atomic<bool>* stop = nullptr);

} // namespace g2emu
