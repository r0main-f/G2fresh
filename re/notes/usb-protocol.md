# Nord Modular G2 — USB protocol (editor ⇄ synth)

Reverse-engineered from the Clavia G2 editor v1.62 for Mac (Ghidra program `G2Editor_i386`, decompiled C in
`re/out/decomp/`). It is cross-checked against Bruno Verhue's GPL-2+ editor (`third_party/nord_g2_editor/`), which was
written against real hardware. It is meant to be precise enough to write both a host-side codec/client (libusb) and a
device emulator.

Golden vectors: `tests/protocol/*.json`, produced by `tests/protocol/gen_vectors.py` (§15).
The section payloads (patch/performance data) use the `.pch2` encoding; see `re/notes/pch2-format.md`. That encoding is
not repeated here.

Evidence tags used throughout:
* **[C]** proven from Clavia code or data (function name and address given).
* **[V]** from Verhue's sources (file:line, paths relative to `third_party/nord_g2_editor/`; "Gen2 Mess" =
  `Gen2/Common/BVE.NMG2Mess.pas`, "Gen2 USB" = `Gen2/Common/BVE.NMG2USB.pas`).
* **[V-cap]** a hex dump found in Verhue's comments, probably captured from the original editor.
* **[I]** inferred, not proven by code. Treat as a hypothesis to verify on hardware.

Addresses are i386 VMAs in `G2Editor_i386`. Only functions below 0x260000 are real code; the "[clone .eh]" functions in
the decomp are garbage.

---------------------------------------------------------------------------------------------------

## 0. Summary

```
USB  : VID 0x0FFC (Clavia), PID 0x0002, one vendor interface, 3 endpoints:
       interrupt-IN (16-byte packets), bulk-IN, bulk-OUT.  No control requests are used.
OUT  : [len16 BE][01][hdr][sess][molecule...][crc16 BE]     len = whole frame, CRC-16/XMODEM over [2..n-3]
       version request = 00 05 80 91 88
IN   : interrupt packet byte0&3 == 2 -> message (byte0>>4 bytes) inside the packet
                           byte0&3 == 1 -> message of BE16 length bytes[1..2] follows on bulk-IN
       message = [01][hdr][sess][molecule...][crc16 BE]  or  [80][version info...][crc16 BE]
hdr  : OUT 0x20 first-of-bubble | 0x10 realtime | 0x08 last-of-bubble | slot(0..3, 4 = synth/perf)
       IN  0x08 response | slot
sess : session number of the slot/performance (0..0x3F), or 0x40|molecule-count ("void session")
flow : at most one outstanding non-realtime bubble; realtime messages are fire-and-forget.
```

---------------------------------------------------------------------------------------------------

## 1. Device identification and USB set-up

### 1.1 Matching [C]
* `CPortManager::CPortManager` 0x102ff2 builds `CUSBManager(listener, 0x0FFC, 0x0002, EInterruptPresence=1, maxPorts=4)`
  (call at `CPortManager.c`, `CUSBManager::CUSBManager` 0x10925a).
* `CUSBManager::Init` 0x108eda: `IOServiceMatching("IOUSBDevice")` with `idVendor`=0x0FFC and `idProduct`=0x0002.
  It installs `IOServiceFirstMatch` (`DeviceAddedCallback` 0x10943e) and `IOServiceTerminate` (`DeviceRemovedCallback`
  0x108e12, matched by `locationID`) notifications, plus `IORegisterForSystemPower` (`SystemPowerCallback` 0x109610).
* **G2 keyboard, G2X and G2 Engine share VID/PID 0x0FFC/0x0002.** The model is only known from the version reply (§5.2).
  Verhue uses the same pair: `VENDOR_ID = $FFC`, `PRODUCT_ID = 2` (Gen3/Common/BVE.NMG2Types.pas:51-52) [V].

### 1.2 Opening the device [C] (IOKit method names mapped from vtable offsets; the mapping is [I] but standard)
`CMacUsbHandle::OpenDevice` 0x107e9c:
1. `GetLocationID` (+0x50) → handle+0xc (used to match removal).
2. `USBDeviceOpen` (+0x20). On `kIOReturnExclusiveAccess` (0xE00002C5) it retries **20× with a 1 s wait**. Any other
   error, or 20 failures, throws `XUSBError`.
3. `ResetDevice` (+0x64).
4. `GetNumberOfConfigurations` (+0x4c, must be > 0), `GetConfigurationDescriptorPtr(0)` (+0x54),
   `SetConfiguration(bConfigurationValue)` (+0x5c): **the first configuration**.
5. `CreateInterfaceIterator` (+0x70, "don't care" request). For every interface, `CMacUsbHandle::OpenInterface` 0x107b5a:
   * `USBInterfaceOpenSeize` (+0xb0);
   * `GetNumEndpoints` (+0x4c);
   * `InitPipes` 0x107aca, which calls `GetPipeProperties` (+0x68) for pipes 1..n and classifies by transfer type only:
     * bulk + IN → bulk-in pipe (handle+9);
     * bulk + OUT → bulk-out pipe (handle+10);
     * any other type → interrupt pipe (handle+8).
6. The async event source goes to the run loop, or an MP task runs `myThreadProc` 0x107982.

Verhue (libusb) [V] (Gen3/Common/BVE.NMG2USBUnix.pas:157-186; Gen2 USB:818-859): `libusb_open`, detach the kernel driver
of interface 0 if active, `libusb_claim_interface(0)`, and hard-coded endpoints **0x81 interrupt-IN, 0x82 bulk-IN,
0x03 bulk-OUT**. Windows: `usb_set_configuration(config[0])` and endpoints taken in descriptor order. No control transfers
and no alternate setting.

**Recommended host implementation:** match 0x0FFC/0x0002; set configuration 1 (the first); claim interface 0; find the
endpoints by type/direction (expected 0x81 / 0x82 / 0x03); do **not** send any control request.

**Emulator descriptor [I]:** USB 1.1 full-speed vendor-class device, 1 configuration, 1 interface (class 0xFF), endpoints
0x81 interrupt-IN (wMaxPacketSize 16, bInterval unknown — use 1..10 ms), 0x82 bulk-IN (64), 0x03 bulk-OUT (64).
Max packet sizes and bInterval are not visible in either code base (Clavia only reads 16-byte interrupt transfers).

### 1.3 Transfer primitives [C]
| op | Clavia | parameters |
|---|---|---|
| interrupt read | `CUSBManager::OpenPort` 0x10861a / `HandleInterrupt` 0x108b44: `ReadPipeAsync(interruptPipe, buf, 16, InterruptPipeCallback)` | 16-byte buffer. After each completion the callback runs, the thread **sleeps 10 ms** (`MPDelayUntil`), then re-arms. A completion with an error is not re-armed (`InterruptPipeCallback` 0x108c56). |
| bulk read | `CUSBManager::RxData` 0x108994 / 0x108a7e: `ReadPipeTO(bulkIn, buf, &len, noData=0, completion=1000 ms)` | It loops until a non-empty read or a total of **5000 ms**, then `XUSBError`. **The first non-empty read is taken as the whole message** (no accumulation of partial reads). kIOReturnNotResponding (0xE00002ED) and 0xE0004006 count as an empty read. |
| bulk write | `CUSBManager::TxData` 0x1088c6: `WritePipeTO(bulkOut, buf, len, 5000, 5000)` | One transfer per frame. kIOReturnNoDevice (0xE00002C0) is ignored. Other errors throw `XUSBError`, which `CSynthPort::CleanDirtyData` swallows. |

Verhue [V]: the interrupt read has an infinite timeout. The extended bulk read makes up to 5 tries of 500 ms each and
**accumulates** partial reads (Gen2 USB:1111-1177). Writes are single bulk transfers with an infinite timeout. The older
`BVE.NMG2USB - kopie.pas` split writes into 4096-byte packets.

**Emulator requirement:** deliver each extended message as **one** bulk-IN transfer (the Clavia editor does not
reassemble). With a 64-byte max packet the transfer ends on a short packet, or on a zero-length packet when the size is a
multiple of 64 [I].

---------------------------------------------------------------------------------------------------

## 2. Transport layer: interrupt notifications

Every device→host message is announced on the interrupt pipe as a 16-byte packet.
`CSynthPortUSB::USBDataReceivedCallback` 0x107194 [C] dispatches on `b0 & 3`:

| `b0 & 3` | name | layout | Clavia | Verhue |
|---|---|---|---|---|
| 2 | **embedded** | `b0 = (n<<4) \| 2`; bytes 1..n = the whole message incl. CRC (n ≤ 15) | message = packet[1..n] | `IsEmbedded: b0 & $0F = 2` |
| 1 | **extended** | `b0 = 0x?1`; `b1..b2` = BE16 message length L. The message (L bytes incl. CRC) is then read from bulk-IN | `RxData(bulkIn, L, 5000 ms)` | `IsExtended: b0 & $0F = 1` |
| 0 | (inline-long) | `b1..b2` = BE16 length, message inline from byte 3 | handled, never observed | treated as an error |
| 3 | — | ignored | ignored | error, receive thread stops |

* The high nibble of an extended announcement is ignored by both editors. An emulator should send 0x01 [I].
* Embedded is used when the message is ≤ 15 bytes [V-cap: `82 01 04 00 80 00 3F 30 40 …`].
  A device may also send short messages as extended [I]. Hosts must accept both.
* Unused trailing bytes of the interrupt packet are don't-care (zero in the vectors).

---------------------------------------------------------------------------------------------------

## 3. Message framing

