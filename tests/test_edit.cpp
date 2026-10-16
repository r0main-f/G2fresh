#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/param_text.hpp"
#include "g2/special.hpp"
#include "test_support.hpp"

#include <set>

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

TEST_CASE("morph, knob and MIDI assignments")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::setMorph(p, 0, Location::Va, osc, 0, 2, -40);
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 2) == std::int8_t{-40});
    CHECK(edit::morphsOf(p, 1, Location::Va, osc, 0).empty()); // per variation
    edit::setMorph(p, 0, Location::Va, osc, 0, 2, -30);         // the same group: no duplicate
    CHECK(p.morphs[0].assigns.size() == 1);
    CHECK(edit::morphRange(p, 0, Location::Va, osc, 0, 2) == std::int8_t{-30});

    edit::assignKnob(p, 9, Location::Va, osc, 0);
    CHECK(edit::knobOf(p, Location::Va, osc, 0) == 9);
    edit::assignKnob(p, 20, Location::Va, osc, 0); // moves to the new knob
    CHECK(edit::knobOf(p, Location::Va, osc, 0) == 20);
    CHECK_FALSE(p.knobs[9].has_value());
    CHECK(edit::knobName(0) == "A1-1"); // the original's "Page A1" (SMNU Knob menu, ids 2010-2024; manual p19)
    CHECK(edit::knobName(9) == "A2-2");
    CHECK(edit::knobName(119) == "E3-8");

    edit::assignMidiCc(p, 21, Location::Va, osc, 1);
    CHECK(edit::midiCcOf(p, Location::Va, osc, 1) == 21);
    edit::assignMidiCc(p, 21, Location::Va, osc, 2); // a CC drives one parameter
    CHECK_FALSE(edit::midiCcOf(p, Location::Va, osc, 1));

    // An oscillator's knobs have no label to rename (CanChangeName).
    CHECK_FALSE(edit::canRenameParam(p, Location::Va, osc, 0));
    CHECK_THROWS_AS(edit::setParamLabel(p, Location::Va, osc, 0, "Pitch"), std::invalid_argument);
    CHECK(edit::paramLabel(p, Location::Va, osc, 0).empty());
    CHECK_FALSE(p.va.find(osc)->customData.has_value());

    edit::removeModule(p, Location::Va, osc); // everything referring to it goes
    CHECK(p.morphs[0].assigns.empty());
    CHECK_FALSE(p.knobs[20].has_value());
    const auto bytes = savePatch(p);
    CHECK(savePatch(loadPatch(bytes)) == bytes);
}

TEST_CASE("copy a variation, copy and paste modules")
{
    Patch p = loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    const u8 first = p.va.modules.front().index;
    edit::setParam(p, Location::Va, first, 0, 0, 99);
    edit::copyVariation(p, 0, 4);
    CHECK(p.va.find(first)->params[4][0] == 99);

    // Copy two cabled modules and paste them in the FX area.
    const Cable c = p.va.cables.front();
    const auto clip = edit::copyModules(p, Location::Va, {c.fromModule, c.toModule});
    REQUIRE(clip.modules.size() == 2);
    REQUIRE_FALSE(clip.cables.empty());
    const auto fxBefore = p.fx.modules.size();
    const auto added = edit::pasteModules(p, Location::Fx, clip, 3, 10);
    CHECK(added.size() == 2);
    CHECK(p.fx.modules.size() == fxBefore + 2);
    bool cablePasted = false;
    for (const auto& fc : p.fx.cables)
        cablePasted |= std::find(added.begin(), added.end(), fc.fromModule) != added.end()
                       && std::find(added.begin(), added.end(), fc.toModule) != added.end();
    CHECK(cablePasted);
    // Pasting again in the same area renumbers names and indices.
    const auto again = edit::pasteModules(p, Location::Fx, clip, 3, 10);
    CHECK(again != added);
    CHECK(p.fx.find(again[0])->name != p.fx.find(added[0])->name);

    edit::moveModules(p, Location::Fx, added, 1, 2);
    edit::removeModules(p, Location::Fx, again);
    CHECK(p.fx.modules.size() == fxBefore + 2);
    const auto bytes = savePatch(p);
    CHECK(savePatch(loadPatch(bytes)) == bytes);
}

