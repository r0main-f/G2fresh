// The patch settings strip: voices, category, volume, glide, bend, vibrato,
// arpeggiator, octave shift, sustain and the 8 morph groups. Values follow the
// selected variation; every change is undoable.
#pragma once

#include "PatchDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace g2ui {

class PatchSettingsBar : public juce::Component, private juce::ChangeListener {
public:
    explicit PatchSettingsBar(PatchDocument& doc);
    ~PatchSettingsBar() override;

    std::function<void(const juce::String&)> onStatus;

    void paint(juce::Graphics&) override;
    void resized() override;

    // One knob, menu or toggle bound to a value of the document.
    class Control;

private:
    struct Group {
        juce::String title;
        std::vector<Control*> controls;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    Control& add(Group& group, std::unique_ptr<Control> c);

    PatchDocument& doc_;
    juce::OwnedArray<Control> controls_;
    std::vector<Group> groups_;
};

} // namespace g2ui
