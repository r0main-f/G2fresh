#include "MainView.h"

#include "BackgroundMenu.h"
#include "BankBrowser.h"

#include "g2/proto/logging_transport.hpp"
#include "CableLayout.h"

#include "Dialogs.h"
#include "LiveEditing.h"
#include "LiveShow.h"
#include "MenuIds.h"
#include "PatchKeys.h"
#include "PatchLibrary.h"
#include "PatchLibraryModel.h"
#include "PerfSwitch.h"
#include "SynthSettingsWindow.h"
#include "ToolbarLayout.h"
#include "ZoomFit.h"

#include "g2/patch_load.hpp"

namespace g2ui {
namespace {

const juce::String kOpenPattern = "*.pch2;*.prf2";

using namespace menuid;


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
const juce::String kMorphKeyName = "Option";
#else
const juce::String kCmd = "Ctrl+";
const juce::String kShift = "Shift+";
const juce::String kMorphKeyName = "Ctrl";
#endif

} // namespace

MainView::MainView(PatchDocument& doc, bool standalone)
    : doc_(doc), standalone_(standalone), va_(doc, g2::Location::Va), fx_(doc, g2::Location::Fx)
{
    setLookAndFeel(&lookAndFeel_.get());
    recent_.setMaxNumberOfItems(kRecentMax);
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
                               "live. Pick the one to see and edit (keys 1-8).");
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
    // The original's Init button beside the variations: the init variation into the one shown.
    varInit_.setTooltip("Init: recall the variation init (the Init variation's values into this variation)");
    varInit_.onClick = [this] { recallVariationInit(); };
    addAndMakeVisible(varInit_);
    for (int i = 0; i < 4; ++i) {
        auto* b = slots_.add(new juce::TextButton(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i))));
        b->setClickingTogglesState(true);
        b->setRadioGroupId(2);
        b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3070c0));
        b->onClick = [this, i] { slotClicked(i); };
        addChildComponent(b);
        addChildComponent(keyboardMarks_.add(new KeyboardMark()));
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
    // The background pop-up's synth items (BackgroundMenu.h): a patch to a slot or Local, a performance on or off.
    auto lastCc = [this] { return synth_ != nullptr ? synth_->lastMidiCc() : std::nullopt; };
    settings_.lastMidiCc = lastCc;
    for (auto* area : {&va_, &fx_})
        area->lastMidiCc = lastCc;
    for (auto* area : {&va_, &fx_})
        area->addSynthItems = [this](juce::PopupMenu& m) {
            if (synth_ == nullptr)
                return;
            const auto items = synthItems(synth_->ready(), doc_.isPerformance(), synth_->performanceBound(), synth_->boundSlot());
            for (std::size_t i = 0; i < items.size(); ++i) {
                juce::PopupMenu::Item it(items[i].label);
                it.itemID = 20000 + static_cast<int>(i); // not one of the area's own ids
                it.isTicked = items[i].ticked;
                const int slot = static_cast<int>(i);
                if (doc_.isPerformance())
                    it.action = [this] { synth_->performanceBound() ? synth_->unbind() : synth_->sendPerformance(); };
                else if (slot < 4)
                    it.action = [this, slot] { synth_->sendPatch(slot); };
                else
                    it.action = [this] { synth_->unbind(); };
                m.addItem(std::move(it));
            }
        };
    settings_.voicesOnSynth = [this] {
        return synth_ != nullptr ? std::pair{synth_->patchConnected(), synth_->currentVoices()}
                                 : std::pair<bool, std::optional<std::uint8_t>>{false, std::nullopt};
    };
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
        area->onArrowKey = [this](const juce::KeyPress& key) { return keyPressed(key); };
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
    randomizeButton_.setTooltip("Randomize the current variation (or the selected modules). Modules excluded from mutation, "
                                "switches and modes are kept. Undo to go back.");
    randomizeButton_.onClick = [this] { randomize(false); };
    mutatorButton_.setTooltip("Breed new versions of the current variation: randomize, mutate, cross, audition");
    mutatorButton_.onClick = [this] { showMutator(); };
    addAndMakeVisible(randomizeButton_);
    // The focused morph group, beside Randomize and the Mutator: the original keeps its morph dials in the patch
    // window's toolbar (a click on one focuses its group, CMorphContainer::Click @00123a84); G2fresh's dials are in the
    // settings strip, collapsed by default, so the group can be chosen here too.
    morphGroup_.setTooltip("The focused morph group: the one Morph Assign, the morph drag and Option+arrows act on");
    morphGroup_.onChange = [this] {
        if (const int id = morphGroup_.getSelectedId(); id > 0 && id - 1 != doc_.focusedMorphGroup())
            doc_.setFocusedMorphGroup(id - 1);
    };
    addAndMakeVisible(morphGroup_);
    colourButton_.setTooltip("Module colour: new modules get it; choosing one paints the selected modules");
    colourButton_.setColour(juce::TextButton::buttonColourId, ModulePainter::moduleColour(0));
    colourButton_.onClick = [this] { chooseModuleColour(); };
    bucketButton_.setTooltip("Paint the selected modules with the module colour");
    bucketButton_.onClick = [this] {
        if (auto* a = areaWithSelection())
            a->colourSelection(moduleColour_);
    };
    addAndMakeVisible(colourButton_);
    addAndMakeVisible(bucketButton_);
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
    // The original's Init 1 and Init 2, after New (manual p59): the init patch, into the active slot of a performance.
    for (auto [b, n] : {std::pair{&init1_, 1}, std::pair{&init2_, 2}}) {
        b->setTooltip("Init " + juce::String(n) + ": a new patch from Init Patch " + juce::String(n)
                      + " (File > Save InitPatch " + juce::String(n) + " makes it)");
        b->onClick = [this, n] { recallInitPatch(n); };
        addAndMakeVisible(b);
    }
    perfButton_.setTooltip("Perf: the G2's performance mode (lit when on). A click switches it; the G2 then plays its "
                           "performance, or its patch-mode slots again");
    perfButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3070c0));
    perfButton_.onClick = [this] {
        if (synth_ == nullptr || !synth_->ready())
            return;
        const bool on = !synth_->performanceMode();
        // CPerfButton::Click @000774fe: the opposite of the mode shown, and whether Shift is held.
        const bool shift = juce::ModifierKeys::currentModifiers.isShiftDown();
        const bool live = synth_->bound();
        auto doSwitch = [this, on, shift, live] {
            if (synth_ == nullptr || !synth_->ready() || synth_->performanceMode() == on)
                return;
            synth_->setPerformanceMode(on, shift);
            // A document live on the synth becomes what it now plays, once it is read again (PerfSwitch.h).
            if (live)
                setStatus(on ? "The G2 is in performance mode: opening the performance it plays"
                             : "The G2 is in patch mode: opening the patch it plays");
            else
                setStatus(on ? "The G2 is in performance mode: Synth > Get Performance to edit what it plays"
                             : "The G2 is in patch mode: Synth > Get Patch to edit one of its slots");
            updateSynthStatus();
        };
        // The document live on the synth is replaced: unsaved changes are asked about first, as for Get Patch.
        if (live)
            confirmDiscard(doSwitch);
        else
            doSwitch();
    };
    addChildComponent(perfButton_);
    init1_.setConnectedEdges(juce::Button::ConnectedOnRight);
    init2_.setConnectedEdges(juce::Button::ConnectedOnLeft);
    addChildComponent(live_);
    vaPort_.setScrollBarsShown(true, true);
    fxPort_.setScrollBarsShown(true, true);
    addAndMakeVisible(vaPane_);
    addAndMakeVisible(divider_);
    addAndMakeVisible(fxPane_);
    fxPane_.setText("FX AREA", "Monophonic: runs once, shared by all voices. Put effects here (reverb, chorus, delay...).",
                    "The FX area processes the mix of all voices once. Route voices here with the Fx/Bus outputs of "
                    "the voice area, e.g. 2-Out set to Fx.");
    divider_.setTooltip("Drag to share the window between the areas. Its buttons: only the FX Area, only the Voice "
                        "Area, back to before (keys F and V switch too)");

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
    // The Perf button, with a G2 to switch.
    const bool showPerf = synth_->ready();
    perfButton_.setToggleState(showPerf && synth_->performanceMode(), juce::dontSendNotification);
    if (perfButton_.isVisible() != showPerf) {
        perfButton_.setVisible(showPerf);
        resized();
    }
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
    const bool editorOnIt = synth_ != nullptr && synth_->kind() == SynthSync::Kind::Emulated;
    switch (liveShow(available, available && emulator_->emulatorRunning(), editorOnIt, otherSynth,
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
    // Live Editing (LiveEditing.h): stops it, or sends the document again to start it (round-6 hand tests).
    const auto live = liveEditingItem(ready, synth_->bound(), doc_.isPerformance(), synth_->kind() == Kind::Emulated,
                                      juce::jlimit(0, 3, synth_->lastSlot()));
    m.addItem(item(kUnbind, live.label, {}, live.enabled, live.ticked));
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
        // The original's New To, Open To and Save From (SMNU File 110, 107, 113).
        const auto target = slotTarget(doc_.isPerformance(), synth_ != nullptr && synth_->ready());
        juce::PopupMenu newTo, openTo, saveFrom;
        for (int s = 0; s < 4; ++s) {
            const auto label = "Slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + s));
            newTo.addItem(item(kNewToBase + s, label));
            openTo.addItem(item(kOpenToBase + s, label));
            saveFrom.addItem(item(kSaveFromBase + s, label));
        }
        m.addSubMenu("New To", newTo, target != SlotTarget::None);
        m.addSubMenu("Open To", openTo, target != SlotTarget::None);
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
        m.addSubMenu("Save From", saveFrom, target != SlotTarget::None);
        m.addItem(item(kSaveInitPatch1, "Save InitPatch 1")); // the original's File menu (SMNU, items 108 and 109)
        m.addItem(item(kSaveInitPatch2, "Save InitPatch 2"));
        m.addSeparator();
        m.addItem(item(kPerformanceSettings, "Performance Settings...", kCmd + "R", doc_.isPerformance()));
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
        m.addItem(item(kPasteParams, "Paste Params", kCmd + "E", selected && !AreaView::clipboard().empty()));
        m.addItem(item(kDelete, "Delete", "Delete", selected || va_.hasHighlightedCable() || fx_.hasHighlightedCable()));
        m.addItem(item(kSelectAll, "Select All", kCmd + "A"));
        m.addSeparator();
        // G2fresh's own: a selection as a patch file, and a patch file's modules into this patch.
        m.addItem(item(kSaveSelection, "Save Selection as Patch...", {}, selected));
        m.addItem(item(kInsertPatch, "Insert Patch File..."));
        m.addSeparator();
        juce::PopupMenu variations;
        for (int v = 0; v < g2::kFileVariations; ++v)
            variations.addItem(item(kCopyVariationBase + v, v < g2::kUserVariations ? "Variation " + juce::String(v + 1) : juce::String("Init"),
                                    {}, v != doc_.variation()));
        m.addSubMenu("Copy Variation " + juce::String(doc_.variation() + 1) + " To", variations);
        m.addItem(item(kRecallInit, "Recall Init into Variation " + juce::String(doc_.variation() + 1)));
        const juce::String target = selected ? "Selected Modules" : "Variation " + juce::String(doc_.variation() + 1);
        m.addItem(item(kRandomize, "Randomize " + target));
        m.addItem(item(kMutate, "Mutate " + target));
        m.addItem(item(kMutator, "Patch Mutator...", kCmd + "2"));
        m.addSeparator();
        m.addItem(item(kRename, doc_.isPerformance() ? "Rename Slot..." : "Rename Patch..."));
        m.addItem(item(kPatchNotes, "Patch Notes...", kCmd + "J"));
        m.addSeparator();
        m.addItem(item(kDeleteUnused, "Delete Unused Modules")); // the original's Patch menu (SMNU, item 305)
        m.addSeparator();
        // The original's Tools menu (SMNU 1086-1088).
        m.addItem(item(kAssignMidiSelection, "Assign MIDI to Selection", {}, selected));
        m.addItem(item(kDeassignMidiSelection, "Deassign MIDI from Selection", {}, selected));
        m.addItem(item(kDeassignAllMidi, "Deassign All MIDI"));
    } else if (index == 2) {
        m.addItem(item(kLiveMode, "Live (Emulated G2 Panel and Keyboard)", kCmd + "L", true, liveMode_));
        m.addSeparator();
        m.addItem(item(kZoomIn, "Zoom In", kCmd + "+", zoom_ < kMaxZoom));
        m.addItem(item(kZoomOut, "Zoom Out", kCmd + "-", zoom_ > kMinZoom));
        m.addItem(item(kZoomReset, "Actual Size", kCmd + "0"));
        m.addItem(item(kZoomFit, "Zoom to Fit", kShift + kCmd + "0"));
        m.addSeparator();
        m.addItem(item(kShowSettings, "Show Patch Settings", kCmd + "P", true, !settings_.isCollapsed()));
        juce::PopupMenu cables;
        static const char* const names[7] = {"Red", "Blue", "Yellow", "Orange", "Green", "Purple", "White"};
        for (int c = 0; c < 7; ++c)
            cables.addItem(item(kCablesBase + c, names[c], {}, true, doc_.patch().header.cablesVisible[static_cast<std::size_t>(c)]));
        m.addSubMenu("Show Cables", cables);
        // The original's H and S buttons and their keys (Space, Control+Space).
        m.addItem(item(kHideAllCables, "Hide All Cables", "Space", true, !doc_.cablesShown()));
        m.addItem(item(kShakeCables, "Shake Cables", "Ctrl+Space", isCurved(cableStyle())));
        juce::PopupMenu styles;
        static const char* const styleNames[4] = {"Straight 3D", "Curved 3D", "Straight Thin", "Curved Thin"};
        for (int s = 0; s < 4; ++s)
            styles.addItem(item(kCableStyleBase + s, styleNames[s], {}, true, static_cast<int>(cableStyle()) == s));
        m.addSubMenu("Cable Style", styles);
        juce::PopupMenu opacity;
        for (std::size_t i = 0; i < kCableOpacities.size(); ++i)
            opacity.addItem(item(kCableOpacityBase + static_cast<int>(i), juce::String(kCableOpacities[i]) + "%", {}, true,
                                 cableOpacity() == kCableOpacities[i]));
        m.addSubMenu("Cable Opacity", opacity);
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
        else if (id == kUnbind) {
            const auto live = liveEditingItem(synth_->ready(), synth_->bound(), doc_.isPerformance(),
                                              synth_->kind() == SynthSync::Kind::Emulated, juce::jlimit(0, 3, synth_->lastSlot()));
            switch (live.action) {
            case LiveEditingAction::Stop: synth_->unbind(); break;
            case LiveEditingAction::SendPatch: synth_->sendPatch(live.slot); break;
            case LiveEditingAction::SendPatchAlone: synth_->sendPatchAlone(); break;
            case LiveEditingAction::SendPerformance: synth_->sendPerformance(); break;
            case LiveEditingAction::None: break;
            }
        }
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
    if (id >= kCableOpacityBase && id < kCableOpacityBase + static_cast<int>(kCableOpacities.size())) {
        setCableOpacity(kCableOpacities[static_cast<std::size_t>(id - kCableOpacityBase)]);
        va_.settingsChanged();
        fx_.settingsChanged();
        menuItemsChanged();
        return;
    }
    if (id >= kCableStyleBase && id < kCableStyleBase + 4) {
        setCableStyle(cableStyleFromSetting(id - kCableStyleBase));
        va_.settingsChanged();
        fx_.settingsChanged();
        menuItemsChanged();
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
    if (isRecentFileItem(id)) {
        const auto f = recent_.getFile(id - kRecentBase);
        confirmDiscard([this, f] { openFile(f); });
        return;
    }
    switch (id) {
    case kNew: newPatch(); break;
    case kNewPerformance:
        confirmDiscard([this] {
            doc_.newPerformance();
            setZoom(1.0f);
        });
        break;
    case kPerformanceSettings: showPerformanceSettings(doc_, this); break;
    case kCut: copySelection(true); break;
    case kCopy: copySelection(false); break;
    case kPaste: pasteClipboard(); break;
    case kDuplicate:
        if (auto* a = areaWithSelection())
            a->duplicateSelection();
        break;
    case kSelectAll: activeArea().selectAll(); break;
    case kSaveSelection: saveSelection(); break;
    case kRecallInit: recallVariationInit(); break;
    case kPasteParams:
        if (auto* a = areaWithSelection())
            a->pasteParams(AreaView::clipboard());
        break;
    case kDeleteUnused: {
        const auto before = doc_.patch().va.modules.size() + doc_.patch().fx.modules.size();
        doc_.perform("Delete unused modules", [](g2::Patch& p) { g2::edit::deleteUnusedModules(p); });
        const auto gone = before - (doc_.patch().va.modules.size() + doc_.patch().fx.modules.size());
        setStatus(gone == 0 ? juce::String("No unused module") : juce::String(static_cast<int>(gone)) + (gone == 1 ? " unused module deleted" : " unused modules deleted"));
        break;
    }
    case kAssignMidiSelection:
    case kDeassignMidiSelection:
        if (auto* a = areaWithSelection()) {
            const auto loc = a->location();
            const auto modules = a->selection();
            doc_.perform(id == kAssignMidiSelection ? "Assign MIDI to selection" : "Deassign MIDI from selection",
                         [&](g2::Patch& p) {
                             id == kAssignMidiSelection ? g2::edit::autoAssignMidiCcs(p, loc, modules)
                                                        : g2::edit::deassignMidiCcs(p, loc, modules);
                         });
        }
        break;
    case kDeassignAllMidi:
        doc_.perform("Deassign all MIDI", [](g2::Patch& p) { g2::edit::deassignAllMidiCcs(p); });
        break;
    case kInsertPatch: insertPatchFile(); break;
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
    case kSaveInitPatch1: saveInitPatch(1); break;
    case kNewToBase: case kNewToBase + 1: case kNewToBase + 2: case kNewToBase + 3: newTo(id - kNewToBase); break;
    case kOpenToBase: case kOpenToBase + 1: case kOpenToBase + 2: case kOpenToBase + 3: openTo(id - kOpenToBase); break;
    case kSaveFromBase: case kSaveFromBase + 1: case kSaveFromBase + 2: case kSaveFromBase + 3: saveFrom(id - kSaveFromBase); break;
    case kSaveInitPatch2: saveInitPatch(2); break;
    case kClearRecent:
        recent_.clear();
        userSettings().setValue("recentFiles", recent_.toString());
        menuItemsChanged(); // the native menu bar shows the list it was given until told
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
    case kZoomFit: zoomToFit(); break;
    case kShowSettings: settings_.setCollapsed(!settings_.isCollapsed()); break;
    case kClassicLook: setLook(currentLook() == Look::Classic ? Look::Modern : Look::Classic); break;
    case kAnimateCables: setAnimation(!cableAnimation()); break;
    case kHideAllCables: doc_.toggleCablesShown(); break;
    case kShakeCables:
        va_.shakeCables();
        fx_.shakeCables();
        break;
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
              + " (modules excluded from mutation, and switches, are kept)");
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

void MainView::slotClicked(int slot)
{
    const auto m = juce::ModifierKeys::getCurrentModifiers();
#if JUCE_MAC
    const bool morph = m.isAltDown();
#else
    const bool morph = m.isCtrlDown();
#endif
    std::string refused;
    switch (slotClick(m.isShiftDown(), morph)) {
    case SlotClick::Focus:
        doc_.setSlot(slot);
        break;
    case SlotClick::ToggleActive:
        doc_.editPerformance([&](g2::file::PerfHeader& h) { refused = g2::edit::toggleSlotEnabled(h, slot); });
        if (const auto* perf = doc_.performance(); perf && perf->header.focusedSlot != doc_.slot())
            doc_.setSlot(perf->header.focusedSlot); // a slot turned on takes the focus, one turned off gives it
        break;
    case SlotClick::ToggleKeyboard:
        doc_.editPerformance([&](g2::file::PerfHeader& h) { refused = g2::edit::toggleSlotKeyboard(h, slot); });
        break;
    }
    if (!refused.empty())
        setStatus(juce::String(refused));
    updateToolbar();
}

void MainView::chooseModuleColour()
{
    juce::PopupMenu colours;
    for (int c : ModulePainter::moduleColourMenuOrder()) {
        juce::PopupMenu::Item item(c == 0 ? juce::String("Default") : juce::String("Colour ") + juce::String(c));
        item.itemID = 1 + c;
        item.colour = ModulePainter::moduleColour(c);
        item.isTicked = moduleColour_ == c;
        colours.addItem(std::move(item));
    }
    colours.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(colourButton_),
                          [safe = juce::Component::SafePointer<MainView>(this)](int r) {
                              if (!safe || r <= 0)
                                  return;
                              safe->moduleColour_ = static_cast<std::uint8_t>(r - 1);
                              safe->colourButton_.setColour(juce::TextButton::buttonColourId, ModulePainter::moduleColour(r - 1));
                              for (auto* area : {&safe->va_, &safe->fx_})
                                  area->setNewModuleColour(safe->moduleColour_);
                              if (auto* a = safe->areaWithSelection())
                                  a->colourSelection(safe->moduleColour_);
                          });
}

void MainView::recallVariationInit()
{
    const auto to = static_cast<std::uint8_t>(doc_.variation());
    doc_.perform("Recall Init", [&](g2::Patch& p) { g2::edit::recallVariationInit(p, to); });
    setStatus("Recalled the Init variation into variation " + juce::String(to + 1));
}

juce::File MainView::dialogFolder() const
{
    // The document's folder, else G2fresh/Patches in the Music folder (PatchLibraryModel.h), else Documents.
    const auto music = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getFullPathName().toStdString();
    const juce::File folder{juce::String(g2ui::dialogFolder(doc_.file().getFullPathName().toStdString(), defaultPatchFolder(music)))};
    return folder.isDirectory() ? folder : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

void MainView::newTo(int slot)
{
    const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + slot));
    if (doc_.isPerformance()) {
        // The slot's patch emptied, as one undo step, and focused.
        doc_.setSlot(slot);
        if (doc_.perform("New patch in slot " + letter, [](g2::Patch& p) { p = g2::Patch::makeDefault(); }))
            doc_.setName("No name");
        return;
    }
    if (synth_ == nullptr || !synth_->ready())
        return;
    confirmDiscard([this, slot] {
        doc_.newPatch();
        setZoom(1.0f);
        synth_->sendPatch(slot); // the slot cleared for it
    });
}

void MainView::openTo(int slot)
{
    chooser_ = std::make_unique<juce::FileChooser>("Open a G2 patch into slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + slot)),
                                                   dialogFolder(), "*.pch2");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, slot](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              if (!doc_.isPerformance()) {
                                  if (synth_ == nullptr || !synth_->ready())
                                      return;
                                  confirmDiscard([this, f, slot] {
                                      loadFile(f);
                                      if (!doc_.isPerformance())
                                          synth_->sendPatch(slot);
                                  });
                                  return;
                              }
                              // Into the performance's slot, as one undo step (as Open in Slot).
                              juce::MemoryBlock mb;
                              g2::Patch patch;
                              try {
                                  if (!f.loadFileAsData(mb))
                                      throw std::runtime_error("cannot read the file");
                                  const auto* data = static_cast<const std::uint8_t*>(mb.getData());
                                  auto loaded = g2::load(std::vector<std::uint8_t>(data, data + mb.getSize()), {});
                                  auto* p = std::get_if<g2::Patch>(&loaded.content);
                                  if (p == nullptr)
                                      throw std::runtime_error("not a patch");
                                  patch = std::move(*p);
                              } catch (const std::exception& e) {
                                  setStatus(f.getFileName() + ": " + e.what());
                                  return;
                              }
                              doc_.setSlot(slot);
                              if (doc_.perform("Open in slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + slot)),
                                               [&patch](g2::Patch& p) { p = patch; }))
                                  doc_.setName(f.getFileNameWithoutExtension());
                          });
}

