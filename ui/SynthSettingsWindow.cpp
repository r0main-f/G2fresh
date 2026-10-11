#include "SynthSettingsWindow.h"

#include "SynthSettingsForm.h"
#include "Theme.h"

namespace g2ui {
namespace {

const juce::Colour kBackground{0xff2e3035};
const juce::Colour kDimText{0xffa8adb6};

// Escape in a text field cancels the window, as anywhere else in it.
void escape(juce::Component& from)
{
    if (auto* w = from.findParentComponentOfClass<juce::DialogWindow>())
        w->closeButtonPressed();
}

// A value with up and down arrows (CSingleLineNumberEditView + CUpDownButtonView): typed values are taken when
// typing ends (Return, or the field losing focus); read-only shows text only (Control Pedal Gain).
class Spin : public juce::Component {
public:
    std::function<void(int)> onStep;                    // +1 / -1
    std::function<void(const juce::String&)> onTyped;

    Spin(int maxLength, bool readOnly)
    {
        field_.setFont(theme::font());
        field_.setJustification(juce::Justification::centredRight);
        field_.setInputRestrictions(maxLength);
        field_.setReadOnly(readOnly);
        field_.setCaretVisible(!readOnly);
        if (readOnly)
            field_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        field_.onReturnKey = [this] { typed(); };
        field_.onFocusLost = [this] { typed(); };
        field_.onEscapeKey = [this] { escape(*this); };
        for (auto* b : {&up_, &down_}) {
            b->setRepeatSpeed(400, 70);
            addAndMakeVisible(*b);
        }
        up_.onClick = [this] { step(1); };
        down_.onClick = [this] { step(-1); };
        addAndMakeVisible(field_);
    }

    void setText(const juce::String& text)
    {
        shown_ = text;
        field_.setText(text, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto arrows = r.removeFromRight(14).reduced(0, 1);
        up_.setBounds(arrows.removeFromTop(arrows.getHeight() / 2));
        down_.setBounds(arrows);
        r.removeFromRight(3);
        field_.setBounds(r);
    }

private:
    void typed()
    {
        if (field_.getText() != shown_ && onTyped)
            onTyped(field_.getText());
    }
    void step(int d)
    {
        if (onStep)
            onStep(d);
    }

    juce::TextEditor field_;
    juce::ArrowButton up_{"up", 0.75f, kDimText}, down_{"down", 0.25f, kDimText};
    juce::String shown_;
};

class Panel : public juce::Component, private juce::Timer {
public:
    explicit Panel(SynthSync& synth) : synth_(synth)
    {
        const auto& settings = synth_.link()->state().settings;
        opened_ = settings;
        lastSent_ = settings;
        echo_.start(settings);
        form_.take(settings);

        auto label = [this](juce::Label& l, const juce::String& text, bool dim = false) {
            l.setText(text, juce::dontSendNotification);
            l.setFont(theme::font());
            if (dim)
                l.setColour(juce::Label::textColourId, kDimText);
            addAndMakeVisible(l);
        };
        auto group = [this](juce::GroupComponent& g, const juce::String& text) {
            g.setText(text);
            g.setColour(juce::GroupComponent::outlineColourId, juce::Colour(0xff4a4f58));
            g.setColour(juce::GroupComponent::textColourId, kDimText);
            addAndMakeVisible(g);
        };
        auto toggle = [this](juce::ToggleButton& b, bool SynthSettingsForm::*field) {
            b.onClick = [this, &b, field] {
                form_.*field = b.getToggleState();
                changed();
            };
            addAndMakeVisible(b);
        };

        label(nameLabel_, "Name");
        name_.setFont(theme::font());
        name_.setInputRestrictions(static_cast<int>(SynthSettingsForm::kNameLength),
                                   juce::String(SynthSettingsForm::kNameChars.data(), SynthSettingsForm::kNameChars.size()));
        name_.onReturnKey = [this] { takeName(); };
        name_.onFocusLost = [this] { takeName(); };
        name_.onEscapeKey = [this] { escape(*this); };
        addAndMakeVisible(name_);

        group(channels_, "MIDI channel");
        for (int i = 0; i < 4; ++i) {
            auto& row = slots_[static_cast<std::size_t>(i)];
            label(row.label, "Slot " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)));
            row.channel.onStep = [this, i](int d) { setChannel(i, form_.channel[static_cast<std::size_t>(i)] + d); };
            row.channel.onTyped = [this, i](const juce::String& t) { setChannel(i, t.getIntValue()); };
            addAndMakeVisible(row.channel);
            row.active.onClick = [this, i] {
                form_.active[static_cast<std::size_t>(i)] = slots_[static_cast<std::size_t>(i)].active.getToggleState();
                changed();
            };
            addAndMakeVisible(row.active);
        }

