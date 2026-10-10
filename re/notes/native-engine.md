# Native engine: feasibility test (clean-room module processors vs the emulated G2)

Question: can C++ module processors, written clean-room from our module specification, sound like the G2 (here the
emulated G2 running Clavia's own OS and DSP code), and what do they cost?

Short answer: yes for the modules tried. Linear behaviour (pitch, levels, filter responses, envelope curves) matches to
hundredths of a dB. Waveform details match to about -50 to -80 dB. The CPU cost is about 1/200 of the emulator's. The
emulator itself has a control-rate timing fault, so it can't be the reference for timing (§3.3).

Measured 2026-10-09 on an M1 with the emulator of `re/notes/g2-hardware-and-emulation.md` §3.6 (OS 1.62, four
emulated DSP56367s, our protocol client on the emulated USB).

## 1. Method (clean-room)

### Sources
* **Specification:**
  * `re/notes/dsp-module-catalog.md` (formulas, parameter conversions, measured responses);
  * the module database (`data/modules.json`);
  * the editor's parameter display (`core/src/param_text.cpp`: cutoff Hz, envelope times, mixer dB);
  * public knowledge of synthesis.
* **Not used:** Clavia's DSP code, or anything derived from it:
  * fragments, OS images, DSP dumps, `.asm`/`.dis`;
  * the DSP sections of the hardware note.
  * The catalog entries used here contain no code.
* **Reference:** the emulated G2 used as a **black box**: a patch goes in and the DAC audio comes out. Nothing else is
  read from it.
  * `tools/firmware/g2blackbox.py` writes only `dac.wav`/`dac.json`: no DSP memories and no host-port streams.
  * To drive the emulator, `g2blackbox.py` imports `g2hostemu.py`'s USB/audio plumbing (`Emu`, `UsbHost`,
    `wire_audio`, `AudioRecorder`).

### Loop per module
1. Write the processor from the catalog and the display formulas.
2. Build test patches with the core's edit API (`g2mkpatch`).
3. Render them natively (`g2audio`) and on the emulator (`g2blackbox.py`).
4. Align latency and gain, then compare (`g2compare.py`).
5. Where the catalog is silent or wrong, fit a model to the black-box output: waveform residual kernels, filter
   transfer functions measured on 400 harmonics, envelope traces read sample by sample from the Env output.
6. Repeat until the residual stops improving.

### Tools (new)
| Tool | What |
|------|------|
| `tools/g2audio/g2mkpatch` | Builds a test patch from items, e.g. `osc=OscA osc.Wave=2 out=2-Out osc.0>out.0 gain=127` |
| `tools/g2audio/g2audio` | Native render; now plays `key=N@ON-OFF` notes and `repeat=R` for timing |
| `tools/firmware/g2blackbox.py` | One emulator boot for a list of jobs: upload the patch, settle, play notes, record the DACs. **Run with `nice -n 10`** |
| `tools/g2audio/g2compare.py` | Native vs emulator: lag (integer, then fractional), least-squares gain, residual, phase-blind spectral residual, harmonics, envelope |
| `emu/protobridge` `g2p_send_kbd_performance` | Uploads a performance with the keyboard on slot A (see §3.2) |

### Engine changes (generic)
* `Io` carries the keyboard (note signal, gate, velocities), FX and Bus 1–4, and the `controlTick` flag. An audio-rate
  module with a control-rate part, such as an envelope driving a VCA, steps that part on control ticks.
* `Processor::connected()` tells a processor which inputs are cabled. An unconnected AM input means full level, not 0.
* The engine applies the patch Gain setting (§3.1) to the outputs. `setKey(note, gate, velocity)` sets the keyboard,
  and the pitch keeps the last note after the gate closes.

## 2. Results per module

### How to read the numbers
* **Time-domain residual:** rms(emu − g·native) / rms(emu), after aligning lag and gain on a 0.3 s window.
  * For waveforms with aliasing, a fractional time shift can't align the alias components. Their phase depends on the
    oscillator's start phase, which the emulator does not show.
  * So for oscillators I also give the **model residual**: the native code's waveform model (the native output matches
    it to −150 dB) fitted to the emulator's output with an exact phase.
* **Spectral residual:** the same comparison on magnitude spectra (Blackman–Harris), so it ignores phase.