void MainView::saveFrom(int slot)
{
    g2::Patch patch;
    juce::String name;
    if (const auto* perf = doc_.performance()) {
        patch = doc_.slotPatch(slot);
        name = PatchDocument::fromG2Bytes(perf->header.slots[static_cast<std::size_t>(slot)].patchName);
    } else if (const auto onSynth = synth_ != nullptr ? synth_->synthSlotPatch(slot) : std::nullopt) {
        patch = onSynth->first;
        name = juce::String(onSynth->second);
    } else {
        return;
    }
    const auto fileName = juce::String(g2::edit::fileNameForPatch(name.toStdString())).trim();
    const auto folder = dialogFolder();
    chooser_ = std::make_unique<juce::FileChooser>("Save the patch in slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + slot)),
                                                   folder.getChildFile((fileName.isEmpty() ? juce::String("No name") : fileName) + ".pch2"), "*.pch2");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, patch](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              f = f.hasFileExtension(".pch2") ? f : f.withFileExtension(".pch2");
                              const auto bytes = g2::savePatch(patch);
                              setStatus(f.replaceWithData(bytes.data(), bytes.size()) ? "Saved " + f.getFileName()
                                                                                       : "Could not write " + f.getFullPathName());
                          });
}

juce::File MainView::initPatchFile(int n)
{
    return userSettings().getFile().getParentDirectory().getChildFile(initPatchFileName(n));
}

