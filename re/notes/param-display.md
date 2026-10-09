# Parameter display text (`ParamText`)

How the original editor (Mac v1.62, `G2Editor_i386`) turns parameter values
into display strings such as `415.3Hz`, `1.02s`, `-17.6` or `C4`, and how that
maps onto the panel ids `InfoFunc` / `Text Func`.

- Port: `core/include/g2/param_text.hpp`, `core/src/param_text.cpp`
  (`g2::paramtext::format(id, values)` plus one function per original function,
  same names).
- Data: `data/param_text.json` (id tables, per-function arguments/formulas,
  and the per-module bindings described below).
- Tests: `tests/test_param_text.cpp`, `tests/test_param_text_golden.cpp`
  (digests in `tests/golden/param_text_digests.json`).
- Emulator that produced the expected values: `tools/paramtext/emulate.py`.

Addresses are virtual addresses in `original/mac/G2Editor_i386` (same as the
Ghidra program). The Ghidra export also contains a second copy of many
functions at 0x24xxxx/0x5ccxxx: those are the `.eh` (unwind) symbols decoded as
code and are garbage; ignore them.

## 1. The three function tables

All display functions live in namespace `ParamText` (0x29ec6..0x2fe40) and
write into a caller buffer (`char[11]`..`char[23]`). There are three lookup
functions, each a `switch` on an 8-bit id:

| getter | address | signature of the returned function | default |
|---|---|---|---|
| `GetSingelDependencyTextFunction(int)` | 0x2c9a4 | `void f(uchar v, char*)` | `Default` (= `From0to100`) |
| `GetDualDependencyTextFunction(int)` | 0x2d178 | `void f(uchar v, uchar dep1, char*)` | `DualDefault` (`"%d"` of v) |
| `GetTripleDependencyTextFunction(int)` | 0x2d2c0 | `void f(uchar v, uchar dep1, uchar dep2, char*)` | `TripleDefault` (`"%d"` of v) |

The three tables share one id space (the dual/triple ids are holes in the single
table, e.g. 60 = triple `OscFreqDep`, 103 = dual `LFOFreq`), but **the table is
chosen by how many values the display depends on, not by the id**: id 0 is
`Default` in the single table and `OscSubFreq` in the dual table. Ids that are
not in the chosen table fall back to that table's default. 227 ids are
assigned (203 single, 18 dual, 6 triple); the full list is in section 6.

The ids are the same numbers as PANL `InfoFunc` and TextField `Text Func`, and
as the byte at +0x11 of the module definition records (below).

## 2. Who calls them

### 2.1 Parameter text of a module (tooltips, knob floaters, "param text")

`CBuildModule::GetParamText(paramIndex, value, CModule*, string&)` (0xab1f0)
looks `paramIndex` up in a `std::map<uchar, CParamTextRegisterABC*>` at
`CBuildModule+0x44`. Not found: `ParamText::Default(value)`. Found: the
register object's virtual `GetText`:

| class | vtable | ConnectText overload | GetText | call |
|---|---|---|---|---|
| `CParamTextRegisterSingel` | 0x2a6f80 | 0xaa86a | 0xab1c0 | `f(value)` |
| `CParamTextRegisterDual` | 0x2a6f90 | 0xaa8ce | 0xabc6e | `f(value, dep(d1))` |
| `CParamTextRegisterTripple` | 0x2a6fa0 | 0xaa3d8 | 0xabd14 | `f(value, dep(d1), dep(d2))` |

`dep(d)`: if bit 7 of `d` is set, the module's mode value `d & 0x7f` (the
`std::vector<uchar>` at `CModule+0x24`, i.e. the "static"/mode parameters);
otherwise `CModule::GetParamValue(d, focusedVariation)`.

The map is filled in two passes:

1. **Module definition** — `CBuildModule::CBuildModule(SModuleInfo const&)`
   (0xaa93a). `SModuleInfo` (one `_k<Name>ModuleInfo` symbol per module): byte
   +0x00 = module type id (the id used in patches), +0x04 = pointer to the
   parameter records, +0x18 = parameter count. Each record is 0x1c bytes:

   | offset | content |
   |---|---|
   | +0x00 | name, 11 bytes (`"Coarse"`) |
   | +0x0b | max value |
   | +0x0c | default value |
   | +0x11 | text function id, 0xff = none |
   | +0x12 | (unknown, not used for text) |
   | +0x13 | dependency 1, 0xff = none |
   | +0x14 | dependency 2, 0xff = none |
   | +0x18 | pointer (unknown) |

   id != 0xff registers: dep1 == 0xff → single, else dep2 == 0xff → dual, else
   triple. 130 of the 903 parameters are registered this way, e.g. OscA
   `Coarse` = triple 60 `OscFreqDep(coarse, param1 = Fine, param6 = Tune Md)`,
   LfoA `Rate` = dual 103 `LFOFreq(rate, param7 = Range)`, FxReverb `Time` =
   dual 107 `ReverbTime(time, mode0)`.
2. **Panel** — `CMBGeneric::ConnectTextToParams` (0x84d70): for every PANL
   element that is a control (`CPanelControlObjectData::IsControl`, 0x89578:
   `Knob ButtonRadio ButtonIncDec ButtonText LevelShift EnvCurve ButtonFlat
   TextEdit ButtonRadioEdit ButtonPush`; not `Led`, `MiniVU`, `PartSelector`,
   jacks), if its `CodeRef` is not registered yet, register
   `GetSingelDependencyTextFunction(InfoFunc)`. So the module-definition id wins
   and the panel `InfoFunc` is always a single-value fallback.

