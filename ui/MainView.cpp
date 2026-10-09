#include "MainView.h"

#include "BankBrowser.h"

#include "g2/proto/logging_transport.hpp"
#include "CableLayout.h"

#include "Dialogs.h"

#include "g2/patch_load.hpp"

namespace g2ui {
namespace {

const juce::String kOpenPattern = "*.pch2;*.prf2";

enum MenuId {
    kNew = 1, kOpen, kSave, kSaveAs, kClearRecent, kNewPerformance, kPerformanceSettings,
    kUndo, kRedo, kDelete, kRename, kCut, kCopy, kPaste, kDuplicate, kSelectAll, kPatchNotes,
    kRandomize, kMutate, kMutator,
    kZoomIn, kZoomOut, kZoomReset, kShowSettings, kClassicLook, kAnimateCables,
    kAudioSettings, kRemoveBendPoints,
    kRecentBase = 100,       // + index into the recent files list
    kCopyVariationBase = 200, // + target variation (8 = init)
    kCablesBase = 300,        // + cable colour
    kMidiOutOff = 400, kMidiOutBase = 401, // + index into the device list
    kMidiChannelBase = 600,   // + channel (0: as played)
    kSynthG2 = 700, kSynthVirtual, kSynthDisconnect, kSendPerformance, kGetPerformance, kUnbind, kSynthMemory, kShowUsbLog,
    kSendPatchBase = 710,     // + slot
    kGetPatchBase = 720,      // + slot
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
        if (auto* editor = name_.getCurrentTextEditor())
            editor->setInputRestrictions(PatchDocument::kMaxNameLength);
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
    load_.setTooltip("Patch load estimated from the original editor's module tables (cycles and the fullest "
                     "memory, per area; the voice area for one voice). The synth reports the real figures.");
    addAndMakeVisible(load_);
    synthStatus_.setFont(theme::font());
    synthStatus_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff26282c));
    synthStatus_.setTooltip("The connection to the G2 (Synth menu). \"live\": edits go to the synth as you make them.");
    addChildComponent(synthStatus_);

    settings_.onStatus = [this](const juce::String& s) { setStatus(s); };
    settings_.onLayoutChanged = [this] { resized(); };
    addAndMakeVisible(settings_);
    browser_.onAdd = [this](std::uint8_t type) { va_.addModule(type); };
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
    if (synth_ != nullptr)
        synth_->removeChangeListener(this);
}

void MainView::setSynth(SynthSync* synth)
{
    if (synth_ != nullptr)
        synth_->removeChangeListener(this);
    synth_ = synth;
    if (synth_ != nullptr)
        synth_->addChangeListener(this);
    synthStatus_.setVisible(synth_ != nullptr);
    va_.setLiveLeds(synth_);
    fx_.setLiveLeds(synth_);
    updateSynthStatus();
    menuItemsChanged();
    resized();
}

void MainView::updateLoad()
{
    // What the synth reports while the patch is live on it, else the estimate.
    const auto reported = synth_ != nullptr ? synth_->reportedLoad() : std::nullopt;
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

void MainView::updateSynthStatus()
{
    if (synth_ == nullptr)
        return;
    juce::String text = synth_->statusText();
    if (synth_->boundSlot() >= 0)
        text << "  |  live: slot " << juce::String::charToString(static_cast<juce::juce_wchar>('A' + synth_->boundSlot()));
    else if (synth_->performanceBound())
        text << "  |  live: performance";
    synthStatus_.setText(text, juce::dontSendNotification);
    const auto colour = synth_->ready() ? (synth_->bound() ? juce::Colour(0xff6fd06f) : juce::Colour(0xffd0d0d0))
                                        : juce::Colour(0xff9a9ea6);
    synthStatus_.setColour(juce::Label::textColourId, colour);
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
    m.addItem(item(kSynthDisconnect, "Disconnect", {}, synth_->kind() != Kind::None));
    m.addSeparator();
    const bool ready = synth_->ready();
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
        m.addItem(item(kDelete, "Delete", "Delete", selected));
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
    } else if (name == "Options") {
        if (onAudioSettings)
            m.addItem(item(kAudioSettings, "Audio/MIDI Settings..."));
        if (midiOut_ != nullptr) {
            // The G2's USB connection carries no notes: MIDI goes to the
            // synth through a MIDI interface.
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
            m.addSubMenu("MIDI Output to G2", out);
        }
    }
    return m;
}

