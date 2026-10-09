#include "g2/crc.hpp"

namespace g2 {

std::uint16_t crc16(std::span<const std::uint8_t> data, std::uint16_t seed)
{
    std::uint16_t crc = seed;
    for (std::uint8_t b : data) {
        crc ^= static_cast<std::uint16_t>(b) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<std::uint16_t>(crc << 1);
    }
    return crc;
}

} // namespace g2
