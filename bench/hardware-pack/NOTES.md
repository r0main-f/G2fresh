# Hardware bench pack: notes for us

What a friend with a G2 gets is `patches/`, `B10 Stimulus.wav`, the instructions (`README.md` / `README.fr.md`, or the printable `README.html` / `README.fr.html`) and the form (`form.txt` / `form.fr.txt`). Zip those and send them. The rest of this folder is ours: `manifest.json` (what each patch is designed to do), `expected/` (what the emulated G2 makes of it) and this file.

Everything here is G2fresh's own. The patches are written module by module by `tools/benchpack/g2benchpack.cpp` with the core's edit API; no Clavia or third-party patch, text or image is in the pack. `expected/*.json` are our measurements of the emulated G2 running the user's own OS 1.62. The renders themselves are the output of Clavia's OS: they stay out of the repository (`expected/renders/`, ignored).

## Levels

- **Inside the G2** a full-level signal is ±64 units, which we write 1.0: an oscillator's sine, a Constant at 64, a logic high. A signal clips at ±4.0 (256 units, +12 dB) [emulated, B02].
- **At the DAC**, with Master Level full and an output module at Pad 0 dB, 1.0 becomes a word of 0.1247 (inverted): a full-level sine peaks at **-18.08 dBFS** of the DAC's full scale. The internal clip is therefore -6.04 dBFS at the DAC. Pad +6 dB doubles the word: the internal clip then sits at -0.02 dBFS [emulated, B02, B04].
- **At the ADC** an In module at Pad 0 dB passes the converter's word on as it is: the ADC's full scale is a signal of 4.0, which is the internal clip. Through a 2-In and a 2-Out, both at Pad 0 dB, the DAC's word is 0.4987 of the ADC's (inverted): **-6.04 dB** from the ADC's full scale to the DAC's [emulated, B10].
- **G2fresh** plays the DAC words times -8: a full-level signal is 0 dBFS in the host. So `G2fresh level = DAC level + 18.06 dB`. Every output level in `expected/` is a DAC level. Its inputs take a host level of 1.0 as a quarter of the ADC's full scale (a signal of 1.0); the pack does not use that convention: B10's input levels are relative to the ADC's full scale.
- What a full-level sine is in volts at OUT 1 is what B01 asks a real G2, together with the form's Q08 (the interface's dBu at 0 dBFS). What the ADC's full scale is in volts is what B10 asks.

## The patches

All play as soon as they are loaded, with one voice, keyboard tracking off everywhere, and the same values in all nine variations. The sequenced ones use their own ClkGen ("Intern", 120 BPM), never the master clock, and start each loop after a silent gap. "Load" is the VA cycles the emulated G2 reports, as a share of one DSP.

