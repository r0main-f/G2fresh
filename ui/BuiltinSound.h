// The patch played by G2fresh itself (the plugin's or app's own sound engine)
// when no G2 plays it, as the editor sees it: on/off and a status line.
// Implemented by the plugin processor, so the editor needs no audio code.
#pragma once

#include <juce_core/juce_core.h>

namespace g2ui {

class BuiltinSound {
public:
    virtual ~BuiltinSound() = default;
    virtual bool builtinSoundEnabled() const = 0;
    virtual void setBuiltinSoundEnabled(bool on) = 0;
    // What plays and what does not ("2 modules not supported yet (...)").
    virtual juce::String builtinSoundStatus() const = 0;

    // The emulated G2: the user's own G2 OS (from Clavia's free OS updater)
    // running in G2fresh, which then plays any patch. Available when G2fresh
    // is built with the emulator (G2_BUILD_EMU).
    virtual bool emulatorAvailable() const { return false; }
    // Starts it from the OS updater at `firmware` and connects the editor to
    // it (SynthSync::connectEmulated). Returns why not, or "" when started.
    virtual juce::String startEmulator(const juce::File& firmware)
    {
        juce::ignoreUnused(firmware);
        return "this build has no emulator";
    }
};

} // namespace g2ui
