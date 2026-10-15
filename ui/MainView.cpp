#include "MainView.h"

#include "BankBrowser.h"

#include "g2/proto/logging_transport.hpp"
#include "CableLayout.h"

#include "Dialogs.h"
#include "LiveShow.h"
#include "PatchLibrary.h"
#include "SynthSettingsWindow.h"

#include "g2/patch_load.hpp"

namespace g2ui {
namespace {

const juce::String kOpenPattern = "*.pch2;*.prf2";

enum MenuId {
    kNew = 1, kOpen, kSave, kSaveAs, kClearRecent, kNewPerformance, kPerformanceSettings, kPatchLibrary,
    kUndo, kRedo, kDelete, kRename, kCut, kCopy, kPaste, kDuplicate, kSelectAll, kPatchNotes,
    kRandomize, kMutate, kMutator,
    kZoomIn, kZoomOut, kZoomReset, kShowSettings, kClassicLook, kAnimateCables,
    kAudioSettings, kRemoveBendPoints, kLiveMode,
    kRecentBase = 100,       // + index into the recent files list
    kCopyVariationBase = 200, // + target variation (8 = init)
    kCablesBase = 300,        // + cable colour
    kMidiOutOff = 400, kMidiOutBase = 401, // + index into the device list
    kMidiChannelBase = 600,   // + channel (0: as played)
    kSynthG2 = 700, kSynthVirtual, kSynthDisconnect, kSendPerformance, kGetPerformance, kUnbind, kSynthMemory, kShowUsbLog, kFullUsbLog,
    kSynthSettings,
    kSendPatchBase = 710,     // + slot
    kGetPatchBase = 720,      // + slot
    kSynthEmulated = 730, kEmulatorFirmware, kHostClock, kMixOut34, kEmulatorExpansion,
    kEmulatorBufferBase = 740, // + buffer (EmulatorHost::emulatorBuffer)
    // Opt-in deviations from the original editor (View menu).
    kWheelEditsKnobs = 800, kDoubleClickMorph, kDoubleClickReset, kDoubleClickNothing, kShowLoadEstimate,
    kMorphGroupColours,
};

juce::PopupMenu::Item item(int id, const juce::String& text, const juce::String& shortcut = {}, bool enabled = true,
                           bool ticked = false)
{
    juce::PopupMenu::Item i(text);
    i.itemID = id;
    i.shortcutKeyDescription = shortcut;
    i.isEnabled = enabled;
    i.isTicked = ticked;
    return i;
}

#if JUCE_MAC
const juce::String kCmd = juce::String::fromUTF8("\xe2\x8c\x98");   // ⌘
const juce::String kShift = juce::String::fromUTF8("\xe2\x87\xa7"); // ⇧
#else
const juce::String kCmd = "Ctrl+";
const juce::String kShift = "Shift+";
#endif

} // namespace

MainView::MainView(PatchDocument& doc, bool standalone)
    : doc_(doc), standalone_(standalone), va_(doc, g2::Location::Va), fx_(doc, g2::Location::Fx)
{
    setLookAndFeel(&lookAndFeel_.get());
    recent_.setMaxNumberOfItems(12);
    recent_.restoreFromString(userSettings().getValue("recentFiles"));

    if (standalone_) {
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(this);
#else
        menuBar_ = std::make_unique<juce::MenuBarComponent>(this);
        addAndMakeVisible(*menuBar_);
#endif
    } else {
        menuButton_.setTooltip("File, Edit and View commands");
        menuButton_.onClick = [this] {
            juce::PopupMenu menu;
            const auto names = getMenuBarNames();
            for (int i = 0; i < names.size(); ++i)
                menu.addSubMenu(names[i], getMenuForIndex(i, names[i]));
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton_),
                               [safe = juce::Component::SafePointer<MainView>(this)](int id) {
                                   if (safe && id > 0)
                                       safe->menuItemSelected(id, 0);
                               });
        };
        addAndMakeVisible(menuButton_);
    }

    // The patch name: double-click to edit (16 characters, as on the G2).
    name_.setFont(theme::font(true));
    name_.setEditable(false, true, false);
    name_.setTooltip("Patch name: double-click to rename (up to 16 characters)");
    name_.onEditorShow = [this] {
        // The G2's 16 characters, from its set (CNameDialog refuses other keys).
        if (auto* editor = name_.getCurrentTextEditor())
            editor->setInputRestrictions(PatchDocument::kMaxNameLength, PatchDocument::allowedNameCharacters());
    };
    name_.onTextChange = [this] { doc_.setName(name_.getText()); };
    slotPrefix_.setFont(theme::font());
    slotPrefix_.setColour(juce::Label::textColourId, juce::Colour(0xffa8adb6));
    edited_.setFont(theme::font());
    edited_.setColour(juce::Label::textColourId, juce::Colour(0xffa8adb6));
    for (auto* l : {&slotPrefix_, &name_, &edited_})
        addAndMakeVisible(l);

    variationLabel_.setFont(theme::font());
    variationLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa8adb6));
    variationLabel_.setJustificationType(juce::Justification::centredRight);
    variationLabel_.setTooltip("A patch holds 8 variations: complete sets of knob and setting values, switchable "
                               "live. Pick the one to see and edit (Cmd 1-8).");
    addAndMakeVisible(variationLabel_);
    for (int i = 0; i < g2::kUserVariations; ++i) {
        auto* b = variations_.add(new juce::TextButton(juce::String(i + 1)));
        b->setClickingTogglesState(true);
        b->setRadioGroupId(1);
        b->setTooltip("Variation " + juce::String(i + 1) + ": one of the patch's 8 sets of knob and setting values");
        b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffd08020));
        b->onClick = [this, i] { doc_.setVariation(i); };
        addAndMakeVisible(b);
    }
    for (int i = 0; i < 4; ++i) {
        auto* b = slots_.add(new juce::TextButton(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i))));
        b->setClickingTogglesState(true);
        b->setRadioGroupId(2);
        b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3070c0));
        b->onClick = [this, i] { doc_.setSlot(i); };
        addChildComponent(b);
    }
    status_.setFont(theme::font());
    status_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff26282c));
    addAndMakeVisible(status_);
    load_.setFont(theme::font());
    load_.setJustificationType(juce::Justification::centredRight);
    load_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff26282c));
    load_.setTooltip("Patch load as the synth reports it while the patch is live on it (\"--\" until then, as in "
                     "the original editor). View > Show Load Estimate Offline shows G2fresh's estimate instead.");
    addAndMakeVisible(load_);
    synthStatus_.setFont(theme::font());
    synthStatus_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff26282c));
    synthStatus_.setTooltip("The connection to the G2 (Synth menu). \"live\": edits go to the synth as you make them.");
    addChildComponent(synthStatus_);

    settings_.onStatus = [this](const juce::String& s) { setStatus(s); };
    settings_.onLayoutChanged = [this] { resized(); };
    addAndMakeVisible(settings_);
    // Into the area last clicked, below its selection (CTabButton::Action).
    browser_.onAdd = [this](std::uint8_t type) { activeArea().insertModule(type); };
    browser_.onStatus = [this](const juce::String& s) { setStatus(s); };
    addAndMakeVisible(browser_);

    for (auto* area : {&va_, &fx_}) {
        area->onStatus = [this](const juce::String& s) { setStatus(s); };
        area->onActivated = [this](AreaView* a) {
            activeArea_ = a;
            // One selection at a time: clicking an area clears the other's.
            (a == &va_ ? fx_ : va_).clearSelection();
            menuItemsChanged();
        };
        area->onZoom = [this, area](float factor, juce::Point<int> where) { setZoom(zoom_ * factor, area, where); };
    }
    vaPort_.setViewedComponent(&vaZoom_, false);
    fxPort_.setViewedComponent(&fxZoom_, false);

    zoomOut_.setTooltip("Zoom out (Cmd -)");
    zoomIn_.setTooltip("Zoom in (Cmd +)");
    zoomReset_.setTooltip("Actual size (Cmd 0). Cmd + scroll or pinch to zoom around the mouse");
    zoomOut_.onClick = [this] { setZoom(zoom_ / 1.25f); };
    zoomIn_.onClick = [this] { setZoom(zoom_ * 1.25f); };
    zoomReset_.onClick = [this] { setZoom(1.0f); };
    for (auto* b : {&zoomOut_, &zoomReset_, &zoomIn_})
        addAndMakeVisible(b);
    randomizeButton_.setTooltip("Randomize the current variation (or the selected modules). Locked modules, "
                                "switches and modes are kept. Undo to go back.");
    randomizeButton_.onClick = [this] { randomize(false); };
    mutatorButton_.setTooltip("Breed new versions of the current variation: randomize, mutate, cross, audition");
    mutatorButton_.onClick = [this] { showMutator(); };
    addAndMakeVisible(randomizeButton_);
    addAndMakeVisible(mutatorButton_);
    // Edit / Live: the patch, or the emulated G2 as an instrument
    editButton_.setTooltip("Edit the patch: modules and cables (Cmd L switches)");
    liveButton_.setTooltip("Play the Emulated G2: its front panel and a keyboard (Cmd L switches)");
    for (auto* b : {&editButton_, &liveButton_}) {
        b->setClickingTogglesState(false);
        b->setRadioGroupId(4711);
        addAndMakeVisible(b);
    }
    editButton_.setConnectedEdges(juce::Button::ConnectedOnRight);
    liveButton_.setConnectedEdges(juce::Button::ConnectedOnLeft);
    editButton_.setToggleState(true, juce::dontSendNotification);
    editButton_.onClick = [this] { setLiveMode(false); };
    liveButton_.onClick = [this] { setLiveMode(true); };
    addChildComponent(live_);
    vaPort_.setScrollBarsShown(true, true);
    fxPort_.setScrollBarsShown(true, true);
    addAndMakeVisible(vaPane_);
    addAndMakeVisible(divider_);
    addAndMakeVisible(fxPane_);
    fxPane_.setText("FX AREA", "Monophonic: runs once, shared by all voices. Put effects here (reverb, chorus, delay...).",
                    "The FX area processes the mix of all voices once. Route voices here with the Fx/Bus outputs of "
                    "the voice area, e.g. 2-Out set to Fx.");
    layout_.setItemLayout(0, 60, -1.0, -0.62);
    layout_.setItemLayout(1, 6, 6, 6);
    layout_.setItemLayout(2, 60, -1.0, -0.38);

    setWantsKeyboardFocus(true);
    doc_.addChangeListener(this);
    updateToolbar();
    setZoom(static_cast<float>(userSettings().getDoubleValue("zoom", 1.0)), nullptr, std::nullopt, false);
}

