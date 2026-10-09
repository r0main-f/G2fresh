// Sends the plugin's incoming MIDI to a hardware MIDI output (into the G2's
// MIDI IN). The audio thread only stamps each message with the time it is due
// and pushes it into a lock-free queue; a sender thread sends it on time.
// System exclusive messages are not forwarded.
#pragma once

#include "MidiOutputTarget.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <vector>

class MidiForwarder final : public g2ui::MidiOutputTarget, private juce::Thread {
public:
    MidiForwarder();
    ~MidiForwarder() override;

    // Audio thread.
    void prepare(double sampleRate) { sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0; }
    void process(const juce::MidiBuffer& midi);

    // MidiOutputTarget (message thread).
    juce::Array<Device> availableDevices() const override;
    juce::String deviceIdentifier() const override { return deviceId_; }
    void setDevice(const juce::String& identifier) override;
    int channel() const override { return channel_; }
    void setChannel(int channel) override;

    // For the plugin state: the device's name finds it again if its
    // identifier changed (another USB port, another machine).
    juce::String deviceName() const { return deviceName_; }
    void restore(const juce::String& identifier, const juce::String& name, int channel);
    // Message thread: the settings changed (to save them with the project).
    std::function<void()> onChange;

private:
    struct Event {
        double due = 0.0; // Time::getMillisecondCounterHiRes()
        std::array<std::uint8_t, 3> data{};
        int size = 0;
    };
    void run() override;

    static constexpr int kCapacity = 4096;
    juce::AbstractFifo fifo_{kCapacity};
    std::array<Event, kCapacity> events_;
    std::atomic<double> sampleRate_{44100.0};
    std::atomic<int> channel_{0};
    std::atomic<bool> active_{false};

    juce::CriticalSection outputLock_; // message thread and sender thread
    std::unique_ptr<juce::MidiOutput> output_;
    juce::String deviceId_, deviceName_;
    std::vector<Event> pending_; // sender thread only
};