| Patch | Measures | How | Emulated G2, OS 1.62 | Load |
|---|---|---|---|---|
| B01 Level Ref | output level, sample clock | OscA sine, B5 +21 cents, to Out 1/2 | 999.876 Hz, peak -18.08 dBFS, THD -82 dB | 6 % |
| B02 Level Steps | clip point | the sine times a SeqLev value, then x4 and x2: 1 s of silence, then 1 s each at -24, -18, -12, -6, 0, +3, +6, +9, +12, +15, +18 dB re full level. Out 1 through Pad 0 dB, Out 2 through Pad +6 dB | Out 1: -42.2 ... -18.1 (0 dB) ... -6.0 dBFS (+12 dB), clipped from +15 dB. Out 2: 6.02 dB above, -0.02 dBFS at +12 dB | 11 % |
| B03 Output Map | which output is which, level per output | four sines into a 4-Out | Out 1 440.00, Out 2 587.33, Out 3 783.99, Out 4 1046.50 Hz, all -18.08 dBFS | 19 % |
| B04 Pad | the Pad switch | the sine, gated by two SeqLev, into two 2-Out on Out 1/2: 1 s silence, 3 s Pad 0 dB, 3 s Pad +6 dB, 1 s silence | +6.02 dB | 13 % |
| B05 White Noise | analogue response | Noise, Color 0 | -25.2 dBFS rms, peak -19.0 | 3 % |
| B06 Bright Saw | harmonics and aliasing | OscA saw, pitch from a SeqNote: silence, then C5 C6 C7 G7 C8 G8 C9 E9, 1 s each | partials per note in the JSON (harmonics, and aliases from about -41 dB at E9) | 11 % |
| B07 Silence | noise floor, hum | a 2-Out, nothing connected | digital silence | 2 % |
| B08 Clock 120 (performance) | master clock tempo, polarity | Out 1: a 5 ms positive pulse on every beat of the master clock (120 BPM, running, set by the performance). Out 2: the same from the patch's own ClkGen at 120 BPM | master 120.000 BPM; own clock 119.820 BPM; pulses positive | 5 % |
| B09a Saw Filter | a real chain | SeqNote, OscB saw (synced on each note), FltClassic 24 dB swept by an EnvD that is also the VCA | 4.006 s loop | 13 % |
| B09b FM Phrase | FM | SeqNote, two Operators (x2 into x1), triggered, Sync on | 4.006 s loop | 14 % |
| B09c Drum | DrumSynth | SeqEvent, DrumSynth with its noise off | 4.006 s loop, peak -9.8 dBFS | 12 % |
| B09d Rev Delay | Reverb and delay | a 1 ms pulse into Reverb (Hall, 1.5 s, mix 50), four seconds later another into DlyStereo (252 / 378 ms, feedback 50) | 8.012 s loop | 48 % |
| B10 Audio In | input gain, delay, polarity, clip point | 2-In (Pad 0 dB) to 2-Out (Pad 0 dB), `B10 Stimulus.wav` played into IN 1 | DAC word = 0.4987 x ADC word: -6.04 dB from full scale to full scale; 20.4 frames (0.212 ms); in phase; clean up to the ADC's full scale | 3 % |
| B11 Voices Light | voice count | 2 x (OscB, FltNord, EnvADSR) per voice, 32 voices asked | 16 voices: "16 (32)"; on a G2X 32: "32 (32)" | 21 % per voice |
| B12 Voices Mid | voice count | 3 of those chains | 12 voices; on a G2X 24 | 31 % per voice |
| B13 Voices Heavy | voice count with effects | 8 chains, then a Hall reverb in the FX area | 3 voices; on a G2X 7 | 79 % per voice, FX 31 % |
| B14 Wheels | which way the stick and the global wheels count | a steady A4; morph groups: P.Stick +5 semitones, G.Wh 1 +12, G.Wh 2 brings in an E6 | top of each control's range: 587 Hz, 880 Hz, E6 in; stick at the bottom: 330 Hz | 10 % |

Changes to the list we were given, and why:

- **Steady patches have no start marker** (B01, B03, B05, B07, B08): nothing in them changes, so there is nothing to align, and an unbroken tone gives the frequency to a fraction of a ppm.
- **B02 has two outputs**: with Pad 0 dB the DAC never goes above -6 dBFS (the signal clips inside the G2 first), so only the Pad +6 dB output can show where the analogue stage gives up.
- **B04 alternates by itself**: no module can move a parameter, but two output modules on the same outputs, one per Pad position, fed in turn, do the same in one recording.
- **B06 stops at E9**: at the sequencer's top note (127, G9) the oscillator plays 13.28 kHz, not G9's 12.54 kHz [emulated].
- **B08 is a performance**: the master clock's tempo and Run/Stop belong to the performance, not to a patch. Its second output carries the patch's own clock as a reference that does not depend on them.
- **B09d takes half a DSP**: the Reverb and the stereo delay are heavy modules. It still fits any G2.
- **B14 is extra**: a friend cannot tell which way a wheel "counts" without something to hear.

## B10: how loud the stimulus is at the ADC

`B10 Stimulus.wav` is all 1 kHz: three 20 ms bursts at -6 dBFS, three seconds at -20 dBFS, then one-second steps at -40, -30, -20, -12, -6 and -1 dBFS. How loud that reaches the G2's ADC cannot be known beforehand:

```
level at the ADC (dB re its full scale) = level in the file (dBFS) + K
K = the interface's output level at 0 dBFS (dBu) - the sine level that is the ADC's full scale (dBu)
```

The first term depends on the interface and its volume knob, the second is not in Clavia's manual (IN 1-4 are line inputs, -10 dBV nominal). So the prediction is made at a K we choose, and the analysis finds the recording's K.

