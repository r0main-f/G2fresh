// The sound the plugin (and the stand-alone app) makes by itself: an
// interface for the engine behind it, the emulated G2 (EmulatedSoundEngine).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

class SoundEngine {
public:
    virtual ~SoundEngine() = default;

    // Audio thread (prepareToPlay): the host's sample rate and block size.
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    // Audio thread: renders the block into `out` (stereo: Out 1/2) from the
    // MIDI in `midi`.
    virtual void render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) = 0;
    // Message thread: one line for the user (what does not play yet).
    virtual juce::String status() const = 0;
};
