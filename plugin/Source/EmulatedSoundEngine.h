// The SoundEngine of the emulated G2 (emu/g2emu): the user's own G2 OS on an
// emulated ColdFire and four emulated DSPs, running in real time on threads of
// its own (g2emu::Runner). It plays any patch the OS can compile. Patches do
// not come through setPatch(): the editor sends them over the emulated USB
// cable (SynthSync::connectEmulated with a link on machine()), as to a G2.
//
// Outputs 1/2 of the G2 at 96 kHz, resampled to the host's rate, in signal
// units (as the native engine). The track's MIDI goes to the G2's MIDI IN,
// sample-accurate (a fixed latency, Runner::latencyFrames). The flash (the
// synth's banks) is kept in a file between sessions.
//
// Threads: made, prepared and destroyed on the message thread while the audio
// thread does not use it (the processor swaps it in under its callback lock);
// render() on the audio thread.
#pragma once

#include "SoundEngine.h"

#include "g2emu/runner.hpp"

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

class EmulatedSoundEngine final : public SoundEngine {
public:
    // Powers the machine on with the flash saved in `flashFile` (if any).
    // Throws std::exception when the emulator cannot start.
    EmulatedSoundEngine(const g2emu::Firmware& firmware, juce::File flashFile);
    // Stops the machine and saves its flash.
    ~EmulatedSoundEngine() override;

    g2emu::Machine& machine() { return runner_->machine(); }
    // Offline rendering (a bounce): wait for the emulator instead of dropping
    // out when it runs slower than the host asks.
    void setOffline(bool offline) { offline_ = offline; }

    void prepare(double sampleRate, int maxBlock) override;
    void setPatch(const g2::Patch&, int) override {} // over the USB link
    void render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) override;
    juce::String status() const override;

    // Where the flash is kept: in the user's G2fresh settings folder.
    static juce::File defaultFlashFile();

private:
    juce::File flashFile_;
    std::vector<std::uint8_t> savedFlash_; // as loaded: only a changed flash is written back
    std::unique_ptr<g2emu::Runner> runner_;

    // Audio thread.
    bool offline_ = false;
    double hostRate_ = 96000.0;
    juce::Interpolators::Lagrange resamplers_[2];
    std::vector<float> frames_;      // 4 channels, interleaved, as the runner gives them
    std::vector<float> render96_[2]; // outputs 1/2 at 96 kHz not consumed yet
    juce::AudioBuffer<float> stereo_;
};
