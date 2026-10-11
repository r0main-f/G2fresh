#include "Dialogs.h"

#include "Theme.h"

namespace g2ui {
namespace {

constexpr int kMaxNotes = 1024; // the file format's limit

juce::String noteName(int key)
{
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(names[key % 12]) + juce::String(key / 12 - 1);
}

void launch(std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* parent)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(content.release());
    o.dialogTitle = title;
    o.componentToCentreAround = parent;
    o.dialogBackgroundColour = juce::Colour(0xff2e3035);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

class NotesEditor : public juce::Component {
public:
    explicit NotesEditor(PatchDocument& doc) : doc_(doc)
    {
        editor_.setMultiLine(true, true);
        editor_.setReturnKeyStartsNewLine(true);
        editor_.setFont(theme::font());
        editor_.setInputRestrictions(kMaxNotes);
        editor_.setText(juce::String(doc.patch().notes), false);
        info_.setFont(theme::font());
        info_.setColour(juce::Label::textColourId, juce::Colour(0xffa8adb6));
        info_.setText("Notes are saved with the patch (up to 1024 characters).", juce::dontSendNotification);
        ok_.onClick = [this] {
            const auto text = editor_.getText().replace("\r\n", "\n").toStdString();
            doc_.perform("Edit patch notes", [&](g2::Patch& p) { p.notes = text; });
            close();
        };
        cancel_.onClick = [this] { close(); };
        for (auto* c : std::initializer_list<juce::Component*>{&editor_, &info_, &ok_, &cancel_})
            addAndMakeVisible(c);
        setSize(520, 380);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        auto buttons = r.removeFromBottom(30);
        ok_.setBounds(buttons.removeFromRight(90));
        buttons.removeFromRight(8);
        cancel_.setBounds(buttons.removeFromRight(90));
        info_.setBounds(buttons);
        r.removeFromBottom(8);
        editor_.setBounds(r);
    }

private:
    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState(0);
    }

