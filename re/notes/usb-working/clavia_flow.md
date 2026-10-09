# Clavia G2 editor (Mac v1.62, i386 slice): USB connection flow, LED stream, MIDI, errors

Source: Ghidra decomp in `re/out/decomp/*.c`, plus direct reads of the i386 Mach-O slice
(`nm`, `otool -Iv`, `objdump`, a small `__eh_frame`/LSDA parser to recover C++ catch clauses).
All addresses are i386 VMAs. **PROVEN** = read directly from code or data. **INFERRED** = deduced, not shown by code.

Correction to the brief: `CSynthPort::CTimeoutManager::SuggestAction` (0x103f56) **never returns 2**.
It returns 3 when it is waiting for a reply and 10 000 ms have passed since max(lastSend, lastReceive).
It returns 1 when it is not waiting (`now - lastReceive >= 0`, so in practice always), and 0 otherwise.
The version request is sent from `CSynthPort::CleanDirtyData` (0x104b7c) whenever flag `port+0x400` ("need version") is set.
The constructor sets that flag, and so does `MajorError`.

---------------------------------------------------------------------------------------------------

## 0. Wire framing (needed to read the rest)

### 0.1 Editor → synth (bulk OUT). `CSynthPortUSB::BuildOutputMessage` 0x10691a (PROVEN)
```
[0..1] total length, big-endian (includes these 2 bytes and the CRC)
[2]    0x01                      (0x80 for the version request)
[3]    slot&7 | 0x20 BubbleBegin | 0x08 BubbleEnd | 0x10 RealTime
[4]    session number (0..0x3f)  OR  0x40 | moleculeCount   ("void session")
[5..]  molecule bytes (one or more molecules concatenated)
[n-2..n-1] CRC16 big-endian over bytes [2 .. n-3]
```
* The version request is exactly `00 05 80 crcHi crcLo`. `CSynthPort::SendVersionRequest` (0x10496c) builds a header of
  type 2 with no data, and `BuildOutputMessage` writes only the single 0x80 byte.
* CRC: `CRC_Get16` 0x14fd40 with a table from `CRC_Init` 0x14fce0. The polynomial is 0x1021, MSB-first, init 0 (CRC-16/XMODEM).
* Slot field: 0..3 = slots A..D, **4 = synth/performance level** (the default of `CBubble::CBubble` 0xca6a).
* Constants in `__const` 0x1e5b5f.. (PROVEN): `kUSBMessageVoidSessionFlag=0x40`, `kUSBMessageDebug=0x80`,
  `kUSBMessageResponseFlag=0x08`, `kUSBMessageRealTimeFlag=0x10`, `kUSBMessageBubbleEndFlag=0x08`,
  `kUSBMessageBubbleBeginFlag=0x20`, `kUSBMessageChannelMask=0x07`.
* The internal "request" type (`COutputMessage::IsRequest`, header[7]==0x10) is **not** put on the USB wire.
* `CBubblePort::GenerateMessages` 0x11828a splits a bubble into messages of at most 0xffff bytes.
  The first message carries Begin and the last carries End, so a normal single-message bubble has byte[3] = `0x28|slot`.
* `CBubblePort::GenerateRealtimeMessages` 0x118536 builds one message per molecule with Begin+End+RealTime,
  so byte[3] = `0x38|slot`. The session byte is the slot session.

### 0.2 Synth → editor. `CSynthPortUSB::USBDataReceivedCallback` 0x107194, `InterruptHandleData` 0x106f72, `BuildHeaderAndData` 0x106e92 (PROVEN)
The interrupt IN pipe is read 16 bytes at a time (`ReadPipeAsync(..., 0x10, InterruptPipeCallback)`).
Byte 0 & 3 of each interrupt packet selects the case:
* `1` "extended": length = BE16 in bytes 1..2. The payload is fetched from the **bulk IN** pipe with
  `CUSBManager::RxData(port, buf, len, 5000 ms)` (0x108a7e).
* `2` "embedded": length = `b0>>4`. The payload is in bytes 1.. of the same 16-byte packet.
* `0`: length = BE16 in bytes 1..2, payload inline from byte 3 (this path is not otherwise exercised).

The payload (the "message") has no length prefix:
```
[0] 0x80 => version reply ; anything else => normal message (value not checked)
[1] slot = b1 & 7 ; response flag = b1 & 0x08 (kUSBMessageResponseFlag)
[2] session byte (bit 0x40 = void session, low 6 bits = session number)
[3..n-3] molecules  (first byte = molecule id)
[n-2..n-1] CRC16 BE over [0..n-3]
```
* A bad CRC causes the message to be **silently dropped** (0x106f72). No status is set.
* Classification by id byte [3]: 0x39/0x3a is blink, 0x38/0x1f is release.
  These are `kMesBlinkDualBit=0x39`, `kMesBlinkMultiBit=0x3a`, `kMesPatchRelease=0x38`, `kMesPerformanceRelease=0x1f`,
  `kMesException=0x7e`, `kMesAck=0x7f` (`__const` 0x1e5a44..49).
* `CBubblePort::AddToResultBubble` 0x117e8a parses molecules in sequence:
  * 0x7e throws `XMHostMidi(code = next byte)`.
  * 0x7f (ack) ends parsing.
  * An unknown id throws `XMUnregisteredMolecule`.

### 0.3 Version reply layout. `CSynthPort::CheckVersionMessage` 0x104d2c and `CSynthInfo(SVersionMessage)` 0x103e56 (PROVEN)
Offsets are counted from the 0x80 byte:

