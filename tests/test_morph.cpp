// The morph model of the original editor: assignments keyed by (parameter,
// group), the 25-parameter limit, the morphable flag, the range clamp, the
// popup's Morph assign and the morph dial's Clear Group / Copy To.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "test_support.hpp"

#include <set>
#include <tuple>

using namespace g2;

namespace {

constexpr u8 kOscB = 7;

// Adds a module with a parameter that has the given morphable flag (and, if
// `notMax127`, a maximum other than 127); returns (module, parameter).
std::pair<u8, u8> addWithParam(Patch& p, bool morphable, bool notMax127 = false)
{
    for (const auto& def : db::modules()) {
        if (!def.selectable)
            continue;
        for (std::size_t i = 0; i < def.params.size(); ++i) {
            const auto& d = def.params[i];
            if (d.morphable != morphable || (notMax127 && (d.max == 127 || d.max < 2)))
                continue;
            try {
                return {edit::addModule(p, Location::Va, def.typeId, 1, 0), static_cast<u8>(i)};
            } catch (const std::invalid_argument&) {
                break;
            }
        }
    }
    FAIL("no such parameter in the database");
    return {};
}

// Puts a parameter in a group with a small non-zero range.
void morph(Patch& p, u8 v, u8 m, u8 param, u8 group)
{
    edit::setMorph(p, v, Location::Va, m, param, group, edit::clampMorphRange(p, v, Location::Va, m, param, 1) ? 1 : -1);
}

std::size_t multiGroupParams(const Patch& p, std::size_t v)
{
    std::set<std::tuple<u8, u8, u8>> seen, multi;
    for (const auto& a : p.morphs[v].assigns)
        if (!seen.insert({a.location, a.module, a.param}).second)
            multi.insert({a.location, a.module, a.param});
    return multi.size();
}

} // namespace

TEST_CASE("a parameter keeps each of its morph groups through edits and saves")
{
    // Corpus files with parameters in several groups (manual p91): editing
    // one group leaves the others, and undoing the edit restores the file.
    std::size_t files = 0, params = 0;
    for (const auto& path : g2test::corpusFiles()) {
        if (path.extension() != ".pch2")
            continue;
        const auto bytes = g2test::readBytes(path);
        Patch p;
        try {
            p = loadPatch(bytes);
        } catch (const std::exception&) {
            continue;
        }
        bool found = false;
        for (std::size_t v = 0; v < p.morphs.size(); ++v) {
            if (multiGroupParams(p, v) == 0)
                continue;
            found = true;
            for (const auto& a : p.morphs[v].assigns) {
                const auto loc = static_cast<Location>(a.location);
                const auto groups = edit::morphsOf(p, static_cast<u8>(v), loc, a.module, a.param);
                if (groups.size() < 2 || a.range == 0 || a.range == -128)
                    continue;
                INFO(path.string() << " variation " << v << " module " << int(a.module) << " param " << int(a.param));
                ++params;
                Patch q = p;
                const int other = a.range > 0 ? a.range - 1 : a.range + 1;
                edit::storeMorph(q, static_cast<u8>(v), loc, a.module, a.param, a.morph, other == 0 ? 1 : other);
                for (const auto& g : groups)
                    if (g.group != a.morph)
                        CHECK(edit::morphRange(q, static_cast<u8>(v), loc, a.module, a.param, g.group) == g.range);
                CHECK(q.morphs[v].assigns.size() == p.morphs[v].assigns.size());
                edit::storeMorph(q, static_cast<u8>(v), loc, a.module, a.param, a.morph, a.range);
                CHECK(savePatch(q) == bytes);
                edit::clearMorph(q, static_cast<u8>(v), loc, a.module, a.param, a.morph);
                CHECK(edit::morphsOf(q, static_cast<u8>(v), loc, a.module, a.param).size() == groups.size() - 1);
                CHECK(q.morphs[v].assigns.size() == p.morphs[v].assigns.size() - 1);
                break; // one parameter per variation is enough
            }
        }
        files += found;
    }
    // The committed corpus has none; G2_EXTRA_CORPUS can add some.
    INFO(files << " files, " << params << " parameters in several groups");
    SUCCEED();

    // A patch built here: one parameter in three groups, saved and reloaded.
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::setMorph(p, 0, Location::Va, osc, 0, 3, 20);
    edit::setMorph(p, 0, Location::Va, osc, 0, 0, -10);
    edit::setMorph(p, 0, Location::Va, osc, 0, 6, 5);
    const Patch again = loadPatch(savePatch(p));
    const auto groups = edit::morphsOf(again, 0, Location::Va, osc, 0);
    REQUIRE(groups.size() == 3);
    CHECK((groups[0].group == 0 && groups[0].range == -10));
    CHECK((groups[1].group == 3 && groups[1].range == 20));
    CHECK((groups[2].group == 6 && groups[2].range == 5));
    // Stored in the order the original writes its map: parameter, then group.
    REQUIRE(again.morphs[0].assigns.size() == 3);
    CHECK(again.morphs[0].assigns[0].morph == 0);
    CHECK(again.morphs[0].assigns[1].morph == 3);
    CHECK(again.morphs[0].assigns[2].morph == 6);
    // A range edit and a removal touch one group only.
    Patch q = again;
    edit::setMorph(q, 0, Location::Va, osc, 0, 3, -7);
    CHECK(edit::morphRange(q, 0, Location::Va, osc, 0, 0) == std::int8_t{-10});
    CHECK(edit::morphRange(q, 0, Location::Va, osc, 0, 3) == std::int8_t{-7});
    edit::setMorph(q, 0, Location::Va, osc, 0, 3, 0); // range 0: out of the group
    CHECK_FALSE(edit::morphRange(q, 0, Location::Va, osc, 0, 3));
    CHECK(edit::morphsOf(q, 0, Location::Va, osc, 0).size() == 2);
    edit::clearMorph(q, 0, Location::Va, osc, 0, 6);
    CHECK(edit::morphsOf(q, 0, Location::Va, osc, 0).size() == 1);
}

