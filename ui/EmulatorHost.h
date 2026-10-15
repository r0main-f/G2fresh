// The emulated G2 as the editor sees it: the user's own G2 OS (from Clavia's
// free OS updater) running in G2fresh, which plays the patch. Implemented by
// the plugin processor, so the editor needs no audio or emulator code.
// Available when G2fresh is built with the emulator (G2_BUILD_EMU).
#pragma once

#include "G2Panel.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <span>

namespace g2ui {

// What the computer test found (EmulatorHost::testExpansion).
struct ExpansionTest {
    juce::String error;      // why it did not measure; empty when it did
    double speed = 0;        // a heavy patch on eight DSPs, x real time (off-line), on the fastest threads tried
    int threads = 0;         // those threads, which the emulated G2 then uses with the board
    int voices = 0;          // the voices the G2 OS gave that patch (7 on eight DSPs, as the manual says)
    enum Advice { Holds, QuietOnly, TooSlow } advice = TooSlow;
    int othersRunning = 0;   // other emulated G2s in this host while it measured (they slow it down)
};

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
    // Whether Out 3/4 are mixed into Out 1/2 when the host does not take the Out 3/4 output (off: as the G2, whose
    // headphones carry Out 1/2 only).
    virtual bool mixOut34() const { return false; }
    // The panel's Master Level knob, 0 .. 1 (kept by the host with the project).
    virtual float masterLevel() const { return 1.0f; }
    virtual void setMixOut34(bool on) { juce::ignoreUnused(on); }
    // The panel's Mic Level knob, 0 .. 1 (kept with the project): the gain of the XLR microphone input's preamp.
    virtual float micLevel() const { return 0.5f; }
    // Whether the audio input In 1 is the XLR microphone input, through its preamp, instead of the IN 1 jack (off:
    // the jack, which on a G2 disconnects the microphone; the Mic Level knob then does nothing, as on a G2).
    virtual bool micInput() const { return false; }
    virtual void setMicInput(bool on) { juce::ignoreUnused(on); }
    // Its buffer: how much audio it keeps ready ahead of the host against a busy computer, which is most of its
    // latency. 0 Low, 1 Normal, 2 Safe (plugin/Source/AudioIo.h); the user's choice for this computer, kept in
    // the settings, not in the project.
    virtual int emulatorBuffer() const { return 0; }
    virtual void setEmulatorBuffer(int buffer) { juce::ignoreUnused(buffer); }
    // Whether it has the voice expansion board: eight DSPs, as every G2X, instead of four (the default, a deliberate
    // deviation: a heavy patch on eight runs below real time on an M1). The user's choice for this computer, kept in
    // the settings; a running emulated G2 restarts to take it (its OS finds the board at power-on).
    virtual bool emulatorExpansion() const { return false; }
    virtual void setEmulatorExpansion(bool on) { juce::ignoreUnused(on); }
    // Measures how fast this computer runs a heavy patch on eight DSPs (about 20 s, off the message thread; the running
    // emulated G2 stops meanwhile and starts again after), then calls `done` on the message thread. Keeps the fastest
    // DSP threads for the board. False when it cannot start (no emulator, no G2 OS, a test already running).
    virtual bool testExpansion(std::function<void(const ExpansionTest&)> done)
    {
        juce::ignoreUnused(done);
        return false;
    }
    // The latency from MIDI or an input to the output with that buffer, as the host runs it now, in milliseconds
    // (0: not running).
    virtual double emulatorLatencyMs(int buffer) const
    {
        juce::ignoreUnused(buffer);
        return 0.0;
    }
    // Blocks of audio it was late for since it started (heard as clicks or gaps): the buffer was too small.
    virtual int emulatorDropouts() const { return 0; }
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
