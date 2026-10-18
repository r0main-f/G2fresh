// The Patch Mutator, as in the original editor: breed new versions of the
// current variation (randomize, mutate, cross, interpolate), audition them,
// keep the good ones in a gene bank.
#pragma once

#include "PatchDocument.h"
#include "PatchKeys.h"

#include "g2/mutate.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui {

class MutatorWindow : public juce::DocumentWindow {
public:
    explicit MutatorWindow(PatchDocument& doc);
    ~MutatorWindow() override;
    void closeButtonPressed() override { setVisible(false); }
    // The Mutator's keys (PatchKeys.h), as the patch window hands them over while the Mutator is open.
    void keyAction(MutatorKey k);
    // The window passes the keys on when nothing inside it has the keyboard focus (round-5 hand tests: N, pressed right
    // after opening it, went nowhere).
    bool keyPressed(const juce::KeyPress& key) override;

private:
    class Panel;
    std::unique_ptr<Panel> panel_;
};

} // namespace g2ui
