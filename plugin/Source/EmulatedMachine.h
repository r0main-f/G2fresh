#pragma once
// The emulated G2 the plugin runs (EmulatedSoundEngine.cpp), in one place so that tools/audiopath measures that same
// machine (tests/CMakeLists.txt: audiopath_runner_is_the_plugins_machine). Plain C++.

#include "g2emu/machine.hpp"

namespace g2io {

inline void pluginMachine(g2emu::Machine::Options& m)
{
    m.model = g2emu::PanelModel::G2X; // the Live view is a G2X: 61 keys and the two global wheels
    // ...without its voice expansion board (a deliberate deviation: a G2's polyphony, four DSPs to emulate instead of
    // eight: on an M1, a heavy patch on eight runs below real time, re/notes §3.12.5)
    m.expansion = g2emu::Expansion::Without;
    // The DSPs on two threads of their own where the computer has 4 performance cores, else one
    // (Machine::autoThreads): two were faster for every patch measured, at the same CPU time
    m.threads = -1;
}

} // namespace g2io
