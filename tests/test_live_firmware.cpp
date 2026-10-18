// Live editing against the user's own G2 OS (emu/g2emu): every kind of edit the editor makes goes to the emulated
// G2 as the original editor sends it (g2/live.hpp, g2/proto/live.hpp), and the patch read back from the synth (Get
// Patch: 3C and what follows) is the editor's, after the edit, its undo and its redo. Parameter edits keep the
// sound going. Skipped without the firmware (G2_FIRMWARE, or original/firmware).
#include "g2/edit.hpp"
#include "g2/live.hpp"
#include "g2/module_db.hpp"
#include "g2/proto/client.hpp"
#include "g2/proto/live.hpp"
#include "g2/proto/logging_transport.hpp"
#include "g2/replace.hpp"
#include "g2/uprate.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include "PerfSync.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>

using namespace g2emu;
using g2::Location;
using g2::u8;

namespace {

std::filesystem::path firmwarePath()
{
    if(const char* e = std::getenv("G2_FIRMWARE")) return e;
    return std::filesystem::path(G2_SOURCE_DIR) / "original" / "firmware";
}

struct Events : g2::proto::Client::Listener
{
    int slotReads = 0, errors = 0;
    void slotChanged(int) override { ++slotReads; }
    void error(u8) override { ++errors; }
};

// A booted emulated G2 and our client, synced.
struct Rig
{
    Firmware fw;
    std::unique_ptr<Machine> m;
    std::unique_ptr<g2::proto::LoggingTransport> transport;
    g2::proto::ManualClock clock;
    std::unique_ptr<g2::proto::Client> client;
    Events events;

    bool boot()
    {
        try
        {
            fw = Firmware::load(firmwarePath());
        }
        catch(const std::exception&)
        {
            return false;
        }
        m = std::make_unique<Machine>(fw);
        m->run(Machine::FrameRate * 2);
        // G2_LIVE_LOG=<file>: the USB traffic, for looking at what went out.
        const char* log = std::getenv("G2_LIVE_LOG");
        transport = std::make_unique<g2::proto::LoggingTransport>(std::make_unique<MachineTransport>(*m),
                                                                  log ? log : "/dev/null", g2::proto::LogData::Full);
        client = std::make_unique<g2::proto::Client>(*transport, clock);
        client->setListener(&events);
        m->plugUsb();
        until([&] { return client->synced(); }, 4);
        return client->synced();
    }
    void step(std::vector<float>* audio = nullptr)
    {
        m->run(Machine::FrameRate / 1000, audio);
        clock.set(std::uint64_t(m->seconds() * 1000));
        client->tick();
    }
    std::uint64_t ms() const { return std::uint64_t(m->seconds() * 1000); }
    template<class F> bool until(F done, double seconds)
    {
        const double end = m->seconds() + seconds;
        while(!done() && m->seconds() < end) step();
        return done();
    }
    // Lets the synth answer everything, and the unsolicited traffic that follows.
    void settle(double seconds = 0.1)
    {
        until([&] { return client->idle(); }, 4);
        const double end = m->seconds() + seconds;
        while(m->seconds() < end) step();
        until([&] { return client->idle(); }, 4);
    }
};

const g2::db::ModuleDef& moduleNamed(const char* name)
{
    for(const auto& m : g2::db::modules())
        if(m.kind == g2::db::ModuleKind::Module && std::string(m.shortName) == name) return m;
    throw std::runtime_error(name);
}

u8 paramIndex(const g2::Patch& p, Location loc, u8 module, const char* name)
{
    const auto* def = p.area(loc).find(module)->def();
    for(std::size_t i = 0; i < def->params.size(); ++i)
        if(std::string(def->params[i].name) == name) return u8(i);
    throw std::runtime_error(name);
}

// What a synth holds of a patch, in a canonical text: what the editor and the synth must agree on.
std::string describe(const g2::Patch& p)
{
    std::ostringstream o;
    const auto& h = p.header;
    o << "header voices " << int(h.voiceCount) << " mono " << int(h.monoMode) << " category " << int(h.category)
      << " variation " << int(h.activeVariation) << " cables";
    for(bool v : h.cablesVisible) o << ' ' << v;
    o << '\n';
    for(const auto loc : {Location::Va, Location::Fx})
    {
        const auto& area = p.area(loc);
        std::vector<const g2::Module*> modules;
        for(const auto& m : area.modules) modules.push_back(&m);
        std::sort(modules.begin(), modules.end(), [](auto* a, auto* b) { return a->index < b->index; });
        for(const auto* m : modules)
        {
            o << "module " << int(loc) << ':' << int(m->index) << " type " << int(m->type) << " at " << int(m->col) << ','
              << int(m->row) << " colour " << int(m->color) << " uprate " << m->uprate << " locked " << m->locked
              << " name '" << m->name << "' modes";
            for(auto v : m->modes) o << ' ' << int(v);
            o << " custom";
            if(m->customData)
                for(auto b : *m->customData) o << ' ' << int(b);
            o << '\n';
            for(std::size_t v = 0; v < m->params.size() && v < std::size_t(g2::kFileVariations); ++v)
            {
                o << "  var " << v << ':';
                for(auto x : m->params[v]) o << ' ' << int(x);
                o << '\n';
            }
        }
        std::vector<std::string> cables;
        for(const auto& c : area.cables)
        {
            std::ostringstream s;
            auto a = std::make_tuple(int(c.fromModule), int(c.fromConn), int(c.fromIsOutput));
            auto b = std::make_tuple(int(c.toModule), int(c.toConn), 0);
            if(!c.fromIsOutput && b < a) std::swap(a, b);
            s << "cable " << int(loc) << ' ' << std::get<0>(a) << '.' << std::get<1>(a) << (std::get<2>(a) ? "out" : "in")
              << " - " << std::get<0>(b) << '.' << std::get<1>(b) << "in colour " << int(c.color);
            cables.push_back(s.str());
        }
        std::sort(cables.begin(), cables.end());
        for(const auto& c : cables) o << c << '\n';
    }
    for(const auto& m : p.settings)
        for(std::size_t v = 0; v < m.params.size() && v < std::size_t(g2::kFileVariations); ++v)
        {
            o << "setting " << int(m.index) << " var " << v << ':';
            for(auto x : m.params[v]) o << ' ' << int(x);
            o << '\n';
        }
    for(std::size_t v = 0; v < p.morphs.size() && v < std::size_t(g2::kFileVariations); ++v)
    {
        auto list = p.morphs[v].assigns;
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            return std::tie(a.location, a.module, a.param, a.morph) < std::tie(b.location, b.module, b.param, b.morph);
        });
        for(const auto& a : list)
            o << "morph var " << v << ' ' << int(a.location) << ':' << int(a.module) << '.' << int(a.param) << " group "
              << int(a.morph) << " range " << int(a.range) << '\n';
    }
    for(std::size_t k = 0; k < p.knobs.size(); ++k)
        if(const auto& a = p.knobs[k])
            o << "knob " << k << ' ' << int(a->location) << ':' << int(a->module) << '.' << int(a->param) << '\n';
    auto ctrls = p.controllers;
    std::sort(ctrls.begin(), ctrls.end(), [](const auto& a, const auto& b) { return a.cc < b.cc; });
    for(const auto& c : ctrls) o << "cc " << int(c.cc) << ' ' << int(c.location) << ':' << int(c.module) << '.' << int(c.param) << '\n';
    for(const auto& c : p.settingsCustomData)
    {
        o << "settings custom " << int(c.index) << ':';
        for(auto b : c.bytes) o << ' ' << int(b);
        o << '\n';
    }
    o << "notes '" << p.notes << "'\n";
    return o.str();
}