        group(file_, "File");
        label(freeLabel_, "Free Mem:");
        label(free_, {});
        toggle(memoryProtect_, &SynthSettingsForm::memoryProtect);

        group(midi_, "MIDI Settings");
        label(globalLabel_, "Global Channel");
        global_.onStep = [this](int d) { setGlobal(form_.globalChannel + d); };
        global_.onTyped = [this](const juce::String& t) { setGlobal(t.getIntValue()); };
        addAndMakeVisible(global_);
        label(sysExLabel_, "SysEx ID");
        sysEx_.onStep = [this](int d) { setSysEx(form_.sysExId + d); };
        sysEx_.onTyped = [this](const juce::String& t) { setSysEx(t.getIntValue()); };
        addAndMakeVisible(sysEx_);
        toggle(sendClock_, &SynthSettingsForm::sendClock);
        toggle(ignoreClock_, &SynthSettingsForm::ignoreExternalClock);
        label(controllersLabel_, "Controllers");
        label(programChangeLabel_, "Program Change");
        toggle(ctrlReceive_, &SynthSettingsForm::controllersReceive);
        toggle(ctrlSend_, &SynthSettingsForm::controllersSend);
        toggle(prgReceive_, &SynthSettingsForm::programChangeReceive);
        toggle(prgSend_, &SynthSettingsForm::programChangeSend);

        group(polarity_, "Pedal polarity");
        for (auto* b : {&open_, &closed_}) {
            b->setRadioGroupId(2);
            b->onClick = [this, b] {
                if (!b->getToggleState())
                    return; // the other button of the pair, switched off
                form_.pedalClosed = b == &closed_;
                changed();
            };
            addAndMakeVisible(*b);
        }

        group(tune_, "Tune");
        label(masterTuneLabel_, "Master Tune");
        label(hz_, {});
        hz_.setJustificationType(juce::Justification::centredRight);
        label(semiLabel_, "Semi");
        semi_.onStep = [this](int d) { setTune(form_.semi + d, form_.cent); };
        semi_.onTyped = [this](const juce::String& t) { setTune(t.getIntValue(), form_.cent); };
        addAndMakeVisible(semi_);
        label(centLabel_, "Cent");
        cent_.onStep = [this](int d) { setTune(form_.semi, form_.cent + d); };
        cent_.onTyped = [this](const juce::String& t) { setTune(form_.semi, t.getIntValue()); };
        addAndMakeVisible(cent_);
        toggle(octaveShift_, &SynthSettingsForm::octaveShift);
        for (int i = 0; i < 5; ++i) {
            auto& b = octaves_[static_cast<std::size_t>(i)];
            b.setButtonText(i == 2 ? juce::String("0") : (i < 2 ? "-" : "+") + juce::String(std::abs(i - 2)));
            b.setClickingTogglesState(true);
            b.setRadioGroupId(1);
            b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3478d4));
            b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            b.setConnectedEdges((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 4 ? juce::Button::ConnectedOnRight : 0));
            b.onClick = [this, i] {
                if (octaves_[static_cast<std::size_t>(i)].getToggleState()) {
                    form_.octave = i - 2;
                    changed();
                }
            };
            addAndMakeVisible(b);
        }

        label(pedalGainLabel_, "Control Pedal Gain");
        pedalGain_.onStep = [this](int d) {
            form_.pedalGain = SynthSettingsForm::clip(form_.pedalGain + d, 0, 32);
            changed();
        };
        addAndMakeVisible(pedalGain_);
        toggle(localOn_, &SynthSettingsForm::localOn);

        ok_.onClick = [this] { ok(); };
        cancel_.onClick = [this] { cancel(); };
        ok_.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
        addAndMakeVisible(ok_);
        addAndMakeVisible(cancel_);

