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
    // Called when the bar collapses or expands (its height changes).
    std::function<void()> onLayoutChanged;

    static constexpr int kHeaderHeight = 26;
    // The height the bar needs at a given width (the morph cards move to a
    // second row when they don't fit beside the other settings).
    int preferredHeight(int width) const;
    bool isCollapsed() const { return collapsed_; }
    void setCollapsed(bool collapsed);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    // One knob, menu or toggle bound to a value of the document.
    class Control;

private:
    struct Group {
        juce::String title;
        std::vector<Control*> controls;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    Control& add(Group& group, std::unique_ptr<Control> c);

    juce::String summary() const;
    int groupWidth(const Group& group, int cardWidth) const;
    // Positions the controls for `width` (if apply) and returns the height.
    int layOut(int width, bool apply);
    static constexpr int kRowHeight = 104;
    static constexpr int kCaptionHeight = 20;
    static constexpr int kCardWidth = 86;

    PatchDocument& doc_;
    juce::OwnedArray<Control> controls_;
    std::vector<Group> groups_;
    bool collapsed_ = false;
};

} // namespace g2ui
