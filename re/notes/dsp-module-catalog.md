# G2 DSP module catalog: what Clavia's OS builds for each module

Measured 2026-10-09 with the emulated G2 of `re/notes/g2-hardware-and-emulation.md` §3.6: the user's own OS 1.62 on
Unicorn, four emulated DSP56367s, G2fresh's protocol client on the emulated USB chip.

How claims are marked:
* **[C]**: measured in Clavia's OS/DSP code running in the emulator.
* **(inferred)**: my reading of the measurements.

The raw catalog is derived from Clavia's firmware, so it stays in the gitignored `original/firmware/catalog/`. This
note has formulas, sizes and short excerpts only.

## 1. Pipeline

### Tools
* `tools/firmware/g2catalog.py` builds the catalog (run it under `nice -n 10`; one boot serves every module).
* `tools/firmware/g2catalog_report.py` turns it into the per-module summary of §4.
* `tools/firmware/g2catalog_response.py` measures responses and costs offline with `g2dspframe`.
* A full refresh, in order:
  1. `g2catalog.py --modules all`;
  2. `g2catalog.py --modules all --connected --response` (one more boot);
  3. `g2catalog_response.py`;
  4. `g2catalog_response.py --costs`;
  5. `g2catalog_report.py --md …`.
* Both use the user's firmware at run time and contain no Clavia data.
* Library side:
  * `emu/protobridge` gained `g2p_send_module_patch`, `g2p_set_param`, `g2p_set_mode`, `g2p_play_note` and
    `g2p_module_cost` (the editor's estimate, `core/patch_load`);
  * `emu/protobridge` also gained `g2p_send_chain_patch` (a source into one or all inputs);
  * `emu/dspframe/g2dspframe` gained test inputs (`in=`, applied at a PC with `inat=`), pokes, multi-word dumps
    and multi-frame traces.

### Steps per module
The OS boots once (about 75 s). After the editor sync, each module goes through these steps:
1. **Upload** a patch built with `core/edit`: the module in the VA area (FX if it is FX-only) plus a 2-Out fed by
   its first two outputs, so the OS keeps it. The patch goes to slot A.
2. **Record the OS's host-port writes** to the four DSPs. The stage-1 monitor's commands decode to X/Y/P writes and
   to DOR0/DOR1/DCO1/LA.
   * Compare with the baseline (a patch with the 2-Out alone).
   * Dump the changed DSP's memories (`dspN_live_*.bin`), for `g2dspframe`.
3. **Match the linked program** against the OS's fragment library (`g2frags.py`). A match tolerates up to a quarter
   of differing words; those are the words the OS patched (addresses, constants, jump targets).
4. **Learn the background writes** during 100 quiet ticks and ignore them later:
   * the output DSP's level ramp at X:`$1739`;
   * cable words the OS rewrites.
5. **Change every parameter live** (realtime message 40 through `proto::Client::setParam`) to 9 values from min to
   max, and every mode to each value.
   * After each change, wait until no new write for 30 ticks: the OS **slews** many parameter words over 64 writes.
   * Record the final word, the number of writes and the first one.
6. **Second session:**
   * Re-sweep the parameters that wrote nothing, with OscA wired to every input.
   * Dump the memories of OscA → module input 0 → 2-Out.
7. **Offline:**
   * Trace 4 frames to find the cable the module reads and the one the 2-Out reads.
   * Drive that cable right before the module's read with sines (50 Hz–20 kHz) and DC, and record the output.
   * Count the frame's instructions against the baseline.

### Unit conventions
* "Fraction" is a DSP word as a signed 24-bit fraction (`0x400000` = 0.5).
* "v" is the knob value.
* DSPs are numbered by chip-select line: dsp3 = A6 = OS DSP 0, the voice DSP a single-voice patch lands on; dsp0 =
  A3, the output DSP.

## 2. Coverage, run time, data [C]
* **All 165 placeable module types** (data/modules.json: selectable, with a VA or FX context) were catalogued
  without errors. The 4 non-selectable internal types (Driver, Resonator, Red2Blue, Blue2Red) were not.
* **Run time:**
  * one boot (about 75 s), then 0.6–35 s per module, about 20 s on average;
  * about 75 minutes in all on an M1 under `nice -n 10`, one emulator session at a time;
  * a second session (about 10 min) re-swept with all inputs connected and dumped the response patches;
  * the offline response and cost runs take about 2 minutes.
* **Parameters:** 903. Their words split as:
  * 272 linear;
  * 41 exponential, plus 56 exponential except at one end;
  * 323 program-word changes ("code");
  * 227 one-word-per-choice selectors;
  * 196 tables (no simple fit);
  * 271 words are slewed by the OS.
* **164 parameters change no DSP word.** Display-only modes (Tune Md, Time Md), MIDI/automation modules handled by
  the ColdFire (CtrlSend/Rcv, NoteRcv, NoteZone, Automate, PCSend), Fx-In Bus, the sequencers' step/event values
  (open: the sequencer data may be held by the OS), and a few levels of mixers fed by one source.
* **Responses** (§1, step 6, offline): measured for 119 modules with an audio input. The 46 modules whose input 0 is
  a trigger/control input read it in a way the cable finder does not see (inferred: through a pointer or a
  converter), so they have none.
* **Files** (`original/firmware/catalog/`, gitignored):
  * per module: `<Module>/module.json`, `dsp3_live_*.bin` and `resp/` (memories of OscA → module → 2-Out, and
    `response.json`);
  * `catalog.json` (index), `baseline.json`, `summary.json` (the report's data), `costs.json`;
  * `_baseline_VA/`, `_baseline_FX/`.

## 3. General findings [C] unless marked
* **A single-voice patch is compiled onto dsp3 (A6, OS DSP 0) only. This holds for FX-area modules too** (Fx-In,
  and the modules that allow only FX).
  * Module code is linked into the per-frame program after the frame prologue.
  * X/Y blocks are allocated upward from `$50` and reached through r3/r4.
  * Cables are zero-page X words: the source writes `move b,x:$n`, readers use `move x:$n,…`.
  * **An unconnected input is linked as an immediate.** The library's `move x:$0,x0` becomes `move #0,x0`. So
    unconnected modulation inputs cost nothing, and their amount knobs are not sent at all; the OS skips them.
* **The OS converts parameters**, and the DSP code uses the words as they are.
  * **2^(v/12) exactly:** every oscillator Coarse, filter Freq and LFO/Random Rate. Coefficients differ per
    family.
    * OscA Coarse → Y:`$58` = 2858.6·2^(v/12) = f·2²⁴/48 kHz, with f the MIDI frequency of note v
      (8.1785·2^(v/12) Hz, 0.033% above C−1 = 8.1758 Hz).
    * Filter Freq → ≈ 0.00045·2^(v/12) (fraction) ≈ π·fc/fs.
      * At FltLP's default (75) that is fc ≈ 1047 Hz. The measured one-pole response gives −3 dB at about
        1.08 kHz (inferred: frequency warping).
      * The curve flattens at the top (v ≥ about 120), (inferred) a clamp below Nyquist.
  * **Fine:** OscA/B Fine → X:`$5A` = 0.4858·2^(v/1536): a ±½-semitone multiplier around 0.5.
  * **Linear:**
    * Sustain → v/128 (0 to 0x7FFFFF);
    * FltNord Res → v/508 (0.25 at 127);
    * DlySingleA Time → the integer (127−v)·256 (inferred: a read offset in samples).
  * **Cubic crossfades** (Reverb Mix): wet = min(1, (v/64)³), dry = min(1, ((128−v)/64)³).
  * **Envelope stage times** → a pair (k, about 0.5·(1−k)) of one-pole coefficients; k falls faster than
    exponentially with v (inferred: k = 1−exp(−1/(t·fs)) with t from the editor's time table).
  * **Mixer levels** → about −(v/127)^2.9 (a table: a dB-like curve, sign folded into the code).
* **The OS slews continuous knob words**, not the DSP. After a change it writes the word 64 times toward the target,
  on a geometric path for pitch-like words (successive values ×2^(1/12) for OscA Coarse) and linear for others.
  This takes about 130 timer ticks. Slewed words are marked in §4.
* **Code-type parameters patch the program**:
  * switches (On/Off, KBT, Reset, KBG) rewrite one instruction;
  * waveform/curve selectors rewrite a jump target into the module's branches (OscA Wave at P:`$29A`; ShpExp Curve
    patches three words).
* **A mode change re-links the whole patch.** Every DSP's frame program is re-uploaded (FltLP SlopeMode: about
  390 P words on all four DSPs). It does not change in place.
* **Library fragments** (`g2frags.py` output, matched with up to ¼ of the words differing):
  * they explain 65% of the linked module words;
  * the OS patches 0–4 words per fragment (cable addresses, immediates, jump targets);
  * 71 modules match no fragment: mostly tiny switch/mixer modules (fragments under 3 words are not matched),
    the delays, reverb and sequencers. **(inferred)** `g2frags.py`'s strict descriptor scan misses some
    descriptor layouts; a raw search of CODE for their words is the next step.
* **Cost against the editor's estimate.**
  * `g2dspframe` counts instructions from the frame entry to the first `rti`; the baseline (2-Out alone) is 124.
  * Where the module's code is on that path, measured/estimated cycles has median 1.1 (FltLP 8/12, FltNord 39/60,
    Vocoder 96/385+…), but some estimates are far off (OscShpA 91/6, OscDual 123/6, PShift 75/1.25).
  * About 100 modules measure ≤ 0. Their code is not on that path when inputs are unconnected, or runs elsewhere:
    control-rate code, or code skipped for unconnected inputs.
  * The editor's `pMem` figures are not linked words (OscA: 4 against 133).

## 4. Per-module catalog

Generated by `tools/firmware/g2catalog_report.py --md` from `original/firmware/catalog/` (re-run it to refresh).

How to read the entries:
* **"cost"** is measured instructions per sample above the baseline. "≤ 0" means not measurable this way (§3).
* **"response"** has the input 0 cable driven by a 0.5 sine or DC at default settings. The gain is output/input at
  the output cable the 2-Out reads; −174 dB means no output.
* **Data** lists the X/Y words that differ from the baseline.
* **"[inputs connected]"** marks parameters measured in the second pass, with OscA on every input.
* Word addresses are those of this one-module patch: blocks shift with module order in larger patches.
* Formulas are fitted to 9 knob values.

### Keyboard: Keyboard (type 1, In/Out, VA)
* in: -; out: Pitch/control, Gate/logic, Lin/control, Release/control, Note/control, Exp/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 12/4 cycles
* DSP 3: program +0 words (estimate P 12+0, cycles 0+12/4); fragments: none matched; data X:$0-$3, X:$5-$5

### 4-Out: 4 Outputs (type 3, In/Out, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio; out: -
* cost [C]: -19 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 19 + 0/4 cycles
* DSP 3: program +-21 words (estimate P 21+0, cycles 19+0/4); fragments: none matched; data 
* Dest (0..2) [inputs connected]: P:$317 code; P:$32E code; Y:$55 one word per choice; Y:$58 one word per choice
* On/Off (0..1): no DSP word changes seen
* Pad (0..1): no DSP word changes seen

### 2-Out: 2 Outputs (type 4, In/Out, VA)
* in: InL/audio, InR/audio; out: -
* Dest (0..5): P:$292 code; Y:$50 one word per choice
* On/Off (0..1): P:$297 code; P:$298 code; X:$51 one word per choice
* Pad (0..1): no DSP word changes seen

### Invert: Logic Inverter (type 5, Logic, VA)
* in: In1/logic, In2/logic; out: Out1/logic, Out2/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -9.94 / -9.95 / -9.96 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.25; 1 kHz harmonics 2-6: -29.7, -9.6, -29.7, -14.1, -29.7 dB
* DSP 3: program +10 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$53-$54