MainView::~MainView()
{
#if JUCE_MAC
    if (standalone_)
        juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    setLookAndFeel(nullptr);
    doc_.removeChangeListener(this);
    synthSettings_.reset();
    library_.reset(); // ends an audition while the synth is still there
    if (synth_ != nullptr) {
        synth_->removeChangeListener(this);
        synth_->onFlashProblem = nullptr;
        synth_->onAlert = nullptr;
    }
}

void MainView::setSynth(SynthSync* synth)
{
    synthSettings_.reset();
    library_.reset();
    if (synth_ != nullptr) {
        synth_->removeChangeListener(this);
        synth_->onFlashProblem = nullptr;
        synth_->onAlert = nullptr;
    }
    synth_ = synth;
    if (synth_ != nullptr) {
        synth_->addChangeListener(this);
        synth_->onFlashProblem = [this](const juce::String& text) { showFlashProblem(text); };
        // The original's alert when the synth reports an exception (7E).
        synth_->onAlert = [safe = juce::Component::SafePointer<MainView>(this)](const juce::String& title,
                                                                                  const juce::String& text) {
            if (safe)
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, text, "OK", safe);
        };
    }
    synthStatus_.setVisible(synth_ != nullptr);
    va_.setLiveLeds(synth_);
    fx_.setLiveLeds(synth_);
    updateSynthStatus();
    menuItemsChanged();
    resized();
}

void MainView::updateLoad()
{
    // What the synth reports while the patch is live on it. Offline the
    // original shows "--" (CTBWindow::SetupLoadMeter); G2fresh's estimate
    // (some tables are approximate, patch-load.md section 2) is opt-in.
    const auto reported = synth_ != nullptr ? synth_->reportedLoad() : std::nullopt;
    if (!reported && !showLoadEstimate()) {
        load_.setText("Load   VA -- cycles, -- memory   FX -- cycles, -- memory  ", juce::dontSendNotification);
        load_.setColour(juce::Label::textColourId, juce::Colour(0xffa8adb6));
        return;
    }
    const auto load = reported ? *reported : g2::patchload::compute(doc_.patch());
    auto pct = [](float f) { return juce::String(juce::roundToInt(f * 100.0f)) + "%"; };
    load_.setText(juce::String(reported ? "Load (synth)" : "Load (estimate)") + "   VA " + pct(load.vaCycles) + " cycles, "
                      + pct(load.vaMemory) + " memory   FX " + pct(load.fxCycles) + " cycles, " + pct(load.fxMemory)
                      + " memory  ",
                  juce::dontSendNotification);
    const float worst = std::max({load.vaCycles, load.vaMemory, load.fxCycles, load.fxMemory});
    load_.setColour(juce::Label::textColourId, worst > 1.0f    ? juce::Colour(0xffff5a5a)
                                               : worst > 0.85f ? juce::Colour(0xffffb347)
                                                               : juce::Colour(0xffa8adb6));
}

// "Low (9 ms)": a buffer of the emulated G2 with the latency it gives in this host now.
void MainView::offerExpansion(const ExpansionTest& t)
{
    updateSynthStatus();
    if (t.error.isNotEmpty()) {
        setStatus("The computer test did not finish (" + t.error + "): the voice expansion board is not fitted");
        return;
    }
    const auto speed = juce::String(t.speed, 1) + " x real time";
    juce::String text = "This computer ran a heavy patch on eight DSPs (" + juce::String(t.voices) + " voices) at " + speed
                      + ", on " + juce::String(t.threads) + " DSP threads.\n\n";
    if (t.advice == ExpansionTest::Holds)
        text << "That is enough for a host: heavy patches should play without clicks or gaps.";
    else if (t.advice == ExpansionTest::QuietOnly)
        text << "That is enough on a quiet computer only: in a busy project, heavy patches will click or drop out. "
                "Light patches play.";
    else
        text << "That is slower than real time: heavy patches will lose their audio. Light patches may play.";
    if (t.othersRunning > 0)
        text << "\n\n" << t.othersRunning << " other Emulated G2(s) ran meanwhile, which slowed the test down.";
    text << "\n\nFit the voice expansion board (twice the voices, twice the CPU)? You can remove it again in the Synth menu.";
    juce::Component::SafePointer<MainView> self(this);
    juce::AlertWindow::showOkCancelBox(
        t.advice == ExpansionTest::Holds ? juce::MessageBoxIconType::QuestionIcon : juce::MessageBoxIconType::WarningIcon,
        "Voice Expansion Board", text, "Fit the Board", "Keep Four DSPs", this,
        juce::ModalCallbackFunction::create([self](int fit) {
            if (self == nullptr || self->emulator_ == nullptr)
                return;
            if (fit != 0) {
                self->emulator_->setEmulatorExpansion(true);
                self->setStatus("Voice expansion board fitted: eight DSPs, as a G2X");
            } else {
                self->setStatus("Four DSPs kept");
            }
            self->updateSynthStatus();
            self->menuItemsChanged();
        }));
}

juce::String MainView::emulatorBufferName(int buffer) const
{
    static const char* const names[] = {"Low", "Normal", "Safe"};
    juce::String text = names[juce::jlimit(0, 2, buffer)];
    if (const double ms = emulator_ != nullptr ? emulator_->emulatorLatencyMs(buffer) : 0.0; ms > 0)
        text << " (" << juce::String(ms, 1) << " ms)";
    return text;
}

