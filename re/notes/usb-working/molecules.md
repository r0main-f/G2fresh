# Nord Modular G2 editor v1.62 (Mac): protocol molecules

Source: Ghidra decomp of `G2Editor_i386` (`re/out/decomp/*.c`, preprocessed copies in `scratchpad/cm/`).
Where the decomp lost tail-call arguments ("Could not recover jumptable"), I read the arguments with `objdump -d` on
`original/mac/G2Editor_i386`. Addresses are the `// 000xxxxx Class::Method` headers. Every function below
0x1f0000 is real code. Functions at 0x200000 and above (".eh clones" and other garbage) are ignored.

Each molecule section gives **[W]** (WriteStream, editor→synth) and **[R]** (ReadStream, synth→editor) in wire order.

---------------------------------------------------------------------------------------------------------

## 0. Bit-stream primitives (CBitStream, 0x146c62–0x14792a), verified against the binary vtable at 0x2b3028

The vtable layout below was checked against the binary. CMIDIOutStream (vtbl 0x293d00) and CMIDIInStream
(vtbl 0x293da0) override **nothing** except their destructors, so on USB the streams behave exactly like a plain CBitStream.

| vt off | method | exact semantics |
|---|---|---|
| +08 | `GetBit` 0x1470e4 | 1 bit, MSB-first in the current byte |
| +0c | `GetUBits(n)` 0x147622 | n ≤ 8 bits, MSB-first, may straddle 2 bytes, no alignment |
| +10 | `GetSBits(n)` 0x1476ae | same, sign-extended |
| +14 | `SkipBits(n)` 0x146eb6 | = `GetUBits(n)` with the result discarded |
| +18 | `UnreadBits(n)` 0x14792a | moves back n bits |
| +1c | `GetUByte` 0x14713a | **first `MoveToByteBoundary()`**, then reads 1 byte |
| +20 | `GetSByte` 0x1471c6 | aligned signed byte |
| +24 | `Get14BitWord` 0x146e40 | `GetUByte()<<7 \| GetUByte()`, big-endian 7-bit pair |
| +28 | `GetUWordMSBZeros` 0x147268 | aligned; `(b0&0x7f) \| (b1&0x7f)<<7`, **little-endian** 7-bit pair |
| +2c | `GetString(max)` 0x1472c8 | aligned; reads chars until NUL or `max`. A NUL is consumed when present (advance len+1). Otherwise it advances `max` |
| +30 | `GetStringFixLength(n)` 0x14739e | aligned; always consumes n bytes (NUL-padded) |
| +34 | `GetByteFlag` 0x147216 | aligned byte != 0 |
| +38 | `PutBit(b)` 0x14746e | 1 bit |
| +3c | `PutUBits(nbits, value)` 0x14773c | **argument order is (nbits, value)**, n ≤ 8, MSB-first. The value is NOT masked, so extra high bits would corrupt the preceding bits |
| +40 | `PutSBits(nbits, value)` 0x1477ea | masked |
| +44 | `ZeroPad(n)` 0x147050 | = `PutUBits(n, 0)` |
| +48 | `FillUBytes(v, count)` 0x146fd4 | count × `PutUByte(v)` |
| +4c | `PutUByte(v)` 0x1474d6 | **first `ZeroPadToByteBoundary()`**, then writes 1 byte |
| +50 | `Put14BitWord(v)` 0x146ed2 | `PutUByte((v>>7)&0x7f); PutUByte(v&0x7f)` |
| +54 | `PutSByte` 0x14752a | aligned |
| +58 | `PutUByteWithMSBZero(v)` 0x147004 | **identical to `PutUByte(v)`** (tail jump to vt+4c, no masking). The name only says that v < 0x80 is expected (SysEx-safe) |
| +5c | `PutString(s, max)` 0x14789e | aligned; writes min(len,max) chars. When len < max a NUL terminator follows. A string of exactly `max` chars has **no** terminator |
| +60 | `PutStringFixLength(s, n)` 0x14757e | aligned; exactly n bytes, zero-padded |
| +64 | `MoveToByteBoundary` 0x146e70 | read side: skips the rest of the current byte |
| +68 | `ZeroPadToByteBoundary` 0x14701a | write side: `PutUBits(8-bitpos, 0)` when not aligned |
| non-virtual | `PutUWord`/`GetUWord` 0x146f40/0x146f74 | 2 aligned bytes, **big-endian 8-bit** (hi, lo) |

Consequences that matter for an implementation:
* Every `u8`, `u16be`, `u14` and `str` field is **byte-aligned**. A bit-field run (`PutUBits`/`PutBit`) followed by a
  byte field is implicitly zero-padded to the next byte.
* `CMIDIOutStream::Initialize(id, type, msb)` 0x0cc70 stores `id` in byte 0 and advances 8 bits.
  `CMIDIInStream::CheckId` 0x0cbb6 consumes byte 0 and returns it. The "expected id" argument is not checked.
* Received molecule boundaries: `CBubblePort::AddToResultBubble` 0x117e8a builds a **new** CMIDIInStream at the
  start of each molecule. It calls `ReadStream` and then advances by `GetUsedBytes()` (= ceil(bits/8)). So every molecule
  starts byte-aligned, and trailing bits of a molecule's last byte are padding. The loop also stops on two bytes:
  * `kMesAck` = 0x7f: end of the list.
  * `kMesException` = 0x7e: throws `XMHostMidi(next byte)`.

  An id the factory does not know throws `XMUnregisteredMolecule`.
* Other `CSynthPort` constants at 0x1e5a44.. (not molecules, handled by the port layer):
  * `kMesPerformanceRelease`=0x1f
  * `kMesPatchRelease`=0x38
  * `kMesBlinkMultiBit`=0x3a (Verhue R_VOLUME_DATA)
  * `kMesBlinkDualBit`=0x39 (Verhue R_LED_DATA)
  * `kMesException`=0x7e (R_ERROR)
  * `kMesAck`=0x7f (R_OK)

## 1. CMolecule framework

**CMolecule vtable** (0x29cea8, checked against the binary):
* +00/+04: dtors
* +08: `WriteStream(CMIDIOutStream&)`
* +0c: `ReadStream(CMIDIInStream&)`
* +10: `WriteStream(CBitStream&)`
* +14: `ReadStream(CBitStream&)`
* +18: `UseOnPatch`
* +1c: `UseOnSynth`
* +20: `UseOnPerformance`
* +24: `IsTerminatingMolecule` (default 0, 0x1a046c)
* **+28: `EditorOnly`** (default 0, 0x1a0474)

`CBubblePort::GenerateMessages` 0x11828a calls vt+0x28 on each molecule and **skips it when it returns true**. It then
calls vt+0x08. The first non-skipped molecule decides the "request" flag of the whole bubble: `IsRequest()` → header
bit 0x10. Molecules are packed back-to-back at byte granularity and split into chunks of ≤ 0xffff bytes.
`GenerateRealtimeMessages` 0x118536 does **not** call EditorOnly. It writes one message per molecule (header bytes 3, 0x20).

**EditorOnly overrides** (never sent, local undo/redo only):
* `CMMutaParams` 0x1a48c2 → 1
* `CMMutaParentCount` 0x1a48e2 → 1
* `CMMutaSetMarker` 0x1a48ec → 1
* `CMMutaClearIndivid` 0x1a48f6 → 1
* `CMMutaParamDump` 0x1a48cc → `mode(+0x11c) != 2`. In mode 2 it IS sent and writes 3×0x4d + 0x65, see below.

None of these has its own id. **`CMMutaLock` (0x90) is NOT EditorOnly**, so it is sent to the synth.

**IsTerminatingMolecule override**: `CMFlashDumpMarker` 0x10cb6 returns true when its status is 3 (wire code 2,
"name follows"). `CSynth::BubbleInput` 0x11dd40 stops applying the rest of a synth bubble after such a molecule.

**EMessageType** (2nd argument of Initialize):
* 0 = normal command
* 1 = realtime
* 2 = request (a response is expected)

The dump molecules that delegate to an NSFile section writer do not call Initialize. They set stream+0x14 (type)
= 0 and +0x18 (MSB usage) = 0 directly. `CMSynthDataDump` sets neither, so it keeps the ctor defaults: type 0, msb 1.

**Destination** (`CMoleculeBuilder` +4, read by `CMoleculeFactory::GetDestination` 0xfbda for the first molecule of a
received bubble; dispatched in `CSynth::RouteBubble` 0x11f940):

| value | meaning | notes |
|---|---|---|
| 1 | Synth | → `CSynth::BubbleInput`, `UseOnSynth` |
| 2 | Patch/slot | → `CPatch::BubbleInput`. Only applied when the bubble's session number matches the slot's. Otherwise the editor creates an empty upload patch and re-syncs |
| 3 | Performance | → `CPerformance::BubbleInput`, `UseOnPerformance` |

