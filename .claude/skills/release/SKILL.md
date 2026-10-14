---
name: release
description: Release a G2fresh version - bump, pull request, tag, check the built files, publish. Only when the user types /release with a version.
disable-model-invocation: true
argument-hint: "<version, e.g. 0.11.0>"
---
Release G2fresh **$ARGUMENTS**. `docs/RELEASING.md` is the reference; this is the routine. Stop and report at the first step that fails: never publish around a failure.

1. **State.** `git status` clean, on `main`, equal to `origin/main`, CI green on that commit (`gh run list --branch main -L 1`). The version must be `X.Y.Z` and greater than the last tag.
2. **Bump on a branch** (`git switch -c release-$ARGUMENTS`): the version in `CMakeLists.txt` (`project(G2fresh VERSION ...)`) and in `README.md` (the Status line, and "every one up to ..." in Download while releases are unsigned). Build, `ctest`. Commit "Version $ARGUMENTS", push, open a pull request, wait for CI green on macOS, Windows and Linux (`gh pr checks --watch`). Merge it (the user asked for the release by typing this command).
3. **Tag** the merged commit on `main`: `git tag -a v$ARGUMENTS -m "G2fresh $ARGUMENTS"`, `git push origin v$ARGUMENTS`. The tag's CI run builds the packages and creates a **draft** release.
4. **Check the files** of the draft, downloaded into a scratch folder (`gh release download v$ARGUMENTS`):
   - five files: macOS `.pkg` and `.zip`, Windows `-setup.exe` and `.zip`, Linux `.tar.gz`;
   - macOS: `vtool -arch arm64 -show-build` and `-arch x86_64` on the VST3's binary give `minos 11.0`; the app's `CFBundleShortVersionString` is the version; `codesign -dv --entitlements -` on the app shows `allow-jit` and `audio-input`; `pkgutil --expand-full` on the `.pkg` shows the three install locations;
   - the released VST3 plays and renders in `g2plugintest` with `bench/hardware-pack/patches/B01 Level Ref.pch2` (on a quiet machine: check `uptime` first);
   - nothing of Clavia's: no `.png`/`.jpg`/`.bmp` file in the macOS package, no image data in its binaries;
   - Windows and Linux: the archives list the expected files (they cannot be run here: say so).
5. **Notes.** Write them for a user, not a developer: what is new, what is fixed, what is still not verified (real G2, DAWs, platforms never played on), whether the files are signed and how to open them if not. Keep the "How these files are signed" lines the build wrote in the draft. Show the notes to the user before publishing if anything in them is a judgement call.
6. **Publish:** `gh release edit v$ARGUMENTS --title ... --notes-file ... --draft=false`, with `--prerelease` as long as the README says "experimental". Confirm with `gh release view`.
7. **Report:** the link, what was checked on the files, what was not.
