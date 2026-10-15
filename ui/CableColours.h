#pragma once
// The original's cable colours, which the Classic look draws (Clavia's editor v1.62: the 16-bit CRGBColor table in
// Global.c, enum 0..6 Audio, Control, Logic, TurboLogic, User1, User2, NoSignal; and the highlight of a held jack's
// chain). The Modern look has its own colours (ModulePainter::cableColour). Plain C++.

#include "g2/patch.hpp"

#include <array>
#include <cstdint>

namespace g2ui {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

inline Rgb originalCableRgb(g2::CableColor c)
{
    static constexpr std::array<Rgb, 7> table{{
        {255, 90, 90},   // red (Audio)
        {100, 100, 255}, // blue (Control)
        {230, 230, 80},  // yellow (Logic)
        {255, 192, 80},  // orange (TurboLogic)
        {80, 210, 80},   // green (User1)
        {200, 0, 230},   // purple (User2; its green is 0x003C in 16 bits)
        {255, 255, 255}, // white (NoSignal)
    }};
    const auto i = static_cast<std::size_t>(c);
    return i < table.size() ? table[i] : table.back();
}

// A held jack's chain (CPatchBackground::CableHighLight): #9BFFFF.
inline constexpr Rgb kOriginalHighlightRgb{155, 255, 255};

} // namespace g2ui
