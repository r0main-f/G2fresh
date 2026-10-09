#include "PluginProcessor.h"
#include "PluginEditor.h"

// The state stored in the host project (and by the stand-alone app between
// sessions): the patch or performance as a .pch2/.prf2, its name and file.
static std::vector<std::uint8_t> encodeState(const g2ui::PatchDocument& doc)
{
    juce::ValueTree state("G2fresh");
    const auto bytes = doc.saveBytes();
    juce::MemoryBlock data(bytes.data(), bytes.size());
    state.setProperty("version", 1, nullptr);
    state.setProperty("name", doc.name(), nullptr);
    state.setProperty("file", doc.file().getFullPathName(), nullptr);
    state.setProperty("data", data.toBase64Encoding(), nullptr);
    juce::MemoryOutputStream out;
    state.writeToStream(out);
    const auto* p = static_cast<const std::uint8_t*>(out.getData());
    return {p, p + out.getDataSize()};
}

G2EditorProcessor::G2EditorProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    stateBytes_ = encodeState(document_);
    document_.addChangeListener(this);
}

G2EditorProcessor::~G2EditorProcessor()
{
    document_.removeChangeListener(this);
}

void G2EditorProcessor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    auto bytes = encodeState(document_);
    const juce::SpinLock::ScopedLockType lock(stateLock_);
    stateBytes_ = std::move(bytes);
}

void G2EditorProcessor::prepareToPlay(double, int) {}

bool G2EditorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void G2EditorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
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
        document_.markSaved();
    } catch (const std::exception& e) {
        DBG("G2fresh: could not restore the patch from the host: " << e.what());
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new G2EditorProcessor();
}
