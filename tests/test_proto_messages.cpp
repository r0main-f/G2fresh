#include <catch2/catch_test_macros.hpp>

#include "g2/proto/bubble.hpp"
#include "g2/proto/sections.hpp"
#include "g2/proto/state.hpp"
#include "proto_vectors.hpp"
#include "test_support.hpp"

#include <string>

using namespace g2::proto;
using g2test::hexOf;
using g2test::vectorBytes;

namespace {

// The single frame of a bubble, as hex.
std::string frameOf(const Bubble& b)
{
    const auto frames = framesFor(b);
    REQUIRE(frames.size() == 1);
    return hexOf(frames.front());
}

// True if `scenario` has a host->device vector with exactly these bytes.
bool inScenario(const char* scenario, const std::string& hex)
{
    for (const auto& v : g2test::kProtoVectors)
        if (std::string(v.scenario) == scenario && v.hostToDevice() && v.hex && hexOf(g2test::bytesOf(v.hex)) == hex)
            return true;
    return false;
}

// The molecules of a device->host vector (embedded packet, or bulk message).
DeviceMessage messageOf(const char* scenario, int step)
{
    const auto& v = g2test::vectorAt(scenario, step);
    const auto bytes = g2test::bytesOf(v.hex);
    std::vector<u8> message = bytes;
    if (v.interrupt())
        message = parseInterrupt(bytes).message;
    const auto m = parseDeviceMessage(message);
    REQUIRE(m);
    return *m;
}

template <class T>
T single(const DeviceMessage& m)
{
    const auto molecules = decode(m.body);
    REQUIRE(molecules.size() == 1);
    REQUIRE(std::holds_alternative<T>(molecules.front()));
    return std::get<T>(molecules.front());
}

} // namespace

