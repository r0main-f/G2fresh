#include "g2/edit.hpp"
#include "g2/patch.hpp"
#include "g2/replace.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>

using namespace g2;

namespace {

constexpr u8 kOscA = 97, kOscB = 7, kOscC = 9, kLfoA = 26, kLfoB = 190, kLfoC = 24, kFxIn = 127;

bool hasCable(const Area& a, u8 from, u8 fromConn, bool fromIsOutput, u8 to, u8 toConn)
{
    return std::any_of(a.cables.begin(), a.cables.end(), [&](const Cable& c) {
        return c.fromModule == from && c.fromConn == fromConn && c.fromIsOutput == fromIsOutput
            && c.toModule == to && c.toConn == toConn;
    });
}

const Cable* cableTo(const Area& a, u8 to, u8 toConn)
{
    for (const auto& c : a.cables)
        if (c.toModule == to && c.toConn == toConn)
            return &c;
    return nullptr;
}

} // namespace

TEST_CASE("replace: groups and candidates in menu order")
{
    REQUIRE(replace::groups().size() == 19);
    CHECK(std::string(replace::groups().front().name) == "Shaper Group");

    CHECK(replace::candidates(kLfoA) == std::vector<u8>{26, 190, 24, 25, 68});
    CHECK(replace::candidates(kOscB) == std::vector<u8>{97, 7, 9, 96, 183, 163, 8, 164, 106, 31, 29, 196, 58, 13, 180, 27});
    CHECK(replace::candidates(4) == std::vector<u8>{4, 3});         // Output group: 2-Out, 4-Out
    CHECK(replace::candidates(1) == std::vector<u8>{1, 199});       // Keyboard group
    CHECK(replace::candidates(kFxIn) == std::vector<u8>{170, 171, 127});
    CHECK(replace::candidates(126).empty());                        // Name bar: no group

    const auto items = replace::menu(kLfoA, Location::Va);
    REQUIRE(items.size() == 5);
    CHECK(std::string(items[1].label) == "LFO B");
    CHECK_FALSE(items[0].enabled); // the module's own type
    CHECK(items[1].enabled);

    // Fx-In exists only in the FX area.
    CHECK_FALSE(replace::canReplace(170, kFxIn, Location::Va));
    CHECK(replace::canReplace(170, kFxIn, Location::Fx));
    CHECK_FALSE(replace::canReplace(kOscA, kOscA, Location::Va));
    CHECK_FALSE(replace::canReplace(kOscA, kLfoA, Location::Va));
}

TEST_CASE("replace: correspondence tables")
{
    // OscA -> OscB with the pitch-variation input connected.
    const std::vector<u8> ins{1}, outs{0};
    const auto map = replace::mapping(kOscA, kOscB, ins, outs);
    CHECK(map.inputs == std::vector<u8>{replace::kNone, 1});
    CHECK(map.outputs == std::vector<u8>{0});
    // Coarse, Fine, KBT, Pitch M, Wave, On/Off, Tune Md
    CHECK(map.params == std::vector<u8>{0, 1, 2, 3, 8, 9, 4});

    // LfoB -> LfoC: no reset input, no sync output, no KBT.
    const std::vector<u8> lins{0, 2}, louts{0, 1};
    const auto lfo = replace::mapping(kLfoB, kLfoC, lins, louts);
    CHECK(lfo.inputs == std::vector<u8>{0, replace::kNone, replace::kNone, replace::kNone});
    CHECK(lfo.outputs == std::vector<u8>{0, replace::kNone});
    CHECK(lfo.params[0] == 0);              // Rate
    CHECK(lfo.params[3] == replace::kNone); // KBT
}