void MainView::updateSynthStatus()
{
    if (synth_ == nullptr)
        return;
    juce::String text = synth_->statusText();
    if (emulator_ != nullptr && emulator_->emulatorAvailable()) {
        const int others = emulator_->emulatorsInHost() - (emulator_->emulatorRunning() ? 1 : 0);
        if (synth_->kind() == SynthSync::Kind::None && others > 0)
            text = "Not connected: another G2fresh here runs the Emulated G2 (Synth > Connect to Emulated G2 starts one "
                   "more, about 1.5 cores each)";
        else if (emulator_->emulatorRunning() && others > 0)
            text << "  |  " << (others + 1) << " Emulated G2s run in this host: heavy on the CPU";
        // the emulated G2 was late for the host: heard as clicks or gaps, and for the user to know
        if (const int dropouts = emulator_->emulatorRunning() ? emulator_->emulatorDropouts() : 0; dropouts > 0)
            text << "  |  " << dropouts << (dropouts == 1 ? " dropout" : " dropouts")
                 << (emulator_->emulatorBuffer() < 2 ? ": try Synth > Emulated G2 Buffer" : ": the computer is too busy for the Emulated G2");
    }
    if (synth_->boundSlot() >= 0)
        text << "  |  live: slot " << juce::String::charToString(static_cast<juce::juce_wchar>('A' + synth_->boundSlot()));
    else if (synth_->performanceBound())
        text << "  |  live: performance";
    synthStatus_.setText(text, juce::dontSendNotification);
    const auto colour = synth_->ready() ? (synth_->bound() ? juce::Colour(0xff6fd06f) : juce::Colour(0xffd0d0d0))
                                        : juce::Colour(0xff9a9ea6);
    synthStatus_.setColour(juce::Label::textColourId, colour);
}

void MainView::startEmulator(bool chooseFirmware)
{
    if (emulator_ == nullptr || !emulator_->emulatorAvailable())
        return;
    auto start = [this](const juce::File& firmware) {
        const auto error = emulator_->startEmulator(firmware);
        if (error.isNotEmpty()) {
            setStatus("Emulated G2: " + error);
            return;
        }
        userSettings().setValue("emulatorFirmware", firmware.getFullPathName());
        setStatus("Emulated G2: starting the G2 OS from " + firmware.getFileName()
                  + " (a few seconds; the patch then plays in G2fresh)");
        updateSynthStatus();
    };
    const juce::File known{userSettings().getValue("emulatorFirmware")};
    if (!chooseFirmware && known.exists()) {
        start(known);
        return;
    }
    chooseClaviaDownload("Locate Clavia's G2 OS 1.62 updater for the Mac (the .dmg as downloaded, or the updater app)",
                         start);
}

void MainView::chooseClaviaDownload(const juce::String& title, std::function<void(const juce::File&)> chosen)
{
    // Clavia's free OS updater for the Mac, as downloaded from nordkeyboards.com:
    // the .dmg itself, or an app in it (or the app's .rsrc file).
    juce::File known;
    for (const auto& path : classicSkinSources())
        if (juce::File(path).exists() && known == juce::File())
            known = juce::File(path);
    chooser_ = std::make_unique<juce::FileChooser>(
        title, known.exists() ? known : juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.dmg;*.app;*.rsrc;*.bin");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [done = std::move(chosen)](const juce::FileChooser& fc) {
                              if (const auto f = fc.getResult(); f != juce::File())
                                  done(f);
                          });
}

void MainView::startForLive()
{
    const bool otherSynth = synth_ != nullptr && synth_->kind() != SynthSync::Kind::None
                         && synth_->kind() != SynthSync::Kind::Emulated;
    const bool available = emulator_ != nullptr && emulator_->emulatorAvailable();
    switch (liveShow(available, available && emulator_->emulatorRunning(), otherSynth,
                     available ? emulator_->emulatorsInHost() : 0)) {
    case LiveShow::Start:
        startEmulator(false); // asks for the G2 OS the first time
        break;
    case LiveShow::Nothing:
        break;
    case LiveShow::NoEmulator:
        setStatus("This build of G2fresh has no Emulated G2: the Live panel plays nothing");
        break;
    case LiveShow::OtherSynth:
        setStatus("The Live panel plays the Emulated G2: Synth > Connect to Emulated G2 to play it");
        break;
    case LiveShow::OtherInstance:
        setStatus("Another G2fresh in this host runs an Emulated G2: Synth > Connect to Emulated G2 starts this "
                  "one too (two at once can drop out)");
        break;
    }
}

void MainView::setLiveMode(bool live)
{
    liveMode_ = live;
    (live ? liveButton_ : editButton_).setToggleState(true, juce::dontSendNotification);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&settings_, &browser_, &vaPane_, &divider_, &fxPane_})
        c->setVisible(!live);
    live_.setVisible(live);
    resized();
    if (live) {
        live_.focusKeyboard();
        startForLive();
    }
    menuItemsChanged();
}

juce::PopupMenu MainView::synthMenu()
{
    juce::PopupMenu m;
    if (synth_ == nullptr)
        return m;
    using Kind = SynthSync::Kind;
    m.addItem(item(kSynthG2, "Connect to G2 (USB)", {}, synth_->kind() != Kind::G2, synth_->kind() == Kind::G2));
    m.addItem(item(kSynthVirtual, "Connect to Virtual G2 (no hardware)", {}, synth_->kind() != Kind::Virtual,
                   synth_->kind() == Kind::Virtual));
    if (emulator_ != nullptr && emulator_->emulatorAvailable()) {
        m.addItem(item(kSynthEmulated, "Connect to Emulated G2 (your G2 OS, plays in G2fresh)", {},
                       synth_->kind() != Kind::Emulated, synth_->kind() == Kind::Emulated));
        m.addItem(item(kEmulatorFirmware, "Choose the G2 OS for the Emulated G2..."));
        m.addItem(item(kHostClock, "Emulated G2 Follows the Host's Tempo (MIDI Clock)", {}, true, emulator_->hostClock()));
        m.addItem(item(kMixOut34, "Mix Out 3/4 into Out 1/2 (when the Out 3/4 output is not used)", {}, true,
                       emulator_->mixOut34()));
        juce::PopupMenu buffer;
        for (int b = 0; b < 3; ++b)
            buffer.addItem(item(kEmulatorBufferBase + b, emulatorBufferName(b), {}, true, emulator_->emulatorBuffer() == b));
        m.addSubMenu("Emulated G2 Buffer (latency)", buffer);
        m.addItem(item(kEmulatorExpansion, "Emulated G2 Voice Expansion Board (8 DSPs: twice the voices and the CPU)", {},
                       true, emulator_->emulatorExpansion()));
    }
    m.addItem(item(kSynthDisconnect, "Disconnect", {}, synth_->kind() != Kind::None));
    m.addSeparator();
    const bool ready = synth_->ready();
    m.addItem(item(kSynthSettings, "Synth Settings...", kCmd + "G", ready));
    m.addSeparator();
    auto slotLabel = [this](int s) {
        const auto name = synth_->slotName(s);
        return "Slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + s))
             + (name.isNotEmpty() ? "  (" + name + ")" : juce::String());
    };
    if (doc_.isPerformance()) {
        m.addItem(item(kSendPerformance, "Send Performance to G2", {}, ready));
    } else {
        juce::PopupMenu send;
        for (int s = 0; s < 4; ++s)
            send.addItem(item(kSendPatchBase + s, slotLabel(s), {}, ready, synth_->boundSlot() == s));
        m.addSubMenu("Send Patch to Slot", send, ready);
    }
    juce::PopupMenu get;
    for (int s = 0; s < 4; ++s)
        get.addItem(item(kGetPatchBase + s, slotLabel(s), {}, ready));
    m.addSubMenu("Get Patch from Slot", get, ready);
    m.addItem(item(kGetPerformance, "Get Performance from G2", {}, ready));
    m.addItem(item(kSynthMemory, "Synth Memory (Banks)...", {}, true));
    m.addSeparator();
    m.addItem(item(kUnbind, synth_->bound() ? "Stop Live Editing" : "Live Editing (send or get a patch first)", {},
                   synth_->bound(), synth_->bound()));
    m.addSeparator();
    m.addItem(item(kShowUsbLog, "Show USB Log (for bug reports)"));
    m.addItem(item(kFullUsbLog, "Full USB Log (includes patch data)", {}, true, SynthSync::fullUsbLog()));
    return m;
}

