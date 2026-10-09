# G2fresh

A modern, native editor for the Clavia **Nord Modular G2**, for macOS (Apple Silicon and Intel), Windows and Linux. It runs as a stand-alone app and as a VST3/AU plugin.

Clavia's last editor (v1.62) is a 32-bit Carbon app on the Mac and depends on a proprietary kernel driver on Windows, so it no longer runs on current systems. G2fresh is a rewrite. Ghidra analysis of the original editor serves as the specification, cross-checked against Bruno Verhue's open-source editor.

**Status:** early. Reverse-engineering specs are in progress in `re/notes/`.

## Layout

| Path | What |
|------|------|
| `re/ghidra_scripts/` | Headless Ghidra scripts (`ExportAll.java`: decompiled C, symbols, call graph, strings) |
| `re/notes/` | Recovered specifications (file format, USB protocol, resources, parameter display) |
| `tools/rsrc/` | Extractors for the original editor's resources (`rsrc.py`, `panl.py`) |
| `core/` | `libg2core`: patch model, `.pch2`/`.prf2` codec |
| `tests/` | Catch2 tests; `tests/corpus/` holds freely licensed patch files |
| `third_party/nord_g2_editor` | Bruno Verhue's editor (GPL-2-or-later), used as a reference |

## Build

```sh
brew install cmake ninja catch2 libusb   # macOS (JUCE is fetched by CMake)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

## Reverse-engineering setup

You need your own copy of the original editor: the "Nord Modular G2 OS v1.62 Update" DMG from nordkeyboards.com. Nothing from it is committed. Everything derived from it lives under the gitignored `original/`, `re/ghidra-db/` and `re/out/` directories.

```sh
lipo "Nord Modular G2 Editor.app/Contents/MacOS/Nord Modular G2 Editor" -thin i386 -output original/mac/G2Editor_i386
$GHIDRA/support/analyzeHeadless re/ghidra-db G2 -import original/mac/G2Editor_i386 \
    -scriptPath re/ghidra_scripts -postScript ExportAll.java re/out
```

On Apple Silicon, first build Ghidra's native components: `cd $GHIDRA/support/gradle && ./gradlew buildNatives`.

## License

AGPL-3.0-or-later (required by JUCE); see `LICENSE` and `NOTICE`. Not affiliated with Clavia DMI AB.