TEST_CASE("messages: the Clavia sync requests (init_sequence)")
{
    const u8 perf = 5, slotA = 0x11;
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({EditorSync{true}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({SessionRequest{4}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({Request{id::SynthDataRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({Request{id::MidiLearnRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::performance(perf, {Request{id::PerfHeaderRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::performance(perf, {Request{id::GlobalPageFocusRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::performance(perf, {Request{id::SlotFocusRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({SessionRequest{0}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {Request{id::CompletePatchRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {Request{id::PageFocusRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {Request{id::CurrentNotesRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {Request{id::TextpadRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {LocationRequest{id::PatchLoadRequest, 1}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {LocationRequest{id::PatchLoadRequest, 0}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {Request{id::FlushBlink}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::patch(0, slotA, {Request{id::ParamFocusRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({Request{id::VoicesRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({Request{id::ClockInfoRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::performance(perf, {Request{id::GlobalKnobMapRequest}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({FlashCommand{id::FlashDataRequest, 0, 0, 0}}))));
    CHECK(inScenario("init_sequence", frameOf(Bubble::synth({EditorSync{false}}))));
    // Verhue's sequence: the same frames with session 0, plus 28 (patch name).
    CHECK(inScenario("init_sequence_verhue", frameOf(Bubble::patch(0, 0, {Request{id::PatchNameRequest}}))));
    CHECK(inScenario("init_sequence_verhue", frameOf(Bubble::performance(0, {Request{id::PerfHeaderRequest}}))));
}

TEST_CASE("messages: the sync replies decode (init_sequence)")
{
    CHECK(messageOf("init_sequence", 4).isResponse());
    single<Ack>(messageOf("init_sequence", 4));
    const auto perfSession = single<SessionNumber>(messageOf("init_sequence", 6));
    CHECK(perfSession.id == id::SessionDump);
    CHECK(perfSession.slot == 4);
    CHECK(perfSession.session == 5);
    const auto slotA = single<SessionNumber>(messageOf("init_sequence", 16));
    CHECK(slotA.slot == 0);
    CHECK(slotA.session == 0x11);
    const auto learn = single<MidiLearn>(messageOf("init_sequence", 11));
    CHECK(learn.slot == 0);
    CHECK(learn.cc == 0);

    // 03, the synth settings (§7.1): 38 bytes after the name.
    const auto synth = single<SynthData>(messageOf("init_sequence", 9)).settings;
    CHECK(synth.name == "G2 Engine");
    CHECK_FALSE(synth.perfMode);
    CHECK_FALSE(synth.memoryProtect);
    CHECK(synth.midiChannels == std::array<u8, 4>{0, 1, 2, 3});
    CHECK(synth.globalChannel == 15);
    CHECK(synth.sysExId == 16);
    CHECK(synth.localOn);
    CHECK(synth.programChangeMode == 1);
    CHECK(synth.controllerMode == 1);
    CHECK_FALSE(synth.sustainPolarity);
}

TEST_CASE("messages: parameter, morph and variation edits (param_changes)")
{
    CHECK(frameOf(Bubble::realtimeFor(0, 0, {ParamChange{1, 1, 0, 64, 0}})) == hexOf(vectorBytes("param_changes", 0)));
    CHECK(frameOf(Bubble::realtimeFor(0, 0, {ParamFocus{0, 1, 1, 0}})) == hexOf(vectorBytes("param_changes", 1)));
    CHECK(frameOf(Bubble::realtimeFor(0, 0, {MorphChange{1, 1, 0, 0, 0x2B, 0}})) == hexOf(vectorBytes("param_changes", 2)));
    CHECK(frameOf(Bubble::patch(2, 7, {MorphChange{0, 5, 2, 3, -16, 2}})) == hexOf(vectorBytes("param_changes", 3)));
    CHECK(frameOf(Bubble::patch(1, 3, {VariationSelect{2}})) == hexOf(vectorBytes("param_changes", 4)));
    CHECK(frameOf(Bubble::patch(0, 0, {VariationCopy{0, 8}})) == hexOf(vectorBytes("param_changes", 5)));

    // From the synth's panel: not responses, hdr = slot.
    const auto knob = messageOf("param_changes", 6);
    CHECK_FALSE(knob.isResponse());
    CHECK(knob.slot() == 1);
    CHECK(knob.session == 3);
    const auto p = single<ParamChange>(knob);
    CHECK(p.location == 1);
    CHECK(p.module == 4);
    CHECK(p.param == 2);
    CHECK(p.value == 127);
    CHECK(p.variation == 0);
    const auto focus = single<ParamFocus>(messageOf("param_changes", 7));
    CHECK(focus.flag == 1);
    CHECK(focus.location == 1);
    CHECK(focus.module == 3);
    CHECK(focus.param == 5);
}

TEST_CASE("messages: patch edits (patch_edits)")
{
    CHECK(frameOf(Bubble::patch(0, 0, {VariationCopy{8, 1}})) == hexOf(vectorBytes("patch_edits", 0)));
    auto notes = [](const std::string& text) {
        return Bubble::patch(0, 0, {sectionMolecule(g2::file::Section{g2::file::kTextpad, g2::file::Textpad{text}, 0, {}})});
    };
    CHECK(frameOf(notes("h")) == hexOf(vectorBytes("patch_edits", 1)));
    CHECK(frameOf(notes("ha")) == hexOf(vectorBytes("patch_edits", 2)));
    CHECK(frameOf(notes("hal")) == hexOf(vectorBytes("patch_edits", 3)));
    CHECK(frameOf(Bubble::patch(0, 0, {ModuleMove{1, 2, 3, 4}})) == hexOf(vectorBytes("patch_edits", 6)));
    CHECK(frameOf(Bubble::patch(0, 0, {CableEdit{id::CableConnect, true, 0, 2, 0, true, 3, 1, false}}))
          == hexOf(vectorBytes("patch_edits", 7)));
    CHECK(frameOf(Bubble::patch(0, 0, {CableEdit{id::CableDelete, true, 0, 2, 0, true, 3, 1, false}}))
          == hexOf(vectorBytes("patch_edits", 8)));
    CHECK(frameOf(Bubble::patch(0, 0, {KnobAssign{2, 0, 1, 0, 5}})) == hexOf(vectorBytes("patch_edits", 9)));
    CHECK(frameOf(Bubble::patch(0, 0, {KnobDeassign{id::KnobDeassign, 5}})) == hexOf(vectorBytes("patch_edits", 10)));
    CHECK(frameOf(Bubble::patch(0, 0, {CtrlAssign{1, 2, 0, 7}})) == hexOf(vectorBytes("patch_edits", 11)));
    CHECK(frameOf(Bubble::patch(0, 0, {CtrlDeassign{7}})) == hexOf(vectorBytes("patch_edits", 12)));
    CHECK(frameOf(Bubble::patch(0, 0, {ModuleRename{1, 2, "Osc1"}})) == hexOf(vectorBytes("patch_edits", 13)));
    CHECK(frameOf(Bubble::patch(0, 0, {NameDump{id::PatchName, "SixteenCharsName"}})) == hexOf(vectorBytes("patch_edits", 14)));

    // Verhue's param paste: a partial 4D section, alone or after 3 morph changes.
    const auto paste = parseHostFrame(vectorBytes("patch_edits", 5));
    REQUIRE(paste);
    const auto molecules = decode(paste->molecules);
    REQUIRE(molecules.size() == 4);
    CHECK(std::get<MorphChange>(molecules[0]).range == 0x2B);
    const auto& section = std::get<SectionDump>(molecules[3]).section;
    CHECK(section.id == g2::file::kParamList);
    const auto& list = std::get<g2::file::ParamList>(section.payload);
    CHECK(list.location == 1);
    REQUIRE(list.modules.size() == 1);
    CHECK(list.modules[0].variations.size() == 1);

    // The generic ack, response bit and slot A.
    const auto ack = messageOf("patch_edits", 15);
    CHECK(ack.isResponse());
    CHECK(ack.slot() == 0);
    single<Ack>(ack);
}

TEST_CASE("messages: a module, as CMModuleNew writes it")
{
    // OscB (type 7) has modes; their count comes from the module database.
    const auto* def = g2::db::find(7);
    REQUIRE(def);
    ModuleNew m{7, 1, 3, 0, 5, 2, 0, 0, std::vector<u8>(def->modes.size(), 1), "OscB1"};
    std::vector<u8> bytes;
    encode(m, bytes);
    const auto back = decode(bytes);
    REQUIRE(back.size() == 1);
    const auto& n = std::get<ModuleNew>(back.front());
    CHECK(n.index == 3);
    CHECK(n.modes == m.modes);
    CHECK(n.name == "OscB1");
}

TEST_CASE("messages: LEDs and meters (led_vu)")
{
    const auto leds = messageOf("led_vu", 1);
    CHECK_FALSE(leds.isResponse());
    CHECK(leds.isBlink());
    CHECK(leds.slot() == 0);
    CHECK(leds.session == 0x11);
    SlotState slot;
    applyToSlot(slot, single<Blink>(leds));
    CHECK(slot.leds[0] == 1);
    CHECK(slot.leds[1] == 2);
    CHECK(slot.leds[5] == 3);
    CHECK(slot.leds[2] == 0);
    CHECK(encodeLeds(slot.leds) == single<Blink>(leds).data);

    const auto meters = messageOf("led_vu", 2);
    CHECK(meters.slot() == 1);
    applyToSlot(slot, single<Blink>(meters));
    CHECK(slot.meters[0] == 0x0010); // big-endian words [I]
    CHECK(slot.meters[1] == 0x0203);
    CHECK(frameOf(Bubble::patch(0, 0x11, {Request{id::FlushBlink}})) == hexOf(vectorBytes("led_vu", 3)));
}

TEST_CASE("messages: session numbers and releases (session_numbers)")
{
    CHECK(frameOf(Bubble::synth({SessionRequest{2}})) == hexOf(vectorBytes("session_numbers", 0)));
    const auto c = single<SessionNumber>(messageOf("session_numbers", 1));
    CHECK(c.slot == 2);
    CHECK(c.session == 0x21);
    const auto release = messageOf("session_numbers", 2);
    CHECK_FALSE(release.isResponse());
    CHECK(release.slot() == kSlotSynth);
    const auto r = single<SessionNumber>(release);
    CHECK(r.id == id::PatchRelease);
    CHECK(r.slot == 1);
    CHECK(r.session == 5);
    CHECK(single<PerformanceRelease>(messageOf("session_numbers", 3)).session == 6);
    // Verhue's compound form: 1F followed by four 36.
    const auto compound = decode(messageOf("session_numbers", 5).body);
    REQUIRE(compound.size() == 5);
    CHECK(std::get<PerformanceRelease>(compound[0]).session == 6);
    for (int i = 0; i < 4; ++i) {
        const auto& s = std::get<SessionNumber>(compound[static_cast<std::size_t>(1 + i)]);
        CHECK(s.slot == i);
        CHECK(s.session == 1 + i);
    }
}

TEST_CASE("messages: MIDI and notes (midi_and_notes)")
{
    CHECK(frameOf(Bubble::synth({PlayNote{false, 60}})) == hexOf(vectorBytes("midi_and_notes", 0)));
    CHECK(frameOf(Bubble::synth({PlayNote{true, 60}})) == hexOf(vectorBytes("midi_and_notes", 1)));
    CHECK(frameOf(Bubble::patch(0, 0, {Request{id::SendCtrlSnap}})) == hexOf(vectorBytes("midi_and_notes", 2)));
    const auto learn = single<MidiLearn>(messageOf("midi_and_notes", 4));
    CHECK(learn.slot == 0);
    CHECK(learn.cc == 63);
}

TEST_CASE("messages: acks and exceptions (errors_and_acks)")
{
    single<Ack>(messageOf("errors_and_acks", 0));
    const auto e = messageOf("errors_and_acks", 1);
    CHECK(e.slot() == 2);
    CHECK(single<Exception>(e).code == 3);
    // The corrupted copy of the ack is dropped.
    const auto bad = parseInterrupt(vectorBytes("errors_and_acks", 2));
    CHECK_FALSE(parseDeviceMessage(bad.message));
}

TEST_CASE("messages: flash banks (bank_ops)")
{
    CHECK(frameOf(Bubble::synth({FlashCommand{id::FlashDataRequest, 0, 0, 0}})) == hexOf(vectorBytes("bank_ops", 0)));
    CHECK(frameOf(Bubble::synth({FlashCommand{id::FlashLoad, 0, 1, 2}})) == hexOf(vectorBytes("bank_ops", 3)));
    CHECK(frameOf(Bubble::synth({FlashCommand{id::FlashStore, 4, 0, 7}})) == hexOf(vectorBytes("bank_ops", 4)));
    CHECK(frameOf(Bubble::synth({FlashDelete{0, 2, 16, 0}})) == hexOf(vectorBytes("bank_ops", 6)));

    const auto list = single<FlashData>(messageOf("bank_ops", 2));
    CHECK(list.flag == 1);
    CHECK(list.type == 0);
    using K = FlashItem::Kind;
    REQUIRE(list.items.size() == 5);
    CHECK(list.items[0].kind == K::SetEntry);
    CHECK(list.items[1].kind == K::Name);
    CHECK(list.items[1].name == "Init");
    CHECK(list.items[1].category == 0);
    CHECK(list.items[2].kind == K::Empty);
    CHECK(list.items[3].name == "Bass 1");
    CHECK(list.items[3].category == 3);
    CHECK(list.items[4].kind == K::EndOfChunk);

    const auto result = single<FlashResult>(messageOf("bank_ops", 5));
    CHECK(result.type == 1);
    CHECK(result.prog == 7);
    CHECK(result.result == 0);
}

TEST_CASE("messages: patch and performance uploads (patch_transfer)")
{
    const auto& patchStep = g2test::vectorAt("patch_transfer", 0);
    const auto& perfStep = g2test::vectorAt("patch_transfer", 1);
    REQUIRE(patchStep.prefix);
    REQUIRE(perfStep.prefix);

    const auto patch = g2::loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    const auto frames = framesFor(patchUpload(0, patch, "MyPatch"));
    REQUIRE(frames.size() == 1);
    const auto prefix = g2test::bytesOf(patchStep.prefix);
    CHECK(hexOf(std::span<const u8>(frames[0]).subspan(2, prefix.size())) == hexOf(prefix)); // 01 28 53 37 ...

    // 37 + the 18 sections, in file order, with 10 variations in the parameter lists.
    const auto f = parseHostFrame(frames[0]);
    REQUIRE(f);
    const auto molecules = decode(f->molecules);
    REQUIRE(molecules.size() == 19);
    const std::array<u8, 18> order{0x21, 0x4A, 0x4A, 0x69, 0x52, 0x52, 0x4D, 0x4D, 0x4D,
                                   0x65, 0x62, 0x60, 0x5B, 0x5B, 0x5B, 0x5A, 0x5A, 0x6F};
    for (std::size_t i = 0; i < order.size(); ++i)
        CHECK(idOf(molecules[i + 1]) == order[i]);
    for (const auto& m : molecules)
        if (const auto* s = std::get_if<SectionDump>(&m))
            if (const auto* l = std::get_if<g2::file::ParamList>(&s->section.payload); l && !l->modules.empty())
                CHECK(l->variationCount == kUsbVariations);

    g2::Performance perf;
    for (auto& slot : perf.slots)
        slot = g2::Patch::makeDefault();
    const auto perfFrames = framesFor(performanceUpload(perf, "MyPatch"));
    REQUIRE(perfFrames.size() == 1);
    const auto perfPrefix = g2test::bytesOf(perfStep.prefix);
    CHECK(hexOf(std::span<const u8>(perfFrames[0]).subspan(2, perfPrefix.size())) == hexOf(perfPrefix)); // 01 2C 42 37 ...

    CHECK(frameOf(Bubble::patch(0, 0x11, {Request{id::CompletePatchRequest}})) == hexOf(vectorBytes("patch_transfer", 2)));
}

TEST_CASE("messages: upload sections go back to the file form")
{
    for (const auto& path : g2test::corpusFiles()) {
        g2::Patch patch;
        try {
            patch = g2::loadPatch(g2test::readBytes(path));
        } catch (const std::exception&) {
            continue; // performances
        }
        INFO(path.string());
        PatchAssembler a;
        for (const auto& s : uploadSections(patch))
            CHECK(a.add(s));
        REQUIRE(a.complete());
        // Section for section (the file's text header is not part of an upload).
        const auto back = a.patch().toSections();
        const auto original = patch.toSections();
        REQUIRE(back.size() == original.size());
        for (std::size_t i = 0; i < back.size(); ++i)
            CHECK(g2::file::encodeSection(back[i]) == g2::file::encodeSection(original[i]));
    }
}

TEST_CASE("messages: unknown or malformed molecules stay raw and round-trip")
{
    const std::vector<u8> unknown{0x41, 1, 2, 3};
    auto m = decode(unknown);
    REQUIRE(m.size() == 1);
    CHECK(std::get<Raw>(m[0]).id == 0x41);
    CHECK(encode(m) == unknown);

    // A known molecule cut short, after a good one.
    const std::vector<u8> cut{0x7D, 0x01, 0x40, 0x01, 0x02};
    m = decode(cut);
    REQUIRE(m.size() == 2);
    CHECK(std::holds_alternative<EditorSync>(m[0]));
    CHECK(std::holds_alternative<Raw>(m[1]));
    CHECK(encode(m) == cut);

    // Non-zero padding in a bit field (25's pad4) is kept raw.
    const std::vector<u8> padded{0x25, 0x02, 0x00, 0x41, 0x00, 0x05};
    m = decode(padded);
    REQUIRE(m.size() == 1);
    CHECK(std::holds_alternative<Raw>(m[0]));
    CHECK(encode(m) == padded);

    // A section whose length runs past the message.
    const std::vector<u8> truncated{0x6F, 0x00, 0x10, 'a'};
    m = decode(truncated);
    REQUIRE(m.size() == 1);
    CHECK(std::holds_alternative<Raw>(m[0]));
    CHECK(encode(m) == truncated);
}

TEST_CASE("messages: a bubble too big for one frame is split at molecule boundaries")
{
    std::vector<Molecule> molecules;
    for (int i = 0; i < 9000; ++i)
        molecules.push_back(ParamChange{1, 1, 0, static_cast<u8>(i & 0x7F), 0}); // 6 bytes each
    const auto frames = framesFor(Bubble::patchVoid(0, molecules));
    REQUIRE(frames.size() == 1); // 54000 bytes still fit
    molecules.insert(molecules.end(), molecules.begin(), molecules.end());
    const auto split = framesFor(Bubble::patchVoid(0, molecules));
    REQUIRE(split.size() == 2);
    const auto first = parseHostFrame(split[0]);
    const auto last = parseHostFrame(split[1]);
    REQUIRE(first);
    REQUIRE(last);
    CHECK((first->hdr & kBubbleBegin) != 0);
    CHECK((first->hdr & kBubbleEnd) == 0);
    CHECK((last->hdr & kBubbleBegin) == 0);
    CHECK((last->hdr & kBubbleEnd) != 0);
    CHECK(decode(first->molecules).size() + decode(last->molecules).size() == molecules.size());
}