// ---- Menus ----------------------------------------------------------------------

juce::StringArray MainView::getMenuBarNames()
{
    juce::StringArray names{"File", "Edit", "View"};
    if (synth_ != nullptr)
        names.add("Synth");
    if (onAudioSettings || midiOut_ != nullptr)
        names.add("Options");
    return names;
}

juce::PopupMenu MainView::getMenuForIndex(int index, const juce::String& name)
{
    if (name == "Synth")
        return synthMenu();
    juce::PopupMenu m;
    if (index == 0) {
        m.addItem(item(kNew, "New Patch", kCmd + "N"));
        m.addItem(item(kNewPerformance, "New Performance"));
        m.addItem(item(kOpen, "Open...", kCmd + "O"));
        juce::PopupMenu recent;
        recent_.createPopupMenuItems(recent, kRecentBase, false, true);
        if (recent.getNumItems() > 0) {
            recent.addSeparator();
            recent.addItem(item(kClearRecent, "Clear Menu"));
        }
        m.addSubMenu("Open Recent", recent, recent.getNumItems() > 0);
        m.addItem(item(kPatchLibrary, "Patch Library...", kCmd + "B"));
        m.addSeparator();
        m.addItem(item(kSave, "Save", kCmd + "S"));
        m.addItem(item(kSaveAs, "Save As...", kShift + kCmd + "S"));
        m.addSeparator();
        m.addItem(item(kPerformanceSettings, "Performance Settings...", {}, doc_.isPerformance()));
    } else if (index == 1) {
        const auto& undo = doc_;
        m.addItem(item(kUndo, "Undo", kCmd + "Z", undo.canUndo()));
        m.addItem(item(kRedo, "Redo", kShift + kCmd + "Z", undo.canRedo()));
        m.addSeparator();
        const bool selected = va_.hasSelection() || fx_.hasSelection();
        m.addItem(item(kCut, "Cut", kCmd + "X", selected));
        m.addItem(item(kCopy, "Copy", kCmd + "C", selected));
        m.addItem(item(kPaste, "Paste", kCmd + "V", !AreaView::clipboard().empty()));
        m.addItem(item(kDuplicate, "Duplicate", kCmd + "D", selected));
        m.addItem(item(kDelete, "Delete", "Delete", selected || va_.hasHighlightedCable() || fx_.hasHighlightedCable()));
        m.addItem(item(kSelectAll, "Select All", kCmd + "A"));
        m.addSeparator();
        juce::PopupMenu variations;
        for (int v = 0; v < g2::kFileVariations; ++v)
            variations.addItem(item(kCopyVariationBase + v, v < g2::kUserVariations ? "Variation " + juce::String(v + 1) : juce::String("Init"),
                                    {}, v != doc_.variation()));
        m.addSubMenu("Copy Variation " + juce::String(doc_.variation() + 1) + " To", variations);
        const juce::String target = selected ? "Selected Modules" : "Variation " + juce::String(doc_.variation() + 1);
        m.addItem(item(kRandomize, "Randomize " + target));
        m.addItem(item(kMutate, "Mutate " + target));
        m.addItem(item(kMutator, "Patch Mutator..."));
        m.addSeparator();
        m.addItem(item(kRename, doc_.isPerformance() ? "Rename Slot..." : "Rename Patch..."));
        m.addItem(item(kPatchNotes, "Patch Notes..."));
    } else if (index == 2) {
        m.addItem(item(kLiveMode, "Live (Emulated G2 Panel and Keyboard)", kCmd + "L", true, liveMode_));
        m.addSeparator();
        m.addItem(item(kZoomIn, "Zoom In", kCmd + "+", zoom_ < kMaxZoom));
        m.addItem(item(kZoomOut, "Zoom Out", kCmd + "-", zoom_ > kMinZoom));
        m.addItem(item(kZoomReset, "Actual Size", kCmd + "0"));
        m.addSeparator();
        m.addItem(item(kShowSettings, "Show Patch Settings", {}, true, !settings_.isCollapsed()));
        juce::PopupMenu cables;
        static const char* const names[7] = {"Red", "Blue", "Yellow", "Orange", "Green", "Purple", "White"};
        for (int c = 0; c < 7; ++c)
            cables.addItem(item(kCablesBase + c, names[c], {}, true, doc_.patch().header.cablesVisible[static_cast<std::size_t>(c)]));
        m.addSubMenu("Show Cables", cables);
        m.addItem(item(kRemoveBendPoints, "Remove All Bend Points", {}, g2::edit::hasCableBends(doc_.patch())));
        m.addItem(item(kClassicLook, "Classic Look", {}, true, currentLook() == Look::Classic));
        m.addItem(item(kAnimateCables, "Animate Cables", {}, true, cableAnimation()));
        m.addSeparator();
        // Deliberate deviations from the original editor, off by default.
        m.addItem(item(kWheelEditsKnobs, "Mouse Wheel Edits Knobs", {}, true, wheelEditsKnobs()));
        juce::PopupMenu doubleClick;
        const auto dc = knobDoubleClick();
        doubleClick.addItem(item(kDoubleClickMorph, "Drag the Morph Range (as the original)", {}, true, dc == KnobDoubleClick::Morph));
        doubleClick.addItem(item(kDoubleClickReset, "Reset to the Default Value", {}, true, dc == KnobDoubleClick::Reset));
        doubleClick.addItem(item(kDoubleClickNothing, "Nothing", {}, true, dc == KnobDoubleClick::Nothing));
        m.addSubMenu("Double-Click on a Knob", doubleClick);
        m.addItem(item(kMorphGroupColours, "Morph Groups in Eight Colours", {}, true, morphGroupColours()));
        m.addItem(item(kShowLoadEstimate, "Show Load Estimate Offline", {}, true, showLoadEstimate()));
    } else if (name == "Options") {
        if (onAudioSettings)
            m.addItem(item(kAudioSettings, "Audio/MIDI Settings..."));
        if (midiOut_ != nullptr) {
            // The G2's USB connection carries no notes: MIDI goes to the
            // synth through a MIDI interface. (Not the emulated G2's MIDI
            // OUT, which is the plugin's MIDI output: Audio/MIDI Settings
            // in the stand-alone app.)
            juce::PopupMenu out;
            const auto current = midiOut_->deviceIdentifier();
            out.addItem(item(kMidiOutOff, "Off", {}, true, current.isEmpty()));
            const auto devices = midiOut_->availableDevices();
            if (!devices.isEmpty())
                out.addSeparator();
            for (int i = 0; i < devices.size() && i < kMidiChannelBase - kMidiOutBase; ++i)
                out.addItem(item(kMidiOutBase + i, devices[i].name, {}, true, devices[i].identifier == current));
            out.addSeparator();
            juce::PopupMenu channels;
            channels.addItem(item(kMidiChannelBase, "As Played", {}, true, midiOut_->channel() == 0));
            for (int c = 1; c <= 16; ++c)
                channels.addItem(item(kMidiChannelBase + c, "Channel " + juce::String(c), {}, true, midiOut_->channel() == c));
            out.addSubMenu("Channel", channels, current.isNotEmpty());
            m.addSubMenu("Forward MIDI to a Hardware G2", out);
        }
    }
    return m;
}