void MainView::saveInitPatch(int n)
{
    // CEditorApp::HandleDefaultPatchSave @00149e1c: the top patch (in a performance, the focused slot's), as it is.
    const auto f = initPatchFile(n);
    const auto bytes = g2::savePatch(doc_.patch());
    if (f.getParentDirectory().createDirectory() && f.replaceWithData(bytes.data(), bytes.size()))
        setStatus("Saved \"" + doc_.name() + "\" as Init Patch " + juce::String(n));
    else
        setStatus("Could not write " + f.getFullPathName());
}

void MainView::recallInitPatch(int n)
{
    // CEditorApp::HandleDefaultPatchOpen @0014a5c8: nothing without the file; else, after asking about unsaved
    // changes, the file as a new patch (if connected, into the focused slot). The file stays the template: Save asks
    // where, rather than overwrite it.
    const auto f = initPatchFile(n);
    juce::MemoryBlock mb;
    if (!f.existsAsFile() || !f.loadFileAsData(mb)) {
        setStatus("No Init Patch " + juce::String(n) + " yet: File > Save InitPatch " + juce::String(n) + " makes one");
        return;
    }
    const auto* data = static_cast<const std::uint8_t*>(mb.getData());
    const std::vector<std::uint8_t> bytes(data, data + mb.getSize());
    const auto name = f.getFileNameWithoutExtension(); // a patch file's name is its file name
    if (doc_.isPerformance()) {
        // Into the focused slot, as one undo step (as Open in Slot).
        g2::Patch patch;
        try {
            auto loaded = g2::load(bytes, {});
            auto* p = std::get_if<g2::Patch>(&loaded.content);
            if (p == nullptr)
                throw std::runtime_error("not a patch");
            patch = std::move(*p);
        } catch (const std::exception& e) {
            setStatus(f.getFileName() + ": " + e.what());
            return;
        }
        if (doc_.perform("Init " + juce::String(n), [&patch](g2::Patch& p) { p = patch; })) {
            doc_.setName(name);
            setStatus("Init Patch " + juce::String(n) + " is in slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + doc_.slot())));
        }
        return;
    }
    confirmDiscard([this, bytes, name, n] {
        try {
            doc_.loadBytes(bytes);
        } catch (const std::exception& e) {
            setStatus("Init Patch " + juce::String(n) + ": " + e.what());
            return;
        }
        doc_.setFile({});
        doc_.setName(name);
        doc_.markSaved(); // just recalled: nothing to lose
        setStatus("New patch from Init Patch " + juce::String(n));
        updateToolbar();
        setZoom(1.0f); // as Open
    });
}

void MainView::zoomToFit()
{
    // G2fresh's own (ZoomFit.h): the largest zoom at which both areas' modules fit their panes, from their tops.
    std::vector<FitArea> areas;
    for (auto [view, port] : {std::pair<AreaView*, juce::Viewport*>{&va_, &vaPort_}, {&fx_, &fxPort_}}) {
        const auto extent = view->contentExtent();
        areas.push_back({extent.x, extent.y, port->getViewWidth(), port->getViewHeight()});
    }
    setZoom(fitZoom(areas, kMinZoom, kMaxZoom, 12));
    vaPort_.setViewPosition(0, 0);
    fxPort_.setViewPosition(0, 0);
}

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
        settings_.synthChanged(); // the voices the synth plays
        updateLoad();
        menuItemsChanged(); // slot names, connection state
        return;
    }
    updateToolbar();
    menuItemsChanged(); // undo/redo state
}

