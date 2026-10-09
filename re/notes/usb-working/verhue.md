# Nord Modular G2 USB protocol as implemented by Bruno Verhue's editor (GPL-2+)

Source tree: `third_party/nord_g2_editor/` (paths below are relative to it).
Primary reference = **Gen2** (finished product, used on real hardware):
`Gen2/Common/BVE.NMG2Mess.pas` (abbrev. **Mess**), `Gen2/Common/BVE.NMG2USB.pas` (**USB**),
`Gen2/Common/BVE.NMG2Types.pas` (**Types**), `Gen2/Common/BVE.NMG2File.pas` (**File**).
Gen3 (`Gen3/Common/*`, unfinished Android rework) and Gen1 (`Gen1/Source/Common/*`, older finished Windows editor)
are cited where they differ or add information. `Gen2/Common/BVE.NMG2USB - kopie.pas` is an older copy of the
USB unit (**USBold**) – cited only for its disconnect/timeout behaviour.

"Quick reference.txt" (Gen2/Gen3) contains **only editor keyboard shortcuts** – no protocol information.
README.md / Gen3/Readme.txt / Gen1 help html contain no protocol details (only libusb-win32 filter-driver install).
The only raw hex dumps in the repo are in code comments (see §9: all CRC-verified).

Conventions: `$xx` = hex. "slot s" = 0..3 (A..D). "pv" = slot patch version byte, "PV" = performance version byte.
Location byte: `0 = FX`, `1 = VA`, `2 = patch settings` (Types:56-58, `TLocationType = (ltFX=0, ltVA=1, ltPatch=2)` Types:537).
"Clavia string" = up to 16 bytes, followed by `$00` **only if shorter than 16** (Mess:785 `WriteClaviaString`, File:12668/12683).
Bit-packed fields are **MSB-first** (TBitWriter/TBitReader, Types:1539/1592).

---------------------------------------------------------------------------------------------------

## 0. Framing details as Verhue implements them

### 0.1 Outgoing (host → G2, bulk OUT)
* `TG2SendMessage.Create` reserves 2 length bytes (Mess:810-817). `PrepareForSend` (Mess:861-886):
  `len = Size+2` (whole frame incl. the 2 length bytes and 2 CRC bytes), big-endian at [0..1];
  CRC = `CrcClavia` (Types:1086, CRC-16/CCITT poly $1021, init 0, no reflection = XMODEM) over bytes [2 .. end-before-CRC], appended hi,lo.
* **The init message is framed too**: `CreateInitMessage` (Mess:2375) writes only `$80` but goes through
  `PrepareForSend`, so the bytes on the wire are **`00 05 80 91 88`** (Gen3 identical: Gen3 Synth.pas:619 + Classes.pas:1499).
* Byte [3] ("command"): `CMD_REQ $20 | CMD_SYS $0C` = `$2C`; `CMD_REQ | CMD_SLOT $08 | s` = `$28+s`;
  `CMD_NO_RESP $30 | CMD_SLOT | s` = `$38+s` (Types:96-103). `$3C` (no-resp system) is never built. `CMD_CUST $0B` is TCP-only.
* `GetCommand` (Mess:824-841): returns `$80` if byte[2]==$80, else `byte[3] & $0F` **only if** `byte[3]>>4 == 2`, else 0.
  This value is what the editor waits for (`FWaitForCmd`). `GetHasResponse` (Mess:843-859): false iff `byte[3]>>4 == 3`.
* Patch-edit messages carry **several sub-messages concatenated** after one `01 28+s pv` header (Mess:5591-5613, §2.4).

### 0.2 Incoming (G2 → host)
* Interrupt EP read of 16 bytes, infinite timeout (USB:1620 `iread(...,16,TIME_OUT)`, TIME_OUT=0 USB:84).
* `isextended`: `buf[0] & $0F == 1`; `isembedded`: `buf[0] & $0F == 2` (USB:1200-1208). Anything else → logged, receive thread terminates (USB:1676-1680).
* **Extended** (`extended_message`, USB:1111-1177): `size = buf[1]<<8 | buf[2]`; bulk-IN reads into a `size`-byte buffer, up to **5 tries**,
  each `bread(..., size, 500 ms)`; timeout counts as a try. CRC check over bulk bytes `[0 .. size-3]`, CRC at `[size-2]` (hi), `[size-1]` (lo);
  a bad CRC is only logged (message still processed). Bulk payload starts with `$01` (normal) or `$80` (init response).
* **Embedded** (`embedded_message`, USB:1179-1198): whole 16-byte interrupt buffer kept; `dil = buf[0] >> 4` = number of bytes after
  byte 0 **including** the 2 CRC bytes; CRC over `buf[1 .. dil-2]`, CRC at `buf[dil-1]` (hi) `buf[dil]` (lo); mismatch only logged.
  Before parsing, Position:=1 and Size:=dil (USB:1988-1994), i.e. byte 0 is skipped.
  Verified example (Mess:2124): `82 01 04 00 80 00 3F 30 40 …` → dil=8, CRC(01 04 00 80 00 3F) = $3040 ✔.
* Response "command" (`TG2ResponseMessage.GetCommand`, Mess:939-958): `$80` if byte[0]==$80; embedded → byte[2]; extended → byte[1].
  So **the init response is an extended message whose bulk payload starts with `$80`** (that is the only way it can match FWaitForCmd=$80).
* After the `$01` marker every inbound message is `[$01][cmd][version][sub-id][payload…]`:
  * cmd `$0C` = answer to a system request (`$2C`), `$08+s` = answer to a slot request (`$28+s`),
  * cmd `$00..$03` = unsolicited slot-s traffic (LED/VU, param changes from front panel…),
  * cmd `$04` = unsolicited performance/system traffic (MIDI CC, clock, version change). `$04` is also accepted as the answer to a `$0C` wait (USB:1839, 1922).
  * version byte: `$40` = "version-independent" messages (version counters/changes); otherwise must equal current pv / PV
    or the message is rejected with "Patch version differs" (Mess:3670-3677) / "Performance version differs" (Mess:2087-2092).
* A single inbound message may contain **several sub-messages**: sys parser loops `until Position >= Size-2` (Mess:1672, 2085);
  patch parser loops similarly (Mess:6911).

---------------------------------------------------------------------------------------------------

## 1. Outgoing message catalog (every builder)

Column "[4]" = byte at frame index 4 (version/session byte). "Payload" = bytes after the message id (index 5…).
"Expected reply" = what the editor's parsers accept / what Verhue's own TCP server emulation returns
(`CreateResponseMessage`, Mess:1447-1576, used by Gen2/Common/BVE.NMG2TCPIP.pas:1049); "(emu)" marks the emulation-only source,
"(parsed)" marks real-response parsers. Matching is by command nibble only (§5), never by sub-id.

