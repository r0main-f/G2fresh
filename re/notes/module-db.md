# Module database (editor v1.62)

How the original editor (Mac, i386) defines its modules, and how
`data/modules.json` / `data/params.json` are built from it.

Pipeline (Ghidra facts are committed, so the build needs neither Ghidra nor Clavia files):

```
ModuleDbDump.java   -> <dump>/blocks.json, block_N.bin, datasyms.tsv  (memory + data symbols)
ModuleDbDisasm.java 0010ae74 -> adddata.asm                           (CReplaceDataBase::AddData)
tools/moduledb/extract_ghidra_facts.py --dump <dump> --decomp re/out/decomp \
    --replace adddata.asm --panels original/derived/panels.json --cbmp original/derived/CBMP \
    -o tools/moduledb/ghidra_facts.json
tools/moduledb/build_moduledb.py      -> data/modules.json, data/params.json
```

## 1. Type id → panel: the mapping

There is no lookup table and no name match. The link is made once, at start-up,
in **`CModuleFactory::CModuleFactory` @ `000adce0`** (identical C1 copy @ `000b475a`).
For every module it runs:

```c
TBitmapID bmp(0x375);                                   // face bitmap (CBMP id 885)
CMBGeneric *b = new CMBGeneric(0x37a /*PANL id 890*/, bmp, _kOut2ModuleInfo);
RegisterModuleBuilder(this, b, 1);                      // 000adb14
```

* `CMBGeneric::CMBGeneric(short panl, TBitmapID, const SModuleInfo&)` @ `00085f88` →
  `CBuildModule::CBuildModule(const SModuleInfo&)` @ `000aa93a` copies the **type id
  from `SModuleInfo` byte 0** into `CBuildModule+8`, then `CMBGeneric::Init` @ `00085d9c`
  does `COpenResourceFile::Read("PANL", panl)` and keeps the parsed panel.
* `RegisterModuleBuilder` stores the builder in a `std::vector<CBuildModule*>` of 220
  (0xDC) slots **indexed by the type id** (`GetType()`), and, if the creator flag is 1,
  assigns it the current category and the next toolbar slot.
* `CModuleFactory::CreateModuleFromType(uchar)` @ `000ad910` / `CreatePanelFromType` @
  `000ad880` index that vector; an empty slot throws `XPOutOfRange(7)`.
  `IsModuleAvailable` @ `000ad702` always returns 1.

So the PANL id is a constructor argument paired with a statically linked
`_k<Name>ModuleInfo` table; the type id lives in that table. This makes the duplicate
names harmless: ClkDiv **420** (type 69), CompLev **422** (59), LevMult **737** (44),
Delay **824** (LogicDelay, 42) are used; 811, 815, 739, 831 and both ClkDivFix (421, 813)
are never referenced.

Counts: 165 `CMBGeneric` modules + 1 `CBModuleName` (type 126 "Name", height 1, face
CBMP 0x1fe = 510) + 7 `CBModuleToolbar` patch-settings pseudo-modules (registered with creator
flag 0, so not in the browser): Morph 6, Gain 95 (`_kToolbarModuleInfo`), Glide 135,
Arpeggiator 136, Bend 137, Vibrato 138, Misc 153. 172 `_k*ModuleInfo` symbols exist, all
registered. 165 of the 194 PANL resources are used.

### Face bitmap (`faceResId`)

The module face (CBMP, 255/256 px wide, 15 px per height unit, labels baked in) is the
**second constructor argument**, not derived from the PANL id. `CMBGeneric::Init` @
`00085d9c` copies that `TBitmapID` to `CBuildModule+0x2c`; `CModuleFactory::CreatePanelFromType`
@ `000ad880` calls **`CPanel::SetColor(EModuleColor)` @ `000c2124`**, which takes
`TBitmapID::GetID(builder+0x2c)` (builder = `CPanel+0xa8`), checks
`COpenResourceFile::DoesExist("CBMP", id)` and reads it (`CBMPUtils::UnpackBMPTo32Bit`), falling
back to a `JPEG` resource of the same id, then tints it with the module colour
(`CPanel::ColorBitmap`). The Name module uses 510 (`CBModuleName` ctor), the 7 patch-settings
pseudo-modules have none. All 166 faces exist and their heights equal 15 × PANL `Height`.
The pairing is irregular (PANL+1 for 67 modules, −1 for 31, +2, −13, +15, … others); e.g.
S&H PANL 357 → CBMP 356, T&H PANL 359 → CBMP 358.