int MainView::splitTotal() const
{
    return fxPane_.getBottom() - vaPane_.getY();
}

void MainView::splitMoved()
{
    resized();
    const auto pos = static_cast<std::uint16_t>(split_.position);
    if (doc_.patch().header.splitterPos != pos)
        doc_.performLive([pos](g2::Patch& p) { p.header.splitterPos = pos; });
}

void MainView::Divider::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    g.fillAll(juce::Colour(0xff25272b));
    g.setColour(juce::Colour(0xff3a3d44));
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(r.getWidth()));
    g.drawHorizontalLine(r.getBottom() - 1, 0.0f, static_cast<float>(r.getWidth()));
    // The three buttons at the left: the bar up (only the FX Area), down (only the Voice Area), back.
    g.setColour(juce::Colour(0xffa8adb6));
    for (int b = 0; b < 3; ++b) {
        const auto box = juce::Rectangle<float>(static_cast<float>(b * SplitBar::kHeight), 0.0f, SplitBar::kHeight, SplitBar::kHeight).reduced(2.5f);
        juce::Path p;
        if (b == 0)
            p.addTriangle(box.getCentreX(), box.getY(), box.getRight(), box.getBottom(), box.getX(), box.getBottom());
        else if (b == 1)
            p.addTriangle(box.getX(), box.getY(), box.getRight(), box.getY(), box.getCentreX(), box.getBottom());
        else {
            p.addTriangle(box.getCentreX(), box.getY(), box.getRight(), box.getCentreY() - 0.5f, box.getX(), box.getCentreY() - 0.5f);
            p.addTriangle(box.getX(), box.getCentreY() + 0.5f, box.getRight(), box.getCentreY() + 0.5f, box.getCentreX(), box.getBottom());
        }
        g.fillPath(p);
    }
}

