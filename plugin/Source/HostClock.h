// The host's tempo and transport as MIDI clock for the emulated G2's MIDI IN, so that its master clock (the
// arpeggiator, clocked LFOs and delays) follows the song: 24 clock ticks (F8) per quarter note at the exact samples
// where the host's beat position crosses them, Start (FA) or Song Position + Continue (F2, FB) when the transport
// starts, Stop (FC) when it stops, and Stop, Song Position, Continue when the position jumps (a loop). While the
// transport is stopped the ticks go on at the host's tempo, so the G2's tempo stays the host's.
//
// Plain C++ (no JUCE), for the audio thread: no allocation, no locks.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

class HostClock {
public:
    struct Position {
        double bpm = 0;              // the host's tempo (0: unknown, no clock)
        bool playing = false;
        std::optional<double> ppq;   // the beat position at the block's first sample (quarter notes)
    };

    // Calls emit(sampleOffset, status, data1, data2, size) for each message falling in this block of `samples`.
    template <typename Emit>
    void process(const Position& pos, double sampleRate, int samples, Emit&& emit)
    {
        if (pos.bpm <= 0 || sampleRate <= 0 || samples <= 0)
            return;
        const double samplesPerQuarter = sampleRate * 60.0 / pos.bpm;
        const double samplesPerTick = samplesPerQuarter / 24.0;
        const bool following = pos.playing && pos.ppq.has_value();

        if (following) {
            const double ppq = *pos.ppq;
            const bool jumped = wasPlaying_ && std::abs(ppq - expectedPpq_) > 0.5 / 24.0;
            if (!wasPlaying_ || jumped) {
                lastTick_ = static_cast<std::int64_t>(std::ceil(ppq * 24.0 - 1e-9)) - 1;
                if (jumped)
                    emit(0, std::uint8_t(0xFC), std::uint8_t(0), std::uint8_t(0), 1);
                if (!jumped && ppq < 0.5 / 24.0) {
                    emit(0, std::uint8_t(0xFA), std::uint8_t(0), std::uint8_t(0), 1); // Start, from the top
                } else {
                    // Song Position (in sixteenth notes) then Continue: the next tick is the first one after it
                    const auto sixteenths = static_cast<int>(std::floor(ppq * 4.0 + 1e-9));
                    emit(0, std::uint8_t(0xF2), std::uint8_t(sixteenths & 0x7f), std::uint8_t((sixteenths >> 7) & 0x7f), 3);
                    emit(0, std::uint8_t(0xFB), std::uint8_t(0), std::uint8_t(0), 1);
                }
            }
            // ticks where ppq * 24 crosses a whole number in this block, each once: a tick right on a block boundary
            // may round into the end of one block and the start of the next
            for (auto tick = std::max(lastTick_ + 1, static_cast<std::int64_t>(std::ceil(ppq * 24.0 - 1e-9)));; ++tick) {
                const double at = (static_cast<double>(tick) / 24.0 - ppq) * samplesPerQuarter;
                if (at >= samples)
                    break;
                emit(std::max(0, static_cast<int>(at)), std::uint8_t(0xF8), std::uint8_t(0), std::uint8_t(0), 1);
                lastTick_ = tick;
            }
            expectedPpq_ = ppq + samples / samplesPerQuarter;
            phase_ = 0.0;
            wasPlaying_ = true;
            return;
        }

        if (wasPlaying_)
            emit(0, std::uint8_t(0xFC), std::uint8_t(0), std::uint8_t(0), 1); // Stop
        wasPlaying_ = false;
        // stopped: ticks at the host's tempo, from where the last one was
        double at = (1.0 - phase_) * samplesPerTick;
        if (phase_ == 0.0)
            at = 0.0;
        for (; at < samples; at += samplesPerTick)
            emit(static_cast<int>(at), std::uint8_t(0xF8), std::uint8_t(0), std::uint8_t(0), 1);
        // the fraction of a tick done at the block's end
        phase_ = 1.0 - (at - samples) / samplesPerTick;
        if (phase_ >= 1.0 || phase_ < 0.0)
            phase_ = 0.0;
    }

    void reset()
    {
        wasPlaying_ = false;
        lastTick_ = -1;
        expectedPpq_ = 0.0;
        phase_ = 0.0;
    }

private:
    bool wasPlaying_ = false;
    double expectedPpq_ = 0.0; // where the next block should start while playing (a jump means a loop)
    std::int64_t lastTick_ = -1; // the last tick sent while playing (ppq * 24)
    double phase_ = 0.0;       // stopped: the fraction of a tick done since the last one
};