TEST_CASE("replace: OscA -> OscB in a corpus patch")
{
    Patch p = loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    Area& va = p.va;
    const Module before = *va.find(1); // OscA1, Out -> Mix4-1B1 In1
    REQUIRE(before.type == kOscA);

    // Give it more to carry: a pitch modulation cable through a link, values
    // in two variations, a morph, a knob and a MIDI controller.
    edit::connect(p, Location::Va, {8, 0, true}, {1, 1, false});  // Keyboard Pitch -> PitchVar
    edit::connect(p, Location::Va, {1, 1, false}, {2, 1, false}); // PitchVar -> OscA2 PitchVar (link)
    edit::setParam(p, Location::Va, 1, 0, 0, 40);  // Coarse
    edit::setParam(p, Location::Va, 1, 0, 3, 90);  // Coarse, variation 4
    edit::setParam(p, Location::Va, 1, 3, 0, 100); // Pitch M
    edit::setParam(p, Location::Va, 1, 4, 0, 5);   // Wave (0..5, OscB's is 0..4)
    edit::setParam(p, Location::Va, 1, 6, 0, 2);   // Tune Md
    edit::setMorph(p, 0, Location::Va, 1, 0, 2, -40);
    edit::setMorph(p, 1, Location::Va, 1, 6, 1, 3);
    edit::assignKnob(p, 5, Location::Va, 1, 1);
    edit::assignMidiCc(p, 20, Location::Va, 1, 2);
    p = loadPatch(savePatch(p));

    const u8 n = replace::replaceModule(p, Location::Va, 1, kOscB);
    CHECK(n == 9); // the lowest free index, the old one still counting
    REQUIRE(va.find(1) == nullptr);
    const Module* m = va.find(n);
    REQUIRE(m);
    CHECK(m->type == kOscB);
    CHECK(m->name == "OscB1");
    CHECK(m->col == before.col);
    CHECK(m->row == before.row);
    CHECK(m->color == before.color);

    // Values: copied where the ranges match, default otherwise.
    CHECK(m->params[0][0] == 40);
    CHECK(m->params[3][0] == 90);
    CHECK(m->params[0][3] == 100);
    CHECK(m->params[0][4] == 2);                 // Tune Md
    CHECK(m->params[0][8] == db::find(kOscB)->params[8].defaultValue); // Wave: other range
    CHECK(m->params.size() == p.variationCount);

    // Cables.
    CHECK(hasCable(va, n, 0, true, 4, 1));       // Out -> Mix4-1B1 In1
    CHECK(hasCable(va, 8, 0, true, n, 1));       // Keyboard -> PitchVar
    CHECK(hasCable(va, n, 1, false, 2, 1));      // the link follows
    CHECK(cableTo(va, 4, 1)->color == CableColor::Red);
    for (const auto& c : va.cables)
        CHECK((c.fromModule != 1 && c.toModule != 1));

    // Assignments.
    CHECK(edit::morphOf(p, 0, Location::Va, n, 0)->group == 2);
    CHECK(edit::morphOf(p, 0, Location::Va, n, 0)->range == -40);
    CHECK(edit::morphOf(p, 1, Location::Va, n, 4)->group == 1); // Tune Md moved to index 4
    CHECK(edit::knobOf(p, Location::Va, n, 1) == 5);
    CHECK(edit::midiCcOf(p, Location::Va, n, 2) == u8{20});

    // Saves and reloads to the same model.
    const auto bytes = savePatch(p);
    const Patch q = loadPatch(bytes);
    CHECK(savePatch(q) == bytes);
    REQUIRE(q.va.find(n));
    CHECK(q.va.find(n)->params == m->params);
    CHECK(q.va.cables.size() == va.cables.size());
    CHECK(q.va.find(n)->name == "OscB1");
}

TEST_CASE("replace: unmatched connectors, bridging and names")
{
    Patch p = Patch::makeDefault();
    const auto va = Location::Va;
    const u8 kb = edit::addModule(p, va, 1, 0, 0);        // Keyboard
    const u8 lfo = edit::addModule(p, va, kLfoB, 1, 0);   // LfoB1
    const u8 osc = edit::addModule(p, va, kOscC, 2, 0);   // OscC1
    const u8 out = edit::addModule(p, va, 4, 3, 0);       // 2-Out
    edit::connect(p, va, {kb, 1, true}, {lfo, 2, false});  // Gate -> Rst (LfoC has none)
    edit::connect(p, va, {lfo, 2, false}, {osc, 2, false}); // Rst -> OscC FmMod (link through Rst)
    edit::connect(p, va, {lfo, 1, true}, {osc, 1, false}); // Sync out (LfoC has none) -> OscC Sync
    edit::connect(p, va, {lfo, 0, true}, {out, 0, false}); // Out -> 2-Out InL
    edit::renameModule(p, va, lfo, "Wobble");

    const u8 n = replace::replaceModule(p, va, lfo, kLfoC);
    const Area& a = p.va;
    CHECK(a.find(n)->name == "Wobble");          // custom names are kept
    CHECK(hasCable(a, n, 0, true, out, 0));
    CHECK(hasCable(a, kb, 1, true, osc, 2));     // bridged around the lost Rst input
    CHECK(cableTo(a, osc, 1) == nullptr);        // the lost Sync output's cable is dropped
    CHECK(a.cables.size() == 2);

    CHECK_THROWS_AS(replace::replaceModule(p, va, n, kOscA), std::invalid_argument);
    CHECK_THROWS_AS(replace::replaceModule(p, va, 99, kLfoA), std::invalid_argument);
    CHECK_THROWS_AS(replace::replaceModule(p, va, n, kLfoC), std::invalid_argument);

    // A default name takes the new type's first free number.
    const u8 m2 = edit::addModule(p, va, kLfoA, 4, 0); // LfoA1
    const u8 r = replace::replaceModule(p, va, m2, kLfoC);
    CHECK(p.va.find(r)->name == "LfoC1");
}
