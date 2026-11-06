#pragma once
// The toolbar's layout: the document's own items on the left (name, Edit/Live, Init 1/2), the rest in groups on the
// right (variations, colour, morph, randomize and Mutator, slots, zoom). G2fresh's own layout. Plain C++.

#include <vector>

namespace g2ui {

struct ToolbarGroup {
    int width = 0;
    int gapBefore = 0; // the space it keeps from what is on its left
};

struct ToolbarPlace {
    int row = 0;
    int x = 0;
};

struct ToolbarLayout {
    int rows = 1;
    std::vector<ToolbarPlace> groups;
};

// `width`: the bar's; `leftWidth`: the left items' (on the first row). `groups` in their order from left to right.
// They fill the first row from its right end; a group that does not fit whole starts a row below, again from the
// right, so none is squeezed out (round-7 hand tests: one button more pushed variations 1 and 2 out of the user's
// window).
inline ToolbarLayout layoutToolbar(int width, int leftWidth, const std::vector<ToolbarGroup>& groups)
{
    ToolbarLayout layout;
    layout.groups.resize(groups.size());
    int row = 0, right = width, left = leftWidth;
    bool rowEmpty = true;
    for (auto i = groups.size(); i-- > 0;) {
        const auto& g = groups[i];
        // (A group wider than an empty row below still goes there: it is the widest the bar has.)
        if ((row == 0 || !rowEmpty) && right - g.width - g.gapBefore < left) {
            ++row;
            right = width;
            left = 0;
            rowEmpty = true;
        }
        layout.groups[i] = {row, right - g.width};
        right -= g.width + g.gapBefore;
        rowEmpty = false;
    }
    layout.rows = row + 1;
    return layout;
}

} // namespace g2ui
