---
name: run-g2fresh
description: How to build, launch and drive G2fresh on this machine to see a change working - the app's windows as images, the plugin in a host, the emulated G2 on the command line. Use when asked to run, start or screenshot G2fresh, or by /verify to reach the running app.
---
Build first (`CLAUDE.md`, "Build and test"). Then pick the surface the change reaches. Every command below was run on this machine; add to this file when you find a handle that is missing.

## The editor's windows, as images (no window opens)

`build/tools/render/g2render_artefacts/Release/g2render`:
- `g2render ui <in.pch2> <out.png>`: the whole editor with a patch open.
- `g2render patch <in.pch2> <out.png>`: the patch alone. `g2render modules <out.png> [typeId...]`: module panels.
- `g2render live <out.png> [--off]`: the Live view (the emulated front panel and keyboard).
- `g2render synthsettings <out.png>`: the Synth Settings window.
- `G2_LOOK=classic` in the environment draws the Classic look; it needs `G2_CLAVIA_DMG=<the user's .dmg>` (a scratch copy), and says on stderr why it fell back to Modern if it did.

Read the PNG with the Read tool to look at it. This reaches painting and layout, not clicks.

## The plugin, as a DAW runs it

`build/tools/plugintest/g2plugintest_artefacts/Release/g2plugintest.app/Contents/MacOS/g2plugintest <G2fresh.vst3 | G2fresh.component> <patch.pch2>`: loads the plugin, starts its emulated G2, plays the patch in real time, saves and restores its state as a host does, renders offline. Always give a patch that sounds by itself (`bench/hardware-pack/patches/B01 Level Ref.pch2`); without one, silence is correct.
- `--inputs <patch>`: audio through In 1-4 (build the patch with `g2mkpatch`, see the tool's usage).
- `--outputs <patch>`: Out 1/2 and Out 3/4 on their buses. `--restore <state>`: a state a host saved.
- It needs the user's OS: `G2_FIRMWARE=<scratch copy of the .dmg>` (or the path G2fresh's settings already hold).
- Its real-time checks fail on a busy machine (load average above about 2): check `uptime` before believing a "plays the patch" failure.

## The emulated G2 alone

`build/emu/g2emurun`: boots the user's OS, connects G2fresh's protocol client, uploads patches, plays notes and records the outputs (`--fw <dmg>`, `--flash <file>`, `--in N:HZ[:LEVEL]`, `--kbd ...`; the options are at the top of `emu/g2emu/tools/g2emurun.cpp`). The firmware tests in `tests/test_g2emu.cpp` and `tests/test_live_firmware.cpp` show how to drive the OS from code: keys, panel buttons, USB messages, reading a patch back.

## The app itself

`build/plugin/G2Editor_artefacts/Release/Standalone/G2fresh.app`. Launching it uses the user's real settings and emulated memory (`~/Library/Application Support/G2fresh`) and puts a window on their screen: ask before doing it, and prefer the surfaces above. A way to run it against scratch settings has not been written down here yet.
