// Special panel controls (vocoder presets, note sequencer zoom/offset/clear/
// random, drum presets). Expected values come from running the original
// editor's code in an emulator (tools/special/emulate.py), see
// re/notes/special-controls.md.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/special.hpp"

#include <array>
#include <vector>

using namespace g2;
using namespace g2::special;

namespace {

using Bands = std::array<u8, 16>;

Patch patchWith(u8 type, u8& index)
{
    Patch p = Patch::makeDefault();
    index = edit::addModule(p, Location::Va, type, 0, 0);
    return p;
}

} // namespace

TEST_CASE("Rnd_GetC generator")
{
    EditorRandom r(0);
    CHECK(r.next() == 0x3619636bu);
    EditorRandom z(0);
    // 0 is never produced: the state that would lead to 0 restarts at the increment.
    std::uint32_t inv = 0x0bb38435u; // multiplicative inverse mod 2^32 (Newton)
    for (int i = 0; i < 5; ++i)
        inv *= 2u - 0x0bb38435u * inv;
    const std::uint32_t pre = (0u - 0x3619636bu) * inv;
    REQUIRE(pre * 0x0bb38435u + 0x3619636bu == 0);
    EditorRandom w(pre);
    CHECK(w.next() == 0x3619636bu);
}

TEST_CASE("vocoder preset buttons (emulated CPnlVocoderPreset::GetRequestedChange)")
{
    CHECK(vocoderBands(VocoderOp::Minus2) == Bands{3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0});
    CHECK(vocoderBands(VocoderOp::Minus1) == Bands{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0});
    CHECK(vocoderBands(VocoderOp::Zero) == Bands{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    CHECK(vocoderBands(VocoderOp::Plus1) == Bands{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    CHECK(vocoderBands(VocoderOp::Plus2) == Bands{0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14});
    CHECK(vocoderBands(VocoderOp::Invert) == Bands{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1});
    CHECK_THROWS_AS(vocoderBands(VocoderOp::Random), std::invalid_argument);

    // Rnd with gRandC = 0 and 12345 before the click, and the state after it.
    EditorRandom r0(0);
    CHECK(vocoderBands(VocoderOp::Random, &r0) == Bands{3, 10, 16, 16, 4, 4, 9, 1, 5, 13, 8, 16, 13, 14, 1, 0});
    CHECK(r0.state() == 0x019cb0d0u);
    EditorRandom r1(12345);
    CHECK(vocoderBands(VocoderOp::Random, &r1) == Bands{8, 11, 9, 16, 0, 10, 10, 9, 4, 3, 0, 3, 10, 3, 11, 15});
    CHECK(r1.state() == 0xe95069c9u);

    CHECK(std::string(vocoderOpLabel(VocoderOp::Invert)) == "Inv");

    u8 m = 0;
    Patch p = patchWith(kVocoderType, m);
    vocoderPreset(p, Location::Va, m, 2, VocoderOp::Invert);
    const auto& params = p.va.find(m)->params;
    CHECK(params[2][0] == 16);
    CHECK(params[2][15] == 1);
    CHECK(params[2][16] == p.va.find(m)->def()->params[16].defaultValue); // untouched
    CHECK(params[0][0] == 1);                                               // other variation untouched
    CHECK(edit::paramText(p, Location::Va, m, 0, 2) == "16");
    vocoderPreset(p, Location::Va, m, 2, VocoderOp::Plus1);
    CHECK(edit::paramText(p, Location::Va, m, 0, 2) == "Off");
    CHECK_THROWS_AS(vocoderPreset(p, Location::Va, m, 2, VocoderOp::Random), std::invalid_argument);
}

TEST_CASE("note sequencer view in custom data")
{
    u8 m = 0;
    Patch p = patchWith(kSeqNoteType, m);
    CHECK(noteSeqView(p, Location::Va, m) == NoteSeqView{1, 5}); // defaults without data
    setNoteSeqView(p, Location::Va, m, {2, 7});
    CHECK(*p.va.find(m)->customData == std::vector<u8>{0, 1, 2, 0, 1, 7});
    CHECK(noteSeqView(p, Location::Va, m) == NoteSeqView{2, 7});

    // Survives a save / load.
    const Patch q = loadPatch(savePatch(p));
    CHECK(noteSeqView(q, Location::Va, m) == NoteSeqView{2, 7});

    // Other records are kept after the view.
    p.va.find(m)->customData = std::vector<u8>{0, 1, 0, 0, 1, 3, 1, 2, 9, 65};
    CHECK(noteSeqView(p, Location::Va, m) == NoteSeqView{0, 3});
    setNoteSeqView(p, Location::Va, m, {1, 4});
    CHECK(*p.va.find(m)->customData == std::vector<u8>{0, 1, 1, 0, 1, 4, 1, 2, 9, 65});

    CHECK_THROWS_AS(setNoteSeqView(p, Location::Va, 99, {}), std::invalid_argument);
}

TEST_CASE("note sequencer zoom and offset buttons (emulated)")
{
    // CPnlNoteSeqZoom::OnClick
    CHECK(nextNoteSeqZoom(0) == 1);
    CHECK(nextNoteSeqZoom(1) == 2);
    CHECK(nextNoteSeqZoom(2) == 0);
    CHECK(nextNoteSeqZoom(3) == 0);
    CHECK(nextNoteSeqZoom(213) == 0);
    CHECK(noteSeqZoomBitmapX(0) == 22);
    CHECK(noteSeqZoomBitmapX(2) == 0);
    // CPnlNoteSeqOffset::HandleChangeRequests: 1 = right half, 2 = left half
    CHECK(stepNoteSeqOffset(0, true) == 1);
    CHECK(stepNoteSeqOffset(0, false) == 0);
    CHECK(stepNoteSeqOffset(5, true) == 6);
    CHECK(stepNoteSeqOffset(5, false) == 4);
    CHECK(stepNoteSeqOffset(9, true) == 9);
    CHECK(stepNoteSeqOffset(9, false) == 8);
    CHECK(noteSeqOffsetText(0) == "C0");
    CHECK(noteSeqOffsetText(9) == "C9");
}

TEST_CASE("note sequencer slider window (emulated CSeqSliderGUI)")
{
    // low high range res topcorr for zoom 0..2, offset 0..9
    struct Row {
        int low, high, range, pixels, topCorr;
    };
    for (u8 o = 0; o < 10; ++o) {
        const int top = o * 12 + 12;
        const std::array<Row, 3> expected{{
            {o * 12 - 24, top, 37, 2, 1},
            {o * 12 - 12, top, 25, 3, 0},
            {o * 12, top, 13, 6, -2},
        }};
        for (u8 z = 0; z < 3; ++z) {
            const auto w = noteSeqWindow({z, o});
            const auto& e = expected[z];
            CHECK(w.low == e.low);
            CHECK(w.high == e.high);
            CHECK(w.range == e.range);
            CHECK(w.pixels == e.pixels);
            CHECK(w.topCorr == e.topCorr);
            CHECK(w.range == w.high - w.low + 1);
        }
    }
    CHECK(noteSeqWindow({213, 5}).range == 13); // anything but 0/1 is zoom 2

    // CPnlSeqSlider::OnClick, y = -4..95 for every view: FNV-1a of the values.
    std::uint32_t h = 0x811c9dc5u;
    for (u8 z = 0; z < 3; ++z)
        for (u8 o = 0; o < 10; ++o)
            for (int y = -4; y < 96; ++y) {
                h ^= noteSeqWindow({z, o}).valueAt(y);
                h *= 0x01000193u;
            }
    CHECK(h == 0x047ef7c9u);
    const auto w = noteSeqWindow({1, 5});
    CHECK(w.valueAt(0) == 72);
    CHECK(w.valueAt(3) == 71);
    CHECK(w.valueAt(74) == 48);
    CHECK(w.valueAt(90) == 48);
    CHECK(noteSeqWindow({0, 0}).valueAt(80) == 0); // clipped at 0, never negative

    // The marker of a value sits where a click sets that value.
    for (u8 z = 0; z < 3; ++z)
        for (u8 o = 0; o < 10; ++o) {
            const auto win = noteSeqWindow({z, o});
            for (int v = std::max(win.low, 0); v <= win.high; ++v) {
                CHECK(win.valueAt(win.markerTop(v) + win.pixels - 1) == v);
                if (win.markerTop(v) >= 0)
                    CHECK(win.valueAt(win.markerTop(v)) == v);
            }
        }
    CHECK(w.markerTop(72) == 0);
    CHECK(w.markerTop(48) == 72);
    CHECK(w.below(47));
    CHECK(w.above(73));
}

TEST_CASE("note sequencer Clr / Rnd")
{
    u8 m = 0;
    Patch p = patchWith(kSeqNoteType, m);
    edit::setParam(p, Location::Va, m, 3, 1, 10);
    noteSeqClear(p, Location::Va, m, 1);
    for (u8 i = 0; i < 16; ++i)
        CHECK(p.va.find(m)->params[1][i] == 64); // emulated "Clr": 64 for every step

    setNoteSeqView(p, Location::Va, m, {2, 5}); // window 60..72
    EditorRandom rng(7);
    noteSeqRandom(p, Location::Va, m, 1, rng);
    for (u8 i = 0; i < 16; ++i) {
        const int v = p.va.find(m)->params[1][i];
        CHECK(v >= 60);
        CHECK(v <= 72);
    }
    const auto win = noteSeqWindow({0, 0}); // -24..12: values 0..12
    CHECK(noteSeqRandomValue(win, 0) == 0);
    CHECK(noteSeqRandomValue(win, 0x7fffffff) == 12);
    CHECK(noteSeqRandomValue(noteSeqWindow({1, 5}), 0x7fffffff) == 72);

    // The original's arithmetic (emulated with the given rand() results): the
    // 32-bit product overflows and nearly every result is the low limit.
    for (std::int32_t r : {0, 1, 1000, 85899345, 85899346, 1073741823, 2147483646, 171798691, 171798692})
        CHECK(noteSeqRandomOriginal(48, 25, r) == 48);
    for (std::int32_t r : {0, 58040098, 58040099, 2147483646})
        CHECK(noteSeqRandomOriginal(-24, 37, r) == 0);
}

TEST_CASE("drum presets (emulated CPnlDrumPresetSelector)")
{
    CHECK(std::string(drumPresetName(0)) == "Kick 1");
    CHECK(std::string(drumPresetName(5)) == "Snare 1");
    CHECK(std::string(drumPresetName(29)) == "Perc 6");
    CHECK_THROWS_AS(drumPresetName(30), std::invalid_argument);
    CHECK(drumPresetValues(29) == std::array<u8, 15>{30, 45, 62, 46, 127, 127, 55, 127, 28, 41, 2, 81, 55, 80, 97});

    // No two presets are equal, and each one matches itself.
    for (int i = 0; i < kDrumPresets; ++i)
        CHECK(drumPresetIndex(drumPresetValues(i)) == i);
    std::array<u8, 15> v = drumPresetValues(29);
    v[14] = 96;
    CHECK_FALSE(drumPresetIndex(v).has_value());
    CHECK_FALSE(drumPresetIndex(std::span<const u8>(v.data(), 14)).has_value());

    // A new DrumSynth has preset 0's values (the defaults); On/Off is not compared.
    u8 m = 0;
    Patch p = patchWith(kDrumSynthType, m);
    CHECK(drumPresetIndex(p, Location::Va, m, 0) == 0);
    edit::setParam(p, Location::Va, m, 15, 0, 0);
    CHECK(drumPresetIndex(p, Location::Va, m, 0) == 0);
    applyDrumPreset(p, Location::Va, m, 3, 7);
    CHECK(drumPresetIndex(p, Location::Va, m, 3) == 7);
    CHECK(drumPresetIndex(p, Location::Va, m, 0) == 0);

    // Up/down as HandleChangeRequests: (dir, index, matched) -> (index, matched, applied)
    struct Step {
        bool up;
        int index;
        bool matched;
        int newIndex;
        bool applied;
    };
    for (const Step s : {Step{true, 0, false, 0, true}, Step{true, 0, true, 1, true}, Step{true, 5, true, 6, true},
                         Step{true, 5, false, 5, true}, Step{true, 29, true, 29, false}, Step{true, 28, true, 29, true},
                         Step{false, 0, true, 0, false}, Step{false, 0, false, 0, false}, Step{false, 5, false, 4, true},
                         Step{false, 5, true, 4, true}}) {
        DrumPresetSelector sel{s.index, s.matched};
        const auto r = sel.step(s.up);
        CHECK(r.has_value() == s.applied);
        CHECK(sel.index == s.newIndex);
        CHECK(sel.matched == (s.applied || s.matched));
    }

    DrumPresetSelector sel;
    sel.update(p, Location::Va, m, 0);
    CHECK(sel.text() == "Kick 1");
    edit::setParam(p, Location::Va, m, 0, 0, 100);
    sel.update(p, Location::Va, m, 0);
    CHECK(sel.text() == "none");
    CHECK(sel.index == 0);
    CHECK(sel.step(p, Location::Va, m, 0, true) == 0); // no match: up re-applies the last preset
    CHECK(drumPresetIndex(p, Location::Va, m, 0) == 0);
    CHECK(sel.step(p, Location::Va, m, 0, true) == 1);
    CHECK(drumPresetIndex(p, Location::Va, m, 0) == 1);
    CHECK(sel.text() == "Kick 2");
    CHECK_THROWS_AS(applyDrumPreset(p, Location::Va, m, 0, -1), std::invalid_argument);
}
