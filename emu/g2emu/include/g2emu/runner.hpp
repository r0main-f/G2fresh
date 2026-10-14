// The emulated G2 running by itself, in real time, for a sound engine: a thread of its own (with the big stack the
// DSPs' JIT needs) runs the Machine ahead of an audio thread that pulls the DAC output, keeping `bufferMs` of audio
// ready. MIDI goes in from any thread; our protocol client talks to the machine through MachineTransport (the
// machine's USB queues are thread-safe), e.g. a LocalLink built on it.
//
// How far ahead (re/notes/audio-path.md). The reader takes a block at a time; the machine then makes up for it, at
// two to three times real time, and rests. Its ring holds, when full,
//   the reserve (bufferMs): never touched while the machine keeps pace; what a stall of its threads may take
//   + the reader's largest block (setReadSize), or the machine's stretch (stretchMs) if that is longer: with
//     small blocks the machine does not start for each one (its threads would sleep and wake thousands of times a
//     second: with 64-frame blocks the slowest 1 % of its chunks ran at half the speed they had with 512, measured
//     on a busy computer), it lets a stretch's worth go and makes it up in one run.
// That sum (and a tenth of a millisecond: the DSPs run a few frames ahead of the ColdFire) is latencyFrames(): the
// machine is never further ahead of the reader, which is what makes MIDI and the inputs land at a fixed time.
//
// The audio is the G2's own: 96 kHz, 4 words per frame as Machine::run() gives them (outputs 1, 3, 2, 4). Resampling
// to the host's rate is the caller's job. The audio inputs go the other way (write()), on the reader's timeline.
#pragma once

#include "g2emu/machine.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace g2emu {

// The audio inputs on their way to the ADCs (Runner::write): frames of 4 floats written by the audio thread at the
// reader's place in the output ring, taken by the thread that runs A6, each `latency` frames after the DACs send
// the output frame it was written with. One writer, one taker, no locks.
class InputQueue final : public AdcSource {
public:
    // `capacity` frames; `maxWrite`: the most frames one write() brings.
    InputQueue(std::size_t capacity, std::size_t maxWrite);
    // The audio thread: `frames` frames for the ring frames from `at` on (1.0: the ADCs' full scale, in their order).
    // A write that does not follow the one before starts over: nothing is taken from before it.
    void write(std::uint64_t at, const float* in, std::size_t frames);
    // The runner's thread: when the DACs send ring frame 0, in DSP clocks (it may be negative).
    void setDacClock(std::int64_t clock) { dacClock_.store(clock, std::memory_order_release); }
    void setLatency(std::uint32_t frames) { latency_.store(frames, std::memory_order_relaxed); }
    std::uint32_t latency() const { return latency_.load(std::memory_order_relaxed); }
    // Frames the ADCs took as silence because their input was not written yet (a latency too short).
    std::uint64_t missing() const { return missing_.load(std::memory_order_relaxed); }
    // A6's thread: the frame sampled at `clock`, as 24-bit words clipped at full scale; silence while there is none.
    void adcFrame(std::uint64_t clock, std::int32_t words[4]) override;

private:
    static constexpr std::int64_t NoClock = INT64_MIN;
    std::vector<float> ring_;
    std::size_t capacity_, maxWrite_;
    std::atomic<std::uint64_t> start_{0}, end_{0};  // the ring frames written, without a gap
    std::atomic<std::int64_t> dacClock_{NoClock};
    std::atomic<std::uint32_t> latency_{0};
    std::atomic<std::uint64_t> missing_{0};
};

class Runner {
public:
    // What one chunk of the machine's thread took, for measurements (Options::onChunk; tools/audiopath).
    struct Chunk {
        std::chrono::steady_clock::time_point start, end;
        std::uint32_t frames = 0;
        bool woke = false;  // the thread had slept before it (the buffer was full), until a read() woke it
    };

    struct Options {
        Machine::Options machine = defaultMachineOptions();
        double bufferMs = 20;      // the reserve: audio kept ready ahead of the reader's next block (setBufferMs)
        double stretchMs = 0;      // the least the machine makes in one run before it rests (0: the reader's block)
        std::uint32_t chunkFrames = 96;  // frames the machine runs at a time at most (1 ms)
        // the memory at power-on, flash then EEPROM (Machine::loadMemory; empty: as the machine starts it)
        std::vector<std::uint8_t> flash;
        // called on the machine's thread after every chunk (none: nothing; for measurements only)
        std::function<void(const Chunk&)> onChunk;
        static Machine::Options defaultMachineOptions()
        {
            Machine::Options o;
            o.threads = -1;  // the DSPs on threads of their own: two where the computer has the cores (Machine::autoThreads)
            return o;
        }
    };