| off | meaning |
|---|---|
| 0 | 0x80 |
| 1 | must be 0x0a. Otherwise the editor uses model=0, mode=2 and the result is status 3 "Unsupported Model". |
| 2 | model: 0=G2 (enum 5), 1=G2 61-key/G2X (6, `CModularG2V`), 2=G2 Rack (7), 3=G2 Engine (8), 4=Native (4), else 0 |
| 3 | mode: 0=normal, 1=update mode, else 2 |
| 4-5 | BE16, stored at port+0x412 (meaning unknown) |
| 6-7 | BE16 firmware version, shown as `v/100 . v%100` with 2 digits, stored at +0x40c |
| 8-9 | BE16 **must be 0x0012** (protocol version), +0x410 |
| 26-29 | BE32, stored at +0x414 |
| 32-33 | BE16, stored at +0x40e |

Acceptance rule (also `CSynthPort::IsSynthAccepted` 0x1038b8): model ∈ {G2, G2-61, Engine}, mode == 0, and word@8 == 0x12.
**The G2 Rack (model byte 2) is rejected.** `CreateSynth` 0x103a2c has no case 7.

If not accepted, `StopUsingPort` is called with:
* model 0 → status 3 (Unsupported Model)
* mode != 0 → status 8 (Update Mode)
* otherwise → status 4 (Version mismatch).

---------------------------------------------------------------------------------------------------

## A. Connection and initialisation sequence

### A.1 USB arrival → port object (PROVEN)
1. `CPortManager::CPortManager` 0x102ff2 creates `CUSBManager(listener=this, VID 0x0ffc, PID 2, interrupt=1, maxPorts=4)` (0x10925a).
   `CUSBManager::Init` 0x108eda then:
   * matches `IOServiceMatching("IOUSBDevice")` plus `idVendor`/`idProduct`;
   * installs `IOServiceFirstMatch` and `IOServiceTerminate` notifications;
   * registers `IORegisterForSystemPower`.
   `UpdateDeviceDriverList` 0x10947a drains the iterators so devices already present are picked up.
2. `DeviceAddedCallback` 0x10943e calls `CUSBManager::LowLevel_NewDevice(service)` 0x10937a.
   This takes the **first CUSBPort entry with no open handle** (index 0..3 = port number) and calls
   `CPortManager::USBDeviceAddedCallback(i)` 0x102284. That calls `CreatePort` 0x1021fa, which only acts for `Port::kUSB`.
   `CreateUSBPort` 0x102064 then constructs `CSynthPortUSB(port i)` 0x106cd2.
3. `CSynthPort::CSynthPort` 0x105688 sets:
   * status=1 "Looking..."
   * `+0x400` needVersion=1
   * 4× `CLedBlinkData` at +10+slot*0xf4
   * waiting `+0x3ec`=0
   * retry counter `+0x3ee`=0
4. `CUSBManager::OpenPort` 0x10861a constructs `CMacUsbHandle(service)` 0x108364, which calls `OpenDevice` 0x107e9c.
   `OpenDevice` goes through these IOUSBDeviceInterface calls (vtable offsets mapped from IOUSBLib, INFERRED naming):
   1. GetLocationID (+0x50).
   2. **USBDeviceOpen (+0x20), retried up to 20× with `WaitForMS(1000)` + `IdleOtherApps` while it returns
      `0xe00002c5` kIOReturnExclusiveAccess.** Any other error aborts.
   3. **ResetDevice (+0x64)**.
   4. GetNumberOfConfigurations (+0x4c, must be >0), GetConfigurationDescriptorPtr(+0x54), SetConfiguration(+0x5c).
   5. CreateInterfaceIterator(+0x70).
   6. For every interface, `OpenInterface` 0x107b5a: **USBInterfaceOpenSeize (+0xb0)**, GetNumEndpoints(+0x4c),
      then `InitPipes` 0x107aca using GetPipeProperties(+0x68). Pipes are classified only by transfer type:
      bulk-IN goes to +9, bulk-OUT to +10, anything else to the interrupt pipe +8.
   7. Async event source added to the run loop, or an MP task.

   `OpenPort` then arms `ReadPipeAsync(interrupt, 16 bytes)`.
5. `InterruptPipeCallback` 0x108c56 (only when result==0) calls `CUSBManager::HandleInterrupt` 0x108b44.
   That calls the port callback, **sleeps 10 ms (`MPDelayUntil`)**, and re-arms the 16-byte read.
   The interrupt pipe is therefore serviced at most about every 10 ms.

### A.2 Handshake (PROVEN)
* `CEditorApp::Idle` 0x1496de calls `CPortManager::Idle` 0x102678, which services **one port per call, round-robin** (`+0x24`).
  That calls `CSynthPort::Idle` 0x1054b4:
  1. If a fatal flag was set by the interrupt path → `MajorError(12)`.
  2. `HandleReceivedData` 0x105102 drains the queue for at most 500 ms.
  3. `SuggestAction`: 1 → `CleanDirtyData`; 3 → timeout handling.
* First pass: `CSynthPort::CleanDirtyData` 0x104b7c sees `+0x400` and calls `SendVersionRequest` 0x10496c
  (`00 05 80 crc`, waiting=1, `+0x3f4`=1).
* Reply 0x80 → `HandleReceivedData` clears waiting and calls `CheckVersionMessage`.
  If accepted, `StartUsingPort` 0x104c54 clears the 4 LED caches, calls `CreateSynth` 0x103a2c, then `SetStatus(13)`.
  `HandleReceivedData` then calls `CSynth::SendPendingMessages` (normally nothing is queued yet).
  * `CModularG2::CModularG2` 0x11eed6 calls `CSynth(port, slots=4, patchBanks=0x20, perfBanks=8)`.
  * `CModularG2V` 0x11efae and `CEngine` 0x11ef5a only change the vtable. G2V overrides `HasGlobalModulationWheels` to true.
* No reply within 10 s: status 1 or 2 → `StopUsingPort(2 "Still looking...")`.
  This clears waiting, so the next Idle sends the version request again. The editor **retries every 10 s forever**.
* A spontaneous version message while running (status 13):
  * identical info → ignored;
  * different info → `StopUsingPort(5 "New device found")`, which is an error status, so the port is destroyed (see D).