TEST_CASE("an edited patch saves although its file had non-zero pad bits")
{
    // Some files pad sections and cable lists with ones; an edit that changes
    // a section's length must not keep pad bits that no longer fit.
    Patch p = loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    p.sectionPad.fill(0x7F);
    p.va.cablePad = 0x7F;
    p.fx.cablePad = 0x7F;
    const auto clip = edit::copyModules(p, Location::Va, {p.va.modules.front().index});
    edit::pasteModules(p, Location::Va, clip, 0, 0);
    std::vector<u8> bytes;
    REQUIRE_NOTHROW(bytes = savePatch(p));
    CHECK(loadPatch(bytes).va.modules.size() == p.va.modules.size());
}

TEST_CASE("modules pushed past the last row stop there")
{
    Patch p = Patch::makeDefault();
    edit::addModule(p, Location::Va, kOscB, 0, 127);
    const u8 b = edit::addModule(p, Location::Va, kOscB, 0, 127); // resolves overlaps: must not hang
    edit::resolveOverlaps(p, Location::Va, b);
    CHECK(p.va.find(b)->row == 127);
}

TEST_CASE("a new note sequencer gets the original's default view")
{
    Patch p = Patch::makeDefault();
    const u8 seq = edit::addModule(p, Location::Va, special::kSeqNoteType, 0, 0);
    REQUIRE(p.va.find(seq)->customData);
    CHECK(*p.va.find(seq)->customData == std::vector<u8>{0, 1, 1, 0, 1, 5});
    CHECK(special::noteSeqView(p, Location::Va, seq) == special::NoteSeqView{1, 5});
    const Patch back = loadPatch(savePatch(p));
    CHECK(special::noteSeqView(back, Location::Va, seq) == special::NoteSeqView{1, 5});
}

TEST_CASE("cable bend points are editor-only")
{
    Patch p = loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    REQUIRE(!p.va.cables.empty());
    const auto original = savePatch(p);
    const Cable cable = p.va.cables.front();
    CHECK_FALSE(edit::hasCableBends(p));

    edit::setCableBend(p, Location::Va, cable, CableBend{40, -25});
    REQUIRE(p.va.cables.front().bend);
    CHECK(*p.va.cables.front().bend == CableBend{40, -25});
    CHECK(p.va.cables.front() == cable); // a cable is its colour and ends
    CHECK(edit::hasCableBends(p));
    CHECK(savePatch(p) == original);     // nothing reaches the .pch2

    // Copy/paste and colour changes keep the bend; removing it restores the curve.
    edit::recolorChain(p, Location::Va, edit::lowerEnd(p, Location::Va, cable), CableColor::Green);
    const auto recoloured = std::find_if(p.va.cables.begin(), p.va.cables.end(), [&](const Cable& c) {
        return c.fromModule == cable.fromModule && c.fromConn == cable.fromConn && c.toModule == cable.toModule
            && c.toConn == cable.toConn;
    });
    REQUIRE(recoloured != p.va.cables.end());
    CHECK(recoloured->color == CableColor::Green);
    CHECK(recoloured->bend);
    edit::clearCableBends(p);
    CHECK_FALSE(edit::hasCableBends(p));
    CHECK_THROWS_AS(edit::setCableBend(p, Location::Fx, Cable{CableColor::Red, 99, 0, true, 98, 0, std::nullopt},
                                       std::nullopt),
                    std::invalid_argument);
}

TEST_CASE("knob targets read and write module parameters and patch settings")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    const edit::Target pitch{Location::Va, osc, 0};
    edit::assignKnob(p, 9, Location::Va, osc, 0);
    REQUIRE(edit::knobTarget(p, 9));
    CHECK(edit::knobTarget(p, 9)->module == osc);
    CHECK_FALSE(edit::knobTarget(p, 10));
    CHECK(edit::targetMax(p, pitch) == 127);
    edit::setTargetValue(p, pitch, 2, 100);
    CHECK(edit::targetValue(p, pitch, 2) == 100);
    CHECK(edit::targetName(p, pitch) == "OscB1 " + std::string(p.va.find(osc)->def()->params[0].name));
    CHECK_FALSE(edit::targetText(p, pitch, 2).empty());

    const edit::Target wheel{Location::Settings, static_cast<u8>(edit::Setting::Morph), 0};
    edit::setTargetValue(p, wheel, 1, 90);
    CHECK(edit::targetValue(p, wheel, 1) == 90);
    CHECK(edit::targetMax(p, wheel) == 127);
    CHECK(edit::targetName(p, wheel) == "Morph " + edit::morphLabel(p, 0));
    CHECK_FALSE(edit::targetValue(p, {Location::Va, 99, 0}, 0));
}

