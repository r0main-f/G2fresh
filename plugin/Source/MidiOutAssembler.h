// The emulated G2's MIDI OUT, from the bytes on its cable to the messages a host takes: channel messages (with
// running status, though OS 1.62 sends a status byte with every message), System Common messages, System Exclusive
// messages put together across audio blocks, and System Real Time bytes (MIDI clock, Start, Stop), which may come
// between the bytes of any other message and go out at once without disturbing it.
//
// A message is given the time of its last byte: when it is complete on the cable, as a MIDI interface receives it.
// A System Exclusive message cut short by another status byte, or longer than the room given, is dropped: a host
// never gets a part of a dump as a whole one.
//
// Plain C++ (no JUCE), for the audio thread: no allocation after the constructor, no locks.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class MidiOutAssembler {
public:
    // `maxSysex`: the longest System Exclusive message taken, F0 and F7 included. The G2's dump blocks are a few
    // hundred bytes each (its own receive buffers hold 512 [C 0x30002A04]).
    explicit MidiOutAssembler(std::size_t maxSysex = 4096) : maxSysex_(maxSysex) { sysex_.reserve(maxSysex); }

    // One byte from the cable at time `at`: calls emit(at, data, size) for the message it completes, if any.
    template <typename Emit>
    void push(std::uint8_t byte, int at, Emit&& emit)
    {
        if (byte >= 0xF8) { // real time: one byte, whatever else is on its way
            emit(at, &byte, std::size_t(1));
            return;
        }
        if (byte & 0x80) {
            const bool wasSysex = inSysex_;
            inSysex_ = false;
            pending_ = 0;
            count_ = 0;
            if (byte == 0xF7) { // the end of a System Exclusive message (alone: nothing)
                if (wasSysex && !tooLong_) {
                    sysex_.push_back(0xF7);
                    emit(at, sysex_.data(), sysex_.size());
                } else if (wasSysex) {
                    ++dropped_;
                }
                return;
            }
            if (wasSysex)
                ++dropped_; // cut short by this status byte
            if (byte == 0xF0) {
                inSysex_ = true;
                tooLong_ = false;
                sysex_.clear();
                sysex_.push_back(0xF0);
                running_ = 0;
                return;
            }
            if (byte >= 0xF0) { // System Common: ends running status
                running_ = 0;
                const int data = dataBytes(byte);
                if (data == 0)
                    emit(at, &byte, std::size_t(1)); // Tune Request
                else if (data > 0)
                    pending_ = byte;
                return; // F4, F5: undefined, nothing
            }
            running_ = pending_ = byte;
            return;
        }
        // a data byte
        if (inSysex_) {
            if (sysex_.size() + 2 <= maxSysex_) // room for it and the F7
                sysex_.push_back(byte);
            else
                tooLong_ = true;
            return;
        }
        if (pending_ == 0) {
            if (running_ == 0)
                return; // no status to go with it
            pending_ = running_;
        }
        message_[static_cast<std::size_t>(1 + count_++)] = byte;
        if (count_ == dataBytes(pending_)) {
            message_[0] = pending_;
            emit(at, message_.data(), static_cast<std::size_t>(1 + count_));
            pending_ = 0;
            count_ = 0;
        }
    }

    // System Exclusive messages dropped so far (cut short, or too long).
    std::uint64_t dropped() const { return dropped_; }
    // Forgets a message on its way.
    void reset()
    {
        inSysex_ = tooLong_ = false;
        pending_ = running_ = 0;
        count_ = 0;
    }

    // How many data bytes follow a status byte (0x80-0xF7); -1: none defined (F0 and F7 frame a System Exclusive
    // message, F4 and F5 are undefined).
    static int dataBytes(std::uint8_t status)
    {
        if (status < 0xF0)
            return (status & 0xE0) == 0xC0 ? 1 : 2; // Program Change and Channel Pressure: one
        switch (status) {
        case 0xF1: return 1; // MTC quarter frame
        case 0xF2: return 2; // Song Position
        case 0xF3: return 1; // Song Select
        case 0xF6: return 0; // Tune Request
        default: return -1;
        }
    }

private:
    std::size_t maxSysex_;
    std::vector<std::uint8_t> sysex_; // the message on its way, from its F0
    std::array<std::uint8_t, 3> message_{};
    std::uint8_t pending_ = 0; // the status of the channel or System Common message on its way (0: none)
    std::uint8_t running_ = 0; // running status (0: none)
    int count_ = 0;            // its data bytes so far
    bool inSysex_ = false, tooLong_ = false;
    std::uint64_t dropped_ = 0;
};
