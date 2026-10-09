#include "MainView.h"

namespace g2ui {
namespace {
const juce::String kPatchPattern = "*.pch2";
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
    const auto f = doc_.file();
    title_.setText((f == juce::File() ? juce::String("New patch") : f.getFileNameWithoutExtension())
                       + (doc_.isDirty() ? " *" : ""),
                   juce::dontSendNotification);
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
        chooser_ = std::make_unique<juce::FileChooser>("Open a G2 patch", doc_.file(), kPatchPattern);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File())
                                      loadFile(fc.getResult());
                              });
    });
}

void MainView::loadFile(const juce::File& f)
{
    juce::MemoryBlock mb;
    if (!f.loadFileAsData(mb)) {
        setStatus("Cannot read " + f.getFullPathName());
        return;
    }
    try {
        const auto* p = static_cast<const std::uint8_t*>(mb.getData());
        doc_.loadBytes(std::vector<std::uint8_t>(p, p + mb.getSize()));
        doc_.setFile(f);
        setStatus("Opened " + f.getFileName());
        updateToolbar();
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
    chooser_ = std::make_unique<juce::FileChooser>("Save the G2 patch", doc_.file(), kPatchPattern);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [write](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              write(f.hasFileExtension("pch2") ? f : f.withFileExtension("pch2"));
                          });
}

} // namespace g2ui
