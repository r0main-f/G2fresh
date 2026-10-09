// The link between a Client and a G2: bulk-OUT frames one way, interrupt
// packets and bulk-IN transfers the other (re/notes/usb-protocol.md §1.3, §2).
// A libusb implementation reads the bulk-IN transfer an extended interrupt
// packet announces before delivering the next packet; the Client relies on
// that order.
//
// Everything is single-threaded: a transport delivers incoming traffic only
// from poll(), which the Client calls from its tick().
#pragma once

#include <cstdint>
#include <span>

namespace g2::proto {

class Transport {
public:
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual void deviceArrived() = 0;
        virtual void deviceRemoved() = 0;
        virtual void interruptPacket(std::span<const std::uint8_t> packet) = 0; // 16 bytes
        virtual void bulkIn(std::span<const std::uint8_t> data) = 0;            // one transfer
    };

    virtual ~Transport() = default;
    void setSink(Sink* sink) { sink_ = sink; }
    // One bulk-OUT transfer. Returns false if it could not be written (no
    // device); Clavia ignores that and lets the reply timeout handle it.
    virtual bool send(std::span<const std::uint8_t> frame) = 0;
    // Delivers pending device events and traffic to the sink.
    virtual void poll() = 0;

protected:
    Sink* sink_ = nullptr;
};

// Milliseconds, injectable so that timeouts are testable.
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::uint64_t nowMs() const = 0;
};

// std::chrono::steady_clock.
class SteadyClock : public Clock {
public:
    std::uint64_t nowMs() const override;
};

// A clock tests move by hand.
class ManualClock : public Clock {
public:
    std::uint64_t nowMs() const override { return now_; }
    void advance(std::uint64_t ms) { now_ += ms; }
    void set(std::uint64_t ms) { now_ = ms; }

private:
    std::uint64_t now_ = 0;
};

} // namespace g2::proto