        show();
        setSize(640, 338);
        startTimerHz(20);
    }

    void paint(juce::Graphics& g) override { g.fillAll(kBackground); }

    void resized() override
    {
        using R = juce::Rectangle<int>;
        // Left: name, MIDI channels, file.
        nameLabel_.setBounds(12, 12, 46, 24);
        name_.setBounds(60, 12, 152, 24);
        channels_.setBounds(12, 44, 200, 166);
        for (int i = 0; i < 4; ++i) {
            auto& row = slots_[static_cast<std::size_t>(i)];
            const int y = 68 + 34 * i;
            row.label.setBounds(22, y, 54, 24);
            row.channel.setBounds(R(78, y, 58, 24));
            row.active.setBounds(144, y, 64, 24);
        }
        file_.setBounds(12, 218, 200, 80);
        freeLabel_.setBounds(22, 240, 76, 22);
        free_.setBounds(98, 240, 90, 22);
        memoryProtect_.setBounds(22, 264, 180, 24);
        // Middle: MIDI settings, pedal polarity.
        midi_.setBounds(222, 4, 200, 232);
        globalLabel_.setBounds(232, 28, 104, 24);
        global_.setBounds(338, 28, 72, 24);
        sysExLabel_.setBounds(232, 58, 104, 24);
        sysEx_.setBounds(338, 58, 72, 24);
        sendClock_.setBounds(232, 88, 186, 22);
        ignoreClock_.setBounds(232, 112, 186, 22);
        controllersLabel_.setBounds(232, 142, 90, 20);
        programChangeLabel_.setBounds(322, 142, 98, 20);
        ctrlReceive_.setBounds(232, 164, 88, 22);
        ctrlSend_.setBounds(232, 188, 88, 22);
        prgReceive_.setBounds(322, 164, 92, 22);
        prgSend_.setBounds(322, 188, 92, 22);
        polarity_.setBounds(222, 240, 200, 80);
        open_.setBounds(236, 262, 160, 22);
        closed_.setBounds(236, 286, 160, 22);
        // Right: tune, pedal gain, local, buttons.
        tune_.setBounds(432, 4, 196, 190);
        masterTuneLabel_.setBounds(442, 28, 96, 22);
        hz_.setBounds(530, 28, 88, 22);
        semiLabel_.setBounds(442, 58, 44, 24);
        semi_.setBounds(R(540, 58, 78, 24));
        centLabel_.setBounds(442, 88, 44, 24);
        cent_.setBounds(R(540, 88, 78, 24));
        octaveShift_.setBounds(442, 120, 176, 22);
        for (int i = 0; i < 5; ++i)
            octaves_[static_cast<std::size_t>(i)].setBounds(444 + 35 * i, 150, 35, 24);
        pedalGainLabel_.setBounds(432, 204, 128, 24);
        pedalGain_.setBounds(R(556, 204, 72, 24));
        localOn_.setBounds(432, 234, 150, 22);
        cancel_.setBounds(468, 300, 76, 26);
        ok_.setBounds(552, 300, 76, 26);
    }

    void cancel()
    {
        // HandleChangeRequests 0x45dc0: the values the window opened with, sent back.
        closing_ = true; // what is being typed is dropped
        form_.take(opened_);
        send();
        close();
    }