`CMolecule::CMolecule(EDestination)` stores the same enum. Almost all molecules pass 0. Exceptions: 3 for
GlobalKnobAssign, GlobalKnobDeassign, GlobalParameterPageFocus and PerformanceHeaderParam; 2 for MutaParams.

**Receivable ids**: `CMoleculeFactory::CMoleculeFactory` 0xfcc8 registers 50 builders (36 `CMB*` classes and 14
`TplMoleculeBuilder<CM*,id,dest>`). The factory is also used locally by `CPatch::FileToBubble` 0xd4d76 and
`CMCompletePerformanceDump` to re-parse .pch2 sections as molecules. `CMBGeneric` is a *module* builder, unrelated.

**Contexts**:
* `NSFile_V7::EContext` (location): 0 = FX, 1 = VA, 2 = patch settings (morph/volume/glide…). `CPatch::GetPatchData`
  0xd3b48 maps 0→+0x28, 1→+0x2c, 2→+0x30. The default is 1. Readers clamp the value to ≤ 2 (`gLimitCnt`). The FX/VA
  naming follows Verhue: inferred, not proven by strings.
* `EFlashFileType`: 0 = patch, 1 = performance. `CSynth::GetFlashData(0)` → +0x1c, which is also the list that handles
  FlashUsage. Inferred.
* `EFlashCommandOrigin`: 0 = `CFlashData` (bank list window), 1 = `CFlashDumper` (patch-file transfer).
* `CFlashEntry` = {u8 bank 0..31, u8 program 0..127}.
* `CConnector` = {u8 module, u8 connector, u8 isOutput}.
* `CCableSegmentSpec` = {u32 color, CConnector from (+4), CConnector to (+7)}.

---------------------------------------------------------------------------------------------------------

## 2. Summary table (sorted by id)

Dir:
* E→S: editor writes the molecule (WriteStream + Initialize).
* S→E: registered in the factory with ReadStream.
* both: both of the above.

Type: EMessageType of WriteStream. Dest: builder destination for received molecules (1 Synth, 2 Patch, 3 Perf).
Verhue: constant name in `BVE.NMG2Types.pas`. "(sect)" = an NSFile_V11 section writer emits the header.

