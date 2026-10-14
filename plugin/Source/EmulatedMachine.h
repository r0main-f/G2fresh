#pragma once
// The emulated G2 the plugin runs (EmulatedSoundEngine.cpp), in one place so that tools/audiopath measures that same
// machine (tests/CMakeLists.txt: audiopath_runner_is_the_plugins_machine). Plain C++.

#include "g2emu/machine.hpp"

#include <string_view>

namespace g2io {

// `expansion`: the user fitted the voice expansion board (Synth > Emulated G2 Voice Expansion Board).
inline void pluginMachine(g2emu::Machine::Options& m, bool expansion = false)
{
    m.model = g2emu::PanelModel::G2X; // the Live view is a G2X: 61 keys and the two global wheels
    // ...without its voice expansion board unless the user fits it (a deliberate deviation: a G2's polyphony, four
    // DSPs to emulate instead of eight: on an M1, a heavy patch on eight runs below real time, re/notes §3.12.5)
    m.expansion = expansion ? g2emu::Expansion::With : g2emu::Expansion::Without;
    // The DSPs on two threads of their own where the computer has 4 performance cores, else one
    // (Machine::autoThreads): two were faster for every patch measured, at the same CPU time
    m.threads = -1;
}

// The choice in the user's settings file, and back (anything else: without, the default).
inline constexpr std::string_view expansionKey(bool expansion) { return expansion ? "on" : "off"; }
inline constexpr bool expansionFromKey(std::string_view key) { return key == "on"; }

} // namespace g2io