### Summary
| Module | Match | Audible difference? |
|--------|-------|---------------------|
| OscA pitch | Within 6 ppm (0.01 cent) for Coarse 45–127. 52/122 ppm at Coarse 20/0 (0.1–0.2 cent): the G2's phase increment is quantized and this is not modelled | No |
| OscA level | Every waveform at 1.0000 (relative to the output scaling of §3.1) | No |
| OscA Sine | Model residual −82 dB: the G2's own sine has a 5th harmonic at −82 dB, which is not modelled. Native vs emulator: −53 to −72 dB time-domain, −62 to −79 dB spectral | No |
| OscA Triangle | Model residual −100 dB. Native vs emulator: −50 to −68 dB time-domain, −59 to −78 dB spectral | No |
| OscA Saw | Model residual −67 / −64 / −58 / −52 dB at 110 / 220 / 880 / 2637 Hz. Harmonic levels within 0.05 dB | No |
| OscA Sqr50 / Sqr25 | Model residual −66 to −54 dB (110 Hz to 2.6 kHz) | No |
| OscA Sqr10 (1/16 pulse) | Model residual −63 to −54 dB up to 1 kHz, but **−29 dB at 2.6 kHz**. When the pulse is narrower than the 4-sample step, the G2 does something the model doesn't (the pulse peaks higher and narrower) | Maybe, on high notes |
| FltLP | \|H\| within 0.05 dB up to 20 kHz and phase within 0.15°, for all 6 slopes and Freq 30–127. No extra delay | No |
| EnvADSR | Shapes, sustain and curve law exact. Times: §2.3 | No, once the timing is as in §3.3 |
| Mix2-1A, Mix4-1A | Component levels within 0.0001 dB for every curve (Exp/Lin/dB), level (20/64/100/127) and On button | No |
| 2-Out, 4-Out, Pad, patch Gain | Within 0.01 dB | No |
| Keyboard | Pitch output = (note − 64)/64, exact; Gate = 1.0 | No |
| Voice (OscA → FltLP 12 dB KBT → EnvADSR, notes 64 and 76) | Sustained harmonics H2–H10 within 0.02 dB; level matches | No |

### 2.1 OscA (type 97), measured as a black box
* **Pitch:** `f = 440·2^((Coarse + (Fine−64)/128 − 69)/12)`. This is exactly the MIDI frequency: 440.003 Hz at Coarse
  69. The catalog's "0.033 % above" is a fit artifact.
* **Keyboard tracking:** KBT adds 64 × the note signal.
* **Waveform shapes:**
  * The saw rises.
  * **The pulses are DC-free:**
    * Sqr25 is +1.5 / −0.5;
    * "Sqr10" is really a **1/16 pulse**, +1.875 / −0.125.
  * Polarities are as inside the patch; the machine's output inverts them (§3.1).
* **Anti-aliasing:**
  * **Saw and pulse edges** are the ideal step convolved with a **triangle kernel of ±2 samples** (a 4-sample
    polyBLEP). I recovered this by binning the residual against the naive wave by the edge's fractional position. The
    measured residual matches (t+2)²/8 on each side.
  * **Triangle:** the two samples within one sample of a corner move inward by one phase increment. This rule fits
    the emulator to −100 dB.
  * **Sine:** computed directly, with no filtering (flat to 12.5 kHz).
* **Not measured:** the Pitch and PitchVar inputs, and the Pitch M curve (linear v/127 assumed).

### 2.2 FltLP (type 87), measured as a black box
* **Method:** the patch's saw before and after the filter, on 400 harmonics.
* **Structure:** SlopeMode s is a cascade of s+1 identical one-poles, `y += k(x − y)`.
* **Coefficient:** `k = 2·sin(π·fc/fs)`, clamped at 1, with fc = 440·2^((Freq−60)/12), the editor's display.
  * The catalog's word ≈ π·fc/fs is half of k.
  * The sine shows at the top of the range: k = 0.965 × 2π·fc/fs at Freq 120.
  * Freq 127 passes the input unchanged.
* **Gain at fc:** −2.86 dB per pole, not −3.
* **Not measured:** the KBT shares other than 100 %, Freq M and the Pitch input.