The link between PANL resources and module types (open question 1 of
`rsrc-formats.md`) is in `CModuleFactory::CModuleFactory` (0xadce0):
`new CMBGeneric(panelResId, bitmap, &_k<Name>ModuleInfo)` for 165 modules.
`data/param_text.json` → `modules[]` lists, per module type, `panel_res_id`, and
for every parameter the effective `func_id`, arity, function and dependencies
(`source: "module"` or `"panel"`).

The patch-settings pseudo modules (`CBModuleToolbar::CBModuleToolbar`, 0x8e4f2)
register hard-coded single ids: Morph group sources 221..228 (params 8..15 of
type 6), master volume 118 / mute 7 (type 0x5f), glide 116 / 115 (type 0x87),
bend 117 / 162 (type 0x89), vibrato 120 / 119 / 171 (type 0x8a), arpeggiator
3 / 112 / 113 / 111 (type 0x88), octave shift 121 / 3 (type 0x99).

### 2.2 TextField displays on the panel

`CPanelCustomObjectData::ReadCustomData` (0x88fca) reads `Dependencies`:

- shorter than 3 characters (empty): the field shows the parameter text of
  `MasterRef` (section 2.1), `Text Func` is ignored.
- otherwise the string is split at `,` into dependency items; `S<n>`/`s<n>` is
  mode value n (`CDependencyItem` type 1), a number is parameter n (type 0).
  `Text Func` is the id.

`CCustomObjectFactory::CreateTextField` (0x875e0) then picks the table by the
number of items: 1 → `CSingelDependencyText` (`GetText` 0xcb2f0), 2 →
`CDualDependencyText` (0xcb336), 3 → `CTripleDependencyText` (0xca8fa), other
→ `Infos::CSmallDefaultText`. `GetText` calls `f(item0, item1, item2)` in list
order (`CPnlCustom::GetDependencyValue`, 0xc91d6). Examples:
OscA `"0,1,6"` + 60 → `OscFreqDep(coarse, fine, tuneMode)`; Reverb `"0,S0"` +
107 → `ReverbTime(time, roomType)`; DelayQuad `"0,8,S0"` + 140 →
`DelayTimeTap(time, timeMode, range)`.

So for `g2::paramtext::format(id, values)`, `values` = the dependency values in
order, and `values.size()` selects the table, exactly like the original.

## 3. Formatting conventions

- Everything goes through `sprintf` (no locale-specific code). Common formats:
  `"%d"`, `"%.1f"`, `"%.*f%s"` (precision chosen per decade), `"%+3d %+4.0f"`
  (semitones + cents), `"%3ddB"`. Rounding is printf's (exact binary value,
  round-half-even), so the port uses `snprintf` with the same formats.
- Arithmetic is SSE (scalar `float`/`double`, no x87, no FMA). Many functions
  mix single and double precision (e.g. `FilterCutOff`: `logf(2.0f)` in float,
  then `exp` in double, result rounded to float before formatting). The port
  reproduces each step's precision; `logf/expf` are modelled as the double
  function rounded to float.
- Units are glued to the number without a space (`415.3Hz`, `1.512s`, `-6.2`
  for dB in most places), with exceptions taken verbatim (`"3162 Hz"`,
  `"-7.7 dB"`, `" 0 dB"`, `"%3d cnt"`).
- Shared helpers (global functions, not in `ParamText`):

  | helper | address | behaviour |
  |---|---|---|
  | `Table(char const**, n, v, buf)` | 0x299a8 | `v < n ? t[v] : ""` |
  | `Table2`, `Table3` | 0x299d4, 0x29a06 | same for 2/3 literal strings |
  | `IntToStr(long, buf, unit)` | 0x29a46 | `"%d"` + unit |
  | `FloatToStrSci(float, buf, unit)` | 0x29a7a | `<0.1`: `"%.1fm"` of x1000 (*); `<1`: `"%.0fm"`; `<10` `"%.3f"`; `<100` `"%.2f"`; `<1000` `"%.1f"`; `<10000` `"%.2fk"`; else `"%.1fk"`, then unit |
  | `FloatToStrSci6` | 0x29bea | one digit more: `"%.3fm" "%.2fm" "%.1fm" "%.4f" "%.3f" "%.2f" "%.3fk"`, `"%.2fk"` of (x+5)/1000 |
  | `FloatToStrTime(float s, buf, unit)` | 0x29d56 | ms = s*1000: `<10` `"%.2fm"`, `<100` `"%.1fm"`, `<999.5` `"%.0fm"`, `<10000` `"%.2fs"`, else `"%.1fs"` of (ms+50)/1000; unit argument ignored |
  | `FloatToStrTimeShort` | 0x29e24 | `<10` `"%.1fm"`, `<999.5` `"%.0fm"`, else `"%.1fs"` |
  | `DelayTimeTapHelp(v, sync, range, eighth, buf)` | 0x2af2e | sync 0: t = (v*range+1)[/8]/96000 s → `"%.2fm" "%.1fm" "%.0fm" "%.3fs"`; sync 1: clock ratio `gSync[v>>2]`; else nothing |
  | `FloatToString(buf, fixed16.16, digits)` | 0x2d322 | only used with 2 digits: `"%d.%02u"` with the fraction rounded |

  (*) quirk: `FloatToStrSci` first prints `"%.2fm"` below 0.01 but then falls
  through and overwrites it with the `< 0.1` branch.
- "m" alone means milliseconds (`" 748m"`, `"25.1m"`); `"s"` seconds.
- Table-driven functions leave the buffer untouched for out-of-range values
  (the original then shows whatever was in the buffer); the port returns "".