### A.3 CSynth construction: what is "dirty" at start. `CSynth::CSynth` 0x11e964 (PROVEN)

| field | init | meaning / cleared by |
|---|---|---|
| `+8` editor-sync state | 0 | 0 → send `7D 01`; 1 = syncing; 2 = done (sends `7D 00`) |
| `+0x10` slot array, 8 bytes/slot | state=1, session=0, **sessionUnknown=1**, lastMidiCC=0x78 | cleared by 0x36 / 0x38 / patch bubble |
| `+0x14` perf state | 1 | 0 = editor must download its performance to the synth |
| `+0x18`, `+0x19` | perf session = 0, **perfSessionUnknown = 1** | 0x36 (slot 4) / 0x1f |
| `+0x30` synth data dirty | 1 | 0x03 SynthDataDump `HandleSynthDataDump` 0x11d2a2 |
| `+0x3c` voices dirty | 1 | 0x05 `HandleNumberOfVoicesDump` 0x11d194 |
| `+0x44` clock dirty | 1 | 0x5d `HandleSysClockInfoDump` 0x11d1be |
| `+0x45` MIDI-learn dirty | 1 | 0x80 `HandleMidiLearn` 0x11d382 |
| `+0x50..0x54` virtual keyboard | none, note 60 | see C |
| 2× `CFlashData` `+0xe` | 1 | flash directory (0x14 requests) |

The constructor then picks one of three performance paths:
* **Typical case** (no unconnected document on top): `CPerformanceManager::CreateInitialPerformanceForSynth` 0xfe600.
  This makes an empty performance with 4 empty `CPatch` objects, then `ConnectSynth(perf, false)`.
  The editor **reads everything from the synth**.
* **The topmost open patch is not connected** to a synth: `CreateDownloadPerformanceFromPatch` 0xfe478.
  That patch is **sent to the synth, slot A**.
* **The top performance exists and has no synth:** `CPerformance::ConnectSynth(top, true)`.
  The **editor performance is sent to the synth**.

In the last case `CSynth::ConnectPerformance` 0x11dab2 sets perf state=0 and marks every slot's session as unknown.

### A.4 The send discipline (PROVEN)
* `CBubblePort` (0x117b82..) keeps three queues: user `+0x2c`, cleaner `+0x34`, realtime `+0x3c`.
* `SendMessages` 0x117d2c flushes **all realtime messages immediately** (no reply awaited). Then, only if the port is not waiting:
  * it sends the front cleaner message (saved in `+0x44` for `ReSendMessages`, state 2); or, if there is none,
  * it sends the front user message (state 1).
* `CSynthPort::SendMessage` 0x104414 sets waiting=1 and the send timestamp. So there is **exactly one outstanding non-realtime message**.
* A reply with the response flag goes to `CSynth::HandleRequestedData` 0x11faec, then `CBubblePort::HandleRequestedData` 0x11809c.
  That accumulates the reply. When the queue for that state is empty, the bubble is complete.
  `UnLockUser` is called for user bubbles. The result goes to `CSynth::RouteBubble` 0x11f940.
* `CSynth::CleanDirtyData` 0x11f394 is called again only when the port is not waiting.
  **Each call emits at most one bubble.** The whole start-up is therefore a strict request/reply chain in the order below.
* User bubbles call `CBubblePort::LockUser` (UI busy). Cleaner bubbles do not.
* `CSynth::SendUserBubble`/`SendCleanerBubble` (0x11ddee/0x11d464) force a **void session** (byte[4] = 0x40|count) with slot 4.
* Performance bubbles carry the perf session (`SendPerformance*Bubble` 0x11d4a8/0x11d54a).
* Patch bubbles carry slot = patch slot and that slot's session (`SendPatch*Bubble` 0x11d860/0x11d90a/0x11d972).

### A.5 Order of messages after the version handshake (typical "read from synth" case)
Bytes are shown from offset [2] of the USB frame. `S`=slot, `ps`=perf session, `ss`=slot session.