void MainView::Divider::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(buttonAt(e.x) >= 0 ? juce::MouseCursor::NormalCursor : juce::MouseCursor::UpDownResizeCursor);
}

void MainView::Divider::mouseDown(const juce::MouseEvent& e)
{
    const int total = owner_.splitTotal();
    switch (buttonAt(e.x)) {
    case 0: owner_.split_.toFx(total); break;
    case 1: owner_.split_.toVoice(total); break;
    case 2: owner_.split_.restore(); break;
    default:
        dragging_ = true;
        grab_ = e.y;
        owner_.split_.beginDrag(total);
        return;
    }
    owner_.splitMoved();
}

void MainView::Divider::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_)
        return;
    owner_.split_.dragTo(e.getEventRelativeTo(&owner_).y - grab_ - owner_.vaPane_.getY(), owner_.splitTotal());
    owner_.resized();
}

void MainView::Divider::mouseUp(const juce::MouseEvent&)
{
    if (std::exchange(dragging_, false))
        owner_.splitMoved();
}

bool MainView::Divider::abortDrag()
{
    if (!std::exchange(dragging_, false))
        return false;
    owner_.splitMoved();
    return true;
}

void MainView::updateToolbar()
{
    // The split bar where this patch has it (each slot's patch its own).
    if (const int pos = doc_.patch().header.splitterPos; pos != split_.position) {
        split_.position = pos;
        resized();
    }
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
            b->setTooltip("Slot " + b->getButtonText() + (name.isNotEmpty() ? ": " + name : juce::String())
                          + ". Click: edit it; Shift-click: on or off; " + kMorphKeyName + "-click: on the keyboard or not");
        }
        b->setToggleState(i == doc_.slot(), juce::dontSendNotification);
        // As the original's slot buttons (manual p58): active slots blue, a red line on top of those the keyboard plays.
        const auto& s = perf ? perf->header.slots[static_cast<std::size_t>(i)] : g2::file::SlotSettings{};
        b->setColour(juce::TextButton::buttonColourId, perf && s.enabled ? juce::Colour(0xff2a4466) : juce::Colour(0xff3a3d44));
        keyboardMarks_[i]->setVisible(perf != nullptr && s.keyboard != 0);
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
    // The morph groups, by number and name (the names can be changed on the morph dials).
    for (int g = 0; g < g2::kMorphGroups; ++g) {
        const auto text = "Morph " + juce::String(g + 1) + ": " + juce::String(g2::edit::morphLabel(doc_.patch(), static_cast<std::uint8_t>(g)));
        if (morphGroup_.getNumItems() <= g)
            morphGroup_.addItem(text, g + 1);
        else if (morphGroup_.getItemText(g) != text)
            morphGroup_.changeItemText(g + 1, text);
    }
    morphGroup_.setSelectedId(doc_.focusedMorphGroup() + 1, juce::dontSendNotification);
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
    // The toolbar: the document's items on the left of the first row, the groups on the right (ToolbarLayout.h), on
    // as many rows as the width needs.
    constexpr int kRowHeight = 26, kRowStep = 30;
    const auto top = r.reduced(6, 4).withHeight(kRowHeight);
    auto bar = top;
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
    bar.removeFromLeft(10);
    init1_.setBounds(bar.removeFromLeft(48));
    init2_.setBounds(bar.removeFromLeft(48));

    // Each group's items from left to right: a component and its width, or a space (nullptr).
    using Items = std::vector<std::pair<juce::Component*, int>>;
    std::vector<Items> groups;
    std::vector<ToolbarGroup> sizes;
    auto add = [&](Items items, int gapBefore) {
        int width = 0;
        for (const auto& item : items)
            width += item.second;
        sizes.push_back({width, gapBefore});
        groups.push_back(std::move(items));
    };
    {
        Items v{{&variationLabel_, textWidth(variationLabel_)}};
        for (auto* b : variations_)
            v.push_back({nullptr, 2}), v.push_back({b, 28});
        v.push_back({nullptr, 4});
        v.push_back({&varInit_, 40});
        add(std::move(v), 0);
    }
    add({{&colourButton_, 28}, {nullptr, 2}, {&bucketButton_, 44}}, 16);
    add({{&morphGroup_, 150}}, 8);
    add({{&randomizeButton_, 92}, {nullptr, 4}, {&mutatorButton_, 92}}, 4);
    if (perfButton_.isVisible())
        add({{&perfButton_, 44}}, 16);
    if (doc_.isPerformance()) {
        Items s;
        for (auto* b : slots_)
            s.push_back({nullptr, 2}), s.push_back({b, 28});
        add(std::move(s), 16);
    }
    add({{&zoomOut_, 28}, {&zoomReset_, 54}, {&zoomIn_, 28}}, 16);

    const auto layout = layoutToolbar(top.getWidth(), bar.getX() - top.getX(), sizes);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        auto x = top.getX() + layout.groups[g].x;
        const auto y = top.getY() + layout.groups[g].row * kRowStep;
        for (const auto& [component, width] : groups[g]) {
            if (component != nullptr)
                component->setBounds(x, y, width, kRowHeight);
            x += width;
        }
    }
    if (doc_.isPerformance())
        for (int i = 0; i < slots_.size(); ++i)
            keyboardMarks_[i]->setBounds(slots_[i]->getBounds().withHeight(3).reduced(3, 0));
    r.removeFromTop(34 + (layout.rows - 1) * kRowStep);

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
    vaPane_.setBounds(r.removeFromTop(SplitBar::shown(split_.position, r.getHeight())));
    divider_.setBounds(r.removeFromTop(SplitBar::kHeight));
    fxPane_.setBounds(r);
    for (auto [port, area] : {std::pair{&vaPort_, &va_}, std::pair{&fxPort_, &fx_}})
        area->setMinimumSize(juce::roundToInt(static_cast<float>(port->getWidth()) / zoom_),
                             juce::roundToInt(static_cast<float>(port->getHeight()) / zoom_));
}