// The lines of one description that the other lacks, both ways.
std::string difference(const std::string& a, const std::string& b)
{
    auto lines = [](const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        for(std::string l; std::getline(in, l);) out.push_back(l);
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto x = lines(a), y = lines(b);
    std::vector<std::string> onlyA, onlyB;
    std::set_difference(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(onlyA));
    std::set_difference(y.begin(), y.end(), x.begin(), x.end(), std::back_inserter(onlyB));
    std::string out;
    for(const auto& l : onlyA) out += "- " + l + "\n";
    for(const auto& l : onlyB) out += "+ " + l + "\n";
    return out;
}

// The editor's side: a document with undo, sending each edit as SynthSync does.
struct Editor
{
    explicit Editor(Rig& rig) : g2(rig) {}
    Rig& g2;
    int slot = 0;
    g2::Patch doc;
    std::string name = "Live";
    g2::proto::LiveSlot live;
    struct Step
    {
        g2::Patch before, after;
        g2::live::Edit edit;
    };
    std::vector<Step> undos, redos;
    int sent = 0; // bubbles and realtime messages sent

    void upload(const g2::Patch& p)
    {
        g2.client->sendPatch(slot, p, name);
        g2.settle();
        doc = p;
        live = g2::proto::LiveSlot(p, name);
    }
    void send(const std::vector<g2::proto::Outgoing>& out)
    {
        if(live.needsUpload())
        {
            INFO("mirror (-) against the document (+):\n" << difference(describe(live.synth()), describe(doc)));
            REQUIRE_FALSE(live.needsUpload());
        }
        for(const auto& o : out)
        {
            ++sent;
            if(!o.realtime)
            {
                g2.client->edit(slot, o.molecules);
                continue;
            }
            for(const auto& m : o.molecules)
            {
                if(const auto* p = std::get_if<g2::proto::ParamChange>(&m))
                    g2.client->setParam(slot, Location(p->location), p->module, p->param, p->value, p->variation);
                else if(const auto* mc = std::get_if<g2::proto::MorphChange>(&m))
                    g2.client->setMorph(slot, Location(mc->location), mc->module, mc->param, mc->morph, mc->range,
                                        mc->variation, true);
            }
        }
    }
    void edit(const std::function<void(g2::Patch&)>& f)
    {
        g2::Patch next = doc;
        g2::live::Recorder recorder;
        f(next);
        undos.push_back({doc, next, recorder.edit()});
        redos.clear();
        doc = next;
        send(live.update(doc, name, &undos.back().edit, false, g2.ms()));
    }
    void undo()
    {
        auto s = undos.back();
        undos.pop_back();
        doc = s.before;
        send(live.update(doc, name, &s.edit, true, g2.ms()));
        redos.push_back(s);
    }
    void redo()
    {
        auto s = redos.back();
        redos.pop_back();
        doc = s.after;
        send(live.update(doc, name, &s.edit, false, g2.ms()));
        undos.push_back(s);
    }
    void rename(const std::string& n)
    {
        name = n;
        send(live.update(doc, name, nullptr, false, g2.ms()));
    }
    // The synth's patch, read again (Get Patch), against the document.
    void check(const char* what)
    {
        INFO(what);
        g2.settle();
        REQUIRE(g2.client->connected());
        CHECK(g2.events.errors == 0);
        g2.client->resyncSlot(slot);
        g2.settle();
        const auto& s = g2.client->state().slots[std::size_t(slot)];
        INFO("synth (-) against the document (+):\n" << difference(describe(s.patch), describe(doc)));
        CHECK(describe(s.patch) == describe(doc));
        CHECK(s.name == name);
    }
    // The edit, its undo and its redo, each read back.
    void roundTrip(const char* what, const std::function<void(g2::Patch&)>& f)
    {
        INFO("edit: " << what);
        edit(f);
        check(what);
        INFO("undo");
        undo();
        check((std::string(what) + ", undone").c_str());
        INFO("redo");
        redo();
        check((std::string(what) + ", redone").c_str());
    }
};

// Keyboard -> OscA -> EnvADSR -> 2-Out, an LfoA, a Mix4-1B and a FltClassic (VA), and a 2-In -> Reverb -> 2-Out (FX).
g2::Patch testPatch()
{
    using namespace g2;
    Patch p = Patch::makeDefault();
    auto add = [&](Location loc, const char* name, u8 col, u8 row) { return edit::addModule(p, loc, moduleNamed(name).typeId, col, row); };
    const auto kbd = add(Location::Va, "Keyboard", 0, 0), osc = add(Location::Va, "OscA", 1, 0),
               env = add(Location::Va, "EnvADSR", 2, 0), out = add(Location::Va, "2-Out", 3, 0);
    add(Location::Va, "LfoA", 0, 6);
    add(Location::Va, "Mix4-1B", 1, 8);
    add(Location::Va, "FltClassic", 2, 8);
    edit::connect(p, Location::Va, {osc, 0, true}, {env, 0, false});
    edit::connect(p, Location::Va, {kbd, 1, true}, {env, 1, false});
    edit::connect(p, Location::Va, {env, 1, true}, {out, 0, false});
    edit::connect(p, Location::Va, {out, 0, false}, {out, 1, false});
    const auto in = add(Location::Fx, "2-In", 0, 0), rev = add(Location::Fx, "Reverb", 1, 0), fxOut = add(Location::Fx, "2-Out", 2, 0);
    edit::connect(p, Location::Fx, {in, 0, true}, {rev, 0, false});
    edit::connect(p, Location::Fx, {rev, 0, true}, {fxOut, 0, false});
    for(u8 v = 0; v < kFileVariations; ++v) edit::setSetting(p, edit::Setting::Gain, 0, v, 100);
    return p;
}

u8 find(const g2::Patch& p, Location loc, const char* name)
{
    for(const auto& m : p.area(loc).modules)
        if(std::string(m.def()->shortName) == name) return m.index;
    throw std::runtime_error(name);
}

#define BOOT_OR_SKIP(rig) \
    Rig rig; \
    if(!rig.boot()) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .dmg, or unpack it into original/firmware)")

} // namespace