TEST_CASE("a variation morphs at most 25 parameters")
{
    Patch p = Patch::makeDefault();
    std::vector<u8> oscs;
    for (int i = 0; i < 8; ++i)
        oscs.push_back(edit::addModule(p, Location::Va, kOscB, 0, static_cast<u8>(i * 8)));
    // The morphable parameters of the four oscillators.
    std::vector<std::pair<u8, u8>> params;
    for (u8 osc : oscs)
        for (std::size_t i = 0; i < db::find(kOscB)->params.size(); ++i)
            if (edit::isMorphable(p, Location::Va, osc, static_cast<u8>(i)))
                params.emplace_back(osc, static_cast<u8>(i));
    REQUIRE(params.size() > 26);
    for (std::size_t i = 0; i < 25; ++i)
        morph(p, 0, params[i].first, params[i].second, 0);
    CHECK(edit::morphedParamCount(p, 0) == 25);
    const auto [m26, p26] = params[25];
    CHECK_FALSE(edit::canMorph(p, 0, Location::Va, m26, p26));
    CHECK_THROWS_AS(morph(p, 0, m26, p26, 0), std::invalid_argument);
    CHECK_THROWS_AS(edit::toggleMorphAssign(p, 0, Location::Va, m26, p26, 0), std::invalid_argument);
    // A morphed parameter can still join other groups: the limit counts parameters.
    morph(p, 0, params[0].first, params[0].second, 5);
    CHECK(edit::morphedParamCount(p, 0) == 25);
    CHECK(p.morphs[0].assigns.size() == 26);
    // Per variation.
    CHECK(edit::canMorph(p, 1, Location::Va, m26, p26));

    // Pasting: each pasted assignment counts one, none past 25
    // (CPatchData::GetNewModulesMolecules).
    Patch src = Patch::makeDefault();
    const u8 osc = edit::addModule(src, Location::Va, kOscB, 0, 0);
    edit::setMorph(src, 0, Location::Va, osc, 0, 1, 10);
    edit::setMorph(src, 0, Location::Va, osc, 0, 2, 10);
    edit::setMorph(src, 0, Location::Va, osc, 1, 1, 10);
    edit::setMorph(src, 1, Location::Va, osc, 1, 1, 10);
    const auto clip = edit::copyModules(src, Location::Va, {osc});
    edit::clearMorph(p, 0, Location::Va, params[24].first, params[24].second, 0); // 24 parameters
    const auto pasted = edit::pasteModules(p, Location::Va, clip, 4, 0);
    REQUIRE(pasted.size() == 1);
    // The first assignment fits (25), the next ones do not.
    CHECK(edit::morphRange(p, 0, Location::Va, pasted[0], 0, 1) == std::int8_t{10});
    CHECK_FALSE(edit::morphRange(p, 0, Location::Va, pasted[0], 0, 2));
    CHECK_FALSE(edit::morphRange(p, 0, Location::Va, pasted[0], 1, 1));
    CHECK(edit::morphRange(p, 1, Location::Va, pasted[0], 1, 1) == std::int8_t{10}); // variation 2 has room
}

