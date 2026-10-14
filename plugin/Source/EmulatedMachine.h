#pragma once
// The emulated G2 the plugin runs (EmulatedSoundEngine.cpp), in one place so that tools/audiopath measures that same
// machine (tests/CMakeLists.txt: audiopath_runner_is_the_plugins_machine). Plain C++.

#include "g2emu/machine.hpp"

#include <span>
#include <string_view>
#include <utility>

namespace g2io {

// `expansion`: the user fitted the voice expansion board (Synth > Emulated G2 Voice Expansion Board);
// `expansionThreads`: the DSP threads the computer test found fastest with it (-1: Machine::autoThreads).
inline void pluginMachine(g2emu::Machine::Options& m, bool expansion = false, int expansionThreads = -1)
{
    m.model = g2emu::PanelModel::G2X; // the Live view is a G2X: 61 keys and the two global wheels
    // ...without its voice expansion board unless the user fits it (a deliberate deviation: a G2's polyphony, four
    // DSPs to emulate instead of eight: on an M1, a heavy patch on eight runs below real time, re/notes §3.12.5)
    m.expansion = expansion ? g2emu::Expansion::With : g2emu::Expansion::Without;
    // The DSPs on two threads of their own where the computer has 4 performance cores, else one
    // (Machine::autoThreads): two were faster for every patch measured, at the same CPU time
    m.threads = expansion && expansionThreads > 0 ? expansionThreads : -1;
}

// The choice in the user's settings file, and back (anything else: without, the default).
inline constexpr std::string_view expansionKey(bool expansion) { return expansion ? "on" : "off"; }
inline constexpr bool expansionFromKey(std::string_view key) { return key == "on"; }

// What the computer test (g2emu::measureSpeed: the bench pack's B13 and a seven-note chord on eight DSPs, off-line)
// tells the user. Real time in a host wants about 1.5 x off-line (re/notes §3.9.7); below 1 the machine cannot keep up
// at all. On an M1 on 2026-10-14, eight DSPs at 0.82 x lost all of the audio in real time; four at 2.0 x lost none
// beside other programs, but did beside a host's busy audio threads (re/notes §3.12.5, audio-path.md §3).
enum class ExpansionAdvice { Holds, QuietOnly, TooSlow };
inline constexpr double kExpansionHoldsSpeed = 1.5, kExpansionQuietSpeed = 1.0;

inline constexpr ExpansionAdvice expansionAdvice(double speed)
{
    return speed >= kExpansionHoldsSpeed ? ExpansionAdvice::Holds
         : speed >= kExpansionQuietSpeed ? ExpansionAdvice::QuietOnly
                                         : ExpansionAdvice::TooSlow;
}

// The computer test: the chord held (B13 then plays seven voices on eight DSPs), the DSP threads tried, the emulated
// seconds measured with each (after about 2 s of booting and sending the patch).
inline constexpr int kExpansionTestNotes[] = {48, 55, 60, 64, 67, 72, 76};
inline constexpr int kExpansionTestThreads[] = {2, 3};
inline constexpr double kExpansionTestSeconds = 3.0;

// Of the DSP thread counts tried, (threads, speed), the fastest (-1: none tried).
inline int fastestThreads(std::span<const std::pair<int, double>> tried)
{
    int best = -1;
    double speed = -1;
    for (const auto& [threads, s] : tried)
        if (s > speed) {
            best = threads;
            speed = s;
        }
    return best;
}

} // namespace g2io