### OscB: Osc B (type 7, Osc, VA)
* in: Pitch/audio, PitchVar/audio, Sync/audio, FmMod/audio, Shape M/audio; out: Out/audio
* cost [C]: 39 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 6 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, -0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +186 words (estimate P 7+0, cycles 6+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300edebc (61 w, 2 patched), 300edcb4 (17 w, 2 patched), 300edd54 (55 w, 4 patched); data X:$0-$1, X:$51-$51, X:$53-$5B, Y:$50-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* Tune Md (0..3): no DSP word changes seen
* FM M (0..127) [inputs connected]: Y:$59 table (slewed)
* Shape (0..127): X:$52 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Shape M (0..127) [inputs connected]: X:$65 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Wave (0..4): P:$299 code; P:$29B code; X:$53 one word per choice; X:$54 one word per choice; X:$55 one word per choice; X:$56 one word per choice; Y:$52 one word per choice; Y:$53 one word per choice
* On/Off (0..1): P:$343 code
* FM PTrk (0..1): no DSP word changes seen

### OscShpB: Osc Shape B (type 8, Osc, VA)
* in: Pitch/audio, PitchVar/audio, Sync/audio, FmMod/audio, Shape M/audio; out: Out/audio
* cost [C]: 87 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 6 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, -6e-05; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +97 words (estimate P 7+0, cycles 6+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300edd7c (86 w, 3 patched); data X:$0-$1, X:$51-$51, X:$53-$5B, Y:$50-$51, Y:$53-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* Tune Md (0..3): no DSP word changes seen
* FM Amt (0..127) [inputs connected]: Y:$59 table (slewed)
* Shape (0..127): X:$52 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Shape M (0..127) [inputs connected]: X:$65 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* On/Off (0..1): P:$2E9 code
* FM PTrk (0..1): no DSP word changes seen
* mode Wave: 0→0 P/0 XY words on -, 1→485 P/34 XY words on 0,1,2,3, 2→455 P/30 XY words on 0,1,2,3, 3→455 P/30 XY words on 0,1,2,3, 4→511 P/30 XY words on 0,1,2,3, 5→428 P/30 XY words on 0,1,2,3, 6→405 P/25 XY words on 0,1,2,3, 7→450 P/30 XY words on 0,1,2,3

### OscC: Osc C (type 9, Osc, VA)
* in: PitchVar/audio, Sync/audio, FmMod/audio, Pitch/audio; out: Out/audio
* cost [C]: 22 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, 0.004; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +30 words (estimate P 0+0, cycles 0+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300edcb4 (17 w, 1 patched); data X:$0-$1, X:$51-$51, X:$53-$5B, Y:$50-$51, Y:$53-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Tune Md (0..3): no DSP word changes seen
* FM M (0..127) [inputs connected]: Y:$59 table (slewed)
* On/Off (0..1): P:$2A7 code
* FM PTrk (0..1): no DSP word changes seen
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* mode Wave: 0→0 P/0 XY words on -, 1→412 P/28 XY words on 0,1,2,3, 2→401 P/28 XY words on 0,1,2,3, 3→405 P/29 XY words on 0,1,2,3, 4→405 P/29 XY words on 0,1,2,3, 5→405 P/29 XY words on 0,1,2,3

### Reverb: Reverb (type 12, FX, VA)
* in: InL/audio, InR/audio; out: OutL/audio, OutR/audio
* cost [C]: 183 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.35 / 0.95 / 0.15 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.26752, 0.01422; 1 kHz harmonics 2-6: -63.6, -67.5, -71.9, -73.2, -75.1 dB
* DSP 3: program +194 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$51-$51, X:$53-$91, Y:$50-$5E
* Time (0..127): Y:$53 exp~ about 0.223809 * 2^(v/312.3) (fraction), off by more than 2% at v=16,31,32,111,127; Y:$54 table; Y:$55 exp~ about 0.379656 * 2^(v/312.3) (fraction), off by more than 2% at v=16,31,32,111,127; Y:$58 table; Y:$59 table
* Bright (0..127): Y:$50 linear 0.00545669*v +0.007 (fraction); Y:$51 linear -0.00545669*v +0.993 (fraction); Y:$55 linear 0.00677805*v +0.00869504 (fraction); Y:$56 linear -0.00779528*v +0.99 (fraction)
* Mix (0..127): X:$8D table; X:$8E table
* On/Off (0..1): P:$337 code; P:$33E code
* mode RoomType: 0→0 P/0 XY words on -, 1→556 P/93 XY words on 0,1,2,3, 2→556 P/93 XY words on 0,1,2,3, 3→556 P/93 XY words on 0,1,2,3

### OscString: Osc String (type 13, Osc, VA)
* in: In/audio, Pitch/control, PitchVar/control; out: Out/audio
* cost [C]: 133 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, 1.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +150 words (estimate P 0+0, cycles 0+0/4); fragments: 300ed56c (8 w, 1 patched), 300f5b88 (53 w, 12 patched); data X:$0-$1, X:$51-$5B, Y:$50-$5F
* Coarse (0..127): Y:$5F exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$63 table (slewed)
* Tune Md (0..3): no DSP word changes seen
* Decay (0..127): Y:$52 table (slewed)
* Damp (0..127): Y:$53 linear -0.0078125*v +0.992188 (fraction) (slewed)
* On/Off (0..1): P:$31F code

### Sw8-1: Switch 8-1 (type 15, Switch, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio, In7/audio, In8/audio; out: Out/audio, Control/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +15 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$1-$1, X:$53-$57, Y:$53-$56
* Source (0..7): X:$54 one word per choice; X:$55 one word per choice; X:$56 one word per choice; X:$57 one word per choice; Y:$53 one word per choice; Y:$54 one word per choice; Y:$55 one word per choice; Y:$56 one word per choice

### ValSw1-2: Value Switch 1-2 (type 17, Switch, VA)
* in: Input/audio, Ctrl/control; out: OutOn/audio, OutOff/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +12 words (estimate P 1+1, cycles 12+0/4); fragments: 300fec70 (12 w, 3 patched); data X:$53-$53, Y:$53-$55
* Value (0..63): X:$53 table

### X-Fade: Cross Fader (type 18, Mixer, VA)
* in: In1/audio, In2/audio, Mod/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -2.5 / -2.5 / -2.5 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1875; 1 kHz harmonics 2-6: -163.6, -161.8, -163.6, -155.9, -163.6 dB
* DSP 3: program +14 words (estimate P 0+0, cycles 0+0/4); fragments: 300fe358 (14 w, 3 patched); data X:$53-$54, Y:$53-$53
* XFadeM (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* XFade (0..127): X:$53 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* Log/Lin (0..1): P:$23C code; P:$23E code

### Mix4-1B: Mixer 4-1 B (type 19, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, Chain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data Y:$53-$53
* Level 1 (0..127) [inputs connected]: X:$57 table (slewed)
* Level 2 (0..127) [inputs connected]: Y:$55 table (slewed)
* Level 3 (0..127) [inputs connected]: X:$58 table (slewed)
* Level 4 (0..127) [inputs connected]: Y:$56 table (slewed)
* Lin/Exp (0..2) [inputs connected]: X:$57 one word per choice; X:$58 one word per choice; Y:$55 one word per choice; Y:$56 one word per choice

### EnvADSR: Envelope ADSR (type 20, Env, VA)
* in: In/audio, Gate/logic, AM/control; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 5 + 31/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +37 words (estimate P 0+0, cycles 5+31/4); fragments: 300f075e (37 w, 4 patched); data X:$53-$64, Y:$53-$57
* Shape (0..3): X:$59 one word per choice; X:$5A one word per choice; X:$5C one word per choice; X:$5D one word per choice; X:$5E one word per choice; X:$60 one word per choice; X:$61 one word per choice; X:$62 one word per choice; X:$64 one word per choice
* Attack (0..127): X:$59 exp 0.496276 * 2^(v/8594) (fraction); X:$5A table
* Decay (0..127): X:$5D exp~ about 0.497796 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Sustain (0..127): X:$5F linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Release (0..127): X:$61 exp~ about 0.497796 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* OutType (0..5): X:$56 one word per choice; X:$57 one word per choice; Y:$57 one word per choice
* KBG (0..1): P:$238 code
* Reset (0..1): P:$241 code

### Mux1-8: Multiplexer 1-8 (type 21, Switch, VA)
* in: In/audio, Ctrl/control; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio, Out5/audio, Out6/audio, Out7/audio, Out8/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +52 words (estimate P 1+1, cycles 12+0/4); fragments: 300f0d44 (38 w, 2 patched); data X:$53-$5F, Y:$53-$59

### PartQuant: Partial Quantizer (type 22, Note, VA)
* in: In/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +18 words (estimate P 0+0, cycles 0+0/4); fragments: 300f7b7e (18 w, 1 patched); data X:$53-$56, Y:$53-$72
* Range (0..127): X:$53 linear 0.00774282*v -4.26064e-05 (fraction)

### ModADSR: Envelope Modulation ADSR (type 23, Env, VA)
* in: Gate/logic, AMod/control, DMod/control, SMod/control, RMod/control, In/audio, AM/control; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 5 + 85/4 cycles
* DSP 3: program +86 words (estimate P 0+0, cycles 5+85/4); fragments: 300f914e (86 w, 7 patched); data X:$53-$7A, Y:$53-$54
* Attack (0..127): X:$60 linear 1.19209e-07*v +0 (fraction)
* Decay (0..127): X:$69 linear 1.19209e-07*v +0 (fraction)
* Sustain (0..127): X:$57 linear 0.00196124*v -0.000313512 (fraction)
* Release (0..127): X:$72 linear 1.19209e-07*v +0 (fraction)
* Atk M (0..127): X:$5D linear -4.79845e-07*v +1.40696e-07 (fraction)
* Dcy M (0..127): X:$67 linear -4.79845e-07*v +1.40696e-07 (fraction)
* Sust M (0..127): X:$58 linear 0.00785192*v -0.0018441 (fraction)
* Rel M (0..127): X:$71 linear -4.79845e-07*v +1.40696e-07 (fraction)
* OutType (0..5): X:$78 one word per choice; X:$79 one word per choice; X:$7A one word per choice
* KBG (0..1): P:$240 code

### LfoC: LFO C (type 24, LFO, VA)
* in: Rate/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +39 words (estimate P 0+0, cycles 0+0/4); fragments: 300f0f6a (7 w, 0 patched), 300f1364 (7 w, 1 patched); data X:$0-$8, X:$53-$59, Y:$53-$56
* Rate (0..127): X:$53 exp 2.12646e-05 * 2^(v/12) (fraction) (slewed)
* Mode (0..1): no DSP word changes seen
* OutType (0..5): X:$58 one word per choice; Y:$56 one word per choice
* Range (0..3): X:$53 one word per choice
* On/Off (0..1): P:$259 code
* mode Wave: 0→0 P/0 XY words on -, 1→389 P/27 XY words on 0,1,2,3, 2→388 P/26 XY words on 0,1,2,3, 3→393 P/27 XY words on 0,1,2,3, 4→413 P/34 XY words on 0,1,2,3, 5→427 P/36 XY words on 0,1,2,3, 6→415 P/34 XY words on 0,1,2,3, 7→429 P/36 XY words on 0,1,2,3

### LfoShpA: LFO Shape A (type 25, LFO, VA)
* in: Rate/control, RateVar/control, Rst/control, Shape M/control, Phase M/control, Dir/control; out: Out/control, Snc/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +361 words (estimate P 0+0, cycles 0+0/4); fragments: 300f0f6a (7 w, 0 patched), 300f0fba (45 w, 5 patched), 300f1364 (7 w, 1 patched); data X:$0-$8, X:$53-$5F, Y:$53-$5E
* Rate (0..127): X:$53 exp 2.12646e-05 * 2^(v/12) (fraction) (slewed)
* Range (0..4): P:$237 code; P:$243 code; X:$53 one word per choice
* KBT (0..4): P:$236 code
* Rate M (0..127) [inputs connected]: X:$5C table (slewed)
* On/Off (0..1): P:$39B code
* Shape (0..127): X:$57 linear 0.0156899*v -1.00251 (fraction) (slewed)
* Phase M (0..127): Y:$56 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Phase (0..127): X:$56 linear 0.015625*v -1 (fraction) (slewed)
* Shape M (0..127): Y:$57 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Mode (0..1): P:$236 code; P:$237 code; P:$243 code
* OutType (0..5): X:$5E one word per choice; Y:$5E one word per choice
* Wave (0..5): P:$278 code; X:$56 one word per choice; X:$58 one word per choice

### LfoA: LFO A (type 26, LFO, VA)
* in: Rate/control, RateVar/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 3 + 0/4 cycles
* DSP 3: program +147 words (estimate P 4+0, cycles 3+0/4); fragments: 300f0f6a (7 w, 0 patched), 300edcb4 (17 w, 4 patched), 300f1724 (45 w, 3 patched), 300f1364 (7 w, 1 patched); data X:$0-$8, X:$53-$5B, Y:$53-$5C
* Rate (0..127): X:$53 exp 2.12646e-05 * 2^(v/12) (fraction) (slewed)
* Mode (0..1): P:$236 code
* KBT (0..4): P:$236 code
* Rate M (0..127) [inputs connected]: X:$5C table (slewed)
* Wave (0..5): P:$249 code; X:$54 one word per choice
* On/Off (0..1): P:$2C5 code
* OutType (0..5): X:$5A one word per choice; Y:$5C one word per choice
* Range (0..3): X:$53 one word per choice

### OscMaster: Osc Master (type 27, Osc, VA)
* in: Pitch/audio, PitchVar/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 24/4 cycles
* DSP 3: program +8 words (estimate P 0+0, cycles 0+24/4); fragments: 300f8a58 (8 w, 2 patched); data X:$0-$0, X:$53-$54, Y:$53-$54
* Coarse (0..127): X:$54 linear 0.00390625*v -0.25 (fraction) (slewed)
* Fine (0..127): Y:$54 linear 3.05176e-05*v -0.00195312 (fraction) (slewed)
* KBT (0..1): X:$53 one word per choice
* Tune Md (0..2): no DSP word changes seen
* Pitch M (0..127): Y:$53 table (slewed)

### Saturate: Saturate (type 28, Shaper, VA)
* in: In/audio, Mod/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 53/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -144.0, -152.2, -149.1, -147.6, -166.1 dB
* DSP 3: program +43 words (estimate P 0+0, cycles 0+53/4); fragments: 300f5274 (43 w, 3 patched); data X:$53-$54, Y:$53-$53
* Sat (0..127): X:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Sat M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* On/Off (0..1): P:$25E code
* Curve (0..3): P:$24C code

### MetNoise: Metallic noise oscillator (type 29, Osc, VA)
* in: FreqMod/audio, ColorMod/audio; out: Out/audio
* cost [C]: 87 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -82.85 / -82.19 / -60.91 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → -1e-05; 1 kHz harmonics 2-6: 1.2, 5.4, 9.1, -2.2, 22.7 dB
* DSP 3: program +116 words (estimate P 0+0, cycles 0+0/4); fragments: 30106270 (12 w, 2 patched), 30106248 (11 w, 2 patched), 30106298 (6 w, 1 patched), 301062c0 (27 w, 1 patched); data X:$50-$6A, Y:$51-$62
* Color (0..127): X:$66 linear -0.00196124*v +0.250314 (fraction) (slewed)
* Freq (0..127): X:$63 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* On/Off (0..1): P:$2C1 code
* Freq M (0..127): Y:$5C linear 0.00785192*v -0.0018441 (fraction) (slewed)
* ColorM (0..127): Y:$5E linear 0.00785192*v -0.0018441 (fraction) (slewed)

### Device: Device (type 30, In/Out, VA)
* in: -; out: Wheel/control, AftTouch/control, ControlPedal/control, SustainPedal/logic, PitchStick/control, GlobalWheel1/control, GlobalWheel2/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 17 + 0/4 cycles
* DSP 3: program +0 words (estimate P 16+0, cycles 17+0/4); fragments: none matched; data X:$0-$6

### Noise: Noise (type 31, Osc, VA)
* in: -; out: Out/audio
* cost [C]: 13 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 13 + 0/4 cycles
* DSP 3: program +13 words (estimate P 0+0, cycles 13+0/4); fragments: 300f8b7a (13 w, 2 patched); data X:$50-$56, Y:$50-$51, Y:$53-$54
* Color (0..127): X:$51 exp~ about 0.844332 * 2^(v/370) (fraction), off by more than 2% at v=16,31,32,111,127 (slewed); X:$52 exp~ about 0.298994 * 2^(v/-12.06) (fraction), off by more than 2% at v=16,31,32,127 (slewed); X:$53 table (slewed)
* On/Off (0..1): P:$296 code

### Eq2Band: Eq 2 Band (type 32, Filter, VA)
* in: In/audio; out: Out/audio
* cost [C]: 35 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -148.0, -149.7, -148.0, -140.6, -148.0 dB
* DSP 3: program +35 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$51, X:$53-$57, Y:$51-$51, Y:$53-$55
* Lo Gain (0..127): X:$51 exp~ about 0.0407434 * 2^(v/-21.53) (fraction), off by more than 2% at v=79,95,111,127 (slewed); X:$52 table (slewed)
* Hi Gain (0..127): X:$53 table (slewed); X:$54 table (slewed)
* Level (0..127): X:$50 table (slewed)
* On/Off (0..1): P:$2A9 code
* Lo Freq (0..2): X:$51 one word per choice
* Hi Freq (0..2): X:$53 one word per choice

### Eq3band: Eq 3 Band (type 33, Filter, VA)
* in: In/audio; out: Out/audio
* cost [C]: 58 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -148.0, -149.7, -148.0, -140.6, -148.0 dB
* DSP 3: program +58 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$51, X:$53-$5B, Y:$51-$51, Y:$53-$57
* Lo (0..127): X:$51 exp~ about 0.0407434 * 2^(v/-21.53) (fraction), off by more than 2% at v=79,95,111,127 (slewed); X:$52 table (slewed)
* Mid (0..127): X:$55 exp~ about 0.0624168 * 2^(v/214.2) (fraction), off by more than 2% at v=0,79,95,111,127 (slewed); X:$56 exp~ about 0.269919 * 2^(v/-23.78) (fraction), off by more than 2% at v=0,79,95,111,127 (slewed); X:$58 table (slewed)
* Mid Frq (0..127): X:$55 exp~ about 0.0032956 * 2^(v/20.5) (fraction), off by more than 2% at v=111,127 (slewed); X:$56 exp~ about 0.0445217 * 2^(v/-971.8) (fraction), off by more than 2% at v=111,127 (slewed)
* Hi (0..127): X:$53 table (slewed); X:$54 table (slewed)
* Level (0..127): X:$50 table (slewed)
* On/Off (0..1): P:$2C0 code
* Lo Freq (0..2): X:$51 one word per choice
* Hi Freq (0..2): X:$53 one word per choice

### ShpExp: Shape Exp (type 34, Shaper, VA)
* in: In/audio, ModIn/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 53/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +26 words (estimate P 0+0, cycles 0+53/4); fragments: 300f524c (26 w, 3 patched); data X:$53-$53, Y:$53-$54
* Shape (0..127): X:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Shape M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* On/Off (0..1): P:$24D code
* Curve (0..3): P:$244 code; P:$246 code; P:$24A code

### SwOnOffM: Switch On/Off Momentary (type 36, Switch, VA)
* in: In/audio; out: Out/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +5 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Switch (0..1): P:$237 code

### Pulse: Pulse (type 38, Logic, VA)
* in: In/logic, Time/control; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -39.65 / -19.8 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: -0.5, -1.3, -2.5, -4.1, -6.3 dB
* DSP 3: program +43 words (estimate P 0+0, cycles 0+0/4); fragments: 300f4250 (28 w, 2 patched), 300f4278 (15 w, 1 patched); data X:$53-$59, Y:$53-$58
* Time (0..127): X:$53 linear 0.00196124*v -0.000313512 (fraction)
* Time M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction)
* Range (0..2): X:$55 one word per choice
* mode PulseMode: 0→0 P/0 XY words on -, 1→405 P/27 XY words on 0,1,2,3

### Mix8-1B: Mixer 8-1 B (type 40, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio, In7/audio, In8/audio, Chain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data Y:$53-$53
* Level 1 (0..127) [inputs connected]: X:$57 table (slewed)
* Level 2 (0..127) [inputs connected]: Y:$55 table (slewed)
* Level 3 (0..127) [inputs connected]: X:$58 table (slewed)
* Level 4 (0..127) [inputs connected]: Y:$56 table (slewed)
* Level 5 (0..127) [inputs connected]: X:$59 table (slewed)
* Level 6 (0..127) [inputs connected]: Y:$57 table (slewed)
* Level 7 (0..127) [inputs connected]: X:$5A table (slewed)
* Level 8 (0..127) [inputs connected]: Y:$58 table (slewed)
* Lin/Exp (0..2) [inputs connected]: X:$57 one word per choice; X:$58 one word per choice; X:$59 one word per choice; X:$5A one word per choice; Y:$55 one word per choice; Y:$56 one word per choice; Y:$57 one word per choice; Y:$58 one word per choice
* Pad (0..2): P:$237 code

### EnvH: Envelope Hold (type 41, Env, VA)
* in: Trig/logic, AM/control, In/audio; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* DSP 3: program +14 words (estimate P 1+1, cycles 12+0/4); fragments: 300f94c2 (14 w, 2 patched); data X:$53-$55, Y:$53-$55
* HldTime (0..127): X:$54 table (slewed)
* OutType (0..3): X:$55 one word per choice; Y:$54 one word per choice

### Delay: Logic Delay (type 42, Logic, VA)
* in: In/logic, TimeMod/control; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.02411; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +42 words (estimate P 1+1, cycles 0+0/4); fragments: 300f4250 (28 w, 2 patched), 300f42a0 (14 w, 1 patched); data X:$53-$59, Y:$53-$55
* Time (0..127): X:$53 linear 0.00196124*v -0.000313512 (fraction)
* Time M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction)
* Range (0..2): X:$55 one word per choice
* mode DelayMode: 0→0 P/0 XY words on -, 1→404 P/24 XY words on 0,1,2,3, 2→420 P/29 XY words on 0,1,2,3

### Constant: Constant Value (type 43, Level, VA)
* in: -; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +0 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$0-$0
* Level (0..127): X:$0 linear 0.00392247*v -0.250627 (fraction) (slewed)
* Pol (0..1): no DSP word changes seen

### LevMult: Level Multiplier (type 44, Level, VA)
* in: In/audio, Mod/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +5 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data 

### FltVoice: Filter Voice (type 45, Filter, VA)
* in: In/audio, Vowel/control, FreqMod/audio; out: Out/audio
* cost [C]: 43 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -10.88 / -17.01 / -21.66 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.06641; 1 kHz harmonics 2-6: -92.1, -94.7, -100.0, -102.6, -104.5 dB
* DSP 3: program +110 words (estimate P 0+0, cycles 0+0/4); fragments: 300fac84 (43 w, 4 patched); data X:$50-$6E, Y:$51-$51, Y:$53-$70
* Vowel 1 (0..8): Y:$5F table; Y:$61 table; Y:$63 table; Y:$64 table; Y:$65 table; Y:$66 table
* Vowel 2 (0..8): X:$66 table; X:$68 table; X:$6A table; X:$6B table; X:$6C table; X:$6D table; Y:$5F table; Y:$61 table; Y:$63 table; Y:$64 table; Y:$65 table; Y:$66 table; Y:$67 table; Y:$69 table; Y:$6B table; Y:$6C table; Y:$6D table; Y:$6E table
* Vowel 3 (0..8): Y:$67 table; Y:$69 table; Y:$6B table; Y:$6C table; Y:$6D table; Y:$6E table
* Level (0..127): Y:$6F table (slewed)
* Nav (0..127): X:$64 linear 0.00196124*v -0.125314 (fraction) (slewed)
* Nav M (0..127): Y:$5E linear 0.00785192*v -0.0018441 (fraction)
* Freq (0..127): X:$5F linear 0.00196124*v -0.125314 (fraction) (slewed)
* Freq M (0..127): Y:$5B linear 0.00785192*v -0.0018441 (fraction)
* Res (0..127): Y:$70 table (slewed)
* On/Off (0..1): P:$2F6 code

### EnvAHD: Envelope AHD (type 46, Env, VA)
* in: Trig/logic, AM/control, In/audio; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +55 words (estimate P 1+1, cycles 0+0/4); fragments: 300f0826 (55 w, 6 patched); data X:$53-$6F, Y:$53-$58
* Shape (0..3): P:$259 code; X:$5C one word per choice; X:$5D one word per choice; X:$61 one word per choice; X:$6A one word per choice; X:$6B one word per choice; X:$6F one word per choice
* Attack (0..127): X:$5C table; X:$5D exp 0.496276 * 2^(v/8594) (fraction); X:$5F exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=16
* Hold (0..127): X:$63 table; X:$64 exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$66 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Trigged (0..1): P:$241 code
* Release (0..127): X:$6A table; X:$6B exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$6D exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* OutType (0..3): X:$58 one word per choice; X:$59 one word per choice
* KBG (0..1): P:$238 code

### Pan: Pan (type 47, Mixer, VA)
* in: In/audio, Mod/audio; out: OutL/audio, OutR/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -2.5 / -2.5 / -2.5 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1875, 0.1875; 1 kHz harmonics 2-6: -163.6, -161.8, -163.6, -155.9, -163.6 dB
* DSP 3: program +14 words (estimate P 0+0, cycles 0+0/4); fragments: 300fe25c (14 w, 2 patched); data X:$53-$54, Y:$53-$53
* Pan M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Pan (0..127): X:$53 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* Log/Lin (0..1): P:$23C code; P:$23E code

### MixStereo: Mixer Stereo (type 48, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio; out: OutL/audio, OutR/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -15.03 / -15.03 / -15.03 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0443, 0.04476; 1 kHz harmonics 2-6: -139.9, -128.0, -131.0, -158.9, -129.7 dB
* DSP 3: program +43 words (estimate P 1+1, cycles 0+0/4); fragments: 300fdde6 (43 w, 7 patched); data X:$53-$60, Y:$53-$5F
* Lvl 1 (0..127): X:$53 table (slewed)
* Lvl 2 (0..127): X:$54 table (slewed)
* Lvl 3 (0..127): X:$55 table (slewed)
* Lvl 4 (0..127): X:$56 table (slewed)
* Lvl 5 (0..127): X:$57 table (slewed)
* Lvl 6 (0..127): X:$58 table (slewed)
* Pan 1 (0..127): X:$59 table (slewed); Y:$59 table (slewed)
* Pan 2 (0..127): X:$5A table (slewed); Y:$5A table (slewed)
* Pan 3 (0..127): X:$5B table (slewed); Y:$5B table (slewed)
* Pan 4 (0..127): X:$5C table (slewed); Y:$5C table (slewed)
* Pan 5 (0..127): X:$5D table (slewed); Y:$5D table (slewed)
* Pan 6 (0..127): X:$5E table (slewed); Y:$5E table (slewed)
* Master (0..127): X:$5F table (slewed)

### FltMulti: Filter Multi-mode (type 49, Filter, VA)
* in: In/audio, PitchVar/control, Pitch/control; out: LP/audio, BP/audio, HP/audio
* cost [C]: 30 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 34 + 32/4 cycles
* response [C] (default settings, input 0 driven): gain -0.07 / -5.36 / -39.34 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24999, 0.0; 1 kHz harmonics 2-6: -127.6, -127.3, -131.5, -139.7, -140.6 dB
* DSP 3: program +46 words (estimate P 35+33, cycles 34+32/4); fragments: 300f2a6a (12 w, 1 patched); data X:$0-$4, X:$51-$51, X:$53-$5A, Y:$51-$51, Y:$53-$58
* Freq (0..127): X:$58 exp~ about 0.000450015 * 2^(v/12) (fraction), off by more than 2% at v=127 (slewed)
* Freq M (0..127) [inputs connected]: X:$57 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* KBT (0..4): P:$236 code
* GComp (0..1): P:$240 code
* Res (0..127): Y:$57 linear -0.0077734*v +1.00183 (fraction) (slewed)
* dB/Oct (0..1): P:$2B1 code; P:$2B3 code; P:$2B4 code; P:$2B6 code
* On/Off (0..1): P:$2B1 code; P:$2B3 code; P:$2B4 code; P:$2B6 code

### ConstSwT: Constant Switch Toggling (type 50, Level, VA)
* in: -; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* DSP 3: program +4 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Value (0..127): X:$53 linear 0.00392247*v -0.250627 (fraction) (slewed)
* State (0..1): P:$237 code
* Pol (0..1): no DSP word changes seen

### FltNord: Filter Nord (type 51, Filter, VA)
* in: In/audio, PitchVar/control, Pitch/control, FMLin/control, Res/control; out: Out/audio
* cost [C]: 39 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 60 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.15 / -10.71 / -78.67 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24999; 1 kHz harmonics 2-6: -163.3, -124.3, -163.3, -133.1, -163.3 dB
* DSP 3: program +62 words (estimate P 61+0, cycles 60+0/4); fragments: 300f2a1a (19 w, 3 patched); data X:$0-$4, X:$51-$51, X:$53-$5C, Y:$51-$51, Y:$53-$5E
* Freq (0..127): X:$5A exp~ about 0.000450015 * 2^(v/12) (fraction), off by more than 2% at v=127 (slewed)
* Pitch M (0..127) [inputs connected]: X:$57 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* KBT (0..4): P:$236 code
* GComp (0..1): P:$248 code
* Res (0..127): Y:$5B linear 0.00196298*v -0.000461033 (fraction) (slewed)
* dB/Oct (0..1): P:$247 code; P:$2B5 code; P:$2B6 code; P:$2C3 code; P:$2C4 code; P:$2C6 code; P:$2C7 code
* On/Off (0..1): P:$247 code; P:$2B5 code; P:$2B6 code; P:$2C3 code; P:$2C4 code; P:$2C6 code; P:$2C7 code
* FrqLinM (0..127) [inputs connected]: Y:$5A linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Type (0..3): P:$247 code; P:$2B5 code; P:$2B6 code; P:$2C3 code; P:$2C4 code; P:$2C6 code; P:$2C7 code; Y:$5D one word per choice
* Res M (0..127): Y:$5A linear 0.00785192*v -0.0018441 (fraction) (slewed)

### EnvMulti: Envelope Multi (type 52, Env, VA)
* in: Gate/logic, In/audio, AM/control; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 4 + 45/4 cycles
* DSP 3: program +55 words (estimate P 0+0, cycles 4+45/4); fragments: 300f0826 (55 w, 5 patched); data X:$53-$76, Y:$53-$58
* Level 1 (0..127): X:$5E linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Level 2 (0..127): X:$65 linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Level 3 (0..127): X:$6C linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Level 4 (0..127): X:$55 linear 0.00785192*v -0.0018441 (fraction) (slewed); X:$73 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Time 1 (0..127): X:$5C table; X:$5D exp 0.496276 * 2^(v/8594) (fraction); X:$5F exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=16
* Time 2 (0..127): X:$63 table; X:$64 exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$66 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Time 3 (0..127): X:$6A table; X:$6B exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$6D exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Time 4 (0..127): X:$71 table; X:$72 exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$74 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Reset (0..1): P:$241 code
* SusPlac (0..3): P:$247 code; X:$60 one word per choice; X:$67 one word per choice; X:$6E one word per choice; Y:$53 one word per choice; Y:$54 one word per choice
* OutType (0..4): X:$58 one word per choice; X:$59 one word per choice
* KBG (0..1): P:$238 code
* Shape (0..3): P:$259 code; X:$5C one word per choice; X:$5D one word per choice; X:$61 one word per choice; X:$63 one word per choice; X:$64 one word per choice; X:$68 one word per choice; X:$6A one word per choice; X:$6B one word per choice; X:$6F one word per choice; X:$71 one word per choice; X:$72 one word per choice; X:$76 one word per choice

### S&H: Sample & Hold (type 53, Switch, VA)
* in: In/audio, Ctrl/logic; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +10 words (estimate P 1+1, cycles 12+0/4); fragments: 300fee66 (10 w, 2 patched); data X:$53-$53, Y:$53-$53

### FltStatic: Filter Static (type 54, Filter, VA)
* in: In/audio; out: Out/audio
* cost [C]: 19 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.07 / -5.36 / -39.34 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24999; 1 kHz harmonics 2-6: -168.6, -127.9, -168.6, -131.3, -168.6 dB
* DSP 3: program +19 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$0-$4, X:$50-$56, Y:$50-$51, Y:$53-$56
* Freq (0..127): X:$52 exp~ about 1.02986 * 2^(v/-668.9) (fraction), off by more than 2% at v=0,95,111,127 (slewed); Y:$50 exp~ about 0.000450015 * 2^(v/12) (fraction), off by more than 2% at v=127 (slewed)
* Res (0..127): X:$52 table (slewed)
* Type (0..2): P:$29B code; P:$29C code; X:$50 one word per choice
* On/Off (0..1): P:$29B code; P:$29C code
* GComp (0..1): no DSP word changes seen

### EnvD: Envelope Decay (type 55, Env, VA)
* in: Trig/logic, AM/control, In/audio; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* DSP 3: program +15 words (estimate P 1+1, cycles 12+0/4); fragments: 300fa37e (15 w, 2 patched); data X:$53-$57, Y:$53-$53
* Decay (0..127): X:$53 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16 (slewed)
* OutType (0..3): X:$56 one word per choice; X:$57 one word per choice

### Automate: MIDI Control Automate (type 57, MIDI, VA)
* in: In/logic; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +1 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$53-$53
* Control (0..127): no DSP word changes seen
* Value (0..127): X:$53 linear 1.19209e-07*v +0 (fraction)
* Channel (0..20): no DSP word changes seen
* Echo (0..1): no DSP word changes seen

### DrumSynth: Drum Synthesizer (type 58, Osc, VA)
* in: Trig/logic, Vel/control, Pitch/control; out: Out/audio
* cost [C]: 72 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 71 + 100/4 cycles
* DSP 3: program +173 words (estimate P 0+0, cycles 71+100/4); fragments: 30101d98 (101 w, 4 patched), 30101d70 (72 w, 4 patched); data X:$50-$75, Y:$50-$6C
* MstTune (0..127): X:$6C exp 0.00130995 * 2^(v/24) (fraction) (slewed)
* SlvTune (0..127): Y:$68 exp 0.00781249 * 2^(v/48) (fraction) (slewed)
* MstDcy (0..127): X:$6D exp~ about 0.991204 * 2^(v/2.306e+04) (fraction), off by more than 2% at v=0,16 (slewed)
* SlvDcy (0..127): X:$71 exp~ about 0.991204 * 2^(v/2.306e+04) (fraction), off by more than 2% at v=0,16 (slewed)
* MstLvl (0..127): X:$75 table (slewed)
* SlvLvl (0..127): X:$73 table (slewed)
* FltFreq (0..127): X:$58 exp~ about 0.000674248 * 2^(v/12) (fraction), off by more than 2% at v=127 (slewed)
* FltRes (0..127): Y:$54 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* FltSwp (0..127): Y:$53 linear 0.00784494*v -0.00125403 (fraction) (slewed)
* FltDcy (0..127): X:$6A exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16 (slewed)
* FltType (0..2): P:$323 code; P:$32F code
* BendAmt (0..127): Y:$5F table (slewed)
* BendDcy (0..127): Y:$62 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16 (slewed)
* Click (0..127): Y:$60 table (slewed)
* Noise (0..127): X:$57 table (slewed)
* On/Off (0..1): P:$336 code

### CompLev: Compare to Level (type 59, Level, VA)
* in: In/control; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 5/4 cycles
* response [C] (default settings, input 0 driven): gain -9.94 / -9.95 / -9.96 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -29.7, -9.6, -29.7, -14.1, -29.7 dB
* DSP 3: program +5 words (estimate P 0+0, cycles 0+5/4); fragments: 300ff356 (5 w, 1 patched); data X:$53-$53, Y:$53-$53
* Level (0..127): X:$53 linear 0.00392247*v -0.250627 (fraction) (slewed)

### Mux8-1X: Multiplexer 8-1 with variable X-Fade (type 60, Switch, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio, In7/audio, In8/audio, Ctrl/control; out: Out/audio
* cost [C]: 15 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 15 + 52/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +72 words (estimate P 1+1, cycles 15+52/4); fragments: 300fe70c (57 w, 1 patched); data X:$51-$51, X:$53-$60, Y:$51-$51, Y:$53-$5A
* X-Fade (0..127): X:$5A linear 0.000240326*v -0.0326824 (fraction) (slewed); X:$5B linear 0.000480652*v -0.0653648 (fraction) (slewed); X:$5C linear 0.000720978*v -0.0980473 (fraction) (slewed); X:$5D linear 0.000961304*v -0.13073 (fraction) (slewed); X:$5E linear 0.00120163*v -0.163412 (fraction) (slewed); X:$5F linear 0.00144196*v -0.196095 (fraction) (slewed); X:$60 linear 0.00168228*v -0.228777 (fraction) (slewed); Y:$57 linear -0.00683594*v +0.929634 (fraction) (slewed); Y:$59 linear -0.00012207*v +0.0175772 (fraction) (slewed)

### Clip: Clip (type 61, Shaper, VA)
* in: In/audio, Mod/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 15/4 cycles
* response [C] (default settings, input 0 driven): gain -1.89 / -1.89 / -1.89 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -15.3, -21.3, -35.3, -35.3, -34.1 dB
* DSP 3: program +13 words (estimate P 0+16, cycles 0+15/4); fragments: 300fa6aa (13 w, 2 patched); data X:$53-$53, Y:$53-$54
* Clip M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Clip (0..127): X:$53 linear -0.00195312*v +0.25 (fraction) (slewed)
* Shape (0..1): P:$23F code
* On/Off (0..1): P:$240 code

### Overdrive: Overdrive (type 62, Shaper, VA)
* in: In/audio, Mod/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 25 + 25/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -143.7, -160.1, -155.4, -174.0 dB
* DSP 3: program +96 words (estimate P 0+0, cycles 25+25/4); fragments: 300f51d4 (70 w, 2 patched), 300f51fc (26 w, 1 patched); data X:$53-$5E, Y:$53-$5D
* Drive M (0..127): X:$5B linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Drive (0..127): Y:$5C linear 0.00196298*v -0.000461005 (fraction) (slewed)
* On/Off (0..1): P:$278 code
* Type (0..3): X:$5E one word per choice; Y:$55 one word per choice; Y:$56 one word per choice; Y:$57 one word per choice; Y:$58 one word per choice; Y:$5D one word per choice
* Shape (0..1): X:$55 one word per choice; Y:$54 one word per choice

### Scratch: Scratch (type 63, FX, VA)
* in: In/audio, Mod/control; out: Out/audio
* cost [C]: 75 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 1 + 1/4 cycles
* response [C] (default settings, input 0 driven): gain -3.48 / 1.55 / -1.71 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → -0.31916; 1 kHz harmonics 2-6: -151.3, -145.5, -157.7, -158.1, -159.8 dB
* DSP 3: program +121 words (estimate P 1+1, cycles 1+1/4); fragments: 300f6316 (16 w, 2 patched), 300f62ee (27 w, 0 patched); data X:$50-$67, Y:$51-$60
* P Ratio (0..127): Y:$5A linear 0.00196124*v -0.125314 (fraction) (slewed)
* Ratio M (0..127): X:$60 linear -0.00785192*v +0.00184413 (fraction) (slewed)
* Delay (0..3): P:$243 code; X:$65 one word per choice
* Active (0..1): P:$302 code

### Gate: Gate (type 64, Logic, VA)
* in: In1_1/logic, In1_2/logic, In2_1/logic, In2_2/logic; out: Out1/logic, Out2/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +20 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data Y:$53-$54
* mode GateMode1: 0→0 P/0 XY words on -, 1→382 P/20 XY words on 0,1,2,3, 2→382 P/20 XY words on 0,1,2,3, 3→382 P/20 XY words on 0,1,2,3, 4→382 P/20 XY words on 0,1,2,3, 5→382 P/20 XY words on 0,1,2,3
* mode GateMode2: 0→0 P/4 XY words on 0,1,2,3, 1→382 P/20 XY words on 0,1,2,3, 2→382 P/20 XY words on 0,1,2,3, 3→382 P/20 XY words on 0,1,2,3, 4→382 P/20 XY words on 0,1,2,3, 5→382 P/20 XY words on 0,1,2,3

### Mix2-1B: Mixer 2-1 B (type 66, Mixer, VA)
* in: In1/audio, In2/audio, Chain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +6 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53, Y:$53-$53
* Inv 1 (0..1): P:$237 code
* Gain 1 (0..127): X:$53 table (slewed)
* Inv 2 (0..1): P:$238 code
* Gain 2 (0..127): Y:$53 table (slewed)
* Lin/Exp (0..2): X:$53 one word per choice; Y:$53 one word per choice

### ClkGen: Clock Generator (type 68, LFO, VA)
* in: Reset/logic; out: 1/96/logic, 1/16/logic, ClkActive/logic, Sync/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +68 words (estimate P 1+1, cycles 0+0/4); fragments: 300f8c62 (68 w, 5 patched); data X:$53-$5F, Y:$53-$5B
* Tempo (0..127): Y:$55 table
* On/Off (0..1): P:$24A code; P:$24B code; P:$251 code; P:$253 code; P:$256 code
* Source (0..1): P:$24A code; P:$251 code; P:$253 code; P:$256 code
* Sync (0..5): X:$57 one word per choice; Y:$54 one word per choice; Y:$58 one word per choice
* Swing (0..127): Y:$59 linear 0.00390625*v -0.95 (fraction); Y:$5B linear 0.00390625*v -1 (fraction)

### ClkDiv: Clock Divider (type 69, Logic, VA)
* in: Clk/logic, Reset/logic; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 19/4 cycles
* response [C] (default settings, input 0 driven): gain -9.94 / -9.96 / -10.62 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -23.7, -9.7, -23.7, -14.4, -23.8 dB
* DSP 3: program +19 words (estimate P 0+0, cycles 0+19/4); fragments: 300ff1d0 (19 w, 2 patched); data X:$53-$57, Y:$53-$54
* Divider (0..127): Y:$53 linear 1.19209e-07*v +1.19209e-07 (fraction)
* mode DivMode: 0→0 P/0 XY words on -, 1→382 P/26 XY words on 0,1,2,3

### EnvFollow: Envelope Follower (type 71, Level, VA)
* in: In/audio; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 18 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: 104.8, 0.0, 97.7, 0.0, 94.2 dB
* DSP 3: program +17 words (estimate P 18+0, cycles 18+0/4); fragments: 300f905a (17 w, 1 patched); data X:$53-$55, Y:$53-$54
* Attack (0..127): X:$54 table (slewed)
* Release (0..127): X:$55 table (slewed)

### NoteScaler: Note Scaler (type 72, Note, VA)
* in: In/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +4 words (estimate P 0+0, cycles 0+0/4); fragments: 300f7db4 (4 w, 1 patched); data X:$53-$53, Y:$53-$53
* Range (0..127): X:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)

### WaveWrap: Wave Wrapper (type 74, Shaper, VA)
* in: In/audio, Mod/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24999; 1 kHz harmonics 2-6: -160.1, -158.0, -160.1, -149.5, -166.1 dB
* DSP 3: program +47 words (estimate P 0+0, cycles 0+0/4); fragments: 300faa40 (48 w, 2 patched); data X:$53-$53, Y:$53-$54
* Wrap M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* WrpGain (0..127): X:$53 linear 0.00193787*v +0.00390625 (fraction) (slewed)
* On/Off (0..1): P:$262 code

### NoteQuant: Note Quantizer (type 75, Note, VA)
* in: In/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +13 words (estimate P 0+0, cycles 0+0/4); fragments: 300f7cd8 (13 w, 1 patched); data X:$53-$54, Y:$53-$53
* Range (0..127): X:$53 linear -0.00784494*v +0.00125405 (fraction) (slewed)
* Notes (0..127): P:$240 code; X:$54 table; Y:$53 linear 1.19209e-07*v +0 (fraction)

### SwOnOffT: Switch On/Off Toggling (type 76, Switch, VA)
* in: In/audio; out: Out/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +5 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Switch (0..1): P:$237 code

### Sw1-8: Switch 1-8 (type 78, Switch, VA)
* in: In/audio; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio, Out5/audio, Out6/audio, Out7/audio, Out8/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +16 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$57, Y:$53-$56
* Dest (0..7): X:$54 one word per choice; X:$55 one word per choice; X:$56 one word per choice; X:$57 one word per choice; Y:$53 one word per choice; Y:$54 one word per choice; Y:$55 one word per choice; Y:$56 one word per choice

### Sw4-1: Switch 4-1 (type 79, Switch, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio; out: Out/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +10 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$55, Y:$53-$54
* Source (0..3): P:$238 code; P:$23C code; X:$55 one word per choice

### LevAmp: Level Amplifier (type 81, Level, VA)
* in: In/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +5 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$53-$53
* Gain (0..127): X:$53 exp~ about 0.0272047 * 2^(v/20) (fraction), off by more than 2% at v=16,79,95,111,127 (slewed)
* Type (0..1): no DSP word changes seen

### Rect: Rectifier (type 82, Shaper, VA)
* in: In/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.02 / -6.02 / -6.02 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -7.4, -154.2, -21.4, -154.9, -28.7 dB
* DSP 3: program +7 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data 
* Type (0..3): P:$238 code; P:$239 code
* On/Off (0..1): P:$23A code

### ShpStatic: Shape Static (type 83, Shaper, VA)
* in: In/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 18/4 cycles
* response [C] (default settings, input 0 driven): gain -4.11 / -4.11 / -4.11 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -169.9, -11.1, -169.9, -18.7, -169.9 dB
* DSP 3: program +16 words (estimate P 0+19, cycles 0+18/4); fragments: 300f5224 (16 w, 3 patched); data X:$53-$55, Y:$53-$53
* Curve (0..3): P:$241 code; X:$53 one word per choice; X:$54 one word per choice; X:$55 one word per choice
* On/Off (0..1): P:$243 code

### EnvADR: Envelope AD/R (type 84, Env, VA)
* in: Gate/logic, In/audio, AM/control; out: Env/control, Out/audio, End/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 5 + 25/4 cycles
* DSP 3: program +41 words (estimate P 0+0, cycles 5+25/4); fragments: 300f075e (37 w, 4 patched); data X:$53-$60, Y:$53-$59
* Shape (0..3): X:$59 one word per choice; X:$5A one word per choice; X:$5C one word per choice; X:$5D one word per choice; X:$5E one word per choice; X:$60 one word per choice
* Attack (0..127): X:$59 exp 0.496276 * 2^(v/8594) (fraction); X:$5A table
* Reset (0..1): P:$241 code
* Dcy/Rel (0..127): X:$5D exp~ about 0.497796 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Trigged (0..1): P:$247 code (slewed); P:$24F code
* OutType (0..3): X:$56 one word per choice; X:$57 one word per choice
* KBG (0..1): P:$238 code
* Type (0..1): P:$24F code

### WindSw: Window Switch (type 85, Switch, VA)
* in: In/audio, Ctrl/control; out: Out/audio, Gate/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +11 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$54, Y:$53-$53
* From (0..127): X:$53 linear 0.00196124*v -0.000313512 (fraction)
* To (0..127): X:$54 linear 0.00196124*v -0.000313512 (fraction)

### 8Counter: 8 Counter (type 86, Logic, VA)
* in: Clk/logic, Rst/logic; out: Out1/logic, Out2/logic, Out3/logic, Out4/logic, Out5/logic, Out6/logic, Out7/logic, Out8/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -79.65 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +34 words (estimate P 1+1, cycles 12+0/4); fragments: 3010386e (34 w, 8 patched); data X:$53-$5B, Y:$53-$55

### FltLP: Filter Lowpass (type 87, Filter, VA)
* in: In/audio, Pitch/control; out: Out/audio
* cost [C]: 8 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.04 / -2.67 / -19.2 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -140.3, -143.0, -171.3, -147.3, -147.9 dB
* DSP 3: program +17 words (estimate P 1+1, cycles 12+0/4); fragments: 300f26d2 (8 w, 2 patched); data X:$0-$4, X:$51-$55, Y:$51-$53
* Freq (0..127): X:$54 exp~ about 0.000450015 * 2^(v/12) (fraction), off by more than 2% at v=127 (slewed)
* Freq M (0..127) [inputs connected]: X:$57 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* KBT (0..4): P:$236 code
* On/Off (0..1): P:$29A code
* mode SlopeMode: 0→0 P/0 XY words on -, 1→383 P/24 XY words on 0,1,2,3, 2→387 P/25 XY words on 0,1,2,3, 3→391 P/26 XY words on 0,1,2,3, 4→395 P/27 XY words on 0,1,2,3, 5→399 P/28 XY words on 0,1,2,3

### Sw1-4: Switch 1-4 (type 88, Switch, VA)
* in: In/audio; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +9 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$55, Y:$53-$54
* Dest (0..3): X:$54 one word per choice; X:$55 one word per choice; Y:$53 one word per choice; Y:$54 one word per choice

### Flanger: Flanger (type 89, FX, VA)
* in: In/audio; out: Out/audio
* cost [C]: 43 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 59 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -4.26 / -10.13 / -9.15 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.28087, 0.49897; 1 kHz harmonics 2-6: -152.2, -151.6, -148.6, -151.8, -157.4 dB
* DSP 3: program +57 words (estimate P 60+0, cycles 59+0/4); fragments: 301007c6 (14 w, 0 patched), 3010079e (43 w, 5 patched); data X:$50-$25B, Y:$50-$E0
* Rate (0..127): X:$25A linear 1.90374e-06*v +3.05156e-07 (fraction) (slewed)
* Range (0..127): Y:$DE linear 0.00671148*v -5.55112e-17 (fraction) (slewed)
* FB (0..127): Y:$54 linear 0.00657059*v -5.74708e-08 (fraction) (slewed)
* On/Off (0..1): P:$2C1 code

### Sw1-2: Switch 1-2 (type 90, Switch, VA)
* in: In/audio; out: Out2/audio, Out1/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.25; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Dest (0..1): P:$238 code

### FlipFlop: Flip Flop (type 91, Logic, VA)
* in: Clk/logic, Res/logic, In/logic; out: NotQ/logic, Q/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +17 words (estimate P 0+0, cycles 0+0/4); fragments: 300ff092 (17 w, 4 patched); data X:$53-$54, Y:$53-$54
* mode OperationMode: 0→0 P/0 XY words on -, 1→380 P/22 XY words on 0,1,2,3

### FltClassic: Filter Classic (type 92, Filter, VA)
* in: In/audio, PitchVar/control, Pitch/control; out: Out/audio
* cost [C]: 33 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 34 + 40/4 cycles
* response [C] (default settings, input 0 driven): gain -0.72 / -11.83 / -79.43 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24479; 1 kHz harmonics 2-6: -126.0, -60.4, -136.4, -134.3, -130.9 dB
* DSP 3: program +59 words (estimate P 34+41, cycles 34+40/4); fragments: 300f2b32 (22 w, 0 patched), 300f2b0a (34 w, 3 patched); data X:$0-$4, X:$51-$5A, Y:$51-$51, Y:$53-$5A
* Freq (0..127): X:$59 exp 0.000449938 * 2^(v/12) (fraction) (slewed)
* Freq M (0..127) [inputs connected]: X:$57 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* KBT (0..4): P:$236 code
* Res (0..127): X:$50 linear 0.00418091*v +5.55112e-17 (fraction) (slewed)
* dB/Oct (0..2): P:$2BF code; P:$2C2 code; P:$2C3 code
* On/Off (0..1): P:$2C4 code

### StChorus: Stereo Chorus (type 94, FX, VA)
* in: In/audio; out: OutL/audio, OutR/audio
* cost [C]: 60 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.62 / -2.83 / -9.68 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.375, 0.375; 1 kHz harmonics 2-6: -171.1, -148.0, -171.1, -154.8, -171.1 dB
* DSP 3: program +113 words (estimate P 0+0, cycles 0+0/4); fragments: 300ff90e (53 w, 0 patched), 300ff8e6 (60 w, 4 patched); data X:$50-$25C, Y:$50-$F0
* Detune (0..127): X:$25B linear 9.53674e-07*v +0 (fraction) (slewed)
* Amount (0..127): X:$52 linear -0.00392247*v +1.00063 (fraction) (slewed); X:$53 linear -0.00392247*v +1.00063 (fraction) (slewed); Y:$5B linear 0.00784494*v -0.00125403 (fraction) (slewed); Y:$5F linear 0.00784494*v -0.00125403 (fraction) (slewed)
* On/Off (0..1): P:$2E5 code; P:$2FA code

### OscD: Osc D (type 96, Osc, VA)
* in: Pitch/audio; out: Out/audio
* cost [C]: 22 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 2 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, 4e-05; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +30 words (estimate P 3+0, cycles 2+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300edcb4 (17 w, 1 patched); data X:$0-$1, X:$51-$51, X:$53-$5B, Y:$50-$51, Y:$53-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Tune Md (0..3): no DSP word changes seen
* On/Off (0..1): P:$2A7 code
* mode Wave: 0→0 P/0 XY words on -, 1→412 P/24 XY words on 0,1,2,3, 2→401 P/24 XY words on 0,1,2,3, 3→405 P/25 XY words on 0,1,2,3, 4→405 P/25 XY words on 0,1,2,3, 5→405 P/25 XY words on 0,1,2,3

### OscA: Osc A (type 97, Osc, VA)
* in: Pitch/audio, PitchVar/audio; out: Out/audio
* cost [C]: 38 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 3 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, -2e-05; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +133 words (estimate P 4+0, cycles 3+0/4); fragments: 300ed56c (8 w, 1 patched), 300edebc (61 w, 3 patched), 300edcb4 (17 w, 2 patched); data X:$0-$1, X:$51-$51, X:$53-$5B, Y:$50-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* Wave (0..5): P:$29A code; X:$52 one word per choice; X:$53 one word per choice; X:$54 one word per choice; X:$55 one word per choice; X:$56 one word per choice; Y:$52 one word per choice; Y:$53 one word per choice
* On/Off (0..1): P:$30E code
* Tune Md (0..3): no DSP word changes seen

### FreqShift: Frequency Shifter (type 98, FX, VA)
* in: In/audio, Shift/control; out: Dn/audio, Up/audio
* cost [C]: 110 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 129 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.01 / 0.01 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → -0.17161, 0.18658; 1 kHz harmonics 2-6: -111.5, -106.6, -111.4, -98.4, -107.2 dB
* DSP 3: program +126 words (estimate P 130+0, cycles 129+0/4); fragments: 30101404 (16 w, 1 patched), 301013dc (110 w, 12 patched); data X:$1-$1, X:$50-$65, Y:$51-$51, Y:$53-$68
* Shift (0..127): X:$63 linear 0.00196298*v -0.000461033 (fraction) (slewed)
* Shift M (0..127): Y:$67 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Range (0..2): Y:$68 one word per choice
* Active (0..1): P:$305 code; P:$306 code

### Sw2-1: Switch 2-1 (type 100, Switch, VA)
* in: In1/audio, In2/audio; out: Out/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +6 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Source (0..1): P:$238 code

### FltPhase: Filter Phase (type 102, Filter, VA)
* in: In/audio, PitchVar/control, Spr/control, FB/control, Pitch/control; out: Out/audio
* cost [C]: 102 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24997; 1 kHz harmonics 2-6: -108.5, -109.0, -112.9, -119.3, -122.4 dB
* DSP 3: program +135 words (estimate P 0+0, cycles 0+0/4); fragments: 300ff4aa (33 w, 3 patched), 300ff482 (102 w, 6 patched); data X:$0-$4, X:$51-$51, X:$53-$61, Y:$50-$51, Y:$53-$64
* Freq M (0..127): Y:$61 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Freq (0..127): X:$5E exp~ about 0.00654495 * 2^(v/17.36) (fraction), off by more than 2% at v=127 (slewed)
* SpreadM (0..127): X:$60 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* FB (0..127): X:$58 linear 0.0156899*v -1.00251 (fraction) (slewed)
* Notch (0..5): P:$2C3 code; P:$2CE code; P:$2D9 code; P:$2E4 code; P:$2EF code; P:$2FA code
* Spread (0..127): Y:$64 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* On/Off (0..1): P:$30E code
* In Lvl (0..127): Y:$51 table (slewed)
* FB M (0..127): Y:$59 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Type (0..2): Y:$5B one word per choice (slewed)
* KBT (0..4): P:$242 code

### EqPeak: Eq Peak (type 103, Filter, VA)
* in: In/audio; out: Out/audio
* cost [C]: 34 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 23 + 13/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -148.0, -149.7, -148.0, -140.6, -148.0 dB
* DSP 3: program +34 words (estimate P 24+14, cycles 23+13/4); fragments: none matched; data X:$50-$57, Y:$51-$51, Y:$53-$55
* Freq (0..127): X:$51 exp~ about 0.000666829 * 2^(v/13.34) (fraction), off by more than 2% at v=111,127 (slewed); X:$52 exp~ about 0.0450406 * 2^(v/-1006) (fraction), off by more than 2% at v=95,111,127 (slewed)
* Gain (0..127): X:$51 exp~ about 0.0143676 * 2^(v/937.4) (fraction), off by more than 2% at v=111,127 (slewed); X:$52 exp~ about 0.330281 * 2^(v/-21.91) (fraction), off by more than 2% at v=79,95,111,127 (slewed); X:$54 table (slewed)
* BWidth (0..127): X:$51 exp 0.015071 * 2^(v/4165) (fraction) (slewed); X:$52 linear -0.000675834*v +0.0868043 (fraction) (slewed)
* On/Off (0..1): P:$2A8 code
* In Lvl (0..127): X:$50 table (slewed)

### ValSw2-1: Value Switch 2-1 (type 105, Switch, VA)
* in: In1/audio, In2/audio, Ctrl/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +9 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$54, Y:$53-$54
* Value (0..63): X:$53 table

### OscNoise: Noise oscillator (type 106, Osc, VA)
* in: Pitch/audio, PitchVar/audio, Width/audio; out: Out/audio
* cost [C]: 77 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, -3e-05; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +114 words (estimate P 0+0, cycles 0+0/4); fragments: 300ed56c (8 w, 1 patched), 300f9fcc (29 w, 1 patched), 300f8b7a (13 w, 1 patched); data X:$0-$1, X:$50-$67, Y:$50-$51, Y:$53-$66
* Coarse (0..127): Y:$5E exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$61 exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* Tune Md (0..3): no DSP word changes seen
* Width M (0..127): Y:$61 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Width (0..127): Y:$62 linear 0.00196298*v -0.000461033 (fraction) (slewed)
* On/Off (0..1): P:$2BB code

### Vocoder: Vocoder (type 108, Filter, VA)
* in: Ctrl/audio, In/audio; out: Out/audio
* cost [C]: 96 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 96 + 1156/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +1254 words (estimate P 1258+0, cycles 96+1156/4); fragments: 300fb2a0 (1156 w, 33 patched), 300fb278 (98 w, 3 patched); data X:$51-$51, X:$53-$1B0, Y:$50-$51, Y:$53-$1DB
* Band 1 (0..16): P:$696 code
* Band 2 (0..16): P:$698 code
* Band 3 (0..16): P:$69A code
* Band 4 (0..16): P:$69C code
* Band 5 (0..16): P:$69E code
* Band 6 (0..16): P:$6A0 code
* Band 7 (0..16): P:$6A2 code
* Band 8 (0..16): P:$6A4 code
* Band 9 (0..16): P:$6A6 code
* Band 10 (0..16): P:$6A8 code
* Band 11 (0..16): P:$6AA code
* Band 12 (0..16): P:$6AC code
* Band 13 (0..16): P:$6AE code
* Band 14 (0..16): P:$6B0 code
* Band 15 (0..16): P:$6B2 code
* Band 16 (0..16): P:$6B4 code
* Emphas (0..1): P:$241 code
* Monitor (0..1): P:$76F code

### LevAdd: Level Add (type 112, Level, VA)
* in: In/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.375; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +4 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$53-$53
* Offset (0..127): X:$53 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* Pol (0..1): X:$53 one word per choice (slewed)

### Fade1-2: Fader 1-2 (type 113, Mixer, VA)
* in: In/audio, Mod/audio; out: OutL/audio, OutR/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +10 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$53-$53, Y:$53-$54
* Out Lvl (0..127): X:$53 linear 0.00196124*v -0.125314 (fraction) (slewed)
* Fade M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)

### Fade2-1: Fader 2-1 (type 114, Mixer, VA)
* in: In1/audio, In2/audio, Mod/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +9 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$53-$53, Y:$53-$53
* In Lvl (0..127): X:$53 linear 0.00196124*v -0.125314 (fraction) (slewed)
* Fade M (0..127): Y:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)