// ---- Batch 1 of the editor fixes (re/notes/fidelity-audit.md, Part B) -------------------

TEST_CASE("push buttons: the momentary parameters of the original")
{
    // Param spec +0x0F: the 12 PANL push buttons (sequencer Clr/Rnd, the
    // momentary switches) and RndClkA's Dice.
    std::set<std::pair<int, std::string>> momentary;
    for (const auto& def : db::modules())
        for (const auto& prm : def.params)
            if (prm.momentary)
                momentary.emplace(def.typeId, prm.name);
    const std::set<std::pair<int, std::string>> expected{
        {36, "Switch"},  {121, "Random"}, {121, "Clear"}, {145, "Random"}, {145, "Clear"},
        {146, "Random"}, {146, "Clear"},  {154, "Random"}, {154, "Clear"}, {186, "Switch"},
        {187, "Switch"}, {188, "State"},  {204, "Dice"}};
    CHECK(momentary == expected);

    Patch p = Patch::makeDefault();
    const u8 seq = edit::addModule(p, Location::Va, 145, 0, 0); // SeqVal
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 1, 0);
    const auto& seqDef = *p.va.find(seq)->def();
    for (u8 i = 0; i < seqDef.params.size(); ++i)
        CHECK(edit::isMomentary(p, Location::Va, seq, i) == (std::string(seqDef.params[i].name) == "Random"
                                                             || std::string(seqDef.params[i].name) == "Clear"));
    CHECK_FALSE(edit::isMomentary(p, Location::Va, osc, 0));
    CHECK_FALSE(edit::isMomentary(p, Location::Settings, 1, 0));
}

TEST_CASE("MIDI controllers: the original's valid and pre-assigned CCs")
{
    for (int cc = 0; cc < 128; ++cc) {
        const bool invalid = cc >= 120 || cc == 0 || cc == 1 || cc == 11 || cc == 18 || cc == 32 || cc == 64
                          || cc == 70 || cc == 96 || cc == 97;
        CHECK(edit::isValidMidiCc(static_cast<u8>(cc)) == !invalid);
        CHECK(edit::isPreAssignedMidiCc(static_cast<u8>(cc)) == (cc == 7 || cc == 17));
    }

    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    for (u8 cc : {0, 1, 11, 64, 70, 120, 127})
        CHECK_THROWS_AS(edit::assignMidiCc(p, cc, Location::Va, osc, 1), std::invalid_argument);
    // CC 7 and 17 belong to the patch volume and octave shift: no other
    // parameter takes them, and those two have no MIDI item.
    CHECK_THROWS_AS(edit::assignMidiCc(p, 7, Location::Va, osc, 1), std::invalid_argument);
    CHECK_THROWS_AS(edit::assignMidiCc(p, 17, Location::Va, osc, 1), std::invalid_argument);
    CHECK(edit::midiCcOf(p, Location::Settings, 2, 0) == 7);
    CHECK(edit::midiCcOf(p, Location::Settings, 7, 0) == 17);
    CHECK_FALSE(edit::canAssignMidiCc(p, Location::Settings, 2, 0));
    CHECK_FALSE(edit::canAssignMidiCc(p, Location::Settings, 7, 0));
    CHECK_THROWS_AS(edit::assignMidiCc(p, 21, Location::Settings, 2, 0), std::invalid_argument);
    CHECK_THROWS_AS(edit::clearMidiCc(p, 7), std::invalid_argument);
    CHECK(edit::canAssignMidiCc(p, Location::Settings, 2, 1)); // the mute button can
    edit::assignMidiCc(p, 119, Location::Va, osc, 1);
    CHECK(edit::midiCcOf(p, Location::Va, osc, 1) == 119);

    // The 5 parameters that are not MIDI-assignable (param spec +0x10).
    int notAssignable = 0;
    for (const auto& def : db::modules())
        for (const auto& prm : def.params)
            notAssignable += prm.midiAssignable ? 0 : 1;
    CHECK(notAssignable == 5);
    const u8 send = edit::addModule(p, Location::Va, 141, 2, 0); // CtrlSend
    CHECK_FALSE(edit::canAssignMidiCc(p, Location::Va, send, 0));
    CHECK_THROWS_AS(edit::assignMidiCc(p, 21, Location::Va, send, 0), std::invalid_argument);
    CHECK(edit::canAssignMidiCc(p, Location::Va, send, 2)); // its channel

    // A mirror of the synth stores whatever the synth has.
    edit::storeMidiCc(p, 120, Location::Va, osc, 2);
    CHECK(edit::midiCcOf(p, Location::Va, osc, 2) == 120);
}

