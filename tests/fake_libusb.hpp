// A stand-in for libusb, linked into g2usbtests instead of the real library, so
// that g2::usb::LibusbTransport (usb/src/libusb_transport.cpp, compiled in as it
// is) runs without hardware against one scripted G2: the test queues its
// interrupt packets and how each bulk-IN read completes.
//
// Like libusb, the fake completes transfers only inside
// libusb_handle_events_timeout_completed (the transport's I/O thread); a
// transfer with nothing queued for it completes TIMED_OUT at its timeout.
//
// Time is the test's: now() is the clock to give the transport, and it moves
// only in advance(). Every timeout here (a transfer's, the one of
// libusb_handle_events_timeout_completed) is counted on it, so nothing depends
// on how fast the machine is.
//
// The I/O thread is idle when it waits in libusb_handle_events_timeout_completed
// with nothing to do at the present time. Every function of the script waits
// for that before it acts (and stats() before it reads), so the thread gets the
// script's events one at a time and in order, and what it does is the same on
// every run. advance() moves the time from one moment the thread has something
// to do (a timeout) to the next, waiting for it to be idle at each. These waits
// are the only ones in real time: after kGuard without the thread going idle
// they throw, which fails the test.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace fakeusb {

using Bytes = std::vector<std::uint8_t>;

// Before each test, before the transport is made: no G2, nothing queued.
void reset(bool hotplug);

// The clock of the transport under test (a g2::usb::LibusbClock).
std::chrono::steady_clock::time_point now();
// Waits until the I/O thread is idle: what was scripted has had its effects,
// and the transport's queue holds them for poll().
void settle();
// Moves the time, then waits as settle() does.
void advance(std::chrono::milliseconds by);
inline constexpr std::chrono::seconds kGuard{10};

void plug();
// Pending transfers fail with NO_DEVICE; with hotplug the departure is told
// first, in a libusb_handle_events_timeout_completed of its own.
void unplug();
// A hotplug arrival for the G2 already there (no departure before it).
void arrivalAgain();
// The next `count` libusb_open fail (no access).
void openFails(int count);

// The next interrupt-IN completion.
void interrupt(Bytes packet);
// The next bulk-IN read completes with these bytes; empty: a zero-length packet.
void bulkIn(Bytes data);
enum class Fail { Error, Stall };
// The next bulk-IN read fails this way.
void bulkInFails(Fail how);

// A cancelled transfer completes this long after libusb_cancel_transfer (at
// once after reset()).
void cancelTakes(std::chrono::milliseconds delay);
// The next libusb_cancel_transfer returns only once the transport is being
// destroyed (libusb_interrupt_event_handler): a transport destroyed while it
// closes its device. The I/O thread held there counts as idle.
void holdCancelUntilStopped();

struct Stats {
    std::vector<std::string> calls; // "open", "reset", "claim", "close"
    int scans = 0;                  // device lists asked for
    int bulkReads = 0;              // bulk-IN transfers submitted
    unsigned readTimeout = 0;       // of the last one
    unsigned sendTimeout = 0;       // of the last bulk-OUT write
    std::vector<Bytes> sent;        // bulk-OUT frames
    int cancelled = 0;              // transfers completed CANCELLED
    int transfers = 0;              // allocated and not freed
    // Once the transport is being destroyed nobody moves the time, so a
    // libusb_handle_events_timeout_completed with nothing to do moves it
    // itself to its next timeout. How many did: each is a wait in real life.
    int waitsWhileStopping = 0;
};
Stats stats();

} // namespace fakeusb