### 1.1 System messages, `[3]=$2C`, `[4]=$41`
| id | Constant | Builder (file:line) | Payload | Expected reply |
|---|---|---|---|---|
| (raw) `$80` | CMD_INIT | `TG2Mess.CreateInitMessage` Mess:2375 | – (frame `00 05 80 91 88`) | extended, bulk[0]=`$80`; **not parsed at all** (`$80: Result:=True`, Mess:1596-1599; emu Mess:1258-1263 "TODO send rest") |
| `$7D` | S_START_STOP_COM | `CreateStartStopCommunicationMessage` Mess:2382 | `[stop]` `$00`=start, `$01`=stop (START_COMM/STOP_COMM Types:235-236) | R_OK `$7F` (emu Mess:1487-1492 `01 0C 00 7F`) |
| `$35` | Q_VERSION_CNT | `TG2Mess.CreateGetPatchVersionMessage` Mess:2398 (perf) / `TG2MessSlot.CreateGetPatchVersionMessage` Mess:3891 (slot) | `[slot]` 0..3, or `$04` = performance. **Always sent as system msg even for slots** | `01 0C 40 36 [slot] [version]` (parsed Mess:1629-1644; emu Mess:1265-1278) |
| `$04` | Q_ASSIGNED_VOICES | `CreateGetAssignedVoicesMessage` Mess:2409 | – | R_ASSIGNED_VOICES `$05` + 4 bytes (parsed Mess:2051→2883) |
| `$02` | Q_SYNTH_SETTINGS | `CreateGetSynthSettingsMessage` Mess:2419 | – | `$03` S_SYNTH_SETTINGS + settings block (parsed Mess:1681-1743; emu Mess:1289-1297) |
| `$03` | S_SYNTH_SETTINGS | `CreateSetSynthSettingsMessage` Mess:2492 / `AddSetSynthSettingsMessage` Mess:2429 | settings block §1.6 | not specifically parsed (R_OK accepted) |
| `$81` | M_UNKNOWN_1 | `CreateUnknown1Message` Mess:2503 | – | sub-id `$80` "Unknown 1", payload ignored (Mess:2072-2075; emu Mess:1299-1307) |
| `$3D` | S_MIDI_DUMP | `CreateMidiDumpMessage` Mess:2513 | – (UI "MIDI dump" button, Gen2/DX4/Frames/UnitSynthSettings.pas:352-356) | not parsed |
| `$14` | Q_LIST_NAMES | `CreateListMessage` Mess:2523 | `[type 0=patch 1=perf] [bank] [location]` | `$16` R_ADD_NAMES list (§2.1), optionally preceded by `$13 xx xx` |
| `$3E` | S_SET_PARAM_MODE | `TG2Mess.CreateSetModeMessage` Mess:2547 | `[mode] [$00]` — comment says `$00 perf, $01 patch` (Mess:2556) but caller says `0:Patch 1:Performance` (USB:2244) – **contradictory** | not parsed; after it Verhue re-inits the performance (USB:2245) |
| `$56` | S_PLAY_NOTE | `CreateNoteMessage` Mess:2559 | `[onoff] [note]` — **onoff `$00`=on, `$01`=off** (Mess:2567) | not parsed (no caller in the UI) |
| `$3B` | Q_MASTER_CLOCK | `CreateGetMasterClockMessage` Mess:2571 | – | `$3F` (S_SET_MASTER_CLOCK) on cmd `$04` path (Mess:2908-2932) |
| `$17` | S_PATCH_BANK_UPLOAD | `CreateUploadBankMessage` Mess:2581 | `[type] [bank] [location]` (synth → disk) | `$18` + `$19` bank data (§2.1, §4.3) |
| `$19` | S_PATCH_BANK_DATA | `CreateDownloadPatchBankMessage` Mess:2595 / `CreateDownloadPerfBankMessage` Mess:2636 | see §4.3 | not specifically parsed |
| `$0A` | S_RETREIVE | `CreateRetrieveMessage` Mess:2680 | `[slot 0..3, 4=perf] [bank] [location]` | ack only; then editor re-inits the slot/perf (§4.2) |
| `$0B` | S_STORE | `CreateStoreMessage` Mess:2699 | `[slot 0..3, 4=perf] [bank] [location]` | `$0D` R_STORE (Mess:1895) |
| `$0E` | S_CLEAR_BANK | `CreateClearBankMessage` Mess:2717 | `[type] [bank] [fromLoc] [bank] [toLoc] [$00 "Unknown"]` | `$12` R_CLEAR_BANK (Mess:1916) |
| `$0C` | S_CLEAR | `CreateClearMessage` Mess:2734 | `[type] [bank] [location] [$00 "Unknown"]` | `$15` R_CLEAR (Mess:1904) |

### 1.2 System messages addressed to the performance, `[3]=$2C`, `[4]=PV` (current perf version)
| id | Constant | Builder | Payload | Expected reply |
|---|---|---|---|---|
| `$10` | Q_PERF_SETTINGS | `CreateGetPerfSettingsMessage` Mess:3127 | – | `$29` C_PERF_NAME + name + chunk `$11` (… possibly more chunks) (parsed Mess:2016-2035; emu Mess:1309-1325) |
| `$59` | M_UNKNOWN_2 | `CreateUnknown2Message` Mess:3138 | – | sub-id `$1E` "unknown_2", payload ignored (Mess:2012-2015; emu Mess:1327-1335). `$1E` = S_SEL_GLOBAL_PAGE id → **probably "get selected global-knob page"** (inference) |
| `$09` | S_SEL_SLOT | `CreateSelectSlotMessage` Mess:3149 | `[slot]` | ack; local apply in ProcessSendMessage Mess:2992-3002 |
| `$3F` | S_SET_MASTER_CLOCK | `CreateSetMasterClockBPMMessage` Mess:3162 | `[$FF "Unknown"] [$01] [bpm]` | ack (no UI caller in Gen2: clock changes are sent as perf-settings chunk instead, USB:2758-2768) |
| `$3F` | S_SET_MASTER_CLOCK | `CreateSetMasterClockRunMessage` Mess:3176 | `[$FF] [$00] [$01 run / $00 stop]` | ack |
| `$11` | C_PERF_SETTINGS (chunk id doubles as msg id) | `CreateSetPerfSettingsMessage` Mess:3236 | the whole chunk `11 len_hi len_lo <data>` (File:12284 WriteSettings, §6) | ack |
| `$29` | C_PERF_NAME | `CreateSetPerfNameMessage` Mess:3255 | Clavia string | ack; local rename Mess:2252-2269 |
| `$5E` | Q_GLOBAL_KNOBS | `CreateGetGlobalKnobsMessage` Mess:3267 | – | chunk `$5F` C_KNOBS_GLOBAL (parsed Mess:2036-2050; emu Mess:1337-1352) |
| `$37` | S_SET_PATCH (performance) | `CreateSetPerformanceMessage` Mess:3193 | **[4]=`$42` hard-coded ("?")** — see §4.1 | ack; local parse Mess:3009-3062 |

### 1.3 Slot request messages, `[3]=$28+s`, `[4]=pv`
| id | Constant | Builder | Payload | Expected reply (cmd `$08+s`) |
|---|---|---|---|---|
| `$3C` | Q_PATCH | `CreateGetPatchMessage` Mess:4045 | – | patch chunks starting with `$21` (§2.2) |
| `$28` | Q_PATCH_NAME | `CreateGetPatchNameMessage` Mess:3937 | – | `$27` + Clavia string (Mess:3516-3525; emu Mess:1370-1378) |
| `$68` | Q_CURRENT_NOTE | `CreateCurrentNoteMessage` Mess:3948 | – | chunk `$69` (Mess:3511; emu Mess:1380-1396) |
| `$6E` | Q_PATCH_TEXT | `CreatePatchNotesMessage` Mess:3902 | – | chunk `$6F` (Mess:3511; emu Mess:1398-1414) |
| `$71` | Q_RESOURCES_USED | `CreateResourceTableMessage` Mess:3923 | `[location]` 1=VA, 0=FX | `$72` (§2.2) |
| `$70` | M_UNKNOWN_6 | `CreateUnknown6Message` Mess:3959 | – | R_OK `$7F` (emu Mess:1564-1567; slot parser accepts R_OK Mess:3646) |
| `$2E` | Q_SELECTED_PARAM | `CreateGetSelectedParameterMessage` Mess:3970 | – | `$2F` S_SEL_PARAM `[?][loc][module][param]` (Mess:3526-3535) |
| `$55` | S_CTRL_SNAPSHOT | `CreateSendControllerSnapshotMessage` Mess:3913 | – ("CC snapshot" button, UnitSynthSettings.pas:268-273) | not parsed |
| `$27` | S_PATCH_NAME | `CreateSetPatchName` Mess:3981 | Clavia string | ack; local rename Mess:3805-3823 |
| `$37` | S_SET_PATCH (slot) | `CreateSetPatchMessage` Mess:3993 | **[4]=`$53` hard-coded ("?")** — see §4.1 | ack; local parse Mess:3723-3759 |
| `$4F` | Q_PARAM_NAMES | `CreateGetParamNamesMessage` Mess:4022 | `[location]` | chunk `$5B` (only used in commented-out init USB:2716-2746) |
| `$4C` | Q_PARAMS | `CreateGetParamsMessage` Mess:4034 | `[location]` | chunk `$4D` (same) |
| `$6A` | S_SEL_VARIATION | `CreateSelectVariationMessage` Mess:4056 | `[variation 0..7]` | ack; local Mess:3776-3789 |
| `$2B` | S_SET_MODE | `TG2MessSlot.CreateSetModeMessage` Mess:4116 | `[loc] [module] [param(mode index)] [value]` (**request, not responseless**) | ack; local Mess:3860-3870 |
| `$44` | S_COPY_VARIATION | `CreateCopyVariationMessage` Mess:4134 | `[from] [to]` ; variation 8 = "init" variation (UnitParam.pas:280-283) | ack; local Mess:3871-3885 |
| `$21`/`$6F`/`$4D`/sub-msgs | patch edits | `CreatePatchMessage` Mess:5591 + Add* | §1.4 | ack (R_OK/R_ERROR in slot parser Mess:3646-3662) |

