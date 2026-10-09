#pragma once

#include "PluginProcessor.h"

class G2EditorView final : public juce::AudioProcessorEditor {
public:
    explicit G2EditorView(G2EditorProcessor&);

    void paint(juce::Graphics&) override;
    void resized() override {}

private:
    G2EditorProcessor& g2processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2EditorView)
};
