// The USB-protocol golden vectors (tests/protocol/*.json) as a C++ table,
// generated into tests/protocol/vectors.inc by tests/protocol/gen_cpp.py.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace g2test {

struct ProtoVector {
    const char* scenario;
    int step;           // index in the JSON file's "steps"
    const char* dir;    // "host->device" or "device->host"
    const char* endpoint; // "bulk_out", "bulk_in", "interrupt_in"
    const char* hex;    // nullptr when only a prefix is given
    const char* prefix; // prefix_hex, or nullptr
    bool valid;         // false: corrupted on purpose
    const char* annotation;

    bool hostToDevice() const { return std::string_view(dir) == "host->device"; }
    bool interrupt() const { return std::string_view(endpoint) == "interrupt_in"; }
};

struct CrcVector {
    const char* data; // ASCII text, or hex bytes
    bool ascii;
    std::uint16_t crc;
};

#include "protocol/vectors.inc"

inline std::vector<std::uint8_t> bytesOf(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    unsigned value = 0;
    int digits = 0;
    for (char c : hex) {
        int d = -1;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        if (d < 0)
            continue;
        value = value * 16 + static_cast<unsigned>(d);
        if (++digits == 2) {
            out.push_back(static_cast<std::uint8_t>(value));
            value = 0;
            digits = 0;
        }
    }
    return out;
}

inline const ProtoVector& vectorAt(std::string_view scenario, int step)
{
    for (const auto& v : kProtoVectors)
        if (scenario == v.scenario && v.step == step)
            return v;
    throw std::out_of_range("no such protocol vector: " + std::string(scenario) + " #" + std::to_string(step));
}

inline std::vector<std::uint8_t> vectorBytes(std::string_view scenario, int step)
{
    return bytesOf(vectorAt(scenario, step).hex);
}

inline std::string hexOf(std::span<const std::uint8_t> bytes)
{
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i)
            s += ' ';
        s += digits[bytes[i] >> 4];
        s += digits[bytes[i] & 15];
    }
    return s;
}

} // namespace g2test
