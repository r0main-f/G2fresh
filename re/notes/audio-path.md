# The audio path between the emulated G2 and the host

Date: 2026-10-13. Measured on the user's computer (Apple M1, 4 performance + 4 efficiency cores, macOS 26) with the
user's own G2 OS 1.62 in `emu/g2emu`. This note covers what lies between the emulated G2's converters and a
host's audio buffers: the sample rate conversion (part 2) and the latency (part 3). The emulated machine itself is
not changed by any of it: the OS and the DSPs see the same world.

Evidence tags as in the other notes: **[measured]** this session, with the tool named; **[emulated]** the user's OS
in g2emu; **[M]** the manual; **[inferred]** a deduction, not checked.

## 0. Summary

| | Before (0.10.2) | Now |
|---|---|---|
| 96 kHz to the host's rate | JUCE's 5-point Lagrange interpolator, no filter: at 48 kHz every second sample. Everything the G2 plays above the host's Nyquist frequency folds into the audible band at full level: a sawtooth's aliases at −41 to −19 dB of the audible signal, white noise's at −4 dB, a 31.6 kHz sine as a 16.4 kHz tone at 0 dB | A Kaiser-windowed sinc, linear phase (`plugin/Source/Resampler.h`): flat within 0.0001 dB up to 20 kHz, 100 dB down from 28 kHz (48 kHz host) or 24.1 kHz (44.1 kHz host). The same sounds: aliases at −129 dB or less |
| The host's rate to 96 kHz (inputs) | The same interpolator: −2.5 dB at 20 kHz (48 kHz), −4.4 dB (44.1 kHz), and an image of every input partial above the host's Nyquist frequency (−10 dB for a 20 kHz partial) | The same filter the other way: flat, images 99 dB down |
| At a 96 kHz host | Through the interpolator (exact, 2 samples late) | Nothing is computed: the samples as they are |
| Latency, 48 kHz, 256-sample blocks | 1297 samples, 27.0 ms | **Safe (the default) 1327 (27.6 ms)**, Normal 607 (12.6 ms), Low 415 (8.6 ms): Synth > Emulated G2 Buffer (part 3.2; Normal's and Low's reserves are provisional) |
| Latency, 48 kHz, 64-sample blocks | 1105 samples, 23.0 ms [inferred from the code, not measured] | **Safe 1135 (23.6 ms)**, Normal 415 (8.6 ms), Low 223 (4.6 ms) |
| Beside other busy programs (provisional: a shared computer) | Dropouts: its threads gave way to other programs' (heavy patch, 20 ms reserve, 64 to 256-frame blocks: 1 to 50 % of the audio missing at a load average of 10 to 16; all of it at 63 to 178) | On macOS its threads have the audio threads' priority: none missing in the same comparisons, also with a 5 ms reserve (part 3.4) |
| Dropouts | Counted, shown only in the capture diagnostics | Counted, shown in the editor's status line |

