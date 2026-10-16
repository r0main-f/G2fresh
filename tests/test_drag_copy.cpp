// Option-drag copies the selection (Clavia's editor v1.62: CClipboard::GetCopyOrMove @0014cb82 copies when the
// drag-copy key, Option on the Mac, is down; fidelity audit P7): the copies land where the drag ends, the originals
// stay, and the cables between the copied modules come with them.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"

#include <algorithm>

using namespace g2;

TEST_CASE("option-drag copy: the copies at the drag's offset, the originals where they were")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 7, 0, 0), out = edit::addModule(p, Location::Va, 4, 1, 0);
    edit::connect(p, Location::Va, {osc, 0, true}, {out, 0, false});
    const auto copies = edit::copyModulesBy(p, Location::Va, {osc, out}, 2, 10);
    REQUIRE(copies.size() == 2);
    CHECK(p.va.modules.size() == 4);
    CHECK(p.va.find(osc)->col == 0);
    CHECK(p.va.find(osc)->row == 0);
    const auto* a = p.va.find(copies[0]);
    const auto* b = p.va.find(copies[1]);
    CHECK(a->col == 2);
    CHECK(a->row == 10);
    CHECK(b->col == 3);
    CHECK(b->row == 10);
    CHECK(std::any_of(p.va.cables.begin(), p.va.cables.end(),
                      [&](const Cable& c) { return c.fromModule == copies[0] && c.toModule == copies[1]; }));
    // Never left of the first column or above the first row.
    const auto clamped = edit::copyModulesBy(p, Location::Va, {osc}, -5, -5);
    CHECK(p.va.find(clamped[0])->col == 0);
}