TEST_CASE("Live editing on the user's G2 OS: cables, as the original sends them", "[g2emu][firmware][live]")
{
    BOOT_OR_SKIP(g2);
    Editor e{g2};
    e.upload(testPatch());
    e.check("uploaded");
    const auto& p = e.doc;
    const u8 lfo = find(p, Location::Va, "LfoA"), mix = find(p, Location::Va, "Mix4-1B"),
             flt = find(p, Location::Va, "FltClassic"), osc = find(p, Location::Va, "OscA"),
             env = find(p, Location::Va, "EnvADSR"), out = find(p, Location::Va, "2-Out");
    using g2::edit::Endpoint;
    e.roundTrip("connect an output", [&](g2::Patch& x) { g2::edit::connect(x, Location::Va, {lfo, 0, true}, {mix, 0, false}); });
    e.roundTrip("connect input to input", [&](g2::Patch& x) { g2::edit::connect(x, Location::Va, {mix, 1, false}, {mix, 0, false}); });
    e.roundTrip("connect to a net from its input end", [&](g2::Patch& x) { g2::edit::connect(x, Location::Va, {flt, 0, false}, {mix, 0, true}); });
    e.roundTrip("recolour a chain", [&](g2::Patch& x) { g2::edit::recolorChain(x, Location::Va, {mix, 0, false}, g2::CableColor::Green); });
    e.roundTrip("move cables", [&](g2::Patch& x) { g2::edit::moveCables(x, Location::Va, {mix, 0, false}, {mix, 2, false}); });
    e.roundTrip("break", [&](g2::Patch& x) { g2::edit::breakJack(x, Location::Va, {mix, 2, false}); });
    e.roundTrip("disconnect", [&](g2::Patch& x) { g2::edit::disconnectJack(x, Location::Va, {out, 0, false}); });
    e.roundTrip("delete a chain", [&](g2::Patch& x) { g2::edit::deleteChain(x, Location::Va, {osc, 0, true}); });
    e.roundTrip("delete unused cables", [&](g2::Patch& x) { g2::edit::deleteUnusedCables(x); });
    e.roundTrip("an uprate: an audio output into the LFO's rate input", [&](g2::Patch& x) {
        g2::edit::connect(x, Location::Va, {osc, 0, true}, {lfo, 0, false});
    });
    (void)env;
}

