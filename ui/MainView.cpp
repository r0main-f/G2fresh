#include "MainView.h"

namespace g2ui {
namespace {

const juce::String kOpenPattern = "*.pch2;*.prf2";

enum MenuId {
    kNew = 1, kOpen, kSave, kSaveAs, kClearRecent,
    kUndo, kRedo, kDelete, kRename,
    kZoomIn, kZoomOut, kZoomReset, kShowSettings, kClassicLook, kAnimateCables,
    kAudioSettings,
    kRecentBase = 100, // + index into the recent files list
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

    settings_.onStatus = [this](const juce::String& s) { setStatus(s); };
    settings_.onLayoutChanged = [this] { resized(); };
    addAndMakeVisible(settings_);
    browser_.onAdd = [this](std::uint8_t type) { va_.addModule(type); };
    browser_.onStatus = [this](const juce::String& s) { setStatus(s); };
    addAndMakeVisible(browser_);

    for (auto* area : {&va_, &fx_}) {
        area->onStatus = [this](const juce::String& s) { setStatus(s); };
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
}

// ---- Menus ----------------------------------------------------------------------

juce::StringArray MainView::getMenuBarNames()
{
    juce::StringArray names{"File", "Edit", "View"};
    if (onAudioSettings)
        names.add("Options");
    return names;
}

juce::PopupMenu MainView::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu m;
    if (index == 0) {
        m.addItem(item(kNew, "New Patch", kCmd + "N"));
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
    } else if (index == 1) {
        const auto& undo = doc_;
        m.addItem(item(kUndo, "Undo", kCmd + "Z", undo.canUndo()));
        m.addItem(item(kRedo, "Redo", kShift + kCmd + "Z", undo.canRedo()));
        m.addSeparator();
        m.addItem(item(kDelete, "Delete Module", "Delete", va_.hasSelection() || fx_.hasSelection()));
        m.addItem(item(kRename, doc_.isPerformance() ? "Rename Slot..." : "Rename Patch..."));
    } else if (index == 2) {
        m.addItem(item(kZoomIn, "Zoom In", kCmd + "+", zoom_ < kMaxZoom));
        m.addItem(item(kZoomOut, "Zoom Out", kCmd + "-", zoom_ > kMinZoom));
        m.addItem(item(kZoomReset, "Actual Size", kCmd + "0"));
        m.addSeparator();
        m.addItem(item(kShowSettings, "Show Patch Settings", {}, true, !settings_.isCollapsed()));
        m.addItem(item(kClassicLook, "Classic Look", {}, true, currentLook() == Look::Classic));
        m.addItem(item(kAnimateCables, "Animate Cables", {}, true, cableAnimation()));
    } else if (index == 3) {
        m.addItem(item(kAudioSettings, "Audio/MIDI Settings..."));
    }
    return m;
}

void MainView::menuItemSelected(int id, int)
{
    if (id >= kRecentBase) {
        const auto f = recent_.getFile(id - kRecentBase);
        confirmDiscard([this, f] { openFile(f); });
        return;
    }
    switch (id) {
    case kNew: newPatch(); break;
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
    return va_.hasSelection() ? &va_ : fx_.hasSelection() ? &fx_ : nullptr;
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

void MainView::changeListenerCallback(juce::ChangeBroadcaster*)
{
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
    for (int i = variations_.size(); --i >= 0;)
        variations_[i]->setBounds(bar.removeFromRight(28)), bar.removeFromRight(2);
    variationLabel_.setBounds(bar.removeFromRight(textWidth(variationLabel_)));

    settings_.setBounds(r.removeFromTop(settings_.preferredHeight(r.getWidth())));
    browser_.setBounds(r.removeFromTop(62));
    status_.setBounds(r.removeFromBottom(24));
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
        const auto bytes = doc_.saveBytes();
        if (f.replaceWithData(bytes.data(), bytes.size())) {
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