TEST_CASE("MIDI controllers: loading repairs the map as CCtrlMap::ValidateAndRepairMap")
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    CHECK_FALSE(edit::repairMidiCcs(p)); // the defaults (7, 17) are fine
    p.controllers.push_back({21, 1, osc, 1});
    p.controllers.push_back({21, 1, osc, 2});  // duplicate CC
    p.controllers.push_back({64, 1, osc, 3});  // sustain: not valid
    p.controllers.push_back({30, 1, 99, 0});   // no such module
    p.controllers.push_back({31, 1, osc, 1});  // duplicate parameter
    p.controllers.push_back({40, 1, osc, 4});
    CHECK(edit::repairMidiCcs(p));
    std::vector<int> ccs;
    for (const auto& c : p.controllers)
        ccs.push_back(c.cc);
    CHECK(ccs == std::vector<int>{7, 17, 21, 40});
    CHECK_FALSE(edit::repairMidiCcs(p));
}

TEST_CASE("names: the G2 character set")
{
    const std::string set = " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!\"#$%&'()*+,./:;<=>?@[\\]^_`{|}";
    for (int c = 0; c < 256; ++c)
        CHECK(edit::isModularChar(static_cast<char>(c)) == (c != 0 && set.find(static_cast<char>(c)) != std::string::npos));
    CHECK_FALSE(edit::isModularChar('~'));

    // Typed text: one space per character outside the set, then 16 characters.
    CHECK(edit::modularName("Caf\xc3\xa9 Bass~") == "Caf  Bass ");
    CHECK(edit::modularName("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                            "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9")
          == std::string(16, ' '));
    CHECK(edit::modularName("Lead\xe9 1") == "Lead  1"); // a Latin-1 byte counts as one character
    CHECK(edit::modularName("Strings 12345678901", 16) == "Strings 12345678");
    CHECK(edit::modularBytes("Pad\xe9\x01") == "Pad  ");
    CHECK(edit::fileNameForPatch("A/B:C*D?\"E<F>|") == "A B C D  E F  ");

    // An accented name can no longer make a performance unsaveable.
    Performance perf;
    for (auto& slot : perf.slots)
        slot = Patch::makeDefault();
    perf.header.slots[0].patchName = edit::modularName("\xc3\xa9t\xc3\xa9 \xc3\xa0 la plage du lac");
    CHECK(perf.header.slots[0].patchName.size() == 16);
    CHECK_NOTHROW(savePerformance(perf));

    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::renameModule(p, Location::Va, osc, "Ba\xc3\x9f~");
    CHECK(p.va.find(osc)->name == "Ba  ");
    CHECK_THROWS_AS(edit::renameModule(p, Location::Va, osc, "Seventeen chars!!"), std::invalid_argument);
    edit::setMorphLabel(p, 2, "M\xc3\xbc");
    CHECK(edit::morphLabel(p, 2) == "M ");
    p.va.find(osc)->name = "Osc\xe9";
    CHECK(edit::filterLoadedNames(p));
    CHECK(p.va.find(osc)->name == "Osc ");
    CHECK_FALSE(edit::filterLoadedNames(p));
}

TEST_CASE("labels: only label controls, written as the original's records")
{
    Patch p = Patch::makeDefault();
    // New modules with label controls carry their captions, as the original writes them.
    const u8 sw = edit::addModule(p, Location::Va, 90, 0, 0);    // Sw1-2: a radio, 2 buttons
    const u8 mix = edit::addModule(p, Location::Va, 123, 1, 0);  // Mix4-1C: 4 label buttons
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 2, 0);
    auto record = [](u8 param, std::initializer_list<const char*> captions) {
        std::vector<u8> r{1, static_cast<u8>(captions.size() * 7 + 1), param};
        for (const char* c : captions) {
            std::string s = c;
            s.resize(7, '\0');
            r.insert(r.end(), s.begin(), s.end());
        }
        return r;
    };
    REQUIRE(p.va.find(sw)->customData);
    CHECK(*p.va.find(sw)->customData == record(0, {"Out 1", "Out 2"}));
    std::vector<u8> mixData;
    for (u8 i = 0; i < 4; ++i) {
        const std::string ch = "Ch " + std::to_string(i + 1);
        const auto r = record(static_cast<u8>(4 + i), {ch.c_str()});
        mixData.insert(mixData.end(), r.begin(), r.end());
    }
    CHECK(*p.va.find(mix)->customData == mixData);
    CHECK_FALSE(p.va.find(osc)->customData);

    CHECK(edit::canRenameParam(p, Location::Va, sw, 0));
    CHECK(edit::canRenameParam(p, Location::Va, mix, 6));
    CHECK_FALSE(edit::canRenameParam(p, Location::Va, mix, 0)); // a level knob
    CHECK(edit::canRenameParam(p, Location::Settings, 1, 3));    // a morph knob
    CHECK_FALSE(edit::canRenameParam(p, Location::Settings, 1, 8));
    CHECK(edit::paramLabels(p, Location::Va, sw, 0) == std::vector<std::string>{"Out 1", "Out 2"});

    // Radio buttons: [1, n*7+1, param, n x 7 chars] (CPnlLabelRadioButton::GetCustomData).
    edit::setParamLabel(p, Location::Va, sw, 0, "Dry", 0);
    edit::setParamLabel(p, Location::Va, sw, 0, "W\xc3\xa9t", 1);
    CHECK(*p.va.find(sw)->customData == record(0, {"Dry", "W t"}));
    CHECK(edit::paramLabels(p, Location::Va, sw, 0) == std::vector<std::string>{"Dry", "W t"});
    CHECK_THROWS_AS(edit::setParamLabel(p, Location::Va, sw, 0, "x", 2), std::invalid_argument);

    // Label buttons: every control's record, in panel order, [1, 8, param, 7 chars].
    edit::setParamLabel(p, Location::Va, mix, 6, "Snare");
    mixData.clear();
    for (u8 i = 0; i < 4; ++i) {
        const std::string ch = i == 2 ? "Snare" : "Ch " + std::to_string(i + 1);
        const auto r = record(static_cast<u8>(4 + i), {ch.c_str()});
        mixData.insert(mixData.end(), r.begin(), r.end());
    }
    CHECK(*p.va.find(mix)->customData == mixData);
    CHECK(edit::paramLabel(p, Location::Va, mix, 6) == "Snare");
    edit::setParamLabel(p, Location::Va, mix, 6, ""); // back to the panel's caption
    CHECK(edit::paramLabel(p, Location::Va, mix, 6) == "Ch 3");

    // Morph knobs rename the morph group.
    edit::setParamLabel(p, Location::Settings, 1, 3, "Bright");
    CHECK(edit::morphLabel(p, 3) == "Bright");

    const auto bytes = savePatch(p);
    const Patch again = loadPatch(bytes);
    CHECK(edit::paramLabels(again, Location::Va, sw, 0) == std::vector<std::string>{"Dry", "W t"});
    CHECK(savePatch(again) == bytes);
}

