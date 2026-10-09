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

TEST_CASE("moving a module onto others pushes them down its column")
{
    Patch p = Patch::makeDefault();
    const u8 a = edit::addModule(p, Location::Va, kOscB, 0, 0);    // rows 0-4
    const u8 b = edit::addModule(p, Location::Va, kOut2, 0, 5);    // rows 5-6
    const u8 c = edit::addModule(p, Location::Va, kKeyboard, 1, 0);
    CHECK(edit::freeRow(p, Location::Va, 0) == 7);

    edit::moveModule(p, Location::Va, c, 0, 3); // drop the keyboard into OscB
    const auto* osc = p.va.find(a);
    const auto* kb = p.va.find(c);
    const auto* out = p.va.find(b);
    CHECK(kb->col == 0);
    CHECK(kb->row == 3);             // the moved module stays where it was dropped
    CHECK(osc->row == 5);            // OscB pushed below the keyboard (rows 3-4)
    CHECK(out->row == 10);           // and 2-Out below OscB
    edit::resolveOverlaps(p, Location::Va);
    CHECK(osc->row == 5);            // already consistent: nothing moves
}

TEST_CASE("patch settings read and write like the original editor")
{
    Patch p = Patch::makeDefault();
    using edit::Setting;
    // Defaults and display text through the original display functions.
    CHECK(edit::settingValue(p, Setting::Gain, 0, 0) == 100);
    CHECK(edit::settingText(p, Setting::Gain, 0, 0) == paramtext::formatSingle(118, 100));
    CHECK(edit::settingText(p, Setting::Bend, 1, 0) == paramtext::formatSingle(162, 1));

    edit::setSetting(p, Setting::Glide, 1, 3, 90);
    CHECK(edit::settingValue(p, Setting::Glide, 1, 3) == 90);
    CHECK(edit::settingValue(p, Setting::Glide, 1, 0) == 28); // other variations untouched
    edit::setSetting(p, Setting::Bend, 1, 0, 200);            // clamped to the range
    CHECK(edit::settingValue(p, Setting::Bend, 1, 0) == 23);

    edit::setVoices(p, edit::VoiceMode::Poly, 8);
    CHECK(edit::voicesText(p) == "8");
    edit::setVoices(p, edit::VoiceMode::Legato);
    CHECK(edit::voicesText(p) == "Legato");
    CHECK(p.header.voiceCount == 8); // kept for when the patch goes back to poly
    CHECK_THROWS_AS(edit::setVoices(p, edit::VoiceMode::Poly, 33), std::invalid_argument);

    edit::setCategory(p, 10);
    CHECK(std::string(edit::categoryName(p.header.category)) == "Pad");

    CHECK(edit::morphLabel(p, 0) == "Wheel");
    edit::setMorphLabel(p, 0, "Bright");
    CHECK(edit::morphLabel(p, 0) == "Bright");
    CHECK(edit::morphLabel(p, 1) == "Vel");

    const auto bytes = savePatch(p);
    const Patch again = loadPatch(bytes);
    CHECK(edit::morphLabel(again, 0) == "Bright");
    CHECK(edit::settingValue(again, Setting::Glide, 1, 3) == 90);
    CHECK(savePatch(again) == bytes);
}
