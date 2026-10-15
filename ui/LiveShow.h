// What showing the Live view does about the emulated G2 (MainView::setLiveMode). Plain C++.
#pragma once

namespace g2ui {

enum class LiveShow {
    Start,         // start it: the panel is the emulated G2's, and a G2 is on when you sit at it
    Nothing,       // it runs
    NoEmulator,    // this build has none
    OtherSynth,    // the editor talks to a G2 on USB or the virtual one: starting would take the editor from it
    OtherInstance, // another instance in the host runs one: two at once lost their audio on an M1
                   // (re/notes/audio-path.md §3.6), so a second starts only when asked, as when a project opens
};

inline LiveShow liveShow(bool emulatorAvailable, bool running, bool otherSynth, int othersInHost)
{
    if (!emulatorAvailable)
        return LiveShow::NoEmulator;
    if (running)
        return LiveShow::Nothing;
    if (otherSynth)
        return LiveShow::OtherSynth;
    if (othersInHost > 0)
        return LiveShow::OtherInstance;
    return LiveShow::Start;
}

} // namespace g2ui
