// Live editing without a synth: the molecules each edit records and sends, in the original editor's order
// (re/notes/usb-protocol.md §9, the decompiled CPatchData::Get*Molecules), their undo, what LiveSlot adds by
// comparison, and how the client handles the synth's side (load reports, knob-map notices, variation 0x7F, the
// performance mode).
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/live.hpp"
#include "g2/module_db.hpp"
#include "g2/proto/bubble.hpp"
#include "g2/proto/client.hpp"
#include "g2/proto/emulator.hpp"
#include "g2/proto/live.hpp"
#include "g2/replace.hpp"
#include "proto_vectors.hpp"

#include <algorithm>
#include <functional>
#include <string>

using namespace g2::proto;
using g2::Location;
using g2::u8;
using g2test::hexOf;

namespace {

u8 typeOf(const char* name)
{
    for (const auto& m : g2::db::modules())
        if (m.kind == g2::db::ModuleKind::Module && std::string(m.shortName) == name)
            return m.typeId;
    throw std::runtime_error(name);
}

struct Fixture {
    g2::Patch p = g2::Patch::makeDefault();
    u8 kbd, osc, env, out, lfo, mix;
    Fixture()
    {
        kbd = g2::edit::addModule(p, Location::Va, typeOf("Keyboard"), 0, 0);
        osc = g2::edit::addModule(p, Location::Va, typeOf("OscA"), 1, 0);
        env = g2::edit::addModule(p, Location::Va, typeOf("EnvADSR"), 2, 0);
        out = g2::edit::addModule(p, Location::Va, typeOf("2-Out"), 3, 0);
        lfo = g2::edit::addModule(p, Location::Va, typeOf("LfoA"), 0, 6);
        mix = g2::edit::addModule(p, Location::Va, typeOf("Mix4-1B"), 1, 8);
        g2::edit::connect(p, Location::Va, {osc, 0, true}, {env, 0, false});
        g2::edit::connect(p, Location::Va, {kbd, 1, true}, {env, 1, false});
        g2::edit::connect(p, Location::Va, {env, 1, true}, {out, 0, false});
        g2::edit::connect(p, Location::Va, {out, 0, false}, {out, 1, false});
    }
};

// Runs an edit on a copy under a recorder.
g2::live::Edit recorded(g2::Patch& p, const std::function<void(g2::Patch&)>& f)
{
    g2::live::Recorder r;
    f(p);
    return r.edit();
}

// The molecule ids, as hex.
std::string ids(const std::vector<g2::live::Op>& ops)
{
    std::vector<u8> out;
    for (const auto& m : toMolecules(ops))
        out.push_back(idOf(m));
    return hexOf(out);
}

std::string bytes(const g2::live::Op& op)
{
    std::vector<u8> out;
    encode(toMolecule(op), out);
    return hexOf(out);
}

} // namespace

TEST_CASE("live: a new cable is 50 from the drag's origin to its target, after the other net's recolours")
{
    Fixture f;
    // LFO output -> mixer input: one 50, not part of a dump (bit 0x10), VA (0x08), the LFO's blue (1).
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::connect(p, Location::Va, {f.lfo, 0, true}, {f.mix, 0, false}); });
    REQUIRE(ids(e.redo()) == "50");
    CHECK(bytes(e.redo()[0]) == hexOf(std::vector<u8>{0x50, 0x19, f.lfo, 0x40, f.mix, 0x00}));
    CHECK(ids(e.undo()) == "51");
    CHECK(bytes(e.undo()[0]) == hexOf(std::vector<u8>{0x51, 0x03, f.lfo, 0x40, f.mix, 0x00}));
    // From an input into a net fed by an output: the input's net takes the output's colour first (54s), then 50.
    e = recorded(f.p, [&](g2::Patch& p) {
        g2::edit::connect(p, Location::Va, {f.mix, 1, false}, {f.mix, 2, false});
        g2::edit::connect(p, Location::Va, {f.mix, 2, false}, {f.osc, 0, true});
    });
    // (the mixer then runs at audio rate: 2A after them)
    CHECK(ids(e.redo()) == "50 54 50 2A");
    CHECK(ids(e.undo()) == "2A 51 54 51");
    // An audio output into a dynamic input: the module's rate (2A) and its outputs' nets (54) follow the 50.
    g2::Patch q = f.p;
    e = recorded(q, [&](g2::Patch& p) { g2::edit::connect(p, Location::Va, {f.osc, 0, true}, {f.lfo, 0, false}); });
    const auto redo = ids(e.redo());
    CHECK(redo.rfind("50", 0) == 0);
    if (redo.find("2A") != std::string::npos)
        CHECK(redo.find("2A") > redo.find("50"));
}

