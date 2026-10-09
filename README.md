# G2fresh

A modern, native editor for the Clavia **Nord Modular G2**, for macOS (Apple Silicon and Intel), Windows and Linux. It runs as a stand-alone app and as a VST3/AU plugin.

Clavia's last editor (v1.62) is a 32-bit Carbon app on the Mac and depends on a proprietary kernel driver on Windows, so it no longer runs on current systems. G2fresh is a rewrite. Ghidra analysis of the original editor serves as the specification, cross-checked against Bruno Verhue's open-source editor.

**Status:** 0.2, an offline patch editor. It opens, edits and saves G2 patches and performances; every public G2 file we know of loads (5,589 files from Clavia's banks and the community archives). It runs as a stand-alone app and as a VST3/AU plugin that stores the patch in your DAW project. Not yet: talking to the synth over USB, the patch-settings panel, and the small module graphs.

## Layout

| Path | What |
|------|------|
| `re/ghidra_scripts/` | Headless Ghidra scripts (`ExportAll.java`: decompiled C, symbols, call graph, strings) |
| `re/notes/` | Recovered specifications (file format, USB protocol, resources, parameter display) |
| `tools/rsrc/` | Extractors for the original editor's resources (`rsrc.py`, `panl.py`) |
| `core/` | `libg2core`: `.pch2`/`.prf2` codec (`file.hpp`), patch model (`patch.hpp`), edit operations (`edit.hpp`), module database (`module_db.hpp`), parameter display text (`param_text.hpp`) |
| `assets/clavia/` | Original module layouts and graphics, bundled into the app (Clavia's property, see NOTICE) |
| `data/` | Module and parameter database (JSON), compiled into the core by `tools/moduledb/gen_cpp.py` |
| `plugin/` | JUCE target: stand-alone app, VST3, AU |
| `tests/` | Catch2 tests; `tests/corpus/` holds freely licensed patch files. Set `G2_EXTRA_CORPUS=<dir>` to also test your own patches |
| `third_party/nord_g2_editor` | Bruno Verhue's editor (GPL-2-or-later), used as a reference |

## Download

Builds for macOS (universal), Windows and Linux are attached to each [release](https://github.com/r0main-f/G2fresh/releases). They are not code-signed:
- **macOS:** after unzipping, right-click the app (or plugin installer) and choose Open. If macOS says the app "is damaged", run `xattr -dr com.apple.quarantine G2fresh.app`. Copy `G2fresh.vst3` to `~/Library/Audio/Plug-Ins/VST3/` and `G2fresh.component` to `~/Library/Audio/Plug-Ins/Components/`.
- **Windows:** SmartScreen may warn: choose "More info", then "Run anyway". Copy `G2fresh.vst3` to `C:\Program Files\Common Files\VST3\`.
- **Linux:** copy `G2fresh.vst3` to `~/.vst3/`.

## Build

```sh
brew install cmake ninja catch2 libusb   # macOS (JUCE is fetched by CMake)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

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