bool MainView::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    const bool repeat = heldKey_ == code;
    heldKey_ = code;
    // The module bar first, as the original hands every key to its toolbar first (CPatchView::HandleKeyDownUp
    // @000f93d4 -> CTBWindow::RouteKeySpec).
    if (!liveMode_ && browser_.routeKey(key, repeat))
        return true;
    // F5-F8 show the hint boxes while held (CPatchView::KeyHintView*; KeyBringDownHintViews on release).
    if (!liveMode_ && code >= juce::KeyPress::F5Key && code <= juce::KeyPress::F8Key && !key.getModifiers().isAnyModifierKeyDown()) {
        const auto mode = hintModeForKey(5 + (code - juce::KeyPress::F5Key));
        va_.setHintMode(mode);
        fx_.setHintMode(mode);
        return true;
    }
    // Esc aborts every drag (|, not ||), as CPatchView::KeyAbortDragOperations @000f7cde; an area's own Esc comes to it
    // first.
    if (code == juce::KeyPress::escapeKey && !liveMode_ && (divider_.abortDrag() | va_.abortDrag() | fx_.abortDrag()))
        return true;
    if (code == juce::KeyPress::spaceKey && !liveMode_ && !repeat) {
        const auto m = key.getModifiers();
#if JUCE_MAC
        const SpaceMods mods{m.isCtrlDown(), m.isShiftDown(), m.isAltDown(), m.isCommandDown()};
#else
        const SpaceMods mods{m.isCtrlDown(), m.isShiftDown(), m.isAltDown(), false};
#endif
        switch (spaceKey(mods)) {
        case SpaceAction::HideAll:
            doc_.toggleCablesShown();
            setStatus(doc_.cablesShown() ? "Cables shown" : "All cables hidden (Space shows them)");
            return true;
        case SpaceAction::Shake:
            va_.shakeCables();
            fx_.shakeCables();
            return true;
        case SpaceAction::None:
            break;
        }
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey
        || code == juce::KeyPress::rightKey) {
        const auto arrow = code == juce::KeyPress::upKey     ? Arrow::Up
                         : code == juce::KeyPress::downKey   ? Arrow::Down
                         : code == juce::KeyPress::leftKey   ? Arrow::Left
                                                             : Arrow::Right;
        const auto m = key.getModifiers();
#if JUCE_MAC
        const FocusMods mods{m.isShiftDown(), m.isAltDown(), m.isCommandDown() || m.isCtrlDown()};
#else
        const FocusMods mods{m.isShiftDown(), m.isCtrlDown(), m.isAltDown()}; // the manual's Ctrl (p115)
#endif
        heldArrow_ = code;
        return focusKeyPressed(focusKey(arrow, mods, repeat, mutator_ && mutator_->isVisible()));
    }
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
    // Find a module (an addition; the original's Command+F is Parameter Pages, so Shift is added).
    if (key == juce::KeyPress('f', cmd | juce::ModifierKeys::shiftModifier, 0) && !liveMode_)
        return browser_.focusSearch(), true;
    if (key == juce::KeyPress('c', cmd, 0))
        return copySelection(false), true;
    if (key == juce::KeyPress('x', cmd, 0))
        return copySelection(true), true;
    if (key == juce::KeyPress('v', cmd, 0))
        return pasteClipboard(), true;
    if (key == juce::KeyPress('e', cmd, 0)) { // Paste Params (the original's Cmd-E)
        if (auto* a = areaWithSelection())
            a->pasteParams(AreaView::clipboard());
        return true;
    }
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
    if (key == juce::KeyPress('0', cmd | juce::ModifierKeys::shiftModifier, 0) || key == juce::KeyPress(')', cmd | juce::ModifierKeys::shiftModifier, 0))
        return zoomToFit(), true;
    // The original's Ctrl (Mac: Command) shortcuts G2fresh has the functions for (manual p116).
    if (key.getModifiers().isCommandDown() && !key.getModifiers().isAltDown() && !liveMode_) {
        const int k = key.getKeyCode();
        switch (commandKey(k > 0 && k < 128 ? static_cast<char>(k) : 0, key.getModifiers().isShiftDown())) {
        case CommandKey::PatchSettings:
            settings_.setCollapsed(!settings_.isCollapsed());
            return true;
        case CommandKey::PerformanceSettings:
            if (doc_.isPerformance())
                showPerformanceSettings(doc_, this);
            return true;
        case CommandKey::Textpad:
            showPatchNotes(doc_, this);
            return true;
        case CommandKey::UploadSlot:
            if (const int slot = synth_ != nullptr ? synth_->synthFocusedSlot() : -1; slot >= 0)
                confirmDiscard([this, slot] { synth_->getPatch(slot); });
            else
                setStatus("Get Patch: no G2 connected");
            return true;
        case CommandKey::None:
            break;
        }
    }
    // ⌘2 is the original's Patch Mutator (⌘1 Dump One and ⌘3 Patch Adjuster are not in G2fresh).
    if (key == juce::KeyPress('2', cmd, 0))
        return showMutator(), true;
    // 1 to 8 select the variation, A to D a performance's slot (CPatchView::InitializeShortcutKeys @000fafd8).
    if (!liveMode_) {
        const auto c = key.getTextCharacter();
        const auto m = key.getModifiers();
        // The Mutator's keys, while it is open.
        if (mutator_ && mutator_->isVisible())
            if (const auto mk = mutatorKey(c > 0 && c < 128 ? static_cast<char>(c) : 0,
                                           m.isCommandDown() || m.isCtrlDown() || m.isAltDown() || m.isShiftDown(), repeat);
                mk != MutatorKey::None) {
                mutator_->keyAction(mk);
                return true;
            }
        std::array<bool, 4> enabled{true, true, true, true};
        if (const auto* perf = doc_.performance())
            for (std::size_t s = 0; s < 4; ++s)
                enabled[s] = perf->header.slots[s].enabled != 0;
        const auto k = patchKey(c > 0 && c < 128 ? static_cast<char>(c) : 0,
                                m.isCommandDown() || m.isCtrlDown() || m.isAltDown() || m.isShiftDown(), repeat,
                                doc_.isPerformance(), enabled);
        if (k.kind == PatchKey::Kind::Variation)
            return doc_.setVariation(k.index), true;
        if (k.kind == PatchKey::Kind::Slot)
            return doc_.setSlot(k.index), true;
        if (k.kind == PatchKey::Kind::ToggleClock) {
            doc_.editPerformance([](g2::file::PerfHeader& h) { h.masterClockRun = h.masterClockRun != 0 ? 0 : 1; });
            if (const auto* perf = doc_.performance())
                setStatus(perf->header.masterClockRun != 0 ? "Master clock running" : "Master clock stopped");
            return true;
        }
        if (k.kind == PatchKey::Kind::AssignMidi) {
            const auto f = doc_.focus();
            if (g2::edit::canAssignMidiCc(doc_.patch(), f.location, f.module, f.param))
                showMidiCcDialog(doc_, f.location, f.module, f.param, this);
            return true;
        }
        if (k.kind == PatchKey::Kind::LearnMidi) {
            // The focused parameter takes the last CC the synth received, unless it holds a pre-assigned one
            // (CPatchView::KeyLearnMidi @000f99fe).
            const auto cc = synth_ != nullptr ? synth_->lastMidiCc() : std::nullopt;
            const auto f = doc_.focus();
            if (!cc) {
                setStatus("No MIDI CC received: move a controller on the synth's MIDI input first");
                return true;
            }
            juce::String error;
            if (doc_.perform("Learn MIDI controller", [&](g2::Patch& p) {
                    g2::edit::assignMidiCc(p, *cc, f.location, f.module, f.param);
                }, &error))
                setStatus("CC " + juce::String(*cc) + " assigned to the focused parameter");
            else
                setStatus("Cannot assign CC " + juce::String(*cc) + ": " + error);
            return true;
        }
        if (k.kind == PatchKey::Kind::ShowVoice || k.kind == PatchKey::Kind::ShowFx) {
            k.kind == PatchKey::Kind::ShowVoice ? split_.showVoice(splitTotal()) : split_.showFx(splitTotal());
            return splitMoved(), true;
        }
    }
    return false;
}

