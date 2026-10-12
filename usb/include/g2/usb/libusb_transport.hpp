// The G2's USB connection through libusb (re/notes/usb-protocol.md §1, §2,
// §11.3): VID 0x0FFC PID 0x0002 (G2, G2X and G2 Engine), the first
// configuration, interface 0, endpoints found by type and direction (expected
// 0x81 interrupt-IN, 0x82 bulk-IN, 0x03 bulk-OUT), no control requests.
//
// A background thread runs libusb's events: an interrupt-IN transfer is always
// pending; when a packet announces an extended message, the bulk-IN transfer
// is read before the next interrupt packet, so the client gets them in that
// order. Device arrival and removal come from libusb hotplug where it exists
// (macOS, Linux) and from scanning once a second elsewhere (Windows). Incoming
// traffic is queued and delivered only from poll(), on the client's thread.
//
// As Clavia's editor does (re/notes/usb-crosscheck.md H1, M2, M3):
// - the device is reset when it is opened (CMacUsbHandle::OpenDevice 0x107e9c),
//   except on Windows (see below);
// - a bulk-IN read that comes back empty, fails, or times out after 1 s, is
//   read again: a G2 sends one or two zero-length packets before most messages.
//   The first non-empty read is the message. After 5 s without one the G2 is
//   given up as Clavia destroys its port (CUSBManager::RxData 0x108994 /
//   0x108a7e): it is reported removed and not opened again until it comes
//   back (unplugged or switched off and on);
// - a bulk-OUT write may take 5 s (CUSBManager::TxData 0x1088c6).
// Not in Clavia: a read whose length differs from the announced one is noted
// in lastError(), and after two in a row the G2 is reported out of step; a
// frame above kMaxSendFrame is noted there (see kMaxSendFrame).
//
// Platform notes:
// - Windows: libusb needs the WinUSB driver bound to the G2 (install it once
//   with Zadig, choosing "WinUSB" for the "Nord Modular G2" device). Clavia's
//   own driver cannot be used through libusb. WinUSB cannot reset a device:
//   there is no reset at open.
// - Linux: the user needs access to the device (a udev rule granting it, e.g.
//   SUBSYSTEM=="usb", ATTRS{idVendor}=="0ffc", ATTRS{idProduct}=="0002", MODE="0666").
// - macOS: libusb resets a device by re-enumerating it, waiting for it to come
//   back (up to 10 s). Since macOS 10.11 the ResetDevice Clavia calls does
//   nothing (libusb, darwin_usb.c), so this goes further than the original
//   does there today; another editor relies on it with a real G2 (G2-Edit).
// - Only one process can claim the G2: the g2bridge process owns it and serves
//   every editor.
#pragma once

#include "g2/proto/transport.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace g2::usb {

inline constexpr std::uint16_t kVendorId = 0x0FFC;
inline constexpr std::uint16_t kProductId = 0x0002;

// Above this size a bulk-OUT frame is still sent, as Clavia sends any frame up
// to 0xFFFF bytes, but noted in lastError() (the USB log): one report from
// hardware has a G2 failing on a message of about 34 KB (one of ~30 440 bytes
// went through) and answering nothing until switched off and on, with Clavia's
// editor failing on the same file (re/notes/usb-crosscheck.md M4). The evidence
// is one file, and 44 of 1 097 user performances are larger, so G2fresh does
// as the original. Patches stay far below (the largest of 4 500: 19 359 bytes).
inline constexpr std::size_t kMaxSendFrame = 32768;

// Clavia's timings; tests give others too.
struct LibusbTimeouts {
    unsigned sendMs = 5000;      // one bulk-OUT write
    unsigned readMs = 1000;      // one bulk-IN read
    unsigned readTotalMs = 5000; // all the reads of one message
};

// Where the transport reads the time, for the deadline of a message's reads and
// for its scans: std::chrono::steady_clock when null. Tests give the clock
// their libusb counts its timeouts with.
using LibusbClock = std::chrono::steady_clock::time_point (*)();

class LibusbTransport final : public proto::Transport {
public:
    explicit LibusbTransport(LibusbTimeouts timeouts = {}, LibusbClock clock = nullptr);
    ~LibusbTransport() override;
    LibusbTransport(const LibusbTransport&) = delete;
    LibusbTransport& operator=(const LibusbTransport&) = delete;

    // libusb started (false: no USB at all; the transport stays silent).
    bool available() const;
    // What went wrong last ("" when fine), for the user and the USB log: the G2
    // could not be opened (an access or driver problem), it stopped answering,
    // a read did not match its announcement, a frame was too large to send.
    std::string lastError() const;

    // A frame above kMaxSendFrame is sent and noted in lastError().
    bool send(std::span<const std::uint8_t> frame) override;
    void poll() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace g2::usb
