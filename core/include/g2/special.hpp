// The special panel controls of the original Clavia editor (Mac v1.62):
// interactive CPnlCustom objects that PANL `Graph` elements create instead of
// a graph (CCustomObjectFactory::CreateCustom). See re/notes/special-controls.md.
//
//  id 22  CPnlVocoderPreset       Vocoder: "-2" "-1" "0" "+1" "+2" "Inv" "Rnd"
//                                 rewrite the 16 band assignments
//  id 25  CPnlNoteSeqZoom         SeqNote: zoom switch of the step sliders
//  id 26  CPnlNoteSeqOffset       SeqNote: scrolls the visible octave range
//  id 27  CPnlNoteSeqClrRnd       "Clr" / "Rnd" of the note sequencer (no
//                                 v1.62 panel uses it; SeqNote has the
//                                 parameters "Clear" 36 / "Random" 35 instead)
//  id 46  CPnlDrumPresetSelector  DrumSynth: preset name and up/down buttons
//
// The buttons change parameters of one variation the way a knob does (the
// original sends one parameter change per value, as a single undo step).
// No UI here: the functions compute values and edit a Patch.
#pragma once

#include "g2/patch.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace g2::special {

inline constexpr u8 kVocoderType = 108;
inline constexpr u8 kSeqNoteType = 121;
inline constexpr u8 kDrumSynthType = 58;

// ---- Random numbers ------------------------------------------------------------

// The editor's own generator Rnd_GetC (0x14ff0e): x = x * 0x0bb38435 +
// 0x3619636b (mod 2^32), with 0 replaced by 0x3619636b. The original seeds it
// once at start-up with the system milliseconds (CEditorApp::Initialize).
class EditorRandom {
public:
    explicit EditorRandom(std::uint32_t seed = 0) : state_(seed) {}
    std::uint32_t next();
    std::uint32_t state() const { return state_; }

private:
    std::uint32_t state_;
};

// ---- Vocoder (id 22) -----------------------------------------------------------

// The buttons left to right. Band parameter i (0..15) gets, for band i:
//   Minus2  i + 3, or 0 (Off) above 16      Plus1   i      (band 1 Off)
//   Minus1  i + 2, or 0 above 16            Plus2   max(i - 1, 0)
//   Zero    i + 1 (the default routing)     Invert  16 - i
//   Random  (Rnd_GetC() >> 8) * 17 / 2^24, truncated: uniform 0..16
enum class VocoderOp : u8 { Minus2, Minus1, Zero, Plus1, Plus2, Invert, Random };
inline constexpr int kVocoderOps = 7;
const char* vocoderOpLabel(VocoderOp op); // "-2", "-1", "0", "+1", "+2", "Inv", "Rnd"

// The 16 band values a button writes. Random draws one number per band, in
// band order; it needs `rng` (std::invalid_argument otherwise).
std::array<u8, 16> vocoderBands(VocoderOp op, EditorRandom* rng = nullptr);
// Writes them to parameters 0..15 of a Vocoder module in one variation.
void vocoderPreset(Patch& patch, Location loc, u8 module, u8 variation, VocoderOp op,
                   EditorRandom* rng = nullptr);

// ---- Note sequencer zoom and offset (ids 25, 26) ---------------------------------

// Editor-only state of a SeqNote module, kept in its custom data as two
// kind-0 records [0, 1, zoom] [0, 1, offset] (zoom control first, as the PANL
// lists it). Neither is a parameter nor sent to the synth.
//   zoom   0..2, default 1: 3, 2 or 1 octaves visible. A click cycles 0->1->2->0.
//   offset 0..9, default 5: the top of the window is note offset*12 + 12
//          ("C<offset>", the text the control shows). Right +1, left -1.
struct NoteSeqView {
    u8 zoom = 1;
    u8 offset = 5;
    bool operator==(const NoteSeqView&) const = default;
};
inline constexpr u8 kNoteSeqZooms = 3;
inline constexpr u8 kNoteSeqOffsets = 10;

// Reads the view from custom data (defaults when absent). Values are returned
// as stored, even out of range (one corpus file has zoom 213).
NoteSeqView noteSeqView(const Module& module);
NoteSeqView noteSeqView(const Patch& patch, Location loc, u8 module);
// The custom data the original writes for a view: {0,1,zoom, 0,1,offset}.
std::vector<u8> noteSeqCustomData(NoteSeqView view);
// Stores the view in a SeqNote module's custom data (other records are kept).
void setNoteSeqView(Patch& patch, Location loc, u8 module, NoteSeqView view);