### 2.3 EnvADSR (type 20), measured from the Env output (DC passes the DAC path)
* **Attack:** reaches 1.0 in the **displayed** time (the editor's EnvTime table).
  * LogExp: exponential toward 1/(1−e^−k) = 1.065, clipped at 1.
  * ExpExp: the mirror image (growth of y + 1/(e^k−1)).
  * In both, k = 2.795 time constants per attack time.
  * LinExp and LinLin: straight.
* **Decay and release:** exponential; the displayed time is the fall to 1 %, so τ = T / ln 100. Measured ratios: 4.58 to
  4.68 (ln 100 = 4.605).
* **LinLin:** decay and release are linear, at full scale per displayed time.
* **Sustain:** v/128, and 127 gives 1.0.
* **Envelope runs at control rate (24 kHz); the VCA (Out = In·Env) at the module's rate.**
* **Comparison** (emulator time × 2.213, §3.3):
  * level error under 0.007 (max) and 0.0015 (rms) for 126 ms – 750 ms stages;
  * attack t90 within 0.3 %;
  * release to 10 % within 1 %.
  * The 0.5–3 ms stages show up to 0.05–0.14 instantaneous error from the emulator's frame jitter around note events.
* **Inferred, not measured:** OutType variants, AM, Reset, and retriggering from a non-zero level.

### 2.4 Mixers (Mix2-1A type 194, Mix4-1A type 193)
* **Mix4-1A:** a plain sum.
* **Mix2-1A levels:**
  * **Exp and dB:** the same curve, `0.01x + 0.99x³` with x = v/127 (the editor's dB display). Measured: 20 → 0.00544,
    64 → 0.1317, 100 → 0.4912.
  * **Lin:** v/128, with 127 = 1.0. Measured: 0.15625, 0.5, 0.78125, 1.0.

## 3. Black-box facts about the emulated machine

### 3.1 Output scaling
DAC word = **−0.0085306** × (signal into Out 1 × patch Gain curve × 2-Out Pad).

* **Level:**
  * A full-scale signal (1.0, an OscA at any wave, an Env at 1.0) gives **−41.38 dBFS**.
  * **The polarity is inverted:** an Env of +1 gives a negative DC word.
  * This includes whatever global/master level the emulated synth has at boot. The native engine does not apply it: it
    outputs signal units.
* **Patch Gain** (patch settings Level): the same `0.01x + 0.99x³` curve. Level 100 measures −6.17 dB, while the
  editor's VolumeDb text says −7.7 dB.
* **2-Out Pad:** ×2 (+6 dB), as the editor's OutPad text says.
* **After an upload, the output level ramps** from 0: about half at 0.45 s, 95 % at 1 s, settled at **~2.5 s**. Settle
  3 s before measuring. (Update: this was the Python emulator's slow OS time. The C++ emulator, whose OS runs on the
  DSPs' clock, reaches the level within 50 ms: `g2-hardware-and-emulation.md` §3.9.2.)
* The DAC path passes DC; there is no blocking filter.

### 3.2 Notes
* The synth's default performance has **Keyboard off on every slot**, so PlayNote (message 56) is acknowledged but
  plays nothing.
  * `g2blackbox.py` first sends a performance with slot A focused and keyboard-enabled (`g2p_send_kbd_performance`).
  * After that, notes play slot A.
* Note-on latency is 33–93 ms and varies (USB plus OS), so comparisons align on the onset. (The C++ emulator: 2-3 ms
  by PlayNote, 1.7 ms by MIDI; the long latencies were the Python emulator's slow OS time.)

### 3.3 Emulator faults that matter for timing
* **Resolved (2026-10-10, g2-hardware-and-emulation.md §3.8):** the 2.21 × came from a bug in the dsp56300
  library: interrupts entered through the JIT stacked a stale SR, so the control-rate wait loop's `ble` fell through
  early. With the fix, LfoA measures 10.3009 Hz for 10.3011 Hz displayed and envelope stages are within 0.6 % of the
  displayed times, with no rescale; the native engine's choice of the editor's times is confirmed. The rescaled
  comparisons below still hold (the fault was a pure speed-up). Kept for the record:
* **Control-rate code ran about 2.21 × too often** (before the fix).
  * LfoC measures 22.79 Hz where the editor displays 10.30 Hz, and 54.18 Hz for 24.40 Hz (×2.21).
  * Envelope stages take 0.452 × the displayed time.
  * The Env output advances in bursts, typically 4 consecutive samples then 4 held, instead of once per 4 samples.
  * `--no-idle-skip` gives the same result.
  * **Inference:** this is an emulator fault, not the G2. Once scaled by 2.213, every attack, decay and release
    matches the editor's displayed time to within 0.5 %, and the display formulas were written to match the hardware.
  * **Consequence:** the native engine uses the editor's times. Emulator comparisons of anything at control rate
    (envelopes, LFOs, slews, the output ramp) must rescale time.
* **Frame repeats:** about 0.1 % of samples, clustered around USB activity (note on/off).
* **Gate dropouts in a bigger patch:** in the 26-module patch of §4, the held note's gate dropped three times (72, 55,
  504 ms), and in another run it stayed lost after 1.4 s.
  * Sub-chains of the same patch matched exactly: t1/t2/t3 within 0.01 dB, DC 0.384 vs 0.384.
  * Where the gate stayed up, the full patch matched on its non-beating component (0.04 dB) and its DC (0.1872 vs
    0.1884).

## 4. CPU (M1, Release `-O3`, one voice at 96 kHz)

### Native (`g2audio … repeat=3`, best of 3)
| Patch | Native |
|-------|--------|
| OscA → 2-Out | 0.15 % of a core (690 × real time) |
| Voice: OscA → FltLP → EnvADSR → 2-Out | 0.29 % of a core (350 ×) |
| 26 modules: 6 OscA, 4 FltLP (6–36 dB), 5 EnvADSR, 5 mixers, Keyboard, 2-Out | **1.5 % of a core (65 ×)**, about 6 ns per module-sample |

### Emulator (DSP threads plus the ColdFire thread, from `dac.json`)
* 1–3 module patches: 2.0–2.7 core-seconds per emulated second, at 1.1–1.8 × real time.
* The 26-module voice (634 DSP instructions per frame on the voice DSP): **3.2 core-s/s**, at 1.0 × real time.
* A fully loaded machine (no idle skip: every DSP runs all 1536 instructions per frame): **11.3 core-s/s**, at 0.23 ×
  real time.

### Comparison and extrapolation
* **Native vs emulator:** about **200 × cheaper** for the same voice. The emulator also needs 75 s to boot and the
  user's firmware.
* **Typical 20–30-module patch with 4–8 voices:** about 6–12 % of one core natively.
* **Effects:** reverb is 183 DSP instructions per sample against about 5–40 for these modules (catalog costs). Even at
  ×5 per module for effects, a full patch stays at a few % of a core per voice.
* **The emulator** can't run such a patch in real time on this machine.

## 5. What this says about all ~165 modules

### Feasible, with the same loop
* About 60 % of the modules are logic, switches, mixers, levels, math, constants or MIDI. They are simple and their
  behaviour is mostly documented.
* About 25 % are oscillators, filters, envelopes and LFOs: the work done here.
* About 15 % are hard (below).
* These 8 processors, plus the tooling, took one session. With the tools in place, a medium module is about one
  measure–fit–compare iteration.

### Needs more than linear measurements
* **Nonlinear modules** (Saturate, Overdrive, Clip, ShpExp, WaveWrap): transfer curves by DC and ramp sweeps.
* **Resonant filters** (FltNord, FltClassic, FltMulti, FltVoice, FltPhase, EqPeak): resonance law, self-oscillation
  and saturation.
* **Effects with internal algorithms** (Reverb, delays, Phaser, Flanger, Chorus, Vocoder, FreqShift, PShift,
  Compress): structure has to be inferred from impulse and step responses. Close but not sample-identical results are
  likely.
* **Noise and random modules:** these match statistically, not sample for sample.
* **Sequencers, clocks and counters:** logic, from the manual and black-box tests.

### Engine work besides modules
* Polyphony and voice allocation, glide, bend, vibrato.
* Morph, variations, and parameter slewing. The OS slews knob words over 64 steps; the native engine changes them at
  once.
* Uprate rules for dynamic modules (already in `core/uprate`).
* The FX area and buses, and the cable order. Feedback loops depend on the G2's run order and one-sample delays, which
  are untested.

## 6. Risks
* **The reference is flawed for timing (§3.3).**
  * Absolute times come from the editor's display formulas. These agreed here to 0.5 %, but no real G2 has confirmed
    them.
  * A real G2 recording of an LFO and an envelope would settle the 2.21 × question.
* **Not bit-exact.**
  * The pitch quantization (≤ 0.2 cent), the sine's −82 dB distortion and the narrow-pulse edge case are not modelled.
  * Phase-sensitive patches (detuned oscillators summed, hard sync, FM) will match in sound but not in waveform.
* **The clean-room discipline has to hold for 165 modules.**
  * Only black-box tools are allowed: `g2blackbox.py` writes audio only.
  * The catalog must keep its no-code form.
  * `g2hostemu.py` mixes plumbing and DSP knowledge in one file. A black-box-only wrapper module would make the
    boundary easier to audit.
* **Emulator robustness:** gate dropouts and frame repeats under load limit comparisons of bigger patches, so test
  module by module.
* **Untested paths in the processors written here:** modulation inputs, KBT shares, OutTypes, Reset and retrigger,
  On/Off bypass. Their code follows the catalog and the manual, but none of it was compared yet.

## 7. Reproducing
```sh
cmake --build build --target g2audio g2mkpatch g2protobridge g2dspbridge
build/tools/g2audio/g2mkpatch t.pch2 osc=OscA osc.Wave=2 osc.KBT=0 osc.Coarse=45 out=2-Out "osc.0>out.0" gain=127
echo '[{"name":"t","patch":"t.pch2","settle":3,"seconds":1,"notes":[[0.1,64,1],[0.6,64,0]]}]' > jobs.json
nice -n 10 VENV/bin/python tools/firmware/g2blackbox.py jobs.json --out emu    # one emulator session at a time
build/tools/g2audio/g2audio t.pch2 nat.wav 1 key=64@0.1-0.6
tools/g2audio/g2compare.py nat.wav emu/t/dac.wav --skip 0.5 --dur 0.3
```
Tests: `g2tests "[engine]"` checks pitch, keyboard tracking, levels, DC-free pulses, saw band-limiting, the filter's
−2.86 dB at fc and its cascades, envelope times and sustain, and the mixer curves, Pad and Gain. None of them need the
emulator.