Panels without a builder have no face in the editor. `faceResIdCandidate` (unproven) is the
otherwise unused face-shaped CBMP at PANL id + 1 with the right height: Driver 998,
Resonator 996, Red2Blue 1022, Blue2Red 1024, PolarPan 675, PolarFade 679, AR-Env 699,
OutBusA 803, SeqA 827, PeakFollow 915, EnvDX 919, LfoD 962, RndStep 1010, RndState 1012,
RndChaos 1016. Unused face-shaped CBMPs left: 394, 395, 396, 403, 408, 425, 509, 730, 731
(probably PulseOsc/SyncOsc, both height 6), 754, 759, 760, 804 (probably OutBusB), 834,
882, 883.

### Categories (module browser)

`RegisterCategory(name, CRGBColor)` @ `000adb92` pushes a page; inlined
`RegisterSpacing` (`*back()+=1`) leaves an empty toolbar slot (separator).
`GetModuleByIndex(category, slot)` @ `000ada16` scans the vector for the builder with that
category/slot. The 16 pages in order: In/Out, Note, Osc, LFO, Rnd, Env, Filter, FX, Delay,
Shaper, Level, Mixer, Switch, Logic, Seq, MIDI (colours in `modules.json`).
`toolbarSlot` keeps the spacer gaps; `pageIndex` is the rank inside the page and equals
Verhue's `PageIndex` for all 166 modules.

## 2. Tables

### `SModuleInfo` (0x24 bytes, `_k<Name>ModuleInfo`, `__const` @ ~002a6fc0…)

