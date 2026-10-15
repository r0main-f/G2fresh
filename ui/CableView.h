#pragma once
// How cables are shown, as Clavia's editor v1.62 does it (fidelity audit C7-C9): the Options dialog's cable style,
// Shake, and Hide All with its keys. Plain C++.

#include <algorithm>
#include <array>

namespace g2ui {

// Options > Cable Style (CCableRenderer::MakeNewCable @0001a706), a preference of the user, for every patch.
enum class CableStyle { Straight3D = 0, Curved3D = 1, StraightThin = 2, CurvedThin = 3 };
inline constexpr CableStyle kDefaultCableStyle = CableStyle::Curved3D; // CPrefManager @0006efea

// A stored value; one the original does not know reads as Curved 3D (SetCableStyle @0006e5bc).
inline CableStyle cableStyleFromSetting(int v)
{
    return v >= 0 && v <= 3 ? static_cast<CableStyle>(v) : kDefaultCableStyle;
}

// The curved styles: what Shake acts on (CCableRenderer::IsCurrentStyleShakeable @00019a16). A straight cable is one
// segment between the jack centres.
inline bool isCurved(CableStyle s)
{
    return s == CableStyle::Curved3D || s == CableStyle::CurvedThin;
}

// k3DCableWidth 3.0, kSlimCableWidth 1.01 (GetCableWidth @001a875a).
inline float cableWidth(CableStyle s)
{
    return s == CableStyle::Curved3D || s == CableStyle::Straight3D ? 3.0f : 1.01f;
}

// View > Cable Opacity, G2fresh's own (the original draws its cables opaque): a setting of the user, in percent.
inline constexpr std::array<int, 4> kCableOpacities{100, 75, 50, 25};
inline int cableOpacityFromSetting(int percent)
{
    return std::find(kCableOpacities.begin(), kCableOpacities.end(), percent) != kCableOpacities.end() ? percent : 100;
}
inline float cableAlpha(int percent)
{
    return static_cast<float>(cableOpacityFromSetting(percent)) / 100.0f;
}

// Whether a cable is drawn: Hide All's flag and its colour's flag both let it show (CPatch::HandlePatchHeaderDump
// @000dab9c); a highlighted one is drawn whatever they say (CCableRenderer::Render @0001a1ce).
inline bool cableShown(bool cablesShown, bool colourShown, bool highlighted)
{
    return highlighted || (cablesShown && colourShown);
}

// What Space does (CPatchView::InitializeShortcutKeys @000fafd8): alone it hides or shows all cables, with the
// Control key (the physical one on the Mac) it shakes them. CShortcutKeys::GetMethod @001c81e2 reads two modifiers as
// none, and any other single one is not bound.
struct SpaceMods {
    bool control = false;
    bool shift = false;
    bool option = false;
    bool command = false;
};
enum class SpaceAction { None, HideAll, Shake };
inline SpaceAction spaceKey(SpaceMods m)
{
    const int held = int(m.control) + int(m.shift) + int(m.option) + int(m.command);
    if (held == 0 || held >= 2)
        return SpaceAction::HideAll;
    return m.control ? SpaceAction::Shake : SpaceAction::None;
}

} // namespace g2ui