TEST_CASE("Live editing on the user's G2 OS: modules, as the original sends them", "[g2emu][firmware][live]")
{
    BOOT_OR_SKIP(g2);
    Editor e{g2};
    e.upload(testPatch());
    const auto& p = e.doc;
    const u8 lfo = find(p, Location::Va, "LfoA"), mix = find(p, Location::Va, "Mix4-1B"), osc = find(p, Location::Va, "OscA"),
             env = find(p, Location::Va, "EnvADSR"), flt = find(p, Location::Va, "FltClassic");
    e.roundTrip("add a module (module bar)", [&](g2::Patch& x) {
        const u8 added = g2::edit::addModule(x, Location::Va, moduleNamed("OscB").typeId, 1, 0);
        g2::edit::resolveOverlaps(x, Location::Va, added); // the make-room moves after it
    });
    e.roundTrip("drop a module (make-room moves first)", [&](g2::Patch& x) {
        g2::edit::dropModule(x, Location::Va, moduleNamed("LfoB").typeId, 0, 1);
    });
    e.roundTrip("add a module with labels (custom data)", [&](g2::Patch& x) {
        g2::edit::addModule(x, Location::Fx, moduleNamed("SwOnOffM").typeId, 4, 0);
    });
    e.roundTrip("move modules", [&](g2::Patch& x) { g2::edit::moveModules(x, Location::Va, {lfo, mix}, 3, 1); });
    e.roundTrip("rename", [&](g2::Patch& x) { g2::edit::renameModule(x, Location::Va, osc, "Lead"); });
    e.roundTrip("recolour", [&](g2::Patch& x) {
        g2::edit::setModuleColor(x, Location::Va, osc, 5);
        g2::edit::setModuleColor(x, Location::Va, env, 5);
    });
    e.roundTrip("lock (Mutator)", [&](g2::Patch& x) { g2::edit::setLocked(x, Location::Va, osc, true); });
    // Knobs, controllers and morphs on modules about to be deleted, replaced or pasted.
    e.edit([&](g2::Patch& x) {
        g2::edit::connect(x, Location::Va, {lfo, 0, true}, {osc, 1, false});
        g2::edit::assignKnob(x, 3, Location::Va, osc, 0);
        g2::edit::assignMidiCc(x, 21, Location::Va, osc, 1);
        g2::edit::setMorph(x, 2, Location::Va, osc, 0, 1, 30);
        g2::edit::setMorph(x, 2, Location::Va, osc, 0, 4, -20);
    });
    e.check("assignments");
    const auto clip = g2::edit::copyModules(e.doc, Location::Va, {lfo, osc, env});
    e.roundTrip("paste with cables and morphs", [&](g2::Patch& x) { g2::edit::pasteModules(x, Location::Va, clip, 5, 0); });
    e.roundTrip("paste into the other area", [&](g2::Patch& x) { g2::edit::pasteModules(x, Location::Fx, clip, 0, 6); });
    // A copy without OscA, through which the LFO's chain ran (spliced), and without the envelope, which fed 2-Out
    // (left white).
    e.edit([&](g2::Patch& x) { g2::edit::connect(x, Location::Va, {osc, 1, false}, {mix, 0, false}); });
    const auto spliced = g2::edit::copyModules(e.doc, Location::Va, {lfo, mix, find(p, Location::Va, "2-Out")});
    CHECK(spliced.cables.size() == 2);
    e.roundTrip("paste with a chain spliced and a chain left white", [&](g2::Patch& x) {
        g2::edit::pasteModules(x, Location::Va, spliced, 6, 0);
    });
    // Paste Params: OscA's values (variation 1) into a new OscA, sent as the area's parameter and morph dumps.
    auto params = g2::edit::copyModules(e.doc, Location::Va, {osc});
    params.variation = 0;
    e.roundTrip("paste params into a module of the same type", [&](g2::Patch& x) {
        const g2::u8 copy = g2::edit::addModule(x, Location::Va, moduleNamed("OscA").typeId, 7, 0);
        REQUIRE(g2::edit::pasteParams(x, Location::Va, {copy}, params, 0).empty());
    });
    e.roundTrip("replace (OscA -> OscB, cables, knob, controller and morphs follow)", [&](g2::Patch& x) {
        g2::replace::replaceModule(x, Location::Va, osc, moduleNamed("OscB").typeId);
    });
    e.roundTrip("delete a module in a chain, with its knob, controller and morphs", [&](g2::Patch& x) {
        g2::edit::removeModules(x, Location::Va, {env});
    });
    e.roundTrip("delete a selection", [&](g2::Patch& x) { g2::edit::removeModules(x, Location::Va, {flt, mix, lfo}); });
}