| Off | Size | Field | Used by |
|----:|-----:|-------|---------|
| 0x00 | u8 | type id | `CBuildModule+8`, `GetType` |
| 0x04 | ptr | param specs `_k<Name>ParamData` (n × 0x1C) | `CBuildModule+0x10` |
| 0x08 | ptr | `_k<Name>ParamEditOrder` (n bytes) | `GetNextParamFrom`/`GetPreviousParamFrom` (knob-floater/keyboard order; Verhue's `DefaultKnob` is its first 8 entries) |
| 0x0C | ptr | `_k<Name>ParamDefValue` (n bytes, = spec+0x0C) | `GetDefaultParamValue` |
| 0x10 | ptr | `_k<Name>LedData` (named LEDs, 16 bytes each: char[8] name, i32 kind 1=VU/2=LED, i32 count) | `GetLedName` |
| 0x14 | ptr | `_k<Name>ModuleSize`: DSP resource use, 10×i16 + 3×i32 (A-code/B-code cycles and X/Y/P/Q memory; B parts folded into A when uprated) | `CModule::GetModuleResourceSpec`, `CPatchLoad::AddModule/RemoveModule`, `CModule::HasBCode` |
| 0x18 | u8 | param count | |
| 0x19 | u8 | input count | `GetInputCount` (`+0x22`) |
| 0x1A | u8 | output count | `GetOutputCount` (`+0x21`) |
| 0x1B | u8 | mode ("part selector") count | `GetPartSelectorCount` |
| 0x1C | u16 | named-LED count | |
| 0x1E | u8 | keep-if-unused (1 for CtrlSend, PCSend, NoteZone, Automate) | `CPatchData::GetDeleteUnusedModulesMolecules` @ `000e8d02` |
| 0x20 | u32 | context mask: bit0 FX, bit1 VA, bit2 patch settings (`NSFile_V7::EContext`) | `CBuildModule::IsValidContext` @ `000aa00e`; only Fx-In (127) = 1 (FX only) |

### Param spec (0x1C bytes, `_k<Name>ParamData[i]`)

| Off | Field |
|----:|-------|
| 0x00 | char[11] short name (what the editor shows) |
| 0x0B | max value (min is always 0) |
| 0x0C | default value |
| 0x0D | morphable (`IsParamMorphable`) |
| 0x0E | companion param for the knob floater button (`CKnobFloaterContainer::SetupState`), 0xFF none |
| 0x0F | momentary (push button; `CPanel::CtrlRelease` @ `000c0794`): Dice, State, Switch, seq Random/Clear |
| 0x10 | MIDI-assignable (`CPanel::CtrlIsMidiAssignable` @ `000bfffa`) |
| 0x11 | text function id, 0xFF = none (then the PANL control's `InfoFunc` is used, `CMBGeneric::ConnectTextToParams` @ `00084d70`) |
| 0x12 | **param class** id (knob sensitivity groups in `CPatch` param change, mutation rules in `CMutaSynthData`). 162 distinct values. Stored as `paramClass`. |
| 0x13, 0x14 | text-function dependencies (param indices, 0xFF none): single/dual/triple `ParamText::Get*DependencyTextFunction` |
| 0x18 | ptr `_k<Name>AffectedParams<i>`: 0xFF-terminated list of params whose text depends on this one |

### Everything else comes from the PANL

`CMBGeneric::GetName` (`Module.Name`), `GetHintText` (`Tooltip`), `GetHeight` (`Height`),
inputs/outputs (`CHoleDataList::PopulateList` on `Input`/`Output`: `CodeRef` = connector
index, `Type` Audio/Control/Logic, `Bandwidth` Static/Dynamic), controls (`CodeRef` = param
index), `PartSelector` (`CodeRef` = mode index, `ImageCount` = number of values) and LEDs.
Modes always start at 0 (`CModule::CModule` resizes the mode vector with 0).

### Flags

* **Locked / Verhue "IsLed"**: `CBuildModule::BuildNewModule` @ `000ab6c2` creates 41 types
  with `CModule::Lock(true)` (`CModule+0x5a`, `IsLocked`). It is the byte after uprate in
  the module-new message (`CMModuleNew::WriteStream` @ `00013056`) and the file, which
  Verhue calls `IsLed`; the two sets are identical. In the editor it excludes the module from
  MutaSynth (`CMutaSynthData`). Stored as `flags.defaultLocked`.
* **Uprate**: `CModule+0x40` (`SetABCodeState`, `NSFile_V11::EModuleBandWidth`), default
  from `CBuildModule::GetBandWidth` which is 0 for every builder. `flags.uprate` = 0;
  `flags.hasDynamicConnectors` tells which modules can follow an uprate.
* Colours: Audio = red, Control = blue, Logic = yellow; Dynamic → blue_red / yellow_orange.

### Replace menu

`CReplaceDataBase::AddData` @ `0010ae74` (decompiler times out; read with
`ModuleDbDisasm.java`) defines 19 groups (Shaper, Level, MIDI Send/Recv, Note, Osc,
Keyboard, Input, Output, Env, Delay, Switch, Mix, Filter, Effect, LFO, RND, Sequencer,
Logic) used by `CModuleReplaceMenu`, plus param/input/output correspondence tables (not
extracted yet). Only registered types occur. Stored as `replaceGroup`.

## 3. Discrepancies vs Verhue's ModuleDef.xml / ParamDef.xml

The editor wins in all cases.

* **PitchTrack (198) outputs**: editor 1 = Pitch (Control, Static), 2 = Gate (Logic, Dynamic);
  Verhue has 1 = Gate, 2 = Pitch.
* **Heights**: EnvMulti (52) 6, not 3; Sw1-4 (88) 3, not 4.
* **Fx-In (127) param 2** default 1, not 0. All other defaults and all ranges (max) agree.
* **Short names**: `Compress` (150, Verhue "Compressor"), `Eq3band` (33, "Eq3Band").
* **Long names** (PANL tooltip): Mix2-1B "Mixer 2-1 B" (Verhue "Scratch"), EnvADDSR "Envelope
  ADBDSR", EnvADR "Envelope AD/R", OscNoise "Noise oscillator", plus case/wording in 12 more
  (`verhueLongName` in modules.json).
* **Param names**: 393 of 903 differ, nearly all Verhue expansions of the 7-char editor name
  (`MstTune`/"Masterfreq", `Band 1`/"BandSel_01", `On/Off`/"On/OFF"); a few are different
  words: Fx-In p0 `Bus` ("Source"), Glide p1 `Active` ("Glide"), RndTrig p0/1 `Prob`/`Prob M`
  ("Step"), RndClkA p4 `Active` ("On/OFF"), EnvAHD p3/p4 `Trigged`/`Release` ("Reset"/"Decay"),
  OscPM p6 `Pitch M` ("FreqMod"), OscString p6 `Damp` ("Moisture"), KeyQuant p8 `Note Bb`
  ("Note A#"). Kept as `verhueName`.
* **PageIndex**: same order; the editor's toolbar has spacer slots (`toolbarSlot`).
* **Missing in Verhue**: the 7 patch-settings modules (6, 95, 135–138, 153).
* **Missing in the editor**: Driver 35, Resonator 56, Red2Blue 201, Blue2Red 203 (see §4).
* Verhue's `ParamDef` ids are his own range taxonomy (`rangeId`, 182 entries →
  `params.json`); they do not correspond 1:1 to the editor's param class (38 classes map to
  several range ids, 21 range ids to several classes).