| id | Clavia class | Dir | Type | Dest | Verhue | Handler (UseOn…) | Payload after id (summary) |
|---|---|---|---|---|---|---|---|
| 02 | CMSynthDataRequest | E→S | 2 | – | Q_SYNTH_SETTINGS | – | (none) |
| 03 | CMSynthDataDump | both | 0 | 1 | S_SYNTH_SETTINGS | CSynth::HandleSynthDataDump | CSynthMap (no length) |
| 04 | CMNumberOfVoicesRequest | E→S | 2 | – | Q_ASSIGNED_VOICES | – | (none) |
| 05 | CMNumberOfVoicesDump | S→E | – | 1 | R_ASSIGNED_VOICES | CSynth::HandleNumberOfVoicesDump | 4×u8 voices A..D |
| 06 | CMSlotSelectionRequest | E→S | 2 | – | – | – | (none) |
| 07 | CMSlotSelectionDump | both | 0 | 3 | – | CPerformance::HandleSlotSelectionDump | pad4, 4×bit enabled A..D |
| 08 | CMSlotFocusRequest | E→S | 2 | – | – | – | (none) |
| 09 | CMSlotFocusDump | both | **2** | 3 | S_SEL_SLOT | CPerformance::HandleSlotFocusDump | pad5, bit none, b2 slot |
| 0a | CMFlashLoadCommand | E→S | 0 | – | S_RETREIVE | – | u8 slot, u8 bank, u8 prog |
| 0b | CMFlashStoreCommand | E→S | 0 | – | S_STORE | – | u8 slot, u8 bank, u8 prog |
| 0c | CMFlashDeleteCommand | E→S | 0 | – | S_CLEAR | – | u8 type, u8 bank, u8 prog, u8 origin |
| 0d | CMFlashCommandResult | S→E | – | 1 | R_STORE | CSynth::HandleFlashCommandResult | u8 type, u8 bank, u8 prog, u8 origin, u8 result |
| 0e | CMFlashDeleteRangeCommand | E→S | 0 | – | S_CLEAR_BANK | – | u8 type, u8 b1, u8 p1, u8 b2, u8 p2, u8 origin |
| 0f | CMKeyboardFocusDump | both | 0 | 3 | – | CPerformance::HandleKeyboardFocusDump | pad4, 4×bit kbd A..D |
| 10 | CMPerformanceHeaderRequest | E→S | 2 | – | Q_PERF_SETTINGS | – | (none) |
| 11 | CMPerformanceHeaderDump | both | 0 (sect) | 3 | C_PERF_SETTINGS | CPerformance::HandlePerformanceHeaderDump | CPerformanceHeader_11 (u16 len) |
| 12 | CMFlashDeleteRangeDump | S→E | – | 1 | R_CLEAR_BANK | CSynth::HandleFlashDeleteRangeDump | u8 type, u8 b1, u8 p1, u8 b2, u8 p2 |
| 13 | CMFlashUsageDump | S→E | – | 1 | R_LIST_NAMES (sic) | CSynth::HandleFlashUsageDump | u14le usage (≤1000) |
| 14 | CMFlashDataRequest | E→S | 2 | – | Q_LIST_NAMES | – | u8 type, u8 bank, u8 prog |
| 15 | CMFlashDeleteDump | S→E | – | 1 | R_CLEAR | CSynth::HandleFlashDeleteDump | u8 type, u8 bank, u8 prog |
| 16 | CMFlashDataDump | S→E | – | 1 | R_ADD_NAMES | CSynth::HandleFlashDataDump | u8 flag, u8 type, tagged name list |
| 17 | CMFlashDumpRequest | E→S | 2 | – | S_PATCH_BANK_UPLOAD | – | u8 type, u8 bank, u8 prog |
| 18 | CMFlashDumpMarker | S→E | – | 1 | R_PATCH_BANK_UPDLOAD | CFlashDumper::Handle* | u8 code, u8 ?, u8 bank, u8 prog, [str16] |
| 19 | CMFlashDumpRawData | both | 0 | 1 | S_PATCH_BANK_DATA | CSynth::HandleFlashRawDataDump | u8 type, u8 bank, u8 prog, str16, u16be n, u8 ver, n bytes |
| 1a | CMCompletePerformanceDump | E→S | 0 | – | – | – | 0x29 + 0x11 + 4×patch sections + 0x5f |
| 1c | CMGlobalKnobAssign | both | 0 | 3 | S_ASS_GLOBAL_KNOB | CPerformance::HandleGlobalKnobAssign | b2 isLed, b2 slot, b2 loc, pad2, u8 mod, u8 par, u16be knob |
| 1d | CMGlobalKnobDeassign | both | 0 | 3 | S_DEASS_GLOB_KNOB | CPerformance::HandleGlobalKnobDeassign | u16be knob |
| 1e | CMGlobalParameterPageFocus | both | 0 | 3 | S_SEL_GLOBAL_PAGE | CPerformance::HandleGlobalParameterPageFocus | u8 page |
| 1f | CMPerformanceRelease | S→E | – | 1 | – (kMesPerformanceRelease) | CSynth::HandlePerformanceRelease | u8 session |
| 20 | CMPatchHeaderRequest | E→S | 2 | – | – | – | (none) |
| 21 | CMPatchHeaderDump | both | 0 (sect) | 2 | C_PATCH_DESCR | CPatch::HandlePatchHeaderDump | CPatchHeaderData_11 (u16 len, fixed) |
| 22 | CMCtrlAssign | both | 0 | 2 | S_ASSIGN_MIDICC | CPatch::HandleCtrlAssign | u8 loc, u8 mod, u8 par, pad1+b7 cc |
| 23 | CMCtrlDeassign | both | 0 | 2 | S_DEASSIGN_MIDICC | CPatch::HandleCtrlDeassign | pad1+b7 cc |
| 25 | CMKnobAssign | both | 0 | 2 | S_ASSIGN_KNOB | CPatch::HandleKnobAssign | u8 mod, u8 par, b2 loc, b2 isLed, pad4, u16be knob |
| 26 | CMKnobDeassign | both | 0 | 2 | S_DEASSIGN_KNOB | CPatch::HandleKnobDeassign | u16be knob |
| 27 | CMPatchNameDump | both | 0 | 2 | S_PATCH_NAME | CPatch::HandlePatchNameDump | str16 |
| 28 | CMPatchNameRequest | E→S | **0** | – | Q_PATCH_NAME | – | (none) |
| 29 | CMPerformanceNameDump | both | 0 | 3 | C_PERF_NAME | CPerformance::HandlePerformanceNameDump | str16 |
| 2a | CMModuleABCode | E→S | 0 | – | S_SET_UPRATE | (CPatch::HandleModuleABCode) | u8 loc, u8 mod, u8 bandwidth |
| 2b | CMDSPPartChange | E→S | 0 | – | S_SET_MODE | (CPatch::HandleModuleDSPGroup) | u8 loc, u8 mod, u8 selector, u8 value |
| 2c | CMPerformanceNameRequest | E→S | **0** | – | – | – | (none) |
| 2d | CMParameterPageFocus | both | 0 | 2 | S_SEL_PARAM_PAGE | CPatch::HandleParameterPageFocus | u8 page |
| 2e | CMParamFocusRequest | E→S | 2 | – | Q_SELECTED_PARAM | – | (none) |
| 2f | CMParamFocusDump | both | **1** | 2 | S_SEL_PARAM | CPatch::HandleParamFocusDump | u8 flag, u8 loc, u8 mod, u8 par |
| 30 | CMModuleNew | both | 0 | 2 | S_ADD_MODULE | CPatch::HandleModuleNew | type, loc, idx, x, y, color, bw, locked, N×mode, str16 |
| 31 | CMModuleRecolor | E→S | 0 | – | S_SET_MODULE_COLOR | (CPatch::HandleModuleRecolor) | u8 loc, u8 mod, u8 color |
| 32 | CMModuleDelete | E→S | 0 | – | S_DEL_MODULE | (CPatch::HandleModuleDelete) | u8 loc, u8 mod |
| 33 | CMModuleName | E→S | 0 | – | S_SET_MODULE_LABEL | (CPatch::HandleModuleName) | u8 loc, u8 mod, str16 |
| 34 | CMModuleMove | E→S | 0 | – | S_MOV_MODULE | (CPatch::HandleModuleMove) | u8 loc, u8 mod, u8 x, u8 y |
| 35 | CMSessionNumberRequest | E→S | 2 | – | Q_VERSION_CNT | – | u8 slot (0..3, 4 = perf) |
| 36 | CMSessionNumberDump | S→E | – | 1 | – | CSynth::HandleSessionNumberDump | u8 slot, u8 session |
| 37 | CMDumpBubbleDestination | E→S | 2 | – | S_SET_PATCH | – | u8 0, u8 bank 0, u8 prog 0, str16 name |
| 38 | CMSessionPatchRelease | S→E | – | 1 | R_PATCH_VERSION_CHANGE (kMesPatchRelease) | CSynth::HandleSessionPatchRelease | u8 slot, u8 session |
| 39/3a | (not molecules) | S→E | – | – | R_LED_DATA / R_VOLUME_DATA | port-level blink data | – |
| 3b | CMClockInfoRequest | E→S | 2 | – | Q_MASTER_CLOCK | – | (none) |
| 3c | CMCompletePatchRequest | E→S | 2 | – | Q_PATCH | – | (none) |
| 3d | CMDumpOneRequest | E→S | 2 | – | S_MIDI_DUMP | – | (none) |
| 3e | CMPerformanceModeChange | E→S (Read exists, not registered) | 0 | – | S_SET_PARAM_MODE (Verhue name) | CSynth::HandlePerformanceModeChange | u8 perfMode, u8 flag2 |
| 3f | CMPerformanceHeaderParam | both | 0 | 3 | S_SET_MASTER_CLOCK | CPerformance::HandlePerformanceHeaderParam | u8 scope, u8 param, [u8 value] |
| 40 | CMParamChange | both | **1** | 2 | S_SET_PARAM | CPatch::HandleParamChange | u8 loc, u8 mod, u8 par, u8 value, u8 variation |
| 42 | CMCustomData | E→S | 0 | – | S_SET_PARAM_LABEL | (CPatch::HandleCustomData) | u8 loc, u8 mod, u8 n, n×u8 |
| 43 | CMMorphChange | both | 0 or 1 | 2 | S_SET_MORPH_RANGE | CPatch::HandleMorphChange | u8 loc, u8 mod, u8 par, u8 morph, u8 \|range\|, u8 neg, u8 var |
| 44 | CMParamSettingCopy | both | 0 | 2 | S_COPY_VARIATION | CPatch::HandleParamSettingCopy | u8 from, u8 to |
| 4a | CMModuleDump | both | 0 (sect) | 2 | C_MODULE_LIST | CPatch::HandleModuleDump | CModuleData_11 (u16 len) |
| 4b | CMModuleRequest | E→S | 2 | – | – | – | u8 loc |
| 4c | CMModuleParamRequest | E→S | 2 | – | Q_PARAMS | – | u8 loc |
| 4d | CMModuleParamDump | both | 0 (sect) | 2 | C_PARAM_LIST | CPatch::HandleModuleParamDump | CModuleParamData_11 (u16 len) |
| 4e | CMModuleNameRequest | E→S | 2 | – | – | – | u8 loc |
| 4f | CMModuleCustomDataRequest | E→S | 2 | – | Q_PARAM_NAMES | – | u8 loc |
| 50 | CMCableConnect | E→S | 0 | – | S_ADD_CABLE | (CPatch::HandleCableConnect) | pad3, b1 !dump, b1 VA, b3 color, from(2), to(2) |
| 51 | CMCableDelete | E→S | 0 | – | S_DEL_CABLE | (CPatch::HandleCableDelete) | pad6, b1 !dump, b1 VA, from(2), to(2) |
| 52 | CMCableDump | both | 0 (sect) | 2 | C_CABLE_LIST | CPatch::HandleCableDump | CCableData_11 (u16 len) |
| 53 | CMCableRequest | E→S | 2 | – | – | – | u8 loc |
| 54 | CMCableRecolor | E→S | 0 | – | S_CABLE_COLOR | (CPatch::HandleCableRecolor) | pad4, b1 VA, b3 color, from(2), to(2) |
| 55 | CMSendCtrlSnap | E→S | 0 | – | S_CTRL_SNAPSHOT | (empty) | (none) |
| 56 | CMPlayNote | E→S | 0 | – | S_PLAY_NOTE | (empty) | u8 0=on/1=off, u8 note |
| 58 | CMParameterPageFocusRequest | E→S | 2 | – | – | – | (none) |
| 59 | CMGlobalParameterPageFocusRequest | E→S | 2 | – | M_UNKNOWN_2 | – | (none) |
| 5a | CMModuleNameDump | both | 0 (sect) | 2 | C_MODULE_NAMES | CPatch::HandleModuleNameDump | CModuleNameData_11 (u16 len) |
| 5b | CMModuleCustomDataDump | both | 0 (sect) | 2 | C_PARAM_NAMES | CPatch::HandleModuleCustomDataDump | CModuleCustomData_11 (u16 len) |
| 5d | CMSysClockInfo | S→E | – | 1 | R_EXT_MASTER_CLOCK | CSynth::HandleSysClockInfoDump | u8 flag, u16be rate |
| 5e | CMPerfKnobMapRequest | E→S | 2 | – | Q_GLOBAL_KNOBS | – | (none) |
| 5f | CMPerformanceKnobMapDump | both | 0 (sect) | 3 | C_KNOBS_GLOBAL | CPerformance::HandlePerformanceKnobMapDump | CKnobMapData_11 mode 1 (u16 len) |
| 60 | CMCtrlMapDump | both | 0 (sect) | 2 | C_CONTROLLERS | CPatch::HandleCtrlMapDump | CCtrlMapData_11 (u16 len) |
| 61 | CMCtrlMapRequest | E→S | 2 | – | – | – | (none) |
| 62 | CMKnobMapDump | both | 0 (sect) | 2 | C_KNOBS | CPatch::HandleKnobMapDump | CKnobMapData_11 mode 0 (u16 len) |
| 63 | CMKnobMapRequest | E→S | 2 | – | – | – | (none) |
| 65 | CMMorphMapDump | both | 0 (sect) | 2 | C_MORPH_PARAM | CPatch::HandleMorphMapDump | CMorphMapData_11 (u16 len) |
| 66 | CMMorphMapRequest | E→S | 2 | – | – | – | (none) |
| 68 | CMCurrentNotesRequest | E→S | **0** | – | Q_CURRENT_NOTE | – | (none) |
| 69 | CMCurrentNotesDump | both | 0 (sect) | 2 | C_CURRENT_NOTE_2 | CPatch::HandleCurrentNotesDump | CCurrentVoiceData_11 (u16 len) |
| 6a | CMParamSettingFocus | both | 0 | 2 | S_SEL_VARIATION | CPatch::HandleParamSettingFocus | pad1+b7 variation |
| 6e | CMTextpadRequest | E→S | 2 | – | Q_PATCH_TEXT | – | (none) |
| 6f | CMNotePadDump | both | 0 (sect) | 2 | C_PATCH_NOTES | CPatch::HandleNotePadDump | CTextpadData_11 (u16 len + text) |
| 70 | CMFlushBlink | E→S | 0 | – | M_UNKNOWN_6 | – | (none) |
| 71 | CMPatchLoadRequest | E→S | 2 | – | Q_RESOURCES_USED | – | u8 (loc==VA) |
| 72 | CMPatchLoad | S→E | – | 2 | R_RESOURCES_USED | CPatch::HandlePatchLoad | u8 isVA + resource counters (28 bytes) |
| 74 | CMBarbMapChangeNotify | S→E | – | 2 | – | CPatch::HandleBarbMapChangeNotify | (none) |
| 75 | CMPerfBarbMapChangeNotify | S→E | – | 3 | – | CPerformance::HandlePerfBarbMapChangeNotify | (none) |
| 7d | CMSynthUnlockEditorSync | E→S | 0 | – | S_START_STOP_COM | – | u8 bool (1 = sync start/lock, 0 = end/unlock) |
| 7e/7f | (kMesException / kMesAck) | S→E | – | – | R_ERROR / R_OK | bubble-port terminators | – |
| 80 | CMMidiLearn | S→E | – | 1 | R_MIDI_CC | CSynth::HandleMidiLearn | u8 slot, u8 cc |
| 81 | CMMidiLearnRequest | E→S | 2 | – | M_UNKNOWN_1 | – | (none) |
| 90 | CMMutaLock | E→S | 0 | – | – | (CPatch::ReceiveMutaLockMolecule) | u8 loc, u8 mod, u8 locked |

