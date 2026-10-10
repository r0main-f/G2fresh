// The SoundEngine of g2engine (native C++ modules, see engine/). It runs at
// the G2's 96 kHz and is resampled to the host's rate.
//
// Threads: setPatch() (message thread) builds a new PatchEngine when the
// patch's structure changed and hands it to the audio thread through an atomic
// slot (the old one comes back the same way and is deleted on the message
// thread); when only parameters or the variation changed, it queues them
// instead, so knobs move without restarting the sound. Notes held when an
// engine is swapped in are played again on it.
#pragma once

#include "SoundEngine.h"

#include "g2/engine/engine.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class NativeSoundEngine final : public SoundEngine {
public:
    NativeSoundEngine();
    ~NativeSoundEngine() override;

    void prepare(double sampleRate, int maxBlock) override;
    void setPatch(const g2::Patch& patch, int variation) override;
    void render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) override;
    juce::String status() const override;

private:
    struct Change {
        g2::Location loc = g2::Location::Va;
        std::uint8_t module = 0, param = 0, value = 0;
        int variation = -1; // >= 0: a variation switch instead
    };
    void collectRetired();
    void applyMidi(g2::engine::PatchEngine& e, const juce::MidiMessage& m);

    // Message thread.
    std::optional<g2::Patch> current_; // what the audio thread's engine was built from / told about
    int currentVariation_ = 0;
    juce::StringArray unsupported_;

    // Hand-over between threads.
    std::atomic<g2::engine::PatchEngine*> pending_{nullptr}; // built, waiting for the audio thread
    std::atomic<g2::engine::PatchEngine*> retired_{nullptr}; // given back for deletion
    static constexpr int kChanges = 1024;
    juce::AbstractFifo changeFifo_{kChanges};
    std::array<Change, kChanges> changes_;

    // Audio thread.
    std::unique_ptr<g2::engine::PatchEngine> engine_;
    std::vector<std::pair<int, int>> held_; // notes held (note, velocity)
    double hostRate_ = 96000.0;
    juce::Interpolators::Lagrange resamplers_[2];
    std::vector<float> render96_[2]; // samples at 96 kHz not consumed yet
    juce::AudioBuffer<float> stereo_; // Out 1/2 at the host's rate
};
