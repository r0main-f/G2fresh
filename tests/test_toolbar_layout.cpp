// The toolbar's layout (ToolbarLayout.h): G2fresh's own.
#include <catch2/catch_test_macros.hpp>

#include "ToolbarLayout.h"

using namespace g2ui;

namespace {

// The toolbar of a performance with a G2 connected, as MainView lays it out: variations and Init, colour and Paint,
// morph, Randomize and Mutator, Perf, slots A-D, zoom; the left items (name, edited, Edit/Live, Init 1/2) 422 wide.
const std::vector<ToolbarGroup> kGroups{{365, 0}, {74, 16}, {150, 8}, {188, 4}, {44, 16}, {118, 16}, {110, 16}};
constexpr int kLeft = 422;

} // namespace

TEST_CASE("The toolbar keeps every group whole: what does not fit goes to a row below")
{
    // Round-7 hand tests: in the user's window the Perf button pushed variations 1 and 2 out of the toolbar.
    for (const int width : {788, 1268, 1428, 2400}) {
        INFO("width " << width);
        const auto l = layoutToolbar(width, kLeft, kGroups);
        REQUIRE(l.groups.size() == kGroups.size());
        for (std::size_t i = 0; i < kGroups.size(); ++i) {
            INFO("group " << i);
            const auto& p = l.groups[i];
            CHECK(p.row >= 0);
            CHECK(p.row < l.rows);
            CHECK(p.x - kGroups[i].gapBefore >= (p.row == 0 ? kLeft : 0));
            CHECK(p.x + kGroups[i].width <= width);
            if (i + 1 < kGroups.size()) {
                // In reading order: the next group is to the right on the same row, or on a row above.
                const auto& q = l.groups[i + 1];
                CHECK(q.row <= p.row);
                if (q.row == p.row)
                    CHECK(p.x + kGroups[i].width + kGroups[i + 1].gapBefore <= q.x);
            }
        }
        // Each row ends at the bar's right edge, as the single row did.
        for (int row = 0; row < l.rows; ++row) {
            int end = -1;
            for (std::size_t i = 0; i < kGroups.size(); ++i)
                if (l.groups[i].row == row)
                    end = l.groups[i].x + kGroups[i].width;
            CHECK(end == width);
        }
    }
    CHECK(layoutToolbar(2400, kLeft, kGroups).rows == 1);
    CHECK(layoutToolbar(1428, kLeft, kGroups).rows == 2);
}