Classes without an id:
* Abstract bases: `CMCableABC`, `CMModuleMoleculeABC`.
* Editor-only: `CMMutaParams`, `CMMutaParentCount`, `CMMutaSetMarker`, `CMMutaClearIndivid`.
* `CMMutaParamDump`: a composite (see §3.7).

Handler names in parentheses belong to molecules that are not registered in the factory. Those UseOn* handlers are only used
locally: `CSynth::BubbleOutput` 0x11de84 and `CPatch` user bubbles send the molecule **and** apply it to the local model.

Counts:
* 109 CM molecule classes (108 files plus `CMBarbMapChangeNotify`, which sits among the CMB files).
* 102 have a wire id:
  * 52 editor→synth only (including 0x3e, which has a ReadStream but is not registered);
  * 15 synth→editor only;
  * 35 both (mostly dumps whose WriteStream is used for uploads).

  So 87 have a WriteStream and 50 are receivable (= the 50 factory builders).
* Ids 0x39, 0x3a, 0x7e and 0x7f are port-level messages, not molecules.

---------------------------------------------------------------------------------------------------------

## 3. Per-molecule layouts

Notation:
* `u8` = aligned byte.
* `bN` = N-bit field (`PutUBits`/`GetUBits`, MSB-first).
* `padN` = N zero bits.
* `u16be` = `PutUWord`.
* `u14` = 7-bit big-endian pair (`Get14BitWord`).
* `str16` = `PutString(s,16)`, NUL-terminated unless exactly 16 chars.
* "this+X" = field offset in the C++ object.

### 3.1 Synth / global

**0x02 CMSynthDataRequest**
* [W] 0x186aa `Initialize(2,2,1)`, no payload.
* Sent by `CSynth::SendSynthDataRequests` 0x11e42a.

**0x03 CMSynthDataDump** (builder 0x1a1dd2, dest 1)
* [R] 0x18702 → `CSynthMap::ReadStream` 0x12012e. [W] 0x1872a → `CSynthMap::WriteStream` 0x120410.
* The writer emits its own id byte 0x03. There is **no length field**.
* Layout (names from the `CSynthMap::Set*/Enable*` accessors at 0x11fb52..0x11fc76):
  1. `u8 0x03`
  2. `str16 synthName`
  3. `b1 perfMode(+4)`, `b7 patchSortMode(+8)`, `b7 perfSortMode(+0xc)`, pad1
  4. `u8 fileFocus.bank(+0x10)`, `u8 fileFocus.prog(+0x11)`
  5. `b1 fileProtect(+0x12)`, pad7
  6. 4 × `u8 midiChannel[slot A..D]`. The value 0x10 means "MIDI disabled for this slot". Otherwise 0..15 (`SetChannel`/`SetMidiEnabled`)
  7. `u8 midiGlobalChannel(+0x2b)`, `u8 midiSysExId(+0x2c)`
  8. `b1 midiLocalOn(+0x2d)`, pad7
  9. `u8 programChangeMode(+0x30)`, `u8 midiCtrlMode(+0x34)`
  10. `b1 midiSendArp(+0x38)`, `b1 midiSendClock(+0x39)`, `b1 midiIgnoreExtClock(+0x3a)`, pad5
  11. `s8 masterTune(+0x3c)`
  12. `b1 octShiftEnable(+0x3e)`, pad7
  13. `s8 octShift(+0x3f)`, `s8 transpose(+0x40)`
  14. `u8 vibratoRate(+0x44)`
  15. `b1 sustainPolarity(+0x41)`, `b1 1` (constant on write, ignored on read), pad6
  16. `u8 controlPedalGain(+0x42)`
  17. 16 × `u8 0` (reserved; read and ignored)
* Note: the editor also *sends* 0x03 with only the perfMode bit changed (`CSynth::CleanDirtyData` 0x11f394, via
  `BubbleOutput`).

**0x04 CMNumberOfVoicesRequest**
* [W] 0x17d9a `Initialize(4,2,1)`, no payload.

**0x05 CMNumberOfVoicesDump** (builder 0x17e7c, dest 1)
* [R] 0x1836c: 4 × `u8` voice count for slots A..D (`CCurrentVoiceInfo::SetNumber`).

**0x35 CMSessionNumberRequest**
* [W] 0x177e0 `Initialize(0x35,2,1)`; `u8 slot(+8)`.
* The slot is 0..3, or 4 for the performance-level session (`CSynth::CleanDirtySynthData` 0x11e7aa passes 4).

**0x36 CMSessionNumberDump** (builder 0x17854, dest 1)
* [R] 0x17994: `u8 slot` (`CRangeAddOff<0,3>`, 4 = performance), `u8 sessionNumber`.
* `CSynth::HandleSessionNumberDump`: slot 4 sets the performance session at synth+0x18. Otherwise it sets the
  per-slot session and calls `FlushLedBlinkData`.

**0x38 CMSessionPatchRelease** (builder 0x17910, dest 1; = `kMesPatchRelease`)
* [R] 0x1794a: `u8 slot(→+9, 0..3)`, `u8 session(→+8)`.
* Meaning: the synth replaced the patch in that slot. The editor stores the session, calls
  `CPatchManager::CreateEmptyUploadPatch` and re-syncs.

**0x1f CMPerformanceRelease** (builder 0x169ce, dest 1; = `kMesPerformanceRelease`)
* [R] 0x1697e: `u8 sessionNumber`.
* The handler `CSynth::HandlePerformanceRelease` creates the initial performance and restarts the sync state machine.

**0x3b CMClockInfoRequest**
* [W] 0x186d6 `(0x3b,2,1)`, no payload.

**0x5d CMSysClockInfo** (builder 0x1a1e64, dest 1)
* [R] 0x188a8: `u8 flag` (bool → CSysClockInfo+0), then `u16be rate` (+4).
* `CSynth::GetClockRate` uses +4 when the clock is external, so the flag is probably "external clock present" and the
  rate is the external BPM. Inferred.

**0x3e CMPerformanceModeChange** (not registered in the factory)
* [W] 0x189d6 `(0x3e,0,0)`: `u8 perfMode(+8)`, `u8 flag2(+9)`.
* [R] 0x18a2c reads the same two bytes as bools.
* UseOnSynth → `CSynthMap::SetPerfMode`. The meaning of flag2 is unknown.

**0x7d CMSynthUnlockEditorSync**
* [W] 0x18862 `(0x7d,0,1)`; `u8 bool(+8)`.
* `CSynth::CleanDirtyData` 0x11f394 sends `true` (01) when the sync starts (state 0→1). It sends `false` (00) once
  everything is clean (state 1→2).
* This matches Verhue's START_COMM=0 / STOP_COMM=1: 01 = editor busy (lock), 00 = editor done (unlock).

**0x80 CMMidiLearn** (builder 0x1a1ef6, dest 1)
* [R] 0x18af4: `u8 slot(→+9, 0..4)`, `u8 midiCC(→+8, 0..119)`.
* `CSynth::HandleMidiLearn` 0x11d382 stores the CC in the slot's learn entry unless the CC is pre-assigned.