A real G2 has no such path: its DACs and ADCs run at the DSPs' 96 kHz, and a note sounds 1.5 to 1.7 ms after its
MIDI bytes [emulated]. The emulated G2 keeps that (it is the OS's own reaction) and adds what the table says.

## 1. The path

```
host block (N samples at the host's rate)
  inputs  -> x 0.25 (AudioIo.h) -> converter up (host rate -> 96 kHz)   -> Runner::write -> InputQueue -> A6's ADCs
  MIDI    -> Runner::midiInAt (at its place in the block, + latencyFrames)              -> UART0 (MIDI IN)
  outputs <- x -8               <- converter down (96 kHz -> host rate) <- Runner::read  <- ring <- A3's DACs
                                                                                          ^ the machine's threads
```

* `plugin/Source/EmulatedSoundEngine.cpp::render` is the whole of it on the audio thread: no locks, no allocation
  for blocks up to the size the host announced.
* `emu/g2emu/src/runner.cpp`: the machine runs on threads of its own (the ColdFire's, and one or two for the four
  DSPs), ahead of the reader. Its ring is the only thing between the two sides.
* `plugin/Source/Resampler.h`: the two converters, each for four channels.

## 2. Sample rate conversion

### 2.1 What was there [measured]

`tools/audiopath/aliasing.py` (a numpy copy of `juce::Interpolators::Lagrange`, checked against its two exact cases:
2:1 returns every second input sample, 1:2 returns the input samples and one interpolated value between each pair).

**Tones** (`aliasing.py tones`): a sine of 0 dB at f at the input; its level at the output, and the level of
everything else that comes out (as one sine of that power).

| 96 kHz to 48 kHz, f | Lagrange | Converter |
|---|---|---|
| 1 to 22 kHz | 0.00 dB; nothing else | 0.00 dB to 20 kHz (−0.40 dB at 22 kHz); nothing else above −146 dB |
| 25 kHz | folds to 23 kHz at 0.0 dB | 23 kHz at −13.7 dB (the transition band: above 20 kHz) |
| 28 kHz | 20 kHz at 0.0 dB | −100.5 dB |
| 30 kHz | 18 kHz at 0.0 dB | −105.0 dB |
| 34, 38, 43, 47 kHz | 14, 10, 5, 1 kHz at 0.0 dB | −115 to −122 dB |

| 96 kHz to 44.1 kHz, f | Lagrange | Converter |
|---|---|---|
| 10 kHz | −0.00 dB; −67.5 dB of other partials, mostly at 2.2 kHz | 0.00 dB; −139 dB |
| 15 kHz | −0.02 dB; −50.1 dB, mostly at 7.2 kHz | 0.00 dB; −137 dB |
| 20 kHz | −0.11 dB; −38.0 dB, mostly at 12.2 kHz | 0.00 dB; −135 dB |
| 25 kHz | folds to 19.1 kHz at −0.3 dB | −111 dB |
| 30 kHz | 14.1 kHz at −0.8 dB | −113 dB |
| 38 kHz | 6.1 kHz at −2.1 dB | −120 dB |
| 47 kHz | 2.9 kHz at −3.4 dB | −131 dB |

At 44.1 kHz the interpolator's places between the samples keep changing, so even a tone the host's rate can hold
comes with partials that are not harmonics of it (the second column of the Lagrange cells).

| Host rate to 96 kHz (inputs), f | Lagrange, 48 kHz | Converter, 48 kHz | Lagrange, 44.1 kHz | Converter, 44.1 kHz |
|---|---|---|---|---|
| 1 kHz | 0.00 dB; image at 47 kHz, −133 dB | 0.00 dB; −110 dB | 0.00 dB; −134 dB | 0.00 dB; −124 dB |
| 5 kHz | 0.00 dB; 43 kHz, −63.5 dB | 0.00 dB; −122 dB | 0.00 dB; 39.1 kHz, −63.8 dB | 0.00 dB; −131 dB |
| 10 kHz | −0.08 dB; 38 kHz, −34.8 dB | 0.00 dB; −110 dB | −0.17 dB; 34.1 kHz, −34.5 dB | 0.00 dB; −118 dB |
| 15 kHz | −0.66 dB; 33 kHz, −19.4 dB | 0.00 dB; −107 dB | −1.24 dB; 29.1 kHz, −18.7 dB | 0.00 dB; −109 dB |
| 18 kHz | −1.57 dB; 30 kHz, −13.3 dB | 0.00 dB; −108 dB | −2.80 dB; 26.1 kHz, −12.4 dB | 0.00 dB; −109 dB |
| 20 kHz | −2.51 dB; 28 kHz, −10.1 dB | 0.00 dB; −99.1 dB | −4.36 dB; 24.1 kHz, −9.1 dB | 0.00 dB; −99.7 dB |

**What the emulated G2 plays** (`aliasing.py material`, on 96 kHz recordings made with `tools/benchpack/g2benchrender`).
The conversion is linear, so the part of a recording above the host's Nyquist frequency, converted alone, is the
aliasing. Its level between 20 Hz and 20 kHz is given against the signal's level in that band; the strongest aliased
partials against the sound's strongest partial.

| Sound | Above 24 kHz, of the whole | Lagrange at 48 kHz: aliasing (strongest partials) | Converter at 48 kHz | Lagrange at 44.1 kHz | Converter at 44.1 kHz |
|---|---|---|---|---|---|
| B06 Bright Saw, C5 (523 Hz) | −35.6 dB | −41.4 dB (19.7 kHz −47, 19.2 kHz −48, 18.7 kHz −48) | −149 dB | −36.8 dB | −141 dB |
| C6 | −32.1 dB | −37.9 dB (19.7 kHz −41, 18.7 kHz −43, 17.7 kHz −44) | −144 dB | −34.0 dB | −142 dB |
| C7 | −29.5 dB | −35.5 dB (18.7 kHz −36, 16.6 kHz −39, 14.5 kHz −43) | −139 dB | −30.0 dB | −137 dB |
| G7 | −26.3 dB | −30.4 dB (19.8 kHz −31, 16.6 kHz −36, 13.5 kHz −40) | −142 dB | −26.8 dB | −141 dB |
| C8 | −24.7 dB | −30.5 dB (18.7 kHz −30, 14.5 kHz −37, 10.3 kHz −46) | −133 dB | −25.2 dB | −136 dB |
| G8 | −20.8 dB | −29.3 dB (16.6 kHz −29, 10.4 kHz −40, 14.7 kHz −47) | −137 dB | −21.2 dB | −137 dB |
| C9 | −18.4 dB | −30.5 dB (14.5 kHz −30, 19.0 kHz −43, 10.6 kHz −45) | −141 dB | −18.8 dB | −129 dB |
| E9 (10.5 kHz) | −23.7 dB | −23.4 dB (16.4 kHz −24, 15.3 kHz −41, 5.8 kHz −47) | −147 dB | −24.4 dB | −139 dB |
| B05 White Noise | −5.0 dB | −3.8 dB | −100 dB | −4.4 dB | −100 dB |
| OscA sine at 31.6 kHz (Coarse 127, a Constant of 80 on Pitch) | 0.0 dB | 16.4 kHz at 0.0 dB (nothing else is audible in the sound) | 16.4 kHz at −118 dB | 12.5 kHz at −1.1 dB, 4.7 kHz at −34 dB | −117 dB |
| OscA sine swept from 47 kHz down to 3 kHz by an LFO | −3.2 dB | −9.2 dB | −119 dB | −3.4 dB | −117 dB |

So a G2 oscillator reaches 48 kHz [emulated: Coarse 127 with a Constant of 127 on its Pitch input plays 48.0 kHz],
and a patch can put a full-level partial anywhere above the host's Nyquist frequency. Through the interpolator it
came back as a tone the patch does not contain. For the pass band the last column of the script (not shown) gives
the Lagrange path an error of −46 to −62 dB of the signal at 44.1 kHz (treble loss and the partials of the first
table), none at 48 kHz, and the converter −107 dB or less at both.

### 2.2 What a real G2 gives

* The G2's converters run at the DSPs' rate, 96 kHz (§1.1 of `g2-hardware-and-emulation.md`). The parts, as the
  community reads the board (medium confidence, not seen by us): two CS4392 DACs, two CS5341 ADCs.
* **CS5341** (ADC), double-speed mode (50 to 100 kHz): pass band to 0.4895 Fs within 0.1 dB (47.0 kHz at 96 kHz),
  stop band from 0.5604 Fs (53.8 kHz), 69 dB, group delay 9/Fs (94 us) [Cirrus Logic data sheet DS564, read through
  a data sheet mirror's text: **check the figures against the PDF before relying on them**].
* **CS4392** (DAC): its own filter table was not retrieved. Cirrus's DACs of the family in double-speed mode pass to
  about 0.45 to 0.5 Fs and stop from 0.577 to 0.583 Fs at 55 to 80 dB (CS4391, CS4364: **not the CS4392's own
  figures**).
* So the G2's line outputs carry everything the DSPs compute up to about 45 kHz, and its inputs take everything up
  to about 47 kHz. The G2 does nothing about its own oscillators' partials above 24 kHz: they are in its analogue
  signal.
* Whoever records a G2 at 48 kHz does so through an audio interface's ADC, whose decimation filter removes what
  48 kHz cannot hold: typically flat to 0.45 Fs (21.8 kHz) and 70 to 110 dB down from 0.55 to 0.6 Fs (26 to 29 kHz),
  as the CS5341's own single-speed mode (stop band from 0.5687 Fs, 70 dB). Between the two edges a little folds,
  above 20 kHz.
* **The goal here**: no aliasing a real G2 recorded at that rate would not have. The converter is such an
  interface's filter: pass band to 20 kHz, stop band from 28 kHz (48 kHz) or 24.1 kHz (44.1 kHz), 100 dB. Partials
  the G2's own DSP code folds at 96 kHz (around its own Nyquist frequency of 48 kHz) are the G2's sound and stay.
* **Audible changes, all deliberate** (README, "Deliberate deviations" has them as a fix):
  * bright patches lose the folded partials (tables above): they sound cleaner at the top than in 0.10.2, as a
    recorded G2 does;
  * the inputs keep their treble (+2.5 dB at 20 kHz at a 48 kHz host, +4.4 dB at 44.1 kHz) and come without images
    above the host's Nyquist frequency, which a patch that shifts, folds or ring-modulates its input could make
    audible;
  * at 44.1 kHz and other rates that are not 96 kHz divided by a whole number, the inharmonic partials of the
    interpolator are gone (−38 dB beside a 20 kHz tone, −67 dB beside a 10 kHz one).
  Not changed: levels, and everything below 20 kHz at a 48 kHz host (the old path was exact there).

### 2.3 The converter

`plugin/Source/Resampler.h` (plain C++, no JUCE; tests in `tests/test_resampler.cpp`).

* A sinc at the lower rate's Nyquist frequency under a Kaiser window (designed for 100 dB: beta 10.06), linear
  phase. Pass band edge: 20 kHz at 44.1 and 48 kHz; 0.4535 of the rate below 44.1 kHz; the same share of the rate as
  at 48 kHz (0.4167) above, so 36.75 kHz at 88.2 kHz and 40 kHz from 96 kHz on. Stop band from (the lower rate −
  the pass band's edge) on: whatever folds or mirrors lands above the pass band or is 100 dB down. At 2:1 the
  filter is a half-band filter.
* Exact for any pair of rates: the two rates as whole numbers of Hz give a ratio M/L in lowest terms; output sample
  j is at input time j M/L (integers: no drift), and each of the L places between two input samples has its own
  row of taps in a table (48 kHz: 1 row down, 2 up; 44.1 kHz: 147 rows down, 320 up). A pair with more than 2048
  places (a host at 47 999 Hz) gets a table of 2049 rows, interpolated.
* A stretch of output takes the input up to its last sample's place and no further (`needs()`), so the engine
  reads exactly the 96 kHz frames a block needs: no slack frames. The two converters keep step: the 96 kHz frames
  the outputs take need the host's input samples up to the block's last or last but one (at most one waits).
* A channel that has been silent for the filter's length gives exact zeros without computing (Out 3/4 of most
  patches, inputs nothing is routed to).
* Equal rates: `transparent()`, the samples are copied.

### 2.4 Measured [measured: `g2audiopath response`, `g2audiopath cpu`, `tests/test_resampler.cpp`]

400 tones per band. "Left of a tone in the stop band": going down, such a tone would fold into the pass band.
"Images": everything that comes out beside a pass-band tone.

| Conversion | Taps | Delay | Pass band | Deviation there | Stop band from | Left of a tone in the stop band | Images and aliases of a pass-band tone |
|---|---|---|---|---|---|---|---|
| 96 kHz to 44.1 kHz | 152 | 76 frames (0.792 ms) | to 20 kHz | 0.0001 dB | 24.1 kHz | −100.3 dB | −127.7 dB |
| 44.1 kHz to 96 kHz | 72 | 36 samples (0.816 ms) | to 20 kHz | 0.0001 dB | 24.1 kHz | (none in the input) | −99.6 dB |
| 96 kHz to 48 kHz | 80 | 40 frames (0.417 ms) | to 20 kHz | 0.0001 dB | 28 kHz | −100.4 dB | −142.9 dB |
| 48 kHz to 96 kHz | 40 | 20 samples (0.417 ms) | to 20 kHz | 0.0001 dB | 28 kHz | (none in the input) | −99.1 dB |
| 96 kHz to 88.2 kHz | 44 | 22 frames (0.229 ms) | to 36.75 kHz | 0.0001 dB | 51.45 kHz | (none in the input) | −105.7 dB |
| 88.2 kHz to 96 kHz | 40 | 20 samples (0.227 ms) | to 36.75 kHz | 0.0001 dB | 51.45 kHz | (none in the input) | −99.9 dB |
| 96 kHz to 176.4 kHz | 40 | 20 frames (0.208 ms) | to 40 kHz | 0.0001 dB | 56 kHz | (none in the input) | −99.9 dB |
| 176.4 kHz to 96 kHz | 72 | 36 samples (0.204 ms) | to 40 kHz | 0.0001 dB | 56 kHz | −99.7 dB | −114.1 dB |
| 96 kHz to 192 kHz | 40 | 20 frames (0.208 ms) | to 40 kHz | 0.0001 dB | 56 kHz | (none in the input) | −99.1 dB |
| 192 kHz to 96 kHz | 80 | 40 samples (0.208 ms) | to 40 kHz | 0.0001 dB | 56 kHz | −100.4 dB | −142.9 dB |

At the lower rate's Nyquist frequency the response is −6 dB (the middle of the transition band).

* **Delay**: half the filter's length, a whole number of input samples; an impulse comes out centred on it, the same
  on both sides to 1e-7. Both ways together: 40 samples (0.83 ms) at 48 kHz, 71 samples (1.61 ms) at 44.1 kHz,
  0.42 ms at 192 kHz, none at 96 kHz. It is in the latency the plugin reports (part 3.1).
* **Round trip** (host to 96 kHz and back): tones up to the pass band's edge come back at their level and at the
  delay's phase; what is left is more than 80 dB down (the test's bound; the stop band's 100 dB is what limits it).
* **The same samples whatever the block size** (1, 7, 64, 256 and 1000 samples, the last more than the converter
  was prepared for), and exactly the input `needs()` says.
* **CPU**, four channels of noise each way, 256-sample blocks, on one performance core of the M1:

  | Host rate | 96 kHz to host | Host to 96 kHz | Both |
  |---|---|---|---|
  | 44.1 kHz | 0.79 % | 0.66 % | 1.45 % of one core |
  | 48 kHz | 0.35 % | 0.37 % | 0.72 % |
  | 88.2 kHz | 0.36 % | 0.40 % | 0.76 % |
  | 96 kHz | 0.01 % | 0.01 % | 0.03 % (copies) |
  | 176.4 kHz | 0.70 % | 0.70 % | 1.41 % |
  | 192 kHz | 0.76 % | 0.74 % | 1.51 % |
  | 47 999 Hz (interpolated table) | 0.82 % | 0.96 % | 1.78 % |

  Against the 130 to 160 % of a core the emulated G2 itself takes. Channels that are silent cost nothing.

## 3. Latency

> **Every timing figure in this part is provisional.** The computer was shared with other work for the whole
> session (other agents building and running emulators beside these measurements; a 1-minute load average between
> 4 and 350 on 8 cores, given with each figure). None of them is a quiet computer's figure, and none of them sets a
> default or a threshold. What they can say: comparisons made in alternation under the same conditions (with and
> without real-time priority, with and without the breath) hold as comparisons; absolute figures (how much reserve a
> setting needs, the speed margin, the wake-up times) are only upper or lower bounds of what a quiet computer does.
> The measurement runs' raw results (JSON lines) were lost with the computer's temporary folder in a restart; what
> is below is what had been printed. `tools/audiopath/sweep.py` (part 3.6) measures it all again, in one command.

### 3.1 Where the latency comes from [from the code; the checks in part 3.5]

From an input sample to what the G2 makes of it at the output, which is what the plugin reports to the host
(`EmulatedSoundEngine::latencySamples`), in the order the sample meets them:

| Part | 96 kHz frames | 48 kHz, 256-sample blocks, Safe |
|---|---|---|
| The input converter's delay (host samples) | – | 20 samples (0.42 ms) |
| The reserve (`Runner::setBufferMs`: the buffer setting) | 96 per ms | 2016 frames (21 ms) |
| One host block, in 96 kHz frames (`setReadSize`: ceil(N × 96000 / R)), or the stretch (96 frames, 1 ms) if that is longer | | 512 frames (5.33 ms) |
| How far the DACs may be ahead of the frames asked for (`Runner::dacLead`: dspLead / 1536 + 2) | 10 | 10 frames (0.10 ms) |
| How far A6's ADCs may run ahead (`Runner::inputLead`: dspLead / 1536 + 8) | 16 | 16 frames (0.17 ms) |
| The G2's own way from its ADCs to its DACs through an In and an Out module (`kAdcToDacFrames`) [emulated] | 20 | 20 frames (0.21 ms) |
| The output converter's delay | | 40 frames (0.42 ms) |
| **Total** | | 20 + 2614 / 2 = **1327 samples, 27.6 ms** |

So, at a host rate R with blocks of N samples, the up and down converters' delays U (host samples) and D (96 kHz
frames, part 2.4: U = 20, D = 40 at 48 kHz; U = 36, D = 76 at 44.1 kHz; none at 96 kHz):

    latency = U + (96 × reserve_ms + max(ceil(N × 96000 / R), 96) + 10 + 16 + 20 + D) × R / 96000   samples

A MIDI note goes the same way without the input's part (U, the 16 frames of the ADCs' lead and the G2's 20):
`midiLatencySamples()` = (96 × reserve_ms + max(block, stretch) + 10 + D) × R / 96000, 0.8 ms less at 48 kHz; then
the G2 itself takes 1.5 to 1.7 ms to sound it (its MIDI line and the OS's reaction) [emulated], as a real G2 does.

**Up to 0.10.2** the same path gave 1297 samples at 48 kHz with 256-sample blocks: the 20 ms reserve (1920 frames),
the block (513 frames), a whole chunk (96 frames: the machine ran in 1 ms chunks until it was past its mark), the
16 frames of the inputs' lead and the G2's 20, and about 15 samples of interpolator delay and of input held back
[from main's code]. Since 97f1f37 the machine stops exactly at its mark, so at rest it holds the reserve (minus 8
frames of slack, 0.08 ms: it does not start for the frame or two a run hands out late) where it held between the
reserve and a millisecond more. **Safe's reserve is 21 ms** so that it keeps at least what 0.10.2 kept, and its
latency is not lower than 0.10.2's (27.6 against 27.0 ms here).

### 3.2 The three settings (Synth > Emulated G2 Buffer)

The user's choice for the computer (the settings file every G2fresh instance shares, `emulatorBuffer`; the
environment variable `G2FRESH_EMULATOR_BUFFER` = low, normal or safe overrides it, for tests), applied while it
plays: the reserve changes (`Runner::setBufferMs`), the plugin reports the new latency (`setLatencySamples`), and
what is on its way keeps its place (one step in time). The menu shows each setting's latency in the running host.

| Setting | Reserve | 48 kHz, 256 | 48 kHz, 64 | 48 kHz, 512 | 44.1 kHz, 512 | 96 kHz, 256 |
|---|---|---|---|---|---|---|
| Safe (the default) | 21 ms | 1327 (27.6 ms) | 1135 (23.6 ms) | 1583 (33.0 ms) | 1530 (34.7 ms) | 2318 (24.1 ms) |
| Normal | 6 ms | 607 (12.6 ms) | 415 (8.6 ms) | 863 (18.0 ms) | 869 (19.7 ms) | 878 (9.1 ms) |
| Low | 2 ms | 415 (8.6 ms) | 223 (4.6 ms) | 671 (14.0 ms) | 692 (15.7 ms) | 494 (5.1 ms) |

(From the formula of part 3.1; the README has the first, second and fourth columns.) A real G2: 1.5 to 2 ms from a
MIDI note to its output.

**Provisional.** Safe is the default because it keeps what 0.10.2 kept, not because a measurement chose it. Low's
2 ms and Normal's 6 ms come from runs on the shared computer (part 3.4) in which the real-time machine never took
more than 0.7 ms of its reserve; they are guesses with a margin, to be set from a quiet computer's sweep. Dropouts
are counted from the first block the machine had ready (`EmulatedSoundEngine::dropouts`: the reads that came
short) and shown in the editor's status line ("3 dropouts: try Synth > Emulated G2 Buffer"): a smaller buffer is
never traded silently for latency.

### 3.3 Before: the buffer as 0.10.2 ran it (Phase 1) [measured: `g2audiopath runner`, old code]

60 s per run, 20 ms reserve, two DSP threads, the callback thread at real-time priority (as a host's), the
machine's threads at the user-interactive class (0.10.2's). Load average 4 to 16 (not quiet). "Reserve taken": how
far below (reserve + block) the ring was when a callback came (negative: above); "chunk speed": emulated time over
wall time of each 1 ms chunk; "wake": from the read that woke the machine's thread to its next chunk.

| Patch | Block (frames) | Missing frames (short blocks of all) | Reserve taken ms p50 / p99 / p99.9 / max | Chunk speed p0.1 / p1 / p50 | Wake ms p99 / max | Longest chunk ms | Load |
|---|---|---|---|---|---|---|---|
| light (B01) | 64 | 0 (0 of 90001) | 0 / 0.43 / 11.68 / 13.76 | 0.70 / 0.97 / 1.99 | 0.017 / 0.333 | 5.2 | 4 |
| light | 128 | 0 (0 of 45001) | 0 / 0 / 0.34 / 1.42 | 0.91 / 1.33 / 2.42 | 0.024 / 0.285 | 1.9 | 4 |
| light | 256 | 0 (0 of 22500) | 0 / 0 / 5.69 / 14.08 | 0.82 / 1.20 / 2.42 | 0.011 / 0.167 | 4.5 | 5 |
| light | 512 | 0 (0 of 11250) | 0 / 0 / 0.35 / 2.75 | 0.89 / 1.63 / 2.42 | 0.010 / 0.074 | 3.0 | 6 |
| light | 1024 | 0 (0 of 5626) | 0 / 0 / 0 / 0.05 | 0.95 / 2.06 / 2.42 | 0.015 / 0.057 | 1.4 | 6 |
| light | 2048 | 0 (0 of 2813) | 0 / 0 / 0 / 0 | 0.89 / 1.92 / 2.42 | 0.015 / 0.059 | 2.7 | 8 |
| heavy (B13, a chord) | 64 | 969 083, 50 % (15 427 of 90 000) | 0 / 20.67 / 20.67 / 20.67 | 0.36 / 0.49 / 2.01 | 0.022 / 0.614 | 4.7 | 10 |
| heavy | 128 | 2 886 817 (22 719 short) | max 21.33 | p1 0.39, p50 1.75 | max 0.520 | | 16 |
| heavy | 256 | 141 494 (566 short) | max 22.67 | p1 0.60, p50 2.01 | max 2.028 | | 11 |

(The heavy rows for 512 to 2048 frames and the four-slot rows were not run: the sweep was stopped when the other
work grew.) Beside the first light run in full: chunk duration p50 0.50, p99 1.02, max 5.19 ms; the speed over
each refill (a wake-up to the next rest) min 0.74, p1 1.13, p50 2.27; one refill took 115 ms; callbacks came
0.021 ms late at p99.9 (0.053 at most), every 0.667 ms ± 0.05.

Ten-second trials at 512 frames, load 5 to 6: heavy with two DSP threads 0 missing (reserve taken at most
8.1 ms, chunk speed p1 1.05, p50 2.02); heavy with one DSP thread all missing (p50 0.94: slower than real time);
four slots (B12) with two threads 0 missing (at most 6.4 ms, p1 1.25, p50 1.91), with one all missing (p50 0.75).

What this says, within its limits: the machine's speed margin is about 2.4 × for a light patch and 2.0 × for a
heavy one at the median, but its slowest chunks fall below real time, and a run of them takes the reserve: with
the old priority a heavy patch beside other work lost audio even with 20 ms. One DSP thread is not enough for
heavy patches on this computer.

### 3.4 The machine's threads at real-time priority [measured, alternated runs; provisional]

On macOS the Runner's thread and the DSP threads become time-constraint threads (period 1 ms, computation 0.5 ms,
preemptible: `makeThreadRealtime` in `emu/g2emu/src/thread.hpp`), as a host's audio threads are, when
`Machine::Options::realtimeThreads` is set; the plugin sets it (`G2FRESH_EMULATOR_REALTIME=0` turns it off). On
Windows and Linux nothing changes, and where macOS refuses the threads stay as they were
(`Runner::Stats::realtime` and the plugin's diagnostics say which). What the machine computes is the same.

* **exp1** (20 s, heavy, 20 ms reserve, load 63 to 178): blocks of 64 and 512 frames. 0.10.2's code, and the new
  code without real-time priority, with or without a 3 ms stretch: **all of the audio missing** (chunk speed p50
  0.72 to 1.0). With real-time priority, with or without the stretch: **none missing**, chunk speed p1 1.76 to
  1.81, p50 2.03 to 2.15.
* **exp3** (20 s, heavy, real-time, 5 ms reserve, load 26 to 36): 32 frames, no stretch: none missing, reserve
  taken at most 0.08 ms, chunk speed p1 1.95, p50 2.32, wake-up p99 0.006 ms, max 0.011; with a 2 ms stretch: none
  missing, at most 0.67 ms taken (the stretch lets the level dip by design), p1 1.76, p50 2.02. 64 frames: none
  missing either way (at most 0.06 ms; p50 2.15 and 2.04). The CPU time of the two (the reason for the stretch)
  was printed but not kept: not known.
* **Matrix, real-time and a 2 ms stretch, 5 ms reserve, 60 s per run, load 35 to 64**, light patch (the heavy and
  four-slot rows were lost):

  | Block | Missing | Reserve taken ms p50 / p99 / p99.9 / max | Chunk speed p0.1 / p1 / p50 | Refill speed min / p1 | Wake ms p99 / max | Longest chunk ms | CPU cores | Load |
  |---|---|---|---|---|---|---|---|---|
  | 32 | 0 | 0 / 0.36 / 0.39 / 0.72 | 1.88 / 2.01 / 2.38 | 1.62 / 2.20 | 0.007 / 0.011 | 1.4 | 1.33 | 50 |
  | 64 | 0 | 0 / 0.04 / 0.06 / 0.08 | 2.00 / 2.11 / 2.39 | 2.08 / 2.22 | 0.007 / 0.013 | 0.6 | 1.31 | 48 |
  | 128 | 0 | 0 / 0 / 0 / 2.35 | 1.88 / 1.98 / 2.36 | 1.01 / 2.14 | 0.007 / 0.023 | 3.9 | 1.36 | 52 |
  | 256 | 0 | max 0.08 | p1 2.10, p50 2.38 | 1.42 / 2.20 | 0.009 / 0.014 | | | 47 |
  | 512 | 0 | max 0.08 | p1 2.03, p50 2.37 | 1.80 / 2.24 | 0.010 / 0.016 | | | 56 |
  | 1024 | 0 | max 0.07 | p1 2.05, p50 2.41 | 1.90 / 2.31 | 0.012 / 0.016 | | | 64 |
  | 2048 | 0 | max 0.07 | p1 1.96, p50 2.41 | 2.21 / 2.28 | 0.018 / 0.026 | | | 35 |

  Compared with part 3.3: the wake-ups are 10 to 50 times shorter at their worst and the slowest chunks twice as
  fast; the reserve taken stayed under 2.4 ms in all of them.
* **The breath.** XNU demotes a real-time thread that has computed for more than 100 quanta (1 s) since it last
  waited, for twice as long (`osfmk/kern/priority.c`, `thread_quantum_expire`; `sched_prim.c`:
  `MAX_UNSAFE_RT_QUANTA` 100, `SAFE_RT_MULTIPLIER` 2). The machine's threads compute without a wait while they are
  behind (the boot, an upload, after a stall). So every 50 ms of work they sleep for 20 us (`RealtimeBreath`).
  **exp5** (20 s, heavy, real-time, 1 ms stretch, 5 ms reserve, beside 8 threads that compute without a pause
  (`g2audiopath burn 8`), one round): 128-frame blocks, without the breath **all of the audio missing** (chunk speed
  p50 0.73), with it **none** (p1 1.90, p50 2.05, wake-up max 0.034 ms), and 0.10.2's way 126 078 frames missing
  (1029 short blocks); 512-frame blocks, none missing without the breath either. **exp4** (the same without the
  burners, load about 21): none missing with or without it. A second, five-round comparison (exp6) was started and
  its results were lost: the breath rests on one round and on the kernel's documented rule. It costs 0.04 % of the
  threads' time and does nothing elsewhere than on macOS with real-time threads.
* **Not measured**: what real-time emulator threads do to the rest of a busy host (other plugins' audio threads,
  which have the same priority; two or more G2fresh instances at once, each with three such threads, on four
  performance cores). The sweep's `two` and `audio` plans measure the emulator's side of it. Joining the host's
  audio workgroup (JUCE `audioWorkgroupContextChanged`, macOS 11+) would be the cooperative way; not done.

### 3.5 Checks [measured]

* `tests/test_g2emu.cpp`, "the reserve and the reader's block are the latency, and MIDI lands by it" (user's OS):
  2 ms reserve, 1 ms stretch, 32-frame reads: latencyFrames() is 192 + 96 + 10; the machine is never further ahead
  of the reader than that; a note lands at the same place after the latency when the reserve changes to 10 ms while
  it runs; no frame missing (the reader waits for the machine, so this checks the bookkeeping, not real time).
* The plugin's reported latency against what comes out: `g2plugintest --inputs` (an input tone's delay by its phase,
  measured against the reported latency); see the report of the run for the figures and the load it ran under.
* The converter's delay in the latency: part 2.4.

### 3.6 The sweep: one command, on a quiet computer

    cmake --build build -j 2 --target g2audiopath
    python3 tools/audiopath/sweep.py --fw "PATH/TO/Nord Modular G2 OS v1.62 Update.dmg"

About an hour (`--quick`: 20 s per run). It refuses to start while the 1-minute load average is above 2, waits for
it before each run, records the 1, 5 and 15-minute load averages before and after every run, and writes
`build/audio-path-sweep/runs.jsonl` and `table.md` (per setting: the runs that would have dropped out with that
setting's reserve). Its plans: quiet (three patches, blocks of 32 to 2048 frames), one DSP thread, beside busy
threads (it starts them itself: `g2audiopath burn`), beside real-time ones, two emulators at once, Low's and
Normal's reserves as they are, and 0.10.2's way. The runs use the plugin's configuration (real-time threads, the
1 ms stretch) and Safe's reserve. Then: the default and the reserves from `table.md`, and this part rewritten
without "provisional".