void MainView::menuItemSelected(int id, int)
{
    if (id == kWheelEditsKnobs) {
        setWheelEditsKnobs(!wheelEditsKnobs());
        setStatus(wheelEditsKnobs() ? juce::String("The mouse wheel over a knob changes its value")
                                    : juce::String("The mouse wheel scrolls the patch, as in the original editor"));
        menuItemsChanged();
        return;
    }
    if (id >= kDoubleClickMorph && id <= kDoubleClickNothing) {
        setKnobDoubleClick(id == kDoubleClickMorph   ? KnobDoubleClick::Morph
                           : id == kDoubleClickReset ? KnobDoubleClick::Reset
                                                     : KnobDoubleClick::Nothing);
        menuItemsChanged();
        return;
    }
    if (id == kMorphGroupColours) {
        setMorphGroupColours(!morphGroupColours());
        setStatus(morphGroupColours()
                      ? juce::String("Morphed parameters show their group's colour")
                      : juce::String("Morphed parameters show the focused group, red, and other groups, blue, as in the original editor"));
        for (auto* a : {&va_, &fx_})
            a->settingsChanged();
        settings_.repaintAll();
        menuItemsChanged();
        return;
    }
    if (id == kShowLoadEstimate) {
        setShowLoadEstimate(!showLoadEstimate());
        updateLoad();
        menuItemsChanged();
        return;
    }
    if (id >= kEmulatorBufferBase && id < kEmulatorBufferBase + 3 && emulator_ != nullptr) {
        emulator_->setEmulatorBuffer(id - kEmulatorBufferBase);
        setStatus("Emulated G2 buffer: " + emulatorBufferName(id - kEmulatorBufferBase)
                  + ". A smaller buffer plays sooner; if you hear clicks or gaps, choose a larger one");
        updateSynthStatus();
        menuItemsChanged();
        return;
    }
    if (id == kEmulatorExpansion && emulator_ != nullptr) {
        if (emulator_->emulatorExpansion()) {
            emulator_->setEmulatorExpansion(false);
            setStatus("Voice expansion board removed: four DSPs, a G2's voices");
            updateSynthStatus();
            menuItemsChanged();
            return;
        }
        // Before fitting it: how fast this computer runs a heavy patch on eight DSPs, and the user decides
        juce::Component::SafePointer<MainView> self(this);
        if (!emulator_->testExpansion([self](const ExpansionTest& t) {
                if (self != nullptr)
                    self->offerExpansion(t);
            })) {
            setStatus("The computer test cannot start: choose the G2 OS first (Synth > Choose the G2 OS for the Emulated G2...)");
            return;
        }
        setStatus("Testing this computer with eight DSPs (about 20 s; the Emulated G2 stops meanwhile)...");
        return;
    }
    if (id == kMixOut34 && emulator_ != nullptr) {
        emulator_->setMixOut34(!emulator_->mixOut34());
        setStatus(emulator_->mixOut34() ? juce::String("Out 3/4 are mixed into Out 1/2 unless the host takes the Out 3/4 output")
                                        : juce::String("Out 1/2 only, as the G2's headphones: route the Out 3/4 output to hear Out 3/4"));
        return;
    }
    if (id == kHostClock && emulator_ != nullptr) {
        emulator_->setHostClock(!emulator_->hostClock());
        setStatus(emulator_->hostClock() ? juce::String("The Emulated G2's master clock follows the host's tempo and transport")
                                         : juce::String("The Emulated G2's master clock runs on its own"));
        return;
    }
    if (synth_ != nullptr && (id == kSynthEmulated || id == kEmulatorFirmware)) {
        startEmulator(id == kEmulatorFirmware);
        return;
    }
    if (synth_ != nullptr && id >= kSynthG2 && id < kGetPatchBase + 4) {
        if (id == kSynthG2)
            synth_->connectG2();
        else if (id == kSynthVirtual)
            synth_->connectVirtual();
        else if (id == kSynthDisconnect)
            synth_->disconnect();
        else if (id == kSendPerformance)
            synth_->sendPerformance();
        else if (id == kGetPerformance)
            confirmDiscard([this] { synth_->getPerformance(); });
        else if (id == kUnbind)
            synth_->unbind();
        else if (id == kShowUsbLog) {
            // Written by g2bridge while it talks to a real G2.
            const juce::File log{juce::String(g2::proto::defaultUsbLogPath())};
            if (log.existsAsFile())
                log.revealToUser();
            else
                setStatus("No USB log yet: it is written while G2fresh talks to a real G2 (" + log.getFullPathName() + ")");
        }
        else if (id == kFullUsbLog) {
            SynthSync::setFullUsbLog(!SynthSync::fullUsbLog());
            setStatus(SynthSync::fullUsbLog()
                          ? juce::String("The USB log will include patch contents (from the next connection to a G2)")
                          : juce::String("The USB log will leave patch contents out (from the next connection to a G2)"));
        }
        else if (id == kSynthSettings)
            showSynthSettings();
        else if (id == kSynthMemory) {
            if (!bankBrowser_)
                bankBrowser_ = std::make_unique<BankBrowser>(*synth_, [this](std::function<void()> then) {
                    confirmDiscard(std::move(then));
                });
            bankBrowser_->setVisible(true);
            bankBrowser_->toFront(true);
        }
        else if (id >= kGetPatchBase)
            confirmDiscard([this, slot = id - kGetPatchBase] { synth_->getPatch(slot); });
        else if (id >= kSendPatchBase)
            synth_->sendPatch(id - kSendPatchBase);
        return;
    }
    if (midiOut_ != nullptr && id >= kMidiOutOff && id < kMidiChannelBase) {
        const auto devices = midiOut_->availableDevices();
        const int i = id - kMidiOutBase;
        midiOut_->setDevice(juce::isPositiveAndBelow(i, devices.size()) ? devices[i].identifier : juce::String());
        setStatus(midiOut_->deviceIdentifier().isEmpty() ? juce::String("MIDI forwarded to a hardware G2: off")
                                                         : "MIDI forwarded to a hardware G2 through " + devices[i].name);
        return;
    }
    if (midiOut_ != nullptr && id >= kMidiChannelBase && id <= kMidiChannelBase + 16) {
        midiOut_->setChannel(id - kMidiChannelBase);
        return;
    }
    if (id >= kCablesBase && id < kCablesBase + 7) {
        const auto colour = static_cast<g2::CableColor>(id - kCablesBase);
        const bool visible = !doc_.patch().header.cablesVisible[static_cast<std::size_t>(colour)];
        doc_.perform(visible ? "Show cables" : "Hide cables",
                     [&](g2::Patch& p) { g2::edit::setCablesVisible(p, colour, visible); });
        return;
    }
    if (id >= kCopyVariationBase && id < kCopyVariationBase + g2::kFileVariations) {
        const auto from = static_cast<std::uint8_t>(doc_.variation());
        const auto to = static_cast<std::uint8_t>(id - kCopyVariationBase);
        doc_.perform("Copy variation", [&](g2::Patch& p) { g2::edit::copyVariation(p, from, to); });
        setStatus("Copied variation " + juce::String(from + 1) + " to "
                  + (to < g2::kUserVariations ? "variation " + juce::String(to + 1) : juce::String("the init variation")));
        return;
    }
    if (id >= kRecentBase) {
        const auto f = recent_.getFile(id - kRecentBase);
        confirmDiscard([this, f] { openFile(f); });
        return;
    }
    switch (id) {
    case kNew: newPatch(); break;
    case kNewPerformance: confirmDiscard([this] { doc_.newPerformance(); }); break;
    case kPerformanceSettings: showPerformanceSettings(doc_, this); break;
    case kCut: copySelection(true); break;
    case kCopy: copySelection(false); break;
    case kPaste: pasteClipboard(); break;
    case kDuplicate:
        if (auto* a = areaWithSelection())
            a->duplicateSelection();
        break;
    case kSelectAll: activeArea().selectAll(); break;
    case kRemoveBendPoints:
        doc_.perform("Remove all bend points", [](g2::Patch& p) { g2::edit::clearCableBends(p); });
        break;
    case kPatchNotes: showPatchNotes(doc_, this); break;
    case kRandomize: randomize(false); break;
    case kMutate: randomize(true); break;
    case kMutator: showMutator(); break;
    case kOpen: open(); break;
    case kPatchLibrary: showLibrary(); break;
    case kSave: save(false); break;
    case kSaveAs: save(true); break;
    case kClearRecent:
        recent_.clear();
        userSettings().setValue("recentFiles", recent_.toString());
        break;
    case kUndo: doc_.undo(); break;
    case kRedo: doc_.redo(); break;
    case kDelete:
        // A highlighted cable first (clicked on its cord), else the selected modules.
        if (activeArea().hasHighlightedCable())
            activeArea().deleteHighlightedCable();
        else if (auto* a = areaWithSelection())
            a->deleteSelected();
        break;
    case kRename: name_.showEditor(); break;
    case kLiveMode: setLiveMode(!liveMode_); break;
    case kZoomIn: setZoom(zoom_ * 1.25f); break;
    case kZoomOut: setZoom(zoom_ / 1.25f); break;
    case kZoomReset: setZoom(1.0f); break;
    case kShowSettings: settings_.setCollapsed(!settings_.isCollapsed()); break;
    case kClassicLook: setLook(currentLook() == Look::Classic ? Look::Modern : Look::Classic); break;
    case kAnimateCables: setAnimation(!cableAnimation()); break;
    case kAudioSettings:
        if (onAudioSettings)
            onAudioSettings();
        break;
    default: break;
    }
}