TEST_CASE("Live editing on the user's G2 OS: values, assignments, variations, header, notes, name", "[g2emu][firmware][live]")
{
    BOOT_OR_SKIP(g2);
    Editor e{g2};
    e.upload(testPatch());
    const auto& p = e.doc;
    const u8 osc = find(p, Location::Va, "OscA"), flt = find(p, Location::Va, "FltClassic"),
             rev = find(p, Location::Fx, "Reverb");
    const u8 coarse = paramIndex(p, Location::Va, osc, "Coarse"), freq = paramIndex(p, Location::Va, flt, "Freq");
    e.roundTrip("a parameter (realtime)", [&](g2::Patch& x) { g2::edit::setParam(x, Location::Va, osc, coarse, 0, 70); });
    e.roundTrip("a parameter in another variation", [&](g2::Patch& x) { g2::edit::setParam(x, Location::Fx, rev, 0, 3, 20); });
    e.roundTrip("a patch setting", [&](g2::Patch& x) { g2::edit::setSetting(x, g2::edit::Setting::Glide, 1, 0, 40); });
    e.roundTrip("a mode", [&](g2::Patch& x) { g2::edit::setMode(x, Location::Fx, rev, 0, 2); });
    e.roundTrip("a morph range (realtime drag)", [&](g2::Patch& x) { g2::edit::setMorph(x, 0, Location::Va, flt, freq, 2, 40); });
    e.roundTrip("morph assign (menu)", [&](g2::Patch& x) { g2::edit::toggleMorphAssign(x, 0, Location::Va, osc, coarse, 3); });
    e.roundTrip("a knob", [&](g2::Patch& x) { g2::edit::assignKnob(x, 9, Location::Va, osc, coarse); });
    e.roundTrip("the knob to another parameter", [&](g2::Patch& x) { g2::edit::assignKnob(x, 9, Location::Va, flt, freq); });
    e.roundTrip("the parameter to another knob", [&](g2::Patch& x) { g2::edit::assignKnob(x, 17, Location::Va, flt, freq); });
    e.roundTrip("a knob off", [&](g2::Patch& x) { g2::edit::clearKnob(x, 17); });
    e.roundTrip("a MIDI controller", [&](g2::Patch& x) { g2::edit::assignMidiCc(x, 22, Location::Va, osc, coarse); });
    e.roundTrip("the controller elsewhere", [&](g2::Patch& x) { g2::edit::assignMidiCc(x, 22, Location::Va, flt, freq); });
    e.roundTrip("a controller off", [&](g2::Patch& x) { g2::edit::clearMidiCc(x, 22); });
    e.roundTrip("a variation", [&](g2::Patch& x) { x.header.activeVariation = 4; });
    e.roundTrip("copy a variation", [&](g2::Patch& x) { g2::edit::copyVariation(x, 3, 6); });
    e.roundTrip("init a variation", [&](g2::Patch& x) { g2::edit::copyVariation(x, 8, 3); });
    e.roundTrip("a label", [&](g2::Patch& x) {
        const u8 sw = g2::edit::addModule(x, Location::Fx, moduleNamed("SwOnOffM").typeId, 4, 0);
        g2::edit::setParamLabel(x, Location::Fx, sw, 0, "Bypass");
    });
    e.roundTrip("a morph group's name", [&](g2::Patch& x) { g2::edit::setMorphLabel(x, 2, "Brite"); });
    e.roundTrip("category and cable visibility (21 at once)", [&](g2::Patch& x) {
        g2::edit::setCategory(x, 5);
        g2::edit::setCablesVisible(x, g2::CableColor::Blue, false);
    });
    // (The G2 OS leaves its notes as they are on an empty 6F: an edit from text to text.)
    e.edit([&](g2::Patch& x) { x.notes = "First notes"; });
    e.check("patch notes (6F)");
    e.roundTrip("patch notes changed (6F)", [&](g2::Patch& x) { x.notes = "Live notes\nsecond line"; });
    e.rename("Renamed Live");
    e.check("patch name (27)");

    // Voices: 21 goes 600 ms after the last change.
    e.edit([&](g2::Patch& x) { g2::edit::setVoices(x, g2::edit::VoiceMode::Poly, 4); });
    const auto at = g2.ms();
    g2.settle(0.3);
    CHECK(e.live.headerPending());
    while(e.live.headerPending() && g2.ms() < at + 2000)
    {
        g2.step();
        e.send(e.live.tick(e.doc, g2.ms()));
    }
    CHECK(g2.ms() >= at + g2::proto::LiveSlot::kVoicesDelayMs);
    e.check("voices");
}

