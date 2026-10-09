#include "PluginProcessor.h"
#include "PluginEditor.h"

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
}

G2EditorProcessor::~G2EditorProcessor()
{
    document_.removeChangeListener(this);
}

void G2EditorProcessor::changeListenerCallback(juce::ChangeBroadcaster*)
{
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

void G2EditorProcessor::prepareToPlay(double sampleRate, int)
{
    midiOut_.prepare(sampleRate);
}

bool G2EditorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void G2EditorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    midiOut_.process(midi); // the track's MIDI to the G2's MIDI IN
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
