#pragma once

#include "AutomationBank.h"
#include "MidiForwarder.h"
#include "PatchDocument.h"

#include <juce_audio_processors/juce_audio_processors.h>

// The G2 makes its own sound: this processor outputs silence. It owns the
// patch being edited and stores it in the host's project, so the patch is
// recalled with the project; exposes the patch's knobs, morph dials and
// variation as automatable parameters (AutomationBank); and forwards its MIDI
// to a hardware port into the G2 (MidiForwarder). Sending to the synth comes
// with the USB bridge.
class G2EditorProcessor final : public juce::AudioProcessor, private juce::ChangeListener {
public:
    G2EditorProcessor();
    ~G2EditorProcessor() override;

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

    g2ui::PatchDocument& document() { return document_; }
    MidiForwarder& midiOutput() { return midiOut_; }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void loadState(std::vector<std::uint8_t> bytes);
    std::vector<std::uint8_t> encodeState() const;
    void refreshState();

    g2ui::PatchDocument document_;
    MidiForwarder midiOut_;
    std::unique_ptr<AutomationBank> automation_;
    juce::SpinLock stateLock_;
    std::vector<std::uint8_t> stateBytes_; // the patch as a .pch2, for the host
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); // guards deferred state loads

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2EditorProcessor)
};
