#pragma once
// The knobs with a reset triangle (panel types "Reset" and "Reset/medium"), as Clavia's editor v1.62 has them
// (CPnlIndicatorKnob): a strip on top of the knob holds the triangle, lit while the value is the centre one; a click on
// it sets that value, or with the morph key clears the morph range in the focused group (CPnlKnobSliderABC::OnClick
// @000cb9b0 area 4, ResetParamValue @000cb92e, ResetMorphValue @000cb960; manual p73). The knob and its inc/dec pair
// are below the strip. Positions are relative to the control's place on the panel. Plain C++.

#include "IncDec.h"

#include <optional>
#include <string_view>

namespace g2ui {

// The strip's hit area (and where the triangle is drawn): medium 6 high and 6 in from each side, small 5 and 5
// (CPnlIndicatorKnob::CPnlIndicatorKnob @000cc346, MapToTargetArea @000cc4b8, Draw @000cc686).
inline std::optional<IncDecRect> resetStrip(std::string_view type)
{
    if (type == "Reset")
        return IncDecRect{5, 0, 21 - 2 * 5, 5};
    if (type == "Reset/medium")
        return IncDecRect{6, 0, 23 - 2 * 6, 6};
    return std::nullopt;
}

// Where the triangle is drawn: the strip's width and height, centred over the knob as drawn (`knobWidth` px wide).
inline std::optional<IncDecRect> resetTriangle(std::string_view type, int knobWidth)
{
    auto s = resetStrip(type);
    if (s)
        s->x = (knobWidth - s->w) / 2;
    return s;
}

// How far below the control's top the knob is: the strip's height, 0 for the other knobs.
inline int knobTop(std::string_view type)
{
    const auto s = resetStrip(type);
    return s ? s->h : 0;
}

inline bool onResetStrip(std::string_view type, int x, int y)
{
    const auto s = resetStrip(type);
    return s && s->contains(x, y);
}

// The centre value the triangle resets to and is lit at (UpdateGraphics @000cdc6c).
inline int resetValue(int max) { return (max + 1) >> 1; }
inline bool resetLit(int value, int max) { return value == resetValue(max); }

// The Replace button every module has at its top left (CReplaceView, 11 x 11 at (1, 1), framed in the module's group
// colour; CPanel::CPanel @000c2b6a): a click opens the Replace menu (CModuleReplaceMenu::DoReplaceMenu from
// CPanel::HandleChangeRequests @000bf2d8). The name bar starts right of it, at the button's width + 1 (CNameBar at
// kWidth + 1, taken as 12: not confirmed).
inline IncDecRect replaceButton() { return {1, 1, 11, 11}; }
inline bool onReplaceButton(int x, int y) { return replaceButton().contains(x, y); }
inline constexpr int kModuleNameX = 12;

} // namespace g2ui