TEST_CASE("Live editing on the user's G2 OS: the Patch Mutator's variation 9", "[g2emu][firmware][live]")
{
    BOOT_OR_SKIP(g2);
    Editor e{g2};
    e.upload(testPatch());
    const u8 osc = find(e.doc, Location::Va, "OscA");
    e.edit([&](g2::Patch& x) { g2::edit::setMorph(x, 1, Location::Va, osc, 0, 2, 25); });
    // An individual in variation 9 (4D x3, 65, 6A 9): the user's variations keep their values and morphs.
    e.roundTrip("audition", [&](g2::Patch& x) {
        const g2::Patch before = x;
        g2::edit::addAuditionVariation(x);
        for(auto& m : x.va.modules)
            if(m.params.size() > 9)
                for(auto& v : m.params[9]) v = static_cast<u8>(v / 2);
        g2::edit::storeMorph(x, 9, Location::Va, osc, 0, 5, 33);
        x.header.activeVariation = 9;
        g2::live::recordVariationDump(before, x, 9);
        g2::live::record(g2::live::VariationSelect{9}, g2::live::VariationSelect{before.header.activeVariation});
    });
    // Accepted into variation 2 (4D x3, 65 for it, then 6A).
    e.roundTrip("copy to variation 2", [&](g2::Patch& x) {
        const g2::Patch before = x;
        for(auto* area : {&x.va, &x.fx})
            for(auto& m : area->modules)
                if(m.params.size() > 9) m.params[1] = m.params[9];
        x.header.activeVariation = 1;
        g2::live::recordVariationDump(before, x, 1);
        g2::live::record(g2::live::VariationSelect{1}, g2::live::VariationSelect{before.header.activeVariation});
    });
    // Variation 9 itself, which a file (and so the read-back patch) leaves out: the synth's VA values (4C 01).
    std::vector<g2::proto::Molecule> reply;
    g2.client->request(g2::proto::Bubble::patch(0, g2.client->state().slots[0].session,
                                                {g2::proto::LocationRequest{g2::proto::id::ParamListRequest, 1}}),
                       [&](const std::vector<g2::proto::Molecule>& r) { reply = r; });
    g2.settle();
    bool seen = false;
    for(const auto& m : reply)
        if(const auto* d = std::get_if<g2::proto::SectionDump>(&m))
            if(const auto* list = std::get_if<g2::file::ParamList>(&d->section.payload))
                for(const auto& pm : list->modules)
                    if(pm.index == osc)
                        for(const auto& v : pm.variations)
                            if(v.variation == 9)
                            {
                                seen = true;
                                CHECK(v.values == e.doc.va.find(osc)->params[9]);
                            }
    CHECK(seen);
}