| # | sent | where it comes from | reply expected / effect |
|---|---|---|---|
| 1 | `01 2C 41 7D 01` SynthUnlockEditorSync(true), user | `CSynth::CleanDirtyData` 0x11f394, state +8 0→1 | response (ack 0x7f presumably; INFERRED) |
| 2 | `01 2C 41 35 04` SessionNumberRequest(perf), cleaner | `CleanDirtySynthData` 0x11e7aa (+0x19) | `36 04 ps` → `HandleSessionNumberDump` 0x11dcd8 sets +0x18 and clears +0x19 |
| 3 | `01 2C 41 02` SynthDataRequest | +0x30 (`SendSynthDataRequests` 0x11e42a) | `03 …` SynthDataDump |
| 4 | `01 2C 41 81` MidiLearnRequest | +0x45 (0x11e374) | `80 slot cc` |
| 5 | (only if a virtual-keyboard note is pending: `56 …`, see C) | | |
| 6 | `01 2C ps 10` PerformanceHeaderRequest | `CPerformance::CleanDirtyData` 0x100e5a (perf+0x78 = header dirty, set by `CPerformanceHeader()` 0x1a9a1a) | `11 …` PerformanceHeaderDump (dest = performance) |
| 7 | `01 2C ps 59` GlobalParameterPageFocusRequest | perf+0x72 (=1 in `CPerformance()` 0x1014f8) | 0x1e GlobalParameterPageFocus |
| 8 | `01 2C ps 08` SlotFocusRequest. Only for the lowest-numbered port with a synth while no focus exists (`GetSlotFocusUpdateAction` 0x102df4 → 1). Other ports send `09 04` (user bubble, "no focus"). | perf slot-focus logic | `09 …` → `HandleSlotFocusDump` 0x100582 → `FocusedSlotChangedFromSynth` 0x1025da |
| 9 | (perf+0x5d pending slot selection: `07`+`0F`; perf+0x54/0x5c pending header/name set: only after a UI change) | | |
| 10+ | **For each slot S = 0..3**, in order: | `CSynth::CleanDirtyData` slot loop | |
| a | `01 2C 41 35 0S` SessionNumberRequest(S) | slot sessionUnknown=1 | `36 0S ss`. Session stored, flag cleared, LED cache of S invalidated |
| b | `01 2(8+S) ss 3C` CompletePatchRequest (header **and** name dirty; only header → `20`, only name → `28`) | `CPatch::SendPatchHeaderRequestBubble` 0xd557c (+0x84, +0x7c set in `CPatch::CPatch`) | a multi-molecule patch dump bubble (0x21 header, 0x4a modules, 0x52 cables, 0x4d params, 0x65 morph, 0x62 knob, 0x60 ctrl, 0x5a names, 0x5b custom, 0x27 name…; exact content INFERRED from the registered builders with destination 2) |
| c | `58` ParameterPageFocusRequest | +0xad | 0x2d |
| d | `4B/53/4C/66/63/61/4E/4F` module / cable / param / morph / knob / ctrl / name / custom requests, **only if still dirty** after b (VA context 1 first, then FX 0) | 0xd53b8, 0xd5296, 0xd5160, CMorphMap / CKnobMap / CCtrlMap::SendRequestBubble, 0xd4f1e, 0xd5040 | matching dumps |
| e | `68` CurrentNotesRequest | +0x9c (=1 in ctor) | `69` CurrentNotesDump |
| f | `6E` TextpadRequest | +0x60 (=1 in ctor) | `6F` NotePadDump |
| g | (`21 …` delayed PatchHeader send if the editor changed it, +0x90 / time +0x98) | | |
| h | `71 01` then `71 00` PatchLoadRequest | `CPatchLoad::SendLoadRequest` 0xee866 | `72 …` CMPatchLoad (DSP load; both must arrive before LEDs are accepted) |
| i | `70` FlushBlink (once per load report, `CPatch[1]`) | `CPatch::CleanDirtyData` 0xd601e | response |
| j | `2E` ParamFocusRequest (once, `CPatch[8]`) | same | `2F` |
| 11 | `01 2C 41 04` NumberOfVoicesRequest | +0x3c (0x11e2be) | `05 v0 v1 v2 v3` |
| 12 | `01 2C 41 3B` ClockInfoRequest | +0x44 (0x11e208) | `5D int/ext word` |
| 13 | `01 2C ps 5E` PerfKnobMapRequest | perf+0xdc (`CPerfKnobMap()` 0xff136 sets dirty) | `5F` |
| 14 | `CFlashDumper::CleanDirtyData` 0x11c8d0 | idle unless a user flash transfer is running | |
| 15 | `01 2C 41 14 …` FlashDataRequest, repeated (patch banks, then performance banks) | `CFlashData::CleanDirtyData` 0x1194aa (+0xe=1 in ctor 0x119656) | `16 …` FlashDataDump |
| 16 | `BoostIdle(false)`, then `01 2C 41 7D 00` SynthUnlockEditorSync(false), user | `CSynth::CleanDirtyData` (+8 1→2) | response. Initial sync done. |

After that `CSynth::CleanDirtyData` returns 0. **Nothing is polled periodically**: there is no keep-alive.
Traffic resumes only when a flag becomes dirty through UI edits or synth-initiated messages.

**Editor → synth download path** (perf state +0x14 == 0): after steps 1–4, the editor sends a user bubble
`37 DumpBubbleDestination(perf name)` + `1A CompletePerformanceDump` (0x11f394).
* If the synth's performance-mode flag differs, a `03 SynthDataDump` with perf mode toggled is sent first.
* After the dump, perf state=1 and all session flags are set again, so steps 2 and 10a are redone.
* A slot whose state is 0 (`CSynth::DownloadPatch` 0x11dc62) gets user bubble `37 dest + complete patch dump`
  in slot S with a void session.

### A.6 Session numbers (PROVEN)
* There is one 6-bit session per slot (`slotData+4`), plus a flag "unknown" (`+5`), plus one perf session (`CSynth+0x18`/`+0x19`).
  `CSynth::CSlotData::SetSessionNumber` 0x11d066 has no callers. The fields are written directly in:
  * `HandleSessionNumberDump` 0x11dcd8: `36 slot session`. Slot 4 = perf.
  * `HandleSessionPatchRelease` 0x11df1c: `38 slot session`. ReadStream 0x1794a reads slot first, then session.
  * `HandlePerformanceRelease` 0x11d7ac: `1F session`.
  * `RouteBubble` 0x11f940: an incoming patch bubble whose header session ≠ the stored one **adopts the new number**
    and recreates the slot via `CPatchManager::CreateEmptyUploadPatch` 0xefcfa (full re-read with `3C`).
* **The editor never increments a session.** The synth owns them. The editor copies the stored value into
  byte[4] of every patch or performance bubble (`CBubble::SetSessionNumber` 0xc990).
* Incoming routing (`RouteBubble`):
  * patch-destination bubbles are used only if the slot session is known and equal;
  * performance-destination bubbles only if the perf session is known and equal;
  * synth-destination bubbles if they are void-session or the perf session matches.
* **0x38 SessionPatchRelease** (synth loaded a new patch in a slot): store the session, invalidate the slot's LED cache,
  create an empty patch for that slot. Its dirty flags then trigger steps 10b–j for that slot only.
  Also `BoostIdle`, and editor-sync state=1, so `7D 00` is sent when clean again.
* **0x1F PerformanceRelease**: perf session stored, then `CreateInitialPerformanceForSynth`. A full re-read follows.
  `RequestEditorSyncLock(4)` marks every slot session unknown if the state was 2. State becomes 1.

### A.7 Focus / slot selection (PROVEN)
* `CPortManager` has `+0x25 CFocusedSlot`: [0] slot, [1] focused port (4 = none), [2..5] per-port "notify" flags.
  It also has `+0x2b` "focus not established" (initially 1).