### LevScaler: Level Scaler (type 115, Note, VA)
* in: Note/control, In/audio; out: Level/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +23 words (estimate P 0+0, cycles 0+0/4); fragments: 300f7a34 (23 w, 1 patched); data X:$0-$0, X:$53-$56, Y:$53-$55
* L.Gain (0..127): Y:$53 linear -6.41926e-07*v +4.10223e-05 (fraction) (slewed)
* BrkPnt (0..127): X:$53 linear 0.00392247*v -0.250627 (fraction)
* R.Gain (0..127): Y:$54 linear 6.41926e-07*v -4.10223e-05 (fraction) (slewed)
* KBT (0..1): P:$237 code

### Mix8-1A: Mixer 8-1 A (type 116, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio, In7/audio, In8/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data Y:$53-$53
* Pad (0..2): P:$237 code

### LevMod: Level Modulator (type 117, Level, VA)
* in: In/audio, Mod/audio, ModDepth/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.02 / -6.02 / -6.02 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.125; 1 kHz harmonics 2-6: -168.0, -150.4, -168.0, -139.1, -168.0 dB
* DSP 3: program +15 words (estimate P 0+0, cycles 0+0/4); fragments: 30101ae4 (15 w, 3 patched); data X:$53-$53, Y:$53-$54
* Depth M (0..127): Y:$53 linear 0.0078125*v +0 (fraction) (slewed)
* Depth (0..127): X:$53 linear 0.00196124*v -0.125314 (fraction) (slewed)