- `±` is Mac Roman 0xB1 in the original; the port emits UTF-8.

### Static data used (copied into the port)

| symbol | address | content |
|---|---|---|
| `Semitone::kNotes` | 0x1dc0c0 | `A Bb B C C# D D# E F F# G G#` (3 bytes each) |
| `EnvelopeTime::gTimeStr` | 0x292320 | 128 x 6-byte strings |
| `NoiseGateAtkTime::gTimeStr` | 0x2926a0 | 128 x 6 |
| `CompressorAttack::atkstr`, `CompressorRelease::rlsstr` | 0x2929a0, 0x292ca0 | 128 x 6 |
| `GlideTimeRate::glidetimestr` | 0x292fa0 | 13 x 6 (dead: overwritten, see quirks) |
| `DelayTimeTapHelp::gSync`, `LFOFreq::gSyncRatios` | 0x292620, 0x293040 | 32 note-value strings each |
| `kRange` of DelayTimeStereo/Tap8/Fx/Tap | 0x1dc0e4/0x1dc0f0/0x1dc10c/0x1dc11c | {378,756,1021}, {4,19,76,378,756,1512,2041}, {378,756,1512,2041}, {4,19,76,378,756,1512,2041} |
| `_envDcyMult`, `_envLinAdd`, `_gVibratoRateDisp`, `_noiseGateRelMult`, `_envFollowRelease`, `_envFollowAttack`, `_ampGainNew` | 0x291100..0x292100 (0x200 apart) | 128 x int32 each (DSP coefficient tables) |
| `_gTimes.145699` (PortamentoTime) | 0x1dc4e0 | 128 x uint16 |
| `PatchMIDIOutChannel::C.202`, `PatchMIDIInChannel::C.203` | 0x29f620, 0x29f680 | `1..16 This SlotA..SlotD` / `1..16 This Keyb` |

### Quirks kept by the port

- `GlideTimeRate` (shape 1, v > 114) copies `glidetimestr` and then overwrites
  it with the computed value; `LevMult` mode 0 special-cases "-127"/"127" and
  then overwrites them with `2v-128`.
- `dB` and `NoiseGateLevel` drop the decimal only for `>= 10`, which a dB value
  never reaches, so they always show one decimal.
- `NoteScaler(0)` is `"0-Oct"`; `VolumeDb(127)` is `"-0.0 dB"`; `dB(127)` is `"-0"`.
- `Phase` uses `"%d%c"` with a NUL char, i.e. just the number.

## 4. Verification