TEST_CASE("live: cable deletes go child -> parent (the input first), undone by 50s with the colours")
{
    Fixture f;
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::deleteChain(p, Location::Va, {f.env, 1, true}); });
    // The chain from the envelope's output: env out -> 2-Out L, then the link L -> R.
    REQUIRE(ids(e.redo()) == "51 51");
    CHECK(bytes(e.redo()[0]) == hexOf(std::vector<u8>{0x51, 0x03, f.out, 0x00, f.env, 0x41}));
    CHECK(bytes(e.redo()[1]) == hexOf(std::vector<u8>{0x51, 0x03, f.out, 0x01, f.out, 0x00}));
    REQUIRE(ids(e.undo()) == "50 50");
    // Reversed, with the red of the envelope's (uprated, audio) output.
    CHECK(bytes(e.undo()[0]) == hexOf(std::vector<u8>{0x50, 0x18, f.out, 0x01, f.out, 0x00}));
    CHECK(bytes(e.undo()[1]) == hexOf(std::vector<u8>{0x50, 0x18, f.out, 0x00, f.env, 0x41}));
}

TEST_CASE("live: deleting modules: assignments, the cables as a dump, the modules, then the rates")
{
    Fixture f;
    g2::edit::assignKnob(f.p, 5, Location::Va, f.env, 1);
    g2::edit::assignMidiCc(f.p, 20, Location::Va, f.env, 2);
    g2::edit::setMorph(f.p, 0, Location::Va, f.env, 1, 3, 20);
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::removeModules(p, Location::Va, {f.env}); });
    const auto redo = ids(e.redo());
    INFO(redo);
    // 26, 23, 43 (range 0); the envelope's net turns white (54) and loses its cables (51, bit 0) and the
    // other nets' links to it (51); then 32.
    CHECK(redo.rfind("26 23 43", 0) == 0);
    CHECK(redo.find("32") > redo.find("51"));
    CHECK(redo.find("54") < redo.rfind("51")); // the output's net: white, then its cables
    for (const auto& op : e.redo())
        if (const auto* d = std::get_if<g2::live::CableDelete>(&op))
            CHECK(d->dump);
    // The undo re-creates the module (30 33 42 4D), its cables, then the assignments.
    const auto undo = ids(e.undo());
    INFO(undo);
    CHECK(undo.find("30 33 42 4D") != std::string::npos);
    CHECK(undo.find("30") < undo.find("50"));
    CHECK(undo.substr(undo.size() - 8) == "43 22 25");
}

TEST_CASE("live: new modules: 30 with its sections, the make-room moves after a click and before a drop")
{
    Fixture f;
    // On top of the oscillator (1, 0): it and the envelope's column neighbours move down.
    auto e = recorded(f.p, [&](g2::Patch& p) {
        const u8 added = g2::edit::addModule(p, Location::Va, typeOf("OscB"), 1, 0);
        g2::edit::resolveOverlaps(p, Location::Va, added);
    });
    INFO(ids(e.redo()));
    CHECK(ids(e.redo()).rfind("30 52 4D 5A 34", 0) == 0);
    CHECK(ids(e.undo()).substr(ids(e.undo()).size() - 2) == "32");
    g2::Patch q = f.p;
    e = recorded(q, [&](g2::Patch& p) { g2::edit::dropModule(p, Location::Va, typeOf("OscB"), 1, 0); });
    INFO(ids(e.redo()));
    CHECK(ids(e.redo()).rfind("34", 0) == 0);
    CHECK(ids(e.redo()).find("30 52 4D 5A") != std::string::npos);
    // A module with label controls brings its captions (5B).
    q = f.p;
    e = recorded(q, [&](g2::Patch& p) { g2::edit::addModule(p, Location::Fx, typeOf("SwOnOffM"), 0, 0); });
    CHECK(ids(e.redo()) == "30 52 4D 5B 5A");
}

TEST_CASE("live: a paste is 30s, 52, 4D, the morphs (43), 5B, 5A; the undo deletes its cables as a dump")
{
    Fixture f;
    g2::edit::setMorph(f.p, 1, Location::Va, f.osc, 0, 2, 25);
    const auto clip = g2::edit::copyModules(f.p, Location::Va, {f.osc, f.env});
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::pasteModules(p, Location::Va, clip, 6, 0); });
    CHECK(ids(e.redo()) == "30 30 52 4D 43 5A");
    CHECK(ids(e.undo()) == "43 51 32 32");
    for (const auto& op : e.undo())
        if (const auto* d = std::get_if<g2::live::CableDelete>(&op))
            CHECK(d->dump);
}

