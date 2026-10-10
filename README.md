# G2fresh

A modern, native editor for the Clavia **Nord Modular G2**, for macOS (Apple Silicon and Intel), Windows and Linux. It runs as a stand-alone app and as a VST3/AU plugin.

Clavia's last editor (v1.62) is a 32-bit Carbon app on the Mac and depends on a proprietary kernel driver on Windows, so it no longer runs on current systems. G2fresh is a rewrite. Ghidra analysis of the original editor serves as the specification, cross-checked against Bruno Verhue's open-source editor.

**Status:** 0.9.0, a patch editor that also plays the patch by itself (the **Emulated G2**, experimental, with a playable front panel and keyboard in the **Live** view), with an experimental USB connection. It opens, edits and saves G2 patches and performances; every public G2 file we know of loads (5,589 files from Clavia's banks and the community archives). It has the full patch settings, morph, knob and MIDI controller assignments, copy/paste and duplicate, module Replace, the randomizer and Patch Mutator, the original's special controls, the patch load meter, the modules' live graphs and descriptions, a modern look and the original Classic look, zoom, animated cables with bend points, and native menus. As a VST3/AU plugin it stores the patch in your DAW project, exposes the patch's knobs, morph dials and variation for automation, and forwards its MIDI to a hardware port into the G2. The Synth menu connects to a G2 over USB (through `g2bridge`) or to a virtual G2: send and get patches and performances, live editing both ways, the synth's memory banks, live LEDs and meters, the load the synth reports. **The USB connection has only been tested against the virtual G2**: see "Testing with a real G2" below. Without a G2, the Emulated G2 runs the G2's own operating system (from Clavia's free OS update, which you download yourself) inside G2fresh, so any patch plays in the app or in your DAW: see "Playing without a G2" below.

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
| `assets/clavia/` | Original module layouts and graphics, bundled into the app (Clavia's property, see NOTICE) |
| `data/` | Module and parameter database (JSON), compiled into the core by `tools/moduledb/gen_cpp.py` |
| `plugin/` | JUCE target: stand-alone app, VST3, AU |
| `emu/` | Emulation, built with `-DG2_BUILD_EMU=ON`: `g2emu`, the whole G2 in C++ running the user's own firmware (`re/notes/g2-hardware-and-emulation.md` §3.9), and the DSP experiments |
| `tests/` | Catch2 tests; `tests/corpus/` holds freely licensed patch files. Set `G2_EXTRA_CORPUS=<dir>` to also test your own patches |
| `third_party/nord_g2_editor` | Bruno Verhue's editor (GPL-2-or-later), used as a reference |

## Download

Builds for macOS (universal), Windows and Linux are attached to each [release](https://github.com/r0main-f/G2fresh/releases). They are not code-signed:
- **macOS:** after unzipping, right-click the app (or plugin installer) and choose Open. If macOS says the app "is damaged", run `xattr -dr com.apple.quarantine G2fresh.app`. Copy `G2fresh.vst3` to `~/Library/Audio/Plug-Ins/VST3/` and `G2fresh.component` to `~/Library/Audio/Plug-Ins/Components/`.
- **Windows:** SmartScreen may warn: choose "More info", then "Run anyway". Copy `G2fresh.vst3` to `C:\Program Files\Common Files\VST3\`.
- **Linux:** copy `G2fresh.vst3` to `~/.vst3/`.

## Playing without a G2

G2fresh can make the G2's sound itself, in the stand-alone app and in the plugin, with the **Emulated G2 (experimental)**: a whole G2 in software (its processor, its four DSPs, the USB link) running Clavia's own G2 OS 1.62, which therefore plays every module and patch. G2fresh contains no Clavia code: download the free **Nord Modular G2 OS v1.62 update for the Mac** from Nord's website ([nordkeyboards.com](https://www.nordkeyboards.com), in the Nord Modular G2 downloads) and keep the `.dmg`; G2fresh reads the Mac download on every system. Then choose **Synth > Connect to Emulated G2**: the first time, G2fresh asks where the `.dmg` (or the updater app in it) is. The synth boots in about 2 seconds; your patch is then sent to it and plays from MIDI (the plugin's track, or a MIDI input in the app's Options), and every edit, variation, knob and memory bank works as with a G2. In a DAW, each G2fresh instance keeps its emulated G2's memory (stored patches and settings) in the project and starts it again when the project opens; the stand-alone app keeps its memory in `Emulated G2 flash.bin` next to G2fresh's settings.

**Live** (in the toolbar, **View > Live** or Cmd L) turns the editor into the instrument: the Emulated G2's front panel, laid out as on a G2X and driven by the G2 OS itself (its five displays and menus, LEDs, knob rings, buttons, knobs and dial, master level), the pitch stick, the mod wheel and the two global wheels, and a 61-key keyboard played with the mouse (velocity from where a key is hit) or the computer keyboard (Z/X: octave). The keys are the G2's own, so focus, octave shift, KB Hold and split work as on the machine. G2fresh switches the G2 OS's MIDI Local on when it starts (an empty memory starts with it off).

- It needs a fast computer: one emulated G2 uses about 1.3 (light patches) to 1.6 (heavy ones) CPU cores of an Apple M1, and runs at 2-2.8 times real time there. Only the first G2fresh instance in a project starts one by itself; others start from the Synth menu or the Live view (each runs its own emulated G2).
- The latency from a MIDI note to its sound is about 32 ms at 48 kHz with 512-sample blocks (26 ms with 256); G2fresh reports it to the host, which compensates it on playback.
- The audio inputs and the expansion ports are not emulated; the pedals are not connected (the control pedal reads 0, the sustain pedal is up). Checked in the emulator (as the G2 OS reads them): the lowest key plays C1, the pitch stick bends up to the right, the mod wheel's vibrato grows upwards. Not verified: the global wheels' direction and the key velocity curve (approximated).
- In a DAW on macOS, the emulator generates code at run time; a host that forbids it cannot run it (not seen so far).
- G2fresh is not affiliated with Clavia. The G2 OS is Clavia DMI AB's copyright: download it yourself from Nord's website and do not share the file; G2fresh never includes or distributes it.

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

## Build

```sh
brew install cmake ninja   # macOS (JUCE, Catch2 and libusb are fetched by CMake)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

`-DG2_WITH_USB=OFF` builds without libusb (the bridge then only serves the virtual G2). `-DG2_BUILD_EMU=ON` adds the Emulated G2 (it fetches the DSP56300 emulator and Gearmulator's ColdFire core, both GPL-3); the release builds have it.

## Reverse-engineering setup

You need your own copy of the original editor: the "Nord Modular G2 OS v1.62 Update" DMG from nordkeyboards.com. The binary, the Ghidra database and the decompiled output stay in the gitignored `original/`, `re/ghidra-db/` and `re/out/` directories. Only the module layouts and graphics the UI needs are committed, in `assets/clavia/` (see NOTICE); `tools/assets/build_assets.py` regenerates them.

```sh
lipo "Nord Modular G2 Editor.app/Contents/MacOS/Nord Modular G2 Editor" -thin i386 -output original/mac/G2Editor_i386
$GHIDRA/support/analyzeHeadless re/ghidra-db G2 -import original/mac/G2Editor_i386 \
    -scriptPath re/ghidra_scripts -postScript ExportAll.java re/out
```

On Apple Silicon, first build Ghidra's native components: `cd $GHIDRA/support/gradle && ./gradlew buildNatives`.

## License

AGPL-3.0-or-later (required by JUCE); see `LICENSE` and `NOTICE`. Not affiliated with Clavia DMI AB.
