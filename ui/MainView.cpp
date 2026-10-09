#include "MainView.h"

namespace g2ui {
namespace {
const juce::String kOpenPattern = "*.pch2;*.prf2";
}

MainView::MainView(PatchDocument& doc)
    : doc_(doc), va_(doc, g2::Location::Va), fx_(doc, g2::Location::Fx)
{
    for (auto* b : {&new_, &open_, &save_, &saveAs_, &undo_, &redo_})
        addAndMakeVisible(b);
    new_.onClick = [this] { newPatch(); };
    open_.onClick = [this] { open(); };
    save_.onClick = [this] { save(false); };
    saveAs_.onClick = [this] { save(true); };
    undo_.onClick = [this] { doc_.undo(); };
    redo_.onClick = [this] { doc_.redo(); };

    for (int i = 0; i < g2::kUserVariations; ++i) {
        auto* b = variations_.add(new juce::TextButton(juce::String(i + 1)));
        b->setClickingTogglesState(true);
        b->setRadioGroupId(1);
        b->setTooltip("Variation " + juce::String(i + 1));
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
    classic_.setToggleState(currentLook() == Look::Classic, juce::dontSendNotification);
    classic_.setTooltip("Draw modules with the original editor's bitmaps");
    classic_.onClick = [this] {
        setCurrentLook(classic_.getToggleState() ? Look::Classic : Look::Modern);
        va_.settingsChanged();
        fx_.settingsChanged();
        va_.repaint();
        fx_.repaint();
    };
    addAndMakeVisible(classic_);
    animate_.setToggleState(cableAnimation(), juce::dontSendNotification);
    animate_.setTooltip("Show the signal flow along cables, from source to destination");
    animate_.onClick = [this] {
        setCableAnimation(animate_.getToggleState());
        va_.settingsChanged();
        fx_.settingsChanged();
    };
    addAndMakeVisible(animate_);
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    addAndMakeVisible(title_);
    status_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff26282c));
    addAndMakeVisible(status_);

    browser_.onAdd = [this](std::uint8_t type) { va_.addModule(type); };
    browser_.onStatus = [this](const juce::String& s) { setStatus(s); };
    addAndMakeVisible(browser_);

    for (auto* area : {&va_, &fx_})
        area->onStatus = [this](const juce::String& s) { setStatus(s); };
    vaPort_.setViewedComponent(&va_, false);
    fxPort_.setViewedComponent(&fx_, false);
    vaPort_.setScrollBarsShown(true, true);
    fxPort_.setScrollBarsShown(true, true);
    addAndMakeVisible(vaPort_);
    addAndMakeVisible(divider_);
    addAndMakeVisible(fxPort_);
    layout_.setItemLayout(0, 60, -1.0, -0.62);
    layout_.setItemLayout(1, 6, 6, 6);
    layout_.setItemLayout(2, 60, -1.0, -0.38);

    setWantsKeyboardFocus(true);
    doc_.addChangeListener(this);
    updateToolbar();
}

MainView::~MainView()
{
    doc_.removeChangeListener(this);
}

void MainView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    updateToolbar();
}

void MainView::updateToolbar()
{
    undo_.setEnabled(doc_.canUndo());
    redo_.setEnabled(doc_.canRedo());
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
    const auto f = doc_.file();
    juce::String title = f == juce::File() ? juce::String(perf ? "New performance" : "New patch")
                                           : f.getFileNameWithoutExtension();
    if (perf) {
        const auto slotName = juce::String(perf->header.slots[static_cast<std::size_t>(doc_.slot())].patchName);
        title << "  -  " << juce::String::charToString(static_cast<juce::juce_wchar>('A' + doc_.slot()))
              << (slotName.isNotEmpty() ? ": " + slotName : juce::String());
    }
    title_.setText(title + (doc_.isDirty() ? " *" : ""), juce::dontSendNotification);
    resized();
}

void MainView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff2e3035));
}

void MainView::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop(32).reduced(4);
    for (auto* b : {&new_, &open_, &save_, &saveAs_})
        b->setBounds(bar.removeFromLeft(b == &saveAs_ || b == &open_ ? 76 : 52)), bar.removeFromLeft(2);
    bar.removeFromLeft(10);
    for (auto* b : {&undo_, &redo_})
        b->setBounds(bar.removeFromLeft(52)), bar.removeFromLeft(2);
    bar.removeFromLeft(14);
    for (auto* b : variations_)
        b->setBounds(bar.removeFromLeft(26)), bar.removeFromLeft(1);
    bar.removeFromLeft(14);
    if (doc_.isPerformance()) {
        for (auto* b : slots_)
            b->setBounds(bar.removeFromLeft(26)), bar.removeFromLeft(1);
        bar.removeFromLeft(14);
    }
    classic_.setBounds(bar.removeFromRight(110));
    animate_.setBounds(bar.removeFromRight(130));
    title_.setBounds(bar);

    browser_.setBounds(r.removeFromTop(56));
    status_.setBounds(r.removeFromBottom(22));
    juce::Component* parts[] = {&vaPort_, &divider_, &fxPort_};
    layout_.layOutComponents(parts, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
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
    for (int i = 0; i < g2::kUserVariations; ++i)
        if (key == juce::KeyPress('1' + i, cmd, 0))
            return doc_.setVariation(i), true;
    return false;
}

void MainView::confirmDiscard(std::function<void()> then)
{
    if (!doc_.isDirty()) {
        then();
        return;
    }
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Discard changes?",
                                       "The current patch has unsaved changes.", "Discard", "Cancel", this,
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
                                      loadFile(fc.getResult());
                              });
    });
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
            doc_.setFile(f);
            doc_.markSaved();
            setStatus("Saved " + f.getFileName());
        } else {
            setStatus("Cannot write " + f.getFullPathName());
        }
    };
    if (!saveAs && doc_.file() != juce::File()) {
        write(doc_.file());
        return;
    }
    const auto ext = doc_.fileExtension();
    chooser_ = std::make_unique<juce::FileChooser>(doc_.isPerformance() ? "Save the G2 performance" : "Save the G2 patch",
                                                   doc_.file(), "*" + ext);
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