### 1.4 Patch-edit sub-messages (appended after `01 28+s pv`, any number per frame)
Container: `CreatePatchMessage` Mess:5591-5613 also builds an **undo** frame with the same header whose
sub-messages are inserted in reverse order at offset 5 (`AddReversed`, `Offset:=5`, Mess:888-899, 910-925).
| Sub-id | Builder | Bytes |
|---|---|---|
| `$2A` S_SET_UPRATE | `AddSetUprateMessage` Mess:4252 | `2A [loc] [module] [uprate 0/1]` |
| `$54` S_CABLE_COLOR | `AddSetCableColorMessage` Mess:4269 | `54 [(loc<<3)|color] [fromModule] [(fromKind<<6)|fromConn] [toModule] [(toKind<<6)|toConn]` (parser reads first byte as 4 bits unknown/1 loc/3 color, Mess:6636-6672). kind: 0=input 1=output (Types:535) |
| `$34` S_MOV_MODULE | `AddMoveModuleMessage` Mess:4291 | `34 [loc] [module] [col] [row]` (also emitted by `AddReplaceModulesMessages` Mess:4864 for modules pushed out of the way) |
| `$50` S_ADD_CABLE | `AddConnectionMessage` Mess:4310 | bits: `50`, `0001`(4, "Unknown"=1), loc(1), color(3), fromModule(8), fromKind(2), fromConn(6), toModule(8), toKind(2), toConn(6). "to" must be an input – swapped if it is an output (Mess:4320-4326). Byte form: `50 [$10|(loc<<3)|color] [fromMod] [(fk<<6)|fc] [toMod] [(tk<<6)|tc]` |
| `$51` S_DEL_CABLE | `AddDeleteConnectionMessage` Mess:4380 / `AddDeleteCableMessage` Mess:4409 | bits: `51`, `0000001`(7, "Unknown"=1), loc(1), fromModule, fromKind(2), fromConn(6), toModule, toKind(2), toConn(6) → `51 [$02|loc] …` |
| `$25` S_ASSIGN_KNOB | `AddAssignKnobMessage` Mess:4418 | `25 [module] [param] [loc<<6] [$00] [knob 0..119]` |
| `$26` S_DEASSIGN_KNOB | `AddDeAssignKnobMessage` Mess:4442 | `26 [$00] [knob]` |
| `$2D` S_SEL_PARAM_PAGE | `AddSelectParamPageMessage` Mess:4458 | `2D [page]` (sent after assign with page = knob div 8, Mess:6150) |
| `$22` S_ASSIGN_MIDICC | `AddAssignMidiCCMessage` Mess:4474 | `22 [loc] [module] [param] [cc]` |
| `$23` S_DEASSIGN_MIDICC | `AddDeassignMidiCCMessage` Mess:4493 | `23 [cc]` |
| `$1C` S_ASS_GLOBAL_KNOB | `AddAssignGlobalKnobMessage` Mess:4508 | bits: `1C`, slot(4), loc(2), 0(2), module(8), param(8), $00(8), globalKnob(8) → `1C [(slot<<4)|(loc<<2)] [module] [param] [$00] [knob]`. **Sent inside a slot frame** (`28+s`) |
| `$1D` S_DEASS_GLOB_KNOB | `AddDeassignGlobalKnobMessage` Mess:4533 | `1D [$00] [knob]` |
| `$1E` S_SEL_GLOBAL_PAGE | `AddSelectGlobalParamPageMessage` Mess:4549 | `1E [page]` (parser: "TODO", Mess:6818) |
| `$6F` C_PATCH_NOTES | `AddPatchNotesMessage` Mess:4565 | chunk `6F len_hi len_lo text…` (captured examples §9) |
| `$21` C_PATCH_DESCR | `AddSetPatchDescriptionMessage` Mess:4591 | chunk `21 len…` (patch settings: voices, mono/poly, category… same as .pch2) used for voice count/mode (USB:3388-3428) |
| `$42` S_SET_PARAM_LABEL | `AddSetModuleParamLabelsMessage` Mess:4612 / `AddCopyModuleParamLabelsMessage` Mess:4675 | `42 [loc] [module] [moduleLen] { [isString] [paramLen] [paramIndex] {7-byte label}* }*` — no chunk header (Flush). `paramLen = 1+7·n`, `moduleLen = Σ(3+7·n)` (File:4817-4830, 4939-4952, 5058-5066). Labels must be in param-index order or later re-upload fails (Mess:4642-4643) |
| `$33` S_SET_MODULE_LABEL | `AddSetModuleLabelMessage` Mess:4715 | `33 [loc] [module] [Clavia string]` |
| `$31` S_SET_MODULE_COLOR | `AddSetModuleColorMessage` Mess:4732 | `31 [loc] [module] [color]` |
| `$43` S_SET_MORPH_RANGE | `AddSetMorphMessage` Mess:4749 | `43 [loc] [module] [param] [morph 0..7] [|range|] [negative 0/1] [variation]` (range stored as 256-x when negative, Mess:6884-6889) |
| `$32` S_DEL_MODULE | `AddDeleteModuleMessage` Mess:4770 | `32 [loc] [module]` |
| `$30` S_ADD_MODULE (copy/undo) | `AddCopyModuleMessage` Mess:4786 | `30 [typeID] [loc] [module] [col] [row] [$00=color] [uprate] [isLed] [mode0..modeN-1] [Clavia string name]` |
| `$4D` chunk | `AddCopyModuleParametersMessage` Mess:4812 | chunk `$4D`: loc(2) setCount(8) varCount(8 = **10**) module(8) paramCount(7) then for var 0..8 `[var(8) values(7 each)]`, var 9 = default values (Mess:4846-4851 "don't know what should be in the 10th variation") |
| `$30`+chunks (new module) | `AddNewModuleMessage` Mess:5028 | raw `30 type loc idx col row 00 uprate isLed modes… name` **then** chunks `$52` (empty cable list: loc(2) 0(12) count 0(10)), `$4D` (10 variations of defaults: "VariationCount, must be 10!" Mess:5101), `$5B` (param labels; SeqNote special case Mess:5121-5137), `$5A` (module name: loc(2) 0(6) count=1 index name) |
| `$30`+chunks (copy/paste) | `AddCopyModulesMessage` Mess:5183 | N×raw `$30…` then chunks `$52` (cables: color3 fromMod8 fromConn6 linkType1 toMod8 toConn6), `$4D`, `$5B`, `$5A` with renumbered indices |
| `$43`… + `$4D` | `AddCopyParametersMessage` Mess:5422 | morph sub-msgs then one `$4D` chunk with 1 variation (captured example §9) |

Top-level `Create*` that wrap the above (Mess): CreateAddNewModuleMessage 5720, CreateCopyModulesMessage 5729, CreateCopyParametersMessage 5739,
CreateDeleteModuleMessage 5748 (deletes cables → `$51` + uprate/colour fixes via `CheckUprateChange` 5633, deassigns CC/knob/global knob, zeroes morphs for variations 0..8 "TODO: Maybe N_USB_VARIATIONS?" 5842, then `$32`),
CreateDeleteModulesMessage 5869, CreateAddConnectionMessage 6033, CreateDeleteConnectionMessage 6062, CreateMoveModulesMessage 6075,
CreateAssignKnobMessage 6115, CreateDeassignKnobMessage 6155, CreateModuleAssignKnobs 6171, CreateModuleAssignGlobalKnobs 6222,
CreateAssignMidiCCMessage 6274, CreateDeassignMidiCCMessage 6294, CreateAssignGlobalKnobMessage 6310, CreateDeassignGlobalKnobMessage 6353,
CreateSetPatchDescriptionMessage 6371, CreateSetPatchNotesMessage 6381 (no undo), CreateSetModuleParamLabelsMessage 6390,
CreateSetModuleLabelMessage 6404, CreateSetModuleColorMessage 6416.