### 3.1 Host → device (bulk OUT) [C] `CSynthPortUSB::BuildOutputMessage` 0x10691a, `SendData` 0x106a48
```
offset  size  field
0       2     total frame length, big-endian (includes these 2 bytes and the CRC)
2       1     0x01                                  (0x80 for the version request, see below)
3       1     hdr  = slot & 7
                   | 0x20 if first message of the bubble   (kUSBMessageBubbleBeginFlag)
                   | 0x10 if realtime                      (kUSBMessageRealTimeFlag)
                   | 0x08 if last message of the bubble    (kUSBMessageBubbleEndFlag)
4       1     sess = session number (0..0x3F)  or  0x40 | molecule count   (kUSBMessageVoidSessionFlag)
5       n     molecules (concatenated, see §4)
5+n     2     CRC-16 big-endian over bytes [2 .. 4+n]
```
* **Version request**: `00 05 80 91 88`. `SendVersionRequest` 0x10496c creates a header of type 2 with no data;
  `BuildOutputMessage` writes `0x80`, then the CRC over that single byte. Verhue sends the same bytes (Gen2 Mess:2375) [V].
* The constants live in `__const` 0x1E5B5F.. [C]: `kUSBMessageVoidSessionFlag=0x40`, `kUSBMessageDebug=0x80`,
  `kUSBMessageResponseFlag=0x08`, `kUSBMessageRealTimeFlag=0x10`, `kUSBMessageBubbleEndFlag=0x08`,
  `kUSBMessageBubbleBeginFlag=0x20`, `kUSBMessageChannelMask=0x07`.
* The editor's internal "request" message type (`COutputMessage::IsRequest`, 0x1036e4) is **not** encoded on the wire.
* In practice every bubble fits in one message, so the `hdr` byte takes one of these values:

  | hdr | meaning | Verhue name |
  |---|---|---|
  | `0x28 \| slot` | normal bubble for slot 0..3 | `CMD_REQ+CMD_SLOT+s` |
  | `0x2C` | normal synth/performance bubble (slot 4) | `CMD_REQ+CMD_SYS` |
  | `0x38 \| slot` | realtime message for slot 0..3 | `CMD_NO_RESP+CMD_SLOT+s` |

  Verhue's constants (`CMD_REQ=$20`, `CMD_NO_RESP=$30`, `CMD_SLOT=$08`, `CMD_SYS=$0C`, Gen2/Common/BVE.NMG2Types.pas:96-103)
  are compositions of these flag bits. `0x3C` (realtime synth message) is never generated by either editor.

### 3.2 Device → host [C] `CSynthPortUSB::InterruptHandleData` 0x106f72, `BuildHeaderAndData` 0x106e92
```
normal message                         version reply
0     0x01 (value not checked)         0     0x80
1     hdr: bit3 0x08 = response,       1..   version info (§5.2)
           bits2..0 = slot (4 = synth/perf)
2     sess (bit6 = void, bits5..0 = session number)
3..   molecules (first byte = molecule id)
n-2   CRC-16 BE over bytes [0 .. n-3]  n-2   CRC-16 BE over [0 .. n-3]
```
* `hdr` bit 3 means "this is the answer to the outstanding request" (`CInputMessage::IsResponse` 0x103510;
  `kUSBMessageResponseFlag`). Verhue matches replies on the same nibble: a reply to `0x28+s` has `hdr = 0x08+s`, a reply to
  `0x2C` has `hdr = 0x0C`. Verhue also accepts `0x04` as a reply to `0x2C` (Gen2 USB:1839/1922) [V].
* The begin/end/realtime bits are not used on input. Unsolicited messages have `hdr = slot` (0..4).
* Message classes by molecule id byte [3] (`BuildHeaderAndData`) [C]:
  * 0x39/0x3A → **blink** (LED/meter, §10);
  * 0x38/0x1F → **release** (§5.3);
  * byte [0] = 0x80 → **version**;
  * anything else → normal.
* A **bad CRC drops the message silently** (Clavia). Verhue only logs it.

### 3.3 CRC [C]
`CRC_Init` 0x14fce0 / `CRC_Get16` 0x14fd40: polynomial 0x1021, init 0x0000, MSB-first, no reflection, no final XOR =
**CRC-16/XMODEM**. `CRC_UnitTest` 0x14fe5e asserts CRC("1234") = 0xD789. CRC("123456789") = 0x31C3.
* OUT: the CRC covers frame bytes [2 .. len-3] (it excludes the 2 length bytes).
* IN: the CRC covers message bytes [0 .. len-3].

Verhue: `CrcClavia`, the same polynomial and coverage (Gen2 USB:1111-1198, Gen2 Mess:861-886) [V].

---------------------------------------------------------------------------------------------------

## 4. Bubbles, molecules, flow control

### 4.1 Molecules [C]
The payload after the 3-byte header is a list of **molecules**: `id (8 bits) + body`.
* The body is written with a `CBitStream` (MSB-first, the same bit packing as `.pch2`; see `pch2-format.md` §1.5).
* Every byte-sized field is byte-aligned: `PutUByte`/`GetUByte` first pad or skip to the byte boundary.
* Molecules are concatenated at **byte** granularity. On input, `CBubblePort::AddToResultBubble` 0x117e8a starts a new
  stream for each molecule and advances by `ceil(bits/8)`.
* Multi-byte integers inside molecule bodies are big-endian (`PutUWord` 0x146f40), except the 7-bit-pair helpers noted in
  §7.
* Strings (`str16`) are Latin-1, at most 16 chars, followed by a NUL **only if shorter than 16** (`CBitStream::PutString`
  0x14789e).

The molecule classes `CM<Name>` serialize with `WriteStream(CMIDIOutStream&)` and `ReadStream(CMIDIInStream&)`.
`CMIDIOutStream::Initialize(id, EMessageType, EMSBUsage)` 0xcc70 writes the id; EMessageType is 0 = normal command,
1 = realtime, 2 = request. EMSBUsage only matters for the obsolete SysEx transport.

`CMoleculeFactory::CMoleculeFactory` 0xfcc8 registers the **50 molecule ids the editor can receive**. An unknown id in an
incoming message throws `XMUnregisteredMolecule`, which kills the connection (status 6, §11).
**An emulator must never send an id outside that list** (§7 column "Dir").

Two terminator bytes appear in place of a molecule id (`CSynthPort` constants at 0x1E5A44..49, `AddToResultBubble`):
* `0x7F kMesAck`: end of the reply; nothing else is parsed. Verhue `R_OK`.
* `0x7E kMesException` + 1 error byte: the synth reports an error (`XMHostMidi`, §11). Verhue `R_ERROR`.

### 4.2 Bubbles and output queues [C] `CBubblePort` 0x117b82..0x11882c
A **bubble** is a list of molecules with a destination slot and a session (`CBubble` 0xca6a; the default is slot 4, void
session). `CBubblePort` has three output queues:

| queue | filled by | sent |
|---|---|---|
| realtime (+0x3C) | `SendRealTimeBubble` 0x1185f0 → `GenerateRealtimeMessages` 0x118536: **one message per molecule**, hdr `0x38\|slot`, sess = bubble session | immediately, all of them, on every `SendMessages` call; no reply expected |
| cleaner (+0x34) | `SendCleanerBubble` 0x118466 (sync/refresh traffic produced by `CleanDirtyData`) | when nothing is outstanding; the message is saved for a possible resend |
| user (+0x2C) | `SendUserBubble` 0x1184c2 (UI edits); also locks the UI (`LockUser`) until the reply | when nothing is outstanding and the cleaner queue is empty |

