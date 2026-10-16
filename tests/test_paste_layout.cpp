// A pasted, duplicated or moved group keeps its arrangement; the modules already there make room below it, as the
// original's drop does (CPatchData::GetMakeRoomForMolecules @000ea184: the dropped modules stay where they are put and
// the others move down). Found by hand testing 0.12.0: each pasted module was placed on its own.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"

using namespace g2;

namespace {
constexpr u8 kMix4 = 19; // two rows high
}

TEST_CASE("paste: the copies keep their arrangement, the module in the way makes room")
{
    Patch p = Patch::makeDefault();
    const u8 x = edit::addModule(p, Location::Va, kMix4, 0, 0), y = edit::addModule(p, Location::Va, kMix4, 1, 2);
    const u8 z = edit::addModule(p, Location::Va, kMix4, 1, 6); // in the way of y's copy
    const auto clip = edit::copyModules(p, Location::Va, {x, y});
    const auto added = edit::pasteModules(p, Location::Va, clip, 0, 4);
    REQUIRE(added.size() == 2);
    const auto* xc = p.va.find(added[0]);
    const auto* yc = p.va.find(added[1]);
    CHECK(xc->col == 0);
    CHECK(xc->row == 4);
    CHECK(yc->col == 1);
    CHECK(yc->row == 6); // two rows below x's copy, as y is below x
    CHECK(p.va.find(z)->row >= 8); // pushed down below y's copy
    CHECK(p.va.find(x)->row == 0);
    CHECK(p.va.find(y)->row == 2);
}

TEST_CASE("move: a moved group keeps its arrangement too")
{
    Patch p = Patch::makeDefault();
    const u8 a = edit::addModule(p, Location::Va, kMix4, 0, 0), b = edit::addModule(p, Location::Va, kMix4, 1, 2);
    const u8 c = edit::addModule(p, Location::Va, kMix4, 3, 3); // overlapping b once moved, from above
    edit::moveModules(p, Location::Va, {a, b}, 2, 2);
    CHECK(p.va.find(a)->col == 2);
    CHECK(p.va.find(a)->row == 2);
    CHECK(p.va.find(b)->col == 3);
    CHECK(p.va.find(b)->row == 4);
    CHECK(p.va.find(c)->row >= 6);
}

TEST_CASE("paste and duplicate: the copies go, whole, where they fit below the selection; nothing moves")
{
    // G2fresh's own placement (the original places a paste with the mouse; manual p100). Found by hand testing: a
    // copy dropped below the selection was interleaved with the modules there.
    Patch p = Patch::makeDefault();
    const u8 a = edit::addModule(p, Location::Va, kMix4, 0, 0), b = edit::addModule(p, Location::Va, kMix4, 1, 2);
    const u8 c = edit::addModule(p, Location::Va, kMix4, 1, 4); // just below b
    const auto clip = edit::copyModules(p, Location::Va, {a, b});
    // Below a (the selection): row 2 would put b's copy on c, so the block goes down until both copies fit.
    const u8 row = edit::blockFreeRow(p, Location::Va, clip, 0, 2);
    CHECK(row == 4);
    const auto added = edit::pasteModules(p, Location::Va, clip, 0, row);
    REQUIRE(added.size() == 2);
    CHECK(p.va.find(added[0])->row == 4);
    CHECK(p.va.find(added[1])->row == 6); // two rows below a's copy, as b is below a
    CHECK(p.va.find(a)->row == 0);        // nothing moved
    CHECK(p.va.find(b)->row == 2);
    CHECK(p.va.find(c)->row == 4);
    // Where it fits at once, it stays there.
    CHECK(edit::blockFreeRow(p, Location::Va, edit::copyModules(p, Location::Va, {a}), 3, 0) == 0);
}
