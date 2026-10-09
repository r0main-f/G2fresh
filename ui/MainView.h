// The editor window's content, shared by the stand-alone app and the plugin.
//
// File, Edit, View and Options commands live in a menu bar: the macOS menu
// bar for the stand-alone app (a menu bar inside the window on Windows and
// Linux), or a Menu button in a plugin, where the host owns the menu bar.
#pragma once

#include "AreaView.h"
#include "ModuleBrowser.h"
#include "MutatorWindow.h"
#include "PatchDocument.h"
#include "PatchSettingsBar.h"
#include "Theme.h"
#include "ZoomHolder.h"

#include "g2/mutate.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace g2ui {

class MainView : public juce::Component,
                 public juce::DragAndDropContainer,
                 public juce::MenuBarModel,
                 private juce::ChangeListener {
public:
    // `standalone`: the app owns the menu bar (otherwise a Menu button is shown).
    MainView(PatchDocument& doc, bool standalone);
    ~MainView() override;

    // Stand-alone app only: opens the audio/MIDI device settings.
    std::function<void()> onAudioSettings;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    void newPatch();
    void open();
    void openFile(const juce::File& f);
    void save(bool saveAs);
    // Calls `then` once the user has saved or discarded unsaved changes.
    void confirmDiscard(std::function<void()> then);

    static constexpr float kMinZoom = 0.5f, kMaxZoom = 2.0f;
    float zoom() const { return zoom_; }
    // Sets the zoom of both areas. `anchor` (in `area`'s local coordinates)
    // stays under the mouse. Without one, the view centres on the module last
    // added or edited (in the area where that happened); other views keep
    // their top-left corner. `remember` saves the zoom in the user's settings.
    void setZoom(float zoom, const AreaView* area = nullptr, std::optional<juce::Point<int>> anchor = std::nullopt,
                 bool remember = true);

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int itemId, int topLevelMenuIndex) override;

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
    void setLook(Look look);
    void setAnimation(bool on);
    AreaView* areaWithSelection();
    AreaView& activeArea() { return activeArea_ ? *activeArea_ : va_; }
    void copySelection(bool cut);
    void pasteClipboard();
    // Randomize (or mutate) the current variation, or only the selection.
    void randomize(bool mutateOnly);

    PatchDocument& doc_;
    const bool standalone_;
    juce::SharedResourcePointer<theme::LookAndFeel> lookAndFeel_;
    juce::TooltipWindow tooltips_{this, 600};
    std::unique_ptr<juce::MenuBarComponent> menuBar_; // Windows/Linux stand-alone
    juce::TextButton menuButton_{"Menu"};              // plugin
    juce::Label slotPrefix_, name_, edited_;
    juce::Label variationLabel_{{}, "Variation"};
    juce::OwnedArray<juce::TextButton> variations_;
    juce::OwnedArray<juce::TextButton> slots_; // performance slots A-D
    juce::TextButton zoomOut_{"-"}, zoomReset_{"100%"}, zoomIn_{"+"};
    juce::TextButton randomizeButton_{"Randomize"}, mutatorButton_{"Mutator..."};
    std::unique_ptr<MutatorWindow> mutator_;
    void showMutator();
    juce::Label status_, load_;
    PatchSettingsBar settings_{doc_};
    ModuleBrowser browser_;
    AreaView va_, fx_;
    ZoomHolder vaZoom_{va_}, fxZoom_{fx_};
    juce::Viewport vaPort_, fxPort_;
    AreaPane vaPane_{vaPort_, juce::Colour(0xff4a90d9)}, fxPane_{fxPort_, juce::Colour(0xffb36ad6)};
    float zoom_ = 1.0f;
    AreaView* activeArea_ = nullptr; // last clicked area: target of Edit commands
    g2::mutate::Random random_{static_cast<std::uint32_t>(juce::Time::currentTimeMillis())};
    juce::StretchableLayoutManager layout_;
    juce::StretchableLayoutResizerBar divider_{&layout_, 1, false};
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::RecentlyOpenedFilesList recent_;
};

} // namespace g2ui
