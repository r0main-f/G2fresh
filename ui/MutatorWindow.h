// The Patch Mutator, as in the original editor: breed new versions of the
// current variation (randomize, mutate, cross, interpolate), audition them,
// keep the good ones in a gene bank.
#pragma once

#include "PatchDocument.h"

#include "g2/mutate.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui {

class MutatorWindow : public juce::DocumentWindow {
public:
    explicit MutatorWindow(PatchDocument& doc);
    ~MutatorWindow() override;
    void closeButtonPressed() override { setVisible(false); }

private:
    class Panel;
    std::unique_ptr<Panel> panel_;
};

} // namespace g2ui