TEST_CASE("live: moves, names, colours, locks, modes, as the original's single molecules")
{
    Fixture f;
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::moveModules(p, Location::Va, {f.lfo}, 0, -6); });
    // The keyboard makes room first, then the LFO moves.
    CHECK(ids(e.redo()) == "34 34");
    CHECK(std::get<g2::live::ModuleMove>(e.redo().back()).module == f.lfo);
    e = recorded(f.p, [&](g2::Patch& p) { g2::edit::renameModule(p, Location::Va, f.osc, "Lead"); });
    CHECK(bytes(e.redo()[0]) == hexOf(std::vector<u8>{0x33, 0x01, f.osc, 'L', 'e', 'a', 'd', 0}));
    e = recorded(f.p, [&](g2::Patch& p) {
        g2::edit::setModuleColor(p, Location::Va, f.osc, 3);
        g2::edit::setModuleColor(p, Location::Va, f.env, 0); // unchanged, still sent
    });
    CHECK(ids(e.redo()) == "31 31");
    e = recorded(f.p, [&](g2::Patch& p) { g2::edit::setLocked(p, Location::Va, f.osc, true); });
    CHECK(bytes(e.redo()[0]) == hexOf(std::vector<u8>{0x90, 0x01, f.osc, 0x01}));
    CHECK(bytes(e.undo()[0]) == hexOf(std::vector<u8>{0x90, 0x01, f.osc, 0x00}));
}

TEST_CASE("live: knob and controller assignments in the original's order")
{
    Fixture f;
    g2::edit::assignKnob(f.p, 2, Location::Va, f.osc, 0);
    g2::edit::assignKnob(f.p, 9, Location::Va, f.env, 1);
    // The oscillator's parameter onto knob 9: 26 for its knob (2), 26 for knob 9's target, 25, 2D page 1.
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::assignKnob(p, 9, Location::Va, f.osc, 0); });
    CHECK(ids(e.redo()) == "26 26 25 2D");
    CHECK(bytes(e.redo()[2]) == hexOf(std::vector<u8>{0x25, f.osc, 0x00, 0x40, 0x00, 0x09}));
    CHECK(bytes(e.redo()[3]) == "2D 01");
    CHECK(ids(e.undo()) == "2D 26 25 25");
    g2::edit::assignMidiCc(f.p, 20, Location::Va, f.osc, 0);
    g2::edit::assignMidiCc(f.p, 21, Location::Va, f.env, 1);
    e = recorded(f.p, [&](g2::Patch& p) { g2::edit::assignMidiCc(p, 21, Location::Va, f.osc, 0); });
    CHECK(ids(e.redo()) == "23 23 22");
    CHECK(ids(e.undo()) == "23 22 22");
}

TEST_CASE("live: the morph menu is a realtime pair; a variation copy is one 44 without undo")
{
    Fixture f;
    const u8 coarse = 0;
    auto e = recorded(f.p, [&](g2::Patch& p) { g2::edit::toggleMorphAssign(p, 0, Location::Va, f.osc, coarse, 2); });
    CHECK(e.realtime);
    CHECK(ids(e.redo()) == "40 43"); // value 0, then the full range
    CHECK(ids(e.undo()) == "43 40");
    e = recorded(f.p, [&](g2::Patch& p) { g2::edit::toggleMorphAssign(p, 0, Location::Va, f.osc, coarse, 2); });
    CHECK(ids(e.redo()) == "43");
    e = recorded(f.p, [&](g2::Patch& p) { g2::edit::copyVariation(p, 2, 5); });
    CHECK(ids(e.redo()) == "44");
    CHECK(e.undo().empty());
}

