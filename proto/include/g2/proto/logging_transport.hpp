// A transport that writes everything going through another one to a text
// file: frames sent, interrupt packets and bulk transfers received, device
// arrival and removal, with the time and the bytes in hex (and the header,
// session and first molecule id of complete messages). For checking the
// protocol against a real G2: a tester sends us the file.
//
// The file is kept small: past `maxBytes` it is renamed to "<path>.1"
// (replacing the previous one) and a new one is started.
//
// Privacy: by default the log leaves out what testers may not want to share:
// patch and performance contents (sections, names, notes, module names and
// labels, synth and performance settings with their names, flash bank name
// lists and files). Each such molecule is written as its id, its length and a
// 32-bit FNV-1a fingerprint ("<4D 812 bytes #1a2b3c4d>"), enough to see what
// was exchanged and whether two copies are equal. Everything else (framing,
// sessions, requests, acks, errors, parameter changes, LEDs, meters) is
// written in full. LogData::Full writes everything.
#pragma once

#include "g2/proto/transport.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>

namespace g2::proto {

enum class LogData : std::uint8_t { Redacted, Full };

// Whether a molecule id carries patch, performance or bank content.
bool isPrivateMolecule(std::uint8_t id);

class LoggingTransport final : public Transport, private Transport::Sink {
public:
    LoggingTransport(std::unique_ptr<Transport> inner, std::string path, LogData data = LogData::Redacted,
                     std::uint64_t maxBytes = 5u << 20);
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

    void line(const char* tag, const std::string& bytes, const std::string& detail = {});
    // The bytes of a frame or message in hex, private molecules (from
    // bodyStart to the 2-byte CRC) redacted unless LogData::Full.
    std::string render(std::span<const std::uint8_t> bytes, std::size_t bodyStart) const;
    void open();

    std::unique_ptr<Transport> inner_;
    std::string path_;
    LogData data_;
    std::uint64_t maxBytes_;
    std::ofstream out_;
    std::uint64_t written_ = 0;
};

// The log's usual place: ~/Library/Logs/G2fresh/usb.log (macOS),
// %LOCALAPPDATA%\G2fresh\usb.log (Windows), $XDG_STATE_HOME/G2fresh/usb.log or
// ~/.local/state/G2fresh/usb.log (Linux).
std::string defaultUsbLogPath();

} // namespace g2::proto
