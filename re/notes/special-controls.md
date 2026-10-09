# Special panel controls (Graph Func 22, 25, 26, 27, 46)

Clavia G2 editor, Mac v1.62 (`original/mac/G2Editor_i386`). These PANL `Graph`
elements do not draw a graph: `CCustomObjectFactory::CreateCustom` (0x87736)
builds an interactive `CPnlCustom` instead (see the table at the end of
`module-graphs.md`). Port: `core/include/g2/special.hpp`, `core/src/special.cpp`
(namespace `g2::special`), tests in `tests/test_special.cpp`. Every value below
was checked by running the original code in unicorn
(`tools/special/emulate.py`, which reads the binary at run time).

## How a custom control changes parameters

A button of the control sets a flag and calls the panel's
`CustomDependencyRequestChange` (`CPanel`, 0xc0c86; container vtable +8).
That loops over the panel's custom controls and calls
`CPnlCustom::GetDependencyRequestedChange` (0xc9240) until it returns false;
that one walks the control's Dependencies (parameter type only) and calls the
control's `GetRequestedChange(depIndex, &value)` (vtable +0xdc). The controls
keep a counter (start 0x10 = idle, reset to 0 by the click) and accept only
`depIndex == counter`, then increment it, so each dependency is asked once,
in order. Each accepted value becomes a `CMParamChange` on the dependency's
parameter for `CPatch::GetFocusedParamSetting()` (the current variation; 0x7f
in a mode flagged at `CPatch+0x68`), all pushed into one `CUndoRedoPair`
(one undo step). So the buttons behave like turning the parameters' knobs in
the current variation.

The controls' own state (zoom, offset) is saved through
`GetCustomData`/`SetCustomData`: `CPanel::GetCustomData` (0xbfc3e) /
`SetCustomData` (0xbe3a2) reset the module's custom-data stream and call each
custom item in panel order; each writes/reads its own bytes.

Two random generators exist:
* `Rnd_GetC` (0x14ff0e): `x = x * 0x0bb38435 + 0x3619636b` (32 bit), 0 replaced
  by 0x3619636b; state `gRandC` (0x34e42c), seeded once with
  `CTimer::GetSystemMS()` in `CEditorApp::Initialize`. Port: `EditorRandom`.
* libc `rand()`, re-seeded with `srand(time(0)); rand();` before each "Rnd"
  click of the sequencer controls (and in `CPnlVocoderPreset`'s constructor,
  which does not use it).

Neither is the BSD `random()` of the randomizer (`g2::mutate::Random`).

## 22 `CPnlVocoderPreset` (Vocoder, deps 0..15)

Ctor 0x9354a, size 154x12, seven `CTextPushButton`s 22 px apart at +0x74,
+0xf0, ... +0x35c: "-2" "-1" "0" "+1" "+2" "Inv" "Rnd"; flags +0x3d8..+0x3de,
counter +0x3df. `HandleChangeRequests` 0x922d2, `GetRequestedChange` 0x92466.

Band parameter i (0-based, value 0 = Off, 1..16 = analysis band; default i+1)
gets:

| button | value | |
|---|---|---|
| -2 | i + 3, 0 if > 16 | bands 15, 16 Off |
| -1 | i + 2, 0 if > 16 | band 16 Off |
| 0 | i + 1 | the default routing |
| +1 | i | band 1 Off |
| +2 | max(i - 1, 0) | bands 1, 2 Off |
| Inv | 16 - i | reversed |
| Rnd | `(int)((double)(Rnd_GetC() >> 8) * 17.0 * 2^-24)` | uniform 0..16, Off included, one draw per band in order |

Shifts clamp to Off (no wrap). No custom data.

## 25 `CPnlNoteSeqZoom` / 26 `CPnlNoteSeqOffset` (SeqNote)

Both have Dependencies 0..15 (the step sliders, `Knob` type `SeqSlider`,
`CPnlSeqSlider`), and change no parameter (`GetRequestedChange` returns 0).

**Storage.** SeqNote's custom data is `[0, 1, zoom] [0, 1, offset]`
(`GetCustomData` 0x9268a / 0x9285a write `0, 1, value`; `SetCustomData`
0x952e0 / 0x92cba read three bytes and keep the third). Zoom comes first
(PANL order). Corpus (1,677 SeqNotes): zoom 0..2, offset 0..9, most
combinations; one file has zoom 213, one has no custom data.

**Zoom** (+0x76, ctor 0x93342 default 1, 11x91): `OnClick` 0x927ce:
`zoom = zoom + 1 < 3 ? zoom + 1 : 0`. `Draw` 0x926f8: cell x = (2 - zoom) * 11
of bitmap 877, 11x91. Then `UpdateDependentSliders` (0x95226) calls
`CPnlSeqSlider::SetZoom` on the 16 sliders.

