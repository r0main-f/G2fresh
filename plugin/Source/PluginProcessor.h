#pragma once

#include "AutomationBank.h"
#include "EmulatorHost.h"
#include "MidiForwarder.h"
#include "PatchDocument.h"
#include "SoundEngine.h"
#include "SynthSync.h"

#include <juce_audio_processors/juce_audio_processors.h>

// The patch editor as a plugin (and the stand-alone app). It owns the patch
// being edited and stores it in the host's project, so the patch is recalled
// with the project; exposes the patch's knobs, morph dials and variation as
// automatable parameters (AutomationBank); forwards its MIDI to a hardware
// port into a G2 (MidiForwarder); and holds the editor's connection to a G2
// (SynthSync). Its audio output is the emulated G2's, while the editor is
// connected to it; otherwise silence (a G2 makes its own sound).
class G2EditorProcessor final : public juce::AudioProcessor,
                                public g2ui::EmulatorHost,
                                private juce::ChangeListener {
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
    g2ui::SynthSync& synth() { return synth_; }

    // EmulatorHost: the emulated G2, which plays the patch in G2fresh.
#if G2FRESH_EMULATOR
    bool emulatorAvailable() const override { return true; }
    juce::String startEmulator(const juce::File& firmware) override;
    bool emulatorRunning() const override { return emulated_ != nullptr; }
    void emulatorMidi(std::span<const std::uint8_t> bytes) override;
#endif

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void loadState(std::vector<std::uint8_t> bytes);
    std::vector<std::uint8_t> encodeState() const;
    void refreshState();
    // Swaps the emulated G2's engine (nullptr: none) while the audio thread
    // does not run; the old one is destroyed here, on the message thread.
    void setEmulated(std::unique_ptr<SoundEngine> engine);

    g2ui::PatchDocument document_;
    MidiForwarder midiOut_;
    std::unique_ptr<AutomationBank> automation_;
    // The emulated G2 (EmulatedSoundEngine) while the editor is connected to
    // it: the only sound G2fresh makes. Before synth_, whose link uses it.
    std::unique_ptr<SoundEngine> emulated_;
    bool emulatedSent_ = false; // the document went to the emulated G2 once it was up
    double sampleRate_ = 0.0;
    int blockSize_ = 512;
    g2ui::SynthSync synth_{document_}; // the G2 connection outlives the editor window
    juce::SpinLock stateLock_;
    std::vector<std::uint8_t> stateBytes_; // the patch as a .pch2, for the host
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); // guards deferred state loads

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2EditorProcessor)
};
