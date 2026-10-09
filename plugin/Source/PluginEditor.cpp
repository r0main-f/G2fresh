#include "PluginEditor.h"

G2EditorView::G2EditorView(G2EditorProcessor& p)
    : AudioProcessorEditor(p), g2processor(p)
{
    setResizable(true, true);
    setResizeLimits(640, 400, 4096, 4096);
    setSize(1024, 640);
}

void G2EditorView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff2b2d31));
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(22.0f));
    g.drawFittedText("G2fresh: Nord Modular G2 editor", getLocalBounds().removeFromTop(80),
                     juce::Justification::centred, 1);
    g.setFont(juce::FontOptions(14.0f));
    g.setColour(juce::Colours::lightgrey);
    const auto status = g2processor.patchData.isEmpty()
        ? juce::String("No patch loaded. No synth connected.")
        : juce::String(static_cast<int>(g2processor.patchData.getSize())) + " bytes of patch data in project state";
    g.drawFittedText(status, getLocalBounds(), juce::Justification::centred, 2);
}