**Offset** (+0x136, ctor 0x930be default 5, 21x25): a `CTextView` showing
`fOcts[offset]` = "C0".."C9" (`SetupState` 0x92afc) above a `CLeftRightButton`
(`Click` 0x77258: x > left + 10 -> 1, else 2). `HandleChangeRequests`
0x92a5c: 1 -> offset + 1 up to 9, 2 -> offset - 1 down to 0, then
`SetOffset` on the sliders (0x92bfc).

**Slider window** (`CSeqSliderGUI`, 0x7c734 `SetZoom`, 0x7c780 `SetOffset`,
0x7c7cc..0x7c824 getters; view 11x75, `kHeight`/`kWidth`):

| zoom | low (+0x21, signed) | high (+0x20) | range | px per value | topCorr | bitmap 874 column x |
|---|---|---|---|---|---|---|
| 0 | o*12 - 24 | o*12 + 12 | 37 | 2 | 1 | 22 |
| 1 | o*12 - 12 | o*12 + 12 | 25 | 3 | 0 | 11 |
| 2 | o*12 | o*12 + 12 | 13 | 6 | -2 | 0 |

Values are note numbers (60 = C4, 72 = C5), so the top of the window is
always "C<offset>" (the offset text) and zoom picks 3, 2 or 1 octaves below
it. Zoom values other than 0/1 act as 2 everywhere.

`CPnlSeqSlider::OnClick` (0xccf66), click at view row y (rect top subtracted):
`d = range - (y - topCorr) / px - 1` (signed idiv, byte arithmetic), `d < 0 -> 0`,
`if low + d < 0: d = -low`, value = low + d. So rows map top-down from `high`
to `max(low, 0)`; the bottom rows clamp. Then the generic vertical drag of
`CPnlControl` (relative). Emulated for every zoom/offset and y = -4..95.

`CSeqSliderGUI::Draw` (0x7fd86), d = value - low, no morph:
* low <= value <= high: bitmap 874 column rows [0, m) then rows
  [m + px, 75) (76 at zoom 2) from the same column, and a black (`CRGBColor::kBlack`) marker
  rectangle 11 x px at row m = 73 - 2d / 72 - 3d / 70 - 6d (= topCorr +
  px*(range - 1 - d)).
* value < low: rows 0..69 of the column, then the "below" arrow (rows 82..86)
  at y 70.
* value > high: the "above" arrow (rows 75..79) at y 0, then rows 5..79 at y 5.
* With a morph assignment, bitmaps 0xd01/0xd02 (0xd03 for morph group 3)
  draw the start/end range; not ported here.

## 27 `CPnlNoteSeqClrRnd` (no panel)

Ctor 0x9440e, 21x30, `CTextPushButton`s "Clr" (+0x74) and "Rnd" (+0xf0),
`GetRequestedChange` 0x91ae0. No v1.62 PANL uses id 27: the SeqNote panel's
"Clr"/"Rnd" buttons are the module parameters 36 "Clear" and 35 "Random"
(push buttons, handled by the synth).

* Clr: `-(flag16f == 0) & 0x40` with +0x16f always 0: every step 64 (E4).
* Rnd: meant to draw each step in the slider's visible window:
  `lo = slider.GetLowLimit(); n = slider.GetRange(); if lo < 0 { n += lo; lo = 0 }`,
  `value = lo + (n * rand()) / 0x7fffffff`. Two bugs make it useless:
  the slider is looked up with the dependency's *value*
  (`CustomGetControl(GetDependencyValue(i))`) instead of its parameter number,
  and `n * rand()` is computed in 32 bits (RAND_MAX = 2^31 - 1), so the quotient
  is almost always 0 (emulated: always `lo`). The port implements the intent
  (`noteSeqRandom`: uniform over [max(low,0), high]) and keeps the original
  arithmetic as `noteSeqRandomOriginal`.

`CPnlSeqClrRnd` (id 2, SeqA panel, 0x91784) has the same overflow
(`(rand() << 7) / 0x7fffffff`), and its Clr writes 0 instead of 64 when
+0x16f (set from a dependency) is non-zero.

## 46 `CPnlDrumPresetSelector` (DrumSynth, deps 0..14)

Ctor 0x94fa2, 64x21: a `CTextView` 50x14 at y + 3 and a `CUpDownButton`
(11x21, `Click` 0x770be: y < top + 10 -> 1 = up, else 2 = down) at x + 53.
State: index +0x136 (0), matched +0x137 (0), counter +0x138, skip +0x139.

* `PresetComparison` (0x90f7c): the first of 30 rows of `fValues` (0x1de4a0,
  30 x 15 bytes) equal to parameters 0..14 (On/Off, parameter 15, is not
  compared) sets index and matched; no match clears matched and keeps index.
  Called from `UpdateGraphics` (0x91004) when a dependency changed, except for
  the 14 updates after a click (+0x139 = 14) while the 15 changes arrive.