### 1.5 Responseless (realtime) messages, `[3]=$38+s`, `[4]=pv at send time`
Built in USB `SendResponslessTable` (USB:1476-1580) from a per-slot de-dup table, not from the Mess builders
(`CreateSetParamMessage` Mess:4070, `CreateSelParamMessage` Mess:4085, `CreateSetMorphMessage` Mess:4099 produce identical bytes but are not used for USB).
| id | Frame (len) | Payload |
|---|---|---|
| `$40` S_SET_PARAM | `00 0D 01 38+s pv 40 …` (13) | `[loc] [module] [param] [value 0..127] [variation]` |
| `$2F` S_SEL_PARAM | `00 0C 01 38+s pv 2F …` (12) | `[$00 "Unknown"] [loc] [module] [param]` |
| `$43` S_SET_MORPH_RANGE | `00 0F 01 38+s pv 43 …` (15) | `[loc] [module] [param] [morph] [|range|] [negative] [variation]` |
No reply. The G2 echoes front-panel changes as unsolicited `$40`/`$2F` (§2.2).

### 1.6 Synth-settings block (`$03`, Mess:2429-2490; parsed identically Mess:1681-1743)
| # | Byte (write side) | Read-side meaning |
|---|---|---|
| – | Clavia string (synth name) | name |
| 0 | `$80` constant | bit7 = "Perf mode? Check!" (Mess:1691), bits6..0 unknown — **writer always sets bit7** |
| 1 | `$00` | unknown |
| 2 | PerfBank | |
| 3 | PerfLocation | |
| 4 | `MemoryProtect<<7` | |
| 5-8 | MidiChannel A,B,C,D (0..15, 16=inactive) | |
| 9 | MidiGlobalChannel (16=off) | |
| 10 | SysExID (16=all) | |
| 11 | `LocalOn<<7` | |
| 12 | `(ProgramChangeReceive<<1)|ProgramChangeSend` | |
| 13 | `(ControllersReceive<<1)|ControllersSend` | |
| 14 | `(SendClock<<6)|(IgnoreExternalClock<<5)` | bit7 unknown |
| 15 | TuneCent (int8 −100..100) | |
| 16 | `GlobalOctaveShiftActive<<7` | |
| 17 | GlobalOctaveShift (int8 −2..2) | |
| 18 | TuneSemi (int8 −6..6) | |
| 19 | `$00` | unknown |
| 20 | `(PedalPolarity<<7)|$40` | bits6..0 unknown (writer forces `$40`) |
| 21 | ControlPedalGain (0..32) | |
| 22-37 | 16 × `$00` | not read |

---------------------------------------------------------------------------------------------------

## 2. Incoming message catalog

### 2.1 System answers (`01 0C …`)  — `TG2Mess.ProcessResponseMessage` Mess:1578-2187
**Version byte `$40`** (Mess:1608-1673), loop of sub-messages:
* `$36 [slot] [ver]` — version counter: slot 0..3 → slot pv, `4` → PV (Mess:1629-1644). Answer to `$35`.
* `$38 [slot] [ver]` — R_PATCH_VERSION_CHANGE, "Follows after retrieving a patch, don't know the meaning of the complete message" (Mess:1645-1662); updates pv/PV only.
* `$1F [PV] {36 [slot] [ver]}×4` — PV then four version triples (Mess:1614-1628; "Performance version?"). Request that triggers it is not identified in the code (presumably perf load/retrieve).

**Version byte == PV** (Mess:1674-2086), loop of sub-messages:
* `$03` synth settings (§1.6).
* `$13 [?=$30] [?=$02]` R_LIST_NAMES — two unknown bytes, otherwise ignored (Mess:1744-1748).
* `$16` R_ADD_NAMES (Mess:1749-1894): `[b: $00 after store / $01 after list] [type 0/1]` then a byte stream:
  `$01 [loc]` set current location; `$03 [bank] [loc]` set bank+location; `$02` end ("????"); `$04` next type (patch→perf→end, bank=loc=0) & end;
  `$05` "last patch in message" end; any other byte = name char: name ends at `$00` **or** after 16 chars, then **1 category byte**; location++ after each name.
  The editor then re-sends `$14` with the cursor (`NextBankListCmd`) until type = 2 (`pftEnd`) (USB:2173-2181, 2196-2203).
  Note: `aBank` is an uninitialised local unless a `$03` arrives first (Mess:1581; same in Gen3 Synth.pas:1478).
* `$0D [slot] [bank] [loc] [?] [?]` R_STORE — "Follows after store (behind list message), but also after clear" (Mess:1895-1903).
* `$15 [type] [bank] [loc]` R_CLEAR (Mess:1904-1915).
* `$12 [type] [fromBank] [fromLoc] [toBank] [toLoc]` R_CLEAR_BANK (Mess:1916-1934).
* `$18 [?=$04] [type] [bank] [loc]` R_PATCH_BANK_UPDLOAD (Mess:1935-1950).
* `$19 [type] [bank] [loc] [name] [size_hi] [size_lo] [ver] [ptype] <size bytes>` bank-dump data; saved as .pch2/.prf2 = text header + `$00` + first `size-1` bytes (Mess:1951-2011; see §4.3).
* `$1E` "unknown_2" (answer to `$59`), payload not consumed (Mess:2012-2015).
* `$29` / `$11`: perf name (≤16, NUL-terminated) **then** chunk(s) read by `TG2FilePerformance.Read` (`$11` settings, possibly `$21…`, `$5F`) (Mess:2016-2035, File:12210-12282).
* `$5F` C_KNOBS_GLOBAL chunk (Mess:2036-2050).
* `$05` / `$5D` → re-dispatched to the performance parser (Mess:2051-2056).
* `$7F` R_OK → `LastResponseMessage:=R_OK` (Mess:2057-2061).
* `$7E [err]` R_ERROR → `ErrorMessage:=True`, `ErrorMessageNo:=err`, logged "G2 returns error xx" (Mess:2062-2071). Error codes are not interpreted anywhere.
* `$80` "Unknown 1" (answer to `$81`), payload not consumed (Mess:2072-2075).
* anything else → "Unknown subcommand", message rejected (Mess:2076-2084).

### 2.2 Slot traffic (`01 08+s …` answers, `01 0s …` unsolicited) — `TG2MessSlot.ProcessResponseMessage` Mess:3439-3681
**Version `$40`**: `$36 [slot] [ver]` → pv (Mess:3456-3460). (Gen3 does **not** consume the slot byte here, Gen3 Slot.pas:767-776 — Gen2/emulation Mess:1272 say the slot byte is present.)
**Version == pv**:
* `$21` C_PATCH_DESCR → full patch: chunk `$21`, then if next byte is `$2D` the 2 bytes `2D 00` are skipped ("extra 2 bytes $2d $00 that comes with patches downloaded with usb (TODO)", Mess:3486-3490), then `Patch.Read` loops chunks until `$6F` or end (File:6766-6790). Chunk set (File:6875-6936 write order, also the .pch2 order): `21, 4A(VA), 4A(FX), 69, 52(VA), 52(FX), 4D(loc2 patch settings), 4D(VA), 4D(FX), 65, 62, 60, 5B(loc2), 5B(VA), 5B(FX), 5A(VA), 5A(FX), 6F` — reader is order-independent; `$2D` is tolerated in File:6863-6867. Over USB the variation count in `4D` is **10** (N_USB_VARIATIONS, Types:50) vs 9 in files.
* `$69`, `$6F`, `$4D`, `$5B` → chunk parsed by `TG2MessPatch.ProcessMessage` (Mess:3511-3515 → 6891-6905).
* `$27 name` S_PATCH_NAME (Mess:3516-3525).
* `$2F [?] [loc] [module] [param]` selected parameter (Mess:3526-3535).
* `$6A [variation]` variation changed (Mess:3536-3545).
* `$3A` R_VOLUME_DATA, `$39` R_LED_DATA — §3.
* `$40 [loc] [module] [param] [value] [variation]` param change from G2; ignored if module==0 (Mess:3599-3611).
* `$72 [loc] <27 bytes>` R_RESOURCES_USED, optionally followed **in the same message** by `72 [loc] <27 bytes>` (Mess:3612-3645). loc 0 → FX table, 1 → VA table. 27-byte table interpretation (Mess:3281-3329): [0..1] cycles red1 (`b1 + b0*128`), [2..3] cycles blue1, [4] internal mem (/128), [7..8] "Resource4" (`b8+b7*128`, /4315), [9..10] Resource5, [11..12] cycles red2, [17..18] cycles blue2, [21..24] RAM 32-bit BE (/260000), others unknown. Load% = `max(100·mem/128, 100·RAM/260000, 100·Res4/4315)`; cycles% = `100·red1/1372 + 100·blue1/5000`.
* `$7F` OK, `$7E [err]` error (Mess:3646-3662).
* else "Unknown subcommand" (Mess:3663-3668).
Patch sub-messages coming from the G2 (e.g. edits on the front panel) are parsed by `TG2MessPatch.ProcessMessage` (Mess:6434-6913, same layouts as §1.4).

