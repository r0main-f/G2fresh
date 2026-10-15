// The module bar's keys, as Clavia's editor v1.62 routes them from the patch window to its toolbar first
// (CPatchView::HandleKeyDownUp @000f93d4 -> CTBWindow::RouteKeySpec; CTabBarView::RouteKeySpec @0012603c for the
// groups, CTabButtonBar::RouteKeySpec @00127238 for the modules of a group; manual p115-116).
#include <catch2/catch_test_macros.hpp>

#include "ModuleBarKeys.h"

#include <vector>

using namespace g2ui;
using Result = ModuleBarKeys::Result;

namespace {

// Three groups of 4, 1 and 3 modules.
ModuleBarKeys bar() { return ModuleBarKeys(std::vector<int>{4, 1, 3}); }

ModuleBarKey tab(bool shift = false) { return {.tab = true, .shift = shift}; }
ModuleBarKey ch(char c) { return {.character = c}; }

} // namespace

TEST_CASE("module bar: each group starts on its first module")
{
    auto b = bar();
    CHECK(b.group() == 0);
    CHECK(b.focused(0) == 0);
    CHECK(b.focused(2) == 0);
}

TEST_CASE("module bar: Tab and Shift+Tab change the group, wrapping, held or not")
{
    auto b = bar();
    CHECK(b.press(tab()) == Result::Handled);
    CHECK(b.group() == 1);
    b.press(tab());
    b.press(tab());
    CHECK(b.group() == 0); // after the last, the first
    b.press(tab(true));
    CHECK(b.group() == 2); // before the first, the last
    ModuleBarKey held = tab();
    held.repeat = true;
    CHECK(b.press(held) == Result::Handled);
    CHECK(b.group() == 0);
    // Any modifier but Shift: not the toolbar's (IsAlternateKeyDown, the Command and Control keys).
    ModuleBarKey cmdTab = tab();
    cmdTab.other = true;
    CHECK(b.press(cmdTab) == Result::None);
    CHECK(b.group() == 0);
}

TEST_CASE("module bar: < and > (or , and .) move along the group's modules, wrapping; each group keeps its own")
{
    auto b = bar();
    CHECK(b.press(ch('>')) == Result::Handled);
    CHECK(b.focused(0) == 1);
    b.press(ch('.'));
    b.press(ch('.'));
    b.press(ch('.'));
    CHECK(b.focused(0) == 0); // after the last, the first
    b.press(ch(','));
    CHECK(b.focused(0) == 3);
    b.press(ch('<'));
    CHECK(b.focused(0) == 2);
    b.press(tab());
    CHECK(b.focused(1) == 0);
    b.press(tab(true));
    CHECK(b.focused(0) == 2); // kept while the other group was shown
    CHECK(b.press(ch('a')) == Result::None);
}

TEST_CASE("module bar: Return or Enter inserts the focused module, on the key going down only")
{
    auto b = bar();
    b.press(ch('>'));
    CHECK(b.press({.insert = true}) == Result::Insert);
    CHECK(b.focused(0) == 1);
    CHECK(b.press({.insert = true, .repeat = true}) == Result::None);
}