- **The prediction** (`expected.py`, with `g2benchrender --in1 FILE --in-level K --in-ref REF`) plays the file into In 1 twice: at K = 0 (the file's full scale is the ADC's: the loudest step is 1 dB under it) and at K = +10 dB (the steps at -6 and -1 dBFS are 4 and 9 dB over it). The file is resampled to the G2's 96 kHz with a windowed sinc and read at each ADC frame's own time, its first sample when the DAC sends the first recorded frame. What is over the ADC's full scale is clipped there by the tool, as a converter stops at its largest word: that clip is ours, not the OS's.
- **What the emulated G2 says:**
  - gain: the DAC's word is 0.4987 of the ADC's, -6.04 dB from full scale to full scale, at every level up to the ADC's full scale;
  - delay: 20.36 frames, 0.212 ms, from an ADC word to the DAC word it becomes (the G2's own way, nothing of the plugin's buffers);
  - polarity: Out 1 follows In 1 in the sign the renders have and G2fresh plays; the DAC's word itself is the negative;
  - clip: nothing between the converters clips first. The 2-In passes the word on and the internal clip (4.0) is the ADC's full scale; a sine just at it leaves the DAC at -6.04 dBFS, B02's last clean level on Out 1. At K = +10 dB the steps 4 and 9 dB over read 2.5 and 7.1 dB low, with -15.5 and -10.5 dB of distortion.
- **A recording** carries the same stimulus on its second channel, through a cable from the interface's output 2 to its input 2 (the loopback). `analyse.py` works from that channel:
  - gain from IN 1 to OUT 1 in volts: channel 1 over the loopback, taking the interface's two outputs as equally loud and its two inputs as equally sensitive. Less our -6.04 dB, that is the DAC's full-scale voltage over the ADC's;
  - delay: the phase of the three-second tone against the loopback's, the bursts saying which cycle of it (a clipped burst rises early; the tone's phase does not move). Polarity: from the bursts; times B08's, the input's own;
  - K: the level at Out 1, less Out 1's chain gain (B01) and our -6.04 dB, is the level at the ADC. The report gives every step's level at the ADC, K, and with the form's Q08 the ADC's full scale in dBu;
  - the clip point: the first step whose gain is 0.5 dB under the quiet steps' or whose distortion is over -40 dB. We expect it to be the first step over the ADC's full scale; if a step under it is compressed, an analogue stage gives up before the converter;
  - without the loopback: no gain and no delay, but K and the clip point still, against the levels the file was written at.

## What the emulator already says (to confirm on a G2)

- A ClkGen on "Intern" at 120 BPM runs at 119.82 BPM (a beat is 48072.2 samples): every sequenced loop is 0.15 % longer than nominal. The master clock at 120 BPM is exact in the emulator (48000.0 samples a beat), where its clock is tied to the sample clock. On a G2 the two may come from different crystals: B08's two outputs give their ratio.
- The Patch Settings page shows the master clock and the voices above knobs 1 and 2: `120 BPM  03 (32)` over `Run  Poly`. After power-on the emulated G2 shows 120 BPM, Run.
- Store with Memory Protect on shows "Memory is protected!" and stores nothing. With it off, Store shows "Store to 1:1" and Patch leaves that without storing.
- Voice counts come in two figures: a G2 or a G2 Engine without the expansion board (four DSPs: `facts`), and a G2X or any G2 with the board (eight DSPs: `facts.g2x`). The emulator has the board since 2026-10-13 (the OS finds it by itself); with it B11-B13 get 32, 24 and 7 voices, the manual's "seven voices plus effects in an expanded or G2X model" (p.121). On eight DSPs B13's reverb runs alone on the last DSP and each of the seven voices on one of the others. The plugin's own emulated G2X still runs four DSPs: in G2fresh it gives the first figure.

## Regenerating

```sh
cmake --build build --target g2benchpack g2benchrender
build/tools/benchpack/g2benchpack bench/hardware-pack            # patches/ and manifest.json
python3 tools/benchpack/make_stimulus.py                         # B10 Stimulus.wav
G2_FIRMWARE="/path/to/Nord Modular G2 OS v1.62 Update.dmg" python3 tools/benchpack/expected.py   # expected/*.json, about 5 minutes (B10: the stimulus into In 1)
python3 tools/benchpack/make_html.py                             # README.html, README.fr.html
```

