# Parameter randomizer and Patch Mutator (editor v1.62)

Addresses are in `G2Editor_i386` (Mac). The editor has **no stand-alone
"randomize patch/module" command**: every randomisation goes through the
Patch Mutator (`CMutaSynthData`, one per patch, `CPatch+0x158`), whose dialog
is `CDialogMuta` / `CDialogMutaBackground` / `CDialogMutaBox` ("Patch Mutator"
window). Rewrite: `core/include/g2/mutate.hpp`, `core/src/mutate.cpp`; golden
data from the original code: `tools/mutate/emulate.py` → `tests/golden/mutate.txt`;
test `tests/test_mutate.cpp`.

## 1. Data model

* An **individual** is one parameter set: three `CModuleParamData<double>`
  (patch settings = context 2, VA = 1, FX = 0; std::map module id → values)
  plus a `CMorphMapData_11` (the morph assignments). `CPatchParams` (0x124
  bytes) holds such sets for several slots ("param settings").
* Values are **doubles in [0, max + 0.999]**; the patch gets `floor(v)`
  (`CPatch::HandleMutaParamDumpMolecule` @000dd6c2). Fractions survive between
  generations, so small mutations accumulate.
* `CMutaSynthData` (ctor @00143378):
  * `+0x004` population, 8 slots: 0..5 children, 6 mother, 7 father (`kMother`/`kFather`);
  * `+0x128` gene bank ("Temporary Storage"), 24 slots = 3 rows of 8;
  * `+0x24c` mirror of the patch's 10 variations (0..7 user, 8 init, 9 = the
    Mutator's audition variation). Filled with `CModuleParamData<double>::Load(..., true)`
    @001d1bce = **value + random()/2147483647** (dither); a later edit stores
    value + 0.5 (`CMutaSynthData::SetParamValue` @00143e58). `AddModule` @00141c08
    gives a new module default + dither in every slot; `RemoveModule` @00143974
    removes it from all.
  * `+0x370` range (default 0.189), `+0x378` probability (0.3721), `+0x384`
    crossover (0.15), `+0x380` link (1), `+0x38c` parent count (0/1/2, box
    colours), `+0x390` group-unlock mask (0xFE), `+0x394` solo mask (0),
    `+0x39c/0x3a0` focused box (kind, index; -1 none), `+0x3a4` focused variation.
* Box spec `CMutaBoxSpec` = (kind 0 population / 1 gene bank / 2 variation, index).

## 2. RNG

libc `random()`, divided by 2147483647.0 (so in [0, 1]). The editor **never
calls `srandom`** (only some panels call `srand` for `rand()`), so every
session starts from the default state = `srandom(1)`: 1804289383, 846930886, ...
`g2::mutate::Random` reimplements BSD random() (TYPE_3 additive feedback,
Park-Miller seeding, 310 discards); tested against macOS libc for several seeds.

## 3. Which parameters change: `CMutaSynthData::IsEnabled` @00141796

Used by Randomize and Mutate only (not by Cross/Interpolate).

1. Locked module (`CModule::IsLocked`, `+0x5a`; the file's lock flag,
   `ModuleDef::defaultLocked` for 41 types; toggled from the module context
   menu → `CPatch::InternalLockSelection` @000d7fca → `CMMutaLock`) → no.
2. **Param class** (param spec byte 0x12, `paramClass` in `data/modules.json`)
   in this list → no: 0x00 08 09 0a 14 15 18 19 1e 1f 20 24 2c 31 32 34 37 3b
   3d 3e 40 45 46 48 4a 4b 55 5b 5d 5e 5f 60 63 67 6a 6f 70 72 74 75 7b 7c 7d
   7e 80 81 84 85 90 91 9f (on/off, KBT, tune mode, LFO/env modes and ranges,
   output type, seq length/controls, MIDI channel/CC, pads, ...). 320 of 934
   parameters. The filter is by class, **not** by `morphable` (46 non-morphable
   params, e.g. Vocoder bands, stay enabled; patch-settings modules are class 0).
3. Groups; `mask = solo ? solo : unlocked`, enabled iff the group bit is set:
   * FX module types 12, 63, 89, 94, 98, 118, 150, 167, 192 → **Effects** 0x80;
   * Delay types 172–179, 181, 182 → **Delays** 0x40;
   * otherwise by class: 02 04 4f 50 56 57 **OscFreq** 0x01; 03 58 **OscFine**
     0x02; 3a 3c 3f 64 **Mixer** 0x04 (no button); 0b 0c 0d 0e 10 47 49 52 9d 9e
     **Envelope** 0x08; 30 33 **SeqValue** 0x10; 2f **SeqEvent** 0x20;
   * any other class: enabled iff no solo is active.

Default mask 0xFE: oscillator coarse tuning is quick-locked.

## 4. Distributions: `ApplyCurve` @001d199c

Curve by class (same switch in both context functions): **Centre** (4) 02 03
0c 10 25 2d 3f 58 79 8a 9b 9c; **Low** (2) 05 0b 0e 27 36 49 51 52 65 76 99;
**High** (3) 0f; **Linear** (1) everything else. N = max, S = N + 0.99, u = random.

* `ApplySingleCurve(p, x, N)` @001d11ba: d = x/S, d^(p+1) if p ≥ 0 else
  d^(−1/(p−1)), × S; odd for x < 0.
* `ApplyDualCurve(pl, ph, x, N)` @001d1278: t = 2(x − S/2)/S; t ≥ 0: 1 + t^... (ph),
  t < 0: 1 − (−t)^... (pl); × S/2.
* Randomize (x = range = −1): y = u·(N + 0.999); Linear: y; Low: Single(1.5, y)
  (∝ d^2.5, low values); High: Single(−1.5, y) (d^0.4); Centre: Dual(2, 2, y) (t³).
