// The emulated G2 as the editor sees it: the user's own G2 OS (from Clavia's
// free OS updater) running in G2fresh, which plays the patch. Implemented by
// the plugin processor, so the editor needs no audio or emulator code.
// Available when G2fresh is built with the emulator (G2_BUILD_EMU).
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <span>

namespace g2ui {

class EmulatorHost {
public:
    virtual ~EmulatorHost() = default;
    virtual bool emulatorAvailable() const { return false; }
    // Starts it from the OS updater at `firmware` and connects the editor to
    // it (SynthSync::connectEmulated). Returns why not, or "" when started.
    virtual juce::String startEmulator(const juce::File& firmware)
    {
        juce::ignoreUnused(firmware);
        return "this build has no emulator";
    }
    // Whether it runs now.
    virtual bool emulatorRunning() const { return false; }
    // MIDI into its MIDI IN (e.g. the on-screen keyboard), from the message
    // thread. Ignored while it does not run.
    virtual void emulatorMidi(std::span<const std::uint8_t> bytes) { juce::ignoreUnused(bytes); }
};

} // namespace g2ui