### Digitizer: Digitizer (type 118, FX, VA)
* in: In/audio, Rate/control; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 27 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.08 / -10.74 / -27.85 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -52.1, -33.0, -45.1, -35.9, -51.9 dB
* DSP 3: program +29 words (estimate P 0+0, cycles 27+0/4); fragments: 301018e0 (29 w, 3 patched); data X:$53-$56, Y:$53-$57
* Bits (0..12): X:$56 table
* Rate (0..127): Y:$53 linear 0.00196124*v -0.125314 (fraction) (slewed)
* Rate M (0..127): X:$53 linear 0.00785192*v -0.0018441 (fraction)
* On/Off (0..1): P:$250 code

### EnvADDSR: Envelope ADBDSR (type 119, Env, VA)
* in: Gate/logic, AM/control, In/audio; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +55 words (estimate P 1+1, cycles 0+0/4); fragments: 300f0826 (55 w, 5 patched); data X:$53-$76, Y:$53-$58
* KBG (0..1): P:$238 code
* Shape (0..3): P:$259 code; X:$5C one word per choice; X:$5D one word per choice; X:$61 one word per choice; X:$63 one word per choice; X:$64 one word per choice; X:$68 one word per choice; X:$6A one word per choice; X:$6B one word per choice; X:$6F one word per choice; X:$71 one word per choice; X:$72 one word per choice; X:$76 one word per choice
* Attack (0..127): X:$5C table; X:$5D exp 0.496276 * 2^(v/8594) (fraction); X:$5F exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=16
* Decay 1 (0..127): X:$63 table; X:$64 exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$66 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Break 1 (0..127): X:$65 linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Decay 2 (0..127): X:$6A table; X:$6B exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$6D exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* Break 2 (0..127): X:$6C linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Release (0..127): X:$71 table; X:$72 exp~ about 0.492088 * 2^(v/7.65e+04) (fraction), off by more than 2% at v=0; X:$74 exp~ about 0.995592 * 2^(v/4.614e+04) (fraction), off by more than 2% at v=0,16
* SusPlac (0..1): X:$67 one word per choice; X:$6E one word per choice; Y:$53 one word per choice; Y:$54 one word per choice
* OutType (0..5): X:$58 one word per choice; X:$59 one word per choice; Y:$58 one word per choice
* Reset (0..1): P:$241 code

