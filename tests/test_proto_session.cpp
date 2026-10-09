#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/proto/client.hpp"
#include "g2/proto/emulator.hpp"
#include "g2/proto/sections.hpp"
#include "proto_vectors.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <utility>

using namespace g2::proto;
using g2::Location;
using g2test::hexOf;

namespace {

struct Recorder : Client::Listener {
    std::vector<Status> statuses;
    int syncs = 0;
    std::vector<int> slotChanges;
    std::vector<std::pair<int, ParamChange>> params;
    std::vector<std::pair<int, u8>> variations;
    std::vector<int> leds;
    std::vector<u8> errors;
    std::vector<MidiLearn> learned;
    int performanceChanges = 0;

    void statusChanged(Status s) override { statuses.push_back(s); }
    void synced() override { ++syncs; }
    void slotChanged(int slot) override { slotChanges.push_back(slot); }
    void performanceChanged() override { ++performanceChanges; }
    void paramChanged(int slot, const ParamChange& p) override { params.emplace_back(slot, p); }
    void variationChanged(int slot, u8 v) override { variations.emplace_back(slot, v); }
    void ledsChanged(int slot) override { leds.push_back(slot); }
    void midiLearned(const MidiLearn& m) override { learned.push_back(m); }
    void error(u8 code) override { errors.push_back(code); }
};

struct Rig {
    explicit Rig(Client::Options options = {}) : client(link, clock, options) { client.setListener(&events); }

    void plugIn()
    {
        link.plugIn();
        run();
    }
    // The emulator answers within the same poll, so a few ticks settle everything.
    void run(int ticks = 3)
    {
        for (int i = 0; i < ticks; ++i)
            client.tick();
    }
    // The frames the client sent, rebuilt from what the emulator received.
    std::vector<std::string> sentFrames() const
    {
        std::vector<std::string> out;
        for (const auto& f : emulator.received())
            out.push_back(hexOf(f.versionRequest ? versionRequest() : hostFrame(f.hdr, f.session, f.molecules)));
        return out;
    }

