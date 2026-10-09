#pragma once

#include <cstdint>
#include <span>

namespace g2 {

// CRC-16/XMODEM (poly 0x1021, init 0, no reflection), as used for patch
// files and USB messages.
std::uint16_t crc16(std::span<const std::uint8_t> data, std::uint16_t seed = 0);

} // namespace g2