### SeqNote: Sequencer Note (type 121, Seq, VA)
* in: Clk/logic, Rst/logic, Loop/logic, Park/logic, Note/control, Trig/logic, RecVal/control, RecEnable/logic; out: Link/logic, Note/control, Trig/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -79.65 / -64.08 / -43.95 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +72 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$0-$0, X:$53-$61, Y:$53-$7E
* Step 1 (0..127): Y:$5A linear 0.00196124*v -0.000313512 (fraction)
* Step 2 (0..127): Y:$5C linear 0.00196124*v -0.000313512 (fraction)
* Step 3 (0..127): Y:$5E linear 0.00196124*v -0.000313512 (fraction)
* Step 4 (0..127): Y:$60 linear 0.00196124*v -0.000313512 (fraction)
* Step 5 (0..127): Y:$62 linear 0.00196124*v -0.000313512 (fraction)
* Step 6 (0..127): Y:$64 linear 0.00196124*v -0.000313512 (fraction)
* Step 7 (0..127): Y:$66 linear 0.00196124*v -0.000313512 (fraction)
* Step 8 (0..127): Y:$68 linear 0.00196124*v -0.000313512 (fraction)
* Step 9 (0..127): Y:$6A linear 0.00196124*v -0.000313512 (fraction)
* Step 10 (0..127): Y:$6C linear 0.00196124*v -0.000313512 (fraction)
* Step 11 (0..127): Y:$6E linear 0.00196124*v -0.000313512 (fraction)
* Step 12 (0..127): Y:$70 linear 0.00196124*v -0.000313512 (fraction)
* Step 13 (0..127): Y:$72 linear 0.00196124*v -0.000313512 (fraction)
* Step 14 (0..127): Y:$74 linear 0.00196124*v -0.000313512 (fraction)
* Step 15 (0..127): Y:$76 linear 0.00196124*v -0.000313512 (fraction)
* Step 16 (0..127): Y:$78 linear 0.00196124*v -0.000313512 (fraction)
* Evnt 1 (0..1): no DSP word changes seen
* Evnt 2 (0..1): no DSP word changes seen
* Evnt 3 (0..1): no DSP word changes seen
* Evnt 4 (0..1): no DSP word changes seen
* Evnt 5 (0..1): no DSP word changes seen
* Evnt 6 (0..1): no DSP word changes seen
* Evnt 7 (0..1): no DSP word changes seen
* Evnt 8 (0..1): no DSP word changes seen
* Evnt 9 (0..1): no DSP word changes seen
* Evnt 10 (0..1): no DSP word changes seen
* Evnt 11 (0..1): no DSP word changes seen
* Evnt 12 (0..1): no DSP word changes seen
* Evnt 13 (0..1): no DSP word changes seen
* Evnt 14 (0..1): no DSP word changes seen
* Evnt 15 (0..1): no DSP word changes seen
* Evnt 16 (0..1): no DSP word changes seen
* Cycles (0..1): P:$24C code
* Length (0..15): X:$57 linear 1.19209e-07*v +1.19209e-07 (fraction); X:$5A table; X:$5B linear 1.19209e-07*v +0 (fraction)
* Pulse (0..1): P:$276 code
* Random (0..1): no DSP word changes seen
* Clear (0..1): no DSP word changes seen

### Mix4-1C: Mixer 4-1 C (type 123, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, Chain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data Y:$53-$53
* Level 1 (0..127): no DSP word changes seen
* Level 2 (0..127) [inputs connected]: Y:$55 table (slewed)
* Level 3 (0..127) [inputs connected]: X:$58 table (slewed)
* Level 4 (0..127) [inputs connected]: Y:$56 table (slewed)
* On 1 (0..1) [inputs connected]: P:$313 code
* On 2 (0..1) [inputs connected]: P:$314 code
* On 3 (0..1) [inputs connected]: P:$316 code
* On 4 (0..1) [inputs connected]: P:$317 code
* Pad (0..1): P:$237 code
* Lin/Exp (0..2) [inputs connected]: X:$57 one word per choice; X:$58 one word per choice; Y:$55 one word per choice; Y:$56 one word per choice

### Mux8-1: Multiplexer 8-1 (type 124, Switch, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio, In7/audio, In8/audio, Ctrl/control; out: Out/audio
* cost [C]: 15 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +53 words (estimate P 1+1, cycles 12+0/4); fragments: 300f0d44 (38 w, 1 patched); data X:$51-$51, X:$53-$5F, Y:$51-$51, Y:$53-$59

### WahWah: Wah-Wah (type 125, Filter, VA)
* in: In/audio, Sweep/control; out: Out/audio
* cost [C]: 34 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -17.78 / 4.24 / -22.53 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: -145.2, -126.9, -151.5, -147.7, -174.1 dB
* DSP 3: program +34 words (estimate P 0+0, cycles 0+0/4); fragments: 3010062e (34 w, 4 patched); data X:$50-$58, Y:$51-$58
* Sweep M (0..127): Y:$50 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Sweep (0..127): X:$50 linear 0.00196124*v -0.000313512 (fraction) (slewed)
* On/Off (0..1): P:$2AB code

### Fx-In: Fx Input (type 127, In/Out, FX)
* in: -; out: OutL/audio, OutR/audio
* cost [C]: 18 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 21 + 0/4 cycles
* DSP 3: program +19 words (estimate P 0+0, cycles 21+0/4); fragments: none matched; data X:$51-$53, Y:$50-$50, Y:$52-$56
* Bus (0..1): no DSP word changes seen
* On/Off (0..1): P:$294 code; P:$299 code
* Pad (0..3): Y:$52 one word per choice

### MinMax: Min/Max Compare (type 128, Level, VA)
* in: A/audio, B/audio; out: Min/audio, Max/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 7/4 cycles
* response [C] (default settings, input 0 driven): gain -6.02 / -6.02 / -6.02 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.25; 1 kHz harmonics 2-6: -7.4, -154.2, -21.4, -154.9, -28.7 dB
* DSP 3: program +7 words (estimate P 0+8, cycles 0+7/4); fragments: none matched; data 

### BinCounter: Binary Counter (type 130, Logic, VA)
* in: Clk/logic, Rst/logic; out: Out001/logic, Out002/logic, Out004/logic, Out008/logic, Out016/logic, Out032/logic, Out064/logic, Out128/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +36 words (estimate P 1+1, cycles 12+0/4); fragments: 3010398c (36 w, 9 patched); data X:$53-$54, Y:$53-$55

### ADConv: A/D Converter (type 131, Logic, VA)
* in: In/audio; out: D0/logic, D1/logic, D2/logic, D3/logic, D4/logic, D5/logic, D6/logic, D7/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -11.07 / -10.71 / -10.57 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.25; 1 kHz harmonics 2-6: -28.9, -18.8, -28.9, -19.1, -28.9 dB
* DSP 3: program +31 words (estimate P 0+0, cycles 12+0/4); fragments: none matched; data X:$53-$53, Y:$53-$53

### DAConv: D/A Converter (type 132, Logic, VA)
* in: D0/logic, D1/logic, D2/logic, D3/logic, D4/logic, D5/logic, D6/logic, D7/logic; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -52.09 / -52.09 / -52.1 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.00195; 1 kHz harmonics 2-6: -29.7, -9.6, -29.7, -14.1, -29.7 dB
* DSP 3: program +29 words (estimate P 0+0, cycles 12+0/4); fragments: none matched; data X:$53-$53

### FltHP: Filter Highpass (type 134, Filter, VA)
* in: In/audio, Pitch/control; out: Out/audio
* cost [C]: 12 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -13.21 / -0.79 / -0.01 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: -135.3, -137.1, -148.0, -136.0, -148.3 dB
* DSP 3: program +25 words (estimate P 1+1, cycles 0+0/4); fragments: 300f297a (9 w, 0 patched); data X:$0-$4, X:$51-$51, X:$53-$57, Y:$53-$53
* Freq (0..127): X:$56 exp~ about 0.000450015 * 2^(v/12) (fraction), off by more than 2% at v=127 (slewed)
* Freq M (0..127) [inputs connected]: X:$5F linear 0.00785192*v -0.0018441 (fraction) (slewed)
* KBT (0..4): P:$236 code
* On/Off (0..1): P:$2A0 code
* mode SlopeMode: 0→0 P/0 XY words on -, 1→394 P/26 XY words on 0,1,2,3, 2→401 P/27 XY words on 0,1,2,3, 3→408 P/28 XY words on 0,1,2,3, 4→415 P/29 XY words on 0,1,2,3, 5→422 P/30 XY words on 0,1,2,3

### T&H: Track & Hold (type 139, Switch, VA)
* in: In/audio, Ctrl/logic; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53

### Mix4-1S: Mixer 4-1 Stereo (type 140, Mixer, VA)
* in: In1L/audio, In1R/audio, In2L/audio, In2R/audio, In3L/audio, In3R/audio, In4L/audio, In4R/audio, ChainL/audio, ChainR/audio; out: OutL/audio, OutR/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228, 0.0; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +13 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data Y:$53-$54
* Level 1 (0..127) [inputs connected]: X:$57 table (slewed)
* Level 2 (0..127): no DSP word changes seen
* Level 3 (0..127) [inputs connected]: X:$59 table (slewed)
* Level 4 (0..127) [inputs connected]: X:$5A table (slewed)
* On 1 (0..1) [inputs connected]: P:$314 code; P:$315 code
* On 2 (0..1) [inputs connected]: P:$317 code; P:$318 code
* On 3 (0..1) [inputs connected]: P:$31A code; P:$31B code
* On 4 (0..1) [inputs connected]: P:$31D code; P:$31E code
* Lin/Exp (0..2) [inputs connected]: X:$57 one word per choice; X:$58 one word per choice; X:$59 one word per choice; X:$5A one word per choice

### CtrlSend: MIDI Control Send (type 141, MIDI, VA)
* in: Send/logic, Value/control; out: Send/logic
* Control (0..127): no DSP word changes seen
* Value (0..127): no DSP word changes seen
* Channel (0..20): no DSP word changes seen

### PCSend: MIDI Program Change Send (type 142, MIDI, VA)
* in: Send/logic, Program/control; out: Send/logic
* Program (0..127) [inputs connected]: X:$5D linear 0.00195312*v +0 (fraction)
* Channel (0..16): no DSP word changes seen