AreaView* MainView::areaWithSelection()
{
    if (activeArea_ && activeArea_->hasSelection())
        return activeArea_;
    return va_.hasSelection() ? &va_ : fx_.hasSelection() ? &fx_ : nullptr;
}

void MainView::copySelection(bool cut)
{
    if (auto* a = areaWithSelection()) {
        AreaView::clipboard() = a->copySelection();
        if (cut)
            a->deleteSelected();
        setStatus(juce::String(static_cast<int>(AreaView::clipboard().modules.size())) + " module(s) "
                  + (cut ? "cut" : "copied"));
        menuItemsChanged();
    }
}

void MainView::randomize(bool mutateOnly)
{
    g2::mutate::Scope scope;
    scope.variation = static_cast<std::uint8_t>(doc_.variation());
    if (auto* a = areaWithSelection())
        for (auto index : a->selection())
            scope.modules.emplace_back(a->location(), index);
    const g2::mutate::Settings settings; // the original dialog's defaults
    auto& rng = random_;
    doc_.perform(mutateOnly ? "Mutate" : "Randomize", [&](g2::Patch& p) {
        if (mutateOnly)
            g2::mutate::mutate(p, scope, settings, rng);
        else
            g2::mutate::randomize(p, scope, settings, rng);
    });
    setStatus(juce::String(mutateOnly ? "Mutated " : "Randomized ")
              + (scope.modules.empty() ? "variation " + juce::String(doc_.variation() + 1)
                                       : juce::String(static_cast<int>(scope.modules.size())) + " module(s)")
              + " (locked modules and switches are kept)");
}

void MainView::showMutator()
{
    if (!mutator_)
        mutator_ = std::make_unique<MutatorWindow>(doc_);
    mutator_->setVisible(true);
    mutator_->toFront(true);
}

void MainView::pasteClipboard()
{
    activeArea().paste(AreaView::clipboard());
}

void MainView::setLook(Look look)
{
    if (look == Look::Classic && !Skin::get().classicLoaded()) {
        // The Classic look's graphics come from the user's own copy of Clavia's
        // download: the one already chosen (for the Emulated G2 too), else ask.
        juce::MouseCursor::showWaitCursor(); // the first time, a second or so to extract them
        const auto error = ensureClassicSkin();
        juce::MouseCursor::hideWaitCursor();
        if (error.isNotEmpty()) {
            setStatus(error);
            chooseClaviaDownload("Locate your copy of Clavia's \"Nord Modular G2 OS v1.62 Update.dmg\" (the Classic look "
                                 "is drawn with the original editor's graphics in it)",
                                 [this](const juce::File& f) {
                                     juce::MouseCursor::showWaitCursor();
                                     const auto e = Skin::get().loadClassic(f);
                                     juce::MouseCursor::hideWaitCursor();
                                     if (e.isNotEmpty()) {
                                         setStatus("Classic look: " + e);
                                         return;
                                     }
                                     rememberClassicSkinSource(f);
                                     setStatus("Classic look: the original editor's graphics, from " + f.getFileName());
                                     setLook(Look::Classic);
                                 });
            return;
        }
    }
    setCurrentLook(look);
    for (auto* a : {&va_, &fx_}) {
        a->settingsChanged();
        a->repaint();
    }
    menuItemsChanged();
}

void MainView::setAnimation(bool on)
{
    setCableAnimation(on);
    va_.settingsChanged();
    fx_.settingsChanged();
    menuItemsChanged();
}

// ---- Zoom ----------------------------------------------------------------------

void MainView::setZoom(float zoom, const AreaView* area, std::optional<juce::Point<int>> anchor, bool remember)
{
    zoom = juce::jlimit(kMinZoom, kMaxZoom, zoom);
    if (std::abs(zoom - 1.0f) < 0.03f)
        zoom = 1.0f; // snap to 100%
    const float old = zoom_;
    // Without a mouse anchor, focus on the module last added or edited.
    const AreaView* focusArea = nullptr;
    std::optional<juce::Point<int>> focus;
    if (!anchor) {
        for (const AreaView* a : {&va_, &fx_})
            if (const auto fp = a->focusPoint(); fp && (!focusArea || a->lastTouchTime() > focusArea->lastTouchTime())) {
                focusArea = a;
                focus = fp;
            }
    }
    struct Pane { juce::Viewport& port; ZoomHolder& holder; const AreaView& view; };
    for (Pane p : {Pane{vaPort_, vaZoom_, va_}, Pane{fxPort_, fxZoom_, fx_}}) {
        const auto viewSize = juce::Point<float>(static_cast<float>(p.port.getViewWidth()), static_cast<float>(p.port.getViewHeight()));
        p.holder.setZoom(zoom);
        const_cast<AreaView&>(p.view).setMinimumSize(juce::roundToInt(viewSize.x / zoom), juce::roundToInt(viewSize.y / zoom));
        if (focus && focusArea == &p.view) {
            // Centre the focused module in its view.
            p.port.setViewPosition((focus->toFloat() * zoom - viewSize * 0.5f).roundToInt());
            continue;
        }
        // Keep the mouse anchor (or the top-left corner) in place.
        juce::Point<float> inView;
        if (anchor && area == &p.view)
            inView = anchor->toFloat() * old - p.port.getViewPosition().toFloat();
        const auto content = (p.port.getViewPosition().toFloat() + inView) / old;
        p.port.setViewPosition((content * zoom - inView).roundToInt());
    }
    zoom_ = zoom;
    zoomReset_.setButtonText(juce::String(juce::roundToInt(zoom * 100.0f)) + "%");
    zoomOut_.setEnabled(zoom > kMinZoom);
    zoomIn_.setEnabled(zoom < kMaxZoom);
    if (remember)
        userSettings().setValue("zoom", static_cast<double>(zoom));
    menuItemsChanged();
}

// ---- Toolbar and layout -------------------------------------------------------------

void MainView::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    updateSynthStatus();
    if (source == synth_) {
        va_.liveLedsChanged();
        fx_.liveLedsChanged();
        updateLoad();
        menuItemsChanged(); // slot names, connection state
        return;
    }
    updateToolbar();
    menuItemsChanged(); // undo/redo state
}