TEST_CASE("live: LiveSlot sends the rest by comparison, as the original sends each kind")
{
    Fixture f;
    LiveSlot slot(f.p, "Live");
    g2::Patch now = f.p;
    // A knob turned: one realtime 40, in its variation.
    g2::edit::setParam(now, Location::Va, f.osc, 0, 3, 70);
    auto out = slot.update(now, "Live", nullptr, false, 0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].realtime);
    CHECK(hexOf(encode(out[0].molecules)) == hexOf(std::vector<u8>{0x40, 0x01, f.osc, 0x00, 70, 0x03}));
    // A module name and the variation: one bubble.
    g2::edit::renameModule(now, Location::Va, f.lfo, "Wobble");
    now.header.activeVariation = 2;
    out = slot.update(now, "Live", nullptr, false, 0);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].realtime);
    // The category goes at once (21); the voice count 600 ms after its last change.
    g2::edit::setCategory(now, 4);
    out = slot.update(now, "Live", nullptr, false, 1000);
    REQUIRE(out.size() == 1);
    CHECK(idOf(out[0].molecules[0]) == 0x21);
    g2::edit::setVoices(now, g2::edit::VoiceMode::Poly, 6);
    CHECK(slot.update(now, "Live", nullptr, false, 2000).empty());
    g2::edit::setVoices(now, g2::edit::VoiceMode::Poly, 7);
    CHECK(slot.update(now, "Live", nullptr, false, 2300).empty());
    CHECK(slot.tick(now, 2800).empty());
    out = slot.tick(now, 2900);
    REQUIRE(out.size() == 1);
    CHECK(idOf(out[0].molecules[0]) == 0x21);
    CHECK_FALSE(slot.headerPending());
    // Notes (6F) and the name (27).
    now.notes = "Hello";
    out = slot.update(now, "Renamed", nullptr, false, 3000);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].molecules.size() == 2);
    CHECK(idOf(out[0].molecules[0]) == 0x6F);
    CHECK(hexOf(encode(std::span<const Molecule>(out[0].molecules).subspan(1))) == "27 52 65 6E 61 6D 65 64 00");
    CHECK_FALSE(slot.needsUpload());
    // A module added without the edit's record: only the whole patch can do.
    g2::edit::addModule(now, Location::Fx, typeOf("Reverb"), 0, 0);
    slot.update(now, "Renamed", nullptr, false, 4000);
    CHECK(slot.needsUpload());
}

TEST_CASE("live: a recorded step and its undo through LiveSlot leave the mirror equal to the document")
{
    Fixture f;
    LiveSlot slot(f.p, "Live");
    std::vector<std::pair<g2::Patch, g2::live::Edit>> steps;
    auto step = [&](const std::function<void(g2::Patch&)>& fn) {
        g2::Patch next = slot.synth();
        auto e = recorded(next, fn);
        const g2::Patch before = steps.empty() ? f.p : steps.back().first;
        (void)before;
        slot.update(next, "Live", &e, false, 0);
        CHECK_FALSE(slot.needsUpload());
        steps.emplace_back(next, e);
    };
    const g2::Patch start = f.p;
    step([&](g2::Patch& p) { g2::edit::connect(p, Location::Va, {f.lfo, 0, true}, {f.mix, 0, false}); });
    step([&](g2::Patch& p) { g2::edit::removeModules(p, Location::Va, {f.env}); });
    step([&](g2::Patch& p) { g2::replace::replaceModule(p, Location::Va, f.osc, typeOf("OscB")); });
    // Undo all three, newest first.
    for (std::size_t i = steps.size(); i-- > 0;) {
        const g2::Patch& target = i == 0 ? start : steps[i - 1].first;
        slot.update(target, "Live", &steps[i].second, true, 0);
        CHECK_FALSE(slot.needsUpload());
    }
}

TEST_CASE("live: the client asks for the load after a structural edit, and a 70 after each report")
{
    Emulator emulator;
    EmulatorTransport link(emulator);
    ManualClock clock;
    Client client(link, clock);
    link.plugIn();
    for (int i = 0; i < 3; ++i)
        client.tick();
    REQUIRE(client.synced());
    const auto from = emulator.received().size();
    // A module added in VA (one bubble), then CPatch::CleanDirtyData's 71 01 and, after its 72, 70.
    g2::Module m;
    m.index = 9;
    m.type = typeOf("LfoA");
    client.edit(0, {ModuleNew{m.type, 1, m.index, 0, 20, 0, 0, 0, {}, "LfoA9"}});
    for (int i = 0; i < 4; ++i)
        client.tick();
    std::vector<std::string> sent;
    for (auto i = from; i < emulator.received().size(); ++i) {
        const auto& f = emulator.received()[i];
        sent.push_back(hexOf(std::vector<u8>(f.molecules.begin(), f.molecules.begin() + std::min<std::size_t>(2, f.molecules.size()))));
    }
    CHECK(sent == std::vector<std::string>{hexOf(std::vector<u8>{0x30, typeOf("LfoA")}), "71 01", "70"});
}