`GenerateMessages` 0x11828a:
* skips molecules whose `EditorOnly()` (vtable +0x28) is true (the mutator's local-only molecules);
* packs the rest back to back and splits them into messages of ≤ 0xFFFF bytes (Begin on the first, End on the last);
* if the session is void, puts `0x40 | number of molecules ending in this message` in the session byte.

This explains Verhue's "mystery" constants [V]:
* `$41` = void session with 1 molecule on every system request;
* `$42` = performance upload (`37` + `1A`, 2 molecules);
* `$53` = patch upload (`37` + 18 sections = 19 molecules).

### 4.3 Flow control — stop-and-wait [C]
* `CBubblePort::SendMessages` 0x117d2c first flushes all realtime messages. Then, **only if the port is not waiting**
  (`CSynthPort+0x3EC`), it sends exactly one cleaner or user message. `CSynthPort::SendMessage` 0x104414 sets "waiting"
  and the send timestamp.
* A received message with the response flag (`hdr & 0x08`) clears "waiting" (`CSynthPort::HandleReceivedData` 0x105102).
  It is passed to `CSynth::HandleRequestedData` → `CBubblePort::HandleRequestedData` 0x11809c, which accumulates the
  molecules into the result bubble. The result bubble is routed when the last message of the request bubble has been
  answered. The next message is then sent.
* Messages **without** the response flag are synth-initiated (`CSynth::HandleSynthInitiatedData`). They are routed at
  once, even while a request is outstanding, and do not affect the waiting state. Blink messages take a separate fast path
  (§10).
* **Every normal bubble gets exactly one response message** (the code requires it). The content can be:
  * the requested dump molecules (optionally followed by `7F`);
  * just `7F`;
  * `7E code`.

  Which commands get only `7F` is [I]. The Verhue code and its TCP emulation reply `7F` to all plain commands
  (Gen2 Mess:1447-1576) [V].
* Verhue Gen2 [V] does the same (one outstanding request, `FWaitForCmd`). A non-matching message received while waiting is
  **dropped**. Gen3 pipelines requests and matches replies FIFO.
* Realtime messages (`0x40` param change, `0x2F` param focus, `0x43` morph while dragging) are never answered.

---------------------------------------------------------------------------------------------------

## 5. Addressing, version handshake, session numbers

### 5.1 Slot field
`hdr & 7`: 0..3 = patch slots A..D; **4 = synth / performance level** (`CBubble` default 0xca6a; Verhue `CMD_SYS=$0C`).
Values 5..7 are never used.

Which header an editor bubble uses (`CSynth::SendUserBubble`/`SendCleanerBubble` 0x11ddee/0x11d464,
`SendPerformance*Bubble` 0x11d4a8/0x11d54a, `SendPatch*Bubble` 0x11d860/0x11d90a/0x11d972) [C]:

| kind | hdr | sess | used for |
|---|---|---|---|
| **S** synth bubble | 0x2C | 0x40\|count (void) | synth settings, session requests, flash, MIDI learn, voices, clock, uploads |
| **P** performance bubble | 0x2C | performance session | perf header/name/knobs/focus/selection |
| **T** patch bubble | 0x28\|slot | that slot's session | patch requests and edits |
| **T-void** patch upload | 0x28\|slot | 0x40\|count | complete patch upload (`37` + sections) |
| **RT** realtime | 0x38\|slot | that slot's session | param change, param focus, morph drag |

### 5.2 Version handshake [C] `CSynthPort::SendVersionRequest` 0x10496c, `CheckVersionMessage` 0x104d2c, `CSynthInfo(SVersionMessage)` 0x103e56
* The first idle pass (`CSynthPort::CleanDirtyData` 0x104b7c, flag `+0x400` "need version" set by the constructor and by
  `MajorError`) sends `00 05 80 91 88`.
* The reply is a message whose first byte is 0x80. In practice it is delivered extended (Verhue sees it on bulk-IN) [V].
  Layout (offsets from the 0x80):

  | off | size | meaning |
  |---|---|---|
  | 0 | 1 | 0x80 |
  | 1 | 1 | must be **0x0A**. Otherwise model = unknown (status 3) |
  | 2 | 1 | model: 0 → "G2" (enum 5, `CModularG2`); 1 → "G2 61"/G2X (enum 6, `CModularG2V`); 2 → G2 Rack (enum 7, **rejected**: `CreateSynth` 0x103a2c has no case); 3 → **G2 Engine** (enum 8, `CEngine`); 4 → Native (enum 4, rejected); else unknown |
  | 3 | 1 | mode: 0 = normal; 1 = update (OS-upgrade) mode → status 8; other = status 8 |
  | 4–5 | BE16 | unknown (port+0x412; it is compared to detect a "new device") |
  | 6–7 | BE16 | **firmware version × 100**, displayed as `V<v/100>.<v%100:02>` (`SetInfoString` 0x104446) |
  | 8–9 | BE16 | **protocol version, must be 0x0012** (`IsSynthAccepted` 0x1038b8), else status 4 "Version mismatch" |
  | 10–25 | | unknown, not read |
  | 26–29 | BE32 | unknown (port+0x414; maybe a serial number [I]) |
  | 30–31 | | unknown |
  | 32–33 | BE16 | unknown (port+0x40e) |

  The total length of the real reply is unknown. It is at least 34 bytes + CRC. Verhue never parses it (Gen2 Mess:1596) [V].
* Acceptance: model ∈ {G2, G2-61, Engine} and mode == 0 and word@8 == 0x0012 → `StartUsingPort` 0x104c54 → status 13
  "<model> Vx.yy". The CSynth object is created with 4 slots, 32 patch banks and 8 performance banks
  (`CModularG2::CModularG2` 0x11eed6).
* No reply within 10 s → status 2 "Still looking...". The request is re-sent every 10 s, forever.
* A version reply received while connected:
  * if the stored fields are identical, it is ignored;
  * if they differ → status 5 "New device found", and the port is destroyed (§11).
  The editor itself never re-sends the version request while connected: **there is no keep-alive** [C].

### 5.3 Session numbers ("patch version" in Verhue) [C]
* There is one 6-bit session number per slot (`CSynth::CSlotData`+4, with an "unknown" flag at +5) and one for the
  performance (`CSynth`+0x18/+0x19). **The synth owns them; the editor never increments them.** The editor copies the
  stored value into byte [4] of every T/P bubble and of realtime messages.
* Sources (synth → editor):
  * `36 slot sess` CMSessionNumberDump: the reply to `35 slot`. Slot 4 = performance (`HandleSessionNumberDump` 0x11dcd8).
  * `38 slot sess` CMSessionPatchRelease (`kMesPatchRelease`), unsolicited: the synth replaced the patch in that slot.
    The editor stores the session, invalidates the slot's LED cache, creates an empty upload patch and **re-reads that
    slot** (`HandleSessionPatchRelease` 0x11df1c).
  * `1F sess` CMPerformanceRelease (`kMesPerformanceRelease`), unsolicited: new performance. The editor re-reads
    everything (`HandlePerformanceRelease` 0x11d7ac).
  * An incoming patch bubble whose session differs from the stored one: the editor adopts it and re-reads the slot
    (`CSynth::RouteBubble` 0x11f940).
* Routing of incoming bubbles (`RouteBubble`):
  * patch-destination molecules are applied only if the slot session is known and equal;
  * performance-destination molecules only if the performance session matches;
  * synth-destination molecules if the session is void or matches.
  The synth presumably applies the same rule to editor bubbles (a stale session makes the synth discard the edit) [I].
* Verhue [V]:
  * initial version 0;
  * updated from `36`, `38` and the compound `1F PV {36 s v}×4` (Gen2 Mess:1614-1662);
  * messages whose version byte is neither 0x40 nor the current value are rejected (Gen2 Mess:3670);
  * a `38` on hdr 0x04 triggers a full re-init of that slot.

---------------------------------------------------------------------------------------------------

## 6. Connection / initialisation sequence

### 6.1 USB arrival → port [C]
1. `DeviceAddedCallback` 0x10943e → `CUSBManager::LowLevel_NewDevice` 0x10937a takes the first free of 4 port slots (the
   port number) → `CPortManager::USBDeviceAddedCallback` 0x102284 → `CreatePort` 0x1021fa → `CreateUSBPort` 0x102064 →
   `CSynthPortUSB::CSynthPortUSB` 0x106cd2 → `CUSBManager::OpenPort` 0x10861a (opens the device as in §1.2 and arms the
   interrupt read).
2. `CSynthPort::CSynthPort` 0x105688: status 1 "Looking...", need-version = 1.
3. The application idle loop (`CEditorApp::Idle` 0x1496de → `CPortManager::Idle` 0x102678, **one port per tick,
   round-robin**) → `CSynthPort::Idle` 0x1054b4:
   * `HandleReceivedData` drains the input queue for ≤ 500 ms;
   * `CTimeoutManager::SuggestAction` 0x103f56 returns 3 (timeout) if a reply has been pending for > 10 000 ms since
     max(last send, last receive); otherwise it returns 1 → `CleanDirtyData` (sends the next needed bubble).

### 6.2 Handshake
`00 05 80 91 88` → version reply (§5.2) → `StartUsingPort` → `CSynth` created with everything dirty
(`CSynth::CSynth` 0x11e964).

### 6.3 Sync order (Clavia, typical "read everything from the synth" case) [C]
`CSynth::CleanDirtyData` 0x11f394 emits **at most one bubble per call** and is only called when nothing is outstanding,
so the following is a strict request/reply chain. Bytes are shown from frame offset [2]; `ps` = performance session,
`ss` = slot session.

| # | frame [2..] | molecule | reply |
|---|---|---|---|
| 1 | `01 2C 41 7D 01` | CMSynthUnlockEditorSync(true) — sync starts (user bubble) | response (`7F` [I]) |
| 2 | `01 2C 41 35 04` | CMSessionNumberRequest(perf) | `36 04 ps` |
| 3 | `01 2C 41 02` | CMSynthDataRequest | `03 …` CMSynthDataDump |
| 4 | `01 2C 41 81` | CMMidiLearnRequest | `80 slot cc` |
| 5 | (`01 2C 41 56 0x nn` only if an on-screen-keyboard note is pending) | CMPlayNote | response |
| 6 | `01 2C ps 10` | CMPerformanceHeaderRequest | `29 name` + `11 …` (Verhue [V]) |
| 7 | `01 2C ps 59` | CMGlobalParameterPageFocusRequest | `1E page` |
| 8 | `01 2C ps 08` | CMSlotFocusRequest (only on the lowest-numbered connected port; other ports send `09 04`) | `09 …` |
| 9 | for S = 0..3: | | |
| 9a | `01 2C 41 35 0S` | CMSessionNumberRequest(S) | `36 0S ss` |
| 9b | `01 28+S ss 3C` | CMCompletePatchRequest (`20` if only the header is dirty, `28` if only the name) | patch sections (§8.2) |
| 9c | `01 28+S ss 58` | CMParameterPageFocusRequest | `2D page` |
| 9d | `4B/53/4C/66/63/61/4E/4F loc` | only for parts still dirty after 9b (VA = 1 first, then FX = 0) | matching section |
| 9e | `01 28+S ss 68` | CMCurrentNotesRequest | `69` section |
| 9f | `01 28+S ss 6E` | CMTextpadRequest | `6F` section |
| 9g | (`21 …` if the editor has a pending header change) | | |
| 9h | `01 28+S ss 71 01`, then `… 71 00` | CMPatchLoadRequest VA, FX | `72 …` each |
| 9i | `01 28+S ss 70` | CMFlushBlink | response |
| 9j | `01 28+S ss 2E` | CMParamFocusRequest | `2F …` |
| 10 | `01 2C 41 04` | CMNumberOfVoicesRequest | `05 v0 v1 v2 v3` |
| 11 | `01 2C 41 3B` | CMClockInfoRequest | `5D …` |
| 12 | `01 2C ps 5E` | CMPerfKnobMapRequest | `5F` section |
| 13 | `01 2C 41 14 t b p` (repeated) | CMFlashDataRequest: patch, then performance name lists | `16 …` |
| 14 | `01 2C 41 7D 00` | CMSynthUnlockEditorSync(false) — sync done (user bubble) | response |

The function addresses for each step are listed in §7. The drivers of the sequence are (`CPatch::CleanDirtyData` 0xd601e,
`CPerformance::CleanDirtyData` 0x100e5a, `CSynth::CleanDirtySynthData` 0x11e7aa, `CPatchLoad::SendLoadRequest`
0xee866, `CFlashData::CleanDirtyData` 0x1194aa).

After step 14, `CleanDirtyData` returns 0 and **nothing is polled**. Traffic resumes only on UI edits or synth-initiated
messages (`38`, `1F`, param changes, …), which set dirty flags again. `CSynth::RequestEditorSyncLock` 0x11da4c (perf or
patch documents re-bound to the synth) restarts the sequence from `7D 01`. Releases (`38`/`1F`) restart it without
`7D 01`, but `7D 00` is still sent at the end.

**Download-instead-of-read cases** [C] (`CSynth::CSynth` 0x11e964): if the editor's topmost document is an unconnected
patch, it is uploaded to slot A (`CreateDownloadPerformanceFromPatch` 0xfe478). If it is an unconnected performance, the
whole performance is uploaded (§8.1), after steps 1–4. A rewrite should make this behaviour explicit and opt-in.

### 6.4 Verhue's sequence [V] (Gen2 USB:2160-2194, 2523-2598, 2877-2951)
1. init `80`;
2. `7D 01` ("stop comm");
3. `35 04`;
4. `02`;
5. `81` ("unknown 1");
6. perf: `10`, `59` ("unknown 2");
7. per slot: `35 s`, `3C`, `28`, `68`, `6E`, `71 01`, `71 00`, `70` ("unknown 6"), `2E`;
8. `04`, `5E`, `3B`, `14 …` (repeated);
9. `7D 00` ("start comm").

The same order, except:
* Verhue sends no `08` slot-focus request and no `58`;
* Verhue sends `28` (name) separately, because its `3C` reply handling does not need it;
* Verhue does `5E` before `3B`.
The order is not critical as long as each request is answered before the next is sent.

Verhue's naming of `7D 01` as "stop comm" and `7D 00` as "start comm" fits the Clavia semantics: while the editor sync is
in progress (`01`) the synth stops pushing unsolicited data. Data flows again after `00` [I].

---------------------------------------------------------------------------------------------------

## 7. Command (molecule) table

Notation:
* **Dir**: E→S editor writes it (`WriteStream` + `Initialize`); S→E registered in `CMoleculeFactory` (the editor can
  receive it); both.
* **T**: EMessageType of the editor's writer: 0 normal, 1 realtime, 2 request.
* **Hdr**: bubble kind from §5.1 (S, P, T, RT, T-void).
* **Dest**: destination of received molecules: 1 synth, 2 patch, 3 performance.

Payload notation:
* `u8` = aligned byte;
* `bN` = N-bit field, MSB first;
* `padN` = zero bits;
* `u16` = big-endian word;
* `str16` = see §4.1;
* "(sect)" = a `.pch2` section: `id`, `u16 length`, body (see `pch2-format.md`).

Location `loc`: 0 = FX, 1 = VA, 2 = patch settings (the values are [C]; the names follow Verhue and pch2-format §1.6).
Addresses are `WriteStream`/`ReadStream` of the `CM*` class.

| id | Clavia molecule | Verhue constant | Dir | T | Hdr | Dest | Payload after id | Reply / notes |
|---|---|---|---|---|---|---|---|---|
| 02 | CMSynthDataRequest 0x186aa | Q_SYNTH_SETTINGS | E→S | 2 | S | – | – | `03` |
| 03 | CMSynthDataDump 0x1872a/0x18702 (`CSynthMap` 0x120410/0x12012e) | S_SYNTH_SETTINGS | both | 0 | S | 1 | §7.1 | E→S only to toggle perfMode |
| 04 | CMNumberOfVoicesRequest 0x17d9a | Q_ASSIGNED_VOICES | E→S | 2 | S | – | – | `05` |
| 05 | CMNumberOfVoicesDump 0x1836c | R_ASSIGNED_VOICES | S→E | – | – | 1 | `u8 voices[4]` (A..D) | |
| 06 | CMSlotSelectionRequest 0x17bb6 | – | E→S | 2 | P | – | – | no caller |
| 07 | CMSlotSelectionDump 0x1853c/0x18480 | – | both | 0 | P | 3 | `pad4, b1 A, b1 B, b1 C, b1 D` (A = 0x08) — slots enabled | |
| 08 | CMSlotFocusRequest 0x17d4e | – | E→S | 2 | P | – | – | `09` |
| 09 | CMSlotFocusDump 0x181c4/0x180de | S_SEL_SLOT | both | **2** | P | 3 | `pad5, b1 none, b2 slot` → byte 0..3, or 0x04 = no focus | focused slot (UI). Verhue sends `09 s` |
| 0A | CMFlashLoadCommand 0x1142a | S_RETREIVE | E→S | 0 | S | – | `u8 slot (0..3, 4 = perf), u8 bank, u8 prog` | load from flash; a `38`/`1F` release follows [V] |
| 0B | CMFlashStoreCommand 0x112b8 | S_STORE | E→S | 0 | S | – | `u8 slot, u8 bank, u8 prog` | `0D` |
| 0C | CMFlashDeleteCommand 0x1131e | S_CLEAR | E→S | 0 | S | – | `u8 type (0 patch/1 perf [I]), u8 bank, u8 prog, u8 origin` | `15` |
| 0D | CMFlashCommandResult 0x116c4 | R_STORE | S→E | – | – | 1 | `u8 type, u8 bank, u8 prog, u8 origin, u8 result` (0 ok, 1, 2, 4, 5; other → 3) | |
| 0E | CMFlashDeleteRangeCommand 0x11394 | S_CLEAR_BANK | E→S | 0 | S | – | `u8 type, u8 bank1, u8 prog1, u8 bank2, u8 prog2, u8 origin` | `12` |
| 0F | CMKeyboardFocusDump 0x18260/0x182f2 | – | both | 0 | P | 3 | `pad4, 4×b1` keyboard enabled A..D | |
| 10 | CMPerformanceHeaderRequest 0x16a14 | Q_PERF_SETTINGS | E→S | 2 | P | – | – | `29 name` + `11` (sect) |
| 11 | CMPerformanceHeaderDump (`CPerformanceHeader_11` 0x64254/0x68e38) | C_PERF_SETTINGS | both | 0 | P | 3 | (sect) | pch2-format §4.1 |
| 12 | CMFlashDeleteRangeDump 0x11626 | R_CLEAR_BANK | S→E | – | – | 1 | `u8 type, u8 b1, u8 p1, u8 b2, u8 p2` | |
| 13 | CMFlashUsageDump 0x10c1a | R_LIST_NAMES (misnamed) | S→E | – | – | 1 | `u8 lo, u8 hi` → `lo \| hi<<7`, ≤ 1000 (flash usage ‰ [I]) | Verhue saw `13 30 02` |
| 14 | CMFlashDataRequest 0x111ec | Q_LIST_NAMES | E→S | 2 | S | – | `u8 type, u8 bank, u8 prog` (start entry) | `16` (optionally preceded by `13`) |
| 15 | CMFlashDeleteDump 0x115bc | R_CLEAR | S→E | – | – | 1 | `u8 type, u8 bank, u8 prog` | |
| 16 | CMFlashDataDump 0x11cfa | R_ADD_NAMES | S→E | – | – | 1 | `u8 flag, u8 type`, then tags: `03 b p` set entry · `01 p` set prog · `02` empty slot (prog++) · `04` end of list · `05` end of chunk (ask again from the next entry) · other byte = first char of `str16 name` + `u8 category` (prog++) | |
| 17 | CMFlashDumpRequest 0x11252 | S_PATCH_BANK_UPLOAD | E→S | 2 | S | – | `u8 type, u8 bank, u8 prog` | `18` + `19` |
| 18 | CMFlashDumpMarker 0x11eae | R_PATCH_BANK_UPDLOAD | S→E | – | – | 1 | `u8 code, u8 ? (type [I]), u8 bank, u8 prog, [str16 name if code == 2]`. code: 0 ok/down-OK · 2 header (terminating) · 3 memory full · 4 empty · 5 corrupt · 1/≥6 protected | |
| 19 | CMFlashDumpRawData 0x10d04/0x10f52 | S_PATCH_BANK_DATA | both | 0 | S | 1 | `u8 type, u8 bank, u8 prog, str16 name, u16 size, u8 version, size × u8` (raw .pch2/.prf2 binary part); size 0xFFFF = no data | §8.3 |
| 1A | CMCompletePerformanceDump 0x17248 | – | E→S | 0 | S (with 37) | – | `29 name`, `11` (sect), 4 × patch sections, `5F` (sect) | §8.1 |
| 1C | CMGlobalKnobAssign 0x1719c/0x1764e | S_ASS_GLOBAL_KNOB | both | 0 | P [I] | 3 | `b2 assignType, b2 slot, b2 loc, pad2, u8 module, u8 param, u16 knob(0..119)` | Verhue sends it in a T frame |
| 1D | CMGlobalKnobDeassign 0x16ebc/0x170ee | S_DEASS_GLOB_KNOB | both | 0 | P | 3 | `u16 knob` | |
| 1E | CMGlobalParameterPageFocus 0x16fb6/0x16ffc | S_SEL_GLOBAL_PAGE | both | 0 | P | 3 | `u8 page` | |
| 1F | CMPerformanceRelease 0x1697e (`kMesPerformanceRelease`) | – | S→E | – | – | 1 | `u8 session` | §5.3 |
| 20 | CMPatchHeaderRequest 0x16604 | – | E→S | 2 | T | – | – | `21` (sect) |
| 21 | CMPatchHeaderDump (`CPatchHeaderData_11` 0x65564/0x664ae) | C_PATCH_DESCR | both | 0 | T | 2 | (sect, fixed length) | |
| 22 | CMCtrlAssign 0x14d1a/0x15538 | S_ASSIGN_MIDICC | both | 0 | T | 2 | `u8 loc, u8 module, u8 param, pad1, b7 cc` | assigns a MIDI CC to a parameter |
| 23 | CMCtrlDeassign 0x14e48/0x14ea4 | S_DEASSIGN_MIDICC | both | 0 | T | 2 | `pad1, b7 cc` | |
| 25 | CMKnobAssign 0x15628/0x15a0c | S_ASSIGN_KNOB | both | 0 | T | 2 | `u8 module, u8 param, b2 loc, b2 assignType, pad4, u16 knob(0..119)` | |
| 26 | CMKnobDeassign 0x14bea/0x153fa | S_DEASSIGN_KNOB | both | 0 | T | 2 | `u16 knob` | |
| 27 | CMPatchNameDump 0x166f8/0x167f6 | S_PATCH_NAME | both | 0 | T | 2 | `str16` (no length field) | |
| 28 | CMPatchNameRequest 0x16630 | Q_PATCH_NAME | E→S | **0** | T | – | – | `27` |
| 29 | CMPerformanceNameDump 0x16b08/0x1702a | C_PERF_NAME | both | 0 | P | 3 | `str16` | |
| 2A | CMModuleABCode 0x129f4 | S_SET_UPRATE | E→S | 0 | T | – | `u8 loc, u8 module, u8 bandwidth (uprate)` | generated on cable changes |
| 2B | CMDSPPartChange 0x12b40 | S_SET_MODE | E→S | 0 | T | – | `u8 loc, u8 module, u8 selector, u8 value` | "mode" parameters (waveform…) |
| 2C | CMPerformanceNameRequest 0x16a40 | – | E→S | **0** | P | – | – | `29` |
| 2D | CMParameterPageFocus 0x15224/0x1526a | S_SEL_PARAM_PAGE | both | 0 | T | 2 | `u8 page` | |
| 2E | CMParamFocusRequest 0x1231c | Q_SELECTED_PARAM | E→S | 2 | T | – | – | `2F` |
| 2F | CMParamFocusDump 0x1220c/0x12282 | S_SEL_PARAM | both | **1** | RT | 2 | W: `u8 0, u8 loc, u8 module, u8 param`; R: `u8 flag, u8 loc, pad1 b7 module, pad1 b7 param` | parameter selected |
| 30 | CMModuleNew 0x13056/0x132c4 | S_ADD_MODULE | both | 0 | T | 2 | `u8 type, u8 loc, u8 index, u8 col, u8 row, u8 color, u8 uprate, u8 locked, N×u8 mode values, str16 name` (N = mode count of the type) | Verhue adds sections `52 4D 5B 5A` in the same bubble |
| 31 | CMModuleRecolor 0x128c4 | S_SET_MODULE_COLOR | E→S | 0 | T | – | `u8 loc, u8 module, u8 color` | |
| 32 | CMModuleDelete 0x12518 | S_DEL_MODULE | E→S | 0 | T | – | `u8 loc, u8 module` | |
| 33 | CMModuleName 0x1278e | S_SET_MODULE_LABEL | E→S | 0 | T | – | `u8 loc, u8 module, str16` | |
| 34 | CMModuleMove 0x1248c | S_MOV_MODULE | E→S | 0 | T | – | `u8 loc, u8 module, u8 col, u8 row` | |
| 35 | CMSessionNumberRequest 0x177e0 | Q_VERSION_CNT | E→S | 2 | **S** (even for slots) | – | `u8 slot (0..3, 4 = perf)` | `36` |
| 36 | CMSessionNumberDump 0x17994 | – | S→E | – | – | 1 | `u8 slot, u8 session` | |
| 37 | CMDumpBubbleDestination 0x1785e | S_SET_PATCH | E→S | 2 | S / T-void | – | `u8 0, u8 bank 0, u8 prog 0, str16 name` (always `37 00 00 00 name`) | heads an upload bubble (§8.1) |
| 38 | CMSessionPatchRelease 0x1794a (`kMesPatchRelease`) | R_PATCH_VERSION_CHANGE | S→E | – | – | 1 | `u8 slot, u8 session` | §5.3 |
| 39 | (blink, not a molecule) | R_LED_DATA | S→E | – | – | – | §10 | |
| 3A | (blink, not a molecule) | R_VOLUME_DATA | S→E | – | – | – | §10 | |
| 3B | CMClockInfoRequest 0x186d6 | Q_MASTER_CLOCK | E→S | 2 | S | – | – | `5D` (Verhue also saw `3F`) |
| 3C | CMCompletePatchRequest 0x16220 | Q_PATCH | E→S | 2 | T | – | – | all patch sections (§8.2) |
| 3D | CMDumpOneRequest 0x18908 | S_MIDI_DUMP | E→S | 2 | S | – | – | menu "Dump One": the synth dumps on its MIDI OUT [I] |
| 3E | CMPerformanceModeChange 0x189d6 | S_SET_PARAM_MODE | E→S | 0 | S [I] | – | `u8 perfMode, u8 flag2(?)` | Verhue polarity contradictory |
| 3F | CMPerformanceHeaderParam 0x16d86/0x16dfe | S_SET_MASTER_CLOCK | both | 0 | P | 3 | `u8 scope (0xFF = global), u8 param, [u8 value if scope > 3 && param < 3]`; param 0 = clock run, 1 = BPM, 2 = keyboard range [I] | |
| 40 | CMParamChange 0x15008/0x155b6 | S_SET_PARAM | both | **1** | RT | 2 | `u8 loc, u8 module, u8 param, u8 value(0..127), u8 variation` | no reply; also sent unsolicited by the synth |
| 42 | CMCustomData 0x15d68 | S_SET_PARAM_LABEL | E→S | 0 | T | – | `u8 loc, u8 module, u8 n, n×u8` (module custom data = Verhue's "param labels") | |
| 43 | CMMorphChange 0x150a4/0x15360 | S_SET_MORPH_RANGE | both | 0 or 1 | T or RT | 2 | `u8 loc, u8 module, u8 param, u8 morph(0..7), u8 \|range\|, u8 negative, u8 variation` | realtime while dragging |
| 44 | CMParamSettingCopy 0x160fe/0x160c2 | S_COPY_VARIATION | both | 0 | T | 2 | `u8 from, u8 to` (8 = init variation) | |
| 4A | CMModuleDump (`CModuleData_11`) | C_MODULE_LIST | both | 0 | T | 2 | (sect) | |
| 4B | CMModuleRequest 0x12bb6 | – | E→S | 2 | T | – | `u8 loc` | `4A` |
| 4C | CMModuleParamRequest 0x12bfc | Q_PARAMS | E→S | 2 | T | – | `u8 loc` | `4D` |
| 4D | CMModuleParamDump (`CModuleParamData_11`) | C_PARAM_LIST | both | 0 | T | 2 | (sect); **10 variations over USB** [V] | |
| 4E | CMModuleNameRequest 0x12c42 | – | E→S | 2 | T | – | `u8 loc` | `5A` |
| 4F | CMModuleCustomDataRequest 0x12c88 | Q_PARAM_NAMES | E→S | 2 | T | – | `u8 loc` | `5B` |
| 50 | CMCableConnect 0xd46c | S_ADD_CABLE | E→S | 0 | T | – | `pad3, b1 1(not-dump), b1 isVA, b3 color, u8 fromModule, u8 fromConn\|0x40·isOutput, u8 toModule, u8 toConn\|0x40·isOutput` | |
| 51 | CMCableDelete 0xd2c6 | S_DEL_CABLE | E→S | 0 | T | – | `pad6, b1 1, b1 isVA`, from, to (as 50) | |
| 52 | CMCableDump (`CCableData_11`) | C_CABLE_LIST | both | 0 | T | 2 | (sect) | |
| 53 | CMCableRequest 0xd210 | – | E→S | 2 | T | – | `u8 loc` | `52` |
| 54 | CMCableRecolor 0xd398 | S_CABLE_COLOR | E→S | 0 | T | – | `pad4, b1 isVA, b3 color`, from, to | |
| 55 | CMSendCtrlSnap 0x1679e | S_CTRL_SNAPSHOT | E→S | 0 | T | – | – | the synth sends assigned CC values out its MIDI port [I] |
| 56 | CMPlayNote 0x187a8 | S_PLAY_NOTE | E→S | 0 | S | – | `u8 0 = on / 1 = off, u8 note` | §12 |
| 58 | CMParameterPageFocusRequest 0x16278 | – | E→S | 2 | T | – | – | `2D` |
| 59 | CMGlobalParameterPageFocusRequest 0x16e7a | M_UNKNOWN_2 | E→S | 2 | P | – | – | `1E` |
| 5A | CMModuleNameDump (`CModuleNameData_11`) | C_MODULE_NAMES | both | 0 | T | 2 | (sect) | |
| 5B | CMModuleCustomDataDump (`CModuleCustomData_11`) | C_PARAM_NAMES | both | 0 | T | 2 | (sect) | |
| 5D | CMSysClockInfo 0x188a8 | R_EXT_MASTER_CLOCK | S→E | – | – | 1 | `u8 flag (external? [I]), u16 rate (BPM)` | |
| 5E | CMPerfKnobMapRequest 0x16e4e | Q_GLOBAL_KNOBS | E→S | 2 | P | – | – | `5F` |
| 5F | CMPerformanceKnobMapDump (`CKnobMapData_11` mode 1) | C_KNOBS_GLOBAL | both | 0 | P | 3 | (sect) | |
| 60 | CMCtrlMapDump (`CCtrlMapData_11`) | C_CONTROLLERS | both | 0 | T | 2 | (sect) | |
| 61 | CMCtrlMapRequest 0x162a4 | – | E→S | 2 | T | – | – | `60` |
| 62 | CMKnobMapDump (`CKnobMapData_11` mode 0) | C_KNOBS | both | 0 | T | 2 | (sect) | |
| 63 | CMKnobMapRequest 0x1624c | – | E→S | 2 | T | – | – | `62` |
| 65 | CMMorphMapDump (`CMorphMapData_11`) | C_MORPH_PARAM | both | 0 | T | 2 | (sect) | |
| 66 | CMMorphMapRequest 0x162d0 | – | E→S | 2 | T | – | – | `65` |
| 68 | CMCurrentNotesRequest 0x167ca | Q_CURRENT_NOTE | E→S | **0** | T | – | – | `69` |
| 69 | CMCurrentNotesDump (`CCurrentVoiceData_11`) | C_CURRENT_NOTE_2 | both | 0 | T | 2 | (sect): last note + held notes (note, attack vel, release vel) | snapshot stored in the patch file, not a live event stream |
| 6A | CMParamSettingFocus 0x15f44/0x15efe | S_SEL_VARIATION | both | 0 | T | 2 | `u8 variation` (R: `pad1, b7`) | |
| 6E | CMTextpadRequest 0x16568 | Q_PATCH_TEXT | E→S | 2 | T | – | – | `6F` |
| 6F | CMNotePadDump (`CTextpadData_11`) | C_PATCH_NOTES | both | 0 | T | 2 | `6F u16 len(≤ 0x400)` + text (no NUL) | |
| 70 | CMFlushBlink 0x162fc | M_UNKNOWN_6 | E→S | 0 | T | – | – | reset/restart the slot's LED stream [I] |
| 71 | CMPatchLoadRequest 0x1651e | Q_RESOURCES_USED | E→S | 2 | T | – | `u8 isVA` | `72` |
| 72 | CMPatchLoad 0x163da | R_RESOURCES_USED | S→E | – | – | 2 | `u8 isVA, u14, u14, u8, u14, 6×u14, u14, u16 hi, u16 lo, u14` (DSP resource counters; `u14` = 7-bit BE pair) | Verhue may get two in one message |
| 74 | CMBarbMapChangeNotify 0xdd58 | – | S→E | – | – | 2 | – | slot knob map changed → re-request `63` |
| 75 | CMPerfBarbMapChangeNotify 0xddaa | – | S→E | – | – | 3 | – | global knob map changed → `5E` |
| 7D | CMSynthUnlockEditorSync 0x18862 | S_START_STOP_COM | E→S | 0 | S | – | `u8 1` = editor sync begins, `0` = done | §6 |
| 7E | kMesException | R_ERROR | S→E | – | – | – | `u8 code` | §11 |
| 7F | kMesAck | R_OK | S→E | – | – | – | – | end of a reply |
| 80 | CMMidiLearn 0x18af4 | R_MIDI_CC | S→E | – | – | 1 | `u8 slot (0..4), u8 cc (0..119)` | reply to `81`, and unsolicited [V-cap] |
| 81 | CMMidiLearnRequest 0x18a9a | M_UNKNOWN_1 | E→S | 2 | S | – | – | `80` |
| 90 | CMMutaLock 0x137fc | – | E→S | 0 | T | – | `u8 loc, u8 module, u8 locked` | mutator lock; sent but probably ignored by older OS [I] |

Counts:
* 102 molecule ids with a wire representation, plus 4 port-level ids (39, 3A, 7E, 7F);
* 87 written by the editor, 50 receivable (= the 50 `CMoleculeFactory` builders);
* 5 Muta molecules are editor-only (never sent).

The `WriteStream`/`ReadStream` addresses in the table are the starting points for checking any field bit by bit.
Ids 0x24, 0x41, 0x45-0x49, 0x57, 0x5C, 0x64, 0x67, 0x6B-0x6D, 0x73, 0x76-0x7C, 0x82-0x8F are unused by the editor.

### 7.1 `03` CMSynthDataDump body (`CSynthMap::WriteStream` 0x120410 / `ReadStream` 0x12012e) [C], Verhue names in ()
```
str16 synthName
b1 perfMode, b7 patchSortMode, b7 perfSortMode, pad1        (Verhue: byte 0x80 "perf mode?")
u8 focusBank, u8 focusProg                                   (PerfBank, PerfLocation)
b1 memoryProtect, pad7
u8 midiChannel[4]   (0..15, 0x10 = off)                      (MidiChannelA..D)
u8 globalChannel, u8 sysExId                                 (16 = off / all)
b1 localOn, pad7
u8 programChangeMode, u8 controllerMode                      (Verhue: bits rx<<1|tx)
b1 sendArp?, b1 sendClock, b1 ignoreExtClock, pad5           (Verhue: SendClock<<6 | IgnoreExtClock<<5)
s8 masterTune (cents)
b1 octShiftEnable, pad7
s8 octShift, s8 transpose (semitones)
u8 vibratoRate?                                              (Verhue: 0x00 "unknown")
b1 sustainPolarity, b1 1, pad6                               (Verhue: PedalPolarity<<7 | 0x40)
u8 controlPedalGain
16 × u8 0
```
The molecule writes its own id 0x03 and has **no length field**. The editor sends it only to change perfMode (§6.3).

---------------------------------------------------------------------------------------------------

## 8. Patch / performance transfer, flash banks

### 8.1 Upload into the edit buffer (editor → synth) [C]
* **Patch into slot S**: one bubble, hdr `0x28|S`, **void session** `0x53` (19 molecules):
  ```
  37 00 00 00 <str16 name>                         CMDumpBubbleDestination
  21 4A 4A 69 52 52 4D 4D 4D 65 62 60 5B 5B 5B 5A 5A 6F   .pch2 sections, in this order
  ```
  (`CSynth::DownloadPatch` 0x11dc62, `CPatch::GetCompletePatchDump` 0xd895a → `FileToBubble` 0xd4d76,
  `CPatchFile_13::WriteMolecules` 0x51e7c.)
  * The location order of the paired sections follows the file writer: VA then FX for 4A/52/5A; patch settings (2), VA,
    FX for 4D/5B [I]. See pch2-format §2.
  * Over USB, `4D` carries **10 variations** ("10 Variations must be in a patch for USB", Gen2 Mess:4016) [V].
    Clavia's dump comes straight from the file writer [C]. Whether that writer emits 10 for USB is to be checked [I].
  * The synth answers, then issues `38 S newSession` [I]. The editor takes the new session from it.
  * Verhue: `01 28+s 53 37 00 00 00 name …` with `$53` "?" hard-coded (Gen2 Mess:3993-4020) [V].
* **Performance**: one synth bubble, hdr `0x2C`, void session `0x42` (2 molecules):
  `37 00 00 00 <str16 perfName>` + `1A` = [`29 str16`, `11` sect, 4 × (the 18 patch sections), `5F` sect]
  (`CMCompletePerformanceDump::WriteStream` 0x17248). Verhue: `01 2C 42 37 00 00 00 …` (Gen2 Mess:3193) [V].
  If the synth's perf-mode differs, a `03` synth-data dump with perfMode toggled is sent first.

### 8.2 Download (synth → editor) [C]
* `T: 3C` → reply hdr `0x08|S`, sess = slot session; molecules = the patch sections (21, 4A, 69, 52, 4D, 65, 62, 60,
  5B, 5A, 27 name, 6F, …). The exact list is [I]: every section with destination 2 can occur.
  Verhue [V] also tolerates `2D 00` after `21` (Gen2 Mess:3486-3490).
* Partial refresh: `20`, `28`, `4B/4C/4E/4F/53 loc`, `61`, `63`, `66`, `68`, `6E`, each answered with its section.
* Performance: `P: 10` → `29 name` + `11`; `P: 5E` → `5F`; slot data is read per slot as above.

### 8.3 Flash (bank memory) operations [C] / [V]
| operation | request | reply |
|---|---|---|
| list names | `S: 14 type bank prog` (repeated from the entry after the last one received while `16` ends with tag `05`) | `16 …` (§7), optionally `13 lo hi` |
| load into slot/perf | `S: 0A slot bank prog` | response, then `38`/`1F` release → re-read |
| store slot/perf | `S: 0B slot bank prog` | `0D` result (Verhue: + `16` with flag 0) |
| delete | `S: 0C type bank prog origin` | `15` and/or `0D` |
| delete range | `S: 0E type b1 p1 b2 p2 origin` | `12` |
| read file from flash | `S: 17 type bank prog` | `18 code …` then `19 type bank prog name size ver <size bytes>` |
| write file to flash | `S: 19 type bank prog name size ver <bytes>` | `18`/`0D` |

`19` raw data = the binary part of a `.pch2`/`.prf2` (version byte, type byte, sections, CRC), **9 variations** as in
files (Gen2 Mess:2595-2670; Verhue size = data + 1, marked "?") [V].
Verhue writes `type` = 0 even for performances; Clavia writes the real file type [C].
Origin: 0 = bank-list window (`CFlashData`), 1 = file transfer (`CFlashDumper`).

---------------------------------------------------------------------------------------------------

## 9. Live parameter changes, morph, variation, knobs, MIDI CC

| action | message | notes |
|---|---|---|
| move a knob / parameter | `RT: 40 loc module param value variation` | no reply. Verhue coalesces per (slot, loc, module, param, variation) and sends only when nothing is outstanding (Gen2 USB:1476-1580) |
| select parameter | `RT: 2F 00 loc module param` | no reply |
| morph range drag | `RT: 43 loc module param morph |range| neg variation` | realtime while dragging; a final `T: 43 …` (normal, acked) on release [I] |
| mode parameter (waveform etc.) | `T: 2B loc module selector value` | acked |
| select variation | `T: 6A v` | acked |
| copy / init variation | `T: 44 from to` (to/from 8 = init) | acked |
| knob assign / deassign | `T: 25 …` / `T: 26 00 knob` | + `2D page` |
| global knob | `P: 1C …` / `P: 1D 00 knob` / `P: 1E page` | Verhue sends these in a T frame |
| MIDI CC assign | `T: 22 loc module param cc` / `T: 23 cc` | |
| module add / move / delete / recolor / rename / uprate | `30` / `34` / `32` / `31` / `33` / `2A` | one T bubble may hold several molecules (Verhue builds composite edits plus an undo bubble) |
| cables | `50` / `51` / `54` | |
| patch notes | `T: 6F len text` | |
| synth → editor panel edits | unsolicited `40`, `2F`, `6A`, `43`, … with hdr = slot (no response flag) | routed only if the session matches |

The realtime variation field: 0..7 = variations 1..8; 8 = init (patch-file variation 8). Over USB, `4D` has a 10th
variation whose meaning is unknown [V].

---------------------------------------------------------------------------------------------------

## 10. LED and meter streams (0x39 / 0x3A)

* **Unsolicited**, pushed by the synth, never requested or acknowledged [C]: no `Initialize(0x39/0x3A)` in the editor.
* Message: `01 <slot> <sess> 39|3A <start> <data…> crc`. hdr = slot 0..3 with no response flag. They are usually
  extended (> 15 bytes).
* Clavia handles them in the interrupt path (`CSynthPortUSB::InterruptHandleData` 0x106f72 →
  `CSynthPort::InterruptHandleBlinkData` 0x1041ea). They are accepted only when the slot's patch is connected **and both
  `72` patch-load replies have arrived**; otherwise they are dropped. Rate: whatever the synth sends. The editor polls the
  interrupt pipe at most every ~10 ms. Neither code base documents the device-side rate.
* **0x39 dual-bit LEDs** [C]:
  * `d[1]` = start index; then 2 bits per LED, **4 LEDs per byte, LSB-first** (bits 1:0 = LED i, 3:2 = i+1, 5:4 = i+2,
    7:6 = i+3), from `d[2]`;
  * indices start..39 are decoded (40 LEDs per slot, `CLedBlinkData::SetDualLed` 0x101e88); with start 0 that is 10 data
    bytes;
  * the handler does no length check.
* **0x3A multi-LED / VU** [C]:
  * `d[1]` = start index; then one 16-bit word per entry for indices start..39 (40 per slot,
    `CLedBlinkData::SetMultiLed` 0x101e4a);
  * Clavia reads each word as a raw i386 `ushort` (**little-endian**, no swap);
  * Verhue reads `[unknown byte][value byte]` per entry [V];
  * **the byte order is an open question** (§14).
* Index → module mapping [C] (`CPatch::UpdateBlink` 0xdb45e, `CPatchBackground::UpdateBlink` 0xdf744):
  * VA modules first, then FX, each sorted by module index; one entry per LED **group** of the module, in group order;
  * single-LED groups go to the 0x39 list, multi-LED groups and VU meters to the 0x3A list;
  * the counters run on across VA → FX.
* Value decoding [C]:
  * single LED: value 0..3 (`CPnlLed::SetValue` 0xcec58);
  * VU meter: 0..0x7E level, > 0x7E = clip (`CPnlVUMeterABC::SetValue` 0xcfc88);
  * LED strip: `(v & 0x3000) == 0x3000` → bitmask; `v == 0xFFF` → all on; else the index of the lit LED.
* Verhue [V] agrees on VA-first ordering (Gen1/Gen2; Gen3 does FX first). Its 0x39 decoder has a shift bug (`shr j`
  instead of `shr 2j`). Do not copy it.
  Verhue's table of modules with LEDs: see its `LedDefs`/`MiniVUDefs` in Gen2/Common/BVE.NMG2Data.pas:6017/6271.
* `T: 70` CMFlushBlink is sent once after each load report. The editor invalidates its LED cache (all 0xFFFF) at the
  same time, so the synth presumably resends the full LED state [I].

---------------------------------------------------------------------------------------------------

## 11. Timeouts, retries, errors, disconnection

### 11.1 Timeouts [C]
* `CTimeoutManager::SuggestAction` 0x103f56: while waiting for a reply, if more than **10 000 ms** have passed since
  max(lastSend, lastReceive) → timeout.
  * Not yet connected (status 1/2) → status 2 "Still looking…", and the version request is retried.
  * Connected → retry counter vs `CSynthPort::kErrorRetry` (`__data` 0x293148 = **0**, never written) → the **first
    timeout is fatal**: `MajorError(7 "Lost Contact")`.
  * `ReSendBubble`/`CBubblePort::ReSendMessages` (resend of the saved cleaner message) is therefore unreachable in
    practice.
  * Any unsolicited traffic (LEDs) refreshes lastReceive, which postpones the timeout.
* Bulk-IN read of an extended message: 5 s → `XUSBError` → fatal (status 12).
* Verhue: no request timeout at all; 5 × 500 ms bulk read retries [V].

### 11.2 Error statuses [C] (`CSynthPort::SetInfoString` 0x104446; strings from `SSTR` resources 0x17D978xx)
| status | text | cause |
|---|---|---|
| 1 | Looking... | initial |
| 2 | Still looking... | no version reply in 10 s |
| 3 | Unsupported Model | version byte 1 ≠ 0x0A or unknown model |
| 4 | Version mismatch (<model> Vx.yy) | Rack / Native, or protocol word ≠ 0x0012 |
| 5 | New device found | the version info changed while connected |
| 6 | Version Corrupted | `XClaviaBase` exception while parsing, e.g. `XMUnregisteredMolecule` |
| 7 | Lost Contact / Check USB cables | 10 s reply timeout; `XUSBError` while handling data |
| 8 | Update Mode | version mode ≠ 0 |
| 10 | Uplink Checksum Error | never set (CRC failures are dropped silently) |
| 11 | Synth Exception | synth sent `7E code` (`XMHostMidi`) |
| 12 | Major Error | other exceptions, interrupt-path fatal flag (`XUSBBufferOverRun`, bulk timeout) |
| 13 | <model> Vx.yy | connected |

`7E` codes (`XMHostMidi` text):
* 3 = "Error in synth.";
* 4 = "Down link check sum error." (the synth received a bad CRC);
* 5 = "Stream execute error. Synth may be corrupted…";
* 6 = "Unfinished bubble error.";
* 0–2 = no text.

Every status other than 1, 2 and 13 makes `CPortManager::Idle` **destroy the port** and show an alert. There is no
automatic reconnect until the device is re-enumerated (replug or wake).

### 11.3 Disconnection [C]
* Unplug: `DeviceRemovedCallback` → `LowLevel_RemoveDevice` 0x108d60 (match on locationID) → port destroyed, alert
  "Lost Contact".
* `~CSynthPortUSB` 0x106894 → `CMacUsbHandle::Close` 0x107a4e: AbortPipe(interrupt), AbortPipe(bulk-in),
  USBInterfaceClose, USBDeviceSuspend(true), USBDeviceClose. **No goodbye message is sent.**
* Verhue Gen2 sends nothing on close and resets the device (`libusb_reset_device`) to break the blocking interrupt read [V].
  The older Verhue code sent `7D 01` and waited up to 3 s for `7F`.
* Sleep: the editor vetoes idle sleep (`IOCancelPowerChange`). On WillSleep it aborts the interrupt pipes. On wake every
  port is destroyed and the devices are re-matched, giving a full handshake.

### 11.4 Multiple synths [C]
* Up to 4 ports. Port number = the first free `CUSBPort` slot at arrival; it is not tied to a socket or serial number.
* Ports are fully independent (own sessions, handshake and queues). They are serviced round-robin, one per idle tick.
* There is one global focus (port, slot). Only the lowest-numbered port asks the synth for its focus (`08`); the others
  are told `09 04`. LEDs and virtual-keyboard notes are only handled for the focused port.

---------------------------------------------------------------------------------------------------

## 12. MIDI over USB, USB-MIDI class, and device ownership

This section answers the requirement that the VST3/AU plugin send DAW MIDI to the G2 over USB without a MIDI cable.

### 12.1 What the G2 USB protocol can carry [C unless marked]
* The **complete** list of editor→synth molecule ids is in §7 (87 writers). None of them carries
  - velocity,
  - MIDI channel,
  - control-change values,
  - pitch bend,
  - aftertouch,
  - program change (as a MIDI event),
  - MIDI clock,
  - or arbitrary MIDI/SysEx bytes for pass-through.

  This is **proven** by the exhaustive molecule list (CMoleculeFactory plus every `CMIDIOutStream::Initialize` caller).
  The binary does not link CoreMIDI (`otool -L`: IOKit, Carbon, libstdc++, libgcc, libSystem; no `_MIDI*` imports).
* The **only note-like message is `56` CMPlayNote** (0x187a8): `01 2C 41 56 <0 = on | 1 = off> <note 0..127>`.
  * There is no velocity field (the synth uses a fixed velocity [I]) and no channel.
  * Header slot = 4 (synth level). **No slot can be addressed.** The synth decides which slot(s) sound, presumably the
    keyboard-focus / slot-selection (`0F`/`07`) state [I].
  * It is a **normal (acknowledged) bubble**, so each note costs one round-trip and **blocks** while any other bubble is
    outstanding. The editor uses it only for its on-screen keyboard: monophonic, one note at a time, the stop is sent
    before the next start, and a held note is stopped when the port loses focus
    (`CSynth::CleanDirtySynthData` 0x11e7aa, `VirtualKeyboardStartNote/StopNote` 0x11def6/0x11d150).
  * Whether the synth accepts overlapping note-ons (polyphony) from `56` is **unknown** [I].
  * Latency: one USB round-trip plus the synth's reply time (unmeasured) plus the editor's 10 ms interrupt re-arm delay.
    Throughput is bounded by the stop-and-wait rule.
* **What a plugin CAN do over USB:**
  * set any patch parameter in realtime with `40` (RT, no ack, no round-trip): this is how DAW automation / CC → parameter
    mapping should be implemented (host-side mapping CC → (slot, loc, module, param));
  * morph amounts (`43`, RT);
  * variation select (`6A`);
  * load patches/performances from flash (`0A`) as a "program change" substitute;
  * crude monophonic note-on/off without velocity (`56`).
* **Other MIDI-related messages are not event transports:**
  * `68`/`69` CurrentNotes: a snapshot of held notes for saving in the patch;
  * `80` MidiLearn: reports only the CC *number* last received on the synth's MIDI IN, plus the slot; no value
    [V-cap `82 01 04 00 80 00 3F 30 40` = slot 0 CC 63];
  * `55` SendCtrlSnap and `3D` DumpOne: make the synth emit data on its own MIDI OUT port [I];
  * `22`/`23`/`60`/`61` assign MIDI CCs to parameters inside the patch (configuration, not events);
  * `03` synth settings: MIDI channels per slot, local on/off, CC/program-change send/receive flags.
* **Conclusion:** the plugin cannot deliver general DAW MIDI (polyphonic notes with velocity, CC, pitch bend, program
  change) to the G2 over the vendor USB protocol. Options:
  1. a real MIDI connection (DIN cable / USB-MIDI interface);
  2. a hybrid: notes over a MIDI cable, parameter automation over USB `40`;
  3. a best-effort monophonic `56` note path without velocity, which must be validated on hardware before relying on it.

  Undocumented firmware messages cannot be excluded (ids 0x41, 0x45–0x49, 0x57, 0x5C, … are unused by the editor), but
  nothing in either code base suggests a MIDI pass-through.

### 12.2 USB-MIDI class interface? [C]/[I]
* The editor matches the **device** by VID/PID, iterates **all interfaces** and seizes them, and classifies pipes only by
  transfer type: 1 interrupt-IN, 1 bulk-IN, 1 bulk-OUT (`InitPipes` 0x107aca). It never checks interface class/subclass.
  The handle pointer is overwritten per interface, so with several interfaces the last one would win — evidence that the
  device has **a single (vendor) interface** [I].
* Verhue claims interface 0 with exactly 3 endpoints (0x81/0x82/0x03) and nothing else [V].
* No code in either editor speaks USB-MIDI (Audio class 1, subclass 3) to the G2. Clavia's `CSynthPortMIDI` (Nord Modular
  G1-style SysEx: `F0 33 <flags> 06 …`, 7-bit packing, `PackSysex` 0x105e68) is **dead code**: no constructor caller, and
  `CPortManager::CreatePort` only builds USB ports.
* **The G2 does not appear to expose a USB-MIDI class interface** [I, high confidence]. Confirm on hardware with
  `lsusb -v` / System Information.

### 12.3 Two host processes / ownership [C]/[I]
* USB level: the editor opens the device with exclusive access. On `kIOReturnExclusiveAccess` it retries 20 × 1 s, then
  gives up **silently** (the exception is swallowed in `CUSBManager::LowLevel_NewDevice` 0x10937a). It only tries again on
  a new device match (replug) or wake. When it does open the device, it calls **ResetDevice** and
  **USBInterfaceOpenSeize**, so it can steal an interface another macOS client holds open but not seized [C].
  * On Linux/Windows with libusb, `libusb_claim_interface` fails with `LIBUSB_ERROR_BUSY` for a second process [I].
  * **Only one process can own the G2 at a time.** A plugin and a standalone editor (or two plugin instances) need a
    single owner process (a daemon/broker) that multiplexes them.
* Protocol level: **there is no ownership token, login or client id.**
  * Session numbers version the slot contents (the synth increments them when a patch changes). They do not identify the
    editor.
  * `7D 01`/`7D 00` (CMSynthUnlockEditorSync) only bracket an editor sync. It is not exclusive [I].
  * `CBubblePort::LockUser` is local UI state.
  * Two editors talking concurrently through some multiplexer would interleave request/reply pairs. Both would see the
    other's changes only through `38`/`1F` releases or unsolicited edits, and the stop-and-wait discipline would have to be
    global per device. A broker must serialise all non-realtime bubbles and route each response to the client that issued
    the outstanding request.
* Several G2 units: independent ports (§11.4).

---------------------------------------------------------------------------------------------------

## 13. Clavia vs Verhue differences (summary)

| topic | Clavia v1.62 | Verhue |
|---|---|---|
| endpoint discovery | by pipe type | hard-coded 0x81/0x82/0x03 |
| interrupt nibble test | `b0 & 3` | `b0 & 0x0F` |
| bulk read | single transfer, 5 s total | accumulates, 5 × 500 ms |
| request timeout | 10 s → fatal "Lost Contact" | none (stalls forever) |
| bad CRC | dropped silently | logged, processed |
| hdr/sess constants | flag bits + molecule count | `$28+s`, `$2C`, `$38+s`, `$41`, and hard-coded `$42`/`$53` |
| init order | §6.3 (with `08`, `58`; no separate `28`) | §6.4 |
| names | molecule semantics | several misnomers: `13` "R_LIST_NAMES" is flash usage; `3E` "S_SET_PARAM_MODE" is perf mode; `42`/`5B`/`4F` "param labels/names" are module custom data; `71`/`72` "resources" is DSP patch load; `81`/`80`/`59`/`70` "unknown" |
| global knob assign | performance-level molecule (dest 3) | sent inside a slot frame |
| LED 0x39 decode | correct (shift 2j) | bug (shift j) |
| LED 0x3A entry | 16-bit host-order word | [unknown byte][value] |
| `19` outer type | real file type | always 0 |
| on close | no message | none (old code: `7D 01`) |

---------------------------------------------------------------------------------------------------

## 14. Open questions (need hardware or more RE)

1. Exact USB descriptors: max packet sizes, interrupt bInterval, interface class/count; and whether a USB-MIDI interface
   exists (expected no).
2. Version reply: total length; meaning of bytes 4–5, 10–25, 26–29, 30–31, 32–33; is model 0 the 49-key G2 (string
   resource `kModularG237ModelString`) and 1 the G2X?
3. Which commands are answered with a bare `7F` and whether a reply ever carries data *and* `7F`.
4. 0x3A word byte order and the meaning of the first byte; the device-side LED/meter rate; whether `70` triggers a full
   resend.
5. `56` PlayNote: velocity used, slot selection, polyphony, timing.
6. Whether the G2 itself rejects bubbles with a stale session number (expected; `7E`?) and which `7E` codes appear.
7. `3E` second byte; `3F` param semantics; `5D` flag; `16` first flag byte; `18` second byte; `72` counter names.
8. Whether the device ever splits a bubble across several messages (Begin/End flags on input), and the high nibble of
   extended announcements.
9. The 10th `4D` variation over USB; whether Clavia's upload path writes 9 or 10 variations.
10. `7D` effect on the synth (front-panel lock? suppression of unsolicited traffic?).

---------------------------------------------------------------------------------------------------

## 15. Test vectors (`tests/protocol/`)

Generated by `python3 -I tests/protocol/gen_vectors.py` (deterministic; it asserts every vector that has an independent
expected value).

| file | content |
|---|---|
| `framing.json` | CRC check values, version request, minimal normal and realtime frames, embedded inbound packet (Verhue capture) |
| `init_sequence.json` | Clavia order (§6.3) with synthetic replies: version reply, sessions, synth data, ack |
| `init_sequence_verhue.json` | Verhue's init request frames, cross-checked against Verhue-derived bytes |
| `param_changes.json` | `40`, `2F`, `43` realtime and normal, `6A`, `44`, inbound panel changes |
| `patch_edits.json` | Verhue-captured variation, notes and param-paste frames; module/cable/knob/CC/name edits |
| `led_vu.json` | 0x39 and 0x3A inbound (extended) messages, FlushBlink |
| `session_numbers.json` | `35`/`36`, `38`, `1F`, Verhue compound `1F` |
| `midi_and_notes.json` | `56` on/off, `55`, `81`/`80` (with the captured `80`) |
| `errors_and_acks.json` | `7F`, `7E code`, CRC-corrupted packet (`valid: false`) |
| `bank_ops.json` | `14`/`16` name list, `0A`, `0B`/`0D`, `0C` |
| `patch_transfer.json` | upload bubble prefixes (`0x53`/`0x42` void-session counts), `3C` download request |

Provenance per step: `verhue-capture` (bytes found in Verhue's sources), `clavia-derived` (encoded with the Clavia
serializer rules above), `verhue-derived`, `synthetic` (device replies with invented but layout-correct values).
**No vector comes from a capture of the real G2**; the captured ones come from Verhue's notes about the original editor.
