#include "MidiForwarder.h"

#include <algorithm>

MidiForwarder::MidiForwarder() : juce::Thread("G2fresh MIDI out")
{
    startThread(juce::Thread::Priority::high);
}

MidiForwarder::~MidiForwarder()
{
    stopThread(1000);
}

void MidiForwarder::process(const juce::MidiBuffer& midi)
{
    if (!active_.load(std::memory_order_relaxed) || midi.isEmpty())
        return;
    const double start = juce::Time::getMillisecondCounterHiRes();
    const double msPerSample = 1000.0 / sampleRate_.load(std::memory_order_relaxed);
    const int channel = channel_.load(std::memory_order_relaxed);
    for (const auto metadata : midi) {
        if (metadata.numBytes < 1 || metadata.numBytes > 3)
            continue; // system exclusive
        Event e;
        e.due = start + metadata.samplePosition * msPerSample;
        e.size = metadata.numBytes;
        std::copy(metadata.data, metadata.data + metadata.numBytes, e.data.begin());
        if (channel > 0 && e.data[0] >= 0x80 && e.data[0] < 0xF0)
            e.data[0] = static_cast<std::uint8_t>((e.data[0] & 0xF0) | (channel - 1));
        const auto scope = fifo_.write(1);
        if (scope.blockSize1 > 0)
            events_[static_cast<std::size_t>(scope.startIndex1)] = e;
        // A full queue drops the message rather than block the audio thread.
    }
}

void MidiForwarder::run()
{
    while (!threadShouldExit()) {
        {
            const auto scope = fifo_.read(fifo_.getNumReady());
            for (int i = 0; i < scope.blockSize1; ++i)
                pending_.push_back(events_[static_cast<std::size_t>(scope.startIndex1 + i)]);
            for (int i = 0; i < scope.blockSize2; ++i)
                pending_.push_back(events_[static_cast<std::size_t>(scope.startIndex2 + i)]);
        }
        if (!pending_.empty()) {
            std::stable_sort(pending_.begin(), pending_.end(), [](const Event& a, const Event& b) { return a.due < b.due; });
            const double now = juce::Time::getMillisecondCounterHiRes();
            const auto due = std::find_if(pending_.begin(), pending_.end(), [now](const Event& e) { return e.due > now; });
            {
                const juce::ScopedLock lock(outputLock_);
                if (output_ != nullptr)
                    for (auto it = pending_.begin(); it != due; ++it)
                        output_->sendMessageNow(juce::MidiMessage(it->data.data(), it->size));
            }
            pending_.erase(pending_.begin(), due);
        }
        // Millisecond timing only while there is somewhere to send to; with no
        // port selected the thread just checks now and then (no idle wake-ups).
        wait(!pending_.empty() ? 1 : active_.load(std::memory_order_relaxed) ? 2 : 100);
    }
}

juce::Array<g2ui::MidiOutputTarget::Device> MidiForwarder::availableDevices() const
{
    juce::Array<Device> list;
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        list.add({d.identifier, d.name});
    return list;
}

void MidiForwarder::setDevice(const juce::String& identifier)
{
    std::unique_ptr<juce::MidiOutput> next;
    juce::String name;
    if (identifier.isNotEmpty()) {
        next = juce::MidiOutput::openDevice(identifier);
        if (next != nullptr)
            name = next->getName();
    }
    active_ = next != nullptr;
    {
        const juce::ScopedLock lock(outputLock_);
        output_ = std::move(next);
    }
    deviceId_ = active_ ? identifier : juce::String();
    deviceName_ = name;
    if (onChange)
        onChange();
}

void MidiForwarder::setChannel(int channel)
{
    channel_ = juce::jlimit(0, 16, channel);
    if (onChange)
        onChange();
}

void MidiForwarder::restore(const juce::String& identifier, const juce::String& name, int channel)
{
    channel_ = juce::jlimit(0, 16, channel);
    juce::String id;
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.identifier == identifier)
            id = d.identifier;
    if (id.isEmpty() && name.isNotEmpty())
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == name)
                id = d.identifier;
    setDevice(id);
}