TEST_CASE("module bar: where an inserted module goes (CTabButton::Action)")
{
    Patch p = Patch::makeDefault();
    CHECK(edit::insertPosition(p, Location::Va, {}) == std::pair<u8, u8>{0, 0});
    const u8 a = edit::addModule(p, Location::Va, kOscB, 0, 2);  // 5 rows high
    const u8 b = edit::addModule(p, Location::Va, kOscB, 1, 0);
    const u8 c = edit::addModule(p, Location::Va, kOscB, 1, 10);
    const int h = p.va.find(a)->def()->height;
    CHECK(edit::insertPosition(p, Location::Va, {a}) == std::pair<u8, u8>{0, static_cast<u8>(2 + h)});
    // The rightmost selected column, below the lowest selected module there.
    CHECK(edit::insertPosition(p, Location::Va, {a, b, c}) == std::pair<u8, u8>{1, static_cast<u8>(10 + h)});
    CHECK(edit::insertPosition(p, Location::Va, {b, a}) == std::pair<u8, u8>{1, static_cast<u8>(h)});
}

TEST_CASE("modules go only where the original allows them: Fx-In in the FX area")
{
    // Audit P3: IsValidContext / CPatchData::CutForMergingWith @000e60a2 keep a module out of an area its contexts
    // don't name (data/modules.json flags.contexts); Fx-In is FX-only.
    Patch p = Patch::makeDefault();
    const db::ModuleDef* fxIn = nullptr;
    const db::ModuleDef* driver = nullptr;
    for (const auto& d : db::modules()) {
        if (std::string(d.shortName) == "Fx-In")
            fxIn = &d;
        if (std::string(d.shortName) == "Driver")
            driver = &d;
    }
    REQUIRE(fxIn != nullptr);
    REQUIRE(driver != nullptr);
    CHECK(fxIn->contexts == 0b001);
    CHECK_THROWS_AS(edit::addModule(p, Location::Va, fxIn->typeId, 0, 0), std::invalid_argument);
    const u8 inFx = edit::addModule(p, Location::Fx, fxIn->typeId, 0, 0);
    // Pasting it into the voice area is refused too.
    const auto clip = edit::copyModules(p, Location::Fx, {inFx});
    CHECK_THROWS_AS(edit::pasteModules(p, Location::Va, clip, 0, 0), std::invalid_argument);
    CHECK(p.va.modules.empty());
    // A module whose contexts the data doesn't give goes anywhere.
    CHECK(driver->contexts == 0b111);
}