**0x81 CMMidiLearnRequest**
* [W] 0x18a9a `(0x81,2,1)`, no payload. Sent by `CSynth::SendMidiLearnRequest` 0x11e374.

**0x56 CMPlayNote**
* [W] 0x187a8 `(0x56,0,1)`: `u8 (kind(+0xc)==1)`, `u8 note(+8)`.
* `CSynth::CleanDirtySynthData` sends `CMPlayNote(0, note)` = note-on and `CMPlayNote(1, note)` = note-off for the
  virtual keyboard. So the bytes are `56 00 nn` (on) and `56 01 nn` (off).
* UseOnPatch is empty.

### 3.2 Performance / slots

**0x06 CMSlotSelectionRequest**
* [W] 0x17bb6 `(6,2,1)`, no payload.

**0x07 CMSlotSelectionDump** (builder 0x17d08, dest 3)
* [W] 0x1853c `(7,0,1)` / [R] 0x18480: `pad4`, then 4 × `b1` "slot enabled", slot A first.
* Byte value = A·0x08 | B·0x04 | C·0x02 | D·0x01.

**0x08 CMSlotFocusRequest**
* [W] 0x17d4e `(8,2,1)`, no payload.

**0x09 CMSlotFocusDump** (builder 0x17d90, dest 3)
* [W] 0x181c4 `(9,2,1)`, a **request** type: `pad5`, `b1 none`, then `b2 slot` (when none=0) or padding (when none=1).
* The byte value is the slot (0..3), or 0x04 for "no slot". [R] 0x180de is symmetrical.

**0x0f CMKeyboardFocusDump** (builder 0x17fe8, dest 3)
* [W] 0x18260 `(0xf,0,1)` / [R] 0x182f2: `pad4`, then 4 × `b1` "keyboard enabled" for slots A..D (A = 0x08).

**0x10 CMPerformanceHeaderRequest**
* [W] 0x16a14 `(0x10,2,1)`, no payload.

**0x11 CMPerformanceHeaderDump** (Tpl builder id 17, dest 3)
* [W] 0xdeba / [R] 0xd662 → `NSFile_V11::CPerformanceHeader_11::WriteStream` 0x64254 / `ReadStream` 0x68e38.
* Header: `u8 0x11`, `u16be length` (CStreamSizer), then the body. Slot names are written inside as str16.

**0x29 CMPerformanceNameDump** (builder 0x16b94, dest 3)
* [W] 0x16b08 / [R] 0x1702a: `str16 name`.
* When the stream is already initialized (nested in 0x1a), it writes a raw `PutUByte(0x29)` instead of calling Initialize.

**0x2c CMPerformanceNameRequest**
* [W] 0x16a40 `(0x2c,0,1)`, no payload. Note type 0, not 2.

**0x3f CMPerformanceHeaderParam** (builder 0x1a228c, dest 3)
* [W] 0x16d86 `(0x3f,0,0)` / [R] 0x16dfe: `u8 scope(+8)`, `u8 param(+9)`, then `u8 value(+10)` **only if scope > 3 && param < 3**.
* The 2-argument ctor uses scope = 0xff (global). `CPerformance::HandlePerformanceHeaderParam` 0xffe40 handles:

  | param | effect |
  |---|---|
  | 0 | value 0..1 → perf+0x90, master-clock run/stop (probable) |
  | 1 | value → perf+0x8c, master clock BPM (used by `GetClockRate`) |
  | 2 | value → perf+0x89 (bool), probably keyboard-range enable |

* So the wire form is `3f ff pp vv`. The scope ≤ 3 form carries no value byte; its use was not observed.

**0x1a CMCompletePerformanceDump** (send only; `CSynth::CleanDirtyData`)
* [W] 0x17248 `Initialize(0x1a,0,0)`, then:
  1. `CMPerformanceNameDump` (0x29 + str16)
  2. `CMPerformanceHeaderDump` (0x11 + len + …)
  3. For slot 0..3: `CPatch::PatchToFile`, then `CPatchFile_13::WriteMolecules` 0x51e7c into a scratch CFileByteStream. That
     stream is re-parsed with `gFactory`, and each molecule's `WriteStream(CMIDIOutStream)` is called. The result is that
     the per-slot .pch2 sections are copied verbatim (sections listed in §4).
  4. `CMPerformanceKnobMapDump` (0x5f).
* Always preceded in the same bubble by 0x37 CMDumpBubbleDestination(perf name).

**0x1c CMGlobalKnobAssign** (builder 0x1a22d2, dest 3)
* [W] 0x1719c `(0x1c,0,0)` / [R] 0x1764e:
  `b2 assignType(+0x14, EDeviceAssignType 0..1)`, `b2 slot(+8)`, `b2 loc(+0xc)`, pad2, `u8 module(+0x10)`,
  `u8 param(+0x11)`, `u16be knob(+0x18)`.
* The knob is `CRangeAddOff<u16,0,119,TagKnobSpec>`; 0x78 (120) = none.

**0x1d CMGlobalKnobDeassign** (builder 0x1a2318, dest 3)
* [W] 0x16ebc `(0x1d,0,0)` / [R] 0x170ee: `u16be knob`.

**0x1e CMGlobalParameterPageFocus** (builder 0x1a235e, dest 3)
* [W] 0x16fb6 `(0x1e,0,1)` / [R] 0x16ffc: `u8 page`.

**0x59 CMGlobalParameterPageFocusRequest**
* [W] 0x16e7a `(0x59,2,1)`, no payload.

**0x5e CMPerfKnobMapRequest**
* [W] 0x16e4e `(0x5e,2,1)`, no payload.

**0x5f CMPerformanceKnobMapDump** (Tpl id 95, dest 3)
* → `CKnobMapData_11::WriteStream` 0x69642 with mode(+0x1c) = 1, which writes id **0x5f**. Mode 0 writes id 0x62.
* Header: `u8 0x5f`, `u16be len`.

**0x75 CMPerfBarbMapChangeNotify** (builder 0x1a2246, dest 3)
* [R] 0xddaa: id only. Tells the editor that the global knob map changed on the synth, so it re-requests it.
* "Barb" = knob/target map: `CBarbTargetSpec`.

### 3.3 Flash (bank) commands

**0x0a CMFlashLoadCommand**
* [W] 0x1142a `(10,0,1)`: `u8 slot(+8)`, `u8 bank(+9)`, `u8 prog(+0xa)`.
* The ctor takes `CRangeAddOff<0,3,TagSlotNumber>` plus a `CFlashEntry`.

**0x0b CMFlashStoreCommand**
* [W] 0x112b8 `(0xb,0,1)`: `u8 slot`, `u8 bank`, `u8 prog`. Same object layout as 0x0a.

**0x0c CMFlashDeleteCommand**
* [W] 0x1131e `(0xc,0,1)`: `u8 fileType(+8)`, `u8 bank(+0xc)`, `u8 prog(+0xd)`, `u8 origin(+0x10)`.
* The ctor is `(EFlashFileType, CFlashEntry, EFlashCommandOrigin)`. `CFlashDumper::SendDeleteInFlash` uses origin 1.

**0x0e CMFlashDeleteRangeCommand**
* [W] 0x11394 `(0xe,0,1)`: `u8 fileType`, `u8 bank1`, `u8 prog1`, `u8 bank2`, `u8 prog2`, `u8 origin`.

**0x0d CMFlashCommandResult** (builder 0x1a1dc8, dest 1)
* [R] 0x116c4: `u8 fileType (bool)`, `u8 bank`, `u8 prog`, `u8 origin` (1 → 1, else 0), `u8 result`.
* The result byte maps to the internal enum as follows: 0→0, 1→1, 2→2, 4→4, 5→5, anything else→3.
* `CSynth::HandleFlashCommandResult` routes by origin: 0 → `CFlashData` (patch or perf list), 1 → `CFlashDumper`.

**0x12 CMFlashDeleteRangeDump** (builder 0x1a1d78, dest 1)
* [R] 0x11626: `u8 fileType`, `u8 bank1`, `u8 prog1`, `u8 bank2`, `u8 prog2`.

**0x13 CMFlashUsageDump** (builder 0x1a1cfc, dest 1)
* [R] 0x10c1a: `u8 lo`, `u8 hi`. The value is `lo | hi<<7`, clamped to 1000 (per-mille usage?).
* It always goes to the patch `CFlashData`.

**0x14 CMFlashDataRequest** ("list names")
* [W] 0x111ec `(0x14,2,1)`: `u8 fileType(+0xc)`, `u8 bank(+8)`, `u8 prog(+9)` = the start entry.
* Sent by `CFlashData::SendFlashDumpRequest` 0x1193e0.

