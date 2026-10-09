#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/param_text.hpp"
#include "test_support.hpp"

using namespace g2;

namespace {
constexpr u8 kKeyboard = 1, kOut2 = 4, kOscB = 7;
}

TEST_CASE("building a small patch from scratch")
{
    Patch p = Patch::makeDefault();
    const u8 kbIdx = edit::addModule(p, Location::Va, kKeyboard, 0, 0);
    const u8 oscIdx = edit::addModule(p, Location::Va, kOscB, 0, 3);
    const u8 outIdx = edit::addModule(p, Location::Va, kOut2, 0, 9);
    const Module& osc = *p.va.find(oscIdx);
    CHECK(kbIdx == 1);
    CHECK(oscIdx == 2);
    CHECK(osc.name == "OscB1");
    REQUIRE(osc.params.size() == kFileVariations);
    CHECK(osc.params[0].size() == osc.def()->params.size());
    CHECK(osc.params[0][0] == osc.def()->params[0].defaultValue);

    // Keyboard pitch -> oscillator pitch (blue), oscillator out -> 2-Out (red).
    const Cable pitch = edit::connect(p, Location::Va, {kbIdx, 0, true}, {oscIdx, 0, false});
    const Cable audio = edit::connect(p, Location::Va, {oscIdx, 0, true}, {outIdx, 0, false});
    CHECK(pitch.color == CableColor::Blue);
    CHECK(audio.color == CableColor::Red);
    edit::connect(p, Location::Va, {outIdx, 0, false}, {outIdx, 1, false}); // link L to R
    CHECK(p.va.cables.size() == 3);

    CHECK_THROWS_AS(edit::connect(p, Location::Va, {oscIdx, 0, true}, {outIdx, 0, false}), std::invalid_argument);
    CHECK_THROWS_AS(edit::connect(p, Location::Va, {oscIdx, 0, true}, {outIdx, 0, true}), std::invalid_argument);
    CHECK_THROWS_AS(edit::connect(p, Location::Va, {oscIdx, 9, true}, {outIdx, 0, false}), std::invalid_argument);

    edit::setParam(p, Location::Va, oscIdx, 0, 0, 76);
    edit::setParam(p, Location::Va, oscIdx, 2, 0, 200); // KBT is on/off: clamped to 1
    CHECK(p.va.find(oscIdx)->params[0][0] == 76);
    CHECK(p.va.find(oscIdx)->params[0][2] == 1);

    // Save, reload, save again: stable.
    const auto bytes = savePatch(p);
    const Patch again = loadPatch(bytes);
    CHECK(savePatch(again) == bytes);
    CHECK(again.va.modules.size() == 3);
    CHECK(again.va.find(oscIdx)->name == "OscB1");
    CHECK(again.va.cables.size() == 3);
}

TEST_CASE("parameter text follows the module's dependencies")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    const auto& v = p.va.find(osc)->params[0];
    // OscB Coarse is displayed with Fine (param 1) and Tune mode (param 4).
    CHECK(edit::paramText(p, Location::Va, osc, 0, 0) == paramtext::formatTriple(60, v[0], v[1], v[4]));
    const std::string before = edit::paramText(p, Location::Va, osc, 0, 0);
    edit::setParam(p, Location::Va, osc, 4, 0, 1); // change the tune mode
    CHECK(edit::paramText(p, Location::Va, osc, 0, 0) != before);
}

TEST_CASE("removing a module removes everything that refers to it")
{
    Patch p = loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    const auto loc = Location::Va;
    // Pick a module that has cables.
    REQUIRE_FALSE(p.va.cables.empty());
    const u8 index = p.va.cables.front().toModule;
    const auto cablesBefore = p.va.cables.size();
    p.knobs[5] = KnobAssign{static_cast<u8>(loc), index, 0, 0, 0};
    p.morphs[0].assigns.push_back({static_cast<u8>(loc), index, 0, 1, 40});
    p.controllers.push_back({20, static_cast<u8>(loc), index, 0});
    // References to other modules must survive.
    p.knobs[6] = KnobAssign{2, 2, 0, 0, 0};

    edit::removeModule(p, loc, index);
    CHECK(p.va.find(index) == nullptr);
    CHECK(p.va.cables.size() < cablesBefore);
    for (const auto& c : p.va.cables)
        CHECK((c.fromModule != index && c.toModule != index));
    CHECK_FALSE(p.knobs[5].has_value());
    CHECK(p.knobs[6].has_value());
    CHECK(p.morphs[0].assigns.empty());
    for (const auto& c : p.controllers)
        CHECK_FALSE((c.location == static_cast<u8>(loc) && c.module == index));
    CHECK(p.controllers.size() == 2); // the default volume and sustain mappings remain

    const auto bytes = savePatch(p);
    CHECK(savePatch(loadPatch(bytes)) == bytes);
}
