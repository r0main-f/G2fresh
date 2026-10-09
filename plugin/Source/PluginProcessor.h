#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// The G2 makes its own sound: this processor outputs silence. Its jobs are to
// carry the patch in the host's project state and, later, to forward
// automation and MIDI to the synth through g2bridge.
class G2EditorProcessor final : public juce::AudioProcessor {
public:
    G2EditorProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Raw .pch2/.prf2 bytes saved with the host project.
    juce::MemoryBlock patchData;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2EditorProcessor)
};
