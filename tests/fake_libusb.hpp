// A stand-in for libusb, linked into g2usbtests instead of the real library, so
// that g2::usb::LibusbTransport (usb/src/libusb_transport.cpp, compiled in as it
// is) runs without hardware against one scripted G2: the test queues its
// interrupt packets and how each bulk-IN read completes.
//
// Like libusb, the fake completes transfers only inside
// libusb_handle_events_timeout_completed (the transport's I/O thread); a
// transfer with nothing queued for it completes TIMED_OUT at its timeout.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fakeusb {

using Bytes = std::vector<std::uint8_t>;

// Before each test, before the transport is made: no G2, nothing queued.
void reset(bool hotplug);
void plug();
void unplug();
// A hotplug arrival for the G2 already there (no departure before it).
void arrivalAgain();

// The next interrupt-IN completion.
void interrupt(Bytes packet);
// The next bulk-IN read completes with these bytes; empty: a zero-length packet.
void bulkIn(Bytes data);
enum class Fail { Error, Stall };
// The next bulk-IN read fails this way.
void bulkInFails(Fail how);

struct Stats {
    std::vector<std::string> calls; // "open", "reset", "claim", "close"
    int bulkReads = 0;              // bulk-IN transfers submitted
    unsigned readTimeout = 0;       // of the last one
    unsigned sendTimeout = 0;       // of the last bulk-OUT write
    std::vector<Bytes> sent;        // bulk-OUT frames
};
Stats stats();

} // namespace fakeusb