private:
    struct Slot {
        juce::Label label;
        Spin channel{2, false};
        juce::ToggleButton active{"active"};
    };

    void ok()
    {
        takeName();
        send();
        close();
    }

    void close()
    {
        closing_ = true;
        stopTimer();
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState(0);
    }

    void takeName()
    {
        if (closing_)
            return;
        const auto text = name_.getText().toStdString();
        if (text == form_.name)
            return;
        form_.name = text;
        changed();
    }

    void setChannel(int slot, int v)
    {
        form_.channel[static_cast<std::size_t>(slot)] = SynthSettingsForm::clip(v, 1, 16);
        changed();
    }
    void setGlobal(int v)
    {
        form_.globalChannel = SynthSettingsForm::wrap(v);
        changed();
    }
    void setSysEx(int v)
    {
        form_.sysExId = SynthSettingsForm::wrap(v);
        changed();
    }
    void setTune(int semi, int cent)
    {
        form_.semi = SynthSettingsForm::clip(semi, -6, 6);
        form_.cent = SynthSettingsForm::clip(cent, -100, 100);
        changed();
    }

    // Every change goes to the synth at once (TrySendData 0x45be2 from Idle).
    void changed()
    {
        if (closing_)
            return;
        show();
        send();
    }

    void send()
    {
        const auto next = form_.applyTo(lastSent_);
        if (next == lastSent_ || !synth_.ready())
            return;
        lastSent_ = next;
        echo_.sent(next);
        synth_.setSynthSettings(next);
    }

    // SetControlValues and SetupState: the fields as the form holds them.
    void show()
    {
        if (!name_.hasKeyboardFocus(true))
            name_.setText(juce::String(form_.name), false);
        for (std::size_t i = 0; i < 4; ++i) {
            slots_[i].channel.setText(juce::String(form_.channel[i]));
            slots_[i].active.setToggleState(form_.active[i], juce::dontSendNotification);
        }
        global_.setText(SynthSettingsForm::globalChannelText(form_.globalChannel));
        sysEx_.setText(SynthSettingsForm::sysExIdText(form_.sysExId));
        sendClock_.setToggleState(form_.sendClock, juce::dontSendNotification);
        ignoreClock_.setToggleState(form_.ignoreExternalClock, juce::dontSendNotification);
        ctrlReceive_.setToggleState(form_.controllersReceive, juce::dontSendNotification);
        ctrlSend_.setToggleState(form_.controllersSend, juce::dontSendNotification);
        prgReceive_.setToggleState(form_.programChangeReceive, juce::dontSendNotification);
        prgSend_.setToggleState(form_.programChangeSend, juce::dontSendNotification);
        hz_.setText(SynthSettingsForm::masterTuneText(form_.semi, form_.cent), juce::dontSendNotification);
        semi_.setText(juce::String(form_.semi));
        cent_.setText(juce::String(form_.cent));
        octaveShift_.setToggleState(form_.octaveShift, juce::dontSendNotification);
        for (int i = 0; i < 5; ++i) {
            auto& b = octaves_[static_cast<std::size_t>(i)];
            b.setToggleState(form_.octave == i - 2, juce::dontSendNotification);
            b.setEnabled(form_.octaveShift);
        }
        pedalGain_.setText(SynthSettingsForm::pedalGainText(form_.pedalGain));
        memoryProtect_.setToggleState(form_.memoryProtect, juce::dontSendNotification);
        (form_.pedalClosed ? closed_ : open_).setToggleState(true, juce::dontSendNotification);
        localOn_.setToggleState(form_.localOn, juce::dontSendNotification);
        showFreeMemory();
    }

    void showFreeMemory()
    {
        if (const auto* link = synth_.link())
            free_.setText(SynthSettingsForm::freeMemoryText(link->state().flashUsage), juce::dontSendNotification);
    }

    // Idle 0x45cd0: closes when the synth goes; takes what changed on the synth.
    void timerCallback() override
    {
        if (!synth_.ready()) {
            close();
            return;
        }
        const auto& now = synth_.link()->state().settings;
        if (echo_.changedElsewhere(now)) {
            lastSent_ = now;
            form_.take(now);
            show();
        }
        showFreeMemory();
    }

    SynthSync& synth_;
    SynthSettingsForm form_;
    g2::proto::SynthSettings opened_, lastSent_;
    SettingsEcho echo_;
    bool closing_ = false;

    juce::Label nameLabel_;
    juce::TextEditor name_;
    juce::GroupComponent channels_, file_, midi_, polarity_, tune_;
    std::array<Slot, 4> slots_;
    juce::Label freeLabel_, free_;
    juce::ToggleButton memoryProtect_{"Memory Protect"};
    juce::Label globalLabel_, sysExLabel_, controllersLabel_, programChangeLabel_;
    Spin global_{3, false}, sysEx_{3, false};
    juce::ToggleButton sendClock_{"Send Clock"}, ignoreClock_{"Ignore External Clock"};
    juce::ToggleButton ctrlReceive_{"Receive"}, ctrlSend_{"Send"}, prgReceive_{"Receive"}, prgSend_{"Send"};
    juce::ToggleButton open_{"Open"}, closed_{"Closed"};
    juce::Label masterTuneLabel_, hz_, semiLabel_, centLabel_, pedalGainLabel_;
    Spin semi_{2, false}, cent_{4, false}, pedalGain_{5, true};
    juce::ToggleButton octaveShift_{"Global Octave Shift"};
    std::array<juce::TextButton, 5> octaves_;
    juce::ToggleButton localOn_{"Local on"};
    juce::TextButton ok_{"OK"}, cancel_{"Cancel"};
};

class Window : public juce::DialogWindow {
public:
    explicit Window(SynthSync& synth)
        : DialogWindow("Synth Settings - Port 1", kBackground, true, true), panel_(new Panel(synth))
    {
        setUsingNativeTitleBar(true);
        setContentOwned(panel_, true);
        setResizable(false, false);
    }
    // The close button and Escape cancel, as the original's Cancel item.
    void closeButtonPressed() override { panel_->cancel(); }

private:
    Panel* panel_; // owned as the content
};

} // namespace

std::unique_ptr<juce::Component> makeSynthSettingsPanel(SynthSync& synth)
{
    return synth.ready() ? std::make_unique<Panel>(synth) : nullptr;
}

std::unique_ptr<juce::Component> openSynthSettings(SynthSync& synth, juce::Component* parent, std::function<void()> closed)
{
    if (!synth.ready())
        return nullptr;
    auto w = std::make_unique<Window>(synth);
    w->centreAroundComponent(parent, w->getWidth(), w->getHeight());
    w->setVisible(true);
    w->enterModalState(true, juce::ModalCallbackFunction::create([then = std::move(closed)](int) {
                           if (then)
                               then();
                       }),
                       false);
    return w;
}

} // namespace g2ui