* Connector names are not in the editor at all (no strings); `modules.json` uses Verhue's.

## 4. Hidden / unreleased modules

The editor has **no** `SModuleInfo`, builder, replace group or type id for any of the 29
unreferenced PANL resources, so none can be selected, created or loaded
(`CreateModuleFromType` throws). The browser is therefore exactly the 166 registered modules.

Of the 22 PANL names missing from Verhue: **Compress** (150) and **Eq3band** (33) are
regular selectable modules (name spelling only). The other 20 exist only as panels:

| PANL | Name | FileName | Ver. | In/Out/Params/Modes |
|-----:|------|----------|-----:|---------------------|
| 416 | ShelvEQ | FilterShelvEQ | 208 | 1/1/5/0 |
| 421, 813 | ClkDivFix | LogicClkDivFix | 208 | 2/3/0/0 |
| 674 | PolarPan | MxrPolarPan | 208 | 3/4/4/0 |
| 678 | PolarFade | MxrPolarFade | 208 | 6/1/4/0 |
| 698 | AR-Env | EnvAR | 206 | 3/2/{1,3,4,5}/0 |
| 732 | PulseOsc | OscPulse | 208 | 5/1/12/1 |
| 733 | SyncOsc | OscSync | 208 | 4/1/11/2 |
| 744, 745 | Mixer6-1A/B | MxrMixer6-1A/B | 206 | 7/1/8/0 |
| 802 | OutBusA | IOOutBusA | 208 | 4/0/0/0 |
| 805 | OutBusB | IOOutBusB | 208 | 2/0/2/0 |
| 826 | SeqA | SeqA | 208 | 2/2/36/0 |
| 887 | AudioIn | AudioIn | 210 | 0/4/0/0 |
| 888 | BusIn | BusIn | 210 | 0/4/0/0 |
| 914 | PeakFollow | PeakDetector | 224 | 1/1/2/0 |
| 918 | EnvDX | EnvDX | 226 | 4/2/9/0 |
| 961 | LfoD | LfoD | 206 | 6/2/13/1 |
| 1009 | RndStep | RndLfoC | 238 | 1/1/6/0 |
| 1011 | RndState | RndLfoLogic | 238 | 1/1/6/0 |
| 1015 | RndChaos | RndClkChaos | 238 | 3/1/5/0 |

Old duplicates, also unused: ClkDiv 811, CompLev 815, LevMult 739 ("Adjustable gain
control", 1 in/1 out/2 params), Delay 831 (FXDelay, "Audio delay").

Verhue's four extra modules have panels (Driver 997, Resonator 995, Red2Blue 1021,
Blue2Red 1023; matched by name, `panelResIdSource`) but no editor tables either; they are
in `modules.json` with `flags.inEditor = false`, data from Verhue. The other 25 are listed
in `unmappedPanels` with `typeId: null`.

Type ids without a builder (0–219): 0, 2, 10, 11, 14, 16, 35, 37, 39, 56, 65, 67, 70, 73,
77, 80, 93, 99, 101, 104, 107, 109–111, 120, 122, 129, 133, 151, 155, 166, 168, 191, 201,
203, 207, 209–219.

## 5. Open questions

* Type ids of the 20 panel-only modules: not in the editor; would need the synth OS/DSP
  image (module code table) or patches that use them.
* Exact meaning of each `ModuleSize` field (which i16 is cycles vs X/Y/P/Q memory); see
  `CPatchLoad::CalculateCriticalResource` @ `000ee0c8` and `Get*Ceil`.
* Names/semantics of the 162 param classes (byte 0x12); the switch in
  `CPatch` param-change handling and `CMutaSynthData` give partial groupings.
* `InfoFunc`/`textFunc` ids → text functions (`ParamText`), see `param-display.md`.
* Replace-menu param/connector correspondence tables in `CReplaceDataBase::AddData`.
* PartSelector `MenuOffset`.
* Where the browser's small toolbar icons come from (not the face; probably `SMTB`).