### NoteSend: MIDI Note Send (type 143, MIDI, VA)
* in: Gate/logic, Vel/control, Note/control; out: -
* cost [C]: -21 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +3 words (estimate P 1+1, cycles 0+0/4); fragments: 300f3d64 (26 w, 3 patched); data X:$51-$54, Y:$50-$54
* Vel (0..127): X:$52 linear 0.00195312*v +0 (fraction)
* Note (0..127): Y:$50 linear 0.00390625*v +0.00195312 (fraction)
* Channel (0..20): X:$53 linear 1.19209e-07*v +0 (fraction)

### SeqEvent: Sequencer Event (type 144, Seq, VA)
* in: Clk/logic, Rst/logic, Loop/logic, Park/logic, Trig1/logic, Trig2/logic; out: Link/logic, Trig1/logic, Trig2/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -79.65 / -64.08 / -43.95 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +72 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$0-$0, X:$53-$61, Y:$53-$7E
* Step 1 (0..1): no DSP word changes seen
* Step 2 (0..1): no DSP word changes seen
* Step 3 (0..1): no DSP word changes seen
* Step 4 (0..1): no DSP word changes seen
* Step 5 (0..1): no DSP word changes seen
* Step 6 (0..1): no DSP word changes seen
* Step 7 (0..1): no DSP word changes seen
* Step 8 (0..1): no DSP word changes seen
* Step 9 (0..1): no DSP word changes seen
* Step 10 (0..1): no DSP word changes seen
* Step 11 (0..1): no DSP word changes seen
* Step 12 (0..1): no DSP word changes seen
* Step 13 (0..1): no DSP word changes seen
* Step 14 (0..1): no DSP word changes seen
* Step 15 (0..1): no DSP word changes seen
* Step 16 (0..1): no DSP word changes seen
* Evnt 1 (0..1): no DSP word changes seen
* Evnt 2 (0..1): no DSP word changes seen
* Evnt 3 (0..1): no DSP word changes seen
* Evnt 4 (0..1): no DSP word changes seen
* Evnt 5 (0..1): no DSP word changes seen
* Evnt 6 (0..1): no DSP word changes seen
* Evnt 7 (0..1): no DSP word changes seen
* Evnt 8 (0..1): no DSP word changes seen
* Evnt 9 (0..1): no DSP word changes seen
* Evnt 10 (0..1): no DSP word changes seen
* Evnt 11 (0..1): no DSP word changes seen
* Evnt 12 (0..1): no DSP word changes seen
* Evnt 13 (0..1): no DSP word changes seen
* Evnt 14 (0..1): no DSP word changes seen
* Evnt 15 (0..1): no DSP word changes seen
* Evnt 16 (0..1): no DSP word changes seen
* Cycles (0..1): P:$24C code
* Length (0..15): X:$57 linear 1.19209e-07*v +1.19209e-07 (fraction); X:$5A table; X:$5B linear 1.19209e-07*v +0 (fraction)
* PulseUp (0..1): P:$275 code
* PulseLo (0..1): P:$276 code

### SeqVal: Sequencer Values (type 145, Seq, VA)
* in: Clk/logic, Rst/logic, Loop/logic, Park/logic, Val/control, Trig/logic; out: Link/logic, Val/control, Trig/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -79.65 / -64.08 / -43.94 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, -0.25; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +72 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$0-$0, X:$53-$61, Y:$53-$7E
* Step 1 (0..127): Y:$5A linear 0.00196298*v -0.000461033 (fraction)
* Step 2 (0..127): Y:$5C linear 0.00196298*v -0.000461033 (fraction)
* Step 3 (0..127): Y:$5E linear 0.00196298*v -0.000461033 (fraction)
* Step 4 (0..127): Y:$60 linear 0.00196298*v -0.000461033 (fraction)
* Step 5 (0..127): Y:$62 linear 0.00196298*v -0.000461033 (fraction)
* Step 6 (0..127): Y:$64 linear 0.00196298*v -0.000461033 (fraction)
* Step 7 (0..127): Y:$66 linear 0.00196298*v -0.000461033 (fraction)
* Step 8 (0..127): Y:$68 linear 0.00196298*v -0.000461033 (fraction)
* Step 9 (0..127): Y:$6A linear 0.00196298*v -0.000461033 (fraction)
* Step 10 (0..127): Y:$6C linear 0.00196298*v -0.000461033 (fraction)
* Step 11 (0..127): Y:$6E linear 0.00196298*v -0.000461033 (fraction)
* Step 12 (0..127): Y:$70 linear 0.00196298*v -0.000461033 (fraction)
* Step 13 (0..127): Y:$72 linear 0.00196298*v -0.000461033 (fraction)
* Step 14 (0..127): Y:$74 linear 0.00196298*v -0.000461033 (fraction)
* Step 15 (0..127): Y:$76 linear 0.00196298*v -0.000461033 (fraction)
* Step 16 (0..127): Y:$78 linear 0.00196298*v -0.000461033 (fraction)
* Evnt 1 (0..1): no DSP word changes seen
* Evnt 2 (0..1): no DSP word changes seen
* Evnt 3 (0..1): no DSP word changes seen
* Evnt 4 (0..1): no DSP word changes seen
* Evnt 5 (0..1): no DSP word changes seen
* Evnt 6 (0..1): no DSP word changes seen
* Evnt 7 (0..1): no DSP word changes seen
* Evnt 8 (0..1): no DSP word changes seen
* Evnt 9 (0..1): no DSP word changes seen
* Evnt 10 (0..1): no DSP word changes seen
* Evnt 11 (0..1): no DSP word changes seen
* Evnt 12 (0..1): no DSP word changes seen
* Evnt 13 (0..1): no DSP word changes seen
* Evnt 14 (0..1): no DSP word changes seen
* Evnt 15 (0..1): no DSP word changes seen
* Evnt 16 (0..1): no DSP word changes seen
* Cycles (0..1): P:$24C code
* Length (0..15): X:$57 linear 1.19209e-07*v +1.19209e-07 (fraction); X:$5A table; X:$5B linear 1.19209e-07*v +0 (fraction)
* Pol (0..1): P:$278 code
* Pulse (0..1): P:$276 code
* Random (0..1): no DSP word changes seen
* Clear (0..1): no DSP word changes seen

### SeqLev: Sequencer Level (type 146, Seq, VA)
* in: Clk/logic, Rst/logic, Loop/logic, Park/logic, Val/control, Trig/logic; out: Link/logic, Val/control, Trig/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -79.65 / -64.08 / -43.95 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +72 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$0-$0, X:$53-$61, Y:$53-$7E
* Step 1 (0..127): Y:$5A linear 0.00196298*v -0.000461033 (fraction)
* Step 2 (0..127): Y:$5C linear 0.00196298*v -0.000461033 (fraction)
* Step 3 (0..127): Y:$5E linear 0.00196298*v -0.000461033 (fraction)
* Step 4 (0..127): Y:$60 linear 0.00196298*v -0.000461033 (fraction)
* Step 5 (0..127): Y:$62 linear 0.00196298*v -0.000461033 (fraction)
* Step 6 (0..127): Y:$64 linear 0.00196298*v -0.000461033 (fraction)
* Step 7 (0..127): Y:$66 linear 0.00196298*v -0.000461033 (fraction)
* Step 8 (0..127): Y:$68 linear 0.00196298*v -0.000461033 (fraction)
* Step 9 (0..127): Y:$6A linear 0.00196298*v -0.000461033 (fraction)
* Step 10 (0..127): Y:$6C linear 0.00196298*v -0.000461033 (fraction)
* Step 11 (0..127): Y:$6E linear 0.00196298*v -0.000461033 (fraction)
* Step 12 (0..127): Y:$70 linear 0.00196298*v -0.000461033 (fraction)
* Step 13 (0..127): Y:$72 linear 0.00196298*v -0.000461033 (fraction)
* Step 14 (0..127): Y:$74 linear 0.00196298*v -0.000461033 (fraction)
* Step 15 (0..127): Y:$76 linear 0.00196298*v -0.000461033 (fraction)
* Step 16 (0..127): Y:$78 linear 0.00196298*v -0.000461033 (fraction)
* Evnt 1 (0..1): no DSP word changes seen
* Evnt 2 (0..1): no DSP word changes seen
* Evnt 3 (0..1): no DSP word changes seen
* Evnt 4 (0..1): no DSP word changes seen
* Evnt 5 (0..1): no DSP word changes seen
* Evnt 6 (0..1): no DSP word changes seen
* Evnt 7 (0..1): no DSP word changes seen
* Evnt 8 (0..1): no DSP word changes seen
* Evnt 9 (0..1): no DSP word changes seen
* Evnt 10 (0..1): no DSP word changes seen
* Evnt 11 (0..1): no DSP word changes seen
* Evnt 12 (0..1): no DSP word changes seen
* Evnt 13 (0..1): no DSP word changes seen
* Evnt 14 (0..1): no DSP word changes seen
* Evnt 15 (0..1): no DSP word changes seen
* Evnt 16 (0..1): no DSP word changes seen
* Cycles (0..1): P:$24C code
* Length (0..15): X:$57 linear 1.19209e-07*v +1.19209e-07 (fraction); X:$5A table; X:$5B linear 1.19209e-07*v +0 (fraction)
* Pol (0..1): P:$278 code; X:$61 one word per choice; Y:$7A one word per choice; Y:$7C one word per choice
* Pulse (0..1): P:$276 code
* Random (0..1): no DSP word changes seen
* Clear (0..1): no DSP word changes seen

### CtrlRcv: MIDI Control Receive (type 147, MIDI, VA)
* in: -; out: Rcv/logic, Val/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +4 words (estimate P 1+1, cycles 0+0/4); fragments: 300f3d8c (4 w, 1 patched); data X:$53-$53, Y:$53-$53
* Control (0..127): no DSP word changes seen
* Channel (0..16): no DSP word changes seen

### NoteRcv: MIDI Note Receive (type 148, MIDI, VA)
* in: -; out: Gate/logic, Vel/control, RelVel/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +5 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$53-$54, Y:$53-$53
* Note (0..127): no DSP word changes seen
* Channel (0..17): no DSP word changes seen

### NoteZone: MIDI Note Zone (type 149, MIDI, VA)
* in: -; out: -
* cost [C]: -21 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +-22 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data 
* RcvChan (0..17): no DSP word changes seen
* NoteMin (0..127): no DSP word changes seen
* NoteMax (0..127): no DSP word changes seen
* Transp (0..127): no DSP word changes seen
* SndChan (0..20): no DSP word changes seen
* Filter (0..1): no DSP word changes seen

### Compress: Compressor (type 150, FX, VA)
* in: InL/audio, InR/audio, SideChain/audio; out: OutR/audio, OutL/audio
* cost [C]: 65 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.25; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +65 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$51-$51, X:$53-$5A, Y:$50-$5A
* Thres (0..42): Y:$52 linear 0.0104167*v +0.5625 (fraction); Y:$57 exp~ about 0.105112 * 2^(v/-8) (fraction), off by more than 2% at v=31,32,37,42
* Ratio (0..66): Y:$54 exp~ about 0.797544 * 2^(v/196.8) (fraction), off by more than 2% at v=8,16,25; Y:$57 table
* Attack (0..127): X:$55 table (slewed)
* Release (0..127): X:$54 table (slewed)
* Output (0..42): Y:$57 exp~ about 0.00169245 * 2^(v/8.108) (fraction), off by more than 2% at v=0,5,10,16
* SideChn (0..1): P:$290 code
* On/Off (0..1): P:$2C3 code

### KeyQuant: Key Quantizer (type 152, Note, VA)
* in: In/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +42 words (estimate P 0+0, cycles 0+0/4); fragments: 301056be (42 w, 2 patched); data X:$53-$61, Y:$53-$61
* Range (0..127): X:$53 linear 0.00784494*v -0.00125453 (fraction) (slewed)
* Capture (0..1): P:$25D code
* Note E (0..1): P:$25D code
* Note F (0..1): P:$25D code
* Note F# (0..1): P:$25D code
* Note G (0..1): P:$25D code
* Note G# (0..1): P:$25D code
* Note A (0..1): P:$25D code
* Note Bb (0..1): P:$25D code
* Note B (0..1): P:$25D code
* Note C (0..1): P:$25D code
* Note C# (0..1): P:$25D code
* Note D (0..1): P:$25D code
* Note D# (0..1): P:$25D code

### SeqCtr: Sequencer Controlled (type 154, Seq, VA)
* in: Ctrl/control, Val/control, Trig/logic; out: Val/control, Trig/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -27.51 / -28.54 / -29.85 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 11.6, 8.1, 7.0, 9.6, -5.1 dB
* DSP 3: program +38 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$53-$5A, Y:$53-$7D
* Step 1 (0..127): Y:$59 linear 0.00196298*v -0.000461033 (fraction); Y:$79 linear 0.00196298*v -0.000461033 (fraction)
* Step 2 (0..127): Y:$5B linear 0.00196298*v -0.000461033 (fraction)
* Step 3 (0..127): Y:$5D linear 0.00196298*v -0.000461033 (fraction)
* Step 4 (0..127): Y:$5F linear 0.00196298*v -0.000461033 (fraction)
* Step 5 (0..127): Y:$61 linear 0.00196298*v -0.000461033 (fraction)
* Step 6 (0..127): Y:$63 linear 0.00196298*v -0.000461033 (fraction)
* Step 7 (0..127): Y:$65 linear 0.00196298*v -0.000461033 (fraction)
* Step 8 (0..127): Y:$67 linear 0.00196298*v -0.000461033 (fraction)
* Step 9 (0..127): Y:$69 linear 0.00196298*v -0.000461033 (fraction)
* Step 10 (0..127): Y:$6B linear 0.00196298*v -0.000461033 (fraction)
* Step 11 (0..127): Y:$6D linear 0.00196298*v -0.000461033 (fraction)
* Step 12 (0..127): Y:$6F linear 0.00196298*v -0.000461033 (fraction)
* Step 13 (0..127): Y:$71 linear 0.00196298*v -0.000461033 (fraction)
* Step 14 (0..127): Y:$73 linear 0.00196298*v -0.000461033 (fraction)
* Step 15 (0..127): Y:$75 linear 0.00196298*v -0.000461033 (fraction)
* Step 16 (0..127): Y:$77 linear 0.00196298*v -0.000461033 (fraction)
* Evnt 1 (0..1): no DSP word changes seen
* Evnt 2 (0..1): no DSP word changes seen
* Evnt 3 (0..1): no DSP word changes seen
* Evnt 4 (0..1): no DSP word changes seen
* Evnt 5 (0..1): no DSP word changes seen
* Evnt 6 (0..1): no DSP word changes seen
* Evnt 7 (0..1): no DSP word changes seen
* Evnt 8 (0..1): no DSP word changes seen
* Evnt 9 (0..1): no DSP word changes seen
* Evnt 10 (0..1): no DSP word changes seen
* Evnt 11 (0..1): no DSP word changes seen
* Evnt 12 (0..1): no DSP word changes seen
* Evnt 13 (0..1): no DSP word changes seen
* Evnt 14 (0..1): no DSP word changes seen
* Evnt 15 (0..1): no DSP word changes seen
* Evnt 16 (0..1): no DSP word changes seen
* Pulse (0..1): P:$252 code
* Pol (0..1): P:$256 code
* XFade (0..3): P:$24D code
* Random (0..1): no DSP word changes seen
* Clear (0..1): no DSP word changes seen

### NoteDet: Note Detector (type 156, In/Out, VA)
* in: -; out: Gate/logic, Vel/control, RelVel/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +5 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$53-$54, Y:$53-$53
* Note (0..127): no DSP word changes seen

### LevConv: Level Converter (type 157, Level, VA)
* in: In/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +5 words (estimate P 1+1, cycles 0+0/4); fragments: 30105e46 (5 w, 1 patched); data X:$53-$53, Y:$53-$53
* OutType (0..5): X:$53 one word per choice; Y:$53 one word per choice
* InType (0..2): X:$53 one word per choice

### Glide: Glide (type 158, Note, VA)
* in: In/control, On/logic; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -24.84 / -44.83 / -64.67 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.23085; 1 kHz harmonics 2-6: -129.2, -84.3, -129.2, -90.2, -129.2 dB
* DSP 3: program +18 words (estimate P 1+1, cycles 0+0/4); fragments: 30105efe (14 w, 3 patched); data X:$53-$54, Y:$53-$58
* Time (0..127): Y:$53 table; Y:$54 table
* Active (0..1): P:$244 code; P:$245 code
* Shape (0..1): P:$23A code

### CompSig: Compare to Signal (type 159, Level, VA)
* in: A/audio, B/audio; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -9.94 / -9.95 / -9.96 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -29.7, -9.6, -29.7, -14.1, -29.7 dB
* DSP 3: program +6 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$53-$53

### ZeroCnt: Zero Crossing Counter (type 160, Note, VA)
* in: In/audio; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → -0.00018; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +29 words (estimate P 0+0, cycles 0+0/4); fragments: 300f579a (13 w, 1 patched), 300f57c2 (16 w, 0 patched); data X:$53-$5A, Y:$53-$57