* UI slot click (`CPatch::SetTrulyFocused`, `CTBWindow::HandleChangeRequests`, `CSynth::DownloadPatch`) calls
  `CPortManager::SetFocusedSlot` 0x10264a. That flags both the old and the new port.
  Each flagged port then sends `09` SlotFocusDump as a performance user bubble: slot, or 4 for ports that lost focus.
  Payload byte = 5 zero bits, 1 bit "none", 2 bits slot, so it equals 0..3 or 0x04.
* Synth-initiated `09` → `HandleSlotFocusDump` 0x100582 → `FocusedSlotChangedFromSynth` (no echo back), and opens/selects the patch view.
* `CSynthPort::DisposeSynth` 0x103b4c → `CPortManager::DefocusPort` 0x102e7c moves focus to the first enabled slot of another active port.
* Slot enable/keyboard: `CPerformance::SetSlotSelection` 0x100b7e sends a user bubble `07` SlotSelectionDump
  (4 zero bits, then 1 bit per slot A..D, MSB-first, so A=0x08…D=0x01). If needed it adds `0F` KeyboardFocusDump (same layout).
  * `SetSlotSelectionWhenPossible` 0x100e02 defers this while `IsGlobalSlotLocked`.
  * Incoming `07`/`0F` update the perf header (0x100470, 0xfffd8).
  * `06` SlotSelectionRequest exists but has no caller.

---------------------------------------------------------------------------------------------------

## B. LED / VU streams

### B.1 Reception
* Unsolicited (PROVEN). The editor never writes 0x39/0x3a (no `Initialize(…,0x39/0x3a)` anywhere) and has no LED timer.
  The synth pushes them. They are classified by id in `BuildHeaderAndData`.
* Gating:
  * `CSynthPortUSB::InterruptHandleData` 0x106f72 handles a blink message **directly in the interrupt path** only if a synth exists,
    the slot's patch is connected, and **both** `CPatchLoad::IsLoadReported(1)` and `(0)` are true (i.e. both `72` replies were received).
  * If the patch is connected but the load is not yet reported, the message is queued, and `HandleReceivedData` re-checks then drops it.
  * If the patch is not connected, the message is dropped.
  * Blink messages refresh `lastReceive` (0x3e4).
* `CSynthPort::InterruptHandleBlinkData` 0x1041ea (PROVEN). `d` = message data starting at the id; slot = header byte1&7, must be < nSlots.
  * **0x39** (dual / 2-bit LEDs): `d[1]` = start index. Values are 2 bits each, **4 per byte, LSB-first**:
    bits1:0 → LED i, 3:2 → i+1, 5:4 → i+2, 7:6 → i+3, then next byte, starting at `d[2]`.
    Indices start..39 are processed (capped at 40), **independent of the message length** (no length check).
  * **0x3a** (multi / VU): `d[1]` = start index, then `*(ushort*)(d+2+2k)` for k = 0..(39-start).
    Words are read in **host order without swap**, i.e. little-endian on the i386 build. Byte order on the wire is INFERRED.
* Storage: `CLedBlinkData` (0x101d36..) holds 40 dual u16 @0, 40 multi u16 @0x50, changed-flags @0xa0/@0xc8, counts @0xf0/0xf2.
  There are 4 per port (one per slot).
  * `Clear()` sets every value to 0xffff ("invalid", so the next value always counts as a change).
  * `ZeroData()` sets everything to 0 and marks it changed (used for display reset on disconnect).
  * **So: 40 dual + 40 multi LEDs per slot (0x28), PROVEN.**
* Invalidate (`FlushLedBlinkData` → `CLedBlinkData::Clear`) happens on: session dump (0x11dcd8), patch release (0x11df1c),
  `DownloadPatch` (0x11dc62), and `CPatch::SetDspReportedLoad` 0xd4a74 (each `72`).
  The last one also clears `CPatch[1]`, so **FlushBlink `70` is re-sent after each new load report**.

### B.2 FlushBlink (0x70)
* Payload is just the id (`CMFlushBlink::WriteStream` 0x162fc, command type).
* It is sent as a patch cleaner bubble in slot S with session ss, once after both PatchLoad replies (step 10i), and again after a reload.
* Meaning (INFERRED): it asks the synth to reset its LED change tracking so a full LED state follows. This matches the editor
  invalidating its cache to 0xffff at the same time.

### B.3 Index → module mapping. `CPatch::UpdateBlink` 0xdb45e, `CPatchBackground::UpdateBlink` 0xdf744, `AddBlinkModule` 0xe05b2, `InsertLedModule` 0xe0560, `CPanel::Blink` 0xc2538 (PROVEN)
* Called from `CPatch::Idle` 0xdb56c, i.e. the app idle loop. There is no dedicated rate.
* Display happens only when `CPatch::IsAcceptingLEDBlink` 0xd467e is true: patch view open and the synth's port is the focused port.
* Two backgrounds are processed: **VA (GetBkg(1)) first, then FX (GetBkg(0))**. The index continues across them (running counters).
* Each background keeps two `vector<CPanel*>`: dual (+0x80) and multi (+0x74). They are **sorted by module ID** (`CLessModuleID`).
  Each module contributes one entry per LED *group*: a single-LED group goes to the dual list, a multi-LED or VU group to the multi list
  (`CPanel::GetLedGroupCnt` 0xbe93c, `IsMutliLedGroup` 0xbe998; group id at `led+0x62`, VU flag `led+0x60`).
* The per-module group index is passed to `panel->Blink(group, value)`:
  * 1-LED group: `SetValue(v)`. `CPnlLed::SetValue` 0xcec58 accepts v<4.
  * VU (`CPnlVUMeterABC::SetValue` 0xcfc88): level 0..0x7e; >0x7e = clipped.
  * Multi-LED group:
    * `(v & 0x3000)==0x3000` → bitmask (LED i on if bit i, fewer than 12 LEDs);
    * `v == 0xfff` → all on;
    * otherwise `v` = index of the single lit LED.

