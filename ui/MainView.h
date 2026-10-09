// The editor window's content, shared by the stand-alone app and the plugin.
#pragma once

#include "AreaView.h"
#include "ModuleBrowser.h"
#include "PatchDocument.h"

namespace g2ui {

class MainView : public juce::Component,
                 public juce::DragAndDropContainer,
                 private juce::ChangeListener {
public:
    explicit MainView(PatchDocument& doc);
    ~MainView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    void newPatch();
    void open();
    void save(bool saveAs);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void updateToolbar();
    void setStatus(const juce::String& s) { status_.setText(s, juce::dontSendNotification); }
    void loadFile(const juce::File& f);
    void confirmDiscard(std::function<void()> then);

    PatchDocument& doc_;
    juce::TextButton new_{"New"}, open_{"Open..."}, save_{"Save"}, saveAs_{"Save As..."};
    juce::TextButton undo_{"Undo"}, redo_{"Redo"};
    juce::OwnedArray<juce::TextButton> variations_;
    juce::Label title_, status_;
    ModuleBrowser browser_;
    AreaView va_, fx_;
    juce::Viewport vaPort_, fxPort_;
    juce::StretchableLayoutManager layout_;
    juce::StretchableLayoutResizerBar divider_{&layout_, 1, false};
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace g2ui