`ctest` checks that the committed patches, pages and stimulus are current (`benchpack_*`), that the analysis finds the faults of synthetic recordings (`benchpack_analysis_selftest`) and, with `G2_FIRMWARE` set and the emulator built, that four patches (B10 with its stimulus among them) still give the committed numbers on the emulated G2.

## Reading what comes back

```sh
python3 tools/benchpack/analyse.py path/to/returned-folder        # writes report.md there, and prints it
```

The folder holds the WAV files and the filled form. The report gives, against `expected/`:

- the chain gain per output from B01 and B03 (with Q08: a full-level sine in dBu and volts, which is our output scaling), and the sample clock in ppm;
- which recorded channel is which output (a crossed pair is put back in order for every file);
- B02: the first step that is compressed or distorted on each output, against ours; B04: the Pad step;
- B05: the noise spectrum against ours in third octaves (the analogue response), B07: the noise floor and mains hum;
- B06: every partial of every note against ours (harmonics: the response up to the recording's bandwidth; non-harmonic partials: aliasing);
- B08: the master clock's tempo, its ratio to the patch's own clock, and the pulses' polarity;
- B09: after aligning on the loop, the envelope's correlation with ours, the level differences over 50 ms stretches and the third-octave spectrum;
- B10: gain, delay and polarity from In 1 to Out 1 against the loopback channel and against ours, how loud the stimulus was at the ADC, the ADC's full scale (in dBu with Q08) and the step where the input compresses (see "B10" above);
- the form's answers next to what the emulated G2 shows.

`python3 tools/benchpack/selftest.py --emulator --work DIR` makes such a folder from our own renders with known faults (gain, 37 ppm, 48 kHz, crossed outputs, inverted polarity, noise, hum, treble loss, an analogue clip, a fast master clock, an input driven 3 dB over its ADC's full scale with the loopback on the other channel) and checks the report: read `DIR/report.md` to see what a report looks like.

## Not certain

- **Not sample-for-sample repeatable**: the phrases repeat exactly in level and spectrum, but a loop is not a whole number of samples and triggers fall on the control rate's 4-sample grid, so two loops of our own render correlate 0.96 to 0.99 as waveforms (envelopes: 0.9999). We compare envelopes and spectra, not waveforms. The Reverb's tail differs slightly from loop to loop.
- **The sample clock figure includes the interface's clock**: a few tens of ppm either way.
- **G2 Engine**: the instructions say Clavia's editor shows the voice count in its toolbar and the OS version when it connects. That is from memory of the editor, not checked.
- **Leaving Store**: "press Patch twice" is what the emulated panel does; untested on hardware.
- **G.Wheel directions** are exactly what we do not know: the emulator's answer above is not a prediction.
- **The G2X's voice counts** are the emulated board's: the OS's own allocation over eight DSPs, which matches the manual's figures, but the board itself (its DSPs' clock, its memory) is modelled after the main board's, not measured. A friend's G2X (or an expanded G2) confirms or corrects `facts.g2x`.
- **B10's delay is not a prediction for a G2**: 20.4 frames is the emulator's digital way, with no converter in it. A G2's converters add their filters' delay, and its board may take fewer frames between the DSPs than the emulator's lines do (`re/notes/g2-hardware-and-emulation.md` §3.11.2). The gain, the polarity of the words and the absence of a clip under the ADC's full scale are digital facts of the OS and should hold.
- **B10's clip above the ADC's full scale** is the render tool's hard stop, not a model of a converter: a real ADC, or the analogue stage before it, may bend sooner or softer. The top steps are 5 to 8 dB apart, so a recording brackets the clip point; it does not measure it.
- **B10's levels at the ADC** (K, the ADC's full scale in dBu) rest on three things a recording cannot show: the G2's gain from the ADC to the DAC being ours, Out 1 still being on the interface input it was on for B01 at the same gain, and the interface's two outputs (and its two inputs) being alike. If the interface's output level is too low for the loudest step to reach the ADC's full scale, there is no clip point, only "above this".
- **Which way the analogue stages turn the signal** is unknown for the inputs and the outputs alike: B08 gives the outputs', B10 the product of both.
