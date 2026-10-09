#include "g2/bitstream.hpp"

namespace g2 {

std::uint32_t BitReader::read(unsigned count)
{
    if (count > 32)
        throw std::invalid_argument("BitReader::read: count > 32");
    if (bitPos_ + count > data_.size() * 8)
        throw FormatError("unexpected end of bit stream");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i, ++bitPos_) {
        const std::uint8_t byte = data_[bitPos_ >> 3];
        value = (value << 1) | ((byte >> (7 - (bitPos_ & 7))) & 1u);
    }
    return value;
}

void BitWriter::write(std::uint32_t value, unsigned count)
{
    if (count > 32)
        throw std::invalid_argument("BitWriter::write: count > 32");
    for (unsigned i = count; i-- > 0; ++bitPos_) {
        if ((bitPos_ & 7) == 0)
            bytes_.push_back(0);
        if ((value >> i) & 1u)
            bytes_.back() |= static_cast<std::uint8_t>(0x80u >> (bitPos_ & 7));
    }
}

void BitWriter::align()
{
    bitPos_ = (bitPos_ + 7) & ~std::size_t{7};
}

} // namespace g2