**0x15 CMFlashDeleteDump** (builder 0x1a1d42, dest 1)
* [R] 0x115bc: `u8 fileType`, `u8 bank`, `u8 prog`.

**0x16 CMFlashDataDump** (builder 0x1a1cb6, dest 1)
* [R] 0x11cfa: `u8 flag(→+9 bool)`, `u8 fileType(→+0xc)`, then a tagged loop. Each tag is read with `GetUByte`:

  | tag | meaning |
  |---|---|
  | `03` | `u8 bank`, `u8 prog`: sets the current entry |
  | `01` | `u8 prog`: sets the current program |
  | `02` | empty location: prog++ (if < 127) |
  | `04` | end of the whole list (+8 = 1); return |
  | `05` | end of this chunk; return |
  | any other value | the byte is the first char of a name: `UnreadBits(8)`, then `str16 name`, `u8 category` (≤ 15), push {entry, name, category}, prog++ |

* `CFlashData::HandleFlashDataDump` 0x119e22: when the flag is set and tag 05 ended the chunk, it requests again from the
  next entry. When tag 04 was seen, it stops.

**0x17 CMFlashDumpRequest** (read a flash entry as a file)
* [W] 0x11252 `(0x17,2,1)`: `u8 fileType(+8)`, `u8 bank(+0xc)`, `u8 prog(+0xd)`.
* Sent by `CFlashDumper::SendRequestPatchFromFlash` 0x1191da.

**0x18 CMFlashDumpMarker** (builder 0x1a1d82, dest 1)
* [R] 0x11eae: `u8 code`, `u8 x(→+0xc, unknown; probably fileType)`, `u8 bank`, `u8 prog`, then `str16 name` **only when code == 2**.
* The code maps to a status (UseOnSynth 0x11c96 → `CFlashDumper`):

  | code | status | handler |
  |---|---|---|
  | 0 | 1 | `HandleDumpDownOK` |
  | 2 | 3 | header with name; `IsTerminatingMolecule`=true |
  | 3 | 4 | `HandleMemoryFull` |
  | 4 | 5 | `HandleEntryEmpty` |
  | 5 | 6 | `HandleEntryCorrupt` |
  | 1 or ≥ 6 | 2 | `HandleMemoryProtected` |

**0x19 CMFlashDumpRawData** (Tpl id 25, dest 1, both directions)
* [W] 0x10d04 `(0x19,0,0)` / [R] 0x10f52:
  `u8 fileType(+0x18)`, `u8 bank(+0x1c)`, `u8 prog(+0x1d)`, `str16 name`, `u16be size(+8)`, `u8 version(+0x10)`,
  then `size` × u8 raw file bytes (`CFileData` serialized into a CFileByteStream).
* On read, size 0xffff means "no data".
* Sent by `CFlashDumper::SendFileToFlash` 0x119332. Received data goes to `CFlashDumper::HandleFlashRawDataDump`.

**0x37 CMDumpBubbleDestination**
* [W] 0x1785e `(0x37,2,1)`: `u8 (this+8 != 0)`, `u8 bank(+0xc)`, `u8 prog(+0xd)`, `str16 name(+0x10)`.
* The ctor 0x1ce294 only sets the name: +8 = 0 and the CFlashEntry is (0,0). **On the wire it is therefore always
  `37 00 00 00 <name>`**.
