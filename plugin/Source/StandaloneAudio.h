#pragma once
// The audio device the stand-alone app starts on (StandaloneApp.cpp). Plain C++.
//
// JUCE's stand-alone holder opens the system's default input as well as its output when a plugin has inputs and
// nothing was chosen yet. Opening an input is not free: a Bluetooth headset's microphone switches the headset to its
// call profile, which changes the sound of every application, and macOS asks for the microphone. A G2 plays its
// IN 1-4 only where a patch has an In module, so the app starts on its output alone until the user chooses an input
// (Options > Audio/MIDI Settings), and keeps that choice from then on.

#include <algorithm>
#include <optional>
#include <span>
#include <string>

namespace g2io {

// The devices of the setup the holder saved last ("audioSetup" in the settings).
struct SavedAudioSetup {
    std::string input, output; // empty: none
    bool byG2fresh = false;    // written by standaloneStart's caller at an earlier start, not chosen by the user
};

struct StandaloneStart {
    std::string output; // the output to open, with no input
};

// `saved`: the saved setup (null: never set up); `outputs`: the output devices present; `systemOutput`: the system's.
// Nothing (the saved setup as it is) when the user chose an input; else the output to open alone: the saved one if
// the user chose it and it is still there, otherwise the system's (as JUCE follows it while nothing is chosen; its
// own fallback for a missing device would open the default input).
inline std::optional<StandaloneStart> standaloneStart(const SavedAudioSetup* saved, std::span<const std::string> outputs,
                                                      const std::string& systemOutput)
{
    if (saved != nullptr && !saved->input.empty())
        return std::nullopt;
    if (saved != nullptr && !saved->byG2fresh && std::find(outputs.begin(), outputs.end(), saved->output) != outputs.end())
        return StandaloneStart{saved->output};
    return StandaloneStart{systemOutput};
}

} // namespace g2io