void MainView::menuItemSelected(int id, int)
{
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
        setStatus(midiOut_->deviceIdentifier().isEmpty() ? juce::String("MIDI output to the G2: off")
                                                         : "MIDI output to the G2: " + devices[i].name);
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
    case kSave: save(false); break;
    case kSaveAs: save(true); break;
    case kClearRecent:
        recent_.clear();
        userSettings().setValue("recentFiles", recent_.toString());
        break;
    case kUndo: doc_.undo(); break;
    case kRedo: doc_.redo(); break;
    case kDelete:
        if (auto* a = areaWithSelection())
            a->deleteSelected();
        break;
    case kRename: name_.showEditor(); break;
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
        updateLoad();
        menuItemsChanged(); // slot names, connection state
        return;
    }
    updateToolbar();
    menuItemsChanged(); // undo/redo state
}

void MainView::updateToolbar()
{
    if (auto* b = variations_[doc_.variation()])
        b->setToggleState(true, juce::dontSendNotification);
    const auto* perf = doc_.performance();
    for (int i = 0; i < slots_.size(); ++i) {
        auto* b = slots_[i];
        b->setVisible(perf != nullptr);
        if (perf) {
            const auto name = juce::String(perf->header.slots[static_cast<std::size_t>(i)].patchName);
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

    settings_.setBounds(r.removeFromTop(settings_.preferredHeight(r.getWidth())));
    browser_.setBounds(r.removeFromTop(62));
    {
        auto bottom = r.removeFromBottom(24);
        load_.setBounds(bottom.removeFromRight(std::min(560, bottom.getWidth() / 2)));
        if (synth_ != nullptr)
            synthStatus_.setBounds(bottom.removeFromRight(std::min(300, bottom.getWidth() / 2)));
        status_.setBounds(bottom);
    }
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
    if (key == juce::KeyPress('-', cmd, 0))
        return setZoom(zoom_ / 1.25f), true;
    if (key == juce::KeyPress('0', cmd, 0))
        return setZoom(1.0f), true;
    for (int i = 0; i < g2::kUserVariations; ++i)
        if (key == juce::KeyPress('1' + i, cmd, 0))
            return doc_.setVariation(i), true;
    return false;
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
        setStatus("Opened " + f.getFileName() + (ignoreChecksum ? " (checksum ignored)" : ""));
        updateToolbar();
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

void MainView::save(bool saveAs)
{
    auto write = [this](const juce::File& f) {
        std::vector<std::uint8_t> bytes;
        try {
            bytes = doc_.saveBytes();
        } catch (const std::exception& e) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot save patch",
                                                   e.what(), {}, this);
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
            doc_.markSaved();
            recent_.addFile(f);
            userSettings().setValue("recentFiles", recent_.toString());
            setStatus("Saved " + f.getFileName());
        } else {
            setStatus("Cannot write " + f.getFullPathName());
        }
    };
    // A patch saved under a new name goes to a new file.
    const bool renamed = !doc_.isPerformance() && doc_.file() != juce::File()
                         && doc_.file().getFileNameWithoutExtension() != doc_.name();
    if (!saveAs && !renamed && doc_.file() != juce::File()) {
        write(doc_.file());
        return;
    }
    const auto ext = doc_.fileExtension();
    const auto folder = doc_.file() != juce::File() ? doc_.file().getParentDirectory()
                                                    : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    const auto suggested = doc_.isPerformance() && doc_.file() != juce::File()
                               ? doc_.file()
                               : folder.getChildFile(juce::File::createLegalFileName(doc_.name()) + ext);
    chooser_ = std::make_unique<juce::FileChooser>(doc_.isPerformance() ? "Save the G2 performance" : "Save the G2 patch",
                                                   suggested, "*" + ext);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [write, ext](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              write(f.hasFileExtension(ext) ? f : f.withFileExtension(ext));
                          });
}

} // namespace g2ui
