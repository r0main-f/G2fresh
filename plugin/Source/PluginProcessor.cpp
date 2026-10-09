#include "PluginProcessor.h"
#include "PluginEditor.h"

G2EditorProcessor::G2EditorProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    stateBytes_ = document_.saveBytes();
    document_.addChangeListener(this);
}

G2EditorProcessor::~G2EditorProcessor()
{
    document_.removeChangeListener(this);
}

void G2EditorProcessor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    auto bytes = document_.saveBytes();
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
        document_.loadBytes(bytes);
    } catch (const std::exception& e) {
        DBG("G2fresh: could not restore the patch from the host: " << e.what());
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new G2EditorProcessor();
}
