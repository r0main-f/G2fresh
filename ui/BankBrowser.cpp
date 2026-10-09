#include "BankBrowser.h"

#include "Theme.h"

#include "g2/edit.hpp"

#include <map>

namespace g2ui {
namespace {

constexpr int kPrograms = 128;     // programs per bank (the name list wraps after 127)
constexpr int kPatchBanks = 32;    // [I] the G2's patch banks; more are shown if the synth lists them
constexpr int kPerformanceBanks = 8;

class Rows : public juce::ListBoxModel {
public:
    std::function<int()> count;
    std::function<juce::String(int)> text;
    std::function<bool(int)> dim;
    std::function<void(int)> selected, doubleClicked;

    int getNumRows() override { return count ? count() : 0; }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected) override
    {
        if (isSelected)
            g.fillAll(juce::Colour(0xff3478d4));
        g.setColour(dim && dim(row) && !isSelected ? juce::Colour(0xff7d828c) : juce::Colours::white);
        g.setFont(theme::font());
        g.drawText(text ? text(row) : juce::String(), 8, 0, width - 12, height, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged(int row) override
    {
        if (selected && row >= 0)
            selected(row);
    }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (doubleClicked)
            doubleClicked(row);
    }
};

} // namespace

class BankBrowser::Panel : public juce::Component, private juce::ChangeListener {
public:
    Panel(SynthSync& synth, std::function<void(std::function<void()>)> confirmDiscard)
        : synth_(synth), confirmDiscard_(std::move(confirmDiscard))
    {
        for (auto* b : {&patches_, &performances_}) {
            b->setClickingTogglesState(true);
            b->setRadioGroupId(1);
            b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3478d4));
            b->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            b->onClick = [this] { showType(patches_.getToggleState() ? 0 : 1); };
            addAndMakeVisible(*b);
        }
        patches_.setToggleState(true, juce::dontSendNotification);
        status_.setFont(theme::font());
        addAndMakeVisible(status_);

