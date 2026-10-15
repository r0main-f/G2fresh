#pragma once
// View > Zoom to Fit: G2fresh's own (the original does not scale its patch window). Plain C++.

#include <algorithm>
#include <span>

namespace g2ui {

// An area: how far its modules reach from its top-left corner (0, 0 when it has none), and its pane's size, in pixels
// at 100%.
struct FitArea {
    int contentRight = 0, contentBottom = 0;
    int viewWidth = 0, viewHeight = 0;
};

// The largest zoom at which every area's modules, with `margin` pixels to spare, fit its pane, within
// [minZoom, maxZoom]; 100% when no area has modules.
inline float fitZoom(std::span<const FitArea> areas, float minZoom, float maxZoom, int margin)
{
    float zoom = 0.0f;
    bool any = false;
    for (const auto& a : areas) {
        if (a.contentRight <= 0 || a.contentBottom <= 0)
            continue;
        const float fits = std::min(static_cast<float>(a.viewWidth) / static_cast<float>(a.contentRight + margin),
                                    static_cast<float>(a.viewHeight) / static_cast<float>(a.contentBottom + margin));
        zoom = any ? std::min(zoom, fits) : fits;
        any = true;
    }
    return any ? std::clamp(zoom, minZoom, maxZoom) : 1.0f;
}

} // namespace g2ui
