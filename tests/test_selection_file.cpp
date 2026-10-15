// Saving a selection as a patch file and inserting a patch file's modules (G2fresh's own: the original has no such
// command; its CClipboardFilePort Save/Load @0006c730/@0006c736 are empty and unused).
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/patch.hpp"

#include <algorithm>

using namespace g2;

namespace {

constexpr u8 kOscB = 7, k2Out = 4, kMix4 = 19, kEnvAdsr = 20;

bool linked(const Area& a, u8 from, u8 to)
{
    return std::any_of(a.cables.begin(), a.cables.end(), [&](const Cable& c) { return c.fromModule == from && c.toModule == to; });
}

} // namespace

TEST_CASE("selection file: the selected modules alone, from the top left, with their cables and morphs")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 3, 10), env = edit::addModule(p, Location::Va, kEnvAdsr, 4, 10),
             out = edit::addModule(p, Location::Va, k2Out, 5, 10);
    edit::connect(p, Location::Va, {osc, 0, true}, {env, 0, false});
    edit::connect(p, Location::Va, {env, 0, true}, {out, 0, false});
    edit::setMorph(p, 0, Location::Va, osc, 0, 2, 10);

    const Patch part = edit::selectionPatch(p, Location::Va, {osc, env});
    REQUIRE(part.va.modules.size() == 2);
    CHECK(part.fx.modules.empty());
    CHECK(std::min(part.va.modules[0].col, part.va.modules[1].col) == 0);
    CHECK(std::min(part.va.modules[0].row, part.va.modules[1].row) == 0);
    CHECK(part.va.cables.size() == 1); // the 2-Out was left out
    CHECK(edit::morphRange(part, 0, Location::Va, part.va.modules[0].index, 0, 2) == 10);

    // A file like any other: it saves and loads.
    const auto bytes = savePatch(part);
    const auto loaded = load(bytes);
    REQUIRE_FALSE(loaded.isPerformance());
    CHECK(std::get<Patch>(loaded.content).va.modules.size() == 2);
}

TEST_CASE("insert a patch file: its voice modules into the voice area, its FX modules into the FX area")
{
    Patch file = Patch::makeDefault();
    const u8 a = edit::addModule(file, Location::Va, kOscB, 0, 0), b = edit::addModule(file, Location::Va, k2Out, 1, 0);
    edit::connect(file, Location::Va, {a, 0, true}, {b, 0, false});
    edit::addModule(file, Location::Fx, kMix4, 0, 0);

    Patch p = Patch::makeDefault();
    edit::addModule(p, Location::Va, kEnvAdsr, 0, 0);
    const auto added = edit::insertPatch(p, file);
    CHECK(added.va.size() == 2);
    CHECK(added.fx.size() == 1);
    CHECK(p.va.modules.size() == 3);
    CHECK(p.fx.modules.size() == 1);
    CHECK(linked(p.va, added.va[0], added.va[1]));
    // Placed below what the area has.
    const auto* env = &p.va.modules.front();
    for (const u8 i : added.va)
        CHECK(p.va.find(i)->row >= env->row + 4);
}