void MainView::updateToolbar()
{
    // While the Patch Mutator plays an individual in variation 9, no
    // variation button is lit (the original focuses variation 9).
    for (int i = 0; i < variations_.size(); ++i)
        variations_[i]->setToggleState(i == doc_.focusedVariation(), juce::dontSendNotification);
    const auto* perf = doc_.performance();
    for (int i = 0; i < slots_.size(); ++i) {
        auto* b = slots_[i];
        b->setVisible(perf != nullptr);
        if (perf) {
            const auto name = PatchDocument::fromG2Bytes(perf->header.slots[static_cast<std::size_t>(i)].patchName);
            b->setTooltip("Slot " + b->getButtonText() + (name.isNotEmpty() ? ": " + name : juce::String()));
        }
        b->setToggleState(i == doc_.slot(), juce::dontSendNotification);
    }
    // In a performance: "<performance file>  A:" then the slot's patch name.
    if (perf) {
        const auto f = doc_.file();
        slotPrefix_.setText((f == juce::File() ? juce::String("New performance") : f.getFileNameWithoutExtension()) + "   "
                                + juce::String::charToString(static_cast<juce::juce_wchar>('A' + doc_.slot())) + ":",
                            juce::dontSendNotification);
    }
    slotPrefix_.setVisible(perf != nullptr);
    if (!name_.isBeingEdited())
        name_.setText(doc_.name(), juce::dontSendNotification);
    name_.setTooltip(perf ? "Slot patch name: double-click to rename (up to 16 characters)"
                          : "Patch name: double-click to rename (up to 16 characters). Save uses it as the file name.");
    edited_.setText(doc_.isDirty() ? "(edited)" : "", juce::dontSendNotification);
    updateLoad();
    const int voices = doc_.patch().header.voiceCount;
    vaPane_.setText("VOICE AREA",
                    "Polyphonic: the synth runs one copy per voice (" + juce::String(voices)
                        + (voices == 1 ? " voice" : " voices") + " in this patch). Oscillators, filters, envelopes...",
                    "Modules here are duplicated for every voice the patch can play. Send the sound out with an Out "
                    "module (2-Out, 4-Out), or to the FX area with its Fx/Bus setting.");
    resized();
}

void MainView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff2e3035));
}

MainView::AreaPane::AreaPane(juce::Viewport& port, juce::Colour marker) : port_(port), marker_(marker)
{
    addAndMakeVisible(port_);
}

void MainView::AreaPane::setText(const juce::String& title, const juce::String& subtitle, const juce::String& tooltip)
{
    title_ = title;
    subtitle_ = subtitle;
    setHelpText(tooltip);
    repaint();
}

void MainView::AreaPane::resized()
{
    port_.setBounds(getLocalBounds().withTrimmedTop(26));
}

void MainView::AreaPane::paint(juce::Graphics& g)
{
    auto bar = getLocalBounds().removeFromTop(26);
    g.setColour(juce::Colour(0xff25272b));
    g.fillRect(bar);
    g.setColour(marker_);
    g.fillRoundedRectangle(bar.removeFromLeft(10).reduced(3, 5).toFloat(), 2.0f);
    g.setColour(juce::Colours::white);
    g.setFont(theme::font(true));
    const int titleWidth = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), title_) + 12;
    g.drawText(title_, bar.removeFromLeft(titleWidth), juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffa8adb6));
    g.setFont(theme::font());
    g.drawText(subtitle_, bar.reduced(4, 0), juce::Justification::centredLeft, true);
}

void MainView::resized()
{
    auto r = getLocalBounds();
    if (menuBar_)
        menuBar_->setBounds(r.removeFromTop(24));
    auto bar = r.removeFromTop(34).reduced(6, 4);
    if (!standalone_) {
        menuButton_.setBounds(bar.removeFromLeft(64));
        bar.removeFromLeft(10);
    }
    auto textWidth = [](const juce::Label& l) {
        return juce::GlyphArrangement::getStringWidthInt(l.getFont(), l.getText()) + 12;
    };
    if (slotPrefix_.isVisible())
        slotPrefix_.setBounds(bar.removeFromLeft(textWidth(slotPrefix_)));
    name_.setBounds(bar.removeFromLeft(std::max(150, textWidth(name_))));
    edited_.setBounds(bar.removeFromLeft(70));
    editButton_.setBounds(bar.removeFromLeft(48));
    liveButton_.setBounds(bar.removeFromLeft(48));

    zoomIn_.setBounds(bar.removeFromRight(28));
    zoomReset_.setBounds(bar.removeFromRight(54));
    zoomOut_.setBounds(bar.removeFromRight(28));
    bar.removeFromRight(16);
    if (doc_.isPerformance()) {
        for (int i = slots_.size(); --i >= 0;)
            slots_[i]->setBounds(bar.removeFromRight(28)), bar.removeFromRight(2);
        bar.removeFromRight(16);
    }
    mutatorButton_.setBounds(bar.removeFromRight(92));
    bar.removeFromRight(4);
    randomizeButton_.setBounds(bar.removeFromRight(92));
    bar.removeFromRight(16);
    for (int i = variations_.size(); --i >= 0;)
        variations_[i]->setBounds(bar.removeFromRight(28)), bar.removeFromRight(2);
    variationLabel_.setBounds(bar.removeFromRight(textWidth(variationLabel_)));

    {
        auto bottom = r.removeFromBottom(24);
        load_.setBounds(bottom.removeFromRight(std::min(560, bottom.getWidth() / 2)));
        if (synth_ != nullptr)
            synthStatus_.setBounds(bottom.removeFromRight(std::min(300, bottom.getWidth() / 2)));
        status_.setBounds(bottom);
    }
    if (liveMode_) {
        live_.setBounds(r);
        return;
    }
    settings_.setBounds(r.removeFromTop(settings_.preferredHeight(r.getWidth())));
    browser_.setBounds(r.removeFromTop(62));
    juce::Component* parts[] = {&vaPane_, &divider_, &fxPane_};
    layout_.layOutComponents(parts, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
    for (auto [port, area] : {std::pair{&vaPort_, &va_}, std::pair{&fxPort_, &fx_}})
        area->setMinimumSize(juce::roundToInt(static_cast<float>(port->getWidth()) / zoom_),
                             juce::roundToInt(static_cast<float>(port->getHeight()) / zoom_));
}

bool MainView::keyPressed(const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress('z', cmd, 0))
        return doc_.undo(), true;
    if (key == juce::KeyPress('z', cmd | juce::ModifierKeys::shiftModifier, 0) || key == juce::KeyPress('y', cmd, 0))
        return doc_.redo(), true;
    if (key == juce::KeyPress('s', cmd, 0))
        return save(false), true;
    if (key == juce::KeyPress('s', cmd | juce::ModifierKeys::shiftModifier, 0))
        return save(true), true;
    if (key == juce::KeyPress('o', cmd, 0))
        return open(), true;
    if (key == juce::KeyPress('n', cmd, 0))
        return newPatch(), true;
    if (key == juce::KeyPress('b', cmd, 0))
        return showLibrary(), true;
    if (key == juce::KeyPress('c', cmd, 0))
        return copySelection(false), true;
    if (key == juce::KeyPress('x', cmd, 0))
        return copySelection(true), true;
    if (key == juce::KeyPress('v', cmd, 0))
        return pasteClipboard(), true;
    if (key == juce::KeyPress('d', cmd, 0)) {
        if (auto* a = areaWithSelection())
            a->duplicateSelection();
        return true;
    }
    if (key == juce::KeyPress('a', cmd, 0))
        return activeArea().selectAll(), true;
    if (key == juce::KeyPress('=', cmd, 0) || key == juce::KeyPress('+', cmd, 0)
        || key == juce::KeyPress('=', cmd | juce::ModifierKeys::shiftModifier, 0))
        return setZoom(zoom_ * 1.25f), true;
    if (key == juce::KeyPress('g', cmd, 0)) {
        showSynthSettings();
        return true;
    }
    if (key == juce::KeyPress('l', cmd, 0)) {
        setLiveMode(!liveMode_);
        return true;
    }
    if (key == juce::KeyPress('-', cmd, 0))
        return setZoom(zoom_ / 1.25f), true;
    if (key == juce::KeyPress('0', cmd, 0))
        return setZoom(1.0f), true;
    for (int i = 0; i < g2::kUserVariations; ++i)
        if (key == juce::KeyPress('1' + i, cmd, 0))
            return doc_.setVariation(i), true;
    return false;
}

// ---- Synth ----------------------------------------------------------------------

