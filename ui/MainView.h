// The editor window's content, shared by the stand-alone app and the plugin.
//
// File, Edit, View and Options commands live in a menu bar: the macOS menu
// bar for the stand-alone app (a menu bar inside the window on Windows and
// Linux), or a Menu button in a plugin, where the host owns the menu bar.
#pragma once

#include "AreaView.h"
#include "EmulatorHost.h"
#include "FocusKeys.h"
#include "LiveView.h"
#include "MidiOutputTarget.h"
#include "SynthSync.h"
#include "ModuleBrowser.h"
#include "MutatorWindow.h"
#include "PatchDocument.h"
#include "PatchSettingsBar.h"
#include "SplitBar.h"
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
    // Where Setup > Forward MIDI to a Hardware G2 sends the MIDI (nullptr: no such menu).
    void setMidiOutput(MidiOutputTarget* target)
    {
        midiOut_ = target;
        menuItemsChanged();
    }
    // The connection to the G2 (nullptr: no Synth menu).
    void setSynth(SynthSync* synth);
    // The emulated G2 (nullptr: none).
    void setEmulator(EmulatorHost* emulator)
    {
        emulator_ = emulator;
        live_.setHost(emulator);
        updateSynthStatus();
        menuItemsChanged();
    }

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;

    void newPatch();
    void open();
    void openFile(const juce::File& f);
    void save(bool saveAs);
    // Calls `then` once the user has saved or discarded unsaved changes.
    void confirmDiscard(std::function<void()> then);
    // After "Discard" at quit: back to the file as saved (a new patch
    // without one), so the next session doesn't restore the discarded edits.
    void discardChanges();

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
    // Saves the document to `f`, asking first if it was converted from an old
    // version (see saveTo in MainView.cpp); writeFile writes it.
    void saveTo(const juce::File& f);
    void writeFile(const juce::File& f);
    // The file name Save suggests for the document's name.
    juce::String suggestedFileName() const;
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
    juce::TextButton varInit_{"Init"};
    // The red line on top of a slot button the keyboard plays (manual p58).
    struct KeyboardMark : juce::Component {
        KeyboardMark() { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xffe03030)); }
    };
    juce::OwnedArray<KeyboardMark> keyboardMarks_;
    juce::OwnedArray<juce::TextButton> slots_; // performance slots A-D
    juce::TextButton zoomOut_{"-"}, zoomReset_{"100%"}, zoomIn_{"+"};
    juce::TextButton randomizeButton_{"Randomize"}, mutatorButton_{"Mutator..."};
    juce::ComboBox morphGroup_;
    // The original's Color selector and paint bucket (manual p60, p82): the colour new modules get; choosing one or
    // the bucket paints the selected modules. Not saved.
    juce::TextButton colourButton_, bucketButton_{"Paint"};
    std::uint8_t moduleColour_ = 0;
    void chooseModuleColour();
    std::unique_ptr<MutatorWindow> mutator_;
    MidiOutputTarget* midiOut_ = nullptr;
    SynthSync* synth_ = nullptr;
    EmulatorHost* emulator_ = nullptr;
    std::unique_ptr<class BankBrowser> bankBrowser_;
    std::unique_ptr<class PatchLibrary> library_;
    // File > Patch Library... (shows or hides it, as the original's Patch Browser key).
    void showLibrary();
    std::unique_ptr<juce::Component> synthSettings_;
    void showSynthSettings();
    // A store the synth refused: the original's alert ("Flash Problem").
    void showFlashProblem(const juce::String& text);
    juce::Label synthStatus_;
    void updateSynthStatus();
    juce::String emulatorBufferName(int buffer) const;
    // The computer test's result, and the question whether to fit the voice expansion board.
    void offerExpansion(const ExpansionTest& t);
    // Connects to the emulated G2, from the G2 OS chosen before (or asks where it is).
    void startEmulator(bool chooseFirmware);
    // Asks where the user's copy of Clavia's download is (the Emulated G2's G2 OS
    // and the Classic look's graphics both come from it).
    void chooseClaviaDownload(const juce::String& title, std::function<void(const juce::File&)> chosen);
    // Edit (the patch's modules and cables) or Live (the emulated G2's panel and keyboard).
    void setLiveMode(bool live);
    // Showing the Live view starts the emulated G2, or says why not (LiveShow.h).
    void startForLive();
    bool liveMode_ = false;
    juce::TextButton editButton_{"Edit"}, liveButton_{"Live"};
    juce::TextButton init1_{"Init 1"}, init2_{"Init 2"};
    LiveView live_{nullptr};
    void updateLoad();
    juce::PopupMenu buildMenu(const std::string& name);
    void addMenuItem(juce::PopupMenu& m, int id);
    bool liveAvailable() const;
    void showMutator();
    juce::Label status_, load_;
    PatchSettingsBar settings_{doc_};
    ModuleBrowser browser_;
    // The arrow keys on the parameter focus (FocusKeys.h). The arrow held, to tell its auto-repeat from a
    // press; the push button an Up holds down until the key is released (CPatchView::KeyRelParamValue).
    bool focusKeyPressed(g2ui::FocusKey key);
    void showFocus();
    void zoomToFit();
    void recallVariationInit();
    // The two init patches (File > Save InitPatch 1/2, the toolbar's Init 1 and Init 2): files beside the settings.
    static juce::File initPatchFile(int n);
    void saveInitPatch(int n);
    // File > New To, Open To, Save From (PatchKeys.h slotTarget): a slot of the performance, or of the connected G2.
    // Where the file dialogs start (PatchLibraryModel.h dialogFolder).
    juce::File dialogFolder() const;
    void newTo(int slot);
    void openTo(int slot);
    void saveFrom(int slot);
    void recallInitPatch(int n);
    void slotClicked(int slot);
    void saveSelection();
    void insertPatchFile();
    void showFocusStatus();
    int heldArrow_ = 0;
    int heldKey_ = 0; // the last key pressed and still held, to tell its auto-repeat
    std::optional<g2::focus::Focus> heldButton_;
    AreaView va_, fx_;
    ZoomHolder vaZoom_{va_}, fxZoom_{fx_};
    juce::Viewport vaPort_, fxPort_;
    AreaPane vaPane_{vaPort_, juce::Colour(0xff4a90d9)}, fxPane_{fxPort_, juce::Colour(0xffb36ad6)};
    float zoom_ = 1.0f;
    AreaView* activeArea_ = nullptr; // last clicked area: target of Edit commands
    g2::mutate::Random random_{static_cast<std::uint32_t>(juce::Time::currentTimeMillis())};
    // The split bar between the areas (SplitBar.h): dragged, or its three buttons (only the FX Area, only the Voice
    // Area, back), as the original's CSplitterView. The position is the patch's (its header's splitterPos).
    class Divider : public juce::Component, public juce::SettableTooltipClient {
    public:
        explicit Divider(MainView& owner) : owner_(owner) { setMouseCursor(juce::MouseCursor::UpDownResizeCursor); }
        void paint(juce::Graphics&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        // Esc during a drag: the bar stays where it got to, and the drag ends (CSplitterView::Release @000f6e7e).
        bool abortDrag();

    private:
        // 0, 1, 2: the button under x (FX only, Voice only, back); -1: the bar.
        int buttonAt(int x) const { return x >= 0 && x < 3 * SplitBar::kHeight ? x / SplitBar::kHeight : -1; }
        MainView& owner_;
        bool dragging_ = false;
        int grab_ = 0;
    };
    friend class Divider;
    SplitBar split_;
    Divider divider_{*this};
    int splitTotal() const;
    // After the bar was moved by the user: the layout, and the patch header (not an undo step, as the original's
    // RequestPatchHeaderChange).
    void splitMoved();
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::RecentlyOpenedFilesList recent_;
};

} // namespace g2ui
