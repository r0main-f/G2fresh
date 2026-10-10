// The emulated G2 as the editor sees it: the user's own G2 OS (from Clavia's
// free OS updater) running in G2fresh, which plays the patch. Implemented by
// the plugin processor, so the editor needs no audio or emulator code.
// Available when G2fresh is built with the emulator (G2_BUILD_EMU).
#pragma once

#include "G2Panel.h"

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

    // ---- Its front panel (the Live view), from the message thread ----------
    // What the panel shows now (live = false: not running, or no panel).
    virtual PanelSnapshot panel() const { return {}; }
    virtual void panelButton(PanelButton button, bool down) { juce::ignoreUnused(button, down); }
    // An endless encoder turned: 0-7 the knobs, kPanelDial the rotary dial;
    // steps > 0 clockwise.
    virtual void panelEncoder(int encoder, int steps) { juce::ignoreUnused(encoder, steps); }
    // A continuous control moved: 0 .. 1 (the pitch stick rests at 0.5).
    virtual void panelAnalog(PanelAnalog control, float value) { juce::ignoreUnused(control, value); }
};

} // namespace g2ui