    PatchDocument& doc_;
    juce::TextEditor editor_;
    juce::Label info_;
    juce::TextButton ok_{"OK"}, cancel_{"Cancel"};
};

// One row per slot, plus the performance-wide settings. Changes apply at once.
class PerformanceEditor : public juce::Component {
public:
    explicit PerformanceEditor(PatchDocument& doc) : doc_(doc)
    {
        static const char* const headers[] = {"Slot", "Patch name", "Enabled", "Keyboard", "Hold", "Lowest key",
                                              "Highest key", "MIDI channel"};
        for (auto* h : headers) {
            auto* l = labels_.add(new juce::Label({}, h));
            l->setFont(theme::font(true));
            l->setColour(juce::Label::textColourId, juce::Colour(0xffa8adb6));
            addAndMakeVisible(l);
        }
        const auto* perf = doc.performance();
        for (int i = 0; i < 4; ++i) {
            auto& row = rows_[static_cast<std::size_t>(i)];
            const auto& s = perf->header.slots[static_cast<std::size_t>(i)];
            row.slot.setText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)), juce::dontSendNotification);
            row.slot.setFont(theme::font(true));
            row.name.setText(PatchDocument::fromG2Bytes(s.patchName), false);
            // 16 characters of the G2 set; others become spaces (CDialogPerformance, RemoveNonModularChars).
            row.name.setInputRestrictions(PatchDocument::kMaxNameLength, PatchDocument::allowedNameCharacters());
            row.name.onTextChange = [this, i] {
                const auto name = g2::edit::modularName(rows_[static_cast<std::size_t>(i)].name.getText().toStdString());
                doc_.editPerformance([&](g2::file::PerfHeader& h) { h.slots[static_cast<std::size_t>(i)].patchName = name; });
            };
            auto toggle = [this, i](juce::ToggleButton& b, bool on, std::uint8_t g2::file::SlotSettings::*field) {
                b.setToggleState(on, juce::dontSendNotification);
                b.onClick = [this, i, &b, field] {
                    const auto v = static_cast<std::uint8_t>(b.getToggleState() ? 1 : 0);
                    doc_.editPerformance([&](g2::file::PerfHeader& h) { h.slots[static_cast<std::size_t>(i)].*field = v; });
                };
            };
            toggle(row.enabled, s.enabled != 0, &g2::file::SlotSettings::enabled);
            toggle(row.keyboard, s.keyboard != 0, &g2::file::SlotSettings::keyboard);
            toggle(row.hold, s.hold != 0, &g2::file::SlotSettings::hold);
            for (auto* box : {&row.lower, &row.upper}) {
                for (int k = 0; k < 128; ++k)
                    box->addItem(noteName(k), k + 1);
            }
            row.lower.setSelectedId(s.kbdRangeLower + 1, juce::dontSendNotification);
            row.upper.setSelectedId(s.kbdRangeUpper + 1, juce::dontSendNotification);
            row.lower.onChange = [this, i] { setKey(i, true); };
            row.upper.onChange = [this, i] { setKey(i, false); };
            for (int ch = 0; ch < 16; ++ch)
                row.channel.addItem(juce::String(ch + 1), ch + 1);
            row.channel.addItem("Off", 17);
            row.channel.setSelectedId(s.midiChannel + 1, juce::dontSendNotification);
            row.channel.onChange = [this, i] {
                const auto v = static_cast<std::uint8_t>(rows_[static_cast<std::size_t>(i)].channel.getSelectedId() - 1);
                doc_.editPerformance([&](g2::file::PerfHeader& h) { h.slots[static_cast<std::size_t>(i)].midiChannel = v; });
            };
            for (auto* c : row.all())
                addAndMakeVisible(c);
        }
        bpmLabel_.setText("Master clock (BPM)", juce::dontSendNotification);
        bpm_.setRange(24, 240, 1);
        bpm_.setSliderStyle(juce::Slider::IncDecButtons);
        bpm_.setValue(perf->header.masterClockBpm, juce::dontSendNotification);
        bpm_.onValueChange = [this] {
            const auto v = static_cast<std::uint8_t>(bpm_.getValue());
            doc_.editPerformance([&](g2::file::PerfHeader& h) { h.masterClockBpm = v; });
        };
        run_.setToggleState(perf->header.masterClockRun != 0, juce::dontSendNotification);
        run_.onClick = [this] {
            const auto v = static_cast<std::uint8_t>(run_.getToggleState());
            doc_.editPerformance([&](g2::file::PerfHeader& h) { h.masterClockRun = v; });
        };
        ranges_.setToggleState(perf->header.kbdRangeEnabled != 0, juce::dontSendNotification);
        ranges_.onClick = [this] {
            const auto v = static_cast<std::uint8_t>(ranges_.getToggleState());
            doc_.editPerformance([&](g2::file::PerfHeader& h) { h.kbdRangeEnabled = v; });
        };
        close_.onClick = [this] {
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
                w->exitModalState(0);
        };
        for (auto* c : std::initializer_list<juce::Component*>{&bpmLabel_, &bpm_, &run_, &ranges_, &close_})
            addAndMakeVisible(c);
        setSize(820, 330);
    }

    void resized() override
    {
        static const int widths[] = {44, 180, 80, 90, 64, 110, 110, 110};
        auto r = getLocalBounds().reduced(14);
        auto head = r.removeFromTop(26);
        for (int c = 0; c < labels_.size(); ++c)
            labels_[c]->setBounds(head.removeFromLeft(widths[c]));
        for (auto& row : rows_) {
            auto line = r.removeFromTop(34).reduced(0, 4);
            auto all = row.all();
            for (std::size_t c = 0; c < all.size(); ++c)
                all[c]->setBounds(line.removeFromLeft(widths[c]).reduced(c == 0 ? 0 : 3, 0));
        }
        r.removeFromTop(16);
        auto line = r.removeFromTop(30);
        bpmLabel_.setBounds(line.removeFromLeft(150));
        bpm_.setBounds(line.removeFromLeft(120));
        line.removeFromLeft(20);
        run_.setBounds(line.removeFromLeft(150));
        ranges_.setBounds(line.removeFromLeft(230));
        close_.setBounds(getLocalBounds().reduced(14).removeFromBottom(30).removeFromRight(90));
    }

private:
    struct Row {
        juce::Label slot;
        juce::TextEditor name;
        juce::ToggleButton enabled, keyboard, hold;
        juce::ComboBox lower, upper, channel;
        std::vector<juce::Component*> all() { return {&slot, &name, &enabled, &keyboard, &hold, &lower, &upper, &channel}; }
    };

    void setKey(int i, bool lower)
    {
        auto& row = rows_[static_cast<std::size_t>(i)];
        const auto v = static_cast<std::uint8_t>((lower ? row.lower : row.upper).getSelectedId() - 1);
        doc_.editPerformance([&](g2::file::PerfHeader& h) {
            auto& s = h.slots[static_cast<std::size_t>(i)];
            (lower ? s.kbdRangeLower : s.kbdRangeUpper) = v;
        });
    }

    PatchDocument& doc_;
    juce::OwnedArray<juce::Label> labels_;
    std::array<Row, 4> rows_;
    juce::Label bpmLabel_;
    juce::Slider bpm_;
    juce::ToggleButton run_{"Master clock running"}, ranges_{"Keyboard ranges enabled"};
    juce::TextButton close_{"Close"};
};

} // namespace

void showPatchNotes(PatchDocument& doc, juce::Component* parent)
{
    launch(std::make_unique<NotesEditor>(doc), "Patch Notes: " + doc.name(), parent);
}

void showPerformanceSettings(PatchDocument& doc, juce::Component* parent)
{
    if (doc.isPerformance())
        launch(std::make_unique<PerformanceEditor>(doc), "Performance Settings", parent);
}

} // namespace g2ui