### 2.3 Performance traffic (`01 04 …`) — `TG2MessPerformance.ProcessResponseMessage` Mess:2842-2975
**Version `$40`**: `$38 [slot] [ver]` R_PATCH_VERSION_CHANGE → sets version and **re-initialises that slot (or the performance for slot 4)** via DoAfterRetreivePatch → USBStartInit(True) (Mess:2856-2872, USB:2472-2476/2827-2831). Gen2 bug: for slot=4 it indexes `GetSlot(4)` (Mess:2860); Gen3 fixed (Perf.pas:410-417).
**Version == PV**:
* `$05 [v0] [v1] [v2] [v3]` R_ASSIGNED_VOICES, voices per slot A..D (Mess:2883-2895).
* `$80 [?] [cc]` R_MIDI_CC — "82 01 04 00 80 00 3f 30 40 … = CC #63" (Mess:2896-2907). The byte before cc is unknown (`$00` in the sample).
* `$3F [$FF?] [0=run|1=bpm] [value]` master clock state (Mess:2908-2932).
* `$5D [?] [hi] [lo]` R_EXT_MASTER_CLOCK, external clock BPM = hi·256+lo (Mess:2933-2945).
* `$29`/`$11` perf name + settings chunk, "Strange, sometimes received after store : because patch bank location is in slot settings" (Mess:2946-2966).

### 2.4 Chunk ids seen in dumps (Types:95-205)
`$11` perf settings, `$21` patch description, `$29` perf name (raw name, not a chunk), `$4A` module list, `$4D` parameter list, `$52` cable list,
`$5A` module names, `$5B` parameter names/labels, `$5F` global knobs, `$60` controllers (MIDI CC), `$62` knobs, `$65` morph params,
`$69` current note, `$6F` patch notes. Performance payload order (Mess:3193-3233 / File:12284-12314):
`[name] [$1A] [$29] [name] 11 (21 4A 4A 69 52 52 4D 4D 4D 65 62 60 5B 5B 5B 5A 5A 6F)×4 5F`.

---------------------------------------------------------------------------------------------------

## 3. LED ($39) and VU/strip ($3A) data

Both arrive as **extended** messages `01 [s 0..3] [pv] [39|3A] …` (`IsLedData`: byte[1] ∈ {0..3} and byte[3] ∈ {$39,$3A}, Mess:960-967).
They bypass the request/response matching completely (USB:1828-1832, 1911-1915) and are rejected if pv differs.
Rate: not controlled/documented by Verhue; they only flow after `S_START_STOP_COM $00`. (USBold had a 500 ms *TCP* re-broadcast timer, USBold:458 — not the G2 rate.)

### 3.1 Which LEDs exist and their order
Per patch, built when modules are created (`CreateLeds`, File:7128-7179) from `LedDefs` (174 entries) and `MiniVUDefs` (29 entries) in Gen2/Common/BVE.NMG2Data.pas:6017 / 6271:
* **LedList** (`$39`): every LED group that contains exactly **one** LED.
* **LedStripList** (`$3A`): every LED group with >1 LED (one entry per group: sequencer/step/position strips) **plus** every MiniVU.
* Both lists are sorted by `FLedComparison` (File:6258-6287): **location descending (VA=1 first, then FX=0)**, then module index ascending, then GroupID ascending.
  Gen1 (finished) agrees: `CompareLedGreenOrder`/`CompareMiniVUOrder` Gen1/Source/Common/g2_graph.pas:2085-2160, lists built 2555-2580 ("These leds are addressed in message $39 / VU-meters and ledgroups with more than 1? led are addressed in message $3A").
  **Gen3 differs**: iterates FX (part 0) first then VA (part 1) (Gen3 Patch.pas:2454-2509). Treat ordering as to-be-verified on hardware; Gen1+Gen2 = VA first.

### 3.2 `$39` R_LED_DATA (Mess:3569-3598)
`39 [unknown byte] [packed…]` — Gen1 reads the unknown byte into a variable called `aLocation` but never uses it (Gen1 g2_mess.pas:3167-3169).
Then 2 bits per LedList entry, 4 per byte, **first LED in bits 1..0**, next in bits 3..2, etc.
**Bug in all three generations**: value is computed as `(b and mask) shr j` with `mask = 3 shl (2*j)` (Mess:3575-3592), i.e. shifted by j instead of 2j, so LED k>0 in a byte yields 0/2/4/6… instead of 0..3. Correct decode: `(b >> (2*j)) & 3`. Gen3 also continues the bit cursor across the FX→VA boundary (no realignment).

### 3.3 `$3A` R_VOLUME_DATA (Mess:3546-3568)
For each LedStripList entry: `[unknown byte] [value byte]` (2 bytes per entry, value passed to `Led.SetValue`). For MiniVUs value = level, for strips = active LED/step (inference from usage). The first byte might be a high byte – unknown.

### 3.4 Modules with LEDs (extracted from Data.pas; "single"=GroupIDs in `$39`, strips = GroupID:count:type in `$3A`, VU = GroupIDs in `$3A`)
| TypeID | Module | `$39` singles | `$3A` strips | `$3A` MiniVUs |
|---|---|---|---|---|
| 3 | 4-Out | - | - | 0,1,2,3 |
| 4 | 2-Out | - | - | 0,1 |
| 5 | Invert | 0,1 | - | - |
| 17 | ValSw1-2 | 0 | - | - |
| 19 | Mix4-1B | - | - | 0 |
| 20 | EnvADSR | 0 | - | - |
| 21 | Mux1-8 | - | 0:8 | - |
| 23 | ModADSR | 0 | - | - |
| 24 | LfoC | 0 | - | - |
| 25 | LfoShpA | 0 | - | - |
| 26 | LfoA | 0 | - | - |
| 32 | Eq2Band | - | - | 0 |
| 33 | Eq3band | - | - | 0 |
| 38 | Pulse | 0 | - | - |
| 40 | Mix8-1B | - | - | 0 |
| 41 | EnvH | 0 | - | - |
| 42 | Delay | 0 | - | - |
| 45 | FltVoice | - | - | 0 |
| 46 | EnvAHD | 0 | - | - |
| 48 | MixStereo | - | - | 0,1 |
| 52 | EnvMulti | 0 | - | - |
| 55 | EnvD | 0 | - | - |
| 58 | DrumSynth | 0 | - | - |
| 60 | Mux8-1X | - | 0:8 | - |
| 64 | Gate | 0,1 | - | - |
| 84 | EnvADR | 0 | - | - |
| 85 | WindSw | 0 | - | - |
| 86 | 8Counter | - | 0:8 | - |
| 91 | FlipFlop | - | 0:2 | - |
| 102 | FltPhase | - | - | 0 |
| 103 | EqPeak | - | - | 0 |
| 105 | ValSw2-1 | 0 | - | - |
| 116 | Mix8-1A | - | - | 0 |
| 119 | EnvADDSR | 0 | - | - |
| 121 | SeqNote | - | 0:17 | - |
| 123 | Mix4-1C | - | - | 0 |
| 124 | Mux8-1 | - | 0:8 | - |
| 127 | Fx-In | - | - | 0,1 |
| 130 | BinCounter | - | 0:8 | - |
| 131 | ADConv | - | 0:8 | - |
| 140 | Mix4-1S | - | - | 0,1 |
| 144 | SeqEvent | - | 0:17 | - |
| 145 | SeqVal | - | 0:17 | - |
| 146 | SeqLev | - | 0:17 | - |
| 150 | Compress | - | 0:10 (ltSequencer) | - |
| 154 | SeqCtr | - | 0:17 (ltSequencer) | - |
| 156 | NoteDet | 0 | - | - |
| 161 | MixFader | - | - | 0 |
| 162 | FltComb | - | - | 0 |
| 169 | ModAHD | 0 | - | - |
| 170 | 2-In | - | - | 0,1 |
| 171 | 4-In | - | - | 0,1,2,3 |
| 180 | Operator | 0 | - | - |
| 189 | NoiseGate | 0 | - | - |
| 190 | LfoB | 0 | - | - |
| 198 | PitchTrack | 0 | - | - |
| 200 | RandomA | 0 | - | - |
| 202 | RandomB | 0 | - | - |
(Within a module the list is GroupID-sorted, so order is by GroupID not by def order.)

