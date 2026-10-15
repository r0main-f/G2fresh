#pragma once
// The inc/dec pair under a knob: one step down on its left half, one up on its right half, shown while the knob is
// hovered or has the parameter focus (CABCKnobGUI::DrawKnobUpDowns @0007bfee; manual p83-84). Positions are relative
// to the knob's place on the panel, from the original's constants (the knobs' MapToTargetArea: small @000cc1d4,
// medium @000cc0f6, big @000cc018). Plain C++.

#include <optional>
#include <string_view>

namespace g2ui {

enum class IncDecKnob { Small, Medium, Big };

// The knob size of a panel knob type; nullopt for the sliders. The reset knobs (a knob with a centre button) are
// taken as their size's knob: not checked against the original.
inline std::optional<IncDecKnob> incDecKnob(std::string_view type)
{
    if (type == "Small" || type == "Reset")
        return IncDecKnob::Small;
    if (type == "Medium" || type == "Reset/medium")
        return IncDecKnob::Medium;
    if (type == "Big")
        return IncDecKnob::Big;
    return std::nullopt;
}

struct IncDecRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool operator==(const IncDecRect&) const = default;
};

// The knob's control: small 21 x 23, medium 23 x 25, big 23 x 27.
inline IncDecRect incDecControlSize(IncDecKnob k)
{
    return k == IncDecKnob::Small ? IncDecRect{0, 0, 21, 23} : k == IncDecKnob::Medium ? IncDecRect{0, 0, 23, 25}
                                                                                       : IncDecRect{0, 0, 23, 27};
}

// Where the pair is drawn: a 21 x 9 box, each half w/2 + 1 wide, so they share a pixel.
struct IncDecPair {
    IncDecRect dec, inc;
};
inline IncDecPair incDecPair(IncDecKnob k)
{
    const int x = k == IncDecKnob::Small ? 0 : 1;
    const int y = k == IncDecKnob::Small ? 14 : k == IncDecKnob::Medium ? 16 : 18;
    return {{x, y, 11, 9}, {x + 10, y, 11, 9}};
}

// What a click at (x, y) hits: below the pair's top, the left part (x < 10 small, < 11 otherwise) decrements and the
// rest of the control increments.
enum class IncDecPart { None, Dec, Inc };
inline IncDecPart incDecAt(IncDecKnob k, int x, int y)
{
    if (!incDecControlSize(k).contains(x, y) || y < incDecPair(k).dec.y)
        return IncDecPart::None;
    return x < (k == IncDecKnob::Small ? 10 : 11) ? IncDecPart::Dec : IncDecPart::Inc;
}

// Held, the pair steps again after 400 ms, then every 70 ms (CPnlControl::UpDownIdleHandler @000c8ae4).
inline constexpr int kIncDecFirstRepeatMs = 400;
inline constexpr int kIncDecRepeatMs = 70;

} // namespace g2ui
