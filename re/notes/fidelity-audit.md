# Fidelity audit: G2fresh against the original G2 and its editor

Date: 2026-10-11, at commit 775fc55. Rule (the user's, 2026-10-11): every feature behaves as the original does: the
G2/G2X hardware and its OS 1.62, and Clavia's editor v1.62. Every deviation is deliberate, opt-in when it changes
what the user hears or sees, and flagged. Part A covers the emulator, the plugin and the Live view; part B the
editor. Each part lists the original behavior with its source, G2fresh's, the gap, a severity and a recommendation;
"verified" marks what was measured (in the emulator running the user's OS) or read in the code or decompilation,
"inferred" the rest. Fixed since the audit: Out 3/4 no longer mixed into Out 1/2 by default (manual: the headphones
carry Out 1/2 only).

## Ranked gaps (both parts)

**Wrong results: a different patch, file or sound**
1. Old patch files (versions 13-22, about 42 % of the public corpus) are not converted to version 23 on load as the
   original does (`CFileConverter::Convert`): they display, save and send differently. (B)
2. Morph model: one morph group per parameter; the original allows several (manual p.91; 98 factory patches), with a
   limit of 25, non-morphable parameters and range clamping. (B)
3. Cables have no net model: two-output nets and loops are possible, deleting a module does not splice the chain,
   the colour rules are missing, Break / chain Delete / Delete Unused Cables are missing. (B)
4. Live editing over USB: structural edits re-upload the whole patch; the patch name, performance settings and name
   are never sent; knob-map notices and store errors are ignored. (B)
5. Emulated G2X has a G2's polyphony: the expansion board's 4 DSPs are stubs (measured: 3 voices where 32 are asked). (A)
6. The emulated G2 starts from an erased memory, not a factory G2: MIDI Ctrl off (CC7 and assigned CCs ignored), all
   slots on channel 1, program change off, memory protect off, no factory patches. (A)
7. MIDI CC assignment allows any CC (bank select, mod wheel, sustain, 7, 17...), silently replacing fixed mappings. (B)
8. Module placement and overlap resolution differ (groups split, unrelated modules moved, Fx-In in the voice area). (B)
9. Sequencer Clr/Rnd buttons latch at 1 (and are saved) instead of acting momentarily. (B)
10. The Patch Mutator auditions in the user's variation (the original: hidden variation 9) and goes to 100 % (original: 50 %). (B)
11. Names are not filtered to the G2 character set (accented names make performances unsaveable). Parameter rename is
    offered everywhere and radio-button labels are written malformed. (B)

**Sound and timing in the plugin (A)**
12. Resampling from 96 kHz without an anti-alias filter (aliasing, inferred from the algorithm).
13. No audio inputs (In modules silent; no mic, no In 1 Level).
14. Tempo sync on by default, and Local / clock receive forced on: changes what the user hears, should be opt-in.
15. Master Level knob not saved, and the on-screen knob can disagree with the machine.
16. The G2's MIDI OUT is dropped (notes, CC, sysex dumps) and its queue grows without bound; incoming sysex skipped.
17. Output level convention (x8) unverified against hardware; latency 26-32 ms against about 2 ms on a G2.

**Interaction (A, B)**
18. The Live panel holds one button at a time: no Shift functions, no hold-and-turn gestures.
19. Performance workflow: New To / Open To / Save From, the performance name, slot rules, Perf mode; new performances
    get MIDI channels 0-3 instead of 0.
20. Missing original editor features: per-slot undo (100 levels), Paste Params, Delete Unused Modules, Init 1&2, Var
    Init recall, Parameter Pages / Overview, MIDI Learn and the controller dialog, Synth Settings, bank delete and
    upload/download, Patch Adjuster, keyboard focus, Options dialog, F5-F8 hints, module help, virtual keyboard over
    USB, controller snapshot / Dump One, Patch Browser disk tab, Window menu, About.

**Deviations on by default or unflagged (B)**
21. Modern look, cable animation, mouse wheel on knobs, double-click resets a knob (original: starts a morph),
    single-click insert from the module bar, immediate paste with renaming, Cmd-D / Cmd-L / Cmd-1..8 differ, offline
    load estimate (original: "--"), "discard changes?" prompt (the original never asks), the stand-alone app restoring
    unsaved edits as saved. No "Deliberate deviations" section in the README.

**Matches the original (no action):** parameter display texts, module graphs (except DXRouter's picture), the patch
load computation, the mutator's core, byte-exact round trips of unedited files, the USB handshake and the live
parameter/mode/CC/variation messages, uprate, Replace; keys C1-C6, the pitch stick, the mod wheel, the global wheels'
CCs, the slot LEDs, external clock handling by the OS.


# Part A — the emulator, the plugin and the Live view


Date: 2026-10-11, at commit 775fc55. Nothing in the repository was changed.

**Rule applied.** Each feature should behave as the original does: the G2/G2X hardware running OS 1.62, and Clavia's editor where it applies. A deviation must be deliberate. If it changes what the user hears or sees, it must also be opt-in and flagged.

**Sources and evidence tags**
- **[M p.N]**: page N of the G2 user manual v1.2 (`original/manual/g2-manual-v1.2.txt`, "=== PAGE N").
- **[Photo]**: `original/photos/`.
- **[Notes §x]**: `re/notes/g2-hardware-and-emulation.md`.
- **[Measured]**: I ran it this session on the user's OS 1.62 in g2emu. I used `build/emu/g2emurun`, plus small probe programs linked against the built libraries. The probes live outside the repository, in `/Users/romainfaraut/.claude/jobs/17960a5e/tmp/emu/` (`probe.cpp`, `probe2/3/4.cpp`, `voices.cpp`, `sysmenu.sh`, with their outputs). The model was G2X, the flash erased, and the settings the plugin forces (Local On, external clock accepted) were applied unless stated otherwise.
- **[Code]**: read in the source (file:line).
- **[Inferred]**: my deduction, not checked.

---

### Top gaps, most severe first

| # | Gap | Severity | Evidence |
|---|---|---|---|
| 1 | **Polyphony is a G2's, not a G2X's.** We present a G2X, but the expansion board's 4 DSPs are stubs, so the OS uses 4 DSPs. A heavy patch gets 3 voices where a G2X gives 7. | **High** | [Measured] `voices.cpp`: `bigsynth.pch2` requesting 32 voices gets "03 (32)" on both the G2X and G2 models; [M p.121]: "minimum polyphony is three voices plus effects or seven voices plus effects in an expanded or G2X model" |
| 2 | **An erased flash gives non-factory system settings.** MIDI Ctrl is Off, so CC7, CC11 and every assigned CC from the DAW are ignored. Slot channels are 1/1/1/1 and the Global channel is 1, so multitimbral use is impossible. Program Change is Off. There are **no factory patches**. | **High** | [Measured] `sysmenu.sh`, `probe`: CC7=0 has no effect with the OS default, and drops the level with Ctrl on. Factory values: [M p.126] Global channel Off, [M p.39/49] Memory Protect On, [M p.21] factory sounds |
| 3 | **Downsampling 96 kHz to the host rate uses Lagrange interpolation with no anti-alias filter.** Content above the host's Nyquist folds into the audible band; a real G2 recorded at 48 kHz does not alias that way. | **High** for bright, FM or distorted patches | [Code] `EmulatedSoundEngine.h:86`, `.cpp:137-138`. [Inferred] from the algorithm: at 96→48 kHz a 4-point Lagrange returns every second input sample, i.e. plain decimation. Not measured |
| 4 | **Audio inputs are absent.** There is no input bus, the ADCs read silence, and the Mic Level knob is decorative. Patches using 2-In/4-In (effects, vocoders) are silent. | **High** for those patches | [Code] `PluginProcessor.cpp:38-40` (outputs only), [Notes §3.9.6], `LiveView.cpp:311`; [M p.26, p.53] |
| 5 | **Live panel: only one button can be held at a time.** Shift functions (Store As, Dump One, Panic, Split Point, Global octave, Shift+Slot, Vari.Init, Clear) and hold-and-turn gestures (hold Slot + dial, hold Morph + knobs, hold Down + dial for naming) cannot be done with the mouse. | **Medium-high** (functional) | [Code] `LiveView.cpp:333-378`: a single `pressed_`, released on mouse-up; [M p.27-31, p.47, p.44] |
| 6 | **The Master Level knob is neither saved nor kept in sync.** It is full at every start. The UI resets to full whenever the editor reopens, while the machine keeps the old value. | **Medium-high** (loudness jumps: knob at 0.5 = −15.7 dB) | [Code] `LiveView.cpp:257` and `MainView.h:134` (LiveView belongs to the editor); [Measured] master 1 / 0.5 = 0 / −15.7 dB |
| 7 | **MIDI OUT is dropped.** The OS transmits notes from the Live keyboard, CCs from the wheels and knobs, clock, Dump One sysex and the MIDI modules' output; the plugin outputs nothing. Incoming sysex is also dropped. The OS's MIDI OUT queue is never drained, so it grows without bound. | **Medium** | [Measured] Live key → `90 45 66 80 45 72`; mod wheel → `b0 01 7f`; G.Wheel 1 → `b0 60 7f`; Dump One → 1099 bytes of sysex. [Code] `PluginProcessor.h:38` `producesMidi() false`; `EmulatedSoundEngine.cpp:110-111` skips sysex; `machine.cpp:807-808, 1091-1097` |
| 8 | **Output reference level is unverified.** The ×(−8) scaling puts signal 1.0 at 0 dBFS. That is 18 dB hotter than "DAC full scale = 0 dBFS", and the DAW sees up to +18 dBFS before the G2's own clip point. | **Medium** (convention; unknown against a real G2) | [Code] `EmulatedSoundEngine.cpp:126-132`; [Notes §3.9 "Master volume"]; [M p.53] −10 dBV line outputs |
| 9 | **Host tempo is on by default and the G2's own clock setting is forced.** The patch's own tempo is replaced, and "MIDI Clk Recv" is forced On (and Local On) at every start, so the G2's own setting cannot be kept. | **Medium** (changes what is heard; flagged in the README but not opt-in) | [Code] `PluginProcessor.h:113`, `.cpp:416-427, 574`; [Measured] the OS follows Start/Stop/Continue (Run/Stop on the display) |
| 10 | **Live latency is 26-32 ms** (Runner buffer of 20 ms plus a host block). A G2 plays about 2 ms after the MIDI bytes. | **Medium** for live play | README:42; [Notes §3.9.2, §3.9.4] |

---

### 1. Audio outputs

| Item | Original | G2fresh | Gap | Severity | Recommendation (effort) |
|---|---|---|---|---|---|
| Out 1-4, headphones | Four unbalanced line outputs at −10 dBV; "The HEADPHONES output routes audio signals which are assigned OUTPUT 1 and 2" [M p.53]. The Master Level knob controls all four outputs and the headphones [M p.26] | Bus "Out 1/2" is on by default. Bus "Out 3/4" is off by default (`PluginProcessor.cpp:38-40`). Out 3/4 are not mixed in by default (`PluginProcessor.h:114`; state default false at `.cpp:575`). The opt-in mix is `EmulatedSoundEngine.cpp:143-149` | **Matches** (fixed in 775fc55; confirmed in code) | – | – |
| Mono | "If only OUTPUT 1 is connected, the audio from OUTPUT 2 is mixed to OUTPUT 1" [M p.53] | (Out 1 + Out 2) × 0.5 (`EmulatedSoundEngine.cpp:150-154`) | Halved; the G2's summing gain is unknown | Low | Document it, or use 1 + 2 without halving if a real G2 measures so (trivial) |
| Output level reference | DAC words saturate at full scale; the analog stage is −10 dBV nominal [M p.53]. The DAC full-scale voltage is **unknown** [Notes §3.9] | Words × (−8), so signal 1.0 ↔ 0 dBFS and DAC full scale ↔ +18 dBFS (`EmulatedSoundEngine.cpp:126-132`). The clip point is faithful (in words) but sits at +18 dBFS in the host | Reference differs from "DAC full scale = 0 dBFS" by 18 dB. Unflagged in the UI | Medium | Measure a real G2: a 1 kHz sine at Out level 127, in dBV at OUT 1. Document the choice; optionally offer "G2 DAC reference (−18 dB)" as an option (small) |
| Polarity | DAC words are inverted relative to the signal [Notes §3.9]. Whether the analog stage inverts again is unknown | Re-inverted by the −8 | Unknown | Low | Check on hardware: a pulse patch into a scope or interface |
| Master Level knob | A physical pot; its position persists. The OS reads it through the ADC (128-step table) [Notes §3.9]. No MIDI CC [M p.26] | Full at every start (`machine.hpp:53`). The UI knob is `LiveView.cpp:257` and is recreated with the editor, so it desyncs from the machine. Not in the project state | Not persisted; UI desync. The taper is faithful (the OS's table) [Measured: 0.5 → −15.7 dB] | Medium-high | Keep the value in the processor and in the state; the UI reads it from there (small) |
| Resampling 96 kHz → host | The G2 runs at 96 kHz [Notes §1.1; OS pitch tables exact at 96 kHz]. A recording at 48 kHz goes through the interface's anti-alias filter | JUCE Lagrange, 4-point, per channel, no low-pass (`EmulatedSoundEngine.h:86`) | Aliasing below the host's Nyquist [Inferred] | High (bright patches) | A proper polyphase/FIR resampler, e.g. a 96→48 half-band plus a fractional stage, or r8brain/libsamplerate (medium). Verify with a 30 kHz sine patch at a 48 kHz host |
| Sample rate | 96.000 kHz (possibly 98.3; open [Notes §4 Q2]) | 96 000 (`machine.hpp:26`) | Matches as far as known | Low | – |

### 2. Audio inputs

| Item | Original | G2fresh | Gap | Severity | Recommendation |
|---|---|---|---|---|---|
| IN 1-4 | Line inputs at −10 dBV, into the Input modules of all four slots [M p.53, p.~139 (line 5669)] | No input bus; the ADCs give silence ([Notes §3.9.6]; `PluginProcessor.cpp:38-40`) | Missing | High (effect and vocoder patches) | See the planned features below |
| Mic (XLR) | A preamp for dynamic microphones; the IN 1 jack overrides it [M p.53]. On the G2X the XLR is on the top panel [Photo g2x-left]. The Mic Level knob affects the XLR only [M p.26] | The knob is drawn at a fixed 20°, not interactive (`LiveView.cpp:311`) | Missing; the knob is decorative | Medium | Tooltip "not emulated" now; later, a gain stage on In 1 when "mic" is the source |
| In 1 Level LEDs (−20, −12, 0 dB) | Show the mic, or IN 1 when nothing is in the mic [M p.26]. Driven by the OS from DSP data (thresholds 0.1/0.25/0.89) [Notes §3.10.3] | Mapped (`PluginProcessor.cpp:319-321`); always off (no input) | Follows once inputs exist | – | – |

### 3. Pedals and controllers

| Item | Original | G2fresh | Gap | Severity | Recommendation |
|---|---|---|---|---|---|
| Sustain pedal | A jack; polarity "Sust Ped Pol" [M p.31, p.53]; morph group 5 "G.Wh1/Sus.P" | The emulator supports it (`machine.hpp:137`). The plugin/UI does not expose it. MIDI CC64 works even with MIDI Ctrl Off [Measured] | No on-screen pedal (CC64 from the DAW works) | Low | A sustain toggle in the Live view, plugged only when used (small) |
| Control pedal | A jack; Ctrl Ped Gain ×1.00-1.50 [M p.31]; morph group 6 | ADC position 2 exists in the emulator (`panel.hpp:56`) but not in the UI's `PanelAnalog` (`G2Panel.h:52`). It reads 0 | The unplugged value on a real G2 is unknown [Notes §3.10.5] | Low | Expose it as an optional slider; check the unplugged reading on hardware |
| Keyboard | G2X: 61 keys C1-C6; G2: 37 keys [M p.15] | Always 61 keys, mapped 36-96 → keys 0-60 (`PluginProcessor.cpp:393-401`, `LiveView.cpp:615`). C1 is verified ([Code] `tests/test_g2emu.cpp:1251-1259`) | Matches the G2X | – | – |
| Velocity | Contact time → falling 128-entry table plus a per-key offset [Notes §3.10.4] | Mouse height → velocity → an exponential fit of the contact time, without per-key offsets [Measured: requested 100 sends 102] | Approximate | Low | Invert the OS table exactly, and read the per-key offsets from RAM (small) |
| Aftertouch | The G2X keyboard has channel aftertouch: ADC position 3, inverted, 128-step curve [Notes §3.10.4]; morph group 4 | Not in `g2ui::PanelAnalog`. The emulator has `PanelAnalog::Aftertouch` (`panel.hpp:57`). MIDI channel pressure from the DAW reaches the OS | No aftertouch from the Live keyboard | Low-medium | Pressure from mouse dwell or a modifier, or a slider; route it to ADC 3 (small) |
| Pitch stick | Sprung to the centre; right → upper morph limit, left mirrored [M p.46]. The bend range is the patch's Bend setting [M p.34] | Horizontal, springs back on mouse-up (`LiveView.cpp:505-510`). 1.0 bends up [Code] (test `test_g2emu.cpp:1261-1275`, OS semantics) | Matches. The hardware wiring (which physical end is 0xFF) is not verified, but agrees with [M p.46] | Low | – |
| Mod wheel | Not sprung; vibrato and morph group 1 [M p.27] | Vertical, up = more (verified on the OS's vibrato in the test) | Matches | – | – |
| G2X global wheels | Hardwired to morph groups 5 and 8, not affected by keyboard focus [M p.27]. CC96/97 [M p.127] (the manual's CC table says GW1 = group 8, which contradicts p.27 and the panel print) | Lying across; right = more (`LiveView.cpp:499-501`). [Measured] GW1 = 1.0 sends `b0 60 7f` (CC96 = 127) | Direction of the physical wheels unverified (README:43). Matches the OS's semantics | Low | Check the direction on a G2X |
| Wheel LEDs | Not in the OS's LED matrix; probably powered [Notes §3.10.3, Inferred]. [Photo g2x-left]: green dots on all three wheels of a powered G2X (LCDs lit); I cannot tell a lit LED from a green lens | Always lit while the G2 runs (`PluginProcessor.cpp:359-361`) | Probably matches [Inferred] | Low | Confirm on a powered G2X in a dark room |
| Octave shift, KB Hold, split | OS functions [M p.27-28] | Keys go through the panel, so the OS applies them (`LiveView.cpp:782-796`) | Matches (OS-driven). The Shift-functions (Global octave, Split Point, Panic) are blocked by gap #5 | see #5 | – |

### 4. Front panel (Live view)

| Item | Original | G2fresh | Gap | Severity | Recommendation |
|---|---|---|---|---|---|
| Two-handed operation | Shift+button, Slot+dial, Morph+knobs, Down+dial [M p.27-31, p.44, p.47] | One mouse pointer; the button is released on mouse-up (`LiveView.cpp:333-378`). No latching, no modifier mapping | **Shift-functions are impossible** [Code, Inferred UX] | Medium-high | Make Shift, Slot A-D, Morph, Focus/Copy and Down latch on Alt-click or right-click, or map the computer's Shift key to the panel Shift; allow turning while a button is latched (small) |
| Layout | The G2X panel [M p.25 drawing, Photo] | Laid out from the manual's drawing; labels as on a production G2X | Matches. The slot LEDs map correctly: above = Keyboard Assign, below = Active Slots/Focus [M p.17]. This also answers the "label not known" in [Notes §3.10.3/§3.10.5] | – | Update the notes with the M p.17 citation |
| Look | Red body, navy/purple cheek, light-grey panel, yellow-green backlit LCDs with dark text, red Store button, light Shift button [Photo] | Graphite surfaces, cyan LEDs and text, dark LCDs (deliberate, commented at `LiveView.cpp:50-60`) | A deliberate, flagged, cosmetic deviation | Low | Optionally a "Classic" panel skin (the editor already has a Classic look) (medium) |
| LED colours | Not established from the photos (all off in them): **check** | Cyan | Unknown | Low | Check on hardware |
| Dimmed LEDs | The manual says Perf.Mode is "dimmed" in Patch mode [M p.28], an inactive slot's LED is "dimmed" [M p.17], and Patch Settings "dims" [M p.33] | The OS's LED buffer is on/off only [Notes §3.10.3]. [Measured] 500 samples at 1 ms show no toggling; Perf.Mode (17) is off in Patch mode | Either the manual's wording is loose or the dimming is in hardware; **unknown** | Low | Check on a real G2 in Patch mode. If hardware dims, model a "dim" state for those LEDs |
| Displays: characters | HD44780 16×2, A00 ROM; the OS defines user characters (g, j, p, q, y, ±, a symbol, a bar) [Notes §3.10.2] | Drawn with the system monospace font from `text()` (`LiveView.cpp:230-243`). User characters with no descender become ▒ (`panel.cpp:447-453`), so ± and the bar glyphs are lost | Glyph shapes and some symbols differ | Low-medium | Draw a 5×8 dot matrix with an A00 font plus `cgram` (the API exists: `panel.hpp:118-125`); pass `cgram` through `PanelSnapshot` (small-medium) |
| Displays: cursor | An underline or blinking cursor when naming (Synth Name, Store As) [M p.31, p.51] | `cursor`/`cursorLine`/`cursorBlink` exist in g2emu (`panel.hpp:111-113`) but are not copied (`PluginProcessor.cpp:347-355`) | **No cursor**, so naming on the panel is blind | Medium-low | Copy the cursor into `PanelSnapshot` and draw it (small) |
| Knob rings | 15 LEDs per knob [Notes §3.10.3] | 15 segments over 300° | Matches | – | – |
| Dial | Accelerates when turned fast (step up to 15) [Notes §3.10.4] | One detent per 8 ms, no acceleration (`machine.hpp:125-127`) | Slower browsing through 4096 locations | Low | Let a fast wheel or drag produce closely spaced detents (small) |
| Encoders | Every transition is a step; the first after a direction change is dropped [Notes §3.10.4] | One UI step = one parameter step (`machine.hpp:125`); 5 px per step | Matches the OS's semantics | – | – |

### 5. MIDI

| Item | Original | G2fresh | Gap | Severity | Recommendation |
|---|---|---|---|---|---|
| Local On/Off | A user setting, memorized [M p.30, p.25]. The figure shows "On". Erased flash: **Off** [Measured] | Forced On whenever it reads Off (`PluginProcessor.cpp:416-427`), and written to flash | The user cannot keep Local Off. Benign while MIDI OUT is not routed | Low | Force it only once (first boot of an empty flash), then respect the setting (small) |
| Slot and Global channels | Factory Global = **Off** [M p.126]. Figure: slots "1 2 3 4" [M p.30, inferred factory]. Erased flash: slots **1 1 1 1**, Global **1** [Measured] | Left as the erased flash has them | Only channel 1 plays. A performance's slots all listen on channel 1, so the G2's multitimbral workflow (4 tracks → 4 slots [M p.125]) needs manual setup | Medium | On an empty flash, initialise to the factory values: slots 1-4, Global Off (small) |
| MIDI Ctrl (CC) | Figure: "Send & Receive" [M p.31, inferred factory]. CC7 = patch volume, CC11 expression, CC96/97 global wheels, and user CCs [M p.126-127]. Erased flash: **Off** [Measured] | Not set | **CC7 and assigned CCs from the DAW are ignored** [Measured: CC7=0 no effect; with Ctrl on, rms 0.70 → 0.10]. CC64 still works; CC1 unverified | High | Initialise to Send & Receive on an empty flash (small) |
| Program Change | Figure: "Send & Receive" [M p.31]. Erased flash: **Off** [Measured] | Not set | DAW program changes ignored | Low-medium | Initialise as above |
| MIDI clock in | Clk Recv: external clock takes over and the display shows the tempo [M p.33, p.31]. Erased flash: Recv **On** [Measured]. Figure: Off | `HostClock.h` sends F8 always (also while stopped), FA from the top, SPP+FB otherwise, FC on stop and on jumps. On by default (`PluginProcessor.h:113`); Clk Recv forced On | [Measured]: the display reads "Ext 60/90/120/140/180" for 60-180 BPM (after averaging; briefly "Ext 122"/"143" while settling). FA → Run, FC → Stop, SPP+FB → Run. So the G2 behaves as a G2 slaved to a DAW. But it is **on by default** and overrides the patch tempo | Medium (heard) | Make "Follow host tempo" explicit at first use (or opt-in per the rule), and respect the G2's own Clk Recv instead of forcing it (small) |
| MIDI clock out / Start-Stop out | Clk Send sends clock; Run/Stop sends Start/Stop [M p.33] | Not routed | Missing (part of #7) | Low | Route it with MIDI OUT |
| Internal clock accuracy | Unknown | [Measured] 120 BPM internal → 48.3 F8/s (+0.6 %) on MIDI OUT | Possibly emulator timing (timer period TRR vs TRR+1, ±2 % [Notes §3.9.2]) or the OS's own table; **check on hardware** | Low | Compare a real G2's F8 rate at 120 BPM |
| MIDI OUT | Notes, CCs, clock, Program Change, sysex dumps, MIDI modules [M p.30, p.29, p.126-131] | `producesMidi() false` (`PluginProcessor.h:38`); `Machine::takeMidiOut` is never called, so the queue grows forever (`machine.cpp:807-808`) | All of it is lost; a small leak | Medium | A lock-free MIDI OUT ring in Runner; drain it on the audio thread into the output MidiBuffer; `producesMidi() true` (medium) |
| Sysex in | Patch and performance dumps over MIDI IN [M p.131-]; studios store sounds this way [M p.16] | Skipped (`EmulatedSoundEngine.cpp:110-111`). `midiInAt` takes 16-byte pieces (`runner.hpp:68`) | Missing | Low-medium | Pass sysex through in pieces (the UART paces it at 31250 baud: about 1.6 s per 5 kB) (small) |
| MIDI LED | Long blink for used messages, short for ignored ones [M p.26] | OS-driven (`PluginProcessor.cpp:318`) | Matches | – | – |

### 6. Memory, startup, performance and slots

| Item | Original | G2fresh | Gap | Severity | Recommendation |
|---|---|---|---|---|---|
| Initial flash | Shipped with factory sound banks and performances [M p.21, p.47-49]; Memory Protect **On** [M p.39, p.49]; Synth Name "ModularG2" (figure, M p.31) | Erased: "-:- No Cat", "No patches stored" [Notes §3.10.2]; Memory Prot Off, Name "" [Measured] | No factory content; system defaults differ (see §5) | High (user expectation) | "Initialise as shipped": set the factory system values and, opt-in, fill banks from patch files the user downloaded from Nord (store over USB). Ship no Clavia content (medium) |
| Memory per instance | One G2 has one memory | Each plugin instance keeps its flash in the project; a new instance starts from the stand-alone's flash (`PluginProcessor.cpp:122-128`) | Deliberate: project recall. A patch stored in one project is absent from the others | Low | Flag it in the UI; optionally a "shared memory" mode |
| Loading a patch | Panel: Load Patch goes into the focused slot; the other slots and the performance stay [M p.35, p.47]. Clavia's editor sends a patch to one slot | The document goes out as a **whole performance** (`Performance::playing`, `core/src/patch.cpp:404-416`; `SynthSync.cpp:217-228`): slot A enabled and on the keyboard, B-D replaced by empty patches and disabled, header reset | Slots B-D and perf settings are wiped when the plugin (re)sends | Low-medium | Send to slot A only once the G2 is up (keep the performance), or flag the behaviour (small) |
| Patch mode | Perf.Mode LED off = Patch mode [M p.35] | After the upload: Perf.Mode off, slot A active, keyboard on A [Measured LEDs 49 and 177 lit, 17 off] | Matches | – | – |
| Boot time | Unknown for a real G2 ([M] gives no figure) | About 1.6 s emulated boot, USB at 2 s, sync 74 ms, then the upload [Notes §3.9] | n/a. An **offline bounce** right after a project opens renders the boot silence, and notes sent before the upload are lost | Low-medium | In offline mode, wait until synced and the patch is sent (small); later, the snapshot |
| One emulated G2 per host | Each track would be its own G2 | Only the first instance auto-starts (`PluginProcessor.cpp:209-212`); others stay silent until started | Deliberate, in the README (line 40). A project's sound depends on instance order | Medium | Show a clear "not started" state on the track; see the shared-G2 plan |
| Polyphony | G2X = 8 DSPs; "four more voices"; minimum 7 + FX [M p.19, p.121] | Expansion DSPs are stubs (`machine.cpp:61-75`), so the OS uses 4 DSPs [Measured: bigsynth 3 voices on "G2X"] | **Half of a G2X's resources** | High | Emulate the expansion board: 4 more DSPs and their links. Their topology needs reverse-engineering; about +1 core of CPU (large). Until then, say "polyphony of a G2 without expansion" in the UI |
| DSP timing | Real cycles at about 147 MHz | Instructions counted as clocks (1536 per frame) [Notes §3.6.4, §3.7.7] | Patches near 100 % load may behave differently | Low | – |
| ColdFire speed | MCF5407 V4 | V2 cycle tables, about 2/3 of the real throughput [Notes §3.9.2] | Background only (meters, upload time) | Low | – |

### 7. Latency and the USB/editor interplay

| Item | Original | G2fresh | Gap | Severity | Recommendation |
|---|---|---|---|---|---|
| Note latency | About 1.7 ms from the MIDI bytes [Notes §3.9.2] | 26-32 ms, reported to the host (README:42) | Live play feels late | Medium | A user setting for the buffer (for example 10 ms) once the emulator is faster; keep the PDC report (small) |
| Editing link | Clavia's editor over USB | The same protocol over the emulated USB (SynthSync) | Matches by design | – | – |
| Link watchdog | – | Restarts the link when it gives up (`PluginProcessor.cpp:182-187`) | n/a | – | – |

---

### Planned features: original behaviour to follow

**Audio inputs.**
- **What the original has:** IN 1-4, line level at −10 dBV [M p.53]; an XLR mic with a preamp on G2/G2X (on the G2X's top panel [Photo]); the IN 1 jack disables the mic [M p.53]; Mic Level affects the XLR only [M p.26]; the In 1 Level LEDs are OS-driven.
- **Following it:**
  - Add an input bus "In 1/2" and an optional "In 3/4", which DAWs can route as a sidechain.
  - Upsample to 96 kHz with a proper filter and feed A6's ADC receive slots (A6 takes the ADCs [Notes §0, §3.7]).
  - Scale so that host 0 dBFS maps to the same reference as the outputs (stay consistent with §1).
  - Offer an In 1 source switch, "Line" or "Mic". Mic applies a gain from the Mic Level knob, which is analog and not read by the OS.
  - Report the extra input-to-output latency.

**Instant start (snapshot).**
- **What the original does:** a G2 powers up with its last system settings [M p.25]. Whether it reloads the last performance on power-up is not stated in v1.2; **check**.
- **Following it:**
  - A snapshot must be keyed by the OS version and a hash of the flash, because the OS caches settings in RAM at boot. A snapshot made with one flash cannot be reused with another.
  - Restore it, then send the document as now.
  - Keep a cold boot as the fallback.

**Several tracks sharing one emulated G2 (4 slots).**
- **What the original does:**
  - One G2 has four slots, each on its own MIDI channel (factory slots 1-4, Global Off [M p.126]).
  - Slots share the DSPs; voice allocation is global [M p.121].
  - Every slot reaches only Out 1-4 through its Out modules; there are no per-slot outputs [M p.18, p.53].
  - MIDI on the Global channel acts like the keyboard, with splits [M p.125].
- **Following it:**
  - One owner instance runs the machine and its four outputs; the other instances send MIDI on their slot's channel.
  - A track's audio is separate only if its patch uses its own Out pair; say so.
  - Set slot channels 1-4 and Global Off as on a factory G2.

**Windows support.**
- The firmware comes from Clavia's Mac `.dmg` on every system (README:36). The Windows updater (Wise) is not read [Notes §4 Q7, §3.9.8]: either support it or keep asking for the `.dmg`, and say so.
- The JIT, the thread priorities and NoAppNap's equivalent need checking under Windows hosts.

**Implied by the gaps above.**
- **Expansion board** (true G2X polyphony).
- **Factory initialisation** of system settings, plus optional factory banks from the user's own downloads.
- **MIDI OUT**: notes from the Live keyboard (a real G2 acts as a master keyboard [M p.27]), CCs, clock, sysex.
- **Two-handed panel gestures**.
- **Master level persistence**.
- **Proper resampling**.
- **LCD glyphs and cursor**.
- **Aftertouch and pedals** in the Live view.

---

### Open questions only real hardware can answer
1. The DAC full-scale level at OUT 1 in dBV, and the output polarity.
2. Whether LEDs really dim (Perf.Mode in Patch mode, inactive slots) and their colours.
3. Whether the wheel LEDs are always lit, and the global wheels' direction.
4. The control pedal's reading when unplugged.
5. The real G2's F8 rate at an internal 120 BPM, compared with the emulator's 48.3 per second.
6. The factory system settings (MIDI Local, slot channels, Ctrl, Prg Chng, Clk Recv). The manual's figures suggest Local On, slots 1-4, Send & Receive, and Clk Recv Off; only "Global Off" and "Memory Protect On" are stated as factory values.
7. Whether the G2 reloads its last performance on power-up.


# Part B — the editor


Scope: everything except the emulator, the plugin sound engine and the Live front-panel view.

Repo state: `main` @ 775fc55. Read-only audit; no repository file was changed.

**Sources for the original**
- **Decompilation:** `re/out/decomp/<Class>.c`, cited as `Class::Method @address`.
- **Menu resource:** `original/rsrc/SMNU/128_Foo.bin` (the menu bar and its shortcuts).
- **Notes:** `re/notes/*.md`.
- **Manual:** `original/manual/g2-manual-v1.2.txt`. The "=== PAGE n" markers match the printed page numbers.
- **Cross-check:** Verhue's editor, `third_party/nord_g2_editor`.

**Method**
- Four parallel read-only passes:
  - files, lifecycle and menus;
  - canvas editing;
  - parameters, morph, mutator and display;
  - synth, USB, performance and plugin.
- I then re-checked the headline claims myself in the code and decompilation (marked "re-checked"):
  - `CPatch::NeedToBeSaved` returns 0;
  - `Patch::fromFile` has no version conversion;
  - `SynthSync::push` → `sendAll()` for structural edits;
  - the patch name is not in `g2::Patch`;
  - `edit::removeModule` erases cables without splicing;
  - `edit::connect` has no net rules;
  - `edit::setMorph` keeps one group per parameter (the manual p91 says "a parameter can also be assigned to several Morph groups");
  - `assignMidiCc` accepts 0-127;
  - ButtonText toggles;
  - `Skin.cpp:195-196` defaults to Modern and animated.

**Tags**
- **V** = verified in code (both sides when an address is given).
- **I** = inferred: check before acting (on hardware, in the emulator, or by further reverse engineering).

**Severity**
- **H:** the user gets a different patch, file or synth state.
- **M:** a missing or different workflow.
- **L:** cosmetic.

**Effort:** h = hours, d = days.

---

### 0. Top gaps, most severe first

| # | Gap | Sev | Effort |
|---|---|---|---|
| 1 | **Old file versions are not converted.** v13-22 load as-is. The original converts each to v23 through `CFileConverter::Convert @00051224` (`ConvertPatchNtoN+1` chain with per-module parameter remaps, morph group 4/5 swap at 20→21, and others). That is 42% of the public corpus (2,375 of 5,585 files; v22 alone is 1,602 files). These files display, edit, save and send differently from the original. | H | 3-6 d |
| 2 | **No cable-tree (net) model.** Connect allows two outputs on one net and cycles. Deleting a module or "Disconnect" removes cables instead of splicing the chain (manual p69). Colour rules are missing: a chain has one colour, and an unfed chain is white. Break, chain Delete and Delete Unused Cables are absent. | H | 2 d (+1.5 d for dependent features) |
| 3 | **Live editing does not send what the original sends.** Every structural edit re-uploads the whole patch: module add/delete/move/colour/rename, cables, header/voices, textpad, labels and seq zoom. Each upload triggers a release and a read-back, re-initialises the sound, and can race with realtime messages that are lost. Also: the patch name is never sent (`27`); performance settings and name are never sent (`11`/`29`); knob-map change notices `74`/`75` are ignored; "all variations" `40` (var 0x7F) is dropped; flash store errors `0D` are ignored; variation copy goes out as N×`40` instead of one `44`. | H | 3-6 d (+S items) |
| 4 | **Morph model is wrong.** It allows one group per parameter, but the original and manual p91 allow several; 334 corpus files and 98 factory patches use this. Also: no 25-morph limit; the morphable flag is ignored (110 parameters); the range clamp is ±127 instead of [0,max]; and popup "Morph assign" behaves differently. | H | 2-3 d |
| 5 | **MIDI CC validity is not enforced.** 0-127 is accepted, including bank select, mod wheel, sustain, CC70 and the reserved CC7/CC17. Assigning CC7 or CC17 silently breaks patch volume or octave shift. | H | 2 h |
| 6 | **Module placement and overlap resolution differ.** A dropped group gets split, unrelated overlapping modules get moved, there is no drop-below rule, and Fx-In can be placed in VA. | H | 1-1.5 d |
| 7 | **Sequencer Clr/Rnd push buttons latch at 1.** They are saved that way, every second click does nothing, and each click is an undo step. | H | 2-3 h |
| 8 | **Patch Mutator audition overwrites the user's current variation.** The original uses hidden variation 9. The range is 0-100% instead of 0-50%. | H | 1-2 d |
| 9 | **Names are not filtered to the G2 character set.** A UTF-8 accent makes a performance unsaveable (exception in `BitWriter::string`). A module rename fails silently. Non-G2 characters get written to files and sent to the synth. | M-H | 2-4 h |
| 10 | **Parameter rename is offered on every parameter.** Radio-button label records are written malformed, and custom labels from loaded patches are never drawn. | H/M | 1 d |
| 11 | **Performance workflow is missing.** No New To / Open To / Save From (building a performance from files, extracting a patch), no performance name, no slot rules (at least one slot, keyboard on an enabled slot, lower ≤ upper), no perf-mode toggle, and new-performance defaults differ (MIDI channels 0,1,2,3 instead of 0,0,0,0). | H/M | 1-2 d |
| 12 | **Deviations that are on by default and not flagged.** Modern look; cable animation; mouse wheel edits values; double-click resets (originally: morph); a single click in the browser inserts a module into VA; paste lands immediately and renames modules; shortcuts reassigned (Cmd-D, Cmd-L, Cmd-1..8); an offline load estimate shown as a figure (original: "--"); the unsaved-changes prompt (v1.62 never asks); restoring the stand-alone session with edits shown as saved. README has no "Deliberate deviations" section. | M | 1-2 d to make opt-in and document |
| 13 | **Missing original features.** Per-slot undo; Paste Params (Cmd-E); Delete Unused Modules; Init 1&2 / Save InitPatch; Var Init recall; Parameter Pages (Ctrl-F) and Overview (Ctrl-L); module Assign/Global Assign; MIDI Learn (L), CC dialog (M), Assign/Deassign MIDI to selection; Synth Settings (Ctrl-G); bank delete and Bank Upload/Download; Patch Adjuster (Ctrl-3); keyboard parameter focus and arrows; Options (cable style, knob mode, morph by double-click); F5-F8 hint overlays; module help; Virtual Keyboard over USB; Controller Snapshot; Dump One; Patch Browser Disk tab; multiple windows / Window menu; About. | M (each) | weeks in total |

---

### 1. Files, document lifecycle, menus

#### 1.1 Load and save

| # | Item | Original (source) | G2fresh | Gap | Sev | Recommendation (effort) | V/I |
|---|---|---|---|---|---|---|---|
| F1 | Old versions 13-22 | `CPatchFilePort::Load @0006cba4` calls `LoadFile(…,0x17)`, then `CFileConverter::Convert @00051224` chains `ConvertPatch12to13…22to23` (Global.c @000480c6…@0004f09c). Per-module `UpdateModule` remaps: RemoveParam, InvertParam, CubifyParam, LimitParam, ScaleParam, Convert_LFORate*. Also `ValidateCables` and `ValidatePartSelectors`. 14→15 adds morph variation 8; 20→21 swaps morph groups 4↔5 (also in global knobs); 21→22 adds a selector value to type 69; 22→23 sets the lock flag from the type. Performance variants too. | `core/src/patch.cpp:236-242` accepts 13..23 and keeps `version`/`textHeader`. No conversion code (re-checked). | Corpus: v19 176, v22 1,602, v13/14/18/20 a few. Performances: v18 13, v19 283, v22 293. Old files show and send different values. | H | Port the chain into `core` as `g2::convert`, called by `Patch::fromFile`. Golden-test against the original under emulation (as `tools/patchload/emulate.py` does). 3-6 d. Interim (1 h): status message "converted from v N / not converted". | V |
| F2 | Version written on save | `CPatch::SaveToFile @000da3f4` always writes `CPatchFileCurrent` (v23, "Info=BUILD 320"). | Keeps the loaded version and header (`patch.cpp:241, 344, 372`). | Consistent only because F1 is missing. | M | After F1: always write 23 / BUILD 320. 1 h. | V |
| F3 | "Convert" alert | `CFilePortUtils::ConvertAlert @000308c4`: "File will be converted to new format. [Cancel][OK]" when saving a converted patch. | None. | Missing. | L | With F1. 1 h. | V |
| F4 | Text header, CRC, round-trip | CRC-16/XMODEM, 4 header lines. | `file.cpp:539-682`; byte-exact round-trip test (`tests/test_file.cpp:140`). | **Matches.** | – | – | V |
| F5 | Load errors | XFileBase exceptions: no partial load. Messages "Checksum error. Could not load file.", "File corrupt.", "Unsupported file version", "File format is not supported!" (`CPatchFilePort::CreateFileReader @0006c73c`). | "Cannot open patch: …" (`MainView.cpp:971-985`). No partial load. | Same behaviour, different wording. | L | Optionally reuse Clavia's texts. 1 h. | V |
| F6 | Bad checksum | Hard failure. | Offers "Open anyway" (`MainView.cpp:972-980`). | User-chosen deviation. | L | Keep; list it under deviations. | V |
| F7 | Leniency | Exact section sizes (`CStreamSizer::VerifySize @00063c32`); file < 64 KiB (`ReadFromFile @0006c472`). | Accepts MacBinary, headerless files, padding, extra bytes, any size (`file.cpp:541-637`). Keeps non-zero pad bits and extra section bytes on save. | More lenient (reads files the original rejects). | L | Document it; zero the pads once the patch is edited; warn at ≥64 KiB. 2 h. | V |
| F8 | Release velocity | The v1.62 writer stores the attack velocity in the release slot (pch2-format §3.3). | Writes `releaseVel` as stored (`file.cpp:109-113`). | Differs only for synth-sourced patches. | L | 1 h. | V |
| F9 | .syx / .mid / NM1 import | Only "Nord Modular G2 File Format 1" is accepted. `CFileReader_Syx/MID/30/31` have no callers. | .pch2/.prf2 only; NM1 is detected and reported. | **Matches.** Do not add imports. | – | – | V |
| F10 | v12 files | 12→13 is converted; v11 is "Unsupported" (`CPatchFile_11::Convert @00052c70`). | <13 is rejected. | No v12 files are in the corpus. | L | With F1. | V |
| F11 | Cable bends sidecar | Originals have no bend points. | `<file>.g2layout` is written only when the user has bent a cable (`ui/CableLayout.h`, `MainView.cpp:1004-1009`). The .pch2 is untouched. | An opt-in addition. OK. | – | Document it in the README. | V |

#### 1.2 New patch and performance

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| N1 | New patch header | `CPatchHeaderData_11` ctor @00064952: 2 voices, mono mode 1, splitter 600, octave 0, 7 cable colours visible. | `patch.cpp:153-178` | **Matches.** | – | – | V |
| N2 | New patch name | "No name" (string resource 41; Verhue `BVE.NMG2Slot.pas:342` agrees). | "New patch" (`PatchDocument.cpp:218,230`). | Different name, sent to the synth and suggested on save. | L-M | Use "No name". 15 min. | V |
| N3 | New performance | `CreateNewPerformance @000fe694`: name "Empty perf", 4 enabled slots, keyboard and focus on A; MIDI channel 0 on every slot (`CSlotSettings` ctor @00064138); 120 BPM, stopped. | Channels 0,1,2,3; slot names "New patch"; no performance name (`PatchDocument.cpp:223-241`). | Different file. | M | Channels 0,0,0,0, "No name", "Empty perf". 30 min. | V |
| N4 | Init 1&2 buttons, Save InitPatch 1/2 | `HandleDefaultPatchSave @00149e1c` and `HandleDefaultPatchOpen @0014a5c8`: `<Prefs>/InitPatch1|2`, loaded into the active slot when connected. Manual p59, p99. | Absent. | Missing. | M | Menu items, two toolbar buttons, files in the settings folder. 4-6 h. | V |

#### 1.3 File, Patch, Performance, Window and Help menus

The original File menu (SMNU): New Patch ⌘N, New Performance, Open ⌘O, New To ▸, Open To ▸, Save ⌘S, Save As, Save All, Save From ▸, Save InitPatch 1/2, Close ⌘W, Close All, Recent Files ▸.

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| M1 | Several documents | One window per patch or performance (`CPatchManager`). | One document; New/Open replace it (`PatchDocument.cpp:135-269`). | Architectural deviation. | M | Document it as deliberate; a multi-window model would take weeks. | V |
| M2 | New To / Open To ▸ slot | `HandleMenuFileNewTo @00149cbc`, `…OpenTo @0014a212`. Manual p98-99, p94. | Absent. A performance can't be built from patch files offline. | Missing. | H | Replace `perf_->slots[i]`, undoable. 3-4 h. | V |
| M3 | Save From ▸ slot (extract a patch) | `HandleMenuFileSaveFrom @00149d00`; manual p97, p99. | Absent. | Missing. | H | 2 h. | V |
| M4 | Save (patch) | `CPatch::SavePatch @000da736`: saves in place if the file name equals the patch name, else Save As; afterwards the patch takes the file name. | `MainView.cpp:990-1040`. | **Matches.** | – | – | V |
| M5 | Save (performance) | `CPerformanceManager::SavePerformance @000fe198`: suggested name = performance name. | Suggests slot A's patch name (`MainView.cpp:1029-1031`). | Wrong suggested name. | L-M | With P2. | V |
| M6 | File-name filter | `RemoveNonFileChars @0014663e`. | `createLegalFileName`. | Different character set. | L | 30 min. | V |
| M7 | Save All, Close, Close All, Window menu (Cascade/Tile/Tile Active Slots ⌘I), open-window list | SMNU; manual p113-114. | Absent (single document). | Missing; follows from M1. | L | Document; optionally a slot A-D list. 1 h. | V |
| M8 | Unsaved-changes prompt | **Never shown in v1.62**: `CPatch::NeedToBeSaved @000d3214` returns 0 (re-checked), so `CSavePatchAlert @000f8d08` is dead code. Manual p99: "Any unsaved Patches will automatically be deleted." | "Discard changes?" [Discard]/[Cancel] on New, Open, Recent, Get and Quit (`MainView.cpp:918-930`, `StandaloneApp.cpp:93-110`). | Shows something the original never shows, and has no Save button. | M | My recommendation: keep it (silent loss is worse), but flag it as a deliberate deviation and add [Save] using Clavia's dormant dialog text ("Patch <name> not Saved!", Save / Don't Save / Cancel). 2 h. | V |
| M9 | Session restore (stand-alone) | Starts empty (`CEditorApp::Initialize @0014bfd8`). | Restores the last patch and marks it **saved** (`PluginProcessor.cpp:560-587`). Quit → "Discard" still saves the edited state (`StandaloneApp.cpp:101-106`). | Deviation plus a bug: Discard doesn't discard, and edits look saved. | M | Persist the dirty flag; on Discard store the file's bytes; document. 2-3 h. | V |
| M10 | Recent Files | 5 entries, added on open only, no "Clear" (`CPrefManager::AddRecentFileName`). | 12 entries, also added on save, plus "Clear Menu" (`MainView.cpp:59, 386-391, 1012`). | Minor. | L | 15 min, or document. | V |
| M11 | Textpad ⌘J (Mac) / Ctrl-H (Win) | Floating window, live, **not undoable**, max 1024 bytes, sends `6F` (`CDialogNotes`, `CPatch::SetTextpadString @000d8c54`). | Modal OK/Cancel "Patch Notes…" in the Edit menu, undoable, no shortcut. OK marks the patch edited even without a change and converts CRLF (`Dialogs.cpp:29-75`). | Different interaction. | L-M | Floating, live, ⌘J; apply only on change. 3 h. | V |
| M12 | Delete Unused Modules | `CPatchData::GetDeleteUnusedModulesMolecules @000e8d02`: iterates until stable, keeps flagged modules (Automate, CtrlSend, PCSend, NoteZone), splices chains. Manual p102. | Absent. | Missing. | M | After C1 (splice). 0.5 d. | V |
| M13 | Patch Settings ⌘P | Floating `CSynthFloater`; its controls have knob/CC assign menus. | Inline collapsible bar, no ⌘P, no right-click assign. | Layout differs; assign menus missing. | L/M | Bind ⌘P (15 min); assign menus 0.5 d. | V |
| M14 | Help menu, About | Keyboard Shortcuts (HTML), Check for Updates; Windows: Contents and About (`CDialogAbout`). | None. | Missing. | L | About + shortcut page. 3 h. | V |

#### 1.4 Names

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| NM1 | Allowed characters (patch, slot, performance, module names) | `NameUtils::IsModularChar @0002989c`: space, a-z, A-Z, 0-9 and ``-!"#$%&'()*+,./:;<=>?@[\]^_`{|}``. Patch names: illegal → space (`Utils::RemoveNonModularChars @001465e8`, manual p60). Module names: illegal keys refused with a beep (`CNameDialog::HandleChangeRequests @000327a4`). 16 bytes. | Truncates to 16 *characters*, stores UTF-8, no filter (`PatchDocument.cpp:142,186,199`; `Dialogs.cpp:97-100`; `AreaView.cpp:1176`). | **Bugs:** an accented 16-character slot name exceeds 16 bytes, `BitWriter::string` throws (`bitstream.cpp:102`) and the performance can't be saved. A module rename with accents fails silently. Latin-1/Mac-Roman names from files are decoded as UTF-8. | M-H | One shared filter at every entry point; decode file bytes as Latin-1. 2-4 h. | V |

---

### 2. Patch canvas: modules, cables, clipboard, undo, view

#### 2.1 Cables

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| C1 | Net model and connect rules | One tree per net rooted at its output (`CCableTree`, `CPatchData::ConnectCable @000e7352`). A drop is refused when both jacks are in the same net or both nets have an output (`CCableDragDropManager::GetCurrentDropAction @0001939c`). | Flat cable list. `edit::connect` (`edit.cpp:199`) refuses only exact duplicates and self-links (re-checked). | Nets with two outputs or cycles can be saved. How the synth reacts is I. | H | Parent-link tree on top of `uprate.cpp`'s `Nets`, plus the connect rule. 1 d. | V |
| C2 | Delete a module | Splices the chain back together: `Out→A→X→B` minus X gives `Out→A→B`. Without its output, the remainder is kept and turned white (`GenerateDeleteSelectionMolecules @000e7fe4`, `CNode::erase @0001b642`; manual p69, p81). | `removeModule` erases every cable touching the module (`edit.cpp:97`, re-checked). | Downstream modules get disconnected. | H | Port the splice. 0.5 d. | V |
| C3 | Jack popup | Disconnect (splices), **Break**, **Color ▸** (6 names, whole chain), **Delete** (whole chain), **Delete Unused Cables** (`CPanel::HoleDoContextMenu @000c006e`; manual p69-70). No popup on the cord. | Jack: Disconnect / Disconnect all (deletes every cable there, `AreaView.cpp:1421`). Cord menu (addition): 7 colours **including White**, one cable only; bend points; Delete one cable (`AreaView.cpp:1384`). | Break, chain Delete and Delete Unused Cables missing; per-cable colour breaks "a serial chain has one colour" (p70); White is offered as a user colour. | M | Core `breakJack`, `deleteChain`, `recolorChain`, `deleteUnusedCables`; route the cord menu through them. 1-1.5 d. | V |
| C4 | Colour rule | A new cable takes the fed side's colour and recolours the other net; unfed chains are white (`GetConnectRecolorMolecules @000e4c32`). | `cableColor(from)` (`edit.cpp:184, 212`): an input-to-input link gets the input's colour; nothing turns white. Uprate recolouring matches. | Different colours saved. | H | With C1. | V |
| C5 | Moving a plug | Double-click-hold or Option on drop moves **all** cables at the jack (`GetMoveCableNodeMolecules @000eab6c`); a drop on empty space acts as Disconnect (splice). | Alt-drag on an input moves the first cable only; a drop on empty space leaves the cable deleted (`AreaView.cpp:771`). | Different result. | M | After C1. 0.5 d. | V |
| C6 | Visible cables per colour | 7 flags in the header, saved. | View ▸ Show Cables, saved; undoable (`MainView.cpp:431`). | **Matches** (undoable is a harmless superset). | – | – | V |
| C7 | Hide All (H button, Space), dots on cabled jacks | Global, not saved (`CPatch::ToggleShowCables @000d6382`). Cabled jacks drawn with a second bitmap frame (`CPnlInHole::Draw @000ca5aa`). Manual p61, p115. | Absent; jacks look the same whether cabled or not. | Hidden cables leave no trace. | L | 4 h. | V (dot colour I) |
| C8 | Shake (S, Ctrl/Cmd+Space) | Re-seeds the twist of curved styles (`CCableRenderer::Shake @0001a8ec`). | Absent. | Missing. | L | With C9. | V |
| C9 | Cable style | Options: Straight 3D, **Curved 3D (default)**, Straight Thin, Curved Thin. The curve is a cubic with no sag: arms L/3 (or L/5+9 when L≥45) along the jack line, seeded twist ±60° (`CRenderSnakeABC::GetControlpoints @0001d13a`). Cables drawn over everything. | Classic: quadratic sag 10+0.25·d; Modern: cubic sag (`AreaView.cpp:426`). No style option. | **Classic does not reproduce the original cable shape.** | L | Port the snake curve for Classic plus the 4-style option. 0.5-1 d. | V |
| C10 | Branch highlight while holding a jack | Highlights the whole tree, also when hidden (`CPatchBackground::CableHighLight @000def5e`; manual p83). | Absent. Addition: click a cord to highlight it, Delete removes it. | The original feature is missing; the addition is fine (user-triggered). Delete should remove the chain per C3. | L | 0.5 d. | V |
| C11 | Bend points | None in the original. | Created only on user action; stored in the `.g2layout` sidecar. | An opt-in addition. OK. Flag it in the README. | – | – | V |
| C12 | Flow animation | None. | **On by default at every launch** (`Skin.cpp:196`, re-checked). | Not opt-in. | L | Default off, persist the choice. 15 min. | V |

#### 2.2 Modules: placement, add, clipboard

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| P1 | Drop and overlap resolution | `CClipboard::Drop @0014d882` → `AdjustPositionForDroppingOn @000e6616` → `GetMakeRoomForMolecules @000ea184` / `InsertSpace @000e54c2`. When the top edge lands inside a module that starts higher, the dropped group goes *below* it. The group is rigid (one delta; dragged modules are never pushed). Only intersecting modules move, with a minimal cascade in the column. The delta is clamped at the edges. | `resolveOverlaps(keep=i)` per module (`edit.cpp:133, 815, 837`): pushes the existing module down; other selected modules can be pushed (D1 rows 10-12, D2 14-16, X 12-15 → G2fresh moves D2 to 15, the original moves X to 16). Re-sorts the whole area, so pre-existing overlaps move on any edit. Clamps each module separately. | Different layout saved. | H | Port the three functions plus a D1/D2/X test. 1 d. | V |
| P2 | Add from the toolbox | Single click = focus only. Double-click or Return inserts into the **focused area**, in the rightmost selected column below the lowest selected module (else 0,0), then make-room, deselect, DSP-RAM check (`CTabButton::Click @00126c04`, `Action @0012698e`; manual p59, p116). Drag: the phantom is **centred** on the cursor (`BeginToolbarPasteOperation @0014d200`). New modules take the **current colour** (`CModuleFactory::CreateModuleFromType @000ad910`). | A single click inserts **always into VA** (`MainView.cpp:141`, `ModuleBrowser.cpp:29`) at column 0 / first free row and selects the module. Drag puts the top-left at the cursor (`AreaView.cpp:200, 1555`). Colour is always 0. | Wrong area and spot; a stray click inserts a module; no current colour. | H/M | Route to `activeArea()`, port the `Action` rule, double-click/Return, centred drop, current colour. 1 d. | V |
| P3 | Context validity | `CutForMergingWith @000e60a2` + `IsValidContext`: Fx-In (127) is FX-only; a paste that would exceed 127 modules is cleared. | `addModule`/`pasteModules` don't check contexts (`edit.cpp:47, 760`). | An invalid patch is possible. | M | Use `contexts` (already in `replace.hpp`). 1 h. | V |
| P4 | Background popup (Cut/Copy/Paste/Insert ▸ group ▸ module, Slot A-D/Local, Disconnect Performance) | `CPatchBackground::DoContextMenu`; manual p68. | No background popup (`AreaView.cpp:746`). | Missing. | M | 0.5 d. | V |
| P5 | Copy content | Clone of the modules (all variations, modes, names, colour, custom data, morphs) plus the **whole tree, pruned with splicing** (copied jacks linked through a non-copied module stay linked; a chain without its output turns white). Knobs and CCs are not copied (`CPatchData::Clone`, `AdaptForMergingWith`, `KeepTreeIntactErase`). | Only cables with both ends selected; the rest matches (`edit.cpp:733`). | Chains through the selection are lost. | M | After C1. 2 h. | V |
| P6 | Paste | Outlines follow the cursor; a click drops into the area under it; drop pipeline; **names kept**; uprate (manual p100). | Pastes immediately below the selection or at the first free row (`AreaView.cpp:1486`). **Renames** default-style names, and the prefix test also catches custom names ("OscBass" → "OscB2", `edit.cpp:781`). | Different spot and names. | M | Placement ghost plus click; keep names. 1 d. | V |
| P7 | Copy by drag | Option-drag copies; a drag into the other area always copies (`CClipboard::GetCopyOrMove @0014cb82`). | Drag moves within one area only (`AreaView.cpp:922`). | Missing. | L-M | 1 d. | V |
| P8 | Duplicate ⌘D | No such command; **⌘D = Download To Slot**. | ⌘D = Duplicate (`MainView.cpp:407, 892`). | An addition on a key the original uses. | L | Move it to another key (see §6). | V |
| P9 | Paste Params ⌘E | `CClipboard::ValidatePasteParams @0014d556` (same count and types, paired by column then row, else "Selection does not match the original data"). `PasteParams @0014d048` → `GetCopyParamsMolecules @000e9502`: values from the copy-time variation into the current variation, plus morphs (25 cap). Manual p68, p100, p116. | Absent. | Missing. | M | `edit::pasteParams` plus tests. 0.5-1 d. | V |
| P10 | Rename module | Double-click the name, or popup; 16 characters; filtered (NM1) (`OpenPanelNameDlg @000f755e`). | Popup only; no filter (NM1). | Missing double-click; see NM1. | M | With NM1. | V |
| P11 | Module colour | 25 colours (same palette and order as G2fresh); toolbar colour selector = current colour for new modules; paint bucket for the selection (manual p60, p82). | Popup only; no current colour (`ModulePainter.cpp:367`). | Missing current colour. | M | 2-3 h. | V |
| P12 | Module popup | Cut, Copy, Paste, Paste Params, Rename, Assign ▸, Global Assign ▸, **Exclude From Mutation**, Help, Delete (`CPanel::DoContextMenu @000bf466`). | Rename, Replace With, Colour, "Lock (keep when randomizing)", Copy, Cut, Duplicate, Delete (`AreaView.cpp:1193`). | Missing Paste, Paste Params, Assign, Global Assign, Help; the label differs. | L-M | 0.5 d (plus Assign, §3.4). | V |
| P13 | Replace | Replace arrow button on every module (`CReplaceView`); mapping per `re/notes/module-replace.md`. | Mapping ported (`core/src/replace.cpp`; differences documented in the note). Only in the popup; no button drawn (`ModulePainter.cpp:401-403`). Make-room inherits P1. | Discoverability. | L | Draw the button. 2-3 h. | V |
| P14 | Uprate | Recomputed after connect, disconnect, move, break, delete, add/paste and replace; never on load (`re/notes/uprate.md`). | Same, for the operations that exist. | **Matches.** | – | – | V |

#### 2.3 Undo, keys, view, look

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| U1 | Undo | **Yes**: 100 steps **per patch/slot** (`CUndoRedo::AddUndoRedoBubblePair @00018eae`); replays messages, so it works live; one step per knob release; load clears the history. Seq Rnd/Clr not undoable (manual p60). Patch settings / variation copy not undoable (I). | `UndoManager(30000,200)`, one history per document (in a performance it jumps between slots); coalesced per parameter; settings undoable (`PatchDocument.cpp:30`). | No per-slot history; otherwise a superset. | M | One UndoManager per slot. 0.5 d. | V |
| U2 | Keyboard parameter focus | ↑/↓ ±1 on the focused parameter; ←/→ previous/next parameter; Shift+arrows move to the neighbouring module; Ctrl(Mac: Option)+↑/↓ morph range; inc/dec buttons under the focused knob (`KeyIncMorphDelta @000f8292`; manual p83-84, p115). | None; only Esc/Delete (`AreaView.cpp:1006-1021`). | A core workflow is missing. | M | 2-3 d. | V (modifier decoding I) |
| U3 | Other patch-view keys | 1-8 variation (no modifier), F/V split, Space hide cables, Ctrl+Space shake, A-D slot, L learn, M CC dialog, R clock, F5-F8 / Ctrl+F8 overlays, Tab/Shift+Tab/`<`/`>`/Return in the toolbox, Esc aborts a drag (`CPatchView::InitializeShortcutKeys @000fafd8`; manual p115-116). | None (Esc only clears a highlighted cable). | Missing. | M | Key table. 1-2 d (with U2). | V |
| U4 | Split bar | Draggable, plus 3 buttons and F/V keys; position **saved in the header** (`CSplitterView`, `splitterPos`). | Plain resizer at 62/38; `header.splitterPos` round-trips but is unused in the UI. | Not applied. | L-M | Bind the bar to `splitterPos`. 0.5 d. | V |
| U5 | Zoom | **None** (`CDimensionState::Magnify*` has no callers). | 0.5-2.0, remembered; default 1.0 (`MainView.h:67`). | An addition, neutral at default. OK. | – | Flag it. | V |
| U6 | Look | Clavia bitmaps; selection = name bar filled with `kModuleFocus` (`CNameBar::Draw @000d0c0c`); Exclude-from-Mutation = orange 2 px frame. | **Modern is the default at every launch** (`Skin.cpp:195`, re-checked); Classic is only for the session; selection = white outline (`AreaView.cpp:325`); no lock marker. | The default breaks the opt-in rule. | M | Classic by default (or a first-run choice), persisted; name-bar highlight; orange lock frame. 0.5 d. | V |
| U7 | Hints | Delayed yellow value hint at the cursor; F5-F8 overlays; second click shows the module description; toolbox hover shows a **module preview**. | Status-bar value; text tooltips. | Missing. | M | Overlay layer. ≈2 d. | V |
| U8 | Toolbox | Icons per group in factory order; preview; keys. | Text buttons in the original order (`ModuleBrowser.cpp:86`). | Order matches; icons, preview and keys missing. | L | – | V |

---

### 3. Parameters, morphs, assignments, variations

#### 3.1 Display text, graphs, load

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| D1 | Parameter text (203 single / 18 dual / 6 triple) | `ParamText::*` | `core/src/param_text.cpp`; golden SHA-256 over 397,952 strings produced by the original in emulation (`tests/test_param_text_golden.cpp`). | **Matches.** Open points in param-display.md §7 (bytes +0x12/+0x18, values >127). | – | V |
| D2 | Graphs (46 PANL graphs) | module-graphs.md §1 | `core/src/graphs.cpp`, bit-exact on 9,778 tuples. | Matches, **except DXRouter (id 42)**: the `Op::Bitmap` (bitmap 993, row `algorithm*66`, `CPnlDXRouterGraph::Draw @0x976fe`) is ignored by `rasterize()` (`graphs.cpp:3268`) and `ModulePainter.cpp:153-218`, so the algorithm picture is blank. Font: JUCE default instead of Arial 9. | M / L | V |
| D3 | Load computation and synth report | `CPatchLoad` 0xed144-0xeeaae; `CMPatchLoad::ReadStream` | `core/src/patch_load.cpp`, bit-exact on 17,732 rows. | **Matches.** | – | V |
| D4 | Load display offline | `CTBWindow::SetupLoadMeter @0x129c84`: "--" and an empty bar until the synth reports. Four bars (VA/FX × cycles/memory), red above 100. | Always a figure, "Load (estimate)" (`MainView.cpp:232-247`). patch-load.md §2: some tables are stubbed, so the estimate is too low. One label; amber above 85%. | The user may believe a patch fits when it doesn't. | M | "--" by default, estimate opt-in, four bars. 0.5 d. | V |
| D5 | Over 100% | The editor doesn't refuse on percentage (the synth mutes, manual p59). It refuses on dynamic RAM, cable count, paste and 127 modules. | Same checks, with a message instead of silence (`edit.cpp:55, 210, 763`). | Same rule; better feedback; flag it. | L | V |
| D6 | Module help | Full HTML help page per module (popup Help, F1). | ~260-character tooltip (`ModuleBrowser.cpp:7-21`). cp1252 decoded as Latin-1 leaves U+0091/U+0092 in 24 strings (`tools/assets/extract_help.py:20`). The patch-settings Glide (type 135) shows the Glide module's text (`module_db_data.cpp:2852`). | Missing full help; text bugs. | L | V |

#### 3.2 Gestures

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| G1 | Knob drag mode | Options "Knob control": Circular/Horizontal/Vertical (manual p108; `CPnlKnobABC::IdleHandler @0xcbf16`). | Vertical only (`AreaView.cpp:821-834`). | Add the preference. Default value: I (check prefs). | M | V |
| G2 | Sensitivity | A full sweep is 256 px for any range (`CPnlControl::ClickDragIdleHandler @0xc8d74`). | `200/(max+1)` px per step (`AreaView.cpp:43`): ~2× faster for 0-127. | XS fix. | L | V |
| G3 | Shift fine adjust | None. | Shift = 4× finer. | Unflagged addition. | L | V |
| G4 | Mouse wheel | Scrolls the view only (`CScrollViewEx::DoMouseWheel`). | Over a knob it **edits the value** (`AreaView.cpp:967-985`). | Scrolling can silently edit the patch. Make editing opt-in. | M | V |
| G5 | Double-click | Starts a morph-range drag ("Morph w/double click"; `CPnlControl::ClickDragOnSecondClickHandler @0xc8818`; manual p108). | Resets to default (`AreaView.cpp:939-959`). | Different meaning. | M | V |
| G6 | Reset triangle (38 elements) | Click resets to `(max+1)>>1`; with the morph key it resets the morph (`CPnlKnobSliderABC::OnClick @0xcb9b0`; manual p73). | No hit area. | Missing. | M | V |
| G7 | Up/down mini-buttons; IncDec auto-repeat | 400/70 ms repeat (`UpDownOnClickHandler @0xc8970`). | Absent; IncDec steps once on mouse-up. | Missing. | M/L | V |
| G8 | Buttons and radios | Act on mouse-down; morph key + click assigns a morph (`CPnlButtonABC::OnClickHandler @0xc341a`). | Act on mouse-up; no morph by click. | – | M | V |
| G9 | **Push buttons (Seq Clr/Rnd, 12 PANL elements)** | 1 on press, 0 on release (`CPnlPushButton::Release @0xc36ee`); not undoable (manual p60). | Toggles like any ButtonText (`AreaView.cpp:1128`, re-checked). | The value stays 1 and is saved; every second click does nothing on the synth (I); each click is an undo step. | H | V |
| G10 | Morph modifier key | Option on Mac, Ctrl on Windows (`IsDragCopyKeyDown @00164898`; manual p92-93). Auto-assigns to the **focused** morph group; knobs, buttons and sliders. | Alt on every platform; knobs only; the group is the last used (initially 1) (`AreaView.cpp:797-850`). | Wrong key on Windows/Linux; wrong group. | M | V |

#### 3.3 Morphs

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| MO1 | Several groups per parameter | Keyed by (param, group) (`CMorphSpec::operator< @00064754`). Manual p91: "A parameter can also be assigned to several Morph groups". | `setMorph` overwrites the first entry's group; `clearMorph` removes every group (`edit.cpp:546-566`, re-checked). Painters, `SynthSync.cpp:56/495` and `mutate::apply` assume one group. 334 corpus files and 98 factory patches have multi-group parameters. | Editing them corrupts them; range edits hit the wrong group; duplicates are possible, which the original rejects ("Parameter double assigned to morph"). | H | Key everything by (loc, module, param, group), with tests. 1-2 d. | V |
| MO2 | 25-morph limit per variation | `CControlMenu::InitMenus @0008fcbe` (`GetNumberOfMorphedParams < 0x19`), `CtrlAutoAssignMorph @000c1b9c`; also enforced on paste and Paste Params (manual p60, p91). | No limit. | Patches the original can't make; synth behaviour I. | H | 2 h. | V |
| MO3 | Morphable flag | `CBuildModule::IsParamMorphable @000a9f4c`. | `ParamDef::morphable` is never read (re-checked). | 110 non-morphable parameters can be morphed. | H | 1 h. | V |
| MO4 | Range clamp | value+range clamped to [0, max]; max/256 per px (`CircularIdleHandler @000cbcf8`, `GetMorphAbsValue @000aa276`). | ±127 at 1 px per unit; arc drawn as range/127 (`AreaView.cpp:838-856`, `ModulePainter.cpp:62`). | Wrong saved range and display when max ≠ 127. | H | 2 h. | V |
| MO5 | Popup Morph assign | Toggles the focused group, sets value 0 and range +max; Deassign removes that group only (`CControlMenu::HandleMenuSelection @0008f5c6`). | Submenu None/1-8, range 32, value kept (`AreaView.cpp:1343-1347`). | Different patch. | H | 1 h. | V |
| MO6 | Focused morph group | Click a toolbar morph dial to focus it (`CMorphContainer::Click @00123a84`). Knob red if morphed in the focused group, blue if in another; only the focused group's sector is drawn (manual p92). | No focused group; 8 colours; the first group's arc is drawn (`ModulePainter.cpp:434`). | Missing; the 8-colour view is an unflagged deviation. | M | 1 d. | V |
| MO7 | Morph dial popup | Clear Group, Copy To ▸ Group, Default, Edit name, Assign knob, Global Assign, MIDI (`CMorphControlMenu @00124a04`; manual p60, p92-93). | None (rename by double-click, `PatchSettingsBar.cpp:327-349`). | Missing. | M | 1 d. | V |
| MO8 | SeqNote/SeqLev morph band | Bitmaps 0xD01-0xD03 (`CSeqSliderGUI::Draw @0007fd86`). | Own translucent band, also in Classic (`SpecialControls.cpp:169, 378, 416`). | Unflagged in Classic. | L | Port the bitmaps for Classic. 0.5-1 d. | V |
| MO9 | Morph names; morphs per variation; copied with the variation | `CPnlMorphName::GetCustomData @0012406e`; manual p93. | `edit.cpp:436-479, 727`. | **Matches.** | – | – | V |

#### 3.4 Knobs, parameter pages, MIDI CC

| # | Item | Original | G2fresh | Gap | Sev | Rec | V/I |
|---|---|---|---|---|---|---|---|
| K1 | **CC validity** | 0-119 minus {0,1,11,18,32,64,70,96,97} (`MIDICtrl::IsValid @00148754`); {7,17} pre-assigned (`IsPreAssigned @0014872e`); manual p90, p126-127. The MIDI item is removed for pre-assigned parameters. | 0-127 (`edit.cpp:608-617`, re-checked; menu `AreaView.cpp:1314-1326`). | Bank select, mod wheel, sustain, CC70 and others are assignable; CC7/CC17 silently replace patch volume / octave shift. | H | Shared tables in core and UI. 2 h. | V |
| K2 | Knob naming | "Page A1…E3", "Knob n" (`CKnobAssignMenu`; manual p19-20). | "1B-3" (`edit.cpp:594`), also in plugin automation names (`AutomationBank.cpp:26`). | Letter and number swapped. | M | Rename before 1.0 (host parameter names change). 1 h. | V |
| K3 | LED knob targets | `CBarbTargetSpec` type 1 = LED (`GetAssignKnobToLedMolecules @000f1848`). | `assignType` kept but ignored (`edit.cpp:326, 569`); pch2-format.md §3.7 wrongly says "1 = button". | 100 corpus entries show wrongly; automation drives the wrong parameter; reassigning deletes them. | M | 0.5 d. | V |
| K4 | Parameter Pages ⌘F; Parameter Overview ⌘L (Assign MIDI / Clear / View MIDI / View Buttons / Global Pages; drag-assign) | `CKnobFloater*`, `CKnobOverview`, `InternalAutoAssignMIDICtrlFromKnobMap @000d6990`; manual p88-90, p108-110. | Absent; ⌘L = Live. | Missing. | M | 3-5 d. | V |
| K5 | Module Assign ▸ page/column, "Multi Column" | `CKnobMap::GetAssignModuleToPageMolecules @000f24d4`; manual p68, p90. | Absent. | Missing. | M | 0.5 d. | V |
| K6 | Global knobs (performance, 120) | `CPerfKnobMap`; `CreateGlobalKnobAssignMenu`. | Loaded and saved only; no UI; nothing sent. | Missing. | M | 1 d. | V |
| K7 | MIDI Control dialog (M), MIDI Learn (L), Assign/Deassign MIDI to Selection, Deassign All | `CDialogMIDIControl::InitDialog @00031476`, `KeyLearnMidi @000f99fe`, `InternalAutoAssignMIDICtrl @000d6cc6` (lowest free valid CC), `…Deassign… @000d67de/@000d64dc`; manual p113, p128-129. | A flat CC list marked "(in use)". | Missing. | M | 1.5 d. | V |
| K8 | MIDI-assignable flag (5 parameters are false) | Param spec +0x10 (`CtrlIsMidiAssignable @000bfffa`). | Dropped by `tools/moduledb/gen_cpp.py`. | – | L-M | 1 h. | V |
| K9 | Parameter Edit name | Only on `CPnlLabelButton`, `CPnlLabelRadioButton` and `CPnlMorphKnob` (`CanChangeName`). Radio record `[1, n·7+1, param, n×7 chars]` (`CPnlLabelRadioButton::GetCustomData @0xc597e`). | "Rename Parameter…" on every parameter (`AreaView.cpp:1334`); always writes `{1,8,param,7 chars}` (`edit.cpp:642-665`), malformed for radios. Custom labels are never drawn (`ModulePainter.cpp:474-508, 801-835`), so names in loaded patches are invisible. | Different file; names missing from display. | H/M | 1 d. | V |

#### 3.5 Variations and patch settings

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| V1 | 8 user variations + Init (index 8) | – | `kUserVariations`=8, `kFileVariations`=9 (`patch.hpp:29-30`). | **Matches.** | – | V |
| V2 | **Var Init button** (recall Init into the focused variation) | `CMParamSettingCopy(8, focused)` (`CPatchToolbar::HandleChangeRequests @000f622a`; manual p61, p88). | Only copying *to* Init exists (`MainView.cpp:411-415`). | Init can't be recalled. 1 h. | M | V |
| V3 | Copy variation | Right-click a variation button ▸ Copy to 1-8/Init (`CPatchToolbar::DoContextMenu @000f5b9c`). Data: VA, FX and settings parameters plus morphs (`HandleParamSettingCopy @000d3eca`). | Edit menu; same data (`edit.cpp:714-727`). | Data matches; menu location differs. | L | V |
| V4 | Variation keys | Plain 1-8. | ⌘1..8 (`MainView.cpp:910`). | See §6. | L-M | V |
| V5 | Voice mode | Legato / Mono / 2..32 (`SetupState @000f38b4`). | Offers poly 1 (`PatchSettingsBar.cpp:232-254`; `edit.cpp:400`). | 15 min. | L-M | V |
| V6 | Voices display | "actual (requested)" live, red at 0 (manual p60-61). | Requested only; the Voices molecule is parsed but unused. | 2 h. | M (live) | V |
| V7 | Settings per variation | Manual p101 | `settingValue(…, variation)` | **Matches.** | – | V |
| V8 | **Patch Adjuster ⌘3** | `CDialogGlobalModifiers @0013935c`: 8 knobs ±50 (Attack, Decay, Sustain, Release, Mod. Rate, Timbre, Resonance, Effects) → `CPatch::ApplyDistribution` (`CGlobalModifiersBackground::HandleChangeRequests @00138e82`). Not in the manual. | Absent; not in the notes. | Missing. Reverse `ApplyDistribution` first. 1-2 d. | M | V (feature), I (maths) |

#### 3.6 Randomizer and Patch Mutator

The core (`core/src/mutate.cpp`) is an exact port, golden-tested in emulation: BSD `random()`, `IsEnabled @00141796`, `ApplyCurve @001d199c`, `CopyRandomizeContext @001425e8`, `CopyMutateContext @00142984`, `RecombineContext @00142288`, `CopyMorphs @00141456`. The UI is not faithful.

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| R1 | **Audition** | Plays the clicked individual in hidden variation 9 and focuses it; variations 1-8 untouched (`CDialogMutaBackground::Click @001406cc` → `GetFocusIndividMolecules @00145506`). Auto-plays child 0 after each generate. | Writes into the **current variation**, one undo step per click (`MutatorWindow.cpp:259-267`). | Overwrites the user's variation. | H | V |
| R2 | Range | `CSmallKnob(…,0x32)`: 0-50%. | Slider 0-100%, no clamp (`MutatorWindow.cpp:122`, `mutate.cpp:327`). | Results the original can't produce. 15 min. | M | V |
| R3 | Module added while mutating | `AddModule @00141c08` adds it to the 8 population slots, 24 bank slots and the mirror. | Mirror only (`mutate.cpp:589-625`). | 2 h + test. | M | V |
| R4 | Lifetime | One per patch (`CPatch+0x158`). | One per window; survives loading another patch (`MainView.cpp:647-652`). | Individuals from patch A can be written into patch B. Reset on load. 2 h. | M | V |
| R5 | Gestures, boxes, solo, keys, lock frame, undo | Drag copy / Alt cross / Shift interpolate; drop on a variation box; double-click mutates; 7 Solo buttons; Mutator keys; orange lock frame; every command undoable. | Context-menu stand-ins; none of the rest. | Missing. ~3 d. | M | V |
| R6 | RNG seed | Never seeded (= `srandom(1)` each session). | Clock-seeded (`MutatorWindow.cpp:52`). | Unflagged. | L | V |
| R7 | Stand-alone Randomize button, Randomize/Mutate Variation/Selected | None in the original. | Toolbar and Edit menu (`MainView.cpp:166-172, 417-418`); uses default settings, not the dialog's. | Unflagged addition. | L | V |
| R8 | Bugs | – | The focus ring never shows (`mutate.cpp:737`, `MutatorWindow.cpp:263`); Link doesn't refresh the range slider (`:143`). | <1 h. | L | V |
| R9 | Labels | OscFreq, OscFine, Envelope, SeqValue, SeqEvent, Delays, Effects; "Temporary Storage"; "Quick Locks…"; Tools menu ⌘2. | Different labels; Edit menu, no shortcut. | – | L | V |

#### 3.7 Special controls

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| S1 | Vocoder presets | `CPnlVocoderPreset @0x9354a` | `special.cpp:104` | **Matches.** | – | V |
| S2 | Drum preset stepping | `PresetComparison @0x90f7c` on every dependency change. | Only at click time; `drumSelectors_` never cleared (`AreaView.cpp:1074-1081`). | Wrong preset on "up" without a match; stale across loads. | L-M | V |
| S3 | Drum preset name click | Plain text. | A menu of 30 presets (`AreaView.cpp:1083-1105`). | Unflagged addition. | L | V |
| S4 | NoteSeq zoom/offset | @0x927ce / @0x92a5c | `special.cpp:139-245`, emulator-exact. | **Matches** offline. Live, a click resends the whole patch (see §4). | M (live) | V |

---

### 4. Synth menu, USB, live editing, performance, banks

#### 4.1 Live editing, editor → synth (`ui/SynthSync.cpp::push`)

| Edit | Original sends | G2fresh sends | Sev | V/I |
|---|---|---|---|---|
| Parameter value | `40` realtime (`CPanel::CtrlRequestParamChange @0xc163a`) | `40` | Matches | V |
| Morph range | `43`, realtime while dragging (`@0xc19b2`) | `43` after the fact | L | V |
| Mode selector | `2B` | `2B` | Matches | V |
| Knob assign/deassign | `25`/`26` + `2D` page focus (`GetAssignKnobToTargetMolecules @000f1580`) | `25`/`26` without `2D` | L | V |
| CC assign/deassign | `22`/`23` | `22`/`23` | Matches | V |
| Variation select | `6A` | `6A` | Matches | V |
| Variation copy/init | one `44 from to` | N × `40`/`43` | M | V |
| Module add/delete/move/recolour/rename/replace; cable add/delete/recolour; uprate | `30/32/34/31/33/50/51/54` + `2A`, one bubble per action; undo sends the inverse bubble | **Whole-patch re-upload** (`SynthSync.cpp:305-312, 410-411`, re-checked), followed by release `38`, read-back, document replaced (`:428-445`). Realtime `40`s sent before the `38` carry the old session (I). | **H** | V |
| Patch header (voices, mode, cable visibility, category) | `21` (≈600 ms delay for voices; `CPatch::RequestPatchHeaderChange @0xd62ae`) | Whole patch | H | V |
| Parameter labels (custom data), seq zoom | `42` | Whole patch | M | V |
| Textpad | `6F` | Whole patch | M | V |
| **Patch name** | `27` (`CPatch::SendFlashNameDump @0xd83e2`) | **Never sent**: the name isn't in `g2::Patch`, so `equalFiles` sees no change (re-checked). A store keeps the old name. | H | V |
| Performance header / name | `11` per field change (`CDialogPerformance::TrySendData @0x39b36` → `SetPerformanceHeader @0x100800`); `29`; toolbar clock `3F FF 00/01` | **Never sent** (`Dialogs.cpp:100-147` → `editPerformance`; `changeListenerCallback` compares only the slot patches, `:292`) | H | V |
| Mutator lock | `90` | Probably nothing | L | I |

**Recommendation:** generate the original's bubbles from the edit layer. The codec and `Client::edit()` already exist (`proto/include/g2/proto/molecules.hpp:353-359`, `client.cpp:663`). 3-6 d. Until then, flag the whole-patch resend in the README.

#### 4.2 Synth → editor

| Message | Original | G2fresh | Sev | V/I |
|---|---|---|---|---|
| `40` with var 0x7F (all variations) | `CPatch::HandleParamChange @0xdc636` → `SetCommonParamValue` | Dropped (`proto/src/state.cpp:215-219`; exception swallowed `SynthSync.cpp:421-425`) | M-H | V |
| `43`, `6A` | – | Handled | Matches | V |
| Panel patch load / `38` / `1F` | Slot re-read | Re-read; bend points kept | Matches | V |
| `74`/`75` knob map changed on the synth | Clear the map, re-request `63`/`5E` (@0xd3918, @0xffeda) | Ignored (no handler in `client.cpp`); the next whole-patch resend erases panel assignments (I) | H | V |
| `80` MIDI learn | Feeds the L key and the "Assign to CC# n" item | Mirrored only | M | V |
| `09` slot focus, `2F` param focus | Switch slot and raise the window (@0x100582); focus highlight (@0xdd0fe) | Not acted on | L-M | V |
| `11/07/0F/29/1C/1D/03` | Update the UI | Mirror only, not the document | M | V |

#### 4.3 Connection, slots, performance mode

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| L1 | Handshake/sync order | usb-protocol.md §6.3 (`CSynth::CleanDirtyData @0x11f394`) | `client.cpp:225-291` | **Matches.** | – | V |
| L2 | Download on connect | The topmost unconnected document is uploaded (`CSynth::CSynth @0x11e964`, `CreateDownloadPerformanceFromPatch @0xfe478`). | USB: read only until the user sends. Emulated G2: auto-sends (`PluginProcessor.cpp:432-438`). | Unflagged deviation (the safer one). | M | V |
| L3 | Timeouts | 10 s, first timeout fatal (`CTimeoutManager::SuggestAction @0x103f56`) | 10 s, 0 retries | **Matches.** | – | V |
| L4 | Errors | Stop alert with the status and `7E` text (`CPortManager::Idle @0x102678`). | Status bar only; `Listener::error` not implemented. | Cosmetic. | L-M | V |
| L5 | Several G2s | Up to 4, one toolbar row each. | First device only (`usb/src/libusb_transport.cpp:103-116`). | Document it. | L-M | V |
| L6 | Slot model and buttons | Click = focus `09`; Shift-click = active `07`; Ctrl-click = keyboard (manual p58); keys A-D. | Local slot switch for performance documents only; `selectSlot` (`09`) has no caller (`MainView.cpp:121`). | Missing. | M | V |
| L7 | Download to Slot ⌘D | `CDialogSlot`; binds the document, enables and focuses the slot (`CPatchManager::DownLoadPatch @0xef62a`). | "Send Patch to Slot" → same upload bubble (`SynthSync.cpp:204-215`); no enable/focus. | Mostly matches. | L-M | V |
| L8 | Upload Active Slot ⌘U | Opens a window on the mirrored focused slot. | "Get Patch from Slot X" replaces the document after a prompt. | UX only. | L | V |
| L9 | Perf button `3E`; perf-mode toggle `03` before a performance upload | `CTBWindow::SetPerformanceMode @0x12a306`; `CleanDirtyData @0x11f394`. | Neither. | A performance sent to a G2 in patch mode may not play as a performance (I). | M-H | V |
| L10 | Toolbar master clock and voices | BPM (red when on external clock), Run, R key; voices "%d (%d)" (`CPatchToolbar::SetupState @0xf38b4`). | In the mirror but not shown. BPM range 24-240 (`Dialogs.cpp:132`); the original clamps 30-240 (`SetClockRate @0003964c`). | Missing; wrong range. | L-M | V |
| L11 | Performance Settings ⌘R | Name, BPM, Run, Keyboard Range enable; per slot Enable, Keyboard, Hold, Lower, Upper. No MIDI channel (that is in Synth Settings). Rules: "At least one slot must be active", the keyboard moves off a disabled slot, lower ≤ upper (`ToggleSlot @000397da`, `ToggleKeyb @0003986e`, `ValidateKeyboardRange @000398be`). | Adds per-slot MIDI channel and patch name; no performance name; none of the rules (`Dialogs.cpp:78-207`); no ⌘R. | Inconsistent performances can be saved. | M | V |
| L12 | Performance name | Toolbar Perf box and dialog; from the file name; `29` (manual p58, p102). | None; Get → Send renames it to "Performance" (`SynthSync.cpp:235`). | Lost name. | H | V |

#### 4.4 Memory banks and other Synth menu items

| # | Item | Original | G2fresh | Gap | Sev | V/I |
|---|---|---|---|---|---|---|
| B1 | Load from memory | `0A` into the focused slot. | Same message; slot chosen in a combo. | **Matches.** | – | V |
| B2 | Store / Save In Synth | `0B`, no overwrite confirmation (manual p85; `CDialogSaveInSynth::Idle @0x3dc80`). | `0B` with a confirmation (`BankBrowser.cpp:218-226`). | Safer deviation; flag it. | L | V |
| B3 | Store result | `0D` codes 1/3/4 → alerts "Can't load from flash / Flash memory full / Memory protect on" (`HandleFlashCommandResult @0x11b062`). | Ignored (`client.cpp:754-758`). | The user believes it was stored. | M-H | V |
| B4 | List refresh | The synth pushes `16`/`15`/`12`. | Re-reads the whole `14` list after a store. | Traffic the original never sends. | L-M | V |
| B5 | Delete entry/bank | `0C`; delete bank asks "Are you sure…" then `0E`. | Missing. | – | M | V |
| B6 | Bank Upload/Download to disk (`.pchList`) | `CFlashDumper @0x1190de…`; manual p106-107. | Missing (codec has `17/18/19`). | – | M | V |
| B7 | Browser: sort by program/name/category; Disk tab | `CFlashListView`; manual p111-113. | Patches/Performances only, no sort, no Disk tab. | Partial. | L-M | V |
| B8 | Synth Settings ⌘G | Name, per-slot MIDI channel and active, global channel, SysEx ID, clock send/ignore, controllers and program-change send/receive, tune, global octave shift, pedal gain, memory protect, pedal polarity, Local On (`CDialogSynthSettings::GetValues @0x40be8`; manual p103-105). | **Missing.** `BridgeLink::setSynthSettings` is a no-op (`link.hpp:61-62`). The plugin writes `03` (Local On, Ignore External Clock off) to the *emulated* G2 only. | – | M | V |
| B9 | Send Controller Snapshot ⌘M (`55`); Dump One ⌘1 (`3D`) | `CPatch::SendControllerSnapCommandBubble @0xd8352` | Missing. | – | L | V |
| B10 | Virtual Keyboard ⌘K | Monophonic `56`, Drone and Repeat (`CKeyboardFloater @0x12ebc2`). | `Client::playNote` has no caller; the Live keyboard drives the emulator only. | Missing for a real G2. | L-M | V |
| B11 | MIDI as a transport | `CSynthPortMIDI` is unreachable. | USB only. | **Matches.** | – | V |
| B12 | USB log | No equivalent (`CDialogLog` is the Bank Upload/Download log). | Redacted log, Full log opt-in. | An addition, opt-in and flagged. OK. | – | V |
| B13 | LEDs and meters | Accepted after both `72` replies; single LEDs → `39`, groups → `3A` (`CPanel::IsMutliLedGroup @0xbe998`). All 4 slots' windows. | Same rule (`LiveLeds.cpp:9-55`), but it assumes flag +0x60 = MiniVU. Only the shown slot. | If +0x60 is not the MiniVU flag, LEDs shift onto the wrong modules (I: `CPnlMiniVU::Draw @0xcf55c` takes 0..3). Find what sets +0x60. | M (I) | I |
| B14 | Load freshness while live | `UpdateCriticalResource` → re-request `71` on any change. | Requested in `enqueueSlotSync` only. | Possibly stale. | L-M | I |

#### 4.5 Plugin-only features

| Item | Finding | Sev |
|---|---|---|
| AutomationBank | Mapping: 120 knobs, each over its target's min..max; 8 morph dials; variation → `6A`; realtime `40` through `push()`. All correct. Problems: names use "1A-1" instead of the original's "A1-1" (K2: rename before 1.0, host names change); global knobs not exposed; LED knob targets are mis-driven (K3). | L-M |
| MidiForwarder | Opt-in, flagged, no SysEx. No loop protection through the G2's MIDI OUT/Thru: document it. | L |
| Emulated G2 auto-send | Mirrors the original's download-on-connect. Flagged. OK. | – |

---

### 5. Deviations that break the standing rule (on by default, or unflagged)

To be made opt-in, or listed in a new README "Deliberate deviations" section. README has none today.

**Changes what the user gets:**
- Whole-patch resend for structural edits (§4.1).
- Single click in the toolbox inserts a module into VA (P2).
- Immediate paste with renaming (P6).
- Mouse wheel edits values (G4).
- Double-click resets instead of starting a morph (G5).
- Mutator writes into the current variation (R1).
- Offline load estimate shown as a figure (D4).
- Session restore marks edits as saved (M9).

**Changes what the user sees:**
- Modern look (U6).
- Cable animation (C12).
- 8-colour morph display (MO6).
- Seq morph band in Classic (MO8).
- Amber load threshold (D4).
- Inline settings bar (M13).
- Drum preset menu (S3).
- White cable colour (C3).

**Shortcuts:**
- ⌘D Duplicate (original: Download To Slot).
- ⌘L Live (original: Parameter Overview).
- ⌘1-8 variations (original: plain 1-8; ⌘1 Dump One, ⌘2 Mutator, ⌘3 Patch Adjuster).
- ⇧⌘S Save As (none in the original).
- Unbound: ⌘P, ⌘J, ⌘R, ⌘B, ⌘W, ⌘E, ⌘T, ⌘F, ⌘K, ⌘G, ⌘U, ⌘M, ⌘I.

**Prompts and safety additions (keep, but flag):**
- Unsaved-changes prompt (M8).
- Store overwrite confirmation (B2).
- "Open anyway" on a bad checksum (F6).
- Module-limit messages (D5).
- No download on connect (L2).

**Unflagged additions:**
- Shift fine adjust (G3).
- Alt as the morph key on all platforms (G10).
- Clock-seeded RNG (R6).
- Stand-alone Randomize (R7).
- Recent Files 12 entries plus Clear (M10).

**Already fine (opt-in and/or flagged):**
- Zoom.
- Cord highlight and Delete.
- Bend points in the sidecar.
- USB log redaction.
- MIDI output forwarding.
- Bad-checksum override (user-chosen).

### 6. Already matching (no action)

- Patch file CRC, header and section layout.
- Byte-exact round-trip of unedited files.
- New patch header defaults.
- Save naming logic for patches.
- Parameter display text (golden).
- Module graphs except DXRouter (golden).
- Patch load computation and synth report decoding (golden).
- Mutator and randomizer core (golden).
- Vocoder presets and NoteSeq zoom/offset.
- Uprate.
- Replace mapping.
- Variation copy data.
- Morph names.
- Settings per variation.
- Visible-cable flags.
- Clipboard scope.
- Select All.
- USB handshake order, timeouts, load/store messages, LED acceptance rule.
- Parameter, mode, CC and variation live messages.
- No MIDI transport and no .syx/.mid import (correctly absent).

### 7. Suggested order of work

1. **Data correctness (≈2 weeks):**
   - file conversion (F1-F3);
   - cable tree plus splice and colour rules (C1-C5);
   - morph model, limits and flags (MO1-MO5);
   - CC tables (K1);
   - push buttons (G9);
   - name filter (NM1);
   - parameter rename gating and radio record (K9);
   - drop pipeline (P1-P3);
   - Mutator audition and range (R1-R2);
   - performance defaults and rules (N3, L11).
2. **Live editing parity (≈1 week):**
   - per-edit bubbles `30…54`, `21`, `27`, `29`, `11`, `42`, `6F`, `44`;
   - `74`/`75` refetch;
   - variation 0x7F;
   - `0D` alerts;
   - slot focus `09`;
   - Perf `3E` / `03` toggle.
3. **Opt-in and flag pass (≈2 d):** everything in §5, plus a README "Deliberate deviations" section.
4. **Missing original features, by value:**
   - Open To / New To / Save From;
   - Paste Params;
   - Delete Unused Modules/Cables;
   - Var Init;
   - keyboard parameter focus and 1-8;
   - MIDI Learn / dialog / auto-assign;
   - Parameter Pages and Overview;
   - Synth Settings;
   - bank delete and Upload/Download;
   - Init 1&2;
   - Patch Adjuster;
   - Options (cable style, knob mode, morph by double-click);
   - hints and F5-F8;
   - per-slot undo;
   - Help and About;
   - Virtual Keyboard over USB;
   - Controller Snapshot and Dump One.

### 8. Open uncertainties (check before acting)

**How the synth reacts (check on hardware or in the emulator):**
- a net with two outputs or a cycle;
- more than 25 morphs;
- a latched Clr/Rnd value;
- realtime `40`s dropped across a whole-patch resend;
- a performance sent while the synth is in patch mode.

**Reverse engineering still to do:**
- what the LED flag +0x60 means (B13);
- what `CPatch+0x68` means (the "0x7f variation" mode, possibly "edit all variations");
- `ApplyDistribution` maths (V8);
- the Mac modifier mapping: manual "Ctrl+arrow" = Option? "Cmd+Space" = Control+Space?;
- the default knob-control mode;
- the mutator key letters;
- whether the current colour persists between launches.

**Not traced:** the hint delay, and the drawing of the up/down buttons under a focused knob.