void MainView::showSynthSettings()
{
    if (synth_ == nullptr || synthSettings_ != nullptr)
        return;
    synthSettings_ = openSynthSettings(*synth_, this, [safe = juce::Component::SafePointer<MainView>(this)] {
        if (safe)
            safe->synthSettings_.reset();
    });
}

void MainView::showLibrary()
{
    // The original's Patch Browser (Tools > Patch Browser, Ctrl-B: CWindowTogglerEx<CBrowser>::Toggle 0x19efbc).
    if (library_ != nullptr && library_->isVisible() && library_->isActiveWindow()) {
        library_->closeButtonPressed();
        return;
    }
    if (!library_)
        library_ = std::make_unique<PatchLibrary>(LibraryHost{
            doc_, synth_, [this](std::function<void()> then) { confirmDiscard(std::move(then)); },
            [this](const juce::File& f) { openFile(f); }});
    library_->open();
}

void MainView::showFlashProblem(const juce::String& text)
{
    // CFlashListView::Idle 0x6e44, CDialogSaveInSynth::Idle 0x3dc80: "[WARN][Flash Problem]%s[OK]", over the window
    // the store came from.
    juce::Component* over = this;
    if (bankBrowser_ != nullptr && bankBrowser_->isVisible())
        over = bankBrowser_.get();
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Flash Problem", text, "OK", over);
}

// ---- Files ----------------------------------------------------------------------

void MainView::confirmDiscard(std::function<void()> then)
{
    if (!doc_.isDirty()) {
        then();
        return;
    }
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Discard changes?",
                                       "\"" + doc_.name() + "\" has unsaved changes.", "Discard", "Cancel", this,
                                       juce::ModalCallbackFunction::create([then](int r) {
                                           if (r == 1)
                                               then();
                                       }));
}

void MainView::discardChanges()
{
    if (!doc_.isDirty())
        return;
    const auto f = doc_.file();
    juce::MemoryBlock mb;
    bool reverted = false;
    if (f.existsAsFile() && f.loadFileAsData(mb)) {
        try {
            const auto* p = static_cast<const std::uint8_t*>(mb.getData());
            doc_.loadBytes(std::vector<std::uint8_t>(p, p + mb.getSize()), true);
            doc_.setFile(f);
            if (const auto layout = cablelayout::fileFor(f); layout.existsAsFile())
                doc_.applyLayoutJson(layout.loadFileAsString());
            reverted = true;
        } catch (const std::exception&) {
        }
    }
    if (!reverted)
        doc_.newPatch();
    doc_.dispatchPendingMessages(); // the saved session state follows now, not after quitting
}

void MainView::newPatch()
{
    confirmDiscard([this] { doc_.newPatch(); });
}

void MainView::open()
{
    confirmDiscard([this] {
        chooser_ = std::make_unique<juce::FileChooser>("Open a G2 patch or performance", doc_.file(), kOpenPattern);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File())
                                      openFile(fc.getResult());
                              });
    });
}

void MainView::openFile(const juce::File& f)
{
    loadFile(f);
}

void MainView::loadFile(const juce::File& f, bool ignoreChecksum)
{
    juce::MemoryBlock mb;
    if (!f.loadFileAsData(mb)) {
        setStatus("Cannot read " + f.getFullPathName());
        return;
    }
    try {
        const auto* p = static_cast<const std::uint8_t*>(mb.getData());
        doc_.loadBytes(std::vector<std::uint8_t>(p, p + mb.getSize()), ignoreChecksum);
        doc_.setFile(f);
        // Cable shapes, kept beside the patch (see CableLayout.h).
        if (const auto layout = cablelayout::fileFor(f); layout.existsAsFile())
            doc_.applyLayoutJson(layout.loadFileAsString());
        recent_.addFile(f);
        userSettings().setValue("recentFiles", recent_.toString());
        // The original converts an old file silently (it asks only when it is
        // saved); G2fresh also says so here.
        juce::StringArray notes;
        if (doc_.convertedFrom() != 0)
            notes.add("converted from version " + juce::String(doc_.convertedFrom()));
        if (ignoreChecksum)
            notes.add("checksum ignored");
        setStatus("Opened " + f.getFileName() + (notes.isEmpty() ? juce::String() : " (" + notes.joinIntoString(", ") + ")"));
        updateToolbar();
        // The original repairs what it cannot load as it is, and says so.
        if (const auto report = doc_.loadReport(); report.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, f.getFileName(),
                                                   report.substring(0, 1).toUpperCase() + report.substring(1) + ".", {}, this);
    } catch (const g2::ChecksumError&) {
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon, "Checksum error",
            f.getFileName() + "'s checksum is wrong: the file may be damaged, or was written by another "
                              "program. Open it anyway?",
            "Open anyway", "Cancel", this,
            juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<MainView>(this), f](int r) {
                if (safe && r == 1)
                    safe->loadFile(f, true);
            }));
    } catch (const std::exception& e) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot open patch",
                                               f.getFileName() + ": " + e.what(), {}, this);
    }
}

juce::String MainView::suggestedFileName() const
{
    // Utils::MakeFileNameFromPatchName: the name with characters a file name
    // can't take as spaces.
    const auto name = juce::String(g2::edit::fileNameForPatch(doc_.name().toStdString())).trim();
    return name.isEmpty() ? juce::String("No name") : name;
}

void MainView::saveTo(const juce::File& f)
{
    // A patch converted from an old version when it was opened: the original
    // asks first, whichever way it is saved (CPatch::SaveToFile @000da3f4 ->
    // CFilePortUtils::ConvertAlert @000308c4, "[WARN][Convert File]File will
    // be converted to new format.[Cancel][OK]"). Cancel saves nothing; OK
    // saves in the current format, and the question is not asked again.
    if (doc_.conversionPending()) {
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon, "Convert File", "File will be converted to new format.", "OK",
            "Cancel", this,
            juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<MainView>(this), f](int r) {
                if (safe == nullptr || r != 1)
                    return;
                safe->doc_.conversionDone();
                safe->writeFile(f);
            }));
        return;
    }
    writeFile(f);
}

void MainView::writeFile(const juce::File& f)
{
    std::vector<std::uint8_t> bytes;
    try {
        bytes = doc_.saveBytes();
    } catch (const std::exception& e) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot save patch", e.what(), {},
                                               this);
        return;
    }
    if (f.replaceWithData(bytes.data(), bytes.size())) {
        // Cable shapes go to the layout file beside the patch; without
        // bent cables an old layout file is removed.
        const auto layout = cablelayout::fileFor(f);
        if (const auto json = doc_.layoutJson(); json.isNotEmpty())
            layout.replaceWithText(json);
        else if (layout.existsAsFile())
            layout.deleteFile();
        const auto name = doc_.name();
        doc_.setFile(f);
        if (doc_.isPerformance())
            doc_.setName(name);
        doc_.conversionDone(); // the file is in the current format now
        doc_.markSaved();
        recent_.addFile(f);
        userSettings().setValue("recentFiles", recent_.toString());
        setStatus("Saved " + f.getFileName());
    } else {
        setStatus("Cannot write " + f.getFullPathName());
    }
}

void MainView::save(bool saveAs)
{
    // A patch saved under a new name goes to a new file.
    const bool renamed = !doc_.isPerformance() && doc_.file() != juce::File()
                         && doc_.file().getFileNameWithoutExtension() != doc_.name();
    if (!saveAs && !renamed && doc_.file() != juce::File()) {
        saveTo(doc_.file());
        return;
    }
    const auto ext = doc_.fileExtension();
    const auto folder = doc_.file() != juce::File() ? doc_.file().getParentDirectory()
                                                    : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    const auto suggested = doc_.isPerformance() && doc_.file() != juce::File()
                               ? doc_.file()
                               : folder.getChildFile(suggestedFileName() + ext);
    chooser_ = std::make_unique<juce::FileChooser>(doc_.isPerformance() ? "Save the G2 performance" : "Save the G2 patch",
                                                   suggested, "*" + ext);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, ext](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              saveTo(f.hasFileExtension(ext) ? f : f.withFileExtension(ext));
                          });
}

} // namespace g2ui