* It is the first molecule of an *upload* bubble:
  * `CSynth::CleanDirtyData` pushes it in front of `CPatch::GetCompletePatchDump` (patch upload into a slot; the name is
    the patch's flash name).
  * It also precedes `CMCompletePerformanceDump` (performance upload).
* Because it is the first molecule and has type 2, the whole upload bubble is a "request".

**0x3d CMDumpOneRequest**
* [W] 0x18908 `(0x3d,2,1)`, no payload.
* Sent from `CEditorApp::HandleMenuSelection` 0x14b838 (menu command 0x197) via `CSynth::SendUserBubble`.
  Verhue calls it S_MIDI_DUMP.

### 3.4 Patch requests (all no-payload or `u8 loc`)

Request sequence from `CPatch::CleanDirtyData` 0xd601e:
1. 0x3c / 0x20 / 0x28 via `SendPatchHeaderRequestBubble` 0xd557c:
   * 0x3c CMCompletePatchRequest when both header and name are dirty.
   * 0x20 CMPatchHeaderRequest when only the header is dirty.
   * 0x28 CMPatchNameRequest when only the name is dirty.
2. 0x58 ParameterPageFocusRequest.
3. 0x4b ModuleRequest (loc 1, then 0).
4. 0x53 CableRequest (1, 0).
5. 0x4c ModuleParamRequest (1, 0, then 2?).
6. 0x66 MorphMapRequest.
7. 0x63 KnobMapRequest.
8. 0x61 CtrlMapRequest.
9. 0x4e ModuleNameRequest (1, 0).
10. 0x4f ModuleCustomDataRequest (1, 0, …).
11. 0x68 CurrentNotesRequest (if dirty).
12. 0x6e TextpadRequest (if dirty).
13. Delayed patch-header send.
14. 0x71 PatchLoadRequest (VA=1, then FX=0).
15. **0x70 FlushBlink** (once, after the load info).
16. 0x2e ParamFocusRequest (once).

Request molecules:

| id | class | WriteStream | Initialize | payload |
|---|---|---|---|---|
| 0x3c | CMCompletePatchRequest | 0x16220 | (0x3c,2,1) | none. The synth answers with the full set of patch sections |
| 0x20 | CMPatchHeaderRequest | 0x16604 | (0x20,2,1) | none |
| 0x28 | CMPatchNameRequest | 0x16630 | (0x28,**0**,1) | none |
| 0x58 | CMParameterPageFocusRequest | 0x16278 | (0x58,2,1) | none |
| 0x4b | CMModuleRequest | 0x12bb6 | (0x4b,2,1) | `u8 loc` |
| 0x4c | CMModuleParamRequest | 0x12bfc | (0x4c,2,1) | `u8 loc`. The ctor also stores a byte at +0xc that is **not written** |
| 0x4e | CMModuleNameRequest | 0x12c42 | (0x4e,2,1) | `u8 loc`. +0xc is not written either |
| 0x4f | CMModuleCustomDataRequest | 0x12c88 | (0x4f,2,1) | `u8 loc` |
| 0x53 | CMCableRequest | 0xd210 | (0x53,2,1) | `u8 loc`. The ctor's u16 at +0xc is not written |
| 0x61 | CMCtrlMapRequest | 0x162a4 | | none |
| 0x63 | CMKnobMapRequest | 0x1624c | | none |
| 0x66 | CMMorphMapRequest | 0x162d0 | | none |
| 0x6e | CMTextpadRequest | 0x16568 | | none |
| 0x2e | CMParamFocusRequest | 0x1231c | | none |
| 0x68 | CMCurrentNotesRequest | 0x167ca | (0x68,**0**,1) | none, normal type |

**0x71 CMPatchLoadRequest**
* [W] 0x1651e `(0x71,2,1)`: `u8 (loc(+8) == 1)`.
* `CPatchLoad::SendLoadRequest` 0xee866 sends 1 (VA), then 0 (FX).

**0x72 CMPatchLoad** (builder 0x1a2174, dest 2; = DSP resource usage)
* [R] 0x163da, into SModuleResourceSpec at +0xc. 28 bytes after the id, in this order:

  | # | field | stored at |
  |---|---|---|
  | 1 | `u8 isVA` | bool → +8 |
  | 2 | `u14` | +0xc |
  | 3 | `u14` | +0xe |
  | 4 | `u8` | +0x10 |
  | 5 | `u14` (truncated to a byte) | +0x11 |
  | 6–11 | 6 × `u14` | +0x14, +0x16, +0x18, +0x1a, +0x1c, +0x1e |
  | 12 | `u14` | +0x20 |
  | 13 | `u16be hi`, `u16be lo` | 32-bit +0x24 |
  | 14 | `u14` | +0x28 |

* The individual counter names (cycles/memory/…) were not recovered. `SModuleResourceSpec` has no named accessors.

**0x70 CMFlushBlink**
* [W] 0x162fc `(0x70,0,1)`, no payload.
* Sent once per patch by `CPatch::CleanDirtyData` after the load/resource exchange. It probably asks the synth to
  (re)start or flush the LED/meter blink stream (0x39/0x3a) for that slot. `CSynth` calls `FlushLedBlinkData` locally when
  sessions change. Semantics inferred.

**0x55 CMSendCtrlSnap**
* [W] 0x1679e `(0x55,0,1)`, no payload.
* Sent by `CPatch::SendControllerSnapCommandBubble` 0xd8352: the synth sends the current values of all assigned
  MIDI controllers. UseOnPatch is empty.

### 3.5 Patch dumps and edits (dest 2)

**0x21 CMPatchHeaderDump** (Tpl id 33)
* → `CPatchHeaderData_11::WriteStream` 0x65564 / `ReadStream` 0x664ae.
* Header: `u8 0x21`, `u16be kPatchHeaderSize>>3`. This is a **fixed** length (runtime constant in bss 0x3133c0), not a CStreamSizer.
* The reader checks the id `'!'` and throws "File corrupt." otherwise.

**0x27 CMPatchNameDump** (builder 0x16758)
* [W] 0x166f8 `(0x27,0,1)` / [R] 0x167f6: `str16 name`. No length field.

**0x40 CMParamChange** (builder 0x1a209a)
* [W] 0x15008 `(0x40,1,1)` (**realtime**) / [R] 0x155b6: `u8 loc`, `u8 module`, `u8 param`, `u8 value`, `u8 variation`.
* The reader clamps loc to ≤ 2.

**0x43 CMMorphChange** (builder 0x1a20e0)
* [W] 0x150a4 `Initialize(0x43, this[0x14], 1)`. The type is 0 or 1, from the last ctor bool: realtime while dragging.
* Payload: `u8 loc(+0xc)`, `u8 module(+0x10)`, `u8 param(+0x11)`, `u8 morph(+0x12, 0..7; 8 = none)`,
  `u8 |range|`, `u8 negative(1 if range < 0)`, `u8 variation(+8)`.
* [R] 0x15360 is the same. Range = −|range| when the negative byte != 0.

**0x44 CMParamSettingCopy** (builder 0x1616a)
* [W] 0x160fe `(0x44,0,1)` / [R] 0x160c2: `u8 fromVariation`, `u8 toVariation`.

**0x6a CMParamSettingFocus** (builder 0x15fac)
* [W] 0x15f44 `(0x6a,0,1)`: `u8 variation`. [R] 0x15efe: `pad1`, `b7 variation`.

**0x2d CMParameterPageFocus** (builder 0x1a212e)
* [W] 0x15224 `(0x2d,0,1)` / [R] 0x1526a: `u8 page`.

**0x2f CMParamFocusDump** (builder 0x1a21ba)
* [W] 0x1220c `(0x2f,1,1)` (**realtime**): `u8 0`, `u8 loc(+0xc)`, `u8 module(+0x10)`, `u8 param(+0x11)`.
* [R] 0x12282: `u8 flag(→+8 bool)`, `u8 loc`, `pad1+b7 module`, `pad1+b7 param`.

**0x22 CMCtrlAssign** (builder 0x1a200e)
* [W] 0x14d1a `(0x22,0,0)` / [R] 0x15538: `u8 loc`, `u8 module`, `u8 param`, `pad1`, `b7 midiCC`.

**0x23 CMCtrlDeassign** (builder 0x1a2054)
* [W] 0x14e48 `(0x23,0,1)` / [R] 0x14ea4: `pad1`, `b7 midiCC`.

**0x25 CMKnobAssign** (builder 0x1a1f82)
* [W] 0x15628 `(0x25,0,0)` / [R] 0x15a0c:
  `u8 module(+0xc)`, `u8 param(+0xd)`, `b2 loc(+8)`, `b2 assignType(+0x10, EDeviceAssignType: reader clamps ≤ 1)`,
  pad4, `u16be knob(+0x14, 0..119, 120 = none)`.
* Per Verhue/pch2, assignType is probably "isLed": 0 = parameter, 1 = LED/button target. Inferred.

**0x26 CMKnobDeassign** (builder 0x1a1fc8)
* [W] 0x14bea `(0x26,0,0)` / [R] 0x153fa: `u16be knob`.

**0x30 CMModuleNew** (builder 0x1a1f3c)
* [W] 0x13056 `(0x30,0,0)` / [R] 0x132c4:
  1. `u8 moduleType(+0xd)`
  2. `u8 loc(+8)`
  3. `u8 moduleIndex(+0xc)`
  4. `u8 x/col(+0xe)`, `u8 y/row(+0xf)`
  5. `u8 color(+0x10)`
  6. `u8 bandwidth/uprate(+0x14)` (= CModule+0x40, same field as ABCode)
  7. `u8 isLocked(+0x18)` (mutator lock)
  8. N × `u8 partSelectorValue`. N = `CModuleFactory::GetPartSelectorCount(type)` on read, the vector size on write: the
     "mode" params.
  9. `str16 name`

**0x31 CMModuleRecolor**
* [W] 0x128c4 `(0x31,0,0)`: `u8 loc`, `u8 module`, `u8 color(+0x10)`.

**0x32 CMModuleDelete**
* [W] 0x12518 `(0x32,0,0)`: `u8 loc`, `u8 module`.

**0x33 CMModuleName**
* [W] 0x1278e `(0x33,0,0)`: `u8 loc`, `u8 module`, `str16 name`.

**0x34 CMModuleMove**
* [W] 0x1248c `(0x34,0,0)`: `u8 loc`, `u8 module`, `u8 x(+0xd)`, `u8 y(+0xe)`.

**0x2a CMModuleABCode**
* [W] 0x129f4 `(0x2a,0,0)`: `u8 loc`, `u8 module`, `u8 bandwidth(+0x10, NSFile_V11::EModuleBandWidth)`.
* Generated by `CPatchData::GenerateBandwidthChangeMolecules` 0xe5048. This is the audio/control-rate ("uprate") switch
  that happens when cables change.

**0x2b CMDSPPartChange**
* [W] 0x12b40 `(0x2b,0,0)`: `u8 loc`, `u8 module`, `u8 partSelectorIndex(+0xd)`, `u8 value(+0xe = CModule::GetPartSelectorValue)`.
* Sent by `CPanel::SelectorRequestChange` 0xc060a. It switches a module's DSP code variant, i.e. a "mode" parameter.
  The local handler is `CPatch::HandleModuleDSPGroup`.

**0x42 CMCustomData**
* [W] 0x15d68 `(0x42,0,0)`: `u8 loc`, `u8 module`, `u8 n`, then n × `u8` custom bytes (`NSFile_V7::CCustomData` vector).
* Verhue calls this S_SET_PARAM_LABEL. Clavia's "module custom data" is what Verhue calls "param labels/names",
  cf. 0x5b/0x4f.

**0x50 CMCableConnect**
* [W] 0xd46c `(0x50,0,0)`:
  1. `pad3`
  2. `b1 (partOfDump == 0)`
  3. `b1 (loc == VA)`
  4. `b3 color(+0xc)`
  5. `u8 fromModule(+0x10)`
  6. `u8 fromConnector | (fromIsOutput ? 0x40 : 0)`
  7. `u8 toModule(+0x13)`
  8. `u8 toConnector | (toIsOutput ? 0x40 : 0)`
* The flags byte is therefore 0x10 | VA<<3 | color for a normal edit.

**0x51 CMCableDelete**
* [W] 0xd2c6 `(0x51,0,0)`: `pad6`, `b1 (partOfDump == 0)`, `b1 (loc == VA)`, then from/to as in 0x50.
* The flags byte is 0x02 | VA.

**0x54 CMCableRecolor**
* [W] 0xd398 `(0x54,0,0)`: `pad4`, `b1 (loc == VA)`, `b3 color`, then from/to as in 0x50.

**0x74 CMBarbMapChangeNotify** (builder 0x1a2200, dest 2)
* [R] 0xdd58: id only. The knob map of the slot changed on the synth, so the editor re-requests 0x63.

**0x90 CMMutaLock**
* [W] 0x137fc `(0x90,0,0)`: `u8 loc(+8)`, `u8 module(+0xc)`, `u8 locked(+0xd)`.
* Built in `CPatch::InternalLockSelection` 0xd7fca as an undo/redo pair. It is not EditorOnly, so it is sent to the synth.
* It is not registered for receiving. The local apply is `CPatch::ReceiveMutaLockMolecule` 0xd3b76 → `CModule::Lock`.
* Note that the id ≥ 0x80 forces MSBUsage = 0 in Initialize.

### 3.6 Dump molecules that delegate to NSFile_V11 section classes

All of these are `TplMoleculeBuilder<…>` (dest 2, except 0x11 and 0x5f, which are dest 3). Their `WriteStream(CMIDIOutStream&)`
sets type 0 and msb 0, then calls the section writer. Their `ReadStream` calls the section reader. **The section writer
emits the 8-bit id itself, followed by a 16-bit big-endian length.**
* For CStreamSizer sections: `TagWord` 0x63b6c writes a 0x0000 placeholder. `WriteSizeAtTag` 0x63b9c zero-pads to a
  byte, then back-patches length = number of body bytes after the length word. Readers: `ReadSize` 0x63c06 and
  `VerifySize` 0x63c32 (which aligns first).
* The body format is the .pch2 section format: same classes, same bit packing.

| id | molecule (W / R addr) | section class (WriteStream / ReadStream) | header form | first body fields |
|---|---|---|---|---|
| 0x11 | CMPerformanceHeaderDump (0xdeba / 0xd662) | CPerformanceHeader_11 (0x64254 / 0x68e38) | id + u16 CStreamSizer | … slot names as str16 |
| 0x21 | CMPatchHeaderDump (0xdf06 / 0xd6b2) | CPatchHeaderData_11 (0x65564 / 0x664ae) | id + u16 = kPatchHeaderSize/8 (fixed) | b7,b7,b7,b7,b5,b5,… |
| 0x4a | CMModuleDump (0xdf52 / 0xd78c) | CModuleData_11 (0x67968 / 0x6774a) | id + u16 sizer | b2 loc, b8 count, per module b8 type, b8 idx, b7 col, b7 row, … |
| 0x4d | CMModuleParamDump (0xdf78 / 0xd904) | CModuleParamData_11 (0x68928 / 0x68620) | id + u16 sizer | b2 loc, b8 moduleCount, b8 variationCount, per module b8 idx, b7 paramCount, … |
| 0x52 | CMCableDump (0xdf2c / 0xd764) | CCableData_11 (0x64d9a / …) | id + u16 sizer | b2 loc, then PutUWord count (aligned → pad6), per cable b3 color, … |
| 0x5a | CMModuleNameDump (0xdf9e / 0xd92c; the CModuleNameData_11 base is at offset 0, the CMolecule at +0x20) | CModuleNameData_11 (0x660ac / 0x68b3e) | id + u16 sizer | … names as str16 |
| 0x5b | CMModuleCustomDataDump (0xdfbc / 0xda9c) | CModuleCustomData_11 (0x64c1a / 0x66b3a) | id + u16 sizer | Verhue "param names/labels" |
| 0x5f | CMPerformanceKnobMapDump (0xdee0 / 0xd6a0) | CKnobMapData_11 mode 1 (0x69642) | id + u16 sizer | 120 knob slots |
| 0x60 | CMCtrlMapDump (0xe008 / 0xdaec) | CCtrlMapData_11 (0x6882a / 0x6a0f6) | id + u16 sizer | |
| 0x62 | CMKnobMapDump (0xdfe2 / 0xdac4) | CKnobMapData_11 mode 0 (0x69642) | id + u16 sizer | `PutUWord(0x78)` = knob count 120 … |
| 0x65 | CMMorphMapDump (0xe02e / 0xdd20) | CMorphMapData_11 (0x69c58 / 0x69326) | id + u16 sizer | b8 variations, b4 8, 8×b2 morph modes, … |
| 0x69 | CMCurrentNotesDump (0xddd6 / 0xddfc) | CCurrentVoiceData_11 (0x64aca / 0x672c4) | id + u16 sizer | b7,b7,b7 last note, b5 count−1, per note b7,b7,b7 |
| 0x6f | CMNotePadDump (0xde94 / 0xd5a2) | CTextpadData_11 (0x64632 / 0x68d80) | id + u16 **explicit** (= min(strlen, 0x400)) | then exactly that many text bytes, **no NUL** (PutString with max = len) |

Non-section dumps with their own id (no length field):
* 0x03 SynthDataDump (`CSynthMap`, which writes 0x03 itself).
* 0x27 PatchNameDump.
* 0x29 PerformanceNameDump.

**Patch upload bubble** (`CPatch::GetCompletePatchDump` 0xd895a → `FileToBubble` 0xd4d76). The sections come from
`CPatchFile_13::WriteMolecules` 0x51e7c, in this order, preceded by 0x37:

1. 0x21
2. 0x4a ×2 (two contexts, probably VA then FX: object offsets +0x38 / +0x48)
3. 0x69
4. 0x52 ×2
5. 0x4d ×3 (VA, FX, patch-settings)
6. 0x65
7. 0x62
8. 0x60
9. 0x5b ×3
10. 0x5a ×2
11. 0x6f

Every id is re-parsed through the factory. All of them are registered; an unregistered id would make that loop spin
forever.

### 3.7 Editor-only / composite molecules (no id)

* `CMMutaParams` (UseOnPatch 0x13582 → `CMutaSynthData::ReceiveMutaParamsMolecule`): EditorOnly=1.
* `CMMutaParentCount` (0x13616): EditorOnly=1.
* `CMMutaSetMarker` (0x139b6 → `CMutaSynthData::SetFocus/ClearFocus`): EditorOnly=1.
* `CMMutaClearIndivid` (0x13740): EditorOnly=1.
* `CMMutaParamDump`: [W] 0x13a18. Only when mode(+0x11c) == 2 (and then EditorOnly=false) does it write:
  * three `CMModuleParamDump` sections (0x4d) built from its CModuleParamData<double> for contexts 2, 1, 0, with values
    floored;
  * then its embedded `CMMorphMapDump` (0x65).

  In other modes it is editor-only. UseOnPatch → `CPatch::HandleMutaParamDumpMolecule`.
* `CMCableABC` (base of 0x50/0x51/0x54): {+8 EContext, +0xc CCableSegmentSpec, +0x18 EPartOfDump (Connect/Delete)}.
* `CMModuleMoleculeABC` (base of 0x2a/0x2b/0x31/0x32/0x34/0x30): {+8 EContext (default 1), +0xc module index}.
  When built from a CModule: +0x34 → context, +0x32 → index.

---------------------------------------------------------------------------------------------------------

## 4. Synchronisation flow observed (`CSynth::CleanDirtyData` 0x11f394, `CleanDirtySynthData` 0x11e7aa)

1. Send `7d 01` (CMSynthUnlockEditorSync(true)) as a user bubble.
2. Synth-level work, one item per idle pass:
   * `35 04` (performance session number);
   * `02` (synth settings) → 0x03;
   * `81` (MIDI learn) → 0x80;
   * pending virtual-keyboard `56 00/01 nn`.
3. Performance: either upload (`37 00 00 00 name` + `1a …`, which marks every slot dirty), or send 0x03 with only perfMode changed.
4. Per slot:
   * when the slot has no patch yet, upload: `37 …` + the patch sections;
   * otherwise send `35 slot`, then run `CPatch::CleanDirtyData` (the request list in §3.4).
5. Then:
   * voice data requests (0x04 / 0x68?);
   * clock requests (0x3b);
   * perf knob map (0x5e);
   * flash dumper / flash name lists (0x14).
6. Finally send `7d 00`.

---------------------------------------------------------------------------------------------------------

## 5. Uncertainties

* **EContext 0 = FX / 1 = VA / 2 = patch settings**: the three values and the default (1) are proven. The FX/VA naming is
  from Verhue and pch2 knowledge, not from strings in this binary.
* **EFlashFileType 0 = patch / 1 = performance**: inferred from `CSynth::GetFlashData` and from the usage dump going
  to the +0x1c list.
* **Semantics that are inferred, not proven**:
  * 0x70 FlushBlink;
  * 0x3e second byte;
  * 0x5d flag;
  * 0x16 first flag byte (stored at +9; drives "request next chunk");
  * 0x18 second byte;
  * 0x2f first byte (written as 0, read as a bool);
  * 0x3f params 0 and 2 (run/stop, keyboard range).
* **0x72 CMPatchLoad**: the counter names are unknown. The SModuleResourceSpec fields are summed by `operator+=`, so
  they are additive resource counts (DSP cycles/memory per DSP), but the individual names were not recovered.
* **0x3f with scope ≤ 3** writes no value byte, both on read and on write. This is odd; no editor path that uses it was found.
* **CMModuleParamRequest / CMModuleNameRequest** store a second byte (+0xc) that is never serialized. **CMCableRequest**
  stores a u16 (+0xc) that is never serialized. I verified this in the disassembly at 0x12bfc, 0x12c42 and 0xd210.
* **`CMIDIOutStream::Initialize`** forces EMSBUsage = 0 for ids ≥ 0x80 (0x81, 0x90). This only matters for the MIDI/SysEx
  transport.
* **0x4a context order** in the upload (VA before FX) is a guess based on object order. The other agent's .pch2 notes
  should confirm it.
* **Verhue names that disagree with Clavia semantics**:

  | id | Verhue name | Clavia molecule |
  |---|---|---|
  | 0x13 | R_LIST_NAMES | flash usage |
  | 0x3e | S_SET_PARAM_MODE | performance mode change |
  | 0x42 | S_SET_PARAM_LABEL | module custom data |
  | 0x5b | C_PARAM_NAMES | module custom data |
  | 0x4f | Q_PARAM_NAMES | module custom data request |
  | 0x71 / 0x72 | Q_/R_RESOURCES_USED | patch load (resource usage) |
