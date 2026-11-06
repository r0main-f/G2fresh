// The patch window's split bar between the Voice and FX areas, as the original's CPatchView::CSplitterView.
#include "SplitBar.h"

#include <catch2/catch_test_macros.hpp>

using g2ui::SplitBar;

TEST_CASE("split bar: the position is the Voice Area's height, kept within the window")
{
    // CPatchView::PositionSubViews: the header's splitterPos below the toolbar, the bar (11 px) at most at the bottom.
    CHECK(SplitBar::kHeight == 11);
    CHECK(SplitBar::shown(600, 900) == 600);
    CHECK(SplitBar::shown(838, 700) == 700 - 11);
    CHECK(SplitBar::shown(0, 700) == 0);
}

TEST_CASE("split bar: V shows only the Voice Area, and again goes back")
{
    // VirtualPushShowPoly @000f6f7a: at the poly edge it restores, else it moves there, remembering where it was.
    SplitBar s;
    s.position = 400;
    s.showVoice(700);
    CHECK(s.position == 700 - SplitBar::kHeight);
    s.showVoice(700);
    CHECK(s.position == 400);
}

TEST_CASE("split bar: F shows only the FX Area, and again goes back")
{
    // VirtualPushShowMono @000f70da, with the mono edge at the top (IsSplitterAtMonoEdge).
    SplitBar s;
    s.position = 400;
    s.showFx(700);
    CHECK(s.position == 0);
    s.showFx(700);
    CHECK(s.position == 400);
}

TEST_CASE("split bar: from one edge to the other keeps the position from before")
{
    // An edge is never remembered (HandleChangeRequests @000f9216: only when !IsSplitterAtEdge).
    SplitBar s;
    s.position = 400;
    s.showFx(700);
    s.showVoice(700);
    CHECK(s.position == 700 - SplitBar::kHeight);
    s.restore();
    CHECK(s.position == 400);
}

TEST_CASE("split bar: restore does nothing before a position is remembered")
{
    // VirtualPushRestore via HandleChangeRequests: only if the remembered position is not 0.
    SplitBar s;
    s.position = 0;
    s.restore();
    CHECK(s.position == 0);
}

TEST_CASE("split bar: a drag remembers where it started, unless at an edge")
{
    // Click @000f7050 remembers the bar's place when it is not at an edge; the drag's steps don't (Idle @000f6ea6).
    SplitBar s;
    s.position = 300;
    s.beginDrag(700);
    s.dragTo(450, 700);
    s.dragTo(500, 700);
    CHECK(s.position == 500);
    s.restore();
    CHECK(s.position == 300);

    SplitBar e;
    e.position = 0;
    e.previous = 300;
    e.beginDrag(700);
    e.dragTo(500, 700);
    e.restore();
    CHECK(e.position == 300);

    SplitBar t;
    t.position = 300;
    t.beginDrag(700);
    t.dragTo(1000, 700);
    CHECK(t.position == 700 - SplitBar::kHeight);
    t.dragTo(-5, 700);
    CHECK(t.position == 0);
}

TEST_CASE("split bar: its buttons go to their edge, and pressed again stay there")
{
    // The first two buttons (CSplitterView::HandleChangeRequests @000f9216: flags 0x78, 0xb8) have no toggle; only
    // the keys go back (VirtualPushShowMono/Poly).
    SplitBar s;
    s.position = 400;
    s.toFx(700);
    s.toFx(700);
    CHECK(s.position == 0);
    s.toVoice(700);
    s.toVoice(700);
    CHECK(s.position == 700 - SplitBar::kHeight);
    s.restore();
    CHECK(s.position == 400);
}
