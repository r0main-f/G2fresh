// Where the editor's "MIDI Output to G2" menu sends its choice. The G2's USB
// protocol carries no MIDI (re/notes/usb-protocol.md §12), so the plugin
// forwards its track's MIDI (the stand-alone app, its MIDI input) to a
// hardware MIDI port wired to the synth's MIDI IN. Implemented by the plugin
// (MidiForwarder), so the editor needs no audio-device code.
#pragma once

#include <juce_core/juce_core.h>

namespace g2ui {

class MidiOutputTarget {
public:
    virtual ~MidiOutputTarget() = default;

    struct Device {
        juce::String identifier, name;
    };
    virtual juce::Array<Device> availableDevices() const = 0;
    virtual juce::String deviceIdentifier() const = 0; // "" when off
    virtual void setDevice(const juce::String& identifier) = 0; // "" turns it off
    // 0: channels as played; 1..16: every channel message to this channel.
    virtual int channel() const = 0;
    virtual void setChannel(int channel) = 0;
};

} // namespace g2ui