TEST_CASE("Live editing on the user's G2 OS: parameter edits keep the sound going", "[g2emu][firmware][live]")
{
    BOOT_OR_SKIP(g2);
    Editor e{g2};
    auto patch = testPatch();
    const u8 osc = find(patch, Location::Va, "OscA"), env = find(patch, Location::Va, "EnvADSR");
    for(u8 v = 0; v < g2::kFileVariations; ++v)
    {
        g2::edit::setParam(patch, Location::Va, osc, paramIndex(patch, Location::Va, osc, "Wave"), v, 0);
        g2::edit::setParam(patch, Location::Va, env, paramIndex(patch, Location::Va, env, "Attack"), v, 0);
        g2::edit::setParam(patch, Location::Va, env, paramIndex(patch, Location::Va, env, "Sustain"), v, 127);
    }
    g2::Performance perf = g2::Performance::playing(patch, "Live");
    g2.client->sendPerformance(perf, "Live", false);
    g2.settle(0.5);
    e.doc = patch;
    e.live = g2::proto::LiveSlot(patch, "Live");

    const std::uint8_t on[3] = {0x90, 69, 100};
    g2.m->midiInAt(on, g2.m->frame() + 10);
    std::vector<float> audio;
    for(int i = 0; i < 200; ++i) g2.step(&audio); // the note sounds
    const std::size_t start = audio.size() / 4;
    // A knob turned for a second: a realtime 40 every 10 ms, in the variation playing.
    const u8 coarse = paramIndex(patch, Location::Va, osc, "Coarse");
    const u8 level = paramIndex(patch, Location::Va, env, "Release");
    const auto session = g2.client->state().slots[0].session;
    const int reads = g2.events.slotReads;
    for(int i = 0; i < 100; ++i)
    {
        e.edit([&](g2::Patch& x) { g2::edit::setParam(x, Location::Va, osc, coarse, 0, static_cast<u8>(60 + i % 8)); });
        if(i % 10 == 0) e.edit([&](g2::Patch& x) { g2::edit::setParam(x, Location::Va, env, level, 0, static_cast<u8>(100 + i / 10)); });
        for(int k = 0; k < 10; ++k) g2.step(&audio);
    }
    REQUIRE(audio.size() / 4 > start + Machine::FrameRate / 2);
    // No gap: in every 2 ms window of Out 1, the signal is there.
    const std::size_t window = Machine::FrameRate / 500;
    std::size_t silent = 0;
    for(std::size_t f = start; f + window <= audio.size() / 4; f += window)
    {
        float peak = 0;
        for(std::size_t k = f; k < f + window; ++k) peak = std::max(peak, std::fabs(audio[k * 4]));
        if(peak < 1e-3f) ++silent;
    }
    CHECK(silent == 0);
    CHECK(g2.events.errors == 0);
    // Realtime messages only: the slot was never read again (no release, no new session).
    CHECK(g2.client->state().slots[0].session == session);
    CHECK(g2.events.slotReads == reads);
    e.check("after the knob");
}

TEST_CASE("Live editing on the user's G2 OS: the performance's settings, name and slot focus", "[g2emu][firmware][live]")
{
    BOOT_OR_SKIP(g2);
    g2::Performance perf;
    for(auto& s : perf.slots) s = g2::Patch::makeDefault();
    perf.slots[1] = testPatch();
    for(int i = 0; i < 4; ++i)
    {
        perf.header.slots[std::size_t(i)].enabled = 1;
        perf.header.slots[std::size_t(i)].patchName = "Slot " + std::string(1, char('A' + i));
    }
    // The synth is switched to performance mode first (03) when it is not in it.
    g2.client->sendPerformance(perf, "Perf One", true);
    g2.settle(0.3);
    CHECK(g2.client->state().settings.perfMode);
    // 29 alone, then 11 (the whole header), as P bubbles.
    g2.client->edit(g2::proto::kSlotSynth, {g2::proto::NameDump{g2::proto::id::PerfName, "Perf Two"}});
    auto header = perf.header;
    header.masterClockBpm = 133;
    header.kbdRangeEnabled = 1;
    header.slots[2].enabled = 0;
    header.slots[1].kbdRangeLower = 40;
    g2.client->edit(g2::proto::kSlotSynth, {g2::proto::SectionDump{g2::file::Section{g2::file::kPerfHeader, header, 0, {}}}});
    // 09: the focused slot.
    g2.client->selectSlot(1);
    g2.settle(0.3);
    REQUIRE(g2.client->connected());
    CHECK(g2.events.errors == 0);
    // Read back: the performance header and name (10), the slot focus (08).
    g2.client->resync();
    g2.settle(0.5);
    const auto& s = g2.client->state();
    CHECK(s.perfName == "Perf Two");
    CHECK(int(s.perfHeader.masterClockBpm) == 133);
    CHECK(int(s.perfHeader.kbdRangeEnabled) == 1);
    CHECK(int(s.perfHeader.slots[2].enabled) == 0);
    CHECK(int(s.perfHeader.slots[1].kbdRangeLower) == 40);
    CHECK(int(s.slotFocus) == 1);
}