bool MainView::keyStateChanged(bool)
{
    if (!juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::F5Key) && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::F6Key)
        && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::F7Key) && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::F8Key)) {
        va_.setHintMode(HintMode::None);
        fx_.setHintMode(HintMode::None);
    }
    if (heldKey_ != 0 && !juce::KeyPress::isKeyCurrentlyDown(heldKey_))
        heldKey_ = 0;
    if (heldArrow_ != 0 && !juce::KeyPress::isKeyCurrentlyDown(heldArrow_)) {
        heldArrow_ = 0;
        // Releasing the key releases a push button Up pressed (CPatchView::KeyRelParamValue @000f8474 ->
        // CPanel::CtrlRelease @000c0794).
        if (const auto f = std::exchange(heldButton_, std::nullopt)) {
            const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
            doc_.performMomentary([f = *f, variation](g2::Patch& p) {
                g2::edit::setParam(p, f.location, f.module, f.param, variation, 0);
            });
        }
    }
    return false;
}

bool MainView::focusKeyPressed(FocusKey key)
{
    using Action = FocusKey::Action;
    const auto f = doc_.focus();
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    const auto name = [&] { return juce::String(f.location == g2::Location::Settings ? 0 : static_cast<int>(f.location)) + ":"
                                   + juce::String(f.module) + ":" + juce::String(f.param); };
    switch (key.action) {
    case Action::None:
        return false;
    case Action::StepValue:
        if (f.location != g2::Location::Settings && g2::edit::isMomentary(doc_.patch(), f.location, f.module, f.param)) {
            // A push button: Up presses it until the key is released; Down, at 0, does nothing.
            if (key.delta > 0 && !heldButton_) {
                heldButton_ = f;
                doc_.performMomentary([f, variation](g2::Patch& p) {
                    g2::edit::setParam(p, f.location, f.module, f.param, variation, 1);
                });
            }
            return true;
        }
        // The original records no undo step for the arrow keys; G2fresh makes the steps on one parameter one undo
        // step (a deliberate deviation, README).
        doc_.performCoalesced("Key value " + name(), [&](g2::Patch& p) {
            if (!g2::focus::stepValue(p, f, variation, key.delta))
                throw std::invalid_argument("at the end of its range");
        });
        break;
    case Action::NextParam:
        doc_.setFocus(key.delta > 0 ? g2::focus::nextParam(doc_.patch(), f) : g2::focus::previousParam(doc_.patch(), f));
        break;
    case Action::NavModule:
        doc_.setFocus(g2::focus::navModule(doc_.patch(), f, key.direction));
        break;
    case Action::StepMorph: {
        const auto group = static_cast<std::uint8_t>(doc_.focusedMorphGroup());
        doc_.performCoalesced("Key morph " + name(), [&](g2::Patch& p) {
            if (!g2::focus::stepMorph(p, f, variation, group, key.delta))
                throw std::invalid_argument("no morph step");
        });
        break;
    }
    }
    showFocus();
    return true;
}