### MixFader: Mixer 8-1 Fader (type 161, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio, In7/audio, In8/audio, Chain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data Y:$53-$53
* Level 1 (0..127): no DSP word changes seen
* Level 2 (0..127) [inputs connected]: Y:$55 table (slewed)
* Level 3 (0..127) [inputs connected]: X:$58 table (slewed)
* Level 4 (0..127) [inputs connected]: Y:$56 table (slewed)
* Level 5 (0..127) [inputs connected]: X:$59 table (slewed)
* Level 6 (0..127) [inputs connected]: Y:$57 table (slewed)
* Level 7 (0..127) [inputs connected]: X:$5A table (slewed)
* Level 8 (0..127) [inputs connected]: Y:$58 table (slewed)
* On 1 (0..1) [inputs connected]: P:$313 code
* On 2 (0..1) [inputs connected]: P:$314 code
* On 3 (0..1) [inputs connected]: P:$316 code
* On 4 (0..1) [inputs connected]: P:$317 code
* On 5 (0..1) [inputs connected]: P:$319 code
* On 6 (0..1) [inputs connected]: P:$31A code
* On 7 (0..1) [inputs connected]: P:$31C code
* On 8 (0..1) [inputs connected]: P:$31D code
* Lin/Exp (0..2) [inputs connected]: X:$57 one word per choice; X:$58 one word per choice; X:$59 one word per choice; X:$5A one word per choice; Y:$55 one word per choice; Y:$56 one word per choice; Y:$57 one word per choice; Y:$58 one word per choice
* Pad (0..2): P:$237 code

### FltComb: Filter Comb (type 162, Filter, VA)
* in: In/audio, Pitch/control, PitchVar/control, FB/control; out: Out/audio
* cost [C]: 95 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.03 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -160.1, -139.3, -160.1, -143.0, -160.1 dB
* DSP 3: program +106 words (estimate P 0+0, cycles 0+0/4); fragments: 300ed56c (8 w, 0 patched); data X:$0-$4, X:$51-$59, Y:$50-$5F
* Freq (0..127): Y:$5F exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* KBT (0..4): P:$236 code
* FB (0..127): X:$54 linear 0.0156899*v -1.00251 (fraction) (slewed)
* FB M (0..127): Y:$57 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Type (0..2): Y:$59 one word per choice
* Inp lvl (0..127): X:$53 table (slewed)
* On/Off (0..1): P:$2F1 code