    struct Stats {
        std::uint64_t framesRead = 0, framesMissing = 0;  // the reader asked for frames that were not there yet
        std::uint64_t shortReads = 0;  // read() calls that came short (dropouts, as the listener counts them)
        // frames the ADCs took as silence because their input had not been written yet (while write() is in use)
        std::uint64_t inputMissing = 0;
        double speed = 0;  // emulated seconds per wall second while the machine runs (its rests left out), recently
        double longestChunkMs = 0;  // the longest wall time one chunk took (a stall of the machine's threads shows here)
        // the machine's thread has the computer's real-time priority (Machine::Options::realtimeThreads asked for it
        // and the system gave it: macOS only)
        bool realtime = false;
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
    // The audio inputs IN 1-4, from the audio thread before its read(), real-time safe: `frames` frames of 4 floats
    // in the ADCs' order (inputs 1, 3, 2, 4, as read() gives the outputs), 1.0 = the ADCs' full scale (they clip
    // there), for the stretch of time the next read() returns: call both with the same count (at most MaxReadSize).
    // The frames reach the ADCs inputLatencyFrames() after the DACs send the frames read with them, always the
    // same: what the G2 makes of an input frame comes out of read() that many frames later, plus the G2's own way
    // from its ADCs to its DACs. Without write() the inputs are silent.
    void write(const float* in, std::size_t frames) { input_.write(read_.load(std::memory_order_relaxed), in, frames); }
    std::uint32_t inputLatencyFrames() const { return input_.latency(); }

    // Stops the machine's thread (for good). Then machine() may be used from the caller's thread, e.g. to save
    // machine().memorySnapshot(). The destructor stops it too.
    void stop();
    // The most frames one read() takes (a host's block, in 96 kHz frames): the machine then keeps that much more
    // ready, so that after the reader's largest take bufferMs are still left. Part of latencyFrames(). Any thread.
    static constexpr std::uint32_t MaxReadSize = 16384;  // 170 ms
    void setReadSize(std::uint32_t frames)
    {
        readSize_.store(std::min(frames, MaxReadSize), std::memory_order_relaxed);
        input_.setLatency(latencyFrames() + inputLead());
    }
    // The reserve, while it runs (any thread; up to MaxBufferMs): latencyFrames() changes with it, and with it when
    // the MIDI and the inputs written from now on land (what is on its way keeps its place: a step in time, once).
    static constexpr double MaxBufferMs = 100;
    void setBufferMs(double ms)
    {
        buffer_.store(std::uint32_t(std::clamp(ms, 0.0, MaxBufferMs) * Machine::FrameRate / 1000.0), std::memory_order_relaxed);
        input_.setLatency(latencyFrames() + inputLead());
    }
    double bufferMs() const { return buffer_.load(std::memory_order_relaxed) * 1000.0 / Machine::FrameRate; }

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
    // The DACs may have sent this many frames more than the machine was asked for: the DSPs run up to dspLead ahead
    // of the ColdFire, and a run's frames come out on the DSPs' serial clock, one more or less. Part of
    // latencyFrames(): 10 frames, 0.1 ms.
    std::uint32_t dacLead() const { return options_.machine.dspLead / Machine::ClocksPerFrame + 2; }
    // The ADCs take their frames this much later than latencyFrames(): A6 may run that far ahead of the frames the
    // machine was asked for (Machine::Options::dspLead), and its input must have been written by then.
    std::uint32_t inputLead() const { return options_.machine.dspLead / Machine::ClocksPerFrame + 8; }

    Options options_;
    InputQueue input_;  // before the machine, whose threads use it to their end
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
    std::atomic<bool> quit_{false}, realtime_{false};
    std::atomic<std::uint32_t> readSize_{0}, buffer_{0};
    std::uint32_t stretch_ = 0;  // frames
    std::atomic<std::uint64_t> shortReads_{0};
    // bumped by every read(): the machine's thread sleeps on it while the buffer is full (no polling: under load a
    // short sleep could overrun by milliseconds and leave the reader short)
    std::atomic<std::uint32_t> consumed_{0};
    struct Thread;
    std::unique_ptr<Thread> thread_;
};

} // namespace g2emu
