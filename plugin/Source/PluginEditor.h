#pragma once

#include "MainView.h"
#include "PluginProcessor.h"

class G2EditorView final : public juce::AudioProcessorEditor {
public:
    explicit G2EditorView(G2EditorProcessor&);

    void resized() override { main_.setBounds(getLocalBounds()); }

private:
    g2ui::MainView main_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2EditorView)
};
