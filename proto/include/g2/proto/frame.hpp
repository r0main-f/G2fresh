// Framing of the G2's USB messages (re/notes/usb-protocol.md §2, §3).
//
// Host -> device: one bulk-OUT transfer per frame
//     [len16 BE] [01] [hdr] [session] molecules... [crc16 BE]
// with the CRC (CRC-16/XMODEM) over everything but the length; the version
// request is the special frame 00 05 80 91 88.
//
// Device -> host: every message is announced by a 16-byte interrupt packet.
// A message of at most 15 bytes travels inside the packet ("embedded",
// byte0 = n<<4 | 2); a longer one is announced ("extended", byte0 & 3 == 1,
// bytes 1..2 = its length) and read as one bulk-IN transfer. A message is
//     [01] [hdr] [session] molecules... [crc16 BE]   or   [80] version info... [crc16 BE]
// with the CRC over everything before it.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace g2::proto {

using u8 = std::uint8_t;

// Header bits (CSynthPortUSB constants, __const 0x1E5B5F).
inline constexpr u8 kBubbleBegin = 0x20; // out: first message of a bubble
inline constexpr u8 kRealtime = 0x10;    // out: realtime (never answered)
inline constexpr u8 kBubbleEnd = 0x08;   // out: last message of a bubble
inline constexpr u8 kResponse = 0x08;    // in: the answer to the outstanding request
inline constexpr u8 kSlotMask = 0x07;
inline constexpr u8 kVoidSession = 0x40; // session byte: 0x40 | molecule count
inline constexpr u8 kSessionMask = 0x3F;

// Slot field values: 0..3 = patch slots A..D, 4 = synth / performance level.
inline constexpr u8 kSlotSynth = 4;

inline constexpr std::size_t kInterruptPacketSize = 16;
inline constexpr std::size_t kMaxEmbedded = 15; // message bytes that fit in an interrupt packet
inline constexpr std::size_t kMaxFrame = 0xFFFF;

// ---- Host -> device ------------------------------------------------------------

std::vector<u8> versionRequest(); // 00 05 80 91 88
// A bulk-OUT frame. Throws std::length_error above kMaxFrame bytes.
std::vector<u8> hostFrame(u8 hdr, u8 session, std::span<const u8> molecules);

struct HostFrame {
    bool versionRequest = false;
    u8 hdr = 0, session = 0;
    std::vector<u8> molecules;

    u8 slot() const { return hdr & kSlotMask; }
    bool realtime() const { return (hdr & kRealtime) != 0; }
    bool voidSession() const { return (session & kVoidSession) != 0; }
};
// The device's reading of a bulk-OUT frame; nullopt if the length or CRC is wrong.
std::optional<HostFrame> parseHostFrame(std::span<const u8> frame);

// ---- Device -> host ------------------------------------------------------------

// A message [01 hdr session molecules crc].
std::vector<u8> deviceMessage(u8 hdr, u8 session, std::span<const u8> molecules);
// The version reply [80 info crc]; `info` excludes the 0x80.
std::vector<u8> versionMessage(std::span<const u8> info);

struct DeviceMessage {
    enum class Kind : u8 { Normal, Version };
    Kind kind = Kind::Normal;
    u8 hdr = 0, session = 0;
    std::vector<u8> body; // Normal: the molecules; Version: the bytes after 0x80

    u8 slot() const { return hdr & kSlotMask; }
    bool isResponse() const { return (hdr & kResponse) != 0; }
    bool voidSession() const { return (session & kVoidSession) != 0; }
    // LED / meter data (molecule id 0x39 / 0x3A, §10), handled apart from requests.
    bool isBlink() const { return kind == Kind::Normal && !body.empty() && (body[0] == 0x39 || body[0] == 0x3A); }
};
// nullopt if the message is too short or its CRC is wrong (Clavia drops those
// silently, CSynthPortUSB::InterruptHandleData 0x106f72).
std::optional<DeviceMessage> parseDeviceMessage(std::span<const u8> message);

// An interrupt packet (CSynthPortUSB::USBDataReceivedCallback 0x107194).
struct Interrupt {
    enum class Kind : u8 {
        Embedded, // `message` holds the message
        Extended, // `length` bytes follow on bulk-IN
        Ignored,  // byte0 & 3 == 3, a short packet, or an inline message that does not fit
    };
    Kind kind = Kind::Ignored;
    std::vector<u8> message;
    std::size_t length = 0;
};
// byte0 & 3: 2 embedded, 1 extended, 0 "inline-long" (length in bytes 1..2,
// message from byte 3; never observed, accepted when it fits), 3 ignored.
Interrupt parseInterrupt(std::span<const u8> packet);

// ---- Version handshake (§5.2) --------------------------------------------------

// The version reply's fields (offsets from the 0x80: CSynthPort::CheckVersionMessage
// 0x104d2c, CSynthInfo 0x103e56). The real reply's length is unknown (at least
// 34 bytes); encode() writes 35 bytes after the 0x80 [I].
struct VersionInfo {
    enum Model : u8 { G2 = 0, G2X = 1, Rack = 2, Engine = 3, Native = 4 };
    u8 marker = 0x0A;                  // offset 1, must be 0x0A
    u8 model = G2;                     // offset 2
    u8 mode = 0;                       // offset 3: 0 normal, 1 OS update mode
    std::uint16_t unknown4 = 0;        // offsets 4..5
    std::uint16_t firmware = 150;      // offsets 6..7: version x 100 ("V1.50")
    std::uint16_t protocol = 0x0012;   // offsets 8..9: must be 0x0012
    std::uint32_t unknown26 = 0;       // offsets 26..29 (a serial number? [I])
    std::uint16_t unknown32 = 0;       // offsets 32..33

    // Accepted by the editor (IsSynthAccepted 0x1038b8): G2, G2X or Engine,
    // normal mode, protocol 0x0012.
    bool supportedModel() const { return marker == 0x0A && (model == G2 || model == G2X || model == Engine); }
    const char* modelName() const;     // "G2", "G2X", "G2 Engine", ...
    bool operator==(const VersionInfo&) const = default;
};
std::vector<u8> encode(const VersionInfo& v); // the bytes after the 0x80
// nullopt when shorter than the fields the editor reads (offset 33).
std::optional<VersionInfo> decodeVersionInfo(std::span<const u8> info);

// How a device sends one message: the interrupt packet, and the bulk-IN
// transfer for an extended message (empty when embedded).
struct Delivery {
    std::array<u8, kInterruptPacketSize> interrupt{};
    std::vector<u8> bulk;
};
// Messages of at most 15 bytes are embedded unless `forceExtended`. The
// extended announcement's high nibble is 0 (both editors ignore it) [I].
Delivery deliver(std::span<const u8> message, bool forceExtended = false);

} // namespace g2::proto