---------------------------------------------------------------------------------------------------

## 4. Connection / initialisation sequence (Gen2)

### 4.1 USB open (`SetUSBActive(True)`, USB:1230-1286; libusb unix `LibUSBInit` USB:818-859)
1. If a handle is open, `LibUSBDone` first. `libusb_open`; if kernel driver active on iface 0 → detach; endpoints hard-coded `$81` intr-in, `$82` bulk-in, `$03` bulk-out; `libusb_claim_interface(0)`.
   (Windows: `usb_set_configuration(config[0])`, claim iface 0, endpoints taken from descriptor order, USB:766-816.) No control transfers, no alt-setting.
2. Start receive thread (and send thread, `{$DEFINE SENDTHREAD}` USB:47), **Sleep(1000)** (USB:1272), then `USBStartInit` (USB:2150).

### 4.2 Message sequence (each step is sent only after the previous request's answer was matched — §5)
`TG2USB.USBInitSeq` (USB:2160-2194), `TG2USBPerformance.USBInitSeq` (USB:2523-2598), `TG2USBSlot.USBInitSeq` (USB:2877-2951):
| # | Message | Builder | Wire (with pv/PV = 0) |
|---|---|---|---|
| 1 | Init | Mess:2375 | `00 05 80 91 88` |
| 2 | Stop comm | Mess:2382 | `00 09 01 2C 41 7D 01 96 94` |
| 3 | Get perf version `$35 04` → sets PV | Mess:2398 | `00 09 01 2C 41 35 04 42 54` |
| 4 | Get synth settings `$02` | Mess:2419 | `00 08 01 2C 41 02 9B AC` |
| 5 | Unknown1 `$81` | Mess:2503 | `00 08 01 2C 41 81 3A 47` |
| 6 | Perf init: Get perf settings `$10` (PV) | Mess:3127 | `00 08 01 2C PV 10 …` |
| 7 | Unknown2 `$59` (PV) | Mess:3138 | |
| 8-… | **for slot A, B, C, D in turn**: (a) `$35 [s]` sys/`$41` → pv; (b) `$3C` Q_PATCH; (c) `$28` name; (d) `$68` current note; (e) `$6E` patch text; (f) `$71 01` resources VA; (g) `$71 00` resources FX; (h) optional auto MIDI-CC assignment messages (USB:2953-3086, only if `AutoAssignMidi`); (i) `$70` unknown6; (j) `$2E` selected param | Mess:3891,4045,3937,3948,3902,3923,3923,3959,3970 | |
| | Get assigned voices `$04` | Mess:2409 | `00 08 01 2C 41 04 FB 6A` |
| | Get global knobs `$5E` (PV) | Mess:3267 | |
| | optional auto MIDI-CC assignments for global knobs (USB:2600-2684) | | |
| | Get master clock `$3B` | Mess:2571 | `00 08 01 2C 41 3B 3C D6` |
| | List names: `$14 00 00 00`, then repeated with the cursor returned in `$16` until type = end | Mess:2523 | `00 0B 01 2C 41 14 00 00 00 EC E5` |
| last | Start comm `$7D 00` → `FInitialized:=True`, `DoAfterG2Init` | Mess:2382 | `00 09 01 2C 41 7D 00 86 B5` |
`$4C`/`$4F` (Q_PARAMS/Q_PARAM_NAMES) and "SetPerformance 'Empty perf'" appear only in a commented-out alternative init (USB:2716-2746).
Gen3 performs exactly the same order (Gen3 Synth.pas:2156-2210, Perf.pas:856-945, Slot.pas:840-927).

Re-init triggers (all end with Start comm when `aStartCommAfterInit=True`, USB:2584-2596 / 2939-2950):
* Slot re-init (`TG2USBSlot.USBStartInit(True)`, USB:2869): after S_RETREIVE response for slot 0..3, after `R_PATCH_VERSION_CHANGE` on cmd `$04`.
* Perf re-init (`TG2USBPerformance.USBStartInit(True)`, USB:2507): after S_RETREIVE for slot 4, after `$38` for slot 4, after `SetPerfMode` (`$3E`, USB:2240-2246).
  Note: re-inits do **not** send Stop comm first.

### 4.3 Version counter tracking
* Initial pv = PV = 0 (Mess:2753-2757, 3331-3337).
* Updated only from: `01 0C 40 36 s v` (Mess:1629-1644), `01 0C 40 38 s v` (1645-1662), `01 0C 40 1F PV {36 s v}×4` (1614-1628),
  `01 08+s 40 36 s v` (3456-3460), `01 04 40 38 s v` (2856-2872, + re-init).
* Patch edits, parameter changes, set-patch etc. **never change pv locally**.
* Consumers: byte [4] of every slot request/edit (`$28+s`), every responseless message (taken at the moment the table is flushed, USB:1501/1516/1530),
  perf requests (`$10 $59 $09 $3F $11 $29 $5E`); fixed `$41` for the rest of the system set; fixed `$53`/`$42` for S_SET_PATCH slot/perf.
* Undo frames capture pv at creation time (Mess:5608) → can be stale when replayed (USB:3344-3354).

---------------------------------------------------------------------------------------------------

## 5. Patch / performance transfer, banks, parameters, MIDI

### 5.1 Upload a patch/performance into the edit buffer (S_SET_PATCH $37)
Slot (Mess:3993-4020): `01 28+s 53 37 00 00 00` + Clavia-style name (`Chunk.WriteName`, Types:1816, then Flush) + patch chunks (order §2.2) written with **10 variations** ("10 Variations must be in a patch for USB, unclear what #10 holds"). No CRC inside.
Perf (Mess:3193-3233): `01 2C 42 37 00 00 00` + name + `1A 29` + name + chunk `$11` + 4× patch chunk sets (10 variations each) + chunk `$5F`. (Gen3 forces byte[4]=$42 explicitly, Gen3 Synth.pas:952.)
The three `$00` bytes after `$37` and the `$1A` byte are unexplained. After the ack the editor parses the sent patch into its model (Mess:3723-3759 / 3009-3062).
Version change is expected to arrive afterwards as `$38`/`$36` (Types comment: "R_PATCH_VERSION_CHANGE = $38; // After upload patch through midi").

### 5.2 Retrieve / store / clear (bank memory ↔ slot)
* Retrieve `$0A [slot|4] [bank] [loc]` → after ack: full re-init of slot/perf (§4.2; Mess:2314-2327, USB:2431-2437).
* Store `$0B [slot|4] [bank] [loc]` → `$0D` R_STORE (+ list `$16` with `b=$00` "after store", and sometimes `$29/$11`).
* Clear `$0C [type] [bank] [loc] [$00]` → `$15`; Clear bank `$0E [type] [bank] [from] [bank] [to] [$00]` → `$12` (bank list entries deleted locally).

