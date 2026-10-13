# G2fresh: working rules

G2fresh is an editor for the Clavia Nord Modular G2 (C++20, JUCE: stand-alone app, VST3, AU) with an emulator of the whole G2 (`emu/g2emu`) that runs the user's own copy of Clavia's OS 1.62 and is the sound engine. Layout and user documentation: `README.md`. Research notes: `re/notes/`.

## Rules that decide the work

1. **The original first.** Every feature behaves as the G2/G2X hardware with OS 1.62 and Clavia's editor v1.62 do. Before building or changing behaviour, establish what the original does and cite it (address in Clavia's code, manual page, a measurement in the emulator). A difference must be deliberate, off by default if it changes what is heard or seen, and listed in README "Deliberate deviations". The `fidelity-checker` agent and `/fidelity` do this check.
2. **Nothing of Clavia's is committed or shipped.** No firmware, images, help or manual text, factory patches, or output of their analysis. The Classic look and the OS come from the user's own download at run time. Ignored folders that hold such material, read-only: `original/`, `re/out/`, `re/ghidra-db/`, `corpus-external/`.
3. **Other editors are references only** (`references/`, index in `re/notes/references.md`): compare, cite file and commit, never copy code.
4. **Other people's repositories:** no `Co-Authored-By` and no "Generated with" line in anything sent upstream, and the user reviews it first.
5. **Nothing leaves the machine without the user's word:** no merge into `main`, tag, release, or message to anyone until they ask for that step.
6. **Say what was not verified.** Tests passing is not the app working; a build on macOS says nothing about Windows.

## How changes reach main

`main` is protected: changes arrive by pull request with CI green on macOS, Windows and Linux.

- Work on a branch (`git switch -c <topic>`); agents work in worktrees under `.claude/worktrees/`.
- Before opening a pull request: `/check` (build, tests, the plugin in a host).
- Open the pull request with `gh pr create`; describe what changed, what the original does, what was verified and what was not.
- Merge only when the user says so, with a merge commit (`gh pr merge --merge --delete-branch`), so each step's commits stay readable.
- A release is `/release <version>`, typed by the user. `docs/RELEASING.md` describes the steps.
- Commit messages: a short imperative subject, a body where it helps, ending with the `Co-Authored-By` line the session gives.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DG2_BUILD_EMU=ON
cmake --build build
ctest --test-dir build -j4
```

- `-DG2_BUILD_EMU=OFF` must also build (a second build directory, e.g. `build-noemu`).
- Tests that need Clavia's files are skipped unless these are set:
  - `G2_FIRMWARE=<Nord Modular G2 OS v1.62 Update.dmg>`: the emulator runs the real OS (live editing, settings, inputs...).
  - `G2_CLAVIA_DMG=<the same .dmg>`: the Classic look's images, compared with fingerprints.
  - `G2_EXTRA_CORPUS=<folder of .pch2/.prf2>`: more patch files for the file tests.
- Copy the `.dmg` to a scratch folder first and point the variables there: a new binary reading `~/Downloads` raises a macOS privacy prompt.
- The plugin in a host, as a DAW uses it (always with a patch that sounds by itself, otherwise silence is the right answer):
  `build/tools/plugintest/g2plugintest_artefacts/Release/g2plugintest.app/Contents/MacOS/g2plugintest build/plugin/G2Editor_artefacts/Release/VST3/G2fresh.vst3 "bench/hardware-pack/patches/B01 Level Ref.pch2"` (`--inputs <patch>`, `--outputs <patch>`, `--restore <state>` are its other modes).
- Real-time results (dropouts, latency, speed) are only valid on a quiet machine: check the load average first, and never measure while other agents build.
- `python3 tools/benchpack/expected.py --check` re-renders the bench pack through the emulator and compares.
- Windows and Linux are built by CI only: write `(std::min)`/`(std::max)` where `windows.h` may be included, and no POSIX-only calls outside guarded code.

## Code

- Match the surrounding code: its naming, comment density and idiom. Comments say why, and cite the original where behaviour comes from it.
- Tests assert facts that hold on any machine: no sleeps, no counts that depend on speed (see `tests/test_usb_transport.cpp` for a clock the test moves).
- Don't install into the user's plug-in folders or `/Applications`, and don't touch their settings (`~/Library/Application Support/G2fresh`), unless asked.