void MainView::showFocus()
{
    const auto f = doc_.focus();
    if (f.location != g2::Location::Settings) {
        auto& area = f.location == g2::Location::Va ? va_ : fx_;
        if (!area.selection().empty() && area.selection() == std::vector<std::uint8_t>{f.module})
            return showFocusStatus();
        (&area == &va_ ? fx_ : va_).clearSelection(); // one selection for the patch
        area.selectFocused(f.module);
    }
    showFocusStatus();
}

void MainView::showFocusStatus()
{
    const auto f = doc_.focus();
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    setStatus(juce::String(g2::edit::targetName(doc_.patch(), f)) + ": "
              + juce::String(g2::edit::targetText(doc_.patch(), f, variation)));
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
            [this](const juce::File& f) {
                openFile(f);
                // Once a patch is opened from it, the library closes (it stays open while you audition), after the
                // open has finished.
                if (doc_.file() == f)
                    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PatchLibrary>(library_.get())] {
                        if (safe != nullptr && safe->isVisible())
                            safe->closeButtonPressed();
                    });
            }});
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
    confirmDiscard([this] {
        doc_.newPatch();
        setZoom(1.0f); // a new document starts at 100%
    });
}

void MainView::open()
{
    confirmDiscard([this] {
        chooser_ = std::make_unique<juce::FileChooser>("Open a G2 patch or performance", doc_.file() != juce::File() ? doc_.file() : dialogFolder(), kOpenPattern);
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
        setZoom(1.0f); // an opened document starts at 100%
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
    const auto folder = dialogFolder();
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

void MainView::saveSelection()
{
    auto* area = areaWithSelection();
    if (!area)
        return;
    const auto part = g2::edit::selectionPatch(doc_.patch(), area->location(), area->selection());
    const auto folder = dialogFolder();
    chooser_ = std::make_unique<juce::FileChooser>("Save the selection as a G2 patch", folder.getChildFile("Selection.pch2"),
                                                   "*.pch2");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, part](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              f = f.hasFileExtension(".pch2") ? f : f.withFileExtension(".pch2");
                              const auto bytes = g2::savePatch(part);
                              if (f.replaceWithData(bytes.data(), bytes.size()))
                                  setStatus("Saved " + juce::String(part.va.modules.size() + part.fx.modules.size())
                                            + " modules to " + f.getFileName());
                              else
                                  setStatus("Could not write " + f.getFullPathName());
                          });
}

void MainView::insertPatchFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Insert the modules of a G2 patch", dialogFolder(), "*.pch2");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              juce::MemoryBlock mb;
                              if (!f.loadFileAsData(mb)) {
                                  setStatus("Could not read " + f.getFullPathName());
                                  return;
                              }
                              g2::Patch file;
                              try {
                                  const auto* data = static_cast<const std::uint8_t*>(mb.getData());
                                  auto loaded = g2::load(std::span<const std::uint8_t>(data, mb.getSize()));
                                  if (loaded.isPerformance()) {
                                      setStatus("A performance's modules cannot be inserted: open one of its patches");
                                      return;
                                  }
                                  file = std::get<g2::Patch>(std::move(loaded.content));
                              } catch (const std::exception& e) {
                                  setStatus(f.getFileName() + ": " + e.what());
                                  return;
                              }
                              juce::String error;
                              if (doc_.perform("Insert " + f.getFileNameWithoutExtension(),
                                               [&](g2::Patch& p) { g2::edit::insertPatch(p, file); }, &error))
                                  setStatus("Inserted the modules of " + f.getFileName());
                              else
                                  setStatus("Cannot insert " + f.getFileName() + ": " + error);
                          });
}

} // namespace g2ui
