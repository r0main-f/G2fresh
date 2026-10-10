// The SoundEngine of the emulated G2 (emu/g2emu): the user's own G2 OS on an
// emulated ColdFire and four emulated DSPs, running in real time on threads of
// its own (g2emu::Runner). It plays any patch the OS can compile. The editor
// sends patches over the emulated USB cable (SynthSync::connectEmulated with a
// link on machine()), as to a G2.
//
// Outputs 1/2 of the G2 at 96 kHz, resampled to the host's rate, in signal
// units (a signal of 1.0 into an Out module is 1.0). The track's MIDI goes to the G2's MIDI IN,
// sample-accurate (a fixed latency, Runner::latencyFrames). The flash (the
// synth's banks) comes from the caller: the plugin keeps it in the host's
// project, the stand-alone app in a file.
//
// Threads: made, prepared and destroyed on the message thread while the audio
// thread does not use it (the processor swaps it in under its callback lock);
// render() on the audio thread.
#pragma once

#include "SoundEngine.h"

#if G2FRESH_NO_APP_NAP
#include "NoAppNap.h"
#endif

#include "g2emu/runner.hpp"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

class EmulatedSoundEngine final : public SoundEngine {
public:
    // Powers the machine on with `flash` (empty: erased) in its flash chip;
    // `saveTo`: where the destructor saves a changed flash (none: nowhere).
    // Throws std::exception when the emulator cannot start.
    EmulatedSoundEngine(const g2emu::Firmware& firmware, std::vector<std::uint8_t> flash, juce::File saveTo);
    // Stops the machine (and saves its flash to `saveTo`).
    ~EmulatedSoundEngine() override;

    // The flash while the machine runs (any thread): how many times the OS changed it, and a copy.
    std::uint64_t flashChanges() const { return runner_->machine().flashChanges(); }
    std::vector<std::uint8_t> flashSnapshot() const { return runner_->machine().flashSnapshot(); }
    // 96 kHz frames the audio thread asked for that the machine had not produced yet (heard as dropouts).
    std::uint64_t framesMissing() const { return runner_->stats().framesMissing; }
    // For diagnostics: the runner's speed while running, its longest chunk, the DSP threads in use.
    juce::String diagnostics() const;
    // From a MIDI event to the DACs, in host samples (the runner's buffer, the resampler).
    int latencySamples() const;

    g2emu::Machine& machine() { return runner_->machine(); }
    // Offline rendering (a bounce): wait for the emulator instead of dropping
    // out when it runs slower than the host asks.
    void setOffline(bool offline) { offline_ = offline; }

    void prepare(double sampleRate, int maxBlock) override;
    void render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) override;
    juce::String status() const override;

    // The stand-alone app's flash, and the first flash of a new plugin
    // instance: in the user's G2fresh settings folder.
    static juce::File defaultFlashFile();
    // How many emulated G2s run in this process (plugin instances share it): each takes about 1.5 cores.
    static int running() { return running_.load(); }
    static std::vector<std::uint8_t> readFlash(const juce::File& file);

private:
    inline static std::atomic<int> running_{0};
    juce::File saveTo_;
    std::vector<std::uint8_t> savedFlash_; // as loaded: only a changed flash is written back
    std::unique_ptr<g2emu::Runner> runner_;
#if G2FRESH_NO_APP_NAP
    NoAppNap noAppNap_; // while it runs, the process is not throttled
#endif

    // Audio thread.
    bool offline_ = false;
    double hostRate_ = 96000.0;
    juce::Interpolators::Lagrange resamplers_[2];
    std::vector<float> frames_;      // 4 channels, interleaved, as the runner gives them
    std::vector<float> render96_[2]; // outputs 1/2 at 96 kHz not consumed yet
    juce::AudioBuffer<float> stereo_;
};
