// The plugin's automatable parameters: a fixed bank that hosts handle well,
// mapped onto the patch being edited (the current slot of a performance):
//   "Knob 1A-1" .. "Knob 5C-8"  the 120 assignable knobs (whatever the patch
//                               assigns them to: Assign Knob in a control's menu)
//   "Morph 1" .. "Morph 8"      the morph dials (patch settings)
//   "Variation"                 the variation played (1..8)
// A host change moves the patch value in the editor, outside the undo
// history (like a knob turned on the synth); an edit in the editor is sent to
// the host, so automation can be recorded. Values apply to the current
// variation. (Over USB, the same changes will go to the synth.)
#pragma once

#include "PatchDocument.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <vector>

class AutomationBank final : private juce::Timer, private juce::ChangeListener {
public:
    AutomationBank(juce::AudioProcessor& processor, g2ui::PatchDocument& doc);
    ~AutomationBank() override;

private:
    struct Slot {
        juce::RangedAudioParameter* param = nullptr;
        float lastHost = -1.0f; // the value last seen from (or sent to) the host
    };
    // What one parameter controls now, and its display, for any thread.
    struct Info {
        juce::String name; // "OscB1 Pitch", or "" when unassigned
        int max = 0;
    };

    void timerCallback() override;              // host -> patch
    void changeListenerCallback(juce::ChangeBroadcaster*) override; // patch -> host
    std::optional<g2::edit::Target> targetOf(int index) const;
    juce::String textFor(int index, float normalised) const;
    void refreshInfo();

    juce::AudioProcessor& processor_;
    g2ui::PatchDocument& doc_;
    std::vector<Slot> slots_; // 120 knobs, then 8 morph dials
    juce::AudioParameterChoice* variation_ = nullptr;
    int lastVariation_ = -1;
    mutable juce::SpinLock infoLock_;
    std::vector<Info> info_;
};