---------------------------------------------------------------------------------------------------

## C. MIDI-over-USB questions

### C.1 Can the G2 USB protocol carry MIDI events?
* **0x56 PlayNote** — PROVEN.
  * `CMPlayNote::WriteStream` 0x187a8 writes `56 <onoff> <note>`: `onoff` = 0 note-on, 1 note-off; `note` is 7-bit, default 60.
    **No velocity, no channel.**
  * Header slot = **4** (default `CBubble`), void session: `01 2C 41 56 00 nn`.
    **The editor does not address a slot.** Which slot(s) sound is up to the synth (INFERRED: its keyboard-focus / slot selection, i.e. 07/0F).
  * The only creator is `CSynth::CleanDirtySynthData` 0x11e7aa.
  * Triggers: `CSynth::VirtualKeyboardStartNote`/`StopNote` (0x11def6/0x11d150), called from `CKeyboardFloater::StartNote/StopNote`
    (0x12ebc2/0x12ec1a, the on-screen keyboard) and `CPatchView::KeyPlayNote/KeyStopNote` (0xf7d62/0xf7d9c, computer keys).
  * Rate/limits: it goes out as a **cleaner bubble inside the dirty-clean loop**. So:
    * there is at most one note message per request/reply round-trip;
    * it is only sent when the port is idle;
    * it is **monophonic**: a new start is ignored while a note is on unless a stop is pending, and the stop is sent before the next start;
    * if the port loses editor focus, a held note is automatically stopped.
* **0x68/0x69 CurrentNotes** — PROVEN.
  * `68` (no payload) is sent when `CPatch+0x9c` is dirty: on patch creation and `CPatchView::PatchMarkCurrentNotesAsDirty`.
  * The `69` reply (`CCurrentVoiceData_11::ReadStream` 0x672c4) is a snapshot for saving in the patch file:
    id 0x69, 16-bit size, last note (note, attack velocity, release velocity, 7 bits each), count-1 (5 bits), then up to 32 × (note, attVel, relVel).
  * This is not a live event stream.
* **0x80 MidiLearn / 0x81 MidiLearnRequest** — PROVEN.
  * `81` has no payload (request, void, slot 4). It is sent at start-up (step 4).
  * `80 <slot 0..4> <cc>` → `HandleMidiLearn` 0x11d382 stores "last used MIDI controller" per slot when cc ≤ 119, valid and not pre-assigned.
    It is used by the MIDI-assign UI.
  * INFERRED: the synth also sends `80` unsolicited when a CC arrives at its MIDI IN. It carries only the CC number, not the value.
* **0x55 SendCtrlSnap**: `55`, no payload, patch user bubble (slot S, session ss). Menu "Send Controller Snapshot" (cmd 404, `CPatchView::HandleMenuSelection`).
  INFERRED: the synth sends the CC values of assigned controllers **out of its own MIDI port**. Nothing comes back over USB except the response.
* **0x3d DumpOneRequest**: `3D`, no payload, synth user bubble (slot 4, void). Menu "Dump One" (cmd 407, `CEditorApp::HandleMenuSelection` 0x14b838).
  INFERRED: it triggers the synth's own MIDI SysEx dump on its MIDI OUT. No `3D` reply molecule is registered in `CMoleculeFactory`.
* **There is no CC, pitch-bend, program-change, aftertouch, clock or SysEx-passthrough molecule.** PROVEN:
  * The full list of written ids is: 02 04 06 07 08 09 0A 0B 0C 0E 0F 10 14 17 19 1A 1C 1D 1E 20 22 23 25 26 27 28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 37 3B 3C 3D 3E 3F 40 42 43 44 4B 4C 4E 4F 50 51 53 54 55 56 58 59 5E 61 63 66 68 6A 6E 70 71 7D 81 90.
  * The registered incoming builders (`CMoleculeFactory` ctor 0xfcc8) are synth/patch/perf data only.
  * The only MIDI-related ids are 0x56, 0x68/0x69, 0x80/0x81 and 0x55.
  * MIDI CC *assignments* (`22` CtrlAssign, `23` CtrlDeassign, `61`/`60` ctrl map) are patch data, not events.

### C.2 USB-MIDI class vs vendor interface — PROVEN
* The binary links only IOKit, Carbon, libstdc++, libgcc and libSystem (`otool -L`).
  **CoreMIDI is not linked, there are no `_MIDI*` imports, and no "CoreMIDI" string.**
  The editor cannot talk to a USB-MIDI (Audio class) interface through macOS.
* `CUSBManager` matches the *device* only by VID 0x0FFC / PID 0x0002 (`IOUSBDevice`, 0x108eda).
  It opens config 0 and iterates all interfaces. Pipes are classified only by transfer type and direction: 1 interrupt-IN, 1 bulk-IN, 1 bulk-OUT.
  No class or subclass check is visible.
  * INFERRED: the G2 exposes one vendor interface. The handle pointer `+4` is overwritten per interface, so the last one wins.
  * Whether the G2 also has a USB-MIDI interface cannot be determined from editor code.
