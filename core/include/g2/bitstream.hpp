// MSB-first bit packing used by the G2 patch format and USB messages.
// Byte and word accessors require byte alignment, like Clavia's CBitStream
// (GetUByte/PutUByte align first; callers make the padding explicit).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace g2 {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    // Reads `count` bits (0..32), most significant bit first.
    std::uint32_t read(unsigned count);
    // Reads a two's-complement signed field.
    std::int32_t readSigned(unsigned count);
    // Number of bits up to the next byte boundary (0..7).
    unsigned padBits() const { return (8 - (bitPos_ & 7)) & 7; }
    // Skips to the next byte boundary and returns the skipped bits.
    std::uint32_t align() { return read(padBits()); }
    std::uint8_t u8();
    std::uint16_t u16();
    // CBitStream::GetString: bytes up to a NUL (consumed) or `maxLen` bytes.
    std::string string(std::size_t maxLen);

    bool atEnd() const { return bitPos_ >= data_.size() * 8; }
    std::size_t bitsLeft() const { return data_.size() * 8 - bitPos_; }
    std::size_t bitPosition() const { return bitPos_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t bitPos_ = 0;
};

class BitWriter {
public:
    void write(std::uint32_t value, unsigned count);
    void writeSigned(std::int32_t value, unsigned count);
    unsigned padBits() const { return (8 - (bitPos_ & 7)) & 7; }
    // Pads to the next byte boundary with the given bits (normally zero).
    void align(std::uint32_t padValue = 0) { write(padValue, padBits()); }
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void bytes(std::span<const std::uint8_t> data);
    // CBitStream::PutString: the bytes, plus a NUL if shorter than `maxLen`.
    void string(const std::string& s, std::size_t maxLen);

    const std::vector<std::uint8_t>& data() const { return bytes_; }
    std::size_t bitPosition() const { return bitPos_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bitPos_ = 0;
};

} // namespace g2
