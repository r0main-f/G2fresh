// The synth's memory: the patches and performances stored in the G2's flash
// banks (the names the synth reports), to load into a slot (and open in the
// editor) or to store a slot into.
#pragma once

#include "SynthSync.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace g2ui {

class BankBrowser : public juce::DocumentWindow {
public:
    // `confirmDiscard` asks before the document is replaced, then calls back.
    BankBrowser(SynthSync& synth, std::function<void(std::function<void()>)> confirmDiscard);
    ~BankBrowser() override;
    void closeButtonPressed() override { setVisible(false); }

private:
    class Panel;
    std::unique_ptr<Panel> panel_;
};

} // namespace g2ui
