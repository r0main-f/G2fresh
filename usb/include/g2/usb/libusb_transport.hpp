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
// Platform notes:
// - Windows: libusb needs the WinUSB driver bound to the G2 (install it once
//   with Zadig, choosing "WinUSB" for the "Nord Modular G2" device). Clavia's
//   own driver cannot be used through libusb.
// - Linux: the user needs access to the device (a udev rule granting it, e.g.
//   SUBSYSTEM=="usb", ATTRS{idVendor}=="0ffc", ATTRS{idProduct}=="0002", MODE="0666").
// - Only one process can claim the G2: the g2bridge process owns it and serves
//   every editor.
#pragma once

#include "g2/proto/transport.hpp"

#include <memory>
#include <span>
#include <string>

namespace g2::usb {

inline constexpr std::uint16_t kVendorId = 0x0FFC;
inline constexpr std::uint16_t kProductId = 0x0002;

class LibusbTransport final : public proto::Transport {
public:
    LibusbTransport();
    ~LibusbTransport() override;
    LibusbTransport(const LibusbTransport&) = delete;
    LibusbTransport& operator=(const LibusbTransport&) = delete;

    // libusb started (false: no USB at all; the transport stays silent).
    bool available() const;
    // Why the G2 could not be opened last time ("" when fine), e.g. an access
    // or driver problem, for the user.
    std::string lastError() const;

    bool send(std::span<const std::uint8_t> frame) override;
    void poll() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace g2::usb