TEST_CASE("Live on the user's G2 OS: a new performance comes back from the synth as it was sent", "[g2emu][firmware][live]")
{
    // Round-6 report: after File > New Performance, untouched, opening a file asked about unsaved changes. The synth's
    // answer to the new performance must hold the settings and name sent, or SynthSync takes it for a change made on the
    // synth (PerfSync.h) and the document becomes edited.
    Rig rig;
    if(!rig.boot()) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .dmg, or unpack it into original/firmware)");
    const auto perf = g2::Performance::makeNew();
    rig.client->sendPerformance(perf, "Empty perf", true);
    rig.settle(0.5);
    const auto& state = rig.client->state();
    CHECK(state.perfName == "Empty perf");
    CHECK(g2ui::sameSettings(state.perfHeader, perf.header));
    if(!g2ui::sameSettings(state.perfHeader, perf.header))
    {
        const auto a = g2ui::settingsOf(state.perfHeader), b = g2ui::settingsOf(perf.header);
        INFO("synth: clock " << int(a.masterClockBpm) << " run " << int(a.masterClockRun) << " / sent: clock " << int(b.masterClockBpm)
                             << " run " << int(b.masterClockRun));
        for(std::size_t s = 0; s < 4; ++s)
            INFO("slot " << s << " synth enabled " << int(a.slots[s].enabled) << " kb " << int(a.slots[s].keyboard)
                         << " ch " << int(a.slots[s].midiChannel) << " / sent " << int(b.slots[s].enabled) << " "
                         << int(b.slots[s].keyboard) << " " << int(b.slots[s].midiChannel));
        CHECK(false);
    }
}

TEST_CASE("Live on the user's G2 OS: the display shows the name of the patch sent after another", "[g2emu][firmware][live]")
{
    // Round-5 report: the Live view's display said "No name" while a named patch played. At launch the emulated G2
    // gets the session's patch (a new one is "No name", SynthSync::sendPatchAlone); a file opened then goes to the
    // same slot as a whole (SynthSync::push -> sendPatch), with its name.
    Rig rig;
    if(!rig.boot()) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .dmg, or unpack it into original/firmware)");
    rig.client->sendPerformance(g2::Performance::playing(testPatch(), "No name"), "No name", false);
    rig.settle(0.3);
    INFO(rig.m->panel().displays[0].text(0) << " / " << rig.m->panel().displays[0].text(1));
    REQUIRE(rig.m->panel().displays[0].text(1).find("No name") != std::string::npos);

    auto other = testPatch();
    g2::edit::addModule(other, Location::Va, moduleNamed("OscB").typeId, 4, 0);
    rig.client->sendPatch(0, other, "PianoStringz");
    rig.settle(0.3);
    INFO(rig.m->panel().displays[0].text(0) << " / " << rig.m->panel().displays[0].text(1));
    CHECK(rig.m->panel().displays[0].text(1).find("PianoStringz") != std::string::npos);
}

TEST_CASE("Live on the user's G2 OS: the Perf button puts the synth into performance mode and back", "[g2emu][firmware][live]")
{
    // The toolbar's Perf (manual p58; CTBWindow::SetPerformanceMode @0012a306 -> CSynth::SendPerformanceModeChange
    // @0011dfae: 3E, sent only when the mode changes, Shift as its second byte). The OS answers with a new performance
    // (1F) and no synth settings, so the client keeps the mode it asked for, as the original's synth map; the slots
    // then hold the recalled performance, and the patch-mode slots come back on the way out (manual p46).
    Rig rig;
    if(!rig.boot()) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .dmg, or unpack it into original/firmware)");
    const auto patch = testPatch();
    rig.client->sendPerformance(g2::Performance::playing(patch, "Probe"), "Probe", false);
    rig.settle(0.5);
    REQUIRE_FALSE(rig.client->state().settings.perfMode);
    rig.client->setPerformanceMode(true, false);
    rig.settle(1.5);
    CHECK(rig.client->state().settings.perfMode);
    CHECK(rig.client->state().slots[0].name != "Probe"); // the performance the synth recalled
    rig.client->setPerformanceMode(false, false);
    rig.settle(1.5);
    CHECK_FALSE(rig.client->state().settings.perfMode);
    CHECK(rig.client->state().slots[0].name == "Probe");
    CHECK(rig.client->state().slots[0].patch.va.modules.size() == patch.va.modules.size());
}
