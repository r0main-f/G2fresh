#include "PluginProcessor.h"
#include "PluginEditor.h"

G2EditorProcessor::G2EditorProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
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
    destData = patchData;
}

void G2EditorProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    patchData.replaceAll(data, static_cast<size_t>(sizeInBytes));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new G2EditorProcessor();
}