### OscShpA: Osc Shape A (type 163, Osc, VA)
* in: Pitch/audio, PitchVar/audio, Sync/audio, FM/audio, Shape/audio; out: Out/audio
* cost [C]: 91 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 6 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, -1e-05; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +588 words (estimate P 7+0, cycles 6+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300edee4 (155 w, 1 patched), 300ede6c (14 w, 1 patched), 300ede44 (80 w, 2 patched), 300ede1c (80 w, 2 patched), 300edd2c (140 w, 6 patched); data X:$0-$4, X:$51-$51, X:$53-$5D, Y:$50-$51, Y:$53-$5A
* Coarse (0..127): Y:$5A exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5C exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* Tune Md (0..3): no DSP word changes seen
* FM M (0..127) [inputs connected]: Y:$59 table (slewed)
* FM PTrk (0..1): no DSP word changes seen
* Shape (0..127): X:$52 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Shape M (0..127) [inputs connected]: X:$67 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Wave (0..5): P:$29A code; X:$53 one word per choice; X:$54 one word per choice; X:$55 one word per choice; X:$56 one word per choice; Y:$52 one word per choice; Y:$53 one word per choice; Y:$54 one word per choice
* On/Off (0..1): P:$4D5 code

### OscDual: Osc Dual (type 164, Osc, VA)
* in: Pitch/audio, PitchVar/audio, Sync/audio, PW/audio, Phase/audio; out: Out/audio
* cost [C]: 123 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 6 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, -5e-05; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +133 words (estimate P 7+0, cycles 6+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300eddf4 (120 w, 4 patched); data X:$0-$1, X:$51-$51, X:$53-$5F, Y:$50-$51, Y:$53-$5A
* Coarse (0..127): Y:$5A exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5E exp 0.242883 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* Tune Md (0..3): no DSP word changes seen
* Sqr Lvl (0..127): X:$58 linear 0.00784494*v -0.00125403 (fraction) (slewed)
* PW M (0..127): X:$53 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Saw Lvl (0..127): X:$59 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Saw Ph (0..127): Y:$53 linear 0.00392596*v -0.250922 (fraction) (slewed)
* Sub Lvl (0..127): X:$5A linear 0.00785192*v -0.0018441 (fraction) (slewed)
* On/Off (0..1): P:$30D code
* Sqr PW (0..127): Y:$52 linear 0.00196298*v -0.000461033 (fraction) (slewed)
* Phase M (0..127): X:$54 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* SoftSqr (0..1): P:$30B code

### DXRouter: DX style router (type 165, Osc, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio, In5/audio, In6/audio; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio, Out5/audio, Out6/audio, Main/audio
* cost [C]: 31 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 15 + 52/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +31 words (estimate P 1+1, cycles 15+52/4); fragments: none matched; data X:$51-$51, X:$53-$54, Y:$51-$55
* Algortm (0..31): P:$28D code; P:$28E code; P:$28F code; P:$290 code; P:$291 code; P:$292 code; P:$293 code; P:$294 code; P:$295 code; P:$296 code; P:$297 code; P:$29B code; P:$29C code; P:$2A1 code; P:$2A2 code; P:$2A3 code; P:$2A5 code; Y:$52 table
* FeedBk (0..7): Y:$50 one word per choice

### PShift: Pitch Shifter (type 167, FX, VA)
* in: In/audio, Pitch/control; out: Out/audio
* cost [C]: 75 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 1 + 1/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → -0.25; 1 kHz harmonics 2-6: -166.1, -135.4, -166.1, -142.8, -166.1 dB
* DSP 3: program +136 words (estimate P 1+1, cycles 1+1/4); fragments: 300f629e (34 w, 2 patched), 300f62c6 (24 w, 0 patched); data X:$50-$66, Y:$51-$61
* Coarse (0..127): X:$5F exp 0.0733335 * 2^(v/47.99) (fraction) (slewed)
* Fine (0..127): Y:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* Pitch M (0..127): X:$5E linear -0.00785192*v +0.00184413 (fraction) (slewed)
* Delay (0..3): P:$254 code; X:$65 one word per choice
* Active (0..1): P:$311 code

### ModAHD: Envelope Modulation AHD (type 169, Env, VA)
* in: Trig/logic, A/control, H/control, D/control, In/audio, AM/control; out: Env/control, Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +61 words (estimate P 0+0, cycles 0+0/4); fragments: 30106d16 (61 w, 6 patched); data X:$53-$67, Y:$53-$55
* Attack (0..127): X:$5D linear 1.19209e-07*v +0 (fraction)
* Hold (0..127): X:$61 linear 1.19209e-07*v +0 (fraction)
* Decay (0..127): X:$65 linear 1.19209e-07*v +0 (fraction)
* Atk M (0..127): X:$5C linear -4.79845e-07*v +1.40696e-07 (fraction)
* Hold M (0..127): X:$60 linear -4.79845e-07*v +1.40696e-07 (fraction)
* Dcy M (0..127): X:$64 linear -4.79845e-07*v +1.40696e-07 (fraction)
* OutType (0..3): X:$59 one word per choice; X:$5A one word per choice
* KBG (0..1): P:$238 code

### 2-In: 2 Inputs (type 170, In/Out, VA)
* in: -; out: OutL/audio, OutR/audio
* cost [C]: 18 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 15 + 0/4 cycles
* DSP 3: program +19 words (estimate P 0+0, cycles 15+0/4); fragments: none matched; data X:$51-$53, Y:$50-$50, Y:$53-$56
* Source (0..3): Y:$50 one word per choice; Y:$52 one word per choice
* On/Off (0..1): P:$294 code; P:$299 code
* Pad (0..3): Y:$52 one word per choice

### 4-In: 4 Inputs (type 171, In/Out, VA)
* in: -; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio
* cost [C]: 36 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 15 + 0/4 cycles
* DSP 3: program +38 words (estimate P 0+0, cycles 15+0/4); fragments: none matched; data X:$51-$51, X:$53-$54, Y:$50-$50, Y:$53-$5A
* Source (0..1): no DSP word changes seen
* On/Off (0..1): P:$294 code; P:$299 code; P:$2A7 code; P:$2AC code
* Pad (0..3): Y:$52 one word per choice; Y:$56 one word per choice

### DlySingleA: Delay Static (type 172, Delay, VA)
* in: In/audio; out: Out/audio
* cost [C]: 30 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24997; 1 kHz harmonics 2-6: -117.9, -127.0, -117.9, -105.3, -117.9 dB
* DSP 3: program +31 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$58, Y:$51-$58
* Time (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* mode DelayRange: 0→0 P/0 XY words on -, 1→393 P/30 XY words on 0,1,2,3, 2→393 P/30 XY words on 0,1,2,3, 3→393 P/30 XY words on 0,1,2,3, 4→393 P/30 XY words on 0,1,2,3, 5→393 P/30 XY words on 0,1,2,3, 6→393 P/30 XY words on 0,1,2,3

### DlySingleB: Delay Single (type 173, Delay, VA)
* in: In/audio, Time/audio; out: Out/audio
* cost [C]: 30 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24997; 1 kHz harmonics 2-6: -117.9, -127.0, -117.9, -105.3, -117.9 dB
* DSP 3: program +31 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$58, Y:$51-$58
* Time (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time M (0..127) [inputs connected]: X:$5C linear 0.00012207*v +0 (fraction) (slewed)
* mode DelayRange: 0→0 P/0 XY words on -, 1→393 P/26 XY words on 0,1,2,3, 2→393 P/26 XY words on 0,1,2,3, 3→393 P/26 XY words on 0,1,2,3, 4→393 P/26 XY words on 0,1,2,3, 5→393 P/26 XY words on 0,1,2,3, 6→393 P/26 XY words on 0,1,2,3

### DelayDual: Delay Dual (type 174, Delay, VA)
* in: In/audio, Time1/audio, Time2/audio; out: Out1/audio, Out2/audio
* cost [C]: 46 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.24997, 0.24997; 1 kHz harmonics 2-6: -117.9, -127.0, -117.9, -105.3, -117.9 dB
* DSP 3: program +47 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$59, Y:$51-$5B
* Time1 (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time1 M (0..127) [inputs connected]: X:$5C linear 0.00012207*v +0 (fraction) (slewed)
* Time2 (0..127): Y:$56 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time2 M (0..127) [inputs connected]: X:$5D linear 0.00012207*v +0 (fraction) (slewed)
* mode DelayRange: 0→0 P/0 XY words on -, 1→409 P/34 XY words on 0,1,2,3, 2→409 P/34 XY words on 0,1,2,3, 3→409 P/34 XY words on 0,1,2,3, 4→409 P/34 XY words on 0,1,2,3, 5→409 P/34 XY words on 0,1,2,3, 6→409 P/34 XY words on 0,1,2,3

### DelayQuad: Delay Quad (type 175, Delay, VA)
* in: In/audio, Time1/audio, Time2/audio, Time3/audio, Time4/audio; out: OutMain/audio, Out1/audio, Out2/audio, Out3/audio, Out4/audio
* cost [C]: 80 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.24997; 1 kHz harmonics 2-6: -166.1, -143.1, -166.1, -158.4, -166.1 dB
* DSP 3: program +81 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$5B, Y:$51-$61
* Time1 (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time1 M (0..127) [inputs connected]: X:$5C linear 0.00012207*v +0 (fraction) (slewed)
* Time2 (0..127): Y:$56 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time2 M (0..127) [inputs connected]: X:$5D linear 0.00012207*v +0 (fraction) (slewed)
* Time3 (0..127): Y:$59 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time3 M (0..127) [inputs connected]: X:$5E linear 0.00012207*v +0 (fraction) (slewed)
* Time4 (0..127): Y:$5C linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time4 M (0..127) [inputs connected]: X:$5F linear 0.00012207*v +0 (fraction) (slewed)
* Time Md (0..1): no DSP word changes seen
* mode DelayRange: 0→0 P/0 XY words on -, 1→443 P/42 XY words on 0,1,2,3, 2→443 P/42 XY words on 0,1,2,3, 3→443 P/42 XY words on 0,1,2,3, 4→443 P/42 XY words on 0,1,2,3, 5→443 P/42 XY words on 0,1,2,3, 6→443 P/42 XY words on 0,1,2,3

### DelayA: Delay A (type 176, Delay, VA)
* in: In/audio; out: Out/audio
* cost [C]: 70 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -158.9, -143.7, -153.2, -155.4, -154.7 dB
* DSP 3: program +71 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$60, Y:$51-$58
* Time (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* FB (0..127): X:$5D linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Flt LP (0..127): X:$56 table (slewed)
* Dry/Wet (0..127): X:$5B table (slewed); X:$5C table (slewed)
* On/Off (0..1): P:$2CC code
* Time Md (0..1): no DSP word changes seen
* mode DelayRange: 0→0 P/0 XY words on -, 1→433 P/38 XY words on 0,1,2,3, 2→433 P/38 XY words on 0,1,2,3, 3→433 P/38 XY words on 0,1,2,3

### DelayB: Delay B (type 177, Delay, VA)
* in: In/audio, FBMod/control, DryWetMod/control; out: Out/audio
* cost [C]: 70 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -158.9, -143.7, -153.2, -155.4, -154.7 dB
* DSP 3: program +71 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$60, Y:$51-$58
* Time (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* FB (0..127): X:$5D linear 0.00784494*v -0.00125403 (fraction) (slewed)
* Flt LP (0..127): X:$56 table (slewed)
* Dry/Wet (0..127): X:$5B table (slewed); X:$5C table (slewed)
* Time Md (0..1): no DSP word changes seen
* FB M (0..127) [inputs connected]: X:$65 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* DryWetM (0..127) [inputs connected]: X:$67 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* On/Off (0..1): P:$2CC code
* Flt HP (0..127): X:$58 table
* mode DelayRange: 0→0 P/0 XY words on -, 1→433 P/38 XY words on 0,1,2,3, 2→433 P/38 XY words on 0,1,2,3, 3→433 P/38 XY words on 0,1,2,3

### DlyClock: Delay Clocked (type 178, Delay, VA)
* in: In/audio, Clk/logic; out: Out/audio
* cost [C]: 15 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +42 words (estimate P 0+0, cycles 0+0/4); fragments: 300f4686 (47 w, 7 patched); data X:$51-$56, Y:$50-$D5
* Delay (0..127): Y:$51 linear 1.19209e-07*v +0 (fraction)

### DlyShiftReg: Shift Register (type 179, Delay, VA)
* in: In/audio, Clk/logic; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio, Out5/audio, Out6/audio, Out7/audio, Out8/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +35 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$53-$53, Y:$53-$5A

### Operator: FM Operator (type 180, Osc, VA)
* in: Freq/control, FM/audio, Gate/logic, Note/control, AMod/control, Vel/control, Pitch/control; out: Out/audio
* cost [C]: 26 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +145 words (estimate P 1+1, cycles 0+0/4); fragments: 300f95fe (40 w, 4 patched), 300f9626 (75 w, 4 patched), 300f96c6 (26 w, 2 patched); data X:$0-$1, X:$50-$84, Y:$51-$51, Y:$53-$64
* KBT (0..1): P:$236 code
* Sync (0..1): P:$29B code
* Tune Md (0..1): no DSP word changes seen
* Coarse (0..31): X:$59 table
* Fine (0..99): X:$59 linear 6.86722e-05*v +0.00686713 (fraction)
* Detune (0..14): X:$59 exp 0.00683931 * 2^(v/1199) (fraction)
* KeyVel (0..7): Y:$5B one word per choice
* RScale (0..7): Y:$58 one word per choice
* R1 (0..99): X:$74 linear 1.19209e-07*v +0 (fraction)
* L1 (0..99): X:$73 table
* R2 (0..99): X:$77 linear 1.19209e-07*v +0 (fraction)
* L2 (0..99): X:$76 table
* R3 (0..99): X:$7A linear 1.19209e-07*v +0 (fraction)
* L3 (0..99): X:$79 table
* R4 (0..99): X:$80 linear 1.19209e-07*v +0 (fraction)
* L4 (0..99): X:$7F table
* A-Mod (0..7): X:$70 one word per choice; Y:$63 one word per choice
* BreakP (0..99): X:$5D linear 0.00390625*v -0.183594 (fraction)
* L-Curve (0..3): P:$25A code
* L-Depth (0..99): X:$60 linear 0.00781238*v +5.55112e-17 (fraction)
* R-Curve (0..3): P:$25B code
* R-Depth (0..99): Y:$5A linear 0.00781238*v +5.55112e-17 (fraction)
* Level (0..99): Y:$57 table
* On/Off (0..1): P:$31A code
* KBG (0..1): P:$295 code

### DlyEight: Delay 8 Tap (type 181, Delay, VA)
* in: In/audio; out: Out1/audio, Out2/audio, Out3/audio, Out4/audio, Out5/audio, Out6/audio, Out7/audio, Out8/audio
* cost [C]: 70 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.25; 1 kHz harmonics 2-6: -166.1, -143.1, -166.1, -158.4, -166.1 dB
* DSP 3: program +71 words (estimate P 0+0, cycles 0+0/4); fragments: none matched; data X:$50-$58, Y:$51-$5C
* Time (0..127): Y:$53 linear 3.8147e-06*v +0 (fraction) (slewed)
* mode DelayRange: 0→0 P/0 XY words on -, 1→433 P/34 XY words on 0,1,2,3, 2→433 P/34 XY words on 0,1,2,3, 3→433 P/34 XY words on 0,1,2,3, 4→433 P/34 XY words on 0,1,2,3, 5→433 P/34 XY words on 0,1,2,3, 6→433 P/34 XY words on 0,1,2,3

### DlyStereo: Delay Stereo (type 182, Delay, VA)
* in: In/audio; out: OutL/audio, OutR/audio
* cost [C]: 148 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.25; 1 kHz harmonics 2-6: -158.9, -143.7, -153.2, -155.4, -154.7 dB
* DSP 3: program +150 words (estimate P 0+0, cycles 0+0/4); fragments: 300f4636 (10 w, 2 patched); data X:$50-$72, Y:$51-$60
* Time L (0..127): Y:$53 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* Time R (0..127): Y:$59 linear -3.05176e-05*v +0.00387573 (fraction) (slewed)
* FB L (0..127): X:$6C linear 0.00784494*v -0.00125403 (fraction) (slewed)
* FB R (0..127): X:$6D linear 0.00784494*v -0.00125403 (fraction) (slewed)
* X-FB L (0..127): Y:$5C linear 0.00785192*v -0.0018441 (fraction) (slewed)
* X-FB R (0..127): Y:$5D linear 0.00785192*v -0.0018441 (fraction) (slewed)
* Time Md (0..1): no DSP word changes seen
* Flt LP (0..127): X:$56 table (slewed); X:$64 table (slewed)
* Dry/Wet (0..127): X:$5B table (slewed); X:$5C table (slewed); X:$69 table (slewed); X:$6A table (slewed)
* On/Off (0..1): P:$2CC code; P:$312 code
* Flt HP (0..127): X:$58 table (slewed); X:$66 table (slewed)
* mode DelayRange: 0→0 P/0 XY words on -, 1→512 P/64 XY words on 0,1,2,3, 2→512 P/64 XY words on 0,1,2,3

### OscPM: Osc Phase Mod (type 183, Osc, VA)
* in: PitchVar/audio, Sync/audio, Phase M/audio, Pitch/audio; out: Out/audio
* cost [C]: 22 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, 0.00144; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +30 words (estimate P 0+0, cycles 0+0/4); fragments: 300ed56c (8 w, 1 patched), 300ed5bc (5 w, 0 patched), 300edcb4 (17 w, 1 patched); data X:$0-$1, X:$51-$51, X:$53-$5B, Y:$50-$51, Y:$53-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$5A exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* KBT (0..1): P:$236 code
* Tune Md (0..3): no DSP word changes seen
* Phase M (0..127) [inputs connected]: Y:$58 table (slewed)
* On/Off (0..1): P:$2A7 code
* Pitch M (0..127) [inputs connected]: X:$57 table (slewed)
* mode Wave: 0→0 P/0 XY words on -, 1→412 P/28 XY words on 0,1,2,3

### Mix1-1A: Mixer 1-1 A (type 184, Mixer, VA)
* in: In/audio, Chain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +5 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53, Y:$53-$53
* Level (0..127): X:$53 table (slewed)
* On (0..1): P:$237 code
* Lin/Exp (0..2): X:$53 one word per choice

### Mix1-1S: Mixer 1-1 Stereo (type 185, Mixer, VA)
* in: InL/audio, InR/audio, LChain/audio, RChain/audio; out: OutL/audio, OutR/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228, 0.0; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +8 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Level (0..127): X:$53 table (slewed)
* On (0..1): P:$238 code; P:$23A code
* Lin/Exp (0..2): X:$53 one word per choice

### Sw1-2M: Switch 1-2 Momentary (type 186, Switch, VA)
* in: In/audio; out: OutOn/audio, OutOff/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0, 0.25; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Switch (0..1): P:$238 code

### Sw2-1M: Switch 2-1 Momentary (type 187, Switch, VA)
* in: InOff/audio, InOn/audio; out: Out/audio, Ctrl/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.0; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +6 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Switch (0..1): P:$238 code

### ConstSwM: Constant Switch Momentary (type 188, Level, VA)
* in: -; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* DSP 3: program +4 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53
* Value (0..127): X:$53 linear 0.00392247*v -0.250627 (fraction) (slewed)
* State (0..1): P:$237 code
* Pol (0..1): no DSP word changes seen

### NoiseGate: Noise Gate (type 189, Level, VA)
* in: In/audio; out: Out/audio, Env/control
* cost [C]: 23 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -0.0 / -0.0 / -0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25, 0.25; 1 kHz harmonics 2-6: -166.1, -143.7, -166.1, -149.1, -160.1 dB
* DSP 3: program +65 words (estimate P 0+0, cycles 0+0/4); fragments: 300f075e (37 w, 4 patched), 301079ce (23 w, 1 patched); data X:$50-$65, Y:$51-$5B
* Thresh (0..127): X:$53 linear 0.00147093*v -0.000235134 (fraction); Y:$52 linear 0.00196124*v -0.000313512 (fraction)
* Attack (0..127): X:$5E exp~ about 0.490962 * 2^(v/5802) (fraction), off by more than 2% at v=16; X:$5F table
* Release (0..127): X:$62 exp~ about 0.490046 * 2^(v/6043) (fraction), off by more than 2% at v=0,16
* On/Off (0..1): P:$2C8 code

### LfoB: LFO B (type 190, LFO, VA)
* in: Rate/control, RateVar/control, Rst/control, Phase/control; out: Out/control, Sync/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +194 words (estimate P 0+0, cycles 0+0/4); fragments: 300f0f6a (7 w, 0 patched), 300f0fba (45 w, 5 patched), 300edcb4 (17 w, 4 patched), 300f1724 (45 w, 3 patched), 300f1364 (7 w, 1 patched); data X:$0-$8, X:$53-$5F, Y:$53-$5E
* Rate (0..127): X:$53 exp 2.12646e-05 * 2^(v/12) (fraction) (slewed)
* Rate M (0..127) [inputs connected]: X:$5C table (slewed)
* Range (0..4): P:$237 code; P:$243 code; X:$53 one word per choice
* KBT (0..4): P:$236 code
* Wave (0..3): P:$278 code; X:$56 one word per choice
* Mode (0..1): P:$236 code; P:$237 code; P:$243 code
* Phase (0..127): X:$56 table (slewed)
* On/Off (0..1): P:$2F4 code
* OutType (0..5): X:$5E one word per choice; Y:$5E one word per choice
* Phase M (0..127): Y:$56 linear 0.00785192*v -0.0018441 (fraction) (slewed)

### Phaser: Phaser (type 192, FX, VA)
* in: In/audio; out: Out/audio
* cost [C]: 56 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.84 / -0.8 / -8.6 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.27576; 1 kHz harmonics 2-6: -134.4, -132.0, -139.7, -141.3, -144.4 dB
* DSP 3: program +101 words (estimate P 0+0, cycles 0+0/4); fragments: 30107bca (6 w, 0 patched), 30107c1a (16 w, 0 patched), 30107ba2 (56 w, 5 patched); data X:$51-$51, X:$53-$62, Y:$51-$51, Y:$53-$5E
* Type (0..1): P:$25C code; P:$2EA code
* Rate (0..127): Y:$59 table
* FB (0..127): X:$50 linear 0.00695935*v -0.00411797 (fraction)
* On/Off (0..1): P:$2EE code

### Mix4-1A: Mixer 4-1 A (type 193, Mixer, VA)
* in: In1/audio, In2/audio, In3/audio, In4/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain 0.0 / 0.0 / 0.0 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -174.0, -154.2, -174.0, -154.9, -174.0 dB
* DSP 3: program +7 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data 

### Mix2-1A: Mixer 2-1 A (type 194, Mixer, VA)
* in: In1/audio, In2/audio, InChain/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -6.18 / -6.18 / -6.18 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.1228; 1 kHz harmonics 2-6: -167.8, -153.0, -153.9, -148.1, -167.8 dB
* DSP 3: program +6 words (estimate P 1+1, cycles 12+0/4); fragments: none matched; data X:$53-$53, Y:$53-$53
* Level1 (0..127): X:$53 table (slewed)
* On1 (0..1): P:$237 code
* Level2 (0..127): Y:$53 table (slewed)
* On2 (0..1): P:$238 code
* Lin/Exp (0..2): X:$53 one word per choice; Y:$53 one word per choice

### ModAmt: Modulation Amount (type 195, Level, VA)
* in: In/audio, Mod/audio; out: Out/audio
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +9 words (estimate P 1+1, cycles 0+0/4); fragments: none matched; data X:$53-$53, Y:$53-$53
* Amount (0..127): X:$53 table (slewed); Y:$53 table (slewed)
* On (0..1): P:$23C code
* Lin/Exp (0..1): no DSP word changes seen
* Mode (0..1): P:$23B code; P:$23C code

### OscPerc: Osc Percussion (type 196, Osc, VA)
* in: Pitch/audio, PitchVar/audio, Trig/audio; out: Out/audio
* cost [C]: 36 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 33 + 21/4 cycles
* response [C] (default settings, input 0 driven): gain -173.98 / -173.98 / -173.98 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.03125, 0.0; 1 kHz harmonics 2-6: 0.0, 0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +44 words (estimate P 0+0, cycles 33+21/4); fragments: 300ed56c (8 w, 0 patched), 300edf0c (36 w, 3 patched); data X:$0-$1, X:$51-$51, X:$53-$5A, Y:$50-$51, Y:$53-$58
* Coarse (0..127): Y:$58 exp 0.000340676 * 2^(v/12) (fraction) (slewed)
* Fine (0..127): X:$59 exp 0.485766 * 2^(v/1536) (fraction) (slewed)
* Tune Md (0..3): no DSP word changes seen
* KBT (0..1): P:$236 code
* Pitch M (0..127) [inputs connected]: X:$57 table
* Decay (0..127): X:$53 exp~ about 0.981908 * 2^(v/5754) (fraction), off by more than 2% at v=0,16 (slewed)
* Click (0..127): Y:$53 table (slewed); Y:$54 table (slewed)
* Punch (0..1): P:$299 code
* On/Off (0..1): P:$2B5 code

### Status: Status (type 197, In/Out, VA)
* in: -; out: PatchActive/logic, VarActive/logic, VoiceNo/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 17 + 0/4 cycles
* DSP 3: program +9 words (estimate P 16+0, cycles 17+0/4); fragments: 30108094 (9 w, 2 patched); data X:$1-$1, X:$53-$55, Y:$53-$54

### PitchTrack: Pitch tracker (type 198, Note, VA)
* in: In/audio; out: Period/logic, Pitch/control, Gate/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 12 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -9.98 / -11.74 / -11.54 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.20844, -0.00018; 1 kHz harmonics 2-6: -4.7, -18.5, -14.5, -12.1, -18.8 dB
* DSP 3: program +122 words (estimate P 1+1, cycles 12+0/4); fragments: 301079ce (23 w, 1 patched), 300f56d2 (6 w, 1 patched), 300f56fa (8 w, 0 patched), 300f5722 (23 w, 0 patched), 300f574a (24 w, 3 patched), 300f5772 (9 w, 2 patched), 300f579a (13 w, 1 patched), 300f57c2 (16 w, 0 patched); data X:$53-$6F, Y:$53-$64
* Thresh (0..127): X:$56 linear 0.00147093*v -0.000235134 (fraction); Y:$55 linear 0.00196124*v -0.000313512 (fraction)

### MonoKey: Monophonic Keyboard (type 199, In/Out, VA)
* in: -; out: Pitch/control, Gate/logic, Vel/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 17 + 0/4 cycles
* DSP 3: program +0 words (estimate P 16+0, cycles 17+0/4); fragments: none matched; data X:$1-$3, X:$5-$5
* Priorty (0..2): P:$28F code

### RandomA: Random A (type 200, Rnd, VA)
* in: Rate/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* DSP 3: program +97 words (estimate P 0+0, cycles 0+0/4); fragments: 300f0f6a (7 w, 0 patched), 300f6790 (60 w, 9 patched), 300f1364 (7 w, 1 patched); data X:$0-$8, X:$53-$5E, Y:$53-$5C
* Rate (0..127): X:$53 exp 2.12646e-05 * 2^(v/12) (fraction) (slewed)
* Mode (0..1): no DSP word changes seen
* OutType (0..2): Y:$5C one word per choice
* Range (0..3): X:$53 one word per choice
* On/Off (0..1): P:$294 code
* Edge (0..4): X:$5B one word per choice
* Step (0..3): X:$57 one word per choice; X:$58 one word per choice

### RandomB: Random B (type 202, Rnd, VA)
* in: Rate/control, RateVar/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 3 + 0/4 cycles
* DSP 3: program +97 words (estimate P 4+0, cycles 3+0/4); fragments: 300f0f6a (7 w, 0 patched), 300f6790 (60 w, 9 patched), 300f1364 (7 w, 1 patched); data X:$0-$8, X:$53-$5E, Y:$53-$5C
* Rate (0..127): X:$53 exp 2.12646e-05 * 2^(v/12) (fraction) (slewed)
* Mode (0..1): P:$236 code
* KBT (0..4): P:$236 code
* Rate M (0..127) [inputs connected]: X:$5C table (slewed)
* Step (0..127): X:$57 table (slewed); X:$58 table (slewed)
* On/Off (0..1): P:$294 code
* OutType (0..2): Y:$5C one word per choice
* Range (0..3): X:$53 one word per choice
* Edge (0..4): X:$5B one word per choice (slewed)

### RndClkA: Random Clock A (type 204, Rnd, VA)
* in: Clk/logic, Rst/logic, Seed/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -85.78 / -90.33 / -69.86 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.22746; 1 kHz harmonics 2-6: -0.0, -0.0, 0.0, -0.0, -0.0 dB
* DSP 3: program +64 words (estimate P 0+0, cycles 0+0/4); fragments: 300f6678 (8 w, 1 patched), 300f6790 (60 w, 8 patched); data X:$53-$5B, Y:$53-$59
* Step (0..127): X:$56 table (slewed); X:$57 table (slewed)
* Mode (0..1): no DSP word changes seen
* Dice (0..1): no DSP word changes seen
* OutType (0..2): Y:$59 one word per choice
* Active (0..1): P:$273 code

### RndTrig: Random Trig (type 205, Rnd, VA)
* in: Clk/logic, Rst/logic, Seed/control, Prob/control; out: Out/logic
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -17.9 / -16.32 / -15.97 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.25; 1 kHz harmonics 2-6: -29.7, -9.6, -29.7, -14.1, -29.7 dB
* DSP 3: program +55 words (estimate P 0+0, cycles 0+0/4); fragments: 300f6678 (8 w, 1 patched), 300f6880 (52 w, 10 patched); data X:$53-$5C, Y:$53-$5B
* Prob (0..127): Y:$59 linear 0.0156899*v -1.00251 (fraction) (slewed)
* Prob M (0..127): Y:$58 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* On/Off (0..1): P:$26A code
* Mode (0..1): no DSP word changes seen

### RndClkB: Random Clock B (type 206, Rnd, VA)
* in: Clk/logic, Rst/logic, Seed/control, Step/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 0/4 cycles
* response [C] (default settings, input 0 driven): gain -83.66 / -83.72 / -57.31 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → -0.08641; 1 kHz harmonics 2-6: 0.0, -0.0, 0.0, 0.0, 0.0 dB
* DSP 3: program +64 words (estimate P 0+0, cycles 0+0/4); fragments: 300f6678 (8 w, 1 patched), 300f6790 (60 w, 8 patched); data X:$53-$5B, Y:$53-$59
* Step (0..127): X:$56 table (slewed); X:$57 table (slewed)
* OutType (0..2): Y:$59 one word per choice
* On/Off (0..1): P:$273 code
* Mode (0..1): no DSP word changes seen
* Step M (0..127) [inputs connected]: Y:$60 linear 0.00785192*v -0.0018441 (fraction) (slewed)
* mode Character: 0→0 P/0 XY words on -, 1→427 P/34 XY words on 0,1,2,3

### RndPattern: Random Pattern (type 208, Rnd, VA)
* in: Clk/logic, Rst/logic, A/control, B/control, Step/control; out: Out/control
* cost [C]: 0 DSP instructions per sample (average over 4 samples, so a control-rate part counts 1/4); editor estimate 0 + 73/4 cycles
* response [C] (default settings, input 0 driven): gain -78.02 / -77.94 / -92.53 dB at 100 Hz / 1 kHz / 10 kHz; DC 0.25 → 0.20544; 1 kHz harmonics 2-6: 0.0, 0.0, -0.0, 0.0, -0.0 dB
* DSP 3: program +84 words (estimate P 0+0, cycles 0+73/4); fragments: 300f6790 (60 w, 8 patched); data X:$53-$5D, Y:$53-$5A
* Pattern (0..127): X:$53 linear 0.00392247*v -0.250627 (fraction)
* Bank (0..127): Y:$53 linear 3.06443e-05*v -0.00195802 (fraction)
* Step (0..127): X:$58 table (slewed); X:$59 table (slewed)
* Loop (0..127): Y:$54 linear -1.19209e-07*v +0 (fraction)
* Step M (0..127): no DSP word changes seen
* OutType (0..2): Y:$5A one word per choice
* On/Off (0..1): P:$287 code
* mode Wave: 0→0 P/0 XY words on -, 1→437 P/36 XY words on 0,1,2,3
