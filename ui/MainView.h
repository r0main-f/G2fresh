// The editor window's content, shared by the stand-alone app and the plugin.
#pragma once

#include "AreaView.h"
#include "ModuleBrowser.h"
#include "PatchDocument.h"
#include "ZoomHolder.h"

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

    static constexpr float kMinZoom = 0.5f, kMaxZoom = 2.0f;
    float zoom() const { return zoom_; }
    // Sets the zoom of both areas. `anchor` (in `area`'s local coordinates)
    // stays under the mouse; without one, the top-left of each view stays put.
    // `remember` saves the zoom in the user's settings.
    void setZoom(float zoom, const AreaView* area = nullptr, std::optional<juce::Point<int>> anchor = std::nullopt,
                 bool remember = true);

private:
    // A titled frame around one area's viewport, explaining what the area is.
    class AreaPane : public juce::Component {
    public:
        AreaPane(juce::Viewport& port, juce::Colour marker);
        void setText(const juce::String& title, const juce::String& subtitle, const juce::String& tooltip);
        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        juce::Viewport& port_;
        juce::Colour marker_;
        juce::String title_, subtitle_;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void updateToolbar();
    void setStatus(const juce::String& s) { status_.setText(s, juce::dontSendNotification); }
    void loadFile(const juce::File& f, bool ignoreChecksum = false);
    void confirmDiscard(std::function<void()> then);

    PatchDocument& doc_;
    juce::TextButton new_{"New"}, open_{"Open..."}, save_{"Save"}, saveAs_{"Save As..."};
    juce::TextButton undo_{"Undo"}, redo_{"Redo"};
    juce::OwnedArray<juce::TextButton> variations_;
    juce::OwnedArray<juce::TextButton> slots_; // performance slots A-D
    juce::TextButton zoomOut_{"-"}, zoomReset_{"100%"}, zoomIn_{"+"};
    juce::ToggleButton classic_{"Classic look"};
    juce::ToggleButton animate_{"Animate cables"};
    juce::Label title_, status_;
    ModuleBrowser browser_;
    AreaView va_, fx_;
    ZoomHolder vaZoom_{va_}, fxZoom_{fx_};
    juce::Viewport vaPort_, fxPort_;
    AreaPane vaPane_{vaPort_, juce::Colour(0xff4a90d9)}, fxPane_{fxPort_, juce::Colour(0xffb36ad6)};
    float zoom_ = 1.0f;
    juce::StretchableLayoutManager layout_;
    juce::StretchableLayoutResizerBar divider_{&layout_, 1, false};
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace g2ui
