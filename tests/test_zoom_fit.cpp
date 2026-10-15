// View > Zoom to Fit (G2fresh's own: the original does not scale its patch window; its zoom box sizes the window to
// the areas, CPatchView::MacGetZoomSize @000f7342). The largest zoom at which every area's modules fit its pane.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ZoomFit.h"

#include <vector>

using namespace g2ui;
using Catch::Approx;

TEST_CASE("zoom to fit: the tightest area decides, within the zoom's limits")
{
    // Modules up to 400 x 300 px in an 800 x 600 pane, with a 10 px margin: min(800 / 410, 600 / 310) = 1.935.
    CHECK(fitZoom(std::vector<FitArea>{{400, 300, 800, 600}}, 0.5f, 2.0f, 10) == Approx(600.0f / 310.0f));
    // Two areas: the one that needs the smaller zoom.
    CHECK(fitZoom(std::vector<FitArea>{{400, 300, 800, 600}, {790, 100, 800, 300}}, 0.5f, 2.0f, 10) == Approx(800.0f / 800.0f));
    // Height can be what decides.
    CHECK(fitZoom(std::vector<FitArea>{{100, 590, 800, 300}}, 0.5f, 2.0f, 10) == Approx(0.5f));
}

TEST_CASE("zoom to fit: clamped to the limits; empty areas don't count; nothing at all is 100%")
{
    CHECK(fitZoom(std::vector<FitArea>{{50, 50, 800, 600}}, 0.5f, 2.0f, 10) == Approx(2.0f));
    CHECK(fitZoom(std::vector<FitArea>{{4000, 300, 800, 600}}, 0.5f, 2.0f, 10) == Approx(0.5f));
    CHECK(fitZoom(std::vector<FitArea>{{400, 300, 800, 600}, {0, 0, 800, 300}}, 0.5f, 2.0f, 10) == Approx(600.0f / 310.0f));
    CHECK(fitZoom(std::vector<FitArea>{{0, 0, 800, 600}, {0, 0, 800, 300}}, 0.5f, 2.0f, 10) == Approx(1.0f));
}
