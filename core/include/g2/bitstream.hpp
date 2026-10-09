// MSB-first bit packing used by the G2 patch format and USB messages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace g2 {

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    // Reads `count` bits (0..32), most significant bit first.
    std::uint32_t read(unsigned count);
    bool atEnd() const { return bitPos_ >= data_.size() * 8; }
    std::size_t bitPosition() const { return bitPos_; }
    // Skips to the next byte boundary.
    void align() { bitPos_ = (bitPos_ + 7) & ~std::size_t{7}; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t bitPos_ = 0;
};

class BitWriter {
public:
    void write(std::uint32_t value, unsigned count);
    // Pads with zero bits to the next byte boundary.
    void align();
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::size_t bitPosition() const { return bitPos_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bitPos_ = 0;
};

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace g2
