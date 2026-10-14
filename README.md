# G2fresh

A modern, native editor for the Clavia **Nord Modular G2**, for macOS (Apple Silicon and Intel), Windows and Linux. It runs as a stand-alone app and as a VST3/AU plugin.

Clavia's last editor (v1.62) is a 32-bit Carbon app on the Mac and depends on a proprietary kernel driver on Windows, so it no longer runs on current systems. G2fresh is a rewrite. Ghidra analysis of the original editor serves as the specification, cross-checked against Bruno Verhue's open-source editor.

**Status:** 0.10.2, a patch editor that also plays the patch by itself (the **Emulated G2**, experimental, with a playable front panel and keyboard in the **Live** view), with an experimental USB connection. It opens, edits and saves G2 patches and performances; every public G2 file we know of loads (5,589 files from Clavia's banks and the community archives). It has the full patch settings, morph, knob and MIDI controller assignments, copy/paste and duplicate, module Replace, the randomizer and Patch Mutator, the original's special controls, the patch load meter, the modules' live graphs and descriptions, a modern look and the original Classic look (built from your own copy of Clavia's download), zoom, animated cables with bend points, and native menus. As a VST3/AU plugin it stores the patch in your DAW project, exposes the patch's knobs, morph dials and variation for automation, and forwards its MIDI to a hardware port into the G2. The Synth menu connects to a G2 over USB (through `g2bridge`) or to a virtual G2: send and get patches and performances, live editing both ways, the synth's memory banks, live LEDs and meters, the load the synth reports. **The USB connection has only been tested against the virtual G2**: see "Testing with a real G2" below. Without a G2, the Emulated G2 runs the G2's own operating system (from Clavia's free OS update, which you download yourself) inside G2fresh, so any patch plays in the app or in your DAW: see "Playing without a G2" below.

## Layout

