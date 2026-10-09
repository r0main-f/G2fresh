// Helpers shared by the module processors (engine/modules/*.cpp).
//
// Clean-room: everything here comes from re/notes/dsp-module-catalog.md, the
// module database, the editor's parameter display (core/param_text) and
// black-box measurements of the emulated G2 (re/notes/native-engine.md).
#pragma once

#include "g2/engine/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace g2::engine {

inline constexpr double kPi = 3.14159265358979323846;

// A parameter of the module in a variation (0 when absent).
inline int param(const Module& m, std::uint8_t variation, std::size_t index)
{
    if (variation < m.params.size() && index < m.params[variation].size())
        return m.params[variation][index];
    if (!m.params.empty() && index < m.params[0].size())
        return m.params[0][index];
    return 0;
}

inline int mode(const Module& m, std::size_t index)
{
    return index < m.modes.size() ? m.modes[index] : 0;
}

// Frequency of a G2 note number (fractional), A4 = note 69 = 440 Hz. The
// oscillators' Coarse knob is a note number [catalog §3, measured exact on the
// emulated G2: 440.003 Hz at Coarse 69].
inline double noteHz(double note)
{
    return 440.0 * std::exp2((note - 69.0) / 12.0);
}

// Mixer level curves (Lin/Exp switch: 0 Exp, 1 Lin, 2 dB). Exp and dB are the
// same curve, 0.01 x + 0.99 x^3 with x = v/127: the dB display of the editor
// (core/param_text dB) and the catalog's "about (v/127)^2.9" table, measured
// on the emulated G2 (20, 64, 100 -> 0.00544, 0.1317, 0.4912). Lin is v/128
// with 127 = 1.0 (measured: 20, 64, 100 -> 0.15625, 0.5, 0.78125).
inline float mixLevel(int v, int curve)
{
    const float x = static_cast<float>(v) / 127.0f;
    if (curve == 1) // Lin: v/128, 127 = 1.0 (measured)
        return v >= 127 ? 1.0f : static_cast<float>(v) / 128.0f;
    return 0.01f * x + 0.99f * x * x * x;
}

} // namespace g2::engine
