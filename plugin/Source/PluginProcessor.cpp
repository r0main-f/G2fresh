#include "PluginProcessor.h"
#include "PluginEditor.h"

#if G2FRESH_EMULATOR
#include "EmulatedSoundEngine.h"
#include "g2emu/transport.hpp"
#endif

// The state stored in the host project (and by the stand-alone app between
// sessions): the patch or performance as a .pch2/.prf2, its name and file.
std::vector<std::uint8_t> G2EditorProcessor::encodeState() const
{
    const auto& doc = document_;
    juce::ValueTree state("G2fresh");
    const auto bytes = doc.saveBytes();
    juce::MemoryBlock data(bytes.data(), bytes.size());
    state.setProperty("version", 1, nullptr);
    state.setProperty("name", doc.name(), nullptr);
    state.setProperty("file", doc.file().getFullPathName(), nullptr);
    state.setProperty("data", data.toBase64Encoding(), nullptr);
    state.setProperty("layout", doc.layoutJson(), nullptr); // cable shapes
    state.setProperty("midiOut", midiOut_.deviceIdentifier(), nullptr);
    state.setProperty("midiOutName", midiOut_.deviceName(), nullptr);
    state.setProperty("midiChannel", midiOut_.channel(), nullptr);
    juce::MemoryOutputStream out;
    state.writeToStream(out);
    const auto* p = static_cast<const std::uint8_t*>(out.getData());
    return {p, p + out.getDataSize()};
}

G2EditorProcessor::G2EditorProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    stateBytes_ = encodeState();
    document_.addChangeListener(this);
    automation_ = std::make_unique<AutomationBank>(*this, document_);
    midiOut_.onChange = [this] { refreshState(); };
    synth_.addChangeListener(this);
}

G2EditorProcessor::~G2EditorProcessor()
{
    synth_.removeChangeListener(this);
    document_.removeChangeListener(this);
    synth_.disconnect(); // the emulated G2's link, before the emulator goes
}

void G2EditorProcessor::setEmulated(std::unique_ptr<SoundEngine> engine)
{
    {
        const juce::ScopedLock lock(getCallbackLock());
        std::swap(emulated_, engine);
    }
    engine.reset(); // the old one: stops its threads, saves its flash
}

#if G2FRESH_EMULATOR
juce::String G2EditorProcessor::startEmulator(const juce::File& firmware)
{
    // One emulated G2 at a time: the running one saves its flash before the new one reads it.
    if (emulated_ != nullptr) {
        synth_.disconnect();
        setEmulated(nullptr);
    }
    std::unique_ptr<EmulatedSoundEngine> engine;
    try {
        const auto fw = g2emu::Firmware::load(firmware.getFullPathName().toStdString());
        if (fw.code() == nullptr)
            return "no G2 OS in " + firmware.getFileName();
        engine = std::make_unique<EmulatedSoundEngine>(fw, EmulatedSoundEngine::defaultFlashFile());
    } catch (const std::exception& e) {
        return juce::String("the emulated G2 cannot start: ") + e.what();
    }
    engine->prepare(sampleRate_ > 0 ? sampleRate_ : 48000.0, blockSize_);
    synth_.connectEmulated(g2emu::emulatedG2Link(engine->machine()));
    emulatedSent_ = false;
    setEmulated(std::move(engine));
    return {};
}
#endif

#if G2FRESH_EMULATOR
void G2EditorProcessor::emulatorMidi(std::span<const std::uint8_t> bytes)
{
    // The machine's MIDI queue is thread-safe; emulated_ changes only on this thread.
    if (emulated_ != nullptr)
        static_cast<EmulatedSoundEngine&>(*emulated_).machine().midiIn(bytes);
}
#endif

void G2EditorProcessor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &synth_) {
        using Kind = g2ui::SynthSync::Kind;
        if (emulated_ != nullptr && synth_.kind() != Kind::Emulated)
            setEmulated(nullptr); // the editor left the emulated G2
        // Once up, the emulated G2 plays the document (later, the user sends what they want).
        if (synth_.kind() == Kind::Emulated && synth_.ready() && !synth_.bound() && !emulatedSent_) {
            emulatedSent_ = true;
            if (document_.isPerformance())
                synth_.sendPerformance();
            else
                synth_.sendPatchAlone();
        }
        return;
    }
    refreshState();
}

void G2EditorProcessor::refreshState()
{
    // Runs from the message loop, where an exception would end the app: if the
    // patch cannot be encoded, keep the last state that could.
    std::vector<std::uint8_t> bytes;
    try {
        bytes = encodeState();
    } catch (const std::exception& e) {
        DBG("G2fresh: cannot encode the state: " << e.what());
        return;
    }
    const juce::SpinLock::ScopedLockType lock(stateLock_);
    stateBytes_ = std::move(bytes);
}

void G2EditorProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    midiOut_.prepare(sampleRate);
    const juce::ScopedLock lock(getCallbackLock());
    sampleRate_ = sampleRate;
    blockSize_ = samplesPerBlock;
    if (emulated_ != nullptr)
        emulated_->prepare(sampleRate, samplesPerBlock);
}

bool G2EditorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void G2EditorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    midiOut_.process(midi); // the track's MIDI to the G2's MIDI IN
#if G2FRESH_EMULATOR
    if (emulated_ != nullptr) { // the editor's synth is the emulated G2: it plays
        auto& emulated = static_cast<EmulatedSoundEngine&>(*emulated_);
        emulated.setOffline(isNonRealtime());
        emulated.render(buffer, midi);
        midi.clear();
        return;
    }
#endif
    buffer.clear();
    midi.clear();
}

juce::AudioProcessorEditor* G2EditorProcessor::createEditor()
{
    return new G2EditorView(*this);
}

void G2EditorProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::SpinLock::ScopedLockType lock(stateLock_);
    destData.replaceAll(stateBytes_.data(), stateBytes_.size());
}

void G2EditorProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> bytes(p, p + sizeInBytes);
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        loadState(std::move(bytes));
    else
        juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(alive_), b = std::move(bytes)]() mutable {
            if (alive.lock())
                loadState(std::move(b));
        });
}

void G2EditorProcessor::loadState(std::vector<std::uint8_t> bytes)
{
    if (bytes.empty())
        return;
    try {
        const auto state = juce::ValueTree::readFromData(bytes.data(), bytes.size());
        if (!state.hasType("G2fresh")) {
            document_.loadBytes(bytes, true); // v0.1: the raw patch file
            return;
        }
        juce::MemoryBlock data;
        data.fromBase64Encoding(state["data"].toString());
        const auto* p = static_cast<const std::uint8_t*>(data.getData());
        document_.loadBytes(std::vector<std::uint8_t>(p, p + data.getSize()), true);
        const juce::String file = state["file"];
        if (file.isNotEmpty() && juce::File::isAbsolutePath(file))
            document_.setFile(juce::File(file));
        document_.setName(state["name"].toString());
        document_.applyLayoutJson(state["layout"].toString());
        midiOut_.restore(state["midiOut"].toString(), state["midiOutName"].toString(), state["midiChannel"]);
        document_.markSaved();
    } catch (const std::exception& e) {
        DBG("G2fresh: could not restore the patch from the host: " << e.what());
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new G2EditorProcessor();
}