    Emulator emulator;
    EmulatorTransport link{emulator};
    ManualClock clock;
    Client client;
    Recorder events;
};

g2::Patch corpusPatch(const char* name)
{
    return g2::loadPatch(g2test::readBytes(std::string(G2_CORPUS_DIR "/pch2csd/") + name));
}

// What a synth holds of a patch: its sections (a file's text header is not sent).
std::vector<u8> saved(const g2::Patch& p)
{
    std::vector<u8> out;
    for (const auto& s : p.toSections()) {
        const auto bytes = g2::file::encodeSection(s);
        out.push_back(s.id);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

} // namespace

TEST_CASE("session: handshake and the full sync in Clavia's order")
{
    Rig rig;
    rig.emulator.state().slots[0].session = 0x11; // as in the golden vectors
    rig.plugIn();
    CHECK(rig.client.status() == Status::Connected);
    CHECK(rig.client.statusLine() == "G2 V1.50");
    CHECK(rig.client.synced());
    CHECK(rig.events.syncs == 1);
    CHECK(rig.client.idle());
    CHECK_FALSE(rig.emulator.editorSyncActive()); // 7D 00 was the last word
    REQUIRE(rig.events.statuses.size() >= 2);
    CHECK(rig.events.statuses[0] == Status::Looking);
    CHECK(rig.events.statuses[1] == Status::Connected);

    // The first frames are the vectors' (version, 7D 01, ..., slot A's requests).
    const auto sent = rig.sentFrames();
    const std::vector<int> steps{0, 3, 5, 7, 10, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24};
    REQUIRE(sent.size() > steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        INFO("frame " << i);
        CHECK(sent[i] == hexOf(g2test::vectorBytes("init_sequence", steps[i])));
    }
    // ... and it ends with 04, 3B, 5E, the name lists, 7D 00.
    auto at = [&](int step) { return std::find(sent.begin(), sent.end(), hexOf(g2test::vectorBytes("init_sequence", step))); };
    CHECK(at(26) < at(27));
    CHECK(at(27) < at(28));
    CHECK(at(28) < at(29));
    CHECK(sent.back() == hexOf(g2test::vectorBytes("init_sequence", 30)));

    // The mirror holds what the synth has.
    const auto& state = rig.client.state();
    CHECK(state.settings.name == "G2 Emulator");
    CHECK(state.perfName == "Emulator");
    CHECK(state.voices == rig.emulator.state().voices);
    CHECK(state.perfSession == rig.emulator.state().perfSession);
    for (int s = 0; s < kSlots; ++s) {
        INFO("slot " << s);
        const auto& mine = state.slots[static_cast<std::size_t>(s)];
        const auto& synth = rig.emulator.state().slots[static_cast<std::size_t>(s)];
        CHECK(mine.session == synth.session);
        CHECK(mine.name == synth.name);
        CHECK(saved(mine.patch) == saved(synth.patch));
        CHECK(mine.load[0].has_value());
        CHECK(mine.load[1].has_value());
    }
    CHECK(rig.events.slotChanges.size() == 4);
}

TEST_CASE("session: upload a patch to slot A and read it back byte for byte")
{
    Rig rig;
    rig.plugIn();
    const auto patch = corpusPatch("test_3osc.pch2");
    const u8 before = rig.emulator.state().slots[0].session;
    rig.client.sendPatch(0, patch, "Three Oscs");
    rig.run();

    // The synth has it, under a new session; the client re-read the slot after the release.
    const auto& synth = rig.emulator.state().slots[0];
    CHECK(synth.name == "Three Oscs");
    CHECK(saved(synth.patch) == saved(patch));
    CHECK(synth.session != before);
    CHECK(rig.client.state().slots[0].session == synth.session);
    CHECK(saved(rig.client.state().slots[0].patch) == saved(patch));
    CHECK(rig.client.state().slots[0].name == "Three Oscs");

    // The download's sections are the upload's, byte for byte.
    std::vector<u8> uploaded;
    for (const auto& f : rig.emulator.received()) {
        const auto m = decode(f.molecules);
        if (!m.empty() && std::holds_alternative<DumpDestination>(m.front()))
            uploaded = encode(std::span<const Molecule>(m).subspan(1));
    }
    REQUIRE_FALSE(uploaded.empty());
    std::vector<u8> downloaded;
    rig.client.request(Bubble::patch(0, rig.client.state().slots[0].session, {Request{id::CompletePatchRequest}}),
                       [&](const std::vector<Molecule>& reply) {
                           for (const auto& m : reply)
                               if (std::holds_alternative<SectionDump>(m))
                                   encode(m, downloaded);
                       });
    rig.run();
    CHECK(hexOf(downloaded) == hexOf(uploaded));
}

TEST_CASE("session: every corpus patch survives the round trip through the synth")
{
    Rig rig;
    rig.plugIn();
    int count = 0;
    for (const auto& path : g2test::corpusFiles()) {
        g2::Patch patch;
        try {
            patch = g2::loadPatch(g2test::readBytes(path));
        } catch (const std::exception&) {
            continue;
        }
        INFO(path.string());
        const int slot = count++ % kSlots;
        rig.client.sendPatch(slot, patch, "Corpus");
        rig.run();
        REQUIRE(rig.client.connected());
        CHECK(saved(rig.emulator.state().slots[static_cast<std::size_t>(slot)].patch) == saved(patch));
        CHECK(saved(rig.client.state().slots[static_cast<std::size_t>(slot)].patch) == saved(patch));
    }
    CHECK(count > 5);
}

TEST_CASE("session: parameter changes both ways")
{
    Rig rig;
    rig.emulator.state().slots[0].patch = corpusPatch("test_3osc.pch2");
    rig.plugIn();
    const auto& va = rig.client.state().slots[0].patch.va;
    REQUIRE_FALSE(va.modules.empty());
    const auto& osc = *std::find_if(va.modules.begin(), va.modules.end(), [](const g2::Module& m) { return !m.params.empty(); });
    const u8 module = osc.index;

    // Editor -> synth: realtime, never answered.
    const auto framesBefore = rig.emulator.received().size();
    rig.client.setParam(0, Location::Va, module, 0, 99, 2);
    rig.run();
    REQUIRE(rig.emulator.received().size() == framesBefore + 1);
    CHECK(rig.emulator.received().back().realtime());
    CHECK_FALSE(rig.client.waiting());
    CHECK(rig.emulator.state().slots[0].patch.va.find(module)->params[2][0] == 99);
    CHECK(rig.client.state().slots[0].patch.va.find(module)->params[2][0] == 99);

    // Synth -> editor: a knob turned on the panel, in the active variation.
    rig.emulator.state().slots[0].patch.header.activeVariation = 1;
    rig.emulator.turnKnob(0, 1, module, 0, 17);
    rig.run();
    REQUIRE(rig.events.params.size() == 1);
    CHECK(rig.events.params[0].first == 0);
    CHECK(rig.events.params[0].second.value == 17);
    CHECK(rig.events.params[0].second.variation == 1);
    CHECK(rig.client.state().slots[0].patch.va.find(module)->params[1][0] == 17);
}

TEST_CASE("session: variations, morphs, knobs, controllers and module edits stay in step")
{
    Rig rig;
    rig.emulator.state().slots[1].patch = corpusPatch("test_3osc.pch2");
    rig.plugIn();
    const auto& patch = rig.client.state().slots[1].patch;
    const auto& m = *std::find_if(patch.va.modules.begin(), patch.va.modules.end(),
                                  [](const g2::Module& x) { return !x.params.empty(); });
    const u8 module = m.index;

    rig.client.selectVariation(1, 3);
    rig.client.copyVariation(1, 3, 5);
    rig.client.setMorph(1, Location::Va, module, 0, 2, 40, 3, true);  // realtime while dragging
    rig.client.setMorph(1, Location::Va, module, 0, 2, -25, 3, false); // acked on release
    rig.client.assignKnob(1, 7, Location::Va, module, 0);
    rig.client.assignMidiCc(1, 21, Location::Va, module, 0);
    rig.client.edit(1, {ModuleMove{1, module, 2, 9}, ModuleRename{1, module, "Renamed"}, ModuleRecolor{1, module, 4}});
    rig.run();

    const auto& synth = rig.emulator.state().slots[1].patch;
    CHECK(synth.header.activeVariation == 3);
    CHECK(synth.knobs[7].has_value());
    CHECK(synth.va.find(module)->name == "Renamed");
    CHECK(synth.va.find(module)->col == 2);
    const auto morph = g2::edit::morphOf(synth, 3, Location::Va, module, 0);
    REQUIRE(morph);
    CHECK(morph->range == -25);
    CHECK(g2::edit::midiCcOf(synth, Location::Va, module, 0) == u8{21});
    // The mirror matches, section for section.
    const auto mine = rig.client.state().slots[1].patch.toSections();
    const auto theirs = synth.toSections();
    for (std::size_t i = 0; i < mine.size(); ++i) {
        INFO("section " << i << " id 0x" << std::hex << int(mine[i].id));
        CHECK(hexOf(g2::file::encodeSection(mine[i])) == hexOf(g2::file::encodeSection(theirs[i])));
    }

    rig.client.deassignKnob(1, 7);
    rig.client.deassignMidiCc(1, 21);
    rig.run();
    CHECK_FALSE(rig.emulator.state().slots[1].patch.knobs[7].has_value());
    CHECK_FALSE(g2::edit::midiCcOf(rig.emulator.state().slots[1].patch, Location::Va, module, 0));

    // From the panel: a variation button.
    rig.emulator.selectVariation(1, 6);
    rig.run();
    REQUIRE(rig.events.variations.size() == 1);
    CHECK(rig.events.variations[0] == std::make_pair(1, u8{6}));
    CHECK(rig.client.state().slots[1].patch.header.activeVariation == 6);
}

TEST_CASE("session: an edit with a stale session number is discarded")
{
    Rig rig;
    rig.plugIn();
    const u8 session = rig.client.state().slots[0].session;
    rig.client.request(Bubble::patch(0, static_cast<u8>((session + 9) & kSessionMask), {VariationSelect{4}}));
    rig.run();
    CHECK(rig.client.connected());
    CHECK(rig.emulator.state().slots[0].patch.header.activeVariation == 0);
}

TEST_CASE("session: patches and performances changed on the synth are read again")
{
    Rig rig;
    rig.plugIn();
    rig.events.slotChanges.clear();
    rig.emulator.loadPatch(2, corpusPatch("test_3osc.pch2"), "From Panel");
    rig.run();
    CHECK(rig.client.state().slots[2].name == "From Panel");
    CHECK(rig.client.state().slots[2].session == rig.emulator.state().slots[2].session);
    CHECK(saved(rig.client.state().slots[2].patch) == saved(rig.emulator.state().slots[2].patch));
    CHECK(rig.events.slotChanges == std::vector<int>{2});

    g2::Performance perf;
    for (auto& s : perf.slots)
        s = g2::Patch::makeDefault();
    perf.header.slots[3].patchName = "Slot D";
    rig.emulator.loadPerformance(perf, "New Perf");
    rig.run();
    CHECK(rig.client.state().perfName == "New Perf");
    CHECK(rig.client.state().slots[3].name == "Slot D");
    CHECK(rig.client.state().perfSession == rig.emulator.state().perfSession);
    CHECK(rig.events.syncs == 3); // the first sync, the slot's, the performance's
}

TEST_CASE("session: performance upload")
{
    Rig rig;
    rig.plugIn();
    g2::Performance perf;
    for (auto& s : perf.slots)
        s = g2::Patch::makeDefault();
    perf.slots[1] = corpusPatch("test_3osc.pch2");
    perf.header.slots[1].patchName = "Oscs";
    rig.client.sendPerformance(perf, "My Perf");
    rig.run();
    CHECK(rig.emulator.state().perfName == "My Perf");
    CHECK(saved(rig.emulator.state().slots[1].patch) == saved(perf.slots[1]));
    CHECK(rig.client.state().perfName == "My Perf");
    CHECK(rig.client.state().slots[1].name == "Oscs");
    CHECK(saved(rig.client.state().slots[1].patch) == saved(perf.slots[1]));
}

TEST_CASE("session: LEDs and meters, once the slot's load reports are in")
{
    Rig rig;
    rig.plugIn();
    std::array<u8, kLedsPerSlot> leds{};
    leds[3] = 1;
    leds[39] = 3;
    rig.emulator.sendLeds(0, leds);
    std::array<std::uint16_t, kLedsPerSlot> meters{};
    meters[0] = 0x7E;
    rig.emulator.sendMeters(0, meters);
    rig.run();
    CHECK(rig.client.state().slots[0].leds == leds);
    CHECK(rig.client.state().slots[0].meters[0] == 0x7E);
    CHECK(rig.events.leds == std::vector<int>{0, 0});
    CHECK_FALSE(rig.client.waiting()); // never answered
}

TEST_CASE("session: MIDI learn and notes")
{
    Rig rig;
    rig.plugIn();
    rig.emulator.midiLearn(2, 74);
    rig.run();
    REQUIRE(rig.events.learned.size() == 1);
    CHECK(rig.events.learned[0].slot == 2);
    CHECK(rig.events.learned[0].cc == 74);
    rig.client.playNote(60, true);
    rig.client.playNote(60, false);
    rig.run();
    CHECK(rig.client.idle());
    const auto sent = rig.sentFrames();
    CHECK(sent[sent.size() - 2] == hexOf(g2test::vectorBytes("midi_and_notes", 0)));
    CHECK(sent.back() == hexOf(g2test::vectorBytes("midi_and_notes", 1)));
}

TEST_CASE("session: flash name lists in chunks, load and store")
{
    Rig rig;
    auto& bank = rig.emulator.flash()[0];
    for (u8 p = 0; p < 40; ++p)
        if (p % 5 != 1) // some empty programs
            bank[{0, p}] = {"Patch " + std::to_string(p), static_cast<u8>(p % 16), g2::savePatch(g2::Patch::makeDefault())};
    bank[{2, 100}] = {"Far Away", 3, g2::savePatch(corpusPatch("test_3osc.pch2"))};
    rig.emulator.flash()[1][{0, 0}] = {"A Perf", 0, {}};
    rig.plugIn();
    REQUIRE(rig.client.synced());

    const auto& names = rig.client.state().flash[0];
    REQUIRE(names.size() == bank.size());
    std::size_t i = 0;
    for (const auto& [where, entry] : bank) {
        INFO(entry.name);
        CHECK(names[i].bank == where.first);
        CHECK(names[i].prog == where.second);
        CHECK(names[i].name == entry.name);
        CHECK(names[i].category == entry.category);
        ++i;
    }
    REQUIRE(rig.client.state().flash[1].size() == 1);
    CHECK(rig.client.state().flash[1][0].name == "A Perf");
    // More than one chunk was needed.
    const auto requests = std::count_if(rig.emulator.received().begin(), rig.emulator.received().end(), [](const HostFrame& f) {
        return !f.molecules.empty() && f.molecules[0] == id::FlashDataRequest;
    });
    CHECK(requests > 3);

    rig.client.loadFromFlash(1, 2, 100);
    rig.run();
    CHECK(rig.client.state().slots[1].name == "Far Away");
    CHECK(saved(rig.client.state().slots[1].patch) == saved(corpusPatch("test_3osc.pch2")));

    rig.client.storeToFlash(1, 5, 9);
    rig.run();
    REQUIRE(rig.emulator.flash()[0].count({5, 9}));
    CHECK(rig.emulator.flash()[0][{5, 9}].name == "Far Away");
}

TEST_CASE("session: no version reply, then the synth answers")
{
    Rig rig;
    rig.emulator.setResponding(false);
    rig.plugIn();
    CHECK(rig.client.status() == Status::Looking);
    rig.clock.advance(9'999);
    rig.run();
    CHECK(rig.client.status() == Status::Looking);
    rig.clock.advance(1);
    rig.run();
    CHECK(rig.client.status() == Status::StillLooking);
    CHECK(rig.emulator.received().size() == 2); // the request again
    rig.emulator.setResponding(true);
    rig.clock.advance(10'000);
    rig.run();
    CHECK(rig.client.status() == Status::Connected);
    CHECK(rig.client.synced());
}

TEST_CASE("session: a reply that never comes is Lost Contact (after the configured retries)")
{
    SECTION("Clavia: the first timeout is fatal")
    {
        Rig rig;
        rig.plugIn();
        rig.emulator.setResponding(false);
        rig.client.selectVariation(0, 2);
        rig.run();
        CHECK(rig.client.waiting());
        rig.clock.advance(9'999);
        rig.run();
        CHECK(rig.client.connected());
        rig.clock.advance(1);
        rig.run();
        CHECK(rig.client.status() == Status::LostContact);
        CHECK(isFatal(rig.client.status()));
        CHECK_FALSE(rig.client.waiting());
        // Nothing is sent any more.
        const auto frames = rig.emulator.received().size();
        rig.client.selectVariation(0, 3);
        rig.run();
        CHECK(rig.emulator.received().size() == frames);
    }
    SECTION("with one retry, the message is sent again first")
    {
        Client::Options options;
        options.retries = 1;
        Rig rig(options);
        rig.plugIn();
        rig.emulator.setResponding(false);
        rig.client.selectVariation(0, 2);
        rig.run();
        const auto frames = rig.emulator.received().size();
        rig.clock.advance(10'000);
        rig.run();
        CHECK(rig.client.connected());
        CHECK(rig.emulator.received().size() == frames + 1);
        CHECK(hexOf(hostFrame(rig.emulator.received().back().hdr, rig.emulator.received().back().session,
                              rig.emulator.received().back().molecules))
              == hexOf(hostFrame(rig.emulator.received()[frames - 1].hdr, rig.emulator.received()[frames - 1].session,
                                 rig.emulator.received()[frames - 1].molecules)));
        rig.clock.advance(10'000);
        rig.run();
        CHECK(rig.client.status() == Status::LostContact);
    }
    SECTION("unsolicited traffic postpones the timeout")
    {
        Rig rig;
        rig.plugIn();
        rig.emulator.setResponding(false);
        rig.client.selectVariation(0, 2);
        rig.run();
        rig.clock.advance(8'000);
        rig.emulator.turnKnob(0, 1, 1, 0, 5);
        rig.run();
        rig.clock.advance(8'000);
        rig.run();
        CHECK(rig.client.connected());
    }
}

TEST_CASE("session: version replies the editor refuses")
{
    auto connectTo = [](auto setup) {
        Rig rig;
        setup(rig.emulator.version());
        rig.plugIn();
        return rig.client.status();
    };
    CHECK(connectTo([](VersionInfo& v) { v.model = VersionInfo::Rack; }) == Status::VersionMismatch);
    CHECK(connectTo([](VersionInfo& v) { v.model = VersionInfo::Native; }) == Status::VersionMismatch);
    CHECK(connectTo([](VersionInfo& v) { v.model = 9; }) == Status::UnsupportedModel);
    CHECK(connectTo([](VersionInfo& v) { v.marker = 0x0B; }) == Status::UnsupportedModel);
    CHECK(connectTo([](VersionInfo& v) { v.mode = 1; }) == Status::UpdateMode);
    CHECK(connectTo([](VersionInfo& v) { v.protocol = 0x0011; }) == Status::VersionMismatch);
    CHECK(connectTo([](VersionInfo& v) { v.model = VersionInfo::Engine; }) == Status::Connected);
    CHECK(connectTo([](VersionInfo& v) { v.model = VersionInfo::G2X; }) == Status::Connected);
}

TEST_CASE("session: errors while connected")
{
    SECTION("the synth reports an exception")
    {
        Rig rig;
        rig.plugIn();
        rig.emulator.failNextRequest(5);
        rig.client.selectVariation(0, 1);
        rig.run();
        CHECK(rig.events.errors == std::vector<u8>{5});
        CHECK(rig.client.status() == Status::SynthException);
        CHECK(std::string(exceptionText(5)).find("Stream execute") == 0);
    }
    SECTION("... or only fails the request")
    {
        Client::Options options;
        options.exceptionsAreFatal = false;
        Rig rig(options);
        rig.plugIn();
        rig.emulator.failNextRequest(3);
        std::vector<Molecule> reply;
        rig.client.request(Bubble::synth({Request{id::VoicesRequest}}), [&](const auto& r) { reply = r; });
        rig.run();
        CHECK(rig.client.connected());
        REQUIRE(reply.size() == 1);
        CHECK(std::get<Exception>(reply[0]).code == 3);
    }
    SECTION("a reply with a bad CRC is dropped, and the request times out")
    {
        Rig rig;
        rig.plugIn();
        rig.emulator.corruptNextMessage();
        rig.client.selectVariation(0, 1);
        rig.run();
        CHECK(rig.client.droppedMessages() == 1);
        CHECK(rig.client.waiting());
        rig.clock.advance(10'000);
        rig.run();
        CHECK(rig.client.status() == Status::LostContact);
    }
    SECTION("another device's version reply")
    {
        Rig rig;
        rig.plugIn();
        auto same = rig.emulator.version();
        rig.emulator.outgoing().push_back(deliver(versionMessage(encode(same)), true));
        rig.run();
        CHECK(rig.client.connected()); // identical: ignored
        same.firmware = 160;
        rig.emulator.outgoing().push_back(deliver(versionMessage(encode(same)), true));
        rig.run();
        CHECK(rig.client.status() == Status::NewDevice);
    }
    SECTION("unplugged, then plugged in again")
    {
        Rig rig;
        rig.plugIn();
        rig.link.unplug();
        rig.run();
        CHECK(rig.client.status() == Status::NoDevice);
        rig.plugIn();
        CHECK(rig.client.status() == Status::Connected);
        CHECK(rig.client.synced());
    }
    SECTION("restart() after a fatal status")
    {
        Rig rig;
        rig.plugIn();
        rig.emulator.failNextRequest(3);
        rig.client.selectVariation(0, 1);
        rig.run();
        REQUIRE(rig.client.status() == Status::SynthException);
        rig.client.restart();
        rig.run();
        CHECK(rig.client.status() == Status::Connected);
    }
}

TEST_CASE("session: extended-only delivery, and edits wait for the sync")
{
    Rig rig;
    rig.emulator.setMaxEmbedded(0); // every message on bulk-IN
    rig.plugIn();
    REQUIRE(rig.client.synced());
    // An edit made while a sync runs goes out after it (the user queue waits
    // for the cleaner queue, §4.2).
    rig.client.resync();
    rig.client.selectVariation(0, 4);
    rig.run();
    CHECK(rig.client.synced());
    CHECK(rig.emulator.state().slots[0].patch.header.activeVariation == 4);
    const auto sent = rig.sentFrames();
    const auto variation = hexOf(framesFor(Bubble::patch(0, rig.client.state().slots[0].session, {VariationSelect{4}}))[0]);
    CHECK(sent.back() == variation);
    CHECK(sent[sent.size() - 2] == hexOf(g2test::vectorBytes("init_sequence", 30))); // 7D 00 just before
}
