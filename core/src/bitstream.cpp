#include "g2/bitstream.hpp"

namespace g2 {

std::uint32_t BitReader::read(unsigned count)
{
    if (count > 32)
        throw std::invalid_argument("BitReader::read: count > 32");
    if (count > bitsLeft())
        throw FormatError("unexpected end of data");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i, ++bitPos_) {
        const std::uint8_t byte = data_[bitPos_ >> 3];
        value = (value << 1) | ((byte >> (7 - (bitPos_ & 7))) & 1u);
    }
    return value;
}

std::int32_t BitReader::readSigned(unsigned count)
{
    const std::uint32_t v = read(count);
    if (count > 0 && count < 32 && (v & (1u << (count - 1))))
        return static_cast<std::int32_t>(v) - (std::int32_t{1} << count);
    return static_cast<std::int32_t>(v);
}

std::uint8_t BitReader::u8()
{
    if (bitPos_ & 7)
        throw FormatError("unaligned byte read");
    return static_cast<std::uint8_t>(read(8));
}

std::uint16_t BitReader::u16()
{
    const std::uint16_t hi = u8();
    return static_cast<std::uint16_t>((hi << 8) | u8());
}

std::string BitReader::string(std::size_t maxLen)
{
    std::string s;
    while (s.size() < maxLen) {
        const char c = static_cast<char>(u8());
        if (c == '\0')
            break;
        s.push_back(c);
    }
    return s;
}

void BitWriter::write(std::uint32_t value, unsigned count)
{
    if (count > 32)
        throw std::invalid_argument("BitWriter::write: count > 32");
    if (count < 32 && (value >> count) != 0)
        throw std::invalid_argument("BitWriter::write: value does not fit");
    for (unsigned i = count; i-- > 0; ++bitPos_) {
        if ((bitPos_ & 7) == 0)
            bytes_.push_back(0);
        if ((value >> i) & 1u)
            bytes_.back() |= static_cast<std::uint8_t>(0x80u >> (bitPos_ & 7));
    }
}

void BitWriter::writeSigned(std::int32_t value, unsigned count)
{
    if (count == 0 || count > 32)
        throw std::invalid_argument("BitWriter::writeSigned: bad count");
    if (count < 32) {
        const std::int32_t lo = -(std::int32_t{1} << (count - 1));
        const std::int32_t hi = (std::int32_t{1} << (count - 1)) - 1;
        if (value < lo || value > hi)
            throw std::invalid_argument("BitWriter::writeSigned: value does not fit");
        write(static_cast<std::uint32_t>(value) & ((1u << count) - 1), count);
    } else {
        write(static_cast<std::uint32_t>(value), 32);
    }
}

void BitWriter::u8(std::uint8_t value)
{
    if (bitPos_ & 7)
        throw std::logic_error("unaligned byte write");
    write(value, 8);
}

void BitWriter::u16(std::uint16_t value)
{
    u8(static_cast<std::uint8_t>(value >> 8));
    u8(static_cast<std::uint8_t>(value & 0xFF));
}

void BitWriter::bytes(std::span<const std::uint8_t> data)
{
    for (std::uint8_t b : data)
        write(b, 8);
}

void BitWriter::string(const std::string& s, std::size_t maxLen)
{
    if (s.size() > maxLen || s.find('\0') != std::string::npos)
        throw std::invalid_argument("BitWriter::string: bad string");
    for (char c : s)
        u8(static_cast<std::uint8_t>(c));
    if (s.size() < maxLen)
        u8(0);
}

} // namespace g2
