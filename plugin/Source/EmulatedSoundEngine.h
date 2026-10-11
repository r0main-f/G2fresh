// The SoundEngine of the emulated G2 (emu/g2emu): the user's own G2 OS on an
// emulated ColdFire and four emulated DSPs, running in real time on threads of
// its own (g2emu::Runner). It plays any patch the OS can compile. The editor
// sends patches over the emulated USB cable (SynthSync::connectEmulated with a
// link on machine()), as to a G2.
//
// The G2's four outputs at 96 kHz, resampled to the host's rate, in signal
// units (a signal of 1.0 into an Out module is 1.0): Out 1/2 on the first two
// channels, and Out 3/4 on the next two when the host gives four (the plugin's
// second output bus). Without them Out 3/4 are not heard, as on a G2 whose
// headphones carry Out 1/2 only, unless setMixOut34() mixes them in. The track's MIDI goes to the G2's MIDI IN,
// sample-accurate (a fixed latency, Runner::latencyFrames). The audio inputs In 1-4, also in signal units (AudioIo.h),
// are resampled to 96 kHz and reach the G2's ADCs at a fixed latency too (Runner::write). The "flash" (the
// G2's memory: its flash with the banks, then its EEPROM with the system
// settings, Machine::memorySnapshot) comes from the caller: the plugin keeps it
// in the host's project, the stand-alone app in a file.
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
    // Powers the machine on with the memory `flash` (empty: a new G2);
    // `saveTo`: where the destructor saves a changed memory (none: nowhere).
    // Throws std::exception when the emulator cannot start.
    EmulatedSoundEngine(const g2emu::Firmware& firmware, std::vector<std::uint8_t> flash, juce::File saveTo);
    // Stops the machine (and saves its flash to `saveTo`).
    ~EmulatedSoundEngine() override;

    // The memory while the machine runs (any thread): how many times the OS changed it, and a copy.
    std::uint64_t flashChanges() const { return runner_->machine().memoryChanges(); }
    std::vector<std::uint8_t> flashSnapshot() const { return runner_->machine().memorySnapshot(); }
    // 96 kHz frames the audio thread asked for that the machine had not produced yet (heard as dropouts).
    std::uint64_t framesMissing() const { return runner_->stats().framesMissing; }
    // For diagnostics: the runner's speed while running, its longest chunk, the DSP threads in use.
    juce::String diagnostics() const;
    // What the plugin reports to the host, in host samples: from an input sample to what the G2 makes of it at the
    // output (the two resamplers, the runner's buffer, and the emulated G2's own way from its ADCs to its DACs,
    // kAdcToDacFrames). A MIDI event sounds about half a millisecond sooner than that, plus the OS's reaction
    // (midiLatencySamples).
    int latencySamples() const;
    int midiLatencySamples() const;
    // The emulated G2's way from the ADCs to the DACs through an In and an Out module, in 96 kHz frames: 19 to 21,
    // set when the DSPs start [emulated: tests/test_g2emu.cpp]. The board's own figure is not known.
    static constexpr int kAdcToDacFrames = 20;
    // 96 kHz frames the ADCs took as silence because their input was late (none, by construction: a check).
    std::uint64_t inputMissing() const { return runner_->stats().inputMissing; }
    // In 1 as the XLR microphone input, through its preamp at the Mic Level knob's gain (`level` 0 .. 1), instead of
    // the IN 1 jack (any thread).
    void setMic(bool in1IsMic, float level);

    g2emu::Machine& machine() { return runner_->machine(); }
    // Offline rendering (a bounce): wait for the emulator instead of dropping
    // out when it runs slower than the host asks.
    void setOffline(bool offline) { offline_ = offline; }
    // Out 3/4 mixed into Out 1/2 when the host gives no channels for them (any thread).
    void setMixOut34(bool on) { mixOut34_ = on; }

    void prepare(double sampleRate, int maxBlock) override;
    void render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const juce::AudioBuffer<float>* in = nullptr) override;
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

    std::atomic<bool> mixOut34_{false};
    std::atomic<float> in1Gain_{1.0f}; // the microphone preamp's gain while In 1 is the microphone, else 1

    // Audio thread.
    bool offline_ = false;
    double hostRate_ = 96000.0;
    juce::Interpolators::Lagrange resamplers_[4];
    std::vector<float> frames_;      // 4 channels, interleaved, as the runner gives them
    std::vector<float> render96_[4]; // outputs 1-4 at 96 kHz not consumed yet
    juce::AudioBuffer<float> outs_;  // outputs 1-4 at the host's rate
    // The inputs, the other way: the same interpolation as the outputs (it leaves images of the input above the
    // host's Nyquist frequency, as the outputs fold what is above it: re/notes/fidelity-audit.md).
    juce::Interpolators::Lagrange inResamplers_[4];
    int inHeld_ = 0;                 // host samples of silence the inputs start with: they are never short then
    std::vector<float> inHost_[4];   // inputs 1-4 at the host's rate not consumed yet
    std::vector<float> in96_;        // one input at 96 kHz
    std::vector<float> inFrames_;    // 4 channels, interleaved, as the runner takes them
};