TEST_CASE("only morphable parameters take morphs")
{
    Patch p = Patch::makeDefault();
    const auto [m, param] = addWithParam(p, false);
    CHECK_FALSE(edit::isMorphable(p, Location::Va, m, param));
    CHECK_FALSE(edit::canMorph(p, 0, Location::Va, m, param));
    CHECK_THROWS_AS(edit::setMorph(p, 0, Location::Va, m, param, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(edit::toggleMorphAssign(p, 0, Location::Va, m, param, 0), std::invalid_argument);
    // A loaded patch may have one: it stays, and can be removed.
    edit::storeMorph(p, 0, Location::Va, m, param, 0, 1);
    CHECK(edit::morphRange(p, 0, Location::Va, m, param, 0) == std::int8_t{1});
    edit::clearMorph(p, 0, Location::Va, m, param, 0);
    CHECK(p.morphs[0].assigns.empty());
}

TEST_CASE("a morph range keeps value + range within the parameter's range")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::setParam(p, Location::Va, osc, 0, 0, 100);
    edit::setMorph(p, 0, Location::Va, osc, 0, 1, 100);
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 1) == std::int8_t{27});
    edit::setMorph(p, 0, Location::Va, osc, 0, 1, -127);
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 1) == std::int8_t{-100});

    // A parameter whose maximum is not 127.
    const auto [m, param] = addWithParam(p, true, true);
    const int max = p.va.find(m)->def()->params[param].max;
    edit::setParam(p, Location::Va, m, param, 0, 0);
    edit::setMorph(p, 0, Location::Va, m, param, 2, 127);
    CHECK(edit::morphRange(p, 0, Location::Va, m, param, 2) == static_cast<std::int8_t>(max));
    CHECK(edit::clampMorphRange(p, 0, Location::Va, m, param, -5) == 0);
}

TEST_CASE("the popup's Morph assign toggles the focused group")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::setParam(p, Location::Va, osc, 0, 2, 90);
    edit::setMorph(p, 2, Location::Va, osc, 0, 4, -20);
    // Assign: value 0, range +max, in the focused group only.
    CHECK(edit::toggleMorphAssign(p, 2, Location::Va, osc, 0, 1));
    CHECK(p.va.find(osc)->params[2][0] == 0);
    CHECK(edit::morphRange(p, 2, Location::Va, osc, 0, 1) == std::int8_t{127});
    CHECK(edit::morphRange(p, 2, Location::Va, osc, 0, 4) == std::int8_t{-20}); // the other group stays
    CHECK(p.va.find(osc)->params[0][0] != 0);                                   // other variations too
    // Deassign: that group only.
    CHECK_FALSE(edit::toggleMorphAssign(p, 2, Location::Va, osc, 0, 1));
    CHECK_FALSE(edit::morphRange(p, 2, Location::Va, osc, 0, 1));
    CHECK(edit::morphRange(p, 2, Location::Va, osc, 0, 4) == std::int8_t{-20});
}

TEST_CASE("the morph dial's Clear Group and Copy To")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::setMorph(p, 0, Location::Va, osc, 0, 2, 10);
    edit::setMorph(p, 0, Location::Va, osc, 1, 2, -5);
    edit::setMorph(p, 0, Location::Va, osc, 1, 3, 7);
    edit::setMorph(p, 1, Location::Va, osc, 0, 2, 9);

    edit::copyMorphGroup(p, 0, 2, 3);
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 3) == std::int8_t{10});
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 1, 3) == std::int8_t{-5}); // replaces its range there
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 2) == std::int8_t{10}); // the source stays
    CHECK_FALSE(edit::morphRange(p, 1, Location::Va, osc, 0, 3));               // one variation

    edit::clearMorphGroup(p, 0, 2);
    CHECK(edit::morphsOf(p, 0, Location::Va, osc, 0).size() == 1);
    CHECK(edit::morphsOf(p, 0, Location::Va, osc, 1).size() == 1);
    CHECK(edit::morphRange(p, 1, Location::Va, osc, 0, 2) == std::int8_t{9});
}

TEST_CASE("a loaded morph map is repaired as the original reads it")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    auto& list = p.morphs[0].assigns;
    list.push_back({1, osc, 0, 2, 10});
    list.push_back({1, osc, 0, 2, 30}); // a repeat: the first is kept
    list.push_back({1, osc, 0, 5, 4});  // another group: kept
    CHECK_FALSE(edit::repairMorphs(p)); // nothing to report
    REQUIRE(list.size() == 2);
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 2) == std::int8_t{10});
    list.push_back({1, 77, 0, 1, 5});   // no such module
    list.push_back({1, osc, 120, 1, 5}); // no such parameter
    CHECK(edit::repairMorphs(p));       // "Morph assignment problem"
    CHECK(list.size() == 2);
}

TEST_CASE("a synth's morph changes address one group")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::storeMorph(p, 0, Location::Va, osc, 0, 1, 40);
    edit::storeMorph(p, 0, Location::Va, osc, 0, 7, -40);
    edit::storeMorph(p, 0, Location::Va, osc, 0, 1, 0); // range 0 removes group 1 only
    CHECK_FALSE(edit::morphRange(p, 0, Location::Va, osc, 0, 1));
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 7) == std::int8_t{-40});
    CHECK_THROWS_AS(edit::storeMorph(p, 0, Location::Va, 99, 0, 1, 4), std::invalid_argument);
}