* `SetupState` (0x9113c): the name `fNames[index]` (0x1de680, char[8]) if
  matched, else "none".
* `HandleChangeRequests` (0x91054): up: if index < 29 { if matched: index++;
  apply index }. Down: if index > 0 { index--; apply }. Apply =
  `GetRequestedChange` (0x9120c) writing `fValues[index][i]` to parameter i,
  0..14; matched = 1. So up without a match re-applies the last matched
  preset (initially Kick 1); down without a match applies the one before it.

| # | name | MstTune SlvTune MstDcy SlvDcy MstLvl SlvLvl FltFreq FltRes FltSwp FltDcy FltType BendAmt BendDcy Click Noise |
|---:|---|---|
| 0 | Kick 1 | 42 15 46 50 120 102 57 32 39 49 1 68 61 79 115 (the module defaults) |
| 1 | Kick 2 | 43 26 55 53 105 94 63 18 78 36 1 76 44 25 123 |
| 2 | Kick 3 | 31 71 45 35 127 0 90 24 71 27 0 84 42 81 112 |
| 3 | Kick 4 | 32 61 58 42 113 110 90 0 0 27 1 37 55 90 127 |
| 4 | Kick 5 | 36 39 50 52 104 92 111 0 40 32 0 34 68 79 69 |
| 5 | Snare 1 | 79 55 35 43 127 42 102 46 0 37 2 2 0 127 127 |
| 6 | Snare 2 | 68 3 48 42 84 127 55 66 63 40 2 63 37 126 122 |
| 7 | Snare 3 | 64 23 36 44 120 84 26 0 19 44 2 24 44 127 98 |
| 8 | Snare 4 | 68 57 47 33 105 0 91 28 42 42 0 81 44 112 127 |
| 9 | Snare 5 | 85 107 32 23 127 94 0 0 63 45 2 39 55 102 98 |
| 10 | Tom1 1 | 80 38 56 47 102 58 98 26 27 50 0 33 69 105 97 |
| 11 | Tom1 2 | 69 38 57 52 102 58 96 26 27 51 0 33 69 105 89 |
| 12 | Tom1 3 | 56 38 58 52 102 58 93 26 27 53 0 33 69 105 89 |
| 13 | Tom2 1 | 86 2 55 44 99 0 67 0 56 56 0 86 65 81 117 |
| 14 | Tom2 2 | 69 2 56 45 99 0 67 0 56 56 0 86 65 81 117 |
| 15 | Tom2 3 | 55 2 58 49 99 0 67 0 56 57 0 86 65 81 117 |
| 16 | Tom3 1 | 70 28 45 51 113 81 102 4 12 47 0 44 46 96 97 |
| 17 | Tom3 2 | 58 28 46 53 113 81 102 4 12 47 0 44 57 93 97 |
| 18 | Tom3 3 | 48 28 52 54 108 81 102 4 12 47 0 44 66 96 103 |
| 19 | Cymb 1 | 127 93 0 0 0 102 91 0 26 46 2 127 0 0 127 |
| 20 | Cymb 2 | 127 93 0 0 0 102 91 50 16 55 2 127 0 0 127 |
| 21 | Cymb 3 | 127 127 19 0 28 58 112 60 0 50 2 0 0 81 107 |
| 22 | Cymb 4 | 127 127 0 0 0 127 102 107 0 53 2 127 0 0 127 |
| 23 | Cymb 5 | 28 111 42 64 73 28 83 57 37 71 0 83 50 109 123 |
| 24 | Perc 1 | 77 32 31 45 117 93 32 32 40 45 2 110 126 116 30 |
| 25 | Perc 2 | 127 99 52 48 86 71 110 120 0 45 2 47 16 49 114 |
| 26 | Perc 3 | 96 60 36 42 127 72 32 32 40 45 2 46 24 89 46 |
| 27 | Perc 4 | 87 60 36 42 127 72 32 32 40 45 2 46 24 89 46 |
| 28 | Perc 5 | 110 93 58 66 92 79 92 127 0 58 0 0 0 127 89 |
| 29 | Perc 6 | 30 45 62 46 127 127 55 127 28 41 2 81 55 80 97 |

FltType: 0 LP, 1 BP, 2 HP (parameter text). No two rows are equal.

## Open points

* The SeqNote "Clr"/"Rnd" parameters (35, 36) are acted on by the synth; how
  the new step values come back to the editor was not traced.
* `CPatch+0x68` (variation 0x7f for custom changes) was not identified.
* Morph-range drawing of the step sliders (bitmaps 0xd01..0xd03) is not
  described in detail.