u8 nextNoteSeqZoom(u8 zoom);                // CPnlNoteSeqZoom::OnClick
u8 stepNoteSeqOffset(u8 offset, bool right); // CPnlNoteSeqOffset::HandleChangeRequests (clamped 0..9)
std::string noteSeqOffsetText(u8 offset);    // "C0".."C9" (CPnlNoteSeqOffset::fOcts)
// Source x of the zoom switch's 11x91 cell in bitmap 877: (2 - zoom) * 11.
int noteSeqZoomBitmapX(u8 zoom);

// The visible value window of the 16 step sliders (CSeqSliderGUI, 11x75 px).
// A zoom other than 0 or 1 behaves as 2, an offset above 9 as 9.
struct NoteSeqWindow {
    int zoom = 1;
    int low = 48;    // lowest visible value: offset*12 - 24 / -12 / 0 (may be < 0)
    int high = 72;   // highest visible value: offset*12 + 12
    int range = 25;  // high - low + 1: 37 / 25 / 13
    int pixels = 3;  // rows per value: 2 / 3 / 6
    int topCorr = 0; // click y correction: 1 / 0 / -2

    bool below(int value) const { return value < low; }
    bool above(int value) const { return value > high; }
    // Top row of the black marker (pixels rows high) of a value inside the
    // window: 73 - 2d / 72 - 3d / 70 - 6d with d = value - low.
    int markerTop(int value) const;
    // The value a click at view row y sets (CPnlSeqSlider::OnClick):
    // d = range - 1 - (y - topCorr) / pixels (C division), at least 0;
    // value = max(low + d, 0). Never above `high` for y >= 0 (inside the view).
    u8 valueAt(int y) const;
    // Source x of the slider's column in bitmap 874: 22 / 11 / 0.
    int bitmapX() const;
};
NoteSeqWindow noteSeqWindow(NoteSeqView view);

// ---- Note sequencer Clr / Rnd (id 27) --------------------------------------------

// "Clr": every step to 64 (E4), steps 0..15 of one variation.
void noteSeqClear(Patch& patch, Location loc, u8 module, u8 variation);
// "Rnd": every step to a random value of the visible window, clipped at 0:
// lo = max(low, 0), n = range + min(low, 0) values, value = lo + n * r / 2^31
// with r = rng.next() >> 1. This is the intended behaviour; the original's
// code, which no panel reaches, is broken (see rndOriginal).
void noteSeqRandom(Patch& patch, Location loc, u8 module, u8 variation, EditorRandom& rng);
u8 noteSeqRandomValue(const NoteSeqWindow& window, std::uint32_t r31);
// What CPnlNoteSeqClrRnd::GetRequestedChange (0x91ae0) computes for a slider
// low limit / range and a libc rand() result: lo + int32(n * r) / 0x7fffffff,
// where the 32-bit product overflows, so the result is almost always `lo`.
u8 noteSeqRandomOriginal(int low, int range, std::int32_t rand);

// ---- Drum presets (id 46) --------------------------------------------------------

inline constexpr int kDrumPresets = 30;
inline constexpr int kDrumPresetParams = 15; // DrumSynth parameters 0..14 (all but On/Off)

const char* drumPresetName(int preset); // "Kick 1" .. "Perc 6"
const std::array<u8, kDrumPresetParams>& drumPresetValues(int preset);
// The first preset whose 15 values equal values[0..14] (PresetComparison), or
// nullopt ("none"). Fewer than 15 values never match.
std::optional<int> drumPresetIndex(std::span<const u8> values);
std::optional<int> drumPresetIndex(const Patch& patch, Location loc, u8 module, u8 variation);
// Writes a preset's 15 values to one variation of a DrumSynth module.
void applyDrumPreset(Patch& patch, Location loc, u8 module, u8 variation, int preset);

// The selector's own state (not saved): the last matched or chosen preset and
// whether the parameters match a preset now. The display shows the preset's
// name, or "none".
struct DrumPresetSelector {
    int index = 0;
    bool matched = false;

    // After a parameter change: PresetComparison. Without a match `index` is kept.
    void update(std::span<const u8> values);
    void update(const Patch& patch, Location loc, u8 module, u8 variation);
    std::string text() const;
    // An up (next) or down (previous) click: the preset to apply, or nullopt
    // when nothing happens. Up from a matched preset moves to the next one; up
    // without a match applies `index` itself; up stops at 29, down at 0.
    std::optional<int> step(bool up);
    // step(), then applyDrumPreset() of the result. Returns the applied preset.
    std::optional<int> step(Patch& patch, Location loc, u8 module, u8 variation, bool up);
};

} // namespace g2::special
