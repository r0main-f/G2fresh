#pragma once

#include <algorithm>

namespace g2ui {

// The patch window's split bar between the Voice Area (above) and the FX Area, as the original's
// CPatchView::CSplitterView. Its position is the Voice Area's height in pixels, which the patch header keeps
// (splitterPos; CPatchView::PositionSubViews, HandleChangeRequests @000fc6ba). V shows only the Voice Area and F only
// the FX Area, each again going back (VirtualPushShowPoly @000f6f7a, VirtualPushShowMono @000f70da); the bar's third
// button goes back to the position before (VirtualPushRestore @000f6f12). An edge is never the position remembered.
struct SplitBar {
    static constexpr int kHeight = 11; // the bar, and its three buttons: 11 x 11 (PositionSubViews @000f7142)

    int position = 600; // the Voice Area's height
    int previous = 0;   // the last position away from the edges; 0: none

    // The position shown in a window `total` pixels high (both areas and the bar): the bar stays in it.
    static int shown(int position, int total) { return std::clamp(position, 0, std::max(0, total - kHeight)); }

    // The keys V and F: to the edge, or back from it.
    void showVoice(int total)
    {
        if (shown(position, total) == voiceEdge(total))
            restore();
        else
            toVoice(total);
    }
    void showFx(int total)
    {
        if (shown(position, total) == 0)
            restore();
        else
            toFx(total);
    }
    // The bar's first two buttons: to the edge.
    void toVoice(int total) { moveTo(voiceEdge(total), total); }
    void toFx(int total) { moveTo(0, total); }

    void restore()
    {
        if (previous != 0)
            position = previous;
    }

    // A drag of the bar: where it starts is remembered (unless at an edge), each step only moves it.
    void beginDrag(int total)
    {
        if (!atEdge(total))
            previous = shown(position, total);
    }
    void dragTo(int to, int total) { position = shown(to, total); }

private:
    void moveTo(int to, int total)
    {
        if (!atEdge(total))
            previous = shown(position, total);
        position = to;
    }

    static int voiceEdge(int total) { return std::max(0, total - kHeight); }
    bool atEdge(int total) const
    {
        const int p = shown(position, total);
        return p == 0 || p == voiceEdge(total);
    }
};

} // namespace g2ui