TEST_CASE("Performance Settings: a slot stays active, the keyboard moves off a disabled slot, lower <= upper")
{
    // Audit L11: CDialogPerformance::ToggleSlot @000397da, ToggleKeyb @0003986e, ValidateKeyboardRange @000398be.
    auto perf = Performance::makeNew(); // four slots active, the keyboard on A
    auto& h = perf.header;
    // Disabling the slot with the keyboard: the keyboard and the focus go to the first other active slot.
    CHECK(edit::toggleSlotEnabled(h, 0).empty());
    CHECK(h.slots[0].enabled == 0);
    CHECK(h.slots[0].keyboard == 0);
    CHECK(h.slots[1].keyboard == 1);
    CHECK(h.focusedSlot == 1);
    // The last active slot is not disabled.
    CHECK(edit::toggleSlotEnabled(h, 2).empty());
    CHECK(edit::toggleSlotEnabled(h, 3).empty());
    CHECK(edit::toggleSlotEnabled(h, 1) == "At least one slot must be active.");
    CHECK(h.slots[1].enabled == 1);
    // Enabling a slot focuses it.
    CHECK(edit::toggleSlotEnabled(h, 2).empty());
    CHECK(h.slots[2].enabled == 1);
    CHECK(h.focusedSlot == 2);
    // The keyboard: not off the only slot that has it; on and off elsewhere.
    CHECK(edit::toggleSlotKeyboard(h, 1) == "At least one slot must be active.");
    CHECK(h.slots[1].keyboard == 1);
    CHECK(edit::toggleSlotKeyboard(h, 2).empty());
    CHECK(h.slots[2].keyboard == 1);
    CHECK(edit::toggleSlotKeyboard(h, 1).empty());
    CHECK(h.slots[1].keyboard == 0);
    // The keyboard range: an upper end below the lower end pulls the lower end down.
    h.slots[0].kbdRangeLower = 60;
    h.slots[0].kbdRangeUpper = 40;
    edit::validateKeyboardRange(h, 0);
    CHECK(h.slots[0].kbdRangeLower == 40);
    CHECK(h.slots[0].kbdRangeUpper == 40);
}

