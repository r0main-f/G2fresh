// A transport that writes everything going through another one to a text
// file: frames sent, interrupt packets and bulk transfers received, device
// arrival and removal, with the time and the bytes in hex (and the header,
// session and first molecule id of complete messages). For checking the
// protocol against a real G2: a tester sends us the file.
//
// The file is kept small: past `maxBytes` it is renamed to "<path>.1"
// (replacing the previous one) and a new one is started.
#pragma once

#include "g2/proto/transport.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>

namespace g2::proto {

class LoggingTransport final : public Transport, private Transport::Sink {
public:
    LoggingTransport(std::unique_ptr<Transport> inner, std::string path, std::uint64_t maxBytes = 5u << 20);
    ~LoggingTransport() override;

    bool send(std::span<const std::uint8_t> frame) override;
    void poll() override;

    // A free-form line (status changes, errors), timestamped.
    void note(const std::string& text);
    Transport& inner() { return *inner_; }
    const std::string& path() const { return path_; }

private:
    void deviceArrived() override;
    void deviceRemoved() override;
    void interruptPacket(std::span<const std::uint8_t> packet) override;
    void bulkIn(std::span<const std::uint8_t> data) override;

    void line(const char* tag, std::span<const std::uint8_t> bytes, const std::string& detail = {});
    void open();

    std::unique_ptr<Transport> inner_;
    std::string path_;
    std::uint64_t maxBytes_;
    std::ofstream out_;
    std::uint64_t written_ = 0;
};

// The log's usual place: ~/Library/Logs/G2fresh/usb.log (macOS),
// %LOCALAPPDATA%\G2fresh\usb.log (Windows), $XDG_STATE_HOME/G2fresh/usb.log or
// ~/.local/state/G2fresh/usb.log (Linux).
std::string defaultUsbLogPath();

} // namespace g2::proto