| Path | What |
|------|------|
| `re/ghidra_scripts/` | Headless Ghidra scripts (`ExportAll.java`: decompiled C, symbols, call graph, strings) |
| `re/notes/` | Recovered specifications (file format, USB protocol, resources, parameter display) |
| `tools/rsrc/` | Extractors for the original editor's resources (`rsrc.py`, `panl.py`) |
| `core/` | `libg2core`: `.pch2`/`.prf2` codec (`file.hpp`), patch model (`patch.hpp`), edit operations (`edit.hpp`), module database (`module_db.hpp`), parameter display text (`param_text.hpp`) |
| `proto/` | `libg2proto`: the USB protocol (framing, molecules, the client's handshake / sync / edits) and a virtual G2 for tests (`emulator.hpp`); spec in `re/notes/usb-protocol.md` |
| `usb/` | `libg2usb`: the USB connection through libusb (`LibusbTransport`); libusb is built from source |
| `bridge/` | `g2bridge`, the process that owns the G2's USB connection and serves every editor over a local socket, and `BridgeLink`, an editor's link to it |
| `assets/clavia/` | The original's module panel layouts (positions, sizes, captions; no pixels, see NOTICE) |
| `data/` | Module and parameter database (JSON), compiled into the core by `tools/moduledb/gen_cpp.py` |
| `plugin/` | JUCE target: stand-alone app, VST3, AU |
| `emu/` | Emulation, built with `-DG2_BUILD_EMU=ON`: `g2emu`, the whole G2 in C++ running the user's own firmware (`re/notes/g2-hardware-and-emulation.md` §3.9), and the DSP experiments |
| `tests/` | Catch2 tests; `tests/corpus/` holds freely licensed patch files. Set `G2_EXTRA_CORPUS=<dir>` to also test your own patches |
| `tools/package/` | What a release is made of: the macOS installer and its signing and notarization (`macos.sh`), the Windows installer (`windows.ps1`, Inno Setup), the Linux archive and its install script (`linux.sh`); `docs/RELEASING.md` says how a release is cut |

## Download

Each [release](https://github.com/r0main-f/G2fresh/releases) has an installer and a plain archive for macOS (universal), Windows (x64) and Linux (x64). A release's notes say whether its macOS and Windows files are signed.

- **macOS** (11 or later; releases up to 0.10.0 needed macOS 26): open `G2fresh-<version>-macOS-universal.pkg`. It installs the app in Applications, the VST3 plugin in `/Library/Audio/Plug-Ins/VST3` and the Audio Unit in `/Library/Audio/Plug-Ins/Components`; untick what you do not want on the Installation Type page. The `.zip` holds the same three bundles, to put there by hand (or the plugins in `~/Library/Audio/Plug-Ins/VST3/` and `~/Library/Audio/Plug-Ins/Components/`, for you alone).
  - A release signed with a Developer ID and notarized by Apple opens like any other software.
  - A release that is not (every one up to 0.10.2, and any build made without the certificates) is refused by macOS at first. Either run `xattr -dr com.apple.quarantine` on the downloaded `.pkg` (or on the three bundles from the zip) in Terminal, or try to open it once and then choose Open Anyway in System Settings > Privacy & Security (on macOS 14 and earlier: right-click the file and choose Open).
  - To remove G2fresh, put `/Applications/G2fresh.app`, `/Library/Audio/Plug-Ins/VST3/G2fresh.vst3` and `/Library/Audio/Plug-Ins/Components/G2fresh.component` in the Trash. Your patches and settings (`~/Library/Application Support/G2fresh`) stay.
- **Windows:** run `G2fresh-<version>-Windows-x64-setup.exe`. It installs the app in `Program Files\G2fresh`, with a Start menu entry, and the VST3 plugin in `C:\Program Files\Common Files\VST3` (each optional), and removes them again from Settings > Apps. The `.zip` holds the same files: put `G2fresh.exe` and `g2bridge.exe` together where you like, and `G2fresh.vst3` in `C:\Program Files\Common Files\VST3\`. If the release is not signed, SmartScreen warns: choose "More info", then "Run anyway". A real G2 needs the WinUSB driver: see "Connecting a G2".
- **Linux:** unpack `G2fresh-<version>-Linux-x64.tar.gz` and run `./install.sh` in it: the app and `g2bridge` go to `~/.local/bin` with an entry in the applications menu, the VST3 plugin to `~/.vst3`, all without root (`./uninstall.sh` removes them). By hand: keep `G2fresh` and `g2bridge` together, and copy `G2fresh.vst3` to `~/.vst3/`. A real G2 needs a udev rule, which the script prints and "Connecting a G2" has.

How the releases are built, signed and notarized: `docs/RELEASING.md`.

## Playing without a G2

G2fresh can make the G2's sound itself, in the stand-alone app and in the plugin, with the **Emulated G2 (experimental)**: a whole G2 in software (its processor, its four DSPs, the USB link) running Clavia's own G2 OS 1.62, which therefore plays every module and patch. G2fresh contains no Clavia code: download the free **Nord Modular G2 OS v1.62 update for the Mac** from Nord's website ([nordkeyboards.com](https://www.nordkeyboards.com), in the Nord Modular G2 downloads) and keep the `.dmg`; G2fresh reads the Mac download on every system. Then choose **Synth > Connect to Emulated G2**: the first time, G2fresh asks where the `.dmg` (or the updater app in it) is. The synth boots in about 2 seconds; your patch is then sent to it and plays from MIDI (the plugin's track, or a MIDI input in the app's Options), and every edit, variation, knob and memory bank works as with a G2. In a DAW, each G2fresh instance keeps its emulated G2's memory (stored patches and settings) in the project and starts it again when the project opens; the stand-alone app keeps its memory in `Emulated G2 flash.bin` next to G2fresh's settings.

**Live** (in the toolbar, **View > Live** or Cmd L) turns the editor into the instrument: the Emulated G2's front panel, laid out as on a G2X and driven by the G2 OS itself (its five displays and menus, LEDs, knob rings, buttons, knobs and dial, master level), the pitch stick, the mod wheel and the two global wheels, and a 61-key keyboard played with the mouse (velocity from where a key is hit) or the computer keyboard (Z/X: octave). The keys are the G2's own, so focus, octave shift, KB Hold and split work as on the machine.

A new emulated G2 starts as a G2 leaves the factory, without its sounds. Its system settings are the G2 OS's own: MIDI Local on, slots A-D on MIDI channels 1-4 (so a track on channel 1 plays slot A, channels 2-4 play slots B-D), MIDI Ctrl and Program Change on Send & Receive, synth name "ModularG2". G2fresh then sets, once, the two the manual gives as factory values: the Global MIDI channel Off and Memory Protect On. To store patches in its memory, first switch Memory Protect off in **Synth > Synth Settings...** (Cmd/Ctrl-G), or in the G2's own System menu (Live view: System, then Down to "Memory Prot", then the dial); until then the G2 refuses, and G2fresh says so as the original does ("Flash Problem: Memory protect on"). Synth Settings holds the G2's other system settings too (name, MIDI channels, clock, controllers, tuning, pedals), for the emulated G2 and a G2 on USB alike; each change takes effect at once, and Cancel puts back what was there. Every setting you change stays, as on a G2. Clavia's factory sounds are not included: the memory starts empty.

- It needs a fast computer: one emulated G2 uses about 1.3 (light patches) to 1.6 (heavy ones) CPU cores of an Apple M1, and runs at 2-2.8 times real time there. Only the first G2fresh instance in a project starts one by itself; others start from the Synth menu or the Live view (each runs its own emulated G2).
- In a DAW, the emulated G2's master clock (arpeggiator, clocked LFOs and delays) follows the host's tempo and transport, as MIDI clock into its MIDI IN (Synth > Emulated G2 Follows the Host's Tempo, off by default: the G2 then keeps its own tempo, as a G2 without MIDI clock does; when on, it also switches the G2's "MIDI Clk Recv" on). Its Out 3/4 come out on a second stereo output, "Out 3/4": route it to a track of its own to hear them. Otherwise only Out 1/2 are heard, as on a G2 whose headphones carry Out 1/2 only; Synth > Mix Out 3/4 into Out 1/2 mixes them in instead (a patch that sends the same signal to both pairs then plays it twice as loud).
- **Audio inputs.** The G2's four inputs IN 1-4 reach the In modules (2-In, 4-In) of every slot, for vocoders and effects on outside audio.
  - In a DAW the plugin has two stereo inputs next to its outputs: "In 1/2" and "In 3/4" (off until routed). They are side-chain inputs of the instrument: route audio to them as your host does for a side-chain (a chooser on the plugin, or the plugin's name as a destination in another track's output chooser). Checked in a test host with the VST3 and the Audio Unit, not yet in a DAW.
  - In the stand-alone app the audio device's inputs 1-4 are IN 1-4: choose the device and its channels in Options > Audio Settings (macOS asks once for the microphone). "Mute audio input" there silences them.
  - An input at 0 dBFS is a signal of 1.0 in the patch, the level of an oscillator, as at the outputs: a patch that only passes a 2-In to a 2-Out plays what it gets, at the level it got it. The inputs clip at +12 dBFS, where the G2's converters do.
  - The three In 1 Level LEDs of the Live view show IN 1's level, as on a G2 (-12 dB at 0 dBFS).
  - On a G2 the microphone input (XLR) feeds IN 1 while nothing is plugged into the IN 1 jack, through a preamp whose gain is the Mic Level knob. Here In 1 is the jack, and the knob does nothing, as on a G2 with a plug in IN 1. Right-click the Mic Level knob in the Live view to make In 1 the microphone input: the knob then adds 0 to +40 dB.
- **Sample rate.** The G2 computes at 96 kHz. At a 96 kHz host you get its samples as they are. At any other rate G2fresh converts them as an audio interface does when you record a G2: flat up to 20 kHz, and nothing the G2 plays above the host's range folds back into it (100 dB down). The inputs go the other way through the same filter.
- **Latency and the buffer.** The emulated G2 runs ahead of your host and keeps a reserve of audio ready, so that a moment in which the computer gives it no time is not heard. That reserve and one host block are its latency, where a real G2 answers a MIDI note in about 2 ms. **Synth > Emulated G2 Buffer (latency)** chooses the reserve, and shows what each choice gives in your host as it runs:

  | Buffer | Reserve | 48 kHz, 256-sample blocks | 48 kHz, 64-sample blocks | 44.1 kHz, 512-sample blocks |
  |---|---|---|---|---|
  | Safe (the default) | 21 ms | 27.6 ms | 23.6 ms | 34.7 ms |
  | Normal | 6 ms | 12.6 ms | 8.6 ms | 19.7 ms |
  | Low | 2 ms | 8.6 ms | 4.6 ms | 15.7 ms |

  G2fresh reports the exact figure to the host, which compensates it on playback, also when you change the buffer; what the G2 makes of an input comes out exactly that much later, and a MIDI note sounds about a millisecond sooner than that plus the G2's own 1.5 ms. The buffer is the computer's setting, not the project's: every G2fresh on this computer uses it. If the emulated G2 is ever late for the host you hear a click or a gap; G2fresh counts them and says so in the status line ("3 dropouts"): choose a larger buffer then. **All three are provisional**: Safe keeps at least the audio G2fresh kept ready up to 0.10.2 (its latency is 0.6 ms longer than 0.10.2's 27.0 ms at 48 kHz with 256-sample blocks), and Normal and Low have only been tried on a computer that other work kept busy (`re/notes/audio-path.md`, part 3); which reserves to offer, and which one is the default, will be set from measurements on a quiet computer. On macOS the emulated G2's threads now have the priority of audio threads (in the measurements so far, beside other busy programs, the difference between losing all of the audio and losing none); on Windows and Linux they keep their priority as before. Setting the environment variable `G2FRESH_EMULATOR_REALTIME=0` gives them back their former priority on macOS too.
- The expansion ports are not emulated; the pedals are not connected (the control pedal reads 0, the sustain pedal is up). Checked in the emulator (as the G2 OS reads them): the lowest key plays C1, the pitch stick bends up to the right, the mod wheel's vibrato grows upwards. Not verified: the global wheels' direction and the key velocity curve (approximated).
- On macOS the emulator generates code at run time. A plugin does so by its host's leave: a host signed with the hardened runtime must have the `allow-jit` or the `allow-unsigned-executable-memory` entitlement (Live 11, Apple's `auval` and Apple's Audio Unit hosting service have the second). In a host with neither, the Emulated G2 does not start and Synth > Connect to Emulated G2 says why in the status line; the rest of the plugin works. No such host seen so far.
- G2fresh is not affiliated with Clavia. The G2 OS is Clavia DMI AB's copyright: download it yourself from Nord's website and do not share the file; G2fresh never includes or distributes it.

## The Classic look

G2fresh draws modules in its own Modern look. **View > Classic Look** draws them as the original editor does, with the original's module faces, knobs, buttons and icons. Those graphics are Clavia's, so G2fresh does not include them: it reads them from the same free download as the Emulated G2, the **Nord Modular G2 OS v1.62 update for the Mac** (`.dmg`), which holds the original editor too. If you already chose the `.dmg` for the Emulated G2, the Classic look uses it; otherwise G2fresh asks for it the first time (the `.dmg`, or the "Nord Modular G2 Editor" app in it). The graphics are extracted once, in about a second, into a `ClassicSkin` folder next to G2fresh's settings (`~/Library/Application Support/G2fresh` on macOS), and extracted again if the file changes. If the file is missing or moved, G2fresh says so in the status line and stays in the Modern look; everything works in either look.

## Connecting a G2

Only one program can hold the G2's USB connection, while you may run the stand-alone app and several plugin instances at once. So a small background program, `g2bridge`, owns the connection and serves every editor. The editors start it when needed, and it quits a few seconds after the last one closes. It ships next to the stand-alone app's executable and inside each plugin bundle (`Contents/Resources/`). `g2bridge --emulator` serves a virtual G2 instead, for trying things without hardware.

- **macOS:** nothing to install.
- **Windows:** libusb needs the WinUSB driver for the G2. Install it once with [Zadig](https://zadig.akeo.ie/): plug in the G2, choose "Options > List All Devices", select the Nord Modular G2, choose "WinUSB" and click "Replace Driver". (Clavia's own driver cannot be used; the original editor needs it, so switch back with Zadig to use that one.)
- **Linux:** give your user access to the device with a udev rule, e.g. `/etc/udev/rules.d/50-nord-g2.rules` containing `SUBSYSTEM=="usb", ATTRS{idVendor}=="0ffc", ATTRS{idProduct}=="0002", MODE="0666"`, then `sudo udevadm control --reload` and replug the G2.

### Testing with a real G2 (USB is experimental)

Everything above is built from the original editor's code and tested against a virtual G2, not yet against a real synth. If you have a G2, you can help:

1. Connect it by USB (see the notes above for Windows and Linux), start G2fresh and choose **Synth > Connect to G2 (USB)**. The status bar shows "Looking for a G2...", then the synth's version.
2. Try **Get Patch from Slot A**, edit a knob (live editing), **Send Patch to Slot B**, **Synth Memory (Banks)...**, and watch the LEDs and meters.
3. Whatever happens, choose **Synth > Show USB Log** and send `usb.log` (and `usb.log.1` if present) with a short description to the project's [issues](https://github.com/r0main-f/G2fresh/issues). The log holds the USB traffic and nothing else, and by default it leaves your patches out: patch and performance contents and names (and the synth's memory lists) appear only as their size and a fingerprint. If we ask for more detail on a specific problem, **Synth > Full USB Log** includes them (best done with a factory patch).

## Deliberate deviations

G2fresh follows the original wherever it can: the G2 and G2X running OS 1.62, and Clavia's editor v1.62. Everything below differs on purpose. What changes the sound or the patch is off by default, except where noted. The full comparison, including the gaps not closed yet, is in [re/notes/fidelity-audit.md](re/notes/fidelity-audit.md).

**Settings, off by default**

| Setting | Where | Default (as the original) |
|---|---|---|
| Emulated G2 Follows the Host's Tempo | Synth menu | Off: the G2 keeps its own clock |
| Mix Out 3/4 into Out 1/2 | Synth menu | Off: Out 3/4 on their own output, or not heard |
| Mouse Wheel Edits Knobs | View menu | Off: the wheel scrolls |
| Double-Click on a Knob | View menu | Drag the morph range ("Reset to the Default Value" and "Nothing" are options) |
| Show Load Estimate Offline | View menu | Off: "--" until the synth reports |
| Morph Groups in Eight Colours | View menu | Off: the focused morph group in red with its range, other groups in blue |

**Emulated G2 and plugin**
- A new emulated G2 has no sounds in its memory: Clavia's factory banks are not included.
- Output level: a full-scale signal inside the G2 (1.0) reaches the host at 0 dBFS. Not yet compared with a real G2's line outputs.
- Input level: 0 dBFS at an input is a signal of 1.0 inside the G2, a quarter of its converters' full scale. Not yet compared with a real G2's line inputs.
- The microphone: one host input, In 1, is the G2's IN 1 jack or, if you choose so, its microphone input (a G2 has a connector for each, and the jack wins). The preamp's range of 0 to +40 dB is an assumption: the manual gives none.
- Stand-alone app: the audio inputs are live from the start. (JUCE mutes a plugin's inputs by default against feedback; a G2 plays its inputs only where a patch has an In module.)
- Latency: one host block and a reserve the user chooses (Synth > Emulated G2 Buffer: 21 ms unless changed, which gives 27.6 ms at 48 kHz with 256-sample blocks; 8.6 or 12.6 ms with the smaller buffers, provisional), from MIDI or an input to the output, reported to the host, where a G2 takes about 2 ms.
- Sample rate: a G2 plays through its own 96 kHz converters. The emulated G2's audio is converted to the host's rate, and its inputs from it, through a filter like a recording interface's (flat to 20 kHz; 100 dB down from 28 kHz at a 48 kHz host, from 24.1 kHz at 44.1 kHz); at a 96 kHz host nothing is converted. **This is a fix you can hear** (after 0.10.2): up to 0.10.2 the conversion had no filter, so that everything the G2 plays above the host's range folded back into it (a bright sawtooth came with partials at -40 to -20 dB that the G2 does not play, a 30 kHz partial as an 18 kHz tone at full level), and the inputs lost 2.5 dB at 20 kHz (4.4 dB at 44.1 kHz). Bright patches now sound as a G2 recorded at that rate does.
- The Master Level knob is stored in the project, as the hardware knob stays where it was left.
- The Live view keeps the G2X's layout but not its look. A mouse can hold only one button, so Option-click latches a button, a click on Shift holds it until the next button, and Shift on the computer keyboard holds Shift during a click.

**Editor: what it shows**
- G2fresh's own look: colours, an amber load threshold, the settings bar (its morph dials are focused by a click on their card), the drum preset menu.
- The Classic look needs your copy of Clavia's download (see "The Classic look"); the original has its graphics built in. Without it, G2fresh shows its Modern look only.
- Animated cables: on by default (a choice made for G2fresh; the original's cables don't move). View > Animate Cables turns them off, and the choice is kept.
- While a jack is held, its chain is highlighted as in the original, and the other cables are dimmed. What a drop will do is shown in the status line, not by the original's cursor shapes.
- Morphed sliders in the Classic look and morphed increment/decrement buttons show no morph colour (the original colours them).
- The morph dial's pop-up has no Global Assign (no global knobs yet).
- Additions with no original equivalent: zoom; a click on a cord highlights its chain, and Delete removes that chain; a cord's pop-up menu (Chain Colour, Delete Chain, bend points); cable bend points (saved next to the patch, never in it); USB log redaction; MIDI output forwarding.
- The morph range shows in the status line while dragging. There are no F5/F7 overlays, no hint box, and no Ctrl+Up/Down range keys.
- The parameter pop-up's Morph Assign item names the group it acts on.
- The Reverb's room type reads Small, Medium, Large or Hall everywhere, as on its panel and in the manual. The original's value text says Room, Hall, Plate or Gate.
- Synth Settings has the original's fields, texts and behaviour in G2fresh's look. Its five Global Octave Shift buttons are labelled -2 to +2 (the original's dialog resource gives them no text).

**Editor: what it does**
- Unsaved changes: G2fresh asks "Discard changes?" before New, Open, Get Patch and Quit. The original never asks and drops unsaved patches.
- The stand-alone app restores the last session at startup (the original starts empty). Edits never saved to a file stay marked edited; "Discard" at Quit returns to the file as saved, or to a new patch.
- Also kept as safety checks: a confirmation before overwriting a stored patch, "Open anyway" for a file with a bad checksum, messages at the module limit, no automatic download on connecting.
- Live editing sends every edit to the synth as the original does, message for message, and undo and redo send the original's inverse messages. What differs:
  - The voices, category and cable visibility (the patch header), the patch notes and variation copies are undo steps; the original has no undo for them. Undoing one sends the header or the notes again, or the variation's old values as one dump.
  - Undoing a label rename restores the old label on the synth too; the original's undo sends the new one again.
  - The variation shown is not an undo step (the original's undo goes back to the previous variation).
  - The patch notes go to the synth when their dialog is closed with OK; the original sends them as you type. (The G2 keeps its notes when it receives empty ones, with either editor.)
  - A voice count changed and changed back within 0.6 s sends nothing; the original sends the value in between.
  - The patch name goes to the synth whenever it changes; the original sends it from its rename dialog and on Save As.
  - Clear Group and Copy To in a morph dial's pop-up send one morph message per parameter, not acknowledged; what the original sends there is not known.
  - A slot's patch name edited in Performance Settings (a field the original does not have) is sent to that slot as a rename.
- In the plugin, host automation during a Patch Mutator audition edits your variation.
- Paste drops the copy below the selection and renames default-named modules. The original places it with the mouse and keeps names. Which modules make room for a moved, added or pasted one follows G2fresh's own rule; the synth gets those moves as the original sends its own.
- Patches read from the synth are not repaired as files are, so G2fresh never sends deassignments back.
- Files from older editor versions (12 to 22) are converted on opening exactly as the original converts them, and saving asks first, as the original does. G2fresh also says "converted from version N" in the status line when the file opens; the original says nothing until you save.
- An unedited version-23 file keeps its own header (for example "BUILD 266") when saved again; the original writes "BUILD 320". The release velocity is written as stored; the original's writer stores the attack velocity there.
- An unedited file saves byte for byte as it was loaded, including cable and morph lists in an order the original would have sorted. The first cable edit in an area rewrites its cable list in the original's order.
- A loaded file with a cable loop (none is known) loses the cable closing the loop at the next cable edit; the original's cable tree would be corrupted instead.
- On buttons whose maximum isn't 127, the original's morph-key toggle quirk is not reproduced.
- Labels and names:
  - Clearing a label restores the panel's caption.
  - Labels and modules are renamed from the pop-up menu.
  - Typed names are filtered per character; slot names from the synth are kept as they are.
- Input:
  - Buttons act on mouse-up, except push buttons, which act on press as in the original.
  - Shift gives fine adjustment.
  - Alt is the morph key on every platform, and works on knobs only.
  - The random generator is seeded from the clock.
  - The stand-alone app has Randomize.
  - Recent Files holds 12 entries and offers Clear.
- Shortcuts that differ:
  - ⌘D duplicates (original: Download To Slot).
  - ⌘L switches to Live (original: Parameter Overview).
  - ⌘1-8 select variations (original: 1-8).
  - ⇧⌘S is Save As.
  - Several of the original's ⌘ shortcuts are not bound yet.

## Build

```sh
brew install cmake ninja   # macOS (JUCE, Catch2 and libusb are fetched by CMake)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

`-DG2_WITH_USB=OFF` builds without libusb (the bridge then only serves the virtual G2). `-DG2_BUILD_EMU=ON` adds the Emulated G2 (it fetches the DSP56300 emulator and Gearmulator's ColdFire core, both GPL-3); the release builds have it. `tools/package/macos.sh` (`linux.sh`, `windows.ps1`) then makes a build's installer and archive in `dist/`, as CI does for a release.

## Reverse-engineering setup

You need your own copy of the original editor: the "Nord Modular G2 OS v1.62 Update" DMG from nordkeyboards.com. The binary, the Ghidra database and the decompiled output stay in the gitignored `original/`, `re/ghidra-db/` and `re/out/` directories. Only the module panel layouts are committed, without their pixels, in `assets/clavia/panels.json` (see NOTICE); `tools/assets/build_assets.py` regenerates it from the editor's `.rsrc`, and `build_assets.py --check` (run by ctest) fails if any pixel or image comes back. The Classic look's graphics are read from the user's download at run time (`clavia/`); `G2_CLAVIA_DMG=<the .dmg> ctest` also checks them, pixel for pixel, against `tests/clavia_skin_fingerprints.txt`, and `G2_LOOK=classic G2_CLAVIA_DMG=<the .dmg> g2render ...` renders with them.

```sh
lipo "Nord Modular G2 Editor.app/Contents/MacOS/Nord Modular G2 Editor" -thin i386 -output original/mac/G2Editor_i386
$GHIDRA/support/analyzeHeadless re/ghidra-db G2 -import original/mac/G2Editor_i386 \
    -scriptPath re/ghidra_scripts -postScript ExportAll.java re/out
```

On Apple Silicon, first build Ghidra's native components: `cd $GHIDRA/support/gradle && ./gradlew buildNatives`.

## License

AGPL-3.0-or-later (required by JUCE); see `LICENSE` and `NOTICE`. Not affiliated with Clavia DMI AB.
