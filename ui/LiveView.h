// The Live view: the emulated G2 as an instrument. Its front panel, laid out
// as on a Nord Modular G2X (the manual's panel drawing), shows what the G2 OS
// drives (displays, LEDs, knob collars) and sends the user's presses, knob
// turns and the master level to it; below it, the pitch stick, the mod wheel,
// the G2X's two global wheels and a 61-key keyboard, playable with the mouse
// (velocity from where the key is hit) and the computer keyboard.
#pragma once

#include "EmulatorHost.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>

namespace g2ui {

class LiveView : public juce::Component, private juce::Timer, private juce::MidiKeyboardState::Listener {
public:
    // `startEmulator`: what the "Start the Emulated G2" button does.
    LiveView(EmulatorHost* host, std::function<void()> startEmulator);
    ~LiveView() override;

    void setHost(EmulatorHost* host);
    // Gives the keyboard the computer keyboard's focus (when the view is shown).
    void focusKeyboard();

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    class Panel;
    class Controllers;
    class Keyboard;
    struct Backdrop : juce::Component {
        void paint(juce::Graphics&) override;
    };

    void timerCallback() override;
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void midi(std::initializer_list<std::uint8_t> bytes);

    EmulatorHost* host_ = nullptr;
    PanelSnapshot snapshot_;
    juce::MidiKeyboardState keyState_;
    std::unique_ptr<Panel> panel_;
    std::unique_ptr<Controllers> controllers_;
    std::unique_ptr<Keyboard> keyboard_;
    juce::TextButton start_{"Start the Emulated G2"};
    Backdrop backdrop_;
    juce::Label hint_;
};

} // namespace g2ui