### 5.3 Bank dump transfer (`S_PATCH_BANK_UPLOAD $17` / `S_PATCH_BANK_DATA $19`)
* Synth → disk: `$17 [type] [bank] [loc]` (Mess:2581); the editor iterates over its BankList (`NextBankUploadMessage`, USB:2205-2218) sending one `$17` per used location.
  Reply carries `$18 [$04?] [type] [bank] [loc]` and `$19 …` data (Mess:1935-2011).
* Disk → synth: `$19 [type] [bank] [loc] [Clavia name] [size_hi] [size_lo] [$17=PATCH_VERSION 23] [$00 "Always 0?"] [ver] [type] <chunks> [crc_hi] [crc_lo]` (Mess:2595-2634),
  where `[ver][type]<chunks>[crc]` is exactly the binary part of a .pch2/.prf2 file (CRC = CrcClavia over ver,type,chunks — File:6971-6981, 7019-7029), **9 variations**, and
  `size = len([ver][type]<chunks>[crc]) + 1` ("// ?"). For performances the outer type byte is still `$00` (`ord(pftPatch)`, Mess:2668-2669, pftPerf commented out) while the inner one is `$01`; inner content = `$11` settings + 4 patches + `$5F` (Mess:2657-2661).
  Iterated over a `.pchList` file (`NextPatchBankDownloadMessage` USB:2307-2331, `NextPerfBankDownloadMessage` USB:2360-2383), destination location = list index−1.
  Gen3 difference: inner version byte = current slot pv instead of the patch file's version (Gen3 Synth.pas:743).
* Upload parsing mirrors this framing (size then ver/type then `size` bytes, file keeps `size-1`).

### 5.4 Parameters, morphs, variations, knobs, CC
* Value change → `TG2USBPatch.SetParamValue` (USB:3373) → `AddParamUpdRec(S_SET_PARAM …)` (USB:3253-3294) → flushed by the send thread as `$38+s pv 40 loc mod par val var`.
* Morph range → `SetMorphValue` (USB:3362-3371): negative ranges sent as `|256−v|` with negative flag 1.
* Select param → `SelectParam` (USB:3380) → `$2F 00 loc mod par` (responseless).
* Mode params (waveform etc.) → `SetModeValue` (USB:3356) → request `$2B loc mod idx val`.
* Variation select `$6A [v]`; copy `$44 [from] [to]`; init variation = copy from 8 (UnitParam.pas:280-283).
* Knobs/global knobs/MIDI CC assignment: patch-edit sub-messages §1.4 (`$25/$26/$2D`, `$1C/$1D/$1E`, `$22/$23`).
  Auto-assign: re-assigning a param first sends deassign of its previous knob and of the knob's previous param (Mess:6115-6153).
* Patch settings (voices / mono-poly-legato) → `$21` chunk sub-message (USB:3388-3436).

### 5.5 MIDI related
* `S_PLAY_NOTE $56 [00=on|01=off] [note]` (system, request).
* `S_MIDI_DUMP $3D` (system, request, no payload).
* `S_CTRL_SNAPSHOT $55` (slot request, no payload).
* `M_UNKNOWN_1 $81` (system, sent once during init) → answer sub-id `$80`; `R_MIDI_CC $80 [?] [cc]` arrives unsolicited on cmd `$04`.
  The editor only uses received CCs for "MIDI learn"-like UI events (`DoMidiCCReceive`, Mess:2796). Whether `$81` enables CC reporting is **not established** in the code — only that its reply id is `$80`.
* `R_EXT_MASTER_CLOCK $5D` gives external BPM; `$3F` reports internal clock run/BPM.

---------------------------------------------------------------------------------------------------

## 6. Threads, timing, sync, errors, disconnect

* **Send path (Gen2, SENDTHREAD on)**: `SendCmdMessage` (USB:2061-2094) prepares the frame and enqueues it. Send thread (USB:1339-1411):
  loop { if `FWaitForCmd = 0`: flush responseless table (`SendResponslessTable`), then if queue non-empty send **one** queued message
  (`SendMessFromQueue` USB:1423-1462: `FWaitForCmd := Mess.Command`, single `bwrite` of the whole frame, timeout 0=infinite, Android 500 ms);
  Sleep(1) (Android 50) } else Sleep(10) (Android 50). Failed write → `FWaitForCmd:=0`.
  ⇒ strictly **one outstanding request**; responseless param messages are only sent when nothing is outstanding.
* Responseless table (USB:1476-1580, 3253-3317): per slot array keyed by (subcmd, loc, module, param, morph, variation); stores latest value only
  (coalescing); new entries start with value 128 = "uninitialised"; entries with value ≥128 are not sent; `Changed` cleared after flush.
  Side effect: re-selecting the same parameter (`$2F`, value always 0) is never re-sent. Android: extra Sleep(100) after each param frame (USB:1564-1566).
* **Receive path**: thread blocks on interrupt read (infinite), assembles the message, queues it and `Queue()`s processing on the main thread (USB:1594-1726).
* **Matching** (`USBProcessMessageQueued` USB:1886-1971): LED/VU → processed immediately. Else if waiting: accept if `Cmd == FWaitForCmd` or (`Cmd==$04` and wait==`$0C`)
  → process response, then — **only if no R_ERROR** — apply the *sent* message to the local model (`USBProcessSendMessage` → `ProcessSendMessage`, Mess:2189-2373 / 3683-3889),
  clear `FWaitForCmd`, and fire the next init/bank step (slot 0..3 → perf → synth callbacks). Non-matching message while waiting →
  logged "Message received out of sync (1)" and **dropped** (even unsolicited param changes). If not waiting → processed as unsolicited ("Resource messages are received here").
  The `ErrorMessage` term of the match condition is always false at that point (reset just before, USB:1909).
* **No response timeout, no retry, no resync**: if an answer never matches, `FWaitForCmd` stays set and all further sends stall (only Android bulk write has a 500 ms timeout).
  Only retry logic is the 5-try bulk read with 500 ms per try for extended payloads (USB:1126-1152). Bad CRCs are logged, not rejected.
* Offline mode: messages are applied locally without sending (USB:2087-2093).
* **Disconnect** (`SetUSBActive(False)`, USB:1288-1320): terminate+free send thread, terminate receive thread, `LibUSBDone` (unix USB:919-939:
  `libusb_reset_device` "Needed to break out the infinite interrupt wait", release iface 0, close, unref; Windows `usb_reset` USB:899-917), `FInitialized:=False`.
  **Gen2 sends no Stop-comm on disconnect.** The older USBold did: if initialised send `$7D 01` and poll up to **3000 ms** (100 ms steps) for R_OK (USBold:1519-1533),
  then drained the interrupt pipe with 1000 ms reads (USBold:1569-1578), and split bulk writes into 4096-byte packets with 5 timeout retries (USBold:1826-1847).
* **Gen3 differences** (Gen3/Common/BVE.NMG2USB.pas): requests are written immediately (no waiting) and kept in a FIFO; each non-LED answer is matched against the FIFO head with the same rule (776-862, 893-921);
  param thread is event-driven, dictionary-coalesced per (slot, subcmd, loc, module, param, morph, variation), stores pv at enqueue time, no `<128` filter, Sleep(1)/Android 50 after each flush (175-372);
  all bulk/interrupt timeouts infinite (Gen3 USBUnix.pas:43, 213-243).

---------------------------------------------------------------------------------------------------

