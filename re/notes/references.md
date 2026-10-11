# References for bench testing

Other G2 projects and real-hardware material, kept to cross-check G2fresh where no G2 is at hand. Their code is a
reference only: nothing is copied into G2fresh. The original (Clavia's editor v1.62 and OS 1.62) still decides;
hardware evidence settles what its code leaves open.

## Other editors (cloned in `references/`, gitignored)

| Project | Licence | Commit cloned | What it offers as a reference |
|---|---|---|---|
| [chrispurusha/G2-Edit](https://github.com/chrispurusha/G2-Edit) | GPL-3.0 | `82224c0` (2026-10-11) | macOS editor and plugin with its own sound engine, tested on a real G2. `Docs/code-notes/usbComms.c.md`: real-hardware USB behaviour. `Docs/findings.md`, `Docs/sound-engine-reference.md`, `Docs/capture-inventory.md`: audio measured from a G2 (levels, filters, envelopes, reverb, pads), the captures themselves kept offline by the author |
| [sirlensalot/g2fx](https://github.com/sirlensalot/g2fx) | BSD-3-Clause | `89ffa2b` | Editor ported from Verhue's and g2ools. `data/capture/*.pcapng`: 18 USB captures of a real G2X (decoded in `usb-crosscheck.md`) |
| [ty-art-ty/g2-webeditor](https://github.com/ty-art-ty/g2-webeditor) | BSD-3-Clause | `ded17c5` | Browser editor on a headless board next to the G2, "verified against real hardware"; vendors g2fx's library |
| [JanBurp/NordModularG2-Editor](https://github.com/JanBurp/NordModularG2-Editor) | AGPL-3.0 | `a1a7e83` | Electron editor with a C USB daemon; protocol and file-format docs, hardware notes (e.g. the ~34 KB message hang) |
| [BVerhue/nord_g2_editor](https://github.com/BVerhue/nord_g2_editor) | GPL-2.0+ | local clone in `third_party/` | The older open-source editor; source of some module-database facts |

Refresh a clone with `git -C references/<name> pull`; note the new commit here when a finding cites it.

## Sound recorded from a real G2

| Source | What | Use |
|---|---|---|
| [nitetime productions](https://s3-eu-west-1.amazonaws.com/nitetimeproductions.co.uk/nord%20modular%20g2%20patches.html) | 19 recordings (music, drum and sound-design patches) made only on a G2, "no samples or external effects", "no further processing", each with its `.pch2`/`.prf2` (G2X or expanded G2 for the music ones). All rights reserved: listen and measure, never commit | Pitch, timbre, envelope timing, rough level; not a null test (random modules, unknown variation and level) |
| G2-Edit's captures (listed in its `Docs/capture-inventory.md`) | Hundreds of 48/192 kHz captures of single modules on a known rig | Not online; ask the author |
| [Autechre AE_LIVE](https://spectralplex.com/get-autechres-nord-g2-patches-run-on-the-nord-modular-g2-editor-on-mac-osx/) | Their 2014-15 live patches; recordings exist as released music | Sanity only |
| Clavia's G2 demo (`original/demo/`, gitignored) | Clavia's own software G2, one voice, half the G2's rate | A second implementation from Clavia, not hardware |

What a comparison can and cannot show: the emulator runs Clavia's own OS and DSP code, so a difference in a patch's
sound points first at what lies outside that code, in order: G2fresh's resampling to the host rate, the output scaling
(`fidelity-audit.md` A8), the analogue stage (DAC, output filters), then the DSP emulation itself.
