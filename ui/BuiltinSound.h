// The patch played by G2fresh itself (the plugin's or app's own sound engine)
// when no G2 plays it, as the editor sees it: on/off and a status line.
// Implemented by the plugin processor, so the editor needs no audio code.
#pragma once

#include <juce_core/juce_core.h>

namespace g2ui {

class BuiltinSound {
public:
    virtual ~BuiltinSound() = default;
    virtual bool builtinSoundEnabled() const = 0;
    virtual void setBuiltinSoundEnabled(bool on) = 0;
    // What plays and what does not ("2 modules not supported yet (...)").
    virtual juce::String builtinSoundStatus() const = 0;
};

} // namespace g2ui
