// The emulated G2 running by itself, in real time, for a sound engine: a thread of its own (with the big stack the
// DSPs' JIT needs) runs the Machine ahead of an audio thread that pulls the DAC output, keeping `bufferMs` of audio
// ready. MIDI goes in from any thread; our protocol client talks to the machine through MachineTransport (the
// machine's USB queues are thread-safe), e.g. a LocalLink built on it.
//
// The audio is the G2's own: 96 kHz, 4 words per frame as Machine::run() gives them (outputs 1, 3, 2, 4). Resampling
// to the host's rate is the caller's job.
#pragma once

#include "g2emu/machine.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace g2emu {

class Runner {
public:
    struct Options {
        Machine::Options machine = defaultMachineOptions();
        double bufferMs = 20;      // audio kept ready ahead of the reader
        std::uint32_t chunkFrames = 96;  // frames the machine runs at a time (1 ms)
        std::vector<std::uint8_t> flash;  // the flash's content at power-on (empty: as the machine starts it)
        static Machine::Options defaultMachineOptions()
        {
            Machine::Options o;
            o.threads = -1;  // the DSPs on threads of their own: two where the computer has the cores (Machine::autoThreads)
            return o;
        }
    };

    struct Stats {
        std::uint64_t framesRead = 0, framesMissing = 0;  // the reader asked for frames that were not there yet
        double speed = 0;  // emulated seconds per wall second while the machine runs (its rests left out), recently
        double longestChunkMs = 0;  // the longest wall time one chunk took (a stall of the machine's threads shows here)
    };

    explicit Runner(const Firmware& firmware);
    Runner(const Firmware& firmware, Options options);
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    // Audio thread, real-time safe (no locks, no allocation): up to `frames` frames of 4 floats into `out`.
    // Returns how many were there; the rest of `out` is set to silence.
    std::size_t read(float* out, std::size_t frames);
    std::size_t available() const;

    // Stops the machine's thread (for good). Then machine() may be used from the caller's thread, e.g. to save
    // machine().flash(). The destructor stops it too.
    void stop();
    // The most frames one read() takes (a host's block, in 96 kHz frames): the machine then keeps that much more
    // ready, so that after the reader's largest take bufferMs are still left. Part of latencyFrames(). Any thread.
    static constexpr std::uint32_t MaxReadSize = 16384;  // 170 ms
    void setReadSize(std::uint32_t frames) { readSize_.store(std::min(frames, MaxReadSize), std::memory_order_relaxed); }

    // Any thread.
    void midiIn(std::span<const std::uint8_t> bytes) { machine_->midiIn(bytes); }
    // Sample-accurate MIDI, from the audio thread before its read(): the bytes start on the G2's MIDI IN `offset`
    // frames (96 kHz) after the first frame the next read() returns, plus latencyFrames(). So the delay from a
    // block's MIDI event to its sound is the same for every event (plus the OS's own reaction). Real-time safe
    // (a lock-free queue of 1024 events of up to 16 bytes; longer messages take several).
    void midiInAt(std::span<const std::uint8_t> bytes, std::uint32_t offset);
    std::uint32_t latencyFrames() const;
    // The front panel (any thread, not real-time safe: they take a mutex): see Machine.
    PanelState panel() const { return machine_->panel(); }
    void panelButton(PanelButton b, bool down) { machine_->panelButton(b, down); }
    void panelEncoder(PanelEncoder e, int steps) { machine_->panelEncoder(e, steps); }
    void panelAnalog(PanelAnalog a, float value) { machine_->panelAnalog(a, value); }
    void panelKey(int key, bool down, int velocity = 100) { machine_->panelKey(key, down, velocity); }
    void panelSustainPedal(bool down) { machine_->panelSustainPedal(down); }
    Machine& machine() { return *machine_; }
    Stats stats() const;
    void resetLongestChunk() { longestChunk_.store(0); }

private:
    void loop();

    Options options_;
    std::unique_ptr<Machine> machine_;
    std::vector<float> ring_;
    std::size_t capacity_ = 0;  // frames
    std::atomic<std::uint64_t> written_{0}, read_{0};
    std::atomic<std::int64_t> frameOffset_{0};  // machine frame - ring frame
    struct MidiEvent {
        static constexpr std::size_t MaxBytes = 16;
        std::uint64_t frame = 0;
        std::uint8_t size = 0;
        std::array<std::uint8_t, MaxBytes> bytes{};
    };
    std::array<MidiEvent, 1024> midiRing_{};
    std::atomic<std::uint64_t> midiHead_{0}, midiTail_{0};
    std::atomic<std::uint64_t> missing_{0};
    std::atomic<double> speed_{0}, longestChunk_{0};
    std::atomic<bool> quit_{false};
    std::atomic<std::uint32_t> readSize_{0};
    // bumped by every read(): the machine's thread sleeps on it while the buffer is full (no polling: under load a
    // short sleep could overrun by milliseconds and leave the reader short)
    std::atomic<std::uint32_t> consumed_{0};
    struct Thread;
    std::unique_ptr<Thread> thread_;
};

} // namespace g2emu