TEST_CASE("live: knob-map notices, every-variation values and the performance mode from the synth's side")
{
    Emulator emulator;
    EmulatorTransport link(emulator);
    ManualClock clock;
    Client client(link, clock);
    link.plugIn();
    for (int i = 0; i < 3; ++i)
        client.tick();
    REQUIRE(client.synced());
    // 74: the slot's knob map again (63); 75: the global one (5E).
    auto from = emulator.received().size();
    emulator.state().slots[1].patch.knobs[7] = g2::file::KnobAssign{2, 1, 0, 0, 0};
    emulator.notify(1, {Request{id::KnobMapChanged}});
    emulator.notify(kSlotSynth, {Request{id::GlobalKnobMapChanged}});
    for (int i = 0; i < 4; ++i)
        client.tick();
    std::vector<u8> asked;
    for (auto i = from; i < emulator.received().size(); ++i)
        asked.push_back(emulator.received()[i].molecules.at(0));
    CHECK(std::count(asked.begin(), asked.end(), u8{0x63}) == 1);
    CHECK(std::count(asked.begin(), asked.end(), u8{0x5E}) == 1);
    CHECK(client.state().slots[1].patch.knobs[7].has_value());
    // A 40 for variation 0x7F sets every variation (CPatch::HandleParamChange).
    const g2::Patch p = [] {
        g2::Patch x = g2::Patch::makeDefault();
        g2::edit::addModule(x, Location::Va, typeOf("OscA"), 0, 0);
        return x;
    }();
    client.sendPatch(2, p, "All");
    for (int i = 0; i < 4; ++i)
        client.tick();
    emulator.notify(2, {ParamChange{1, 1, 0, 33, 0x7F}});
    for (int i = 0; i < 2; ++i)
        client.tick();
    for (const auto& v : client.state().slots[2].patch.va.find(1)->params)
        CHECK(v[0] == 33);
    // A performance upload switches the synth to performance mode first (03), a patch sent alone to patch mode.
    emulator.state().settings.perfMode = false;
    client.resync();
    for (int i = 0; i < 4; ++i)
        client.tick();
    from = emulator.received().size();
    g2::Performance perf;
    for (auto& s : perf.slots)
        s = g2::Patch::makeDefault();
    client.sendPerformance(perf, "Perf", true);
    for (int i = 0; i < 4; ++i)
        client.tick();
    REQUIRE(emulator.received().size() > from + 1);
    CHECK(emulator.received()[from].molecules.at(0) == 0x03);
    CHECK(emulator.received()[from + 1].molecules.at(0) == 0x37);
    CHECK(emulator.state().settings.perfMode);
    from = emulator.received().size();
    client.sendPerformance(perf, "Perf", true); // already in performance mode: no 03
    for (int i = 0; i < 4; ++i)
        client.tick();
    CHECK(emulator.received()[from].molecules.at(0) == 0x37);
}

TEST_CASE("live: the bubbles Clavia's editor sent to a real G2X, byte for byte (live_edits)")
{
    namespace lv = g2::live;
    using lv::Connector;
    using g2::CableColor;
    auto frame = [](u8 session, const std::vector<lv::Op>& ops) {
        return hexOf(framesFor(Bubble::patch(0, session, toMolecules(ops))).front());
    };
    auto vector = [](int step) { return hexOf(g2test::vectorBytes("live_edits", step)); };
    const Connector out1{1, 0, true}, in2b{2, 1, false}, out4{4, 0, true}, in2a{2, 0, false};
    // A new cable: the drag's origin first.
    CHECK(frame(0, {lv::CableConnect{Location::Va, out1, in2b, CableColor::Blue, false}}) == vector(0));
    // With the rate change it causes, in one bubble.
    CHECK(frame(0, {lv::CableConnect{Location::Va, out4, in2a, CableColor::Red, false}, lv::Uprate{Location::Va, 2, true}}) == vector(1));
    // Deleted: the child end (the input) first, its parent (the output) second.
    CHECK(frame(0, {lv::CableDelete{Location::Va, in2a, out4, false}, lv::Uprate{Location::Va, 2, false}}) == vector(2));
    // As part of a dump (the undo of a paste): the bit is clear.
    CHECK(frame(0, {lv::CableDelete{Location::Va, {8, 0, false}, {5, 0, true}, true}, lv::ModuleDelete{Location::Va, 8}}) == vector(3));
    g2::Module osc;
    osc.type = 0x61;
    osc.index = 5;
    osc.col = 1;
    osc.row = 15;
    osc.name = "OscA1";
    CHECK(frame(0, {lv::ModuleNew{Location::Va, osc}}) == vector(4));
    CHECK(frame(1, {lv::CtrlAssign{16, Location::Va, 1, 7}}) == vector(5));
}