TEST_CASE("a knob assigned to an LED is not a parameter's knob")
{
    // Audit K3: a knob target of type 1 is an LED (CKnobMap::GetAssignKnobToLedMolecules @000f1848 builds
    // CBarbTargetSpec(..., 1)); G2fresh read it as a parameter, so the DAW automated and the menus showed one.
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 7, 0, 0);
    p.knobs[5] = KnobAssign{static_cast<u8>(Location::Va), osc, 1, 0}; // assignType 1: the module's LED 0
    CHECK_FALSE(edit::knobTarget(p, 5));
    CHECK_FALSE(edit::knobOf(p, Location::Va, osc, 0));
    p.knobs[6] = KnobAssign{static_cast<u8>(Location::Va), osc, 0, 0}; // a parameter
    CHECK(edit::knobTarget(p, 6) == edit::Target{Location::Va, osc, 0});
    CHECK(edit::knobOf(p, Location::Va, osc, 0) == 6);
}

TEST_CASE("Init recalls the init variation into the variation shown")
{
    // Audit V2: the toolbar's Init button ("recall variation init", CPatchToolbar::HandleChangeRequests @000f622a)
    // sends CMParamSettingCopy(8, the focused variation): the init variation's values into the one shown.
    Patch p = Patch::makeDefault();
    const u8 mix = edit::addModule(p, Location::Va, 19, 0, 0);
    edit::setParam(p, Location::Va, mix, 0, 8, 33); // the init variation
    edit::setParam(p, Location::Va, mix, 0, 2, 100);
    edit::setParam(p, Location::Va, mix, 0, 3, 90);
    edit::recallVariationInit(p, 2);
    CHECK(p.va.find(mix)->params[2][0] == 33);
    CHECK(p.va.find(mix)->params[3][0] == 90); // other variations untouched
    CHECK(p.va.find(mix)->params[8][0] == 33);
}

TEST_CASE("Paste Params: the copied modules' values into the selected modules of the same types")
{
    // Audit P9 (manual p68, p100, p116: Edit > Paste Params, Cmd-E). CClipboard::ValidatePasteParams @0014d556: as many
    // modules as copied, the same types pair by pair, else "Selection does not match the original data". PasteParams
    // @0014d048 -> CPatchData::GetCopyParamsMolecules @000e9502: each copied module's values in the variation it was
    // copied from go into the current variation of the first selected module of its type, with its morphs.
    Patch p = Patch::makeDefault();
    const u8 mixA = edit::addModule(p, Location::Va, 19, 0, 0), envA = edit::addModule(p, Location::Va, 20, 0, 4);
    const u8 mixB = edit::addModule(p, Location::Va, 19, 2, 0), envB = edit::addModule(p, Location::Va, 20, 2, 4);
    edit::setParam(p, Location::Va, mixA, 0, 1, 99); // variation 2's values (index 1)
    edit::setParam(p, Location::Va, envA, 1, 1, 77);
    edit::setMorph(p, 1, Location::Va, mixA, 0, 3, 10);
    auto clip = edit::copyModules(p, Location::Va, {mixA, envA});
    clip.variation = 1;
    CHECK(edit::pasteParams(p, Location::Va, {envB, mixB}, clip, 4).empty());
    CHECK(p.va.find(mixB)->params[4][0] == 99);
    CHECK(p.va.find(envB)->params[4][1] == 77);
    CHECK(edit::morphRange(p, 4, Location::Va, mixB, 0, 3) == 10);
    CHECK(p.va.find(mixB)->params[1][0] != 99); // only the current variation
    // A selection that doesn't match: refused, nothing changes.
    CHECK(edit::pasteParams(p, Location::Va, {mixB}, clip, 5) == "Selection does not match the original data");
    const u8 osc = edit::addModule(p, Location::Va, 7, 4, 0);
    CHECK(edit::pasteParams(p, Location::Va, {osc, envB}, clip, 5) == "Selection does not match the original data");
    CHECK(p.va.find(envB)->params[5][1] != 77);
}

