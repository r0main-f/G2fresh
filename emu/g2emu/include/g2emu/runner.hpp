// The emulated G2 running by itself, in real time, for a sound engine: a thread of its own (with the big stack the
// DSPs' JIT needs) runs the Machine ahead of an audio thread that pulls the DAC output, keeping `bufferMs` of audio
// ready. MIDI goes in from any thread; our protocol client talks to the machine through MachineTransport (the
// machine's USB queues are thread-safe), e.g. a LocalLink built on it.
//
// The audio is the G2's own: 96 kHz, 4 words per frame as Machine::run() gives them (outputs 1, 3, 2, 4). Resampling
// to the host's rate is the caller's job. The audio inputs go the other way (write()), on the reader's timeline.
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
    struct Options {
        Machine::Options machine = defaultMachineOptions();
        double bufferMs = 20;      // audio kept ready ahead of the reader
        std::uint32_t chunkFrames = 96;  // frames the machine runs at a time (1 ms)
        // the memory at power-on, flash then EEPROM (Machine::loadMemory; empty: as the machine starts it)
        std::vector<std::uint8_t> flash;
        static Machine::Options defaultMachineOptions()
        {
            Machine::Options o;
            o.threads = -1;  // the DSPs on threads of their own: two where the computer has the cores (Machine::autoThreads)
            return o;
        }
    };

    struct Stats {
        std::uint64_t framesRead = 0, framesMissing = 0;  // the reader asked for frames that were not there yet
        // frames the ADCs took as silence because their input had not been written yet (while write() is in use)
        std::uint64_t inputMissing = 0;
        // MIDI OUT bytes that reached the reader after the frame they belong to (the machine was late: they are given
        // at the start of the next read()). Bytes nobody takes are lost in the machine (Machine::Stats::midiOutLost).
        std::uint64_t midiOutLate = 0;
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

    // Any thread.
    void midiIn(std::span<const std::uint8_t> bytes) { machine_->midiIn(bytes); }
    // Sample-accurate MIDI, from the audio thread before its read(): the bytes start on the G2's MIDI IN `offset`
    // frames (96 kHz) after the first frame the next read() returns, plus latencyFrames(). So the delay from a
    // block's MIDI event to its sound is the same for every event (plus the OS's own reaction). Real-time safe
    // (a lock-free queue of 2048 events of up to 16 bytes; longer messages take several: 32 kB, more than a
    // performance dump given in one block. A message that does not fit whole is dropped).
    void midiInAt(std::span<const std::uint8_t> bytes, std::uint32_t offset);
    std::uint32_t latencyFrames() const;
    // The G2's MIDI OUT, from the audio thread after its read(), real-time safe (a lock-free queue): the bytes the
    // G2 sent during the frames that read() returned, oldest first, up to out.size() (the rest at the next call);
    // returns how many. `offset`: the frame of that read() in which the byte's stop bit ended on the line, i.e. the
    // frame the DACs sent at that instant, plus midiOutLeadFrames(), always the same: a MIDI event and the sound
    // the G2 made with it stay together. Bytes from before that read() (the first call, or a machine that ran
    // late) come at offset 0.
    struct MidiOut {
        std::uint32_t offset = 0;
        std::uint8_t byte = 0;
    };
    std::size_t midiOut(std::span<MidiOut> out);
    // A byte is given this many frames after the frame the DACs sent as it ended: the DSPs may run that far ahead
    // of the ColdFire (Machine::Options::dspLead), whose UART then has not sent the bytes of those frames yet.
    std::uint32_t midiOutLeadFrames() const { return (options_.machine.threads ? options_.machine.dspLead / Machine::ClocksPerFrame : 0) + 2; }
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
    std::array<MidiEvent, 2048> midiRing_{};
    std::atomic<std::uint64_t> midiHead_{0}, midiTail_{0};
    // MIDI OUT on its way to the reader: each byte with the ring frame it belongs to. The machine's thread fills it
    // from the machine's own queue (which keeps what does not fit here); the reader empties it.
    struct MidiOutEvent {
        std::uint64_t frame = 0;
        std::uint8_t byte = 0;
    };
    std::array<MidiOutEvent, 4096> midiOutRing_{};  // 1.3 s of a line that never rests
    std::atomic<std::uint64_t> midiOutHead_{0}, midiOutTail_{0};
    std::atomic<std::uint64_t> midiOutLate_{0};
    std::size_t lastRead_ = 0;  // the reader's: the frames its last read() asked for
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