## 7. Unknowns / uncertainties flagged by Verhue (and found while reading)
* `R_STORE = $0D; // ?` (Types:117); `$1F` "Performance version?" (Mess:1616); `$38` "don't know the meaning of the complete message" (Mess:1647-1648).
* `$13` R_LIST_NAMES bytes "$30 unknown", "$02 Unknown" (Mess:1746-1747); `$16` first byte "$00 After store / $01 After list"; list code `$02` "????" (Mess:1772).
* `$0D` two trailing "Unknown" bytes (Mess:1900-1901); `$18` first byte "Unknown $04" (Mess:1937).
* `$19` "PatchType, always 0?" / "Always 0?" / size `+1 // ?` (Mess:1974, 2622-2626, 2665-2670); "Check: Crc??" (Mess:1990); "Seems clavia software starts first file entry with 2????" (Mess:2004).
* S_SET_PATCH version bytes `$53 // ?` (Mess:4002), `$42 // ?` (Mess:3203); 10th USB variation "unclear what #10 holds" (Mess:4016) / "Don't know what should be in the 10th variation, maybe default value" (Mess:4846).
* Synth settings: "Perf mode? Check!" (Mess:1691) and several unknown bit fields; writer constants `$80`, `$00`, `$40`, 16×`$00`.
* `$7E` "error no?" (Mess:2064, 3652) — codes never interpreted.
* `$2F` first byte "Unknown" (S_SEL_PARAM, Mess:4093, 3528); `$43`/`$3F` `$FF` "Unknown" (Mess:3171); `$50` 4-bit "Unknown" = 1, `$51` 7-bit "Unknown" = 1 (Mess:4333, 4394); S_ASSIGN_KNOB 6+8 unknown bits (Mess:4432-4433).
* `$0E`/`$0C` trailing `$00 // Unknown` (Mess:2730, 2747); `$59` M_UNKNOWN_2, `$70` M_UNKNOWN_6, `$81` M_UNKNOWN_1 names themselves; `$1E` reply "unknown_2"; `$80` reply "Unknown 1".
* "Read the extra 2 bytes $2d $00 that comes with patches downloaded with usb (TODO)" (Mess:3488).
* `$39` first byte and `$3A` per-entry first byte "Unknown" (Mess:3552, 3571).
* R_MIDI_CC byte before CC unknown; `$5D` first byte unknown.
* `$3E` mode value polarity contradictory (Mess:2556 vs USB:2244).
* Perf settings chunk fields FUnknown2..7 (File:12230-12242); slot settings FUnknown2..4, "First G2 gives 1..4, second G2 gives 5..8..?" (File:11779).
* Resource table: most of the 27 bytes unknown; patch-load formula "not completely accurate yet (LFO's)" (Gen1 UnitG2Editor.pas changelog).
* Morph deletion "TODO: Maybe N_USB_VARIATIONS?" (Mess:5842, 5991).

Bugs/discrepancies to be aware of when using Verhue as an oracle: LED 2-bit shift bug (§3.2); FX/VA LED order Gen3 vs Gen1/Gen2 (§3.1);
Gen2 `GetSlot(4)` on `$38` slot 4 (Mess:2860); Gen3 slot-level `$40/$36` without slot byte; uninitialised `aBank` in list parsing; `ErrorMessage` never true in the match condition;
CreateUploadBank parser allocates `size-1` but reads `size` bytes (Mess:1993-1998).

---------------------------------------------------------------------------------------------------

## 8. Performance-settings / global-knob chunk payloads (for completeness)
* `$11` (File:12284-12302): `unk2(8) unk3(4) selectedSlot(2) unk4(2) keyboardRangeEnabled(8) masterClockBPM(8) unk5(8) masterClockRun(8) unk6(8) unk7(8)`
  then 4× slot: `Clavia name, enabled(8) keyboard(8) hold(8) bank(8) patch(8) kbRangeFrom(8) kbRangeTo(8) unk2(8) unk3(8) unk4(8)` (File:11785-11799).
* `$5F` (File:12000-12007, 11883-11894): `count(16)` then per knob `assigned(1)` and if 1: `loc(2) module(8) isLed(2) param(7) slot(2)`.

---------------------------------------------------------------------------------------------------

## 9. Golden vectors

### 9.1 Hex dumps present in the repo (CRC-16/XMODEM verified by me; all slot A, pv 0)
From Gen1/Source/Windows/UnitG2Editor.pas:97-98, 167-176 (Verhue's notes, apparently captured from the official editor):
| Meaning | Bytes |
|---|---|
| Copy variation 1 → init (var 0 → 8) | `00 0A 01 28 00 44 00 08 0F 5C` |
| Init variation 2 (copy 8 → 1) | `00 0A 01 28 00 44 08 01 17 DC` |
| Patch notes "h" | `00 0B 01 28 00 6F 00 01 68 D3 88` |
| Patch notes "ha" | `00 0C 01 28 00 6F 00 02 68 61 56 C9` |
| Patch notes "hal" | `00 0D 01 28 00 6F 00 03 68 61 6C 28 AD` |
| Param paste Osc B→Osc B (chunk `$4D`, len 15: loc=1(VA) sets=1 vars=1 module=1 params=11 var=1 values 89,64,1,0,0,0,0,0,2,1,0) | `00 19 01 28 00 4D 00 0F 40 40 40 45 80 D9 80 04 00 00 00 00 02 02 00 53 D3` |
| Param paste incl. morphs: 3× `$43` (VA, mod 3, param 0/6/8, morph 0, range $2B/$1E/$04, pos, var 0) + `$4D` chunk (module 3) | `00 31 01 28 00 43 01 03 00 00 2B 00 00 43 01 03 06 00 1E 00 00 43 01 03 08 00 04 00 00 4D 00 0F 40 40 40 C5 80 55 4A 04 00 00 00 00 02 02 00 91 00` |
| Inbound embedded MIDI CC #63 (Mess:2124) | `82 01 04 00 80 00 3F 30 40 00 00 00 00 00 00 00` |

### 9.2 Derived by applying Verhue's builders + CrcClavia (not hardware captures)
| Message | Bytes |
|---|---|
| Init | `00 05 80 91 88` |
| Stop comm | `00 09 01 2C 41 7D 01 96 94` |
| Start comm | `00 09 01 2C 41 7D 00 86 B5` |
| Get perf version ($35 slot 4) | `00 09 01 2C 41 35 04 42 54` |
| Get slot A version ($35 slot 0) | `00 09 01 2C 41 35 00 02 D0` |
| Get synth settings | `00 08 01 2C 41 02 9B AC` |
| Unknown1 ($81) | `00 08 01 2C 41 81 3A 47` |
| Get perf settings, PV=0 | `00 08 01 2C 00 10 97 22` |
| Unknown2 ($59), PV=0 | `00 08 01 2C 00 59 4E CF` |
| Get patch slot A, pv=0 | `00 08 01 28 00 3C AE 0C` |
| Get patch name slot A | `00 08 01 28 00 28 FC B9` |
| Get current note slot A | `00 08 01 28 00 68 B4 7D` |
| Get patch text slot A | `00 08 01 28 00 6E D4 BB` |
| Get resources VA slot A | `00 09 01 28 00 71 01 33 95` |
| Get resources FX slot A | `00 09 01 28 00 71 00 23 B4` |
| Unknown6 ($70) slot A | `00 08 01 28 00 70 27 44` |
| Get selected param slot A | `00 08 01 28 00 2E 9C 7F` |
| Get assigned voices | `00 08 01 2C 41 04 FB 6A` |
| Get global knobs, PV=0 | `00 08 01 2C 00 5E 3E 28` |
| Get master clock | `00 08 01 2C 41 3B 3C D6` |
| List names patch bank0 loc0 | `00 0B 01 2C 41 14 00 00 00 EC E5` |
| Select slot B, PV=0 | `00 09 01 2C 00 09 01 78 94` |
| Select variation index 1, slot A | `00 09 01 28 00 6A 01 EC 1C` |
| Retrieve slot A bank 0 loc 0 | `00 0B 01 2C 41 0A 00 00 00 55 18` |
| Play note 60 on | `00 0A 01 2C 41 56 00 3C C5 A6` |
| Set param (no resp) slot A VA mod1 par0 val 64 var0 | `00 0D 01 38 00 40 01 01 00 40 00 08 1C` |
| Sel param (no resp) slot A VA mod1 par0 | `00 0C 01 38 00 2F 00 01 01 00 E4 BF` |
| Morph (no resp) slot A VA mod1 par0 morph0 +$2B var0 | `00 0F 01 38 00 43 01 01 00 00 2B 00 00 8A CA` |
