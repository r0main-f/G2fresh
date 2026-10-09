#include "PluginEditor.h"

G2EditorView::G2EditorView(G2EditorProcessor& p)
    : AudioProcessorEditor(p), main_(p.document(), p.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
{
    addAndMakeVisible(main_);
    setResizable(true, true);
    setResizeLimits(800, 500, 8192, 8192);
    setSize(1280, 800);
}