* **`CSynthPortMIDI` is dead code**: its constructors (0x106100/0x10632e) have no callers, and `CPortManager::CreatePort` only builds `Port::kUSB`.
  It is the Nord Modular (G1) style SysEx transport:
  * Frame: `F0 33 <b2> 06 <session> <data…> [crcHi7 crcLo7] F7`.
    `b2` = 0x40 new-syntax | slot&3, plus 0x04 bubble begin, 0x08 bubble end, 0x10 has CRC (non-realtime), 0x20 8→7 packed.
    The CRC is the same CRC16 truncated to 14 bits, split 7/7.
    `Convert8To7` 0x105d74 packs 7 bytes into 8.
  * Version request: `F0 33 00 06 00 <appMajor> <appMinor*10+rev> F7` (`PackSysex` 0x105e68).
  * Version reply: length 11–12 with `[4]==1` and `[2]==0`.
  * Constants at `__const` 0x1e5b50 (PROVEN): `kSysexVoidSessionFlag 40, kSysexNewSyntaxFlag 40, kSysex8To7Flag 20, kSysexCheckSumFlag 10,
    kSysexBubbleEndFlag 08, kSysexBufferEmptyFlag 08, kSysexBubbleBeginFlag 04, kSysexChannelMask 03, kMidiModularId 06, kMidiClavia 33,
    kMidiEofSysex F7, kMidiSysex F0, kVersionModelModular 00, kMesVersionInfo 01, kMesVersionRequest **00**`.
  * The byte string "00 40 80 08 10 08 20 07" is not a message. It is `kMesVersionRequest(00)` followed by the USB flag constants laid out next to it.

### C.3 Ownership / busy device
* Device busy (PROVEN):
  * `CMacUsbHandle::OpenDevice` 0x107e9c retries `USBDeviceOpen` **20×, 1 s apart (blocking the UI apart from `IdleOtherApps`)**,
    only for `0xe00002c5` kIOReturnExclusiveAccess. Then it throws `XUSBError`.
  * The exception unwinds through `OpenPort` (catch → `CUSBPort::Reset`, rethrow), the `CSynthPortUSB` ctor (catch → `ClosePort`, rethrow)
    and `CreateUSBPort`/`CreatePort` (catch(...) rethrow).
  * It is **swallowed in `CUSBManager::LowLevel_NewDevice` 0x10937a (catch(...) → `CUSBPort::Reset`)**. No alert, no port.
    The editor does not try again until a new IOKit match (replug) or wake (Resume).
* Once opened, the editor is aggressive (PROVEN): it calls **ResetDevice** and **USBInterfaceOpenSeize** (seizes the interface from other clients).
* Protocol-level "lock" (PROVEN mechanics, INFERRED meaning):
  * `7D SynthUnlockEditorSync <bool>` (command, synth level, user bubble; `CMSynthUnlockEditorSync::WriteStream` 0x18862).
    `01` is sent when the editor starts syncing (state 0→1). `00` is sent when every dirty flag is clean (state →2).
  * `CSynth::RequestEditorSyncLock` 0x11da4c is called by `CSynth::ConnectPerformance` and `CPerformance::ConnectPatch` when editor documents are bound to the synth.
    It moves state 2→0 and marks sessions unknown, so `7D 01` … `7D 00` is repeated.
  * Synth releases (0x38/0x1f) set state=1, so only `7D 00` follows.
  * INFERRED: this tells the synth an editor is mirroring it (for example, to freeze the front panel or buffer changes while syncing).
  * There is **no exclusive-ownership token** in the protocol. The session numbers are versioning, not ownership.
* `CBubblePort::LockUser`/`UnLockUser` 0x117c2a/0x117c0e is **local UI state only** (a user bubble in flight).
  It feeds `CSynth::IsSynthLocked`, `IsSlotLocked`, `IsGlobalSlotLocked` (0x11d1e0..), which disable flash load/store and similar UI actions.

---------------------------------------------------------------------------------------------------

## D. Errors and disconnection

### D.1 Status codes. `CSynthPort::SetInfoString` 0x104446
Text is from `original/rsrc/SSTR/128_Foo.bin`; resource id = 0x17d97800 + n.

| status | string | set by (PROVEN) |
|---|---|---|
| 1 | "Looking..." (20) | ctor |
| 2 | "Still looking..." (21) | Idle timeout with no synth |
| 3 | "Unsupported Model" (22) | CheckVersionMessage, model 0 |
| 4 | "Version mismatch (<model> Vx.yy)" (formatted) | CheckVersionMessage, wrong model, Rack, or protocol ≠ 0x12 |
| 5 | "New device found" (24) | version info changed while running |
| 6 | "Version Corrupted" (23) | `HandleReceivedData` catch(**XClaviaBase&**), e.g. **XMUnregisteredMolecule**, `XMStreamOverflow`, with the exception text |
| 7 | "Lost Contact\rCheck USB cables" (25) | Idle 10 s timeout while running; `HandleReceivedData` catch(**XUSBError&**) |
| 8 | "Update Mode\rRestart synthesizer in normal mode" (26) | CheckVersionMessage, mode ≠ 0 |
| 9 | "Out of memory" (27) | only in dead `CSynthPortMIDI::Idle` |
| 10 | "Uplink Checksum Error" (28) | **no setter found** (USB CRC errors are silently dropped) |
| 11 | "Synth Exception" (29) | `HandleReceivedData` catch(**XMHostMidi&**), with `GetErrorDescription()` |
| 12 | "Major Error" (19) | catch(...) in HandleReceivedData / CleanDirtyData / SendVersionRequest / ReSendBubble / CheckVersionMessage; interrupt-path fatal flag |
| 13 | "<model> Vx.yy" (OK) | `StartUsingPort` |
| other | "Status Error" (30) | |

Catch clauses were recovered from the LSDA, e.g. `HandleReceivedData` → [XMHostMidi, XUSBError, XClaviaBase, ...].
`CSynthPort::CleanDirtyData` **swallows `XUSBError`**: TX errors are ignored; catch(...) → 12.
`CUSBManager::TxData` 0x1088c6 also ignores `0xe00002c0` kIOReturnNoDevice.
`RxData` 0x108994 ignores `0xe00002ed` (NotResponding) and `0xe0004006`.

`XMHostMidi` (synth sent `7E code`, 0x46c7e):
* 3 = "Error in synth."
* 4 = "Down link check sum error."
* 5 = "Stream execute error. Synth may be corrupted. Please turn it off and on."
* 6 = "Unfinished bubble error."
* 0–2 = "".

