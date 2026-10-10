// The sound the plugin (and the stand-alone app) makes by itself, from the
// patch being edited, when no G2 plays it. An interface, so that the engines
// can change behind it: the native engine (g2engine, clean-room C++ modules)
// now, the emulated G2 later.
#pragma once

#include "g2/patch.hpp"

#include <juce_audio_basics/juce_audio_basics.h>

class SoundEngine {
public:
    virtual ~SoundEngine() = default;

    // Audio thread (prepareToPlay): the host's sample rate and block size.
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    // Message thread: the patch changed (structure, parameters or variation).
    // The engine decides what that costs (rebuild, or parameter changes).
    virtual void setPatch(const g2::Patch& patch, int variation) = 0;
    // Audio thread: renders the block into `out` (stereo: Out 1/2) from the
    // MIDI in `midi`.
    virtual void render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) = 0;
    // Message thread: one line for the user (what does not play yet).
    virtual juce::String status() const = 0;
};