* Mutate (x ≥ 0, range r): Linear (`ApplyCurve1` @001d15ee): x + 2r(N + 0.999)(u − ½);
  Low/High/Centre (`ApplyCurve2/3/4` @001d189e/@001d17a0/@001d1690): warp
  x ± N·r with the inverse curve (Single(∓1.5) / Dual(−2, −2)), pick uniformly
  between, map back.

## 5. Operators (one child)

* **`CopyRandomizeContext`** @001425e8 (VA then FX; settings copied by
  `CopyContext` @00142bee, morphs by `CopyMorphs` @00141456): for each module
  (ascending id), each enabled param: one draw. Special case: class 02
  (coarse pitch) when the module's class-0x90 param (Tune Mode) is 3
  (Partial): Dual(5, 2, m + u·(N − m + 0.999)), m = Dual(−5, −2, 49, 127)
  (static). Result clamped to [0, N + 0.999].
* **`CopyMutateContext`** @00142984 (VA then FX): for each enabled param, one
  draw `random() < prob·2147483647`, then the curve; result < 0 → −v,
  > N + 0.999 → reflected.
* **`RecombineContext`** @00142288 (settings, VA, FX): per context
  `pick = (uint)(random()·1.9999/2147483647)` (mother 0 / father 1); for every
  param (no lock or class filter) copy value and the morph assignments (8
  groups) from the picked parent, then `random()/2147483647 < crossover` →
  switch parent. Multi-point crossover, carried across modules.
* **`InterpolateContext`** @00141f84: v = m + (f − m)·t; morph range
  `(char)(int)(m + (f − m)·t)`, kept if non-zero (no RNG).
* `CopyMorphs` stops when source and copy both exceed 25 assignments.

## 6. Mutator commands (6 children each, focus → child 0, audition in variation 9)

| Command | Entry | Source | Notes |
|---|---|---|---|
| Randomize (button, key) | `GetRandomizedChildrenMolecules` @00143ae8, `CPatchView::KeyMutaRandomize` @000f8162 | focused variation (mirror) | parent count 1 |
| Mutate (button) | `CDialogMutaBackground::Mutate` @0013b7ba | mother (needs one) | |
| double-click a box / Mutate key / MutateCurrent key | `GetMutatedChildrenMolecules` @00145100, `KeyMutaMutate` @000f8104, `KeyMutaMutateCurrent` @000f7dd4 | that box / mother / focused variation | source copied to mother first |
| Cross (button), alt-drag a→b, key | `GetRecombinedChildrenMolecules` @00144c3a, `KeyMutaCross` @000f8218 | mother×father / a×b | a→mother, b→father; count 2 |
| Interpolate (button), shift-drag a→b, key | `GetInterpolatedChildrenMolecules` @00144748 | same | t = (i+1)/7 |
| drag a→b | `EndDrag` @0013f11a | | copy (`GetCopyIndividMolecules` @00144528); gene bank→gene bank moves (@00144412); to a variation box writes the patch |
| click a box | `Click` @001406cc | | focus (audition) or, on a variation box, select the variation |
| Delete (gene bank context menu) | `CDialogMutaBox::DoContextMenu` @0013b644, `GetClearIndividMolecules` @00143190 | | only gene bank or population slots 6, 7 |
| "v" Copy to Variations ×3 rows | `HandleChangeRequests` @0013f7b2 | gene bank row r | slot r·8+i → variation i |
| "x" Clear Row ×3 | same | | |
| Store child key | `KeyMutaStoreChild` @000f89e0 | focused child | first free gene bank slot |
| Assign mother/father keys | @000f9b2c / @000f8a8c | focused variation | |

Knobs: Probability 0–100 %, Range 0–50 %, Crossover 0–100 %; **Link** couples
probability and range (`SetMutationRangeFromProb` @00141050:
r = ((√p − 0.22)/0.78·(−0.53) + 0.7)², ≤ 0.5; `SetMutationProbFromRange`
@00140fe4: p = ((√r − 0.7)/0.53·(−0.78) + 0.22)², ≤ 1). Quick locks
OscFreq, OscFine, Envelope, SeqValue, SeqEvent, Delays, Effects (ticked = bit
cleared; ignored while a Solo is on) and a Solo button per row (bits of +0x394).
All commands are undoable (molecules) and refused while the patch is locked.
The dialog is only usable when the patch is not "dirty" (`IsMutaSynthDataAvailable`).

## 7. Rewrite and deliberate differences

* `g2::mutate` is faithful operator by operator: `tests/golden/mutate.txt`
  holds `IsEnabled` for every type/param/mask, `ApplyCurve` samples, the link
  curves, and complete randomize/mutate/cross runs (values, morphs, number of
  random() calls) produced by running the original functions in unicorn.
* No variation 9: the Mutator class only tracks the focused box; the UI decides
  where to audition it. Variation boxes 0..8.
* The mirror is dithered per variation from the Mutator's generator (the
  original dithers context by context for 10 variations), so whole-session
  sequences differ from the original even with seed 1; per-operator draws match.
* `apply` clamps floor(v) to the parameter's range (the original would wrap a
  byte; unreachable with range ≤ 0.5) and keeps one morph group per parameter.
* A module missing from the patch or the other parent is skipped / taken from
  the mother (the original would dereference null).
* `mutate::randomize/mutate(Patch&, Scope)` (in-place, per variation, optional
  module list) are additions, built from the same operators.
* Unused in the editor: `GetDefaultMutationLock` @0004f008 and
  `IsModuleLockedByDefault` @000a9ec4 (same 41-type list as the default lock),
  `GetRandomConvertFunction` @0004e29c (LFO rate converters).
