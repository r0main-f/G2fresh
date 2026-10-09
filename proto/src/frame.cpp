#include "g2/proto/frame.hpp"

#include "g2/crc.hpp"

#include <stdexcept>

namespace g2::proto {
namespace {

void appendCrc(std::vector<u8>& out, std::span<const u8> covered)
{
    const std::uint16_t crc = g2::crc16(covered);
    out.push_back(static_cast<u8>(crc >> 8));
    out.push_back(static_cast<u8>(crc & 0xFF));
}

bool crcMatches(std::span<const u8> bytes)
{
    if (bytes.size() < 2)
        return false;
    const auto covered = bytes.first(bytes.size() - 2);
    const std::uint16_t stored = static_cast<std::uint16_t>((bytes[bytes.size() - 2] << 8) | bytes[bytes.size() - 1]);
    return g2::crc16(covered) == stored;
}

} // namespace

std::vector<u8> versionRequest()
{
    std::vector<u8> out{0x00, 0x05, 0x80};
    appendCrc(out, std::span<const u8>(out).subspan(2));
    return out;
}

std::vector<u8> hostFrame(u8 hdr, u8 session, std::span<const u8> molecules)
{
    const std::size_t length = 2 + 3 + molecules.size() + 2;
    if (length > kMaxFrame)
        throw std::length_error("USB frame longer than 0xFFFF bytes");
    std::vector<u8> out;
    out.reserve(length);
    out.push_back(static_cast<u8>(length >> 8));
    out.push_back(static_cast<u8>(length & 0xFF));
    out.push_back(0x01);
    out.push_back(hdr);
    out.push_back(session);
    out.insert(out.end(), molecules.begin(), molecules.end());
    appendCrc(out, std::span<const u8>(out).subspan(2));
    return out;
}

std::optional<HostFrame> parseHostFrame(std::span<const u8> frame)
{
    if (frame.size() < 5)
        return std::nullopt;
    const std::size_t length = static_cast<std::size_t>((frame[0] << 8) | frame[1]);
    if (length != frame.size() || !crcMatches(frame.subspan(2)))
        return std::nullopt;
    HostFrame f;
    if (frame[2] == 0x80) {
        f.versionRequest = true;
        return f;
    }
    if (frame.size() < 7)
        return std::nullopt;
    f.hdr = frame[3];
    f.session = frame[4];
    f.molecules.assign(frame.begin() + 5, frame.end() - 2);
    return f;
}

std::vector<u8> deviceMessage(u8 hdr, u8 session, std::span<const u8> molecules)
{
    std::vector<u8> out{0x01, hdr, session};
    out.insert(out.end(), molecules.begin(), molecules.end());
    appendCrc(out, out);
    return out;
}

std::vector<u8> versionMessage(std::span<const u8> info)
{
    std::vector<u8> out{0x80};
    out.insert(out.end(), info.begin(), info.end());
    appendCrc(out, out);
    return out;
}

std::optional<DeviceMessage> parseDeviceMessage(std::span<const u8> message)
{
    if (message.size() < 3 || !crcMatches(message))
        return std::nullopt;
    DeviceMessage m;
    if (message[0] == 0x80) {
        m.kind = DeviceMessage::Kind::Version;
        m.body.assign(message.begin() + 1, message.end() - 2);
        return m;
    }
    // Byte 0 is 0x01; Clavia does not check it.
    if (message.size() < 5)
        return std::nullopt;
    m.hdr = message[1];
    m.session = message[2];
    m.body.assign(message.begin() + 3, message.end() - 2);
    return m;
}

Interrupt parseInterrupt(std::span<const u8> packet)
{
    Interrupt i;
    if (packet.empty())
        return i;
    switch (packet[0] & 3) {
    case 2: {
        const std::size_t n = packet[0] >> 4;
        if (n == 0 || 1 + n > packet.size())
            return i;
        i.kind = Interrupt::Kind::Embedded;
        i.message.assign(packet.begin() + 1, packet.begin() + 1 + static_cast<std::ptrdiff_t>(n));
        return i;
    }
    case 1:
        if (packet.size() < 3)
            return i;
        i.kind = Interrupt::Kind::Extended;
        i.length = static_cast<std::size_t>((packet[1] << 8) | packet[2]);
        return i;
    case 0: {
        if (packet.size() < 3)
            return i;
        const std::size_t n = static_cast<std::size_t>((packet[1] << 8) | packet[2]);
        if (n == 0 || 3 + n > packet.size())
            return i;
        i.kind = Interrupt::Kind::Embedded;
        i.message.assign(packet.begin() + 3, packet.begin() + 3 + static_cast<std::ptrdiff_t>(n));
        return i;
    }
    default:
        return i;
    }
}

const char* VersionInfo::modelName() const
{
    switch (model) {
    case G2: return "G2";
    case G2X: return "G2X";
    case Rack: return "G2 Rack";
    case Engine: return "G2 Engine";
    case Native: return "G2 Native";
    default: return "unknown model";
    }
}

std::vector<u8> encode(const VersionInfo& v)
{
    std::vector<u8> info(35, 0); // offsets 1..35
    auto put16 = [&](std::size_t offset, std::uint16_t value) {
        info[offset - 1] = static_cast<u8>(value >> 8);
        info[offset] = static_cast<u8>(value & 0xFF);
    };
    info[0] = v.marker;
    info[1] = v.model;
    info[2] = v.mode;
    put16(4, v.unknown4);
    put16(6, v.firmware);
    put16(8, v.protocol);
    put16(26, static_cast<std::uint16_t>(v.unknown26 >> 16));
    put16(28, static_cast<std::uint16_t>(v.unknown26 & 0xFFFF));
    put16(32, v.unknown32);
    return info;
}

std::optional<VersionInfo> decodeVersionInfo(std::span<const u8> info)
{
    if (info.size() < 33)
        return std::nullopt;
    auto get16 = [&](std::size_t offset) {
        return static_cast<std::uint16_t>((info[offset - 1] << 8) | info[offset]);
    };
    VersionInfo v;
    v.marker = info[0];
    v.model = info[1];
    v.mode = info[2];
    v.unknown4 = get16(4);
    v.firmware = get16(6);
    v.protocol = get16(8);
    v.unknown26 = (std::uint32_t{get16(26)} << 16) | get16(28);
    v.unknown32 = get16(32);
    return v;
}

Delivery deliver(std::span<const u8> message, bool forceExtended)
{
    Delivery d;
    if (!forceExtended && !message.empty() && message.size() <= kMaxEmbedded) {
        d.interrupt[0] = static_cast<u8>((message.size() << 4) | 2);
        for (std::size_t i = 0; i < message.size(); ++i)
            d.interrupt[1 + i] = message[i];
        return d;
    }
    if (message.size() > 0xFFFF)
        throw std::length_error("USB message longer than 0xFFFF bytes");
    d.interrupt[0] = 0x01;
    d.interrupt[1] = static_cast<u8>(message.size() >> 8);
    d.interrupt[2] = static_cast<u8>(message.size() & 0xFF);
    d.bulk.assign(message.begin(), message.end());
    return d;
}

} // namespace g2::proto