TEST_CASE("the toolbar's voices: the synth's count, then the patch's")
{
    // CPatchToolbar::SetupState @000f38b4: a poly patch shows its count; connected, "? (n)" until the synth says how
    // many it plays (CPatch::GetCurrentNumberOfVoices), then "actual (requested)", red when none; Mono or Legato.
    auto p = g2::Patch::makeDefault();
    edit::setVoices(p, edit::VoiceMode::Poly, 6);
    CHECK(edit::voicesShown(p, false, std::nullopt).text == "6");
    CHECK_FALSE(edit::voicesShown(p, false, std::nullopt).red);
    CHECK(edit::voicesShown(p, true, std::nullopt).text == "? (6)");
    CHECK_FALSE(edit::voicesShown(p, true, std::nullopt).red);
    CHECK(edit::voicesShown(p, true, 4).text == "4 (6)");
    CHECK_FALSE(edit::voicesShown(p, true, 4).red);
    CHECK(edit::voicesShown(p, true, 0).text == "0 (6)");
    CHECK(edit::voicesShown(p, true, 0).red);
    edit::setVoices(p, edit::VoiceMode::Mono);
    CHECK(edit::voicesShown(p, true, 1).text == "Mono");
    CHECK(edit::voicesShown(p, true, 0).red);
    edit::setVoices(p, edit::VoiceMode::Legato);
    CHECK(edit::voicesShown(p, false, std::nullopt).text == "Legato");
}

TEST_CASE("module Assign: a module's parameters onto a knob page")
{
    // CKnobMap::GetAssignModuleToPageMolecules @000f24d4 and CKnobAssignMenu::GetNoOfAssignableParams @000bb8b4: the
    // parameters in the module's edit order, without those that are another parameter's button (param spec +0x0E);
    // knob page * 8 + i; at most 8 unless Multi Column, which runs on into the next pages; a knob's old target and the
    // parameter's old knob are freed first.
    const auto* oscB = g2::db::find(7);
    REQUIRE(oscB != nullptr);
    CHECK(edit::assignableParams(*oscB) == std::vector<u8>{0, 1, 3, 8, 6, 7, 5, 9});

    auto p = g2::Patch::makeDefault();
    const auto osc = edit::dropModule(p, Location::Va, 7, 0, 0);
    const auto seq = edit::dropModule(p, Location::Va, 121, 2, 0);
    edit::assignKnob(p, 9, Location::Va, seq, 0); // page A2, knob 2: freed by the assignment below
    edit::assignKnob(p, 40, Location::Va, osc, 0); // the oscillator's first parameter elsewhere: moves
    CHECK_FALSE(edit::isKnobPageUsed(p, 2));
    edit::assignModuleToPage(p, Location::Va, osc, 1, false); // Page A, Column 2
    CHECK(edit::knobOf(p, Location::Va, osc, 0) == 8);
    CHECK(edit::knobOf(p, Location::Va, osc, 1) == 9);
    CHECK(edit::knobOf(p, Location::Va, osc, 9) == 15);
    CHECK_FALSE(edit::knobOf(p, Location::Va, osc, 2)); // a button parameter
    CHECK_FALSE(edit::knobOf(p, Location::Va, seq, 0));
    CHECK_FALSE(p.knobs[40].has_value());
    CHECK(edit::isKnobPageUsed(p, 1));

    const auto* seqNote = g2::db::find(121);
    REQUIRE(edit::assignableParams(*seqNote).size() == 21);
    edit::assignModuleToPage(p, Location::Va, seq, 3, false); // Page B, Column 1: the first 8 only
    CHECK(edit::knobOf(p, Location::Va, seq, edit::assignableParams(*seqNote)[7]) == 31);
    CHECK_FALSE(edit::knobOf(p, Location::Va, seq, edit::assignableParams(*seqNote)[8]));
    edit::assignModuleToPage(p, Location::Va, seq, 3, true); // Multi Column: 21 knobs from B1
    CHECK(edit::knobOf(p, Location::Va, seq, edit::assignableParams(*seqNote)[20]) == 24 + 20);
}

TEST_CASE("a new module takes the toolbar's colour")
{
    // Manual p60, p82: "the color selector stays in its new selection, causing any new modules that you add to the
    // Patch window to get the selected color".
    auto p = g2::Patch::makeDefault();
    const auto a = edit::addModule(p, Location::Va, 7, 0, 0);
    CHECK(p.va.find(a)->color == 0);
    const auto b = edit::addModule(p, Location::Va, 7, 0, 4, 9);
    CHECK(p.va.find(b)->color == 9);
    const auto c = edit::dropModule(p, Location::Va, 7, 0, 8, 3);
    CHECK(p.va.find(c)->color == 3);
    CHECK_THROWS_AS(edit::addModule(p, Location::Va, 7, 0, 12, 25), std::invalid_argument); // 25 colours: 0..24
}