        bankRows_.count = [this] { return bankCount(); };
        bankRows_.text = [this](int b) {
            const int n = static_cast<int>(names_[b].size());
            return "Bank " + juce::String(b + 1) + (n > 0 ? "   (" + juce::String(n) + ")" : juce::String());
        };
        bankRows_.dim = [this](int b) { return names_[b].empty(); };
        bankRows_.selected = [this](int b) {
            bank_ = b;
            programs_.updateContent();
            programs_.repaint();
            updateButtons();
        };
        banks_.setModel(&bankRows_);
        progRows_.count = [] { return kPrograms; };
        progRows_.text = [this](int p) {
            const auto it = names_[bank_].find(p);
            if (it == names_[bank_].end())
                return juce::String(p + 1).paddedLeft('0', 3) + "   -";
            return juce::String(p + 1).paddedLeft('0', 3) + "   " + it->second.first
                 + (type_ == 0 ? "   " + juce::String(g2::edit::categoryName(it->second.second)) : juce::String());
        };
        progRows_.dim = [this](int p) { return !names_[bank_].count(p); };
        progRows_.selected = [this](int) { updateButtons(); };
        progRows_.doubleClicked = [this](int) { load(); };
        programs_.setModel(&progRows_);
        for (auto* l : {&banks_, &programs_}) {
            l->setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff1f2125));
            l->setRowHeight(22);
            addAndMakeVisible(*l);
        }

        slotLabel_.setFont(theme::font());
        addAndMakeVisible(slotLabel_);
        for (int s = 0; s < 4; ++s)
            slot_.addItem("Slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + s)), s + 1);
        slot_.setSelectedId(synth_.boundSlot() >= 0 ? synth_.boundSlot() + 1 : 1, juce::dontSendNotification);
        slot_.onChange = [this] { updateButtons(); };
        addAndMakeVisible(slot_);
        open_.setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(open_);
        load_.onClick = [this] { load(); };
        store_.onClick = [this] { store(); };
        addAndMakeVisible(load_);
        addAndMakeVisible(store_);

        synth_.addChangeListener(this);
        refresh();
        setSize(560, 520);
    }

    ~Panel() override { synth_.removeChangeListener(this); }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2a2c31)); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        auto top = r.removeFromTop(26);
        patches_.setBounds(top.removeFromLeft(100));
        top.removeFromLeft(4);
        performances_.setBounds(top.removeFromLeft(120));
        top.removeFromLeft(10);
        status_.setBounds(top);
        r.removeFromTop(8);
        auto bottom = r.removeFromBottom(28);
        slotLabel_.setBounds(bottom.removeFromLeft(36));
        slot_.setBounds(bottom.removeFromLeft(90));
        bottom.removeFromLeft(8);
        open_.setBounds(bottom.removeFromLeft(130));
        store_.setBounds(bottom.removeFromRight(130));
        bottom.removeFromRight(6);
        load_.setBounds(bottom.removeFromRight(80));
        r.removeFromBottom(8);
        banks_.setBounds(r.removeFromLeft(150));
        r.removeFromLeft(8);
        programs_.setBounds(r);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    int bankCount() const
    {
        return std::max(type_ == 0 ? kPatchBanks : kPerformanceBanks, names_.empty() ? 0 : names_.rbegin()->first + 1);
    }

    void showType(int type)
    {
        type_ = type;
        bank_ = 0;
        refresh();
        banks_.selectRow(0);
    }

    void refresh()
    {
        names_.clear();
        if (const auto* link = synth_.link(); link != nullptr && synth_.ready())
            for (const auto& n : link->state().flash[static_cast<std::size_t>(type_)])
                names_[n.bank][n.prog] = {juce::String(n.name), n.category};
        for (int b = 0; b < bankCount(); ++b)
            names_[b]; // every bank has a (possibly empty) list
        status_.setText(synth_.ready() ? synth_.statusText() + ": the names the synth reports"
                                       : juce::String("Connect to a G2 (Synth menu) to see its memory"),
                        juce::dontSendNotification);
        banks_.updateContent();
        programs_.updateContent();
        banks_.repaint();
        programs_.repaint();
        if (banks_.getSelectedRow() < 0 && synth_.ready())
            banks_.selectRow(0);
        updateButtons();
    }

    int selectedProgram() const { return programs_.getSelectedRow(); }
    int targetSlot() const { return type_ == 0 ? slot_.getSelectedId() - 1 : 4; }

    void updateButtons()
    {
        const bool ready = synth_.ready();
        const int p = selectedProgram();
        const bool used = p >= 0 && names_[bank_].count(p);
        slot_.setVisible(type_ == 0);
        slotLabel_.setVisible(type_ == 0);
        load_.setEnabled(ready && used);
        store_.setEnabled(ready && p >= 0);
        store_.setButtonText(type_ == 0 ? "Store Slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + targetSlot())) + " Here"
                                        : juce::String("Store Performance Here"));
    }

    void load()
    {
        const int p = selectedProgram();
        if (!synth_.ready() || p < 0 || !names_[bank_].count(p))
            return;
        const int slot = targetSlot();
        const auto bank = static_cast<std::uint8_t>(bank_), prog = static_cast<std::uint8_t>(p);
        if (open_.getToggleState())
            confirmDiscard_([this, slot, bank, prog] { synth_.loadFromBank(slot, bank, prog, true); });
        else
            synth_.loadFromBank(slot, bank, prog, false);
    }

    void store()
    {
        const int p = selectedProgram();
        if (!synth_.ready() || p < 0)
            return;
        const int slot = targetSlot();
        const auto bank = static_cast<std::uint8_t>(bank_), prog = static_cast<std::uint8_t>(p);
        const auto it = names_[bank_].find(p);
        if (it == names_[bank_].end()) {
            synth_.storeToBank(slot, bank, prog);
            return;
        }
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, "Replace " + it->second.first + "?",
            "Bank " + juce::String(bank_ + 1) + ", " + juce::String(p + 1) + " holds \"" + it->second.first
                + "\". Store over it? The synth's copy cannot be recovered.",
            "Replace", "Cancel", this,
            juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<Panel>(this), slot, bank, prog](int r) {
                if (safe && r == 1)
                    safe->synth_.storeToBank(slot, bank, prog);
            }));
    }

    SynthSync& synth_;
    std::function<void(std::function<void()>)> confirmDiscard_;
    int type_ = 0; // 0 patches, 1 performances
    int bank_ = 0;
    std::map<int, std::map<int, std::pair<juce::String, std::uint8_t>>> names_; // bank -> prog -> (name, category)
    juce::TextButton patches_{"Patches"}, performances_{"Performances"};
    juce::Label status_;
    Rows bankRows_, progRows_;
    juce::ListBox banks_{"Banks"}, programs_{"Programs"};
    juce::Label slotLabel_{{}, "Into"};
    juce::ComboBox slot_;
    juce::ToggleButton open_{"Open in editor"};
    juce::TextButton load_{"Load"}, store_{"Store Here"};
};

BankBrowser::BankBrowser(SynthSync& synth, std::function<void(std::function<void()>)> confirmDiscard)
    : DocumentWindow("Synth Memory", juce::Colour(0xff2a2c31), DocumentWindow::closeButton),
      panel_(std::make_unique<Panel>(synth, std::move(confirmDiscard)))
{
    setUsingNativeTitleBar(true);
    setContentNonOwned(panel_.get(), true);
    setResizable(true, false);
    centreWithSize(getWidth(), getHeight());
}

BankBrowser::~BankBrowser()
{
    clearContentComponent();
}

} // namespace g2ui