Expected values were not computed by hand but by running the original
functions in a CPU emulator: `tools/paramtext/emulate.py` maps the Mach-O
sections of `G2Editor_i386` into unicorn (x86-32), hooks the imports used by
`ParamText` (`sprintf strcpy strcat memcpy floor exp log log10 pow logf expf`,
found through the binary's indirect symbol table) and implements them in
Python (C-equivalent `printf` formatting; `logf/expf` = double result rounded
to float). The dispatch tables are read by calling the three getters for ids
0..255; functions are found by their symbol names. The script reads the
user's own binary at run time and contains no bytes or code from it.

Every function in the tables was evaluated on v = 0..127 (dual: 128 x 128 or
the valid range of the dependency; triple: OscFreqDep / DXOscFreq 128 x 128 x
mode, delays and ClkGenTempo over their modes), 397,952 strings for the
230 (table, id) entries. The C++ port reproduces all of them (arm64 and
x86_64, -O0..-O3).

The tests keep two forms of this:

- `tests/test_param_text.cpp`: ~1100 individual strings as spot checks.
- `tests/golden/param_text_digests.json` + `tests/test_param_text_golden.cpp`:
  one SHA-256 per (table, id) over all output strings of its domain (each
  string UTF-8 encoded plus `"\n"`, tuples in nested-loop order, first
  argument outermost; the domain is stored with each entry). Id 255 stands for
  each table's default. The test recomputes the digests from the port with a
  small built-in SHA-256.

### Reproducing the digests

Needs Python 3 and unicorn, and the editor binary at
`original/mac/G2Editor_i386` (not distributed with the project):

```sh
python3 -m venv /tmp/g2venv
/tmp/g2venv/bin/pip install unicorn
# -I: isolated mode, nothing is imported from the current directory
/tmp/g2venv/bin/python -I tools/paramtext/emulate.py original/mac/G2Editor_i386 \
    digests tests/golden/param_text_digests.json          # ~20 s
/tmp/g2venv/bin/python -I tools/paramtext/emulate.py original/mac/G2Editor_i386 tables
/tmp/g2venv/bin/python -I tools/paramtext/emulate.py original/mac/G2Editor_i386 call 60 69 64 1
# -> 440.00Hz  (triple id 60 OscFreqDep, coarse 69, fine 64, mode Freq)
/tmp/g2venv/bin/python -I tools/paramtext/emulate.py original/mac/G2Editor_i386 \
    dump /tmp/param_text.tsv   # every string of the domain, for diffing
```

The regenerated file must be byte-identical to the committed one.

Caveat: libm results (`exp`, `log10`, `pow`) are the host's, not Apple's 2007
libm; a different last-ulp result could flip a printed digit at an exact
rounding boundary. One such case was found for `expf` (macOS arm64 `expf` is
not correctly rounded at `OscSubFreq(107, 40)`), hence the double-then-round
model for `logf/expf`, in the emulator and the port alike, which is
deterministic across platforms.

## 5. Comparison with Verhue's editor

`third_party/.../BVE.NMG2Function.pas` (`TG2Function.InfoFunction`) switches on
the same ids (e.g. 21 `FilterCutOff`, 110/45 ClkGen tempo, 137/16 env levels),
which confirms the numbering. His strings are approximations, though:
enumerations mostly agree (On/Off, Poly/Mono, LevelShift, ...), numeric ones
differ in precision and units (e.g. id 0 at 127: `100.0` vs original `100`;
id 21 uses 5 significant digits and `kHz`; id 35 writes `+-` for `±`;
ClkGen shows `MASTER` and `" BPM"`). The port follows the original binary.

## 6. Id table

`args`: argument order as passed (own value first). "Sci(x unit)" =
`FloatToStrSci`, "Time(...)" = `FloatToStrTime`, "TimeShort" =
`FloatToStrTimeShort`. `k[v]` = the coefficient table of that function.

### Single table (`GetSingelDependencyTextFunction`, default `Default`)

| id | function | addr | args | formula / output |
|---:|---|---|---|---|
| 1 | Negative | 0x2a30a | v | "%d" of -v |
| 2 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 3 | OnOff | 0x2a04a | v | Off/On |
| 4 | Mono | 0x2a086 | v | Poly/Mono |
| 5 | Flip | 0x2a0a4 | v | Normal/Inverted |
| 6 | GainCompensation | 0x2a0c2 | v | GC Off/GC On |
| 7 | Mute | 0x2a0fe | v | Muted/Active |
| 8 | Bypass | 0x2a11c | v | Bypass/Active |
| 9 | Loop | 0x2a176 | v | 1-Cycle/Loop |
| 10 | Rec | 0x2a194 | v | Off/Rec On |
| 11 | OffNum | 0x2a1f4 | v | 0 -> "Off", else "%d" |
| 12 | Phase | 0x2a1b2 | v | "%d" of floor(v*360/128 - 180) (16.16 fixed point) |
| 13 | Semitone | 0x2a226 | v | MIDI note name, kNotes[(v+3)%12] + (v/12-1): 60 -> "C4", 69 -> "A4" |
| 14 | SemitoneKey | 0x2a286 | v | Semitone(v), plus " Key" when v == 64 |
| 15 | SemitoneFilter | 0x2a2ce | v | SemitoneKey(v+4) |
| 16 | UniPol | 0x2a330 | v | v/2 with one decimal ("%d.0"/"%d.5"), 127 -> "64.0" |
| 17 | BiPol | 0x2a3ce | v | "%d" of v-64, 127 -> "64" |
| 18 | Polarity | 0x2aadc | v | Bipol/Unipol |
| 19 | Sym | 0x2a43c | v | v-64 with explicit "+" above 64 |
| 20 | DelayTime | 0x2a47e | v | Sci(v*2.5/127 s) |
| 21 | FilterCutOff | 0x2e4d0 | v | Sci(440*2^((v-65)/12) Hz) |
| 22 | DrumMstFreq | 0x2e5e0 | v | note n = (v>>1)+16, half a semitone lower when v is even: "%.1fHz" (or "%.0fHz" from 100) of 440*2^((n-69)/12) |
| 23 | DrumSlvMult | 0x2e6cc | v | ratio 2^((v>>2 + (v&3)/4)/12): "N:1" when it is the nearest integer ratio, else "x%.2f"/"x%.1f" |
| 24 | OnePoleFreq | 0x2eb2e | v | Sci(12*1666.67^(v/127) Hz) |
| 25 | KBT | 0x2dd4c | v | 0 "Off", 64 "Key", 127 "x2", else "x%.2f" of v/64 |
| 26 | KBTOnOff | 0x2a728 | v | KBT Off/KBT On |
| 27 | PulseWidth | 0x2a746 | v | "%d%" of (v*99>>7)+1 |
| 28 | EnvelopeTime | 0x2a7b6 | v | string table (128 entries, " 0.5m" .. "45.0s") |
| 29 | EnvAttackType | 0x2a7dc | v | Exp/Lin/Log |
| 30 | LFORange | 0x2ba7a | v | Sub/Lo/Hi/BPM/ClkSync |
| 31 | LFOWaveFull | 0x2a842 | v | Sine/Tri/Saw/SawN/Sqr |
| 32 | LfoRateMult | 0x2efcc | v | SlaveMult(v-64 semitones, precision 2): "x%.3f"-ish or "N:1"/"1:N" |
| 33 | Slide | 0x2ddda | v | "%.0fms" of 5.3*255.6^(v/127) |
| 34 | Smooth | 0x2a890 | v | Sci(1000^(v/127)/(1000*pi) s) |
| 35 | PlusMinusHalfSteps | 0x2de6c | v | 0 "0", 127 "±64", else "±%.1f" of v/2 |
| 36 | EqGain | 0x2df72 | v | "%.1fdB" of (v-64)*0.28125 (127 counts as 128) |
| 37 | AmpGain | 0x2e08e | v | "x%.2f" of 16^(v/128)/4 (127 -> 4.00) |
| 38 | EqFreq | 0x2fbf6 | v | Sci(20*800^(v/127) Hz) |
| 39 | PhaserFreq | 0x2f68a | v | Sci(100*160^(v/127) Hz) |
| 40 | SampleRate | 0x2a994 | v | Sci(1760*2^((v-69)/12) Hz) |
| 41 | OscWaveFull | 0x2aa1c | v | Sqr/Saw/Tri/Sine |
| 42 | OscWaveSine | 0x2aa62 | v | Off/Sine |
| 43 | TrigGate | 0x2aafa | v | Trig/Gate |
| 44 | GoStop | 0x2ab18 | v | Go/Stop |
| 45 | BPM | 0x2a58e | v | "%dBPM" of 24+2v (v<32), v+56 (v<97), 2v-40 |
| 46 | LevelShift | 0x2ab36 | v | Pos/PosInv/Neg/NegInv/Bip/BipInv/Bip/BipInv |
| 47 | OnOff | 0x2a04a | v | Off/On |
| 48 | EnvAttackType | 0x2a7dc | v | Exp/Lin/Log |
| 49 | EnvADDSRSusPlace | 0x2b922 | v | L1/L2 |
| 50 | EnvMultiSusPlace | 0x2b8dc | v | L1/L2/L3/Trg |
| 51 | EnvMultiType | 0x2b2e8 | v | Bipolar/Uni/Exp/Uni/Lin |
| 52 | Fade2to1 | 0x2ac18 | v | 64 "Mute"; <64 "I1:%d" of (64-v)*2 (0 -> "I1:127"); >64 "I2:%d" of 2v-128 (127 -> "I2:127") |
| 53 | Fade1to2 | 0x2ac92 | v | as Fade2to1 with O1/O2 |
| 54 | AmpGain | 0x2e08e | v | "x%.2f" of 16^(v/128)/4 (127 -> 4.00) |
| 55 | AmRm | 0x2b308 | v | 0 "None", 64 "Am", 127 "Rm", else "%d" |
| 57 | dB_0_6_12 | 0x2b358 | v | 0dB/-6dB/-12dB |
| 58 | OscAWave | 0x2ad52 | v | Sine/Tri/Saw/Sqr50/Sqr25/Sqr10 |
| 59 | OscFine | 0x2ae48 | v | "%+5.1f" cents of (v-64)*50/64 |
| 61 | BiPol | 0x2a3ce | v | "%d" of v-64, 127 -> "64" |
| 62 | FmType | 0x2ae92 | v | FM A/FM B/FM C |
| 63 | OscTuneMode | 0x2aeb2 | v | Semi/Freq/Factor/Partial/Sub |
| 64 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 66 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 67 | OutBusBDest | 0x2b378 | v | 1/2, 3/4, CVA |
| 68 | PartialGen | 0x2b56e | v | "±%d" of v>>1, "*" appended above 64 |
| 69 | NoteScaler | 0x2ded8 | v | PlusMinusHalfSteps(v) + "-Oct"/"-5th"/"-7th" when (v+120)%24 is 0/14/20 |
| 70 | NoteVelScale | 0x2e134 | v | "%.1fdB" of (v-64)/8, 127 -> "8.0dB" |
| 71 | FltKBT | 0x2b5c2 | v | KBT Off/25%/50%/75%/100 |
| 72 | GCOnOff | 0x2a0e0 | v | GC Off/GC On |
| 73 | dB_6_12 | 0x2b648 | v | 6dB/12dB |
| 74 | dB_12_24 | 0x2b666 | v | 12dB/24dB |
| 75 | FilterCType | 0x2da52 | v | LP/BP/HP/BR |
| 76 | EQParBW | 0x2e19e | v | "%.2fOct" of (128-v)/64 |
| 77 | HiLo | 0x2b6ca | v | Low/High |
| 78 | Vowels | 0x2b6e8 | v | A/E/I/O/U/Y/AA/AE/OE |
| 79 | OffNum | 0x2a1f4 | v | 0 -> "Off", else "%d" |
| 80 | Emphasis | 0x2b752 | v | Off/On |
| 81 | VocoderMon | 0x2b78e | v | Active/Monitor |
| 82 | ClipSym | 0x2b770 | v | Asym/Sym |
| 83 | DiodeModes | 0x2b7ac | v | HalfPos/HalfNeg/FullPos/FullNeg |
| 84 | ShaperModes | 0x2b7f2 | v | Inv x3/Inv x2/x2/x3 |
| 85 | PhaserFreq | 0x2f68a | v | Sci(100*160^(v/127) Hz) |
| 86 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 87 | DelayTime | 0x2a47e | v | Sci(v*2.5/127 s) |
| 88 | SampleRate | 0x2a994 | v | Sci(1760*2^((v-69)/12) Hz) |
| 89 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 90 | ReverbType | 0x2b87e | v | Room/Hall/Plate/Gate |
| 91 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 92 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 93 | FilterCdB | 0x2b2c8 | v | 12dB/18dB/24dB |
| 94 | Enum | 0x2a2e4 | v | "%d" of v+1 |
| 99 | SwitchCtrl | 0x2b8c4 | v | "%d" of 4v |
| 100 | SwitchCtrl | 0x2b8c4 | v | "%d" of 4v |
| 101 | SwitchCtrl | 0x2b8c4 | v | "%d" of 4v |
| 104 | LFORange | 0x2ba7a | v | Sub/Lo/Hi/BPM/ClkSync |
| 105 | LfoKBT | 0x2d518 | v | KBT Off/25%/50%/75%/100 |
| 106 | LfoAWave | 0x2bb0a | v | Sine/Tri/Saw/Sqr/RndStep/Rnd |
| 108 | PatchMIDIOutChannel | 0x2bdc8 | v | 1..16, This, SlotA..SlotD |
| 109 | PatchMIDIInChannel | 0x2be14 | v | 1..16, This, Keyb |
| 111 | ArpeggiatorRange | 0x2d7de | v | 1..4 Oct |
| 112 | ArpeggiatorRate | 0x2d824 | v | 1/8, 1/8T, 1/16, 1/16T |
| 113 | ArpeggiatorMode | 0x2d86a | v | Up/Down/Up/Down/Rnd |
| 114 | UniPolShort | 0x2a406 | v | "%d" of v, 63 -> "64" |
| 115 | PortamentoTime | 0x2d6fc | v | gTimes[v]/8192 s: "%.2fs" (>= 1 s) or "%.0fm" |
| 116 | PortamentoMode | 0x2d760 | v | Off/Normal/Auto |
| 117 | BendOnOff | 0x2dae2 | v | Off/On |
| 118 | VolumeDb | 0x2d9b6 | v | "-%.1f dB" (or "-%.0f dB" from 10) of 18*(5.333^((127-v)/127)-1) |
| 119 | VibratoAmount | 0x2d626 | v | "%3d cnt" |
| 120 | VibratoSource | 0x2d64a | v | Off/Aftouch/Wheel |
| 121 | OctShift | 0x2db26 | v | "%d oct" of v-2 with "+" above 0 |
| 123 | FilterCutOff2 | 0x2e558 | v | Sci(440*2^((v-60)/12) Hz) |
| 124 | FilterResonance | 0x2dcae | v | 0.5/(1-0.9v/127)^2 as "%.2f" (below 10) or "%.0f" |
| 125 | FmKBT | 0x2b610 | v | FM Lin/FM Trk |
| 126 | PulseWidthHalf | 0x2a76c | v | "%.0f%" of v*49/127+50 |
| 127 | SignalType | 0x2abd8 | v | Bipol/Pos/Neg |
| 128 | LfoShape | 0x2d964 | v | "%d%" of floor(v*98/127)+1 |
| 129 | EnvFollowerAttack | 0x2b95a | v | 0 "Fast", else Time(log(0.01)/log(1-k[v]/2^23)/96000 s) |
| 130 | EnvFollowerRelease | 0x2b9f2 | v | Time(log(0.01)/log(1-k[v]/2^23)/96000 s) |
| 131 | LFOFreq1 | 0x2f666 | v | LFOFreq(v, 1) |
| 132 | NotePlusMinus | 0x2d56c | v | "±%d.0"/"±%d.5" of v/2, 127 -> "±64.0" |
| 134 | XFade | 0x2ad0c | v | Off/25%/50%/100% |
| 135 | PosNeg | 0x2be60 | v | Pos/Neg |
| 136 | EnvAttackDecayShape | 0x2a7fc | v | LogExp/LinExp/ExpExp/LinLin |
| 138 | EnvReset | 0x2a13a | v | Normal/Reset |
| 139 | EnvADRSust | 0x2a158 | v | AD/AR |
| 144 | DelayTimeMode | 0x2b140 | v | Time/ClkSync |
| 148 | AmpType | 0x2be98 | v | Lin/dB |
| 149 | InputPad | 0x2b4e2 | v | +6 dB/ 0 dB/-6dB/-12dB |
| 150 | In2Src | 0x2b42c | v | In 1/2, In 3/4, Bus 1/2, Bus 3/4 |
| 151 | In4Src | 0x2b472 | v | In/Bus |
| 152 | InCVA | 0x2b4aa | v | FX 1/2, FX 3/4 |
| 153 | dB | 0x2e1f4 | v | 0 "-oo", 1 "-99.9", 2 "-99.0", 127 "-0", else "%.1f" of 20*log10(0.01x+0.99x^3), x=v/127 |
| 155 | OscSinShpWave | 0x2adf4 | v | Sine1..Sine4/TriSaw/Pulse |
| 156 | OscBWave | 0x2ada6 | v | Sine/Tri/Saw/Sqr/DualSaw |
| 157 | LogLin | 0x2bf10 | v | Log/Lin |
| 158 | PortamentoType | 0x2bf48 | v | C-Rate/C-Time |
| 159 | NoiseGateAtkTime | 0x2bf80 | v | string table (" 0.2m" .. " 100m") |
| 160 | NoiseGateRelTime | 0x2bfa6 | v | Time(log(0.01)/log(k[v]/2^23)/24000 s) |
| 161 | BendOnOff | 0x2dae2 | v | Off/On |
| 162 | BendRange | 0x2db00 | v | "%d semi" of v+1 |
| 163 | LFOPhase | 0x2bac8 | v | "%.0f" of v*360/128 |
| 164 | LfoBWave | 0x2bb5e | v | Sine/Tri/Saw/Sqr |
| 165 | LfoCWave | 0x2bba4 | v | Sine/CosBell/TriBell/Saw2Tri/Sqr2Tri/Sqr |
| 166 | ClkGenSync | 0x2a636 | v | 1/2/4/8/16/32 |
| 167 | OverDriveType | 0x2c022 | v | Soft/Hard/Fat/Heavy |
| 168 | OverDriveSym | 0x2c068 | v | Asym/Sym |
| 169 | ExpLin | 0x2bed0 | v | Exp/Lin/dB |
| 170 | PhaserType | 0x2c086 | v | Type I/Type II |
| 171 | VibratoRate | 0x2d6b4 | v | "%d.%02u Hz" of a 16.16 table (4.00 .. 8.00) |
| 172 | FltVariant | 0x2a4fa | v | Notch/Peak/Deep |
| 173 | OscFreqSingle | 0x2faa2 | v | OscFreq(v, 64) |
| 174 | CompressorAttack | 0x2c0a4 | v | string table ("Fast", "0.53m" .. " 767m") |
| 175 | CompressorRelease | 0x2c0ca | v | string table (" 125m" .. "10.2s") |
| 176 | CompressorThreshold | 0x2c0f0 | v | "%3ddB" of v-30, 42 -> " Off" |
| 177 | CompressorRatio | 0x2c12a | v | ratio*10 r = u+10 / 2u / 5u-75 (u = v or v-35, x10 above 34): "%d.%d:1" below 10:1, else " %2d:1" |
| 178 | CompressorLevel | 0x2c200 | v | "%3ddB" of v-30 |
| 179 | MIDIValue | 0x2c226 | v | "%d" |
| 180 | OutPad | 0x2b528 | v |  0 dB/+6 dB/+12dB/+18dB |
| 181 | MonoKeybPrio | 0x2dba4 | v | Last/Low/High |
| 182 | Out2Dest | 0x2b398 | v | Out 1/2, Out 3/4, FX 1/2, FX 3/4, Bus 1/2, Bus 3/4 |
| 183 | Out4Dest | 0x2b3ec | v | Out/FX/Bus |
| 184 | EqFreq100To8k | 0x2fc60 | v | 100*80^(v/127) Hz: "%.0fHz", "%.2fkHz", "%.1fkHz" |
| 185 | OscPhase | 0x2d922 | v | "%.0f" of v*360/128 |
| 186 | InactiveActive | 0x2a068 | v | Inact/Active |
| 187 | ClkGenSource | 0x2a60a | v | Intern/Master |
| 188 | Pad | 0x2d5ee | v | 0dB/-6dB/-12dB |
| 189 | ModAmountMode | 0x2dbc4 | v | m/1-m |
| 190 | ClkGenSwing | 0x2dbe2 | v | "%.1f%" of v/5.08+50 |
| 191 | From0to200Percent | 0x29f52 | v | like From0to100 on 0..200 (exact at multiples of 16, 127="200"), "%.1f" of v*200/128, then "%" |
| 192 | ShapeExpCurve | 0x2b838 | v | x2/x3/x4/x5 |
| 193 | PTrackAlgorithm | 0x2c23c | v | Normal/HiPitch |
| 194 | KeyQuantCaptureRange | 0x2dc20 | v | Closest/Evenly |
| 195 | DigitizerBits | 0x2fdb6 | v | "%d" of v+1, 12 -> " Off" |
| 196 | DXDetune | 0x2c32a | v | "%d" of v-7 |
| 197 | DXBreakPoint | 0x2c342 | v | Semitone(v+9) |
| 199 | DXCurve | 0x2c358 | v | -Lin/-Exp/+Exp/+Lin |
| 200 | DXTuneMode | 0x2c39e | v | Ratio/Fixed |
| 202 | PitchShiftDelay | 0x2c274 | v | 12.5m/25m/50m/100m |
| 203 | RndSmooth | 0x2bbf8 | v | 0%/25%/50%/75%/100% |
| 204 | RndLevelShift | 0x2ab98 | v | Bip/Pos/Neg |
| 205 | RndStep | 0x2bc46 | v | "%d%" of v*100/127 |
| 206 | RndDistribution | 0x2bd3c | v | "%d%" of v*100/127 |
| 207 | RndStepSwitch | 0x2bc8c | v | 25%/50%/75%/100% |
| 208 | RndLogic | 0x2bcd2 | v | 10%..90% |
| 209 | PitchShuttle | 0x2e3f4 | v | 64 "x0", 127 "x4.00", else "x%.2f" (or "- x%.2f" below 64) of \|v-64\|/16 |
| 210 | MIDIFilter | 0x2c2ba | v | Notes/Note+CC |
| 211 | MIDIEcho | 0x2c2f2 | v | EchoOff/EchoOn |
| 212 | RndDensity | 0x2bd82 | v | "%d%" of v*100/127 |
| 213 | EqFreqLo | 0x2a954 | v | 80 Hz/110 Hz/160 Hz |
| 214 | EqFreqHi | 0x2a914 | v | 6 kHz/8 kHz/12 kHz |
| 215 | FlangerRate | 0x2c540 | v | "%.2fHz" of v*384000/2^24 (0 -> 0.0114) |
| 216 | PhaserRate | 0x2c5a2 | v | "%.2fHz" ("%.1f" from 119) of ((v*v>>1)*24000+768000)/2^24 |
| 218 | DrumFilterType | 0x2c74e | v | LP/BP/HP |
| 219 | OffOnOn | 0x2c78e | v | Off/On/On |
| 220 | NoiseGateLevel | 0x2e30c | v | 0 "-oo", 127 "-0dB", else "%.1fdB" of 20*log10(v/127) |
| 221 | MorphGroupSource1 | 0x2c7ce | v | Knob/Wheel |
| 222 | MorphGroupSource2 | 0x2c808 | v | Knob/Vel |
| 223 | MorphGroupSource3 | 0x2c842 | v | Knob/Keyb |
| 224 | MorphGroupSource4 | 0x2c87c | v | Knob/Aft.Tch |
| 225 | MorphGroupSource5 | 0x2c8b6 | v | Knob/Sust.Pd/G.Wh 1 |
| 226 | MorphGroupSource6 | 0x2c8f6 | v | Knob/Ctrl.Pd |
| 227 | MorphGroupSource7 | 0x2c930 | v | Knob/P.Stick |
| 228 | MorphGroupSource8 | 0x2c96a | v | Knob/G.Wh 2 |

### Dual table (`GetDualDependencyTextFunction`, default `DualDefault`)

| id | function | addr | args | formula / output |
|---:|---|---|---|---|
| 0 | OscSubFreq | 0x2f8f6 | coarse, fine | coarse 0 -> "0 Hz", else "%.4fHz" of 0.21484375*2^((coarse+(fine-64)/128-69)/12) |
| 65 | OscSemi2 | 0x2a53a | coarse, fine | "%+3d %+4.0f" of coarse-64 and cents (fine-64)*50/64 |
| 95 | LevMult | 0x2fd2c | mode, v | mode != 0: From0to100(v); else "%d" of 2v-128 |
| 96 | BiUniPol | 0x2aa80 | v, polarity | polarity 0: BiPol(v), else UniPol(v) |
| 97 | OscSyncTimbre | 0x2fa48 | a, b | a != 0: "+%d" of b; else OscFreq(b, 64) |
| 98 | OscPulseTimbre | 0x2fa8a | a, b | OscSyncTimbre(a, b) |
| 102 | dBLin | 0x2fd78 | v, mode | mode 2 (dB): dB(v), else From0to100(v) |
| 103 | LFOFreq | 0x2f41a | rate, range | range 0 Sub: period 699.05/(rate+1) s; 1 Lo: 0.0159*2^(rate/12) Hz; 2 Hi: 0.2555*2^(rate/12) Hz; 3 BPM: "%d" (BPM curve); 4 ClkSync: ratio table [rate>>2]; Hz below 0.1 shown as "%.1fs" period; "%.2f"/"%.1f"/"%.0f" by decade |
| 107 | ReverbTime | 0x2a4bc | time, roomType | Sci((3*roomType+3)*time/127 s) |
| 122 | LogicTime | 0x2b178 | time, range | 1/(96000*(0.93327-0.000644*time)^100) * 10/100/1000 (range 0/1/2) ms, as "%.2fm".."%.1fs" |
| 133 | BiUniPolCompact | 0x2aaae | v, polarity | polarity 0: BiPol(v), else UniPolCompact(v) |
| 137 | MultiEnvBiUni | 0x2af00 | v, outType | outType < 4: UniPol(v), else BiPol(v) |
| 141 | DelayTime2 | 0x2b114 | time, range | DelayTimeTap(time, 0, range) |
| 142 | BodeFreq | 0x2f6f4 | v, range | v^3*{8.78, 97.6, 1568, 1}[range]/2048383 Hz, 3/2/1/0 decimals by decade |
| 145 | DelayTimeTap8 | 0x2b05a | time, range | DelayTimeTapHelp(time, 0, kRange8[range], /8) |
| 147 | AmpGainNew | 0x2dfd4 | v, type | g = k[v]/2^21: type 0 "x%.2f" of g; else "%.1f" of 20*log10(g) ("-oo" at 0) |
| 201 | PitchShift | 0x2a68a | semi, fine | "%+2.1f %+4.0f" of (semi-64)/4 and cents (fine-64)*50/64 |
| 217 | GlideTimeRate | 0x2c638 | time, shape | shape 0: TimeShort(log(0.01)/(24000*log(k[v]/2^22-1)) s); else TimeShort(245760/(1500*a[v]) s) + "/oct" |

### Triple table (`GetTripleDependencyTextFunction`, default `TripleDefault`)

| id | function | addr | args | formula / output |
|---:|---|---|---|---|
| 60 | OscFreqDep | 0x2fe18 | coarse, fine, tuneMode | tuneMode 0 Semi: "%+3d %+4.0f"; 1 Freq: OscFreq; 2 Factor: OscFactor; 3 Partial: OscPartials; else Sub: OscSubFreq |
| 110 | ClkGenTempo | 0x2a5c4 | rate, active, source | active 0 -> "--"; source 0 -> BPM(rate); else "Master" |
| 140 | DelayTimeTap | 0x2b0d6 | time, sync, range | DelayTimeTapHelp(time, sync, kRangeTap[range]) |
| 143 | DelayTimeFx | 0x2b098 | time, sync, range | DelayTimeTapHelp(time, sync, kRangeFx[range]) |
| 146 | DelayTimeStereo | 0x2b01c | time, sync, range | DelayTimeTapHelp(time, sync, kRangeStereo[range]) |
| 198 | DXOscFreq | 0x2c3d6 | coarse, fine, fixed | fixed 0: "x%.2f" of c*(1+fine/100) (c=coarse, 0 -> 0.5); else 10^(coarse&3) * 10^(fine/100) Hz with 3/2/1/0 decimals ("%.0f Hz" with a space for the last decade) |

Functions present in `ParamText` but not reachable from the tables (not
ported or ported only for completeness): `DrumFreq`, `LFORateMul`, `LfoMono`,
`MasterTune`, `EqBw`, `ZeroOne`, `MinusZeroOne`, `ZeroTwo`, `OlddB`, `BidB`,
`AB`, `LevShift`, `OscShapeBiPol`, `OscShapeUniPol` (all empty stubs);
`MIDIClockState`, `PatchCategory` (enum helpers used elsewhere); `MIDIChannel`,
`OscKBT`, `OscCoarse`, `ControlPedalGain`, `FilterType`, `FilterCSlope`,
`MIDIClockKeyboardTrig`, `VocoderLevel` (ported, unused by the tables);
`SmallSlaveMult` is identical to `SlaveMult` (internal helper of `LfoRateMult`).

## 7. Open questions

- Byte +0x12 and the pointer at +0x18 of the module parameter records are not
  used for text; their meaning is unknown.
- `OscSyncTimbre`/`OscPulseTimbre` (dual 97/98) are only used by the hidden
  `SyncOsc`/`PulseOsc` panels; argument meaning is a guess (`a` switch, `b` value).
- Values above 127 were not evaluated; the port returns "" where the original
  would read past a table.
