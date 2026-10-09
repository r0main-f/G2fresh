# Editor resource formats (`Nord Modular G2 Editor.rsrc`)

Source: `Contents/Resources/Nord Modular G2 Editor.rsrc` in the Mac editor v1.62.
This is a classic Resource Manager map stored in the **data fork** (16-byte
header: data offset, map offset, data length, map length, all big-endian).
`tools/rsrc/rsrc.py` reads it.

| Type | Count | Content |
|------|------:|---------|
| `PANL` | 194 | Module panel definitions, as **plain text** (Mac Roman, CRLF). See below. |
| `CBMP` | 206 | **Windows BMP files** (`BM` header, 24-bit, bottom-up), stored as-is. Panel artwork, knobs, buttons, cable/jack sprites. All named "Foo". |
| `SMTB` | 88 | Also plain **BMP files**. Small toolbar/button glyphs (e.g. 16×15). |
| `SMNU` | 1 | Menu bar script in text form: `{File~F; New Patch#N~N=100; …}`, i.e. title, `#` shortcut, `~` mnemonic, `=` command id. Mac variant blocks: `{{MacOSX …}}`. |
| `JPEG` | 73 | JPEG images (backgrounds, splash). |
| others | — | Standard Carbon UI (`DLOG`, `DITL`, `ALRT`, `MENU`, `CURS`, `cicn`, icons, `vers`). |

## CBMP contents

All convert cleanly with `tools/rsrc/bmp2png.py`, which also handles the OS/2 v2 (64-byte) info header that macOS `sips` rejects.

- **Module faces** (about 195): 256 px wide, height = 15 px × module height (30/45/60/…/180). These are the module background with static labels and graphics baked in, e.g. EnvADDSR's "Sustain, Gate, A D1 L1 D2 L2 R, Shape, Env". Live controls (knobs, jacks, buttons, LEDs, text fields) are drawn on top at the PANL positions.
  - The face id is **not** derived from the PANL id: it's the second argument of the module's `CMBGeneric` registration (e.g. S&H: panel 357, face 356; T&H: panel 359, face 358). `CPanel::SetColor` @ `000c2124` loads it as `CBMP`, or as a `JPEG` with the same id.
- **200**: knob pointer sprite strip, 1856×29 = 64 frames of 29×29, white pointer on black (used as a mask).
- **205**: 210×21 colour swatches (module/cable colour picker).
- **199, 204** (230×23, 190×19): probably bigger and smaller swatch or button strips. **201–203** (29×8) and **531/532** (11×5): small glyphs (arrows, LED states). Still to identify.

## PANL grammar

```
block   := "<#" Kind NEWLINE { field | block } "#>"
field   := Key ":" ( Integer | '"' text '"' )      // Key may contain spaces ("Text Func")
```

Top level is always a single `<#Module …#>` block:

| Module field | Meaning |
|--------------|---------|
| `Name` | Short name shown in the module title (`OscA`, `FltNord`). |
| `FileName` | Internal name (`PitchShuttle` for Scratch); likely the DSP/code name. |
| `Tooltip` | Long name. |
| `Height` | Module height in grid units. |
| `XPos`/`YPos` | Title text position (TBD, verify). |
| `Version` | Panel version (206/208/238…). |

Child element kinds (counts over all panels): `Text` 1042, `Input` 517, `Knob` 502,
`Output` 352, `Line` 328, `TextField` 264, `ButtonText` 235, `Led` 195,
`ButtonFlat` 170, `Symbol` 79, `ButtonIncDec` 75, `Bitmap` 59, `Graph` 46,
`MiniVU` 40, `PartSelector` 31, `TextEdit` 28, `ButtonRadio` 26, `LevelShift` 16,
`ButtonRadioEdit` 6, `Frame` 5, `EnvCurve` 2.

Common element fields:
- `ID`: element id, unique within the panel.
- `XPos`, `YPos`: pixel position inside the module (module width is 255 px; rows are 15 px).
- `CodeRef`: index of the parameter (for controls) or connector (for jacks) in the module's DSP definition. This is the index used in patch files and USB messages.
- `InfoFunc`: index of the **value-to-text function** used for the tooltip/display (see `param-display.md`).
- `Type`: knob size (`Small`, `Medium`, `Reset/medium`…) or jack signal (`Audio`, `Control`, `Logic`).
- `Bandwidth`: jack rate, `Static` or `Dynamic` (Dynamic follows the module's uprate).
- `ZPos`: draw order.
- `Text`: label, or comma-separated option labels for buttons (`"12.5m,25m,50m,100m"`).
- `Image`: inline bitmap as colon-separated `rrggbb` pixels, row by row; `ImageWidth` gives the row length and `ImageCount` the number of frames.
- `TextField.MasterRef` / `Text Func` / `Dependencies`: a display whose text is computed from one or more parameters. The number of `Dependencies` picks the single/dual/triple table; "S0"/"s0" refers to mode 0.

## Resolved questions
1. **Type id → PANL/face:** `CModuleFactory::CModuleFactory` @ `000adce0` registers each module with `new CMBGeneric(panelResId, faceBitmapId, _k<Name>ModuleInfo)`. Of each duplicate-name pair, only one panel is referenced. See `module-db.md`; the mapping is in `data/modules.json` (`panelResId`, `faceResId`).
2. **`InfoFunc` / `Text Func`:** ids index three tables (single, dual and triple dependency), and the module definition's own id takes priority over the panel's `InfoFunc`. See `param-display.md`; the port is `core/src/param_text.cpp`.
3. **Panel-only modules:** Compress and Eq3band are real modules (spelling differs). The other 20 panel-only names (SyncOsc, PulseOsc, RndChaos, OutBusA/B, …) have no module table or type id, so the original editor can't create or load them. They are leftovers.
