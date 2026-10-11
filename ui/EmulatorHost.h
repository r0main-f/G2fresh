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
    // Whether the host's tempo and transport drive the emulated G2's master clock (MIDI clock into its MIDI IN).
    virtual bool hostClock() const { return false; }
    virtual void setHostClock(bool on) { juce::ignoreUnused(on); }
    // How many emulated G2s run in this host process (all plugin instances).
    virtual int emulatorsInHost() const { return 0; }
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
    // A key of the panel's keyboard (the G2X's 61 keys, C1-C6: MIDI notes 36-96), velocity 0 .. 1. The OS plays it
    // as its own keyboard (focus, octave shift, KB Hold, split); false when there is no emulated panel.
    virtual bool panelKey(int midiNote, bool down, float velocity) { juce::ignoreUnused(midiNote, down, velocity); return false; }
};

} // namespace g2ui