### D.2 What happens on error (PROVEN)
* `MajorError` 0x104920 sets needVersion=1 and stores the text in `+0x3dc`, then `StopUsingPort` 0x1048e6 (DisposeSynth, SetStatus, flush input, clear waiting).
* `CSynthPort::IsInErrorMode` 0x103dea: every status except 1, 2 and 13.
  On its next visit **`CPortManager::Idle` destroys the port** and shows the alert `[STOP]<info>.\r<major text>.\r[OK]`.
  So statuses 3–12, including "New device found", "Update mode", "Version mismatch" and "Lost contact", end in port destruction and an alert.
  **No automatic reconnect** follows. The CUSBPort slot is freed, and a new port appears only on a new IOKit device match or after wake.
* Timeout while running (Idle 0x1054b4):
  * The counter `+0x3ee` is incremented and compared with `CSynthPort::kErrorRetry`. That is `__data` 0x293148, value **0**, and nothing writes it.
  * So the **first 10 s timeout** gives `MajorError(7 "Lost Contact")`. `ReSendBubble` / `CBubblePort::ReSendMessages` is effectively never reached.
  * Caveat: the timeout is measured from max(lastSend, lastReceive). Unsolicited LED traffic keeps refreshing lastReceive,
    so an unanswered request does not time out while blink data keeps arriving.
* Interrupt-path errors:
  * `XUSBBufferOverRun`: the input ring (`CMessageInput`, about 256 KiB, INFERRED from the 0x40010 flag offset) is full.
  * `XUSBError` from the bulk read timeout of 5 s.
  * Both are caught in `USBDataReceivedCallback` → `CSynthPort::FatalExceptionHandler` 0x1038ac (flag `+8`) → next Idle `MajorError(12)`.
  * `HandleInterrupt` catch(...) → listener `USBDeviceExceptionHandler` 0x102c76 → `PortFatalExceptionHandler` → same flag.
  * An interrupt completion with result ≠ 0 is not re-armed, so the line goes silent and a timeout follows.
* Unplug:
  * `DeviceRemovedCallback` 0x108e12 reads the `locationID` and calls `LowLevel_RemoveDevice` 0x108d60. That matches the handle's location, calls `CUSBPort::Reset`,
    and the listener `USBDeviceRemovedCallback` 0x102320.
  * The removed callback sets `gPendingLostContactDisplay` → alert "[STOP]Lost Contact\rCheck USB cables[OK]" from `CPortManager::Idle`.
    It then calls `DestroyPort` → `~CSynthPortUSB`.
* `~CSynthPortUSB` 0x106894:
  * `DisposeSynth`: `CSynth::Dispose` 0x11d7f6 marks the performance void unless in perf mode and disconnects it.
    Patches stay open offline with LEDs zeroed (`CPerformance::DisconnectSynth` 0x100042). `DefocusPort` runs.
  * `CUSBManager::ClosePort` → `CMacUsbHandle::Close` 0x107a4e: AbortPipe(interrupt), AbortPipe(bulk-in), USBInterfaceClose, Release,
    **USBDeviceSuspend(true)** (+0x80), USBDeviceClose, Release.
  * **No goodbye message is sent to the synth.**
* Sleep/wake (`SystemPowerCallback` 0x109610):
  * kIOMessageCanSystemSleep (0xe0000270) → `IOCancelPowerChange`. **The editor vetoes idle sleep.**
  * WillSleep (0xe0000280) → `CUSBManager::Suspend` 0x1087fa (AbortPipe on each interrupt pipe) + allow.
  * HasPoweredOn (0xe0000300) → `CUSBManager::Resume` 0x1094b2 calls `USBDeviceRemovedCallback` for every open port (port destroyed + "Lost Contact" alert),
    then re-matches devices with `IOServiceGetMatchingServices`, giving a fresh `LowLevel_NewDevice` and a full re-handshake.

---------------------------------------------------------------------------------------------------

## E. CPortManager / multiple synths (PROVEN)
* 4 port slots (`CPortManager+4 + 8*i`: u16 port, u16, `CSynthPort*`). `CUSBManager` maxPorts=4, `CUSBPort` = 0x1c bytes each.
* Port number = index of the first `CUSBPort` without an open handle at arrival time.
  A port number can therefore be reused after removal, and synths are not bound to physical sockets or serials.
  The `locationID` is used only to match removals.
* Each port is fully independent: its own `CSynthPortUSB`, `CSynth`, `CBubblePort`, sessions and handshake.
  * `CPortManager::Idle` runs **one port per editor idle tick** (round-robin), so N synths share the idle budget.
  * Each `CSynthPort::HandleReceivedData` drains for at most 500 ms.
* There is one global focus (port, slot):
  * only the lowest-numbered connected port queries the synth's focus (`08`);
  * every other port is told `09 04` (no focus);
  * focus moves with `SetFocusedSlot` / `DefocusPort` (A.7).
* LED display and virtual-keyboard notes are only active for the focused port (`CPatch::IsAcceptingLEDBlink`; `CleanDirtySynthData` stops notes when the port is not focused).

---------------------------------------------------------------------------------------------------

## Uncertainties
1. Reply content for `7D`, `70`, `55`, `3D` and other commands is assumed to be ack `7F`. The code only requires a response-flagged message.
2. The byte order of 0x3a words on the wire. The i386 code reads them in native little-endian order with no swap. A PPC build was not checked.
3. The exact molecule list inside the `3C` CompletePatchRequest reply is inferred from the registered builders (destination 2).
4. The IOKit method names come from mapping vtable offsets to the IOUSBLib (182/183/190-era) layout.
5. The meanings of `7D`, `70`, `55` and `3D` on the synth side, and the version-reply fields at bytes 4–5, 26–29 and 32–33, are INFERRED or unknown.
6. The meaning of byte 0 of incoming non-version messages is not checked by the editor (typically 0x01; INFERRED).
