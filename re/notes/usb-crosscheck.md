# USB protocol cross-check: G2fresh against the original and three hardware-tested editors

Date: 2026-10-11. This is a read-only audit; nothing in `proto/`, `usb/`, `bridge/`, `ui/` or `tests/` was changed.
It covers USB device handling, start-up, dumps and bank operations, streams from the synth, and errors.
Per-edit messages are left out: the live-editing work checks those itself. Section 8 passes on the hardware facts the
other editors found about edits.

## Sources

| tag | what | version |
|---|---|---|
| **G2fresh** | main checkout, file:line | `235c7f7` |
| **Orig** | Clavia editor v1.62 (Ghidra, `re/out/decomp/`), function address | - |
| **Cap** | USB captures of a **real G2X** in g2fx's repo, `data/capture/*.pcapng` (see below) | g2fx `89ffa2b` |
| **g2fx** | sirlensalot/g2fx, `src/main/java/org/g2fx/g2lib/...` | `89ffa2b486eef89419036cd0a9f8639a9645e43d` |
| **Web** | ty-art-ty/g2-webeditor (its backend vendors g2fx's g2lib at g2fx `e75c6d0`) | `ded17c5a1d9970e65e0bd1aabbc44b9cde3097fa` |
| **JB** | JanBurp/NordModularG2-Editor (AGPL-3, cited only), `cli/src`, `cli/include` | `a1a7e83c44e9ded70be46e1a312e7d94fe9b6440` |
| **Verhue** | `third_party/nord_g2_editor` (local clone) | `870d4a8` |

### The captures are the strongest evidence here

g2fx ships macOS USB captures (`tcpdump -i XHC20`, `data/capture/capture.sh`) of a real **G2X**. The device
descriptor's product string is "Nord Modular G2X", and the version reply says model 1, OS V1.40.

**The captured editor is very likely Clavia's own.** I infer this; no file says so. The evidence:
- `capture-001` (2026-05-14) uploads a whole performance, but g2fx only gained performance upload on 2026-05-29
  (commit `228365d`, "passing regressions", i.e. matching the capture).
- g2fx's `CaptureSender` (src/test/.../g2gui/CaptureSender.java) treats the captures as the reference its own output
  must match.
- The traffic carries behaviour specific to v1.62 (details in the power-on trace below):
  - `09 00` instead of `08` (`CPerformance::CleanDirtyData` 0x100e5a, action 2);
  - `28` after `3C`, but no `58` (`CPatch::CleanDirtyData` 0xd601e is dirty-flag driven);
  - a `70` after every load report.

Treat the captures as "the original editor on real hardware". Check this before relying on it for anything not
corroborated by the code.

I decoded them with a throwaway pcapng reader that is not in the repo. It reads the darwin USB header: request type
at byte 3, I/O length at bytes 4..7, endpoint at byte 30.

Power-on trace (`poweron2.pcapng`). `I:` is the interrupt packet, `B:` the bulk-IN transfer, `ZLP×2` two
zero-length bulk completions:

```
OUT 00 05 80 91 88            I: 01 00 42 ..   B: ZLP×2, then 66 bytes  80 0a 01 00 00 1c 00 8c 00 12 "SCUMBUSTER" ...
OUT 01 2c 41 7d 01            I: 62 01 0c 00 7f ..             (reply session byte 00, not 0x40)
OUT 01 2c 41 35 04            I: 82 01 0c 40 36 04 00 ..
OUT 01 2c 41 02               B: ZLP×2, 55 bytes  01 0c 00 03 "SCUMBUSTER" ...
OUT 01 2c 41 81               B: ZLP×2, 17 bytes  01 0c 00 80 00 ff 80 01 ff 80 02 ff 80 03 ff
OUT 01 2c 00 10 / 59 / 09 00
for S in 0..3:  35 0S, 3C, 28, 68, 6E, 71 01, 71 00, 70, 2E
OUT 04, 3B, 5E, 14 0 b p ... (patch names), 14 1 b p ... (performance names), 7D 00
then unsolicited:  05 voices (hdr 04), 72+72 per slot (hdr = slot), and the editor answers each with T: 70
```

Across the 18 capture files there are 1002 extended messages. 984 arrive after **two zero-length bulk-IN
completions**, 10 after one, and 6 on the first read.

---------------------------------------------------------------------------------------------------

## Findings, ranked by risk of failing on a real G2

| # | risk | finding | right answer | fix |
|---|---|---|---|---|
| H1 | **High** | The libusb transport takes the first **zero-length** bulk-IN completion as the message. On a real G2, 996 of 1002 captured extended messages (the version reply included) were preceded by one or two ZLPs, so the message is lost and later reads desynchronise. | Orig + Cap + all three editors: retry empty reads (Clavia: up to 5 s) | 2-3 h |
| H2 | **High** | A load from flash (`0A`) gets its `38` / `1F` release **inside the reply**. G2fresh only re-reads on an unsolicited release, so the slot or performance is never re-read. The mirror stays stale, LEDs map onto the old patch, and "open from bank" never opens. | Orig (release handlers do not care whether it is a reply) + Cap | 2 h |
| H3 | **High** | Slot names are never read from the synth: the `3C` reply has no `27`, and G2fresh never sends `28`. Names stay empty; a performance-bound resend uploads a slot with an empty name. | Orig (dirty-flag model) + Cap: `28` follows `3C` | 1 h |
| H4 | **High** | `0x3A` meter/strip words are decoded big-endian. All 177 captured `3A` messages carry the value in the **first** byte of each word, the second always 0. All VU meters and LED strips will be wrong. | Orig (raw little-endian read) + Cap + all three editors | 30-60 min |
| M1 | Medium | A patch upload is answered by `36 slot newSession` in a reply on the slot's header. G2fresh ignores a `36` there and expects a `38`, which never comes. The session is stale until the next unsolicited message, which then triggers a full read-back that replaces the open document. | Orig (molecules routed by destination) + Cap | 1-2 h |
| M2 | Medium | The bulk-IN read has no timeout and its length is never checked against the announcement. There is no USB reset when the device is opened. A stale or lost bulk transfer leaves the transport stuck, or permanently one message behind. | Orig: 5 s bulk deadline, `ResetDevice` at open | 1-2 h |
| M3 | Medium | Bulk-OUT timeout is 2 s; Clavia uses 5 s. A timeout mid-frame leaves the G2 half-way through a message (JB: `7E 3`, needs a power cycle). | Orig | 5 min |
| M4 | Medium | No cap on frame size. JB measured on hardware that a ~34 KB message wedges the G2 until a power cycle, and the Clavia editor fails the same way. G2fresh only splits at 0xFFFF. | Hardware (deliberate, protective deviation) | 30 min |
| M5 | Medium | The in-process protocol emulator (`proto/src/emulator.cpp`) and the g2emu host model differ from the hardware in exactly the places H1-H3 and M1 hit, so the session tests encode wrong expectations. | Cap | 3-4 h |
| L1-L12 | Low | Section 7. | | |

---------------------------------------------------------------------------------------------------

## 1. USB device handling

### Side by side

| item | G2fresh | Orig | Cap (real G2X) | g2fx / Web | JB | Verhue |
|---|---|---|---|---|---|---|
| VID/PID | 0x0FFC/0x0002 (`usb/include/g2/usb/libusb_transport.hpp:31-32`) | same (`CPortManager` 0x102ff2) | `12 01 10 01 00 00 00 10 fc 0f 02 00 ...`: USB 1.1, EP0 16 bytes, 1 configuration | same (`usb/UsbService.java:19-21`) | same (`defs.h:98-104`) | same |
| configuration | set only if different (`libusb_transport.cpp:126-131`) | `SetConfiguration(first)` after `ResetDevice` (`CMacUsbHandle::OpenDevice` 0x107e9c) | host sends SET_CONFIGURATION 1 | none (`UsbService.java:121-128`) | none | Windows only |
| USB reset at open | **no** | **yes**, `ResetDevice` (0x107e9c step 3) | - | no; Web's systemd unit runs `usbreset` first (`deploy/g2web.service:12`) | no | resets on close |
| interface / endpoints | interface 0; endpoints found by type (`libusb_transport.cpp:132-147`) | every interface, pipes classified by type (`InitPipes` 0x107aca) | 1 interface, class FF/FF/FF; 0x81 interrupt 16 bytes **bInterval 3**, 0x82 bulk 64, 0x03 bulk 64; config 0xC0 self-powered | hard-coded 0x81/0x82/0x03 (`usb/Usb.java:21-23`) | hard-coded | hard-coded |
| kernel driver | auto-detach (`:124`) | n/a | - | none | none | detach if active |
| interrupt read | async, 16 bytes, **no timeout**, re-armed at once (`:201-212`) | 16 bytes, then a 10 ms sleep before re-arming (`HandleInterrupt` 0x108b44) | 16-byte transfers | sync, 500 ms poll (`UsbReadThread.java:59`) | 200 ms poll; a header that lands with TIMEOUT is still used (`g2_io.c:777-784`) | infinite |
| type test | `b0 & 3` (`proto/src/frame.cpp:112`) | `b0 & 3` (0x107194) | only `x1` / `x2` packets seen | `b0 & 0x0F` (`Usb.java:125`) | `& 0x0F` (`g2_io.c:680`) | `& 0x0F` |
| bulk-IN read | exact announced length, **no timeout**; first completion delivered **even if empty** (`:214-225`, `:255-263`) | `RxData` 0x108a7e: loops while a read returns 0 bytes, 1 s per read, 5 s total, then `XUSBError` | **1-2 ZLPs, then the data**, for 996 of 1002 messages | exact length, 5 s, up to 5 tries; 0 bytes counts as a failure and is retried (`Usb.java:161-190`, `UsbMessage.java:9-11`) | exact length, accumulates, 5 × 500 ms (`g2_io.c:645-656`) | accumulates, 5 × 500 ms, zero reads retried (`Gen2/Common/BVE.NMG2USB.pas:1111-1150`) |
| bulk-OUT write | one transfer, **2000 ms** (`:19`, `:326-336`) | one transfer, 5000 ms (`TxData` 0x1088c6) | frames up to 13 651 bytes in one transfer (`capture-016`) | 10 000 ms (`Usb.java:57`) | 5000 ms; resumes a short write; refuses > 32 768 bytes (`defs.h:110-122`, `g2_io.c:930-983`) | infinite |
| incoming CRC | checked, bad ones dropped (`frame.cpp:88-105`) | dropped silently | - | computed, not checked | not checked | logged |
| hot-plug | libusb hotplug, else a 1 s scan (`:83-101`, `:292-300`) | IOKit notifications | - | libusb hotplug only (no Windows) | 100 ms poll after NO_DEVICE (`daemon.c:1025-1058`) | - |

### H1: zero-length bulk-IN packets (High)

**The defect.**
- `onBulk` (`usb/src/libusb_transport.cpp:255-263`) pushes whatever completed and re-arms the interrupt read.
- `Client::bulkIn` (`proto/src/client.cpp:382-390`) clears `expectedBulk_` and parses it.
- An empty transfer fails the CRC and is dropped (`client.cpp:394-397`).

**What the hardware does** (captures). It usually answers the first one or two bulk-IN requests after an
announcement with zero-length packets (two for 984 of 1002 messages, one for 10), and sends the data after that. libusb completes a transfer on a ZLP with `actual_length = 0`
and status COMPLETED. WinUSB (by default) and Linux usbfs behave the same.

**Consequence.** The version reply is lost, so the status stays "Looking...". The data the G2 still holds then
comes back on the *next* bulk read, giving JB's "one message behind until a power cycle" state (`g2_io.c:777-784`).

**Why tests pass.** The emulator's host model drops empty bulk-IN packets: `emu/g2emu/src/machine.cpp:839`
(`shortPacket = !d.empty() && ...`). The proto emulator never produces them. The comment at `client.cpp:388`
already quotes Clavia's rule ("the first non-empty read"); the transport does not implement it.

**Who is right.** Clavia's `RxData` 0x108a7e retries while a read returns 0 bytes. Verhue, g2fx and JB all retry too.

**Fix.**
1. In `onBulk`, if `actual_length == 0`, resubmit the same bulk read until data arrives or 5 s pass (Clavia's limit).
2. On the deadline, treat it as Clavia's `XUSBError` (Major Error).
3. Make the g2emu host model deliver empty completions the way libusb does, so `test_g2emu` covers the path.

### M2: bulk-IN deadline, length check, reset at open (Medium)

**No deadline.** Bulk-IN has no timeout (`libusb_transport.cpp:220`). While it is pending the interrupt endpoint is
not armed. If the data never comes, the transport waits forever: `Client::restart()` sends a version request whose
announcement nobody reads, and only a replug helps. Clavia gives up after 5 s and destroys the port (also a
replug, but a visible one). This deadline is part of the H1 fix.

**No length check.** The length is never compared with the announced one (`client.cpp:382-390`). A mismatch is the
signature of the desync (stale data in the device's bulk FIFO). Today it is silently dropped as a CRC error, or
worse, parsed as a valid but wrong message. Recommendation: log a mismatch in the USB log and, when it repeats, say
"power-cycle the G2". This is a G2fresh addition with no counterpart in Clavia; flag it as such.

**No reset at open.** G2fresh does not reset the device when it opens it. Clavia does (`ResetDevice`,
`CMacUsbHandle::OpenDevice` 0x107e9c), and Web runs `usbreset` before every start. The original wins, so add
`libusb_reset_device` after `libusb_open`.

**Caveats on the reset** (check per platform before acting):
- it does not cure a firmware-level wedge (Web, `docs/phase4b-ergebnis.md:221-228`: only a power cycle helped);
- on macOS and Windows the handle may become invalid after a reset that re-enumerates, so the open has to be
  redone.

### M3: bulk-OUT timeout (Medium)

The timeout is 2000 ms (`libusb_transport.cpp:19`); Clavia uses 5000 (`TxData` 0x1088c6). On a timeout libusb
cancels the transfer part-way, and the G2 is left inside a frame. JB saw `7E 3` and a wedge from that
(`g2_io.c:930-939`). Use 5000, the original's value.

### M4: frame-size limit (Medium)

**What JB measured** (`defs.h:112-122`, 2026-09-17, real G2): a ~30 440-byte message works. A ~34 012-byte one
gets `7E 3`, every slot is renamed "Error", and the G2 stops answering until a power cycle. They report that the
Clavia editor fails on the same file.

**Where G2fresh stands.** It splits only above 0xFFFF (`proto/src/bubble.cpp:44-59`, `frame.hpp:40`), as Clavia's
`GenerateMessages` 0x11828a does, so it sends such a performance whole.

**Recommendation.** A deliberate, flagged deviation: refuse uploads above 32 768 bytes with a clear message, instead
of wedging the synth. Four big patches in a performance can reach this size.

### Platforms (Low)

- **Windows.**
  - Needs WinUSB through Zadig (`libusb_transport.hpp:14-16`), which replaces Clavia's driver; the original editor
    stops working until that is reverted. JB documents this, plus a Windows 11 driver-signing caveat
    (`docs/getting-started/windows-driver.md:9-27`). Worth a user-facing note.
  - The 1 s scan replaces hot-plug; fine.
- **Linux.** G2fresh's suggested rule is `MODE="0666"`. Web uses `MODE="0660", GROUP="plugdev", TAG+="uaccess"`
  (`deploy/99-clavia-g2.rules`), which gives access to the logged-in user without a world-writable device.
  Prefer that.
- **macOS.** No issue found. JB avoids `libusb_interrupt_transfer` on macOS because sync timeouts misbehave
  (`g2_io.c:773-776`); G2fresh uses async transfers without a timeout, so this does not apply.
- **Guard.** If a timeout is ever added to the interrupt transfer, a TIMED_OUT completion with `actual_length == 16`
  must still be used (JB, hardware). Today `onInterrupt` (`:246-248`) would drop it.

---------------------------------------------------------------------------------------------------

## 2. Session start-up

### Order

| step | G2fresh (`client.cpp:225-291`) | Clavia, on hardware (Cap `poweron2`) | g2fx (`Device.java:63-72`, `Performance.java:341-365`, `Patch.java:545-583`) | JB (`daemon.c:1111-1172`) |
|---|---|---|---|---|
| handshake | `80` | `80` | `80` | `80` |
| lock | `7D 01` | `7D 01` | `7D 01` | `7D 01` |
| synth | `35 04`, `02`, `81` | same | name lists **first**, then `35 04`, `02`, `81` | `02`, `81` |
| performance | `10`, `59`, **`08`** | `10`, `59`, **`09 00`** | `10`, `59`, `3B`, `5E` | `10` |
| per slot | `35 S`, `3C`, **`58`**, `68`, `6E`, `71 01`, `71 00`, `70`, `2E` | `35 S`, `3C`, **`28`**, `68`, `6E`, `71 01`, `71 00`, `70`, `2E` | `35 S`, `3C`, `28`, `68`, `6E`, `71 01`, `71 00`, `70`, `2E` | `35 S`, `3C`, `28` |
| tail | `04`, `3B`, `5E`, `14` (patches, then performances), `7D 00` | same | `04`, `7D 00` | `5E`, `14`, `7D 00` |
| after `7D 00` | nothing | unsolicited `05`, then a `72`+`72` pair per slot; Clavia sends `T: 70` after each | `70` per load report (`Performance.java:435-447`) | - |

### H3: slot names (High)

**Why the sequence differs.** Clavia does not follow a fixed list. `CPatch::CleanDirtyData` (0xd601e) sends a request
only for what is still dirty.
- On hardware the `3C` reply holds `21`, **`2D 00`**, `4A`×2, `52`×2, `4D`×3 (10 variations), `65`, `62`, `60`,
  `5A`×2 and `5B`×3. It has **no `27` name, no `69`, no `6F` and no `7F`** (`poweron2`, all four slots).
- So the page focus is clean (no `58`) and the name is still dirty (`28`).

**The defect.** G2fresh's fixed list sends `58` and never `28` (`client.cpp:271-291`).
- `state_.slots[].name` is set only from a `27` (`state.cpp:331-337`), so it stays empty on a real G2 (and presumably on g2emu, which runs the same OS; not checked).
- The proto emulator hides this: its `3C` reply includes `27` (`emulator.cpp:177-187`).

**Consequences.**
- `SynthSync::slotName` (`ui/SynthSync.cpp:209-214`) shows nothing.
- `getPatch` loads an unnamed document (`:282`).
- `push()`'s performance-bound resend uploads with `state().slots[slot].name` (`:340-341`), renaming the synth's
  slot to "".

**Fix.** Send `28` after `3C`, as the original does here. Better: drive the per-slot requests from what the reply
left out, as Clavia does. Dropping `58` is optional (harmless). The `11` performance header also carries the four
slot names, which could seed the names until `27` arrives.

### Other start-up differences (Low)

- **`08` versus `09 00`.** Both are Clavia paths (`CPerformance::CleanDirtyData` 0x100e5a:
  `GetSlotFocusUpdateAction` 1 asks, 2 pushes the focused slot or 4). No change needed.
- **The version reply** (Cap) is **66 bytes**: `0x80`, 63 info bytes, CRC.
  - Offsets 1-9 are as G2fresh decodes them (`frame.cpp:173-190`): `0A`, model **1 = G2X** (the product string
    confirms it), mode 0, ?, firmware 0x008C = V1.40, protocol 0x0012.
  - **Offset 10 holds the synth's name** ("SCUMBUSTER", NUL-terminated; probably a str16 field to offset 25).
  - G2fresh's emulator sends 35 info bytes (`frame.cpp:156`). Decoding is fine (it needs ≥ 33).
  - Web reports `80 0a 03 00` from its unit, i.e. model 3 = Engine in Clavia's table.
- **Reply session bytes** (Cap):
  - `7F` to `7D 01` comes with session `00`;
  - `36` comes with `0x40`;
  - synth-level replies carry the performance session (e.g. `01 0c 06 ...` in `capture-014..016`).
  - G2fresh ignores the session at slot 4, which is right.
- **The `81` reply** carries **four** `80 slot cc` molecules (cc `FF` = none), extended. G2fresh keeps the last one
  (`state.cpp:442-445`); fine. The proto emulator sends one.
- **Name lists.** G2fresh reads the lists last, as Clavia does; g2fx and Web read them first.
  - Each real `16` reply starts with `13 lo hi`; G2fresh routes that to `FlashUsage`.
  - Performance entries do carry the category byte (`00`), contrary to JB's note.
  - Chunks hold about 13-15 names (about 200 bytes), and the next request starts after the last name. This matches
    G2fresh's cursor (`client.cpp:299-326`).

### Pacing (no finding)

- The G2X answers on a **~12 ms grid** (reply timestamps in `poweron2` fall on multiples of ~12 ms). Clavia sends the
  next request 0.3-1 ms after a reply. G2fresh's stop-and-wait, at a 10 ms tick (bridge `pump(10)`,
  `bridge/main.cpp:110`), is no faster than that, so no pacing is needed for requests.
- JB's 10 ms sleep after every send (`g2_io.h:15`) and 600 ms settle after a performance upload
  (`g2_device.c:1969-1974`) work around their non-stop-and-wait client. With stop-and-wait they are not needed.

---------------------------------------------------------------------------------------------------

## 3. Patch and performance dumps, bank operations

### Uploads (Cap: `capture-008` patch, `capture-001` performance)

| item | G2fresh | Cap (Clavia, real G2X) | match |
|---|---|---|---|
| patch header | `01 28+S 53 37 00 00 00 name` (`sections.cpp:77-83`, `bubble.cpp:66-67`) | `01 28 53 37 00 00 00 "g2fx-uprate-4mod"`; a 16-character name has no NUL | yes |
| section order | 21 4A4A 69 5252 4D4D4D 65 62 60 5B5B5B 5A5A 6F (`sections.cpp:12-18`) | the same, with `6F len 0` | yes |
| variations in `4D`/`65` | 10 for USB (`sections.cpp:20-34`); empty lists left at 0 | 10; empty `4D` lists carry 0 variations | yes |
| performance | `01 2C 42 37 … 1A 29 name 11 …` 4 × 18 sections, `5F` | the same, one 3 861-byte frame | yes |
| **reply to a patch upload** | expects the synth to "answer, then `38`" (notes §8.1, emulator `emulator.cpp:149`) | **`01 08 40 36 00 04`**: response on the slot header, void session, `36 slot newSession`; **no `38`** | **no (M1)** |
| reply to a performance upload | expects `1F` (notes §16.3, emulator `:160`) | `01 0c 40 36 00 1b 36 01 1b 36 02 1c 36 03 1f 36 04 0f`: five `36` in the reply; no `1F` | handled (slot 4 header), but the emulator differs |
| after the upload | nothing | unsolicited `05`, then `72`+`72` for **every** slot; Clavia sends `70` and `2E` to the uploaded slot and `70` to the others (also g2fx `Patch.java:297-301`) | Low (L1) |

### M1: the `36` in an upload reply is ignored (Medium)

**What happens.**
- `handleReply` adopts a session only from a non-void header (`client.cpp:481-486`).
- `route()` sends slot-header molecules to `applyToSlot` (`client.cpp:528-568`), which has no case for
  `SessionNumber` (`state.cpp:327-361`). So the new session is dropped.
- The next unsolicited message on that slot (the `72`, ~12-30 ms later) carries the new session, which triggers
  `resyncSlot` (`client.cpp:514-520`): a full `3C` read-back.
- `SynthSync::slotReplacedOnSynth` (`ui/SynthSync.cpp:477-494`) then overwrites the document with the read-back.
  Edits made in that window are sent with the stale session (ignored by the synth, JB `g2_io.c:468-470`) and then
  erased from the document.

**What Clavia does.** It routes molecules by destination, not by header: `36` goes to `HandleSessionNumberDump`
(0x11dcd8) wherever it arrives. It does not re-read after an upload (Cap: only `70`, `2E`).

**Fix.**
- In `route()`, apply synth-destination molecules (`36`, `38`, `1F`, `05`, `13`, `0D`, `80`) through the
  synth-level path whatever the header slot.
- Drop the assumption that a `38` follows an upload, in the client, the emulator and the tests
  (`tests/test_proto_session.cpp:140-156` asserts the re-read).

### Download (`3C`)

The real reply is `21, 2D page, 4A, 4A, 52, 52, 4D, 4D, 4D, 65, 62, 60, 5A, 5A, 5B, 5B, 5B`: `5A` before `5B`, no
`27`, `69` or `6F`, and no `7F` (Cap; JB `utils.c:119-125` saw the same order).
- `PatchAssembler` (`sections.cpp:118-160`) is seeded from the previous patch, so the missing `69` and `6F` come from
  stale data until the `68` and `6E` replies replace them. That works because both are requested in the sync.
- The name is the H3 defect.

### Flash (bank) operations

| op | G2fresh | Cap (Clavia, real) | note |
|---|---|---|---|
| load `0A slot bank prog` | `client.cpp:739-746`, no handler | **patch:** reply `01 0c 40 38 00 03 0d 00 07 00 00 00` (`38` and `0D` *in the response*), then Clavia re-reads the slot (`35 00`, `3C`, `28`, …). **Performance:** reply `1F 08` + 4 × `36` + `0D` (`capture-003`), then Clavia re-reads all | **H2** |
| store `0B slot bank prog` | `client.cpp:748-759`, re-reads both name lists | reply `13 lo hi` + `16 00 00 03 b p name cat 05` (the stored entry only, flag 0) + `0D … 02`; then an unsolicited `29` + `11` on hdr 04 (`capture-006`) | L5 |
| `0D` result | comment "result 0 = ok" (`molecules.hpp:121`) | 2 after a store, 5 after a delete; `CFlashData::HandleFlashCommandResult` 0x11b062 treats **0, 2 and 5 as success**, 1 "Can't load", 3 "Flash memory full", 4 "Memory protect on" | L6 |
| delete `0C type bank prog origin` | molecule only, no UI | reply `15 t b p` + `13` + `0D t b p origin 05` (`capture-015/016`) | feature gap |
| read file `17` | molecule only; proto emulator sends `18` then `19` | reply is **`19` alone**, with no `18`, when the slot holds data (`capture-013/014`); `size` counts the bytes after the version byte, as `molecules.hpp:153-160` says | emulator only |
| write file `19` | molecule only | reply `18 00 t b p` + `13` + `16` fragment (`capture-015/016`) | feature gap |

### H2: releases inside replies (High)

**The defect.** `route()` re-reads on `38` only when `!fromReply` (`client.cpp:580-581`), and on `1F` only when
`!fromReply` (`client.cpp:588-593`). On hardware both come in the response to `0A`, so:
- `loadFromFlash` never refreshes the mirror;
- `SynthSync::loadFromBank(..., open)` waits for a `slotChanged` (a patch, `ui/SynthSync.cpp:526-531`) or a
  `synced` (a performance, `:515-523`) that never comes (`:559-565`);
- LEDs (still gated by the old `72` replies) are mapped onto the old patch.

**What the original does.** Clavia's `HandleSessionPatchRelease` (0x11df1c) and `HandlePerformanceRelease`
(0x11d7ac) do not check whether the release came in a reply.

**Why tests pass.** The proto emulator sends the release unsolicited after the ack (`emulator.cpp:309-323`), so
`tests/test_proto_session.cpp:399-402` passes.

**Fix.**
- Re-read on any `38` or `1F`.
- After `1F`, re-read what Clavia does (`capture-003`/`-005`): `35 04`, `10`, `59`, then each slot from `3C` (the
  `36`s already gave the sessions), then `7D 00`. G2fresh's full `enqueueSync(false)` also re-reads `02`, `81`,
  focus, `04`, `3B` and both name lists (L4).
- Update the emulator to answer `0A` the way the hardware does.

---------------------------------------------------------------------------------------------------

## 4. Streams from the synth

| item | G2fresh | Orig | Cap | others |
|---|---|---|---|---|
| `39` LEDs | start byte, 2 bits per LED, LSB first (`state.cpp:451-459`) | same (`CLedBlinkData::SetDualLed` 0x101e88) | 17-byte messages, start 0, 10 data bytes | JB (`g2-editor/src/store/led.ts`, hardware commit `e531816`), g2fx (`state/PatchVisuals.java:57-72`) agree |
| **`3A` meters / strips** | **big-endian** word per entry (`state.cpp:469-483`; `state.hpp:88-92` says "read here as big-endian [I]") | raw i386 `ushort`, **little-endian** (`SetMultiLed` 0x101e4a) | 87-byte messages: start 0, 40 × 2 bytes. Over 177 messages in `poweron2` / `capture-002` / `capture-012`, every nonzero value is in the **first** byte of its word (e.g. 1-15, 0x29, 0x2B) and the **second byte is always 0** | g2fx/Web read "[unknown][value]" **starting at the start byte**, i.e. byte 5+2k (`PatchVisuals.java:43-54`); JB reads `data[i+1]` from the start byte (`led.ts:35-44`). All three take the first byte of each little-endian word |
| LED gate | after both `72` replies (`client.cpp:501-505`) | same (0x1041ea) | streams start after `7D 00` | - |
| unsolicited `72` | applied; no `70` (`client.cpp:526-568`) | after each load report, `T: 70` (`CPatch::CleanDirtyData` 0xd601e, flag `this[1]`) | after `7D 00` and after uploads: `72 01 …` and `72 00 …` **in one message** per slot, then Clavia sends `70` | g2fx answers with `70` (`Performance.java:435-447`) |
| panel `1F` | `1F ps` then 4 × `36` applied; full re-sync | re-read | `01 04 40 1f 02 36 00 0e 36 01 0e …` (`capture-005`) | Web/JB: same compound |
| synth news on hdr 04 | `05`, `29`+`11`, `13` routed (`client.cpp:571-619`) | - | `05` after `7D 00` and after uploads; `29`+`11` after a store | JB: also `3F FF 02 v` from SHIFT+GLOBAL (`g2_events.c`), already handled by `PerfHeaderParam` |
| rate | as it comes | as it comes | one LED or meter message per slot every ~12-50 ms | Web ~50 ms, JB ~17 ms |

### H4: meter byte order (High; display only)

**What happens.** With the current read, a VU level `0x0C` becomes 0x0C00:
- a VU meter shows clip for anything nonzero (`> 0x7E`, `CPnlVUMeterABC::SetValue` 0xcfc88);
- an LED strip gets an out-of-range index.

**Who is right.** The original's code (little-endian) and every hardware source agree. G2fresh's notes §10 and §16.5
misread Verhue's "[unknown][value]": the pair starts at the start byte.

**Fix.**
- Little-endian in `decodeMeters` / `encodeMeters`.
- Regenerate `tests/protocol/led_vu.json` and `vectors.inc`.
- Update notes §10, §14.4 and §16.5.

The `70`-after-`72` difference is L1.

---------------------------------------------------------------------------------------------------

## 5. Errors, alerts, timeouts, recovery

| item | G2fresh | Orig | others |
|---|---|---|---|
| reply timeout | 10 s since max(last send, last receive), no retry (`client.cpp:142-151`) | same (`CTimeoutManager::SuggestAction` 0x103f56, `kErrorRetry` 0) | g2fx/Web: none, so a lost reply blocks forever. JB: 500 ms per command, "was 100 ms, too tight under LED/VU load" (`defs.h:108`) |
| `7E code` | fatal by default (`client.hpp:56-58`, `client.cpp:467-473`) | `MajorError` | g2fx/Web/JB: logged only |
| bad CRC | dropped and counted (`client.cpp:394-397`) | dropped | not checked |
| version reply while connected | ignored if identical, else New Device | same (§5.2) | not parsed |
| model / firmware check | model ∈ {G2, G2X, Engine}, mode 0, protocol 0x0012 (`client.cpp:423-437`) | same (`IsSynthAccepted` 0x1038b8) | **none** in g2fx, Web or JB; G2fresh is ahead |
| stuck synth | `restart()` from the UI (`ui/SynthSync.cpp:181`); USB log | port destroyed, replug | Web: `usbreset` plus systemd restart, and a documented "stop, power-cycle, start" (`docs/phase4b-ergebnis.md:221-228`). JB: reconnect loop and a dump of the last 64 frames on disconnect (`g2_io.c:96-136`) |

**Low findings in this area.**
- **L7.** LED traffic refreshes `lastReceive_`, so while LEDs stream a lost reply never times out and the user queue
  stays blocked. This is Clavia's behaviour (original wins). If it is ever changed, make it opt-in and flagged: for
  example a second, absolute deadline from the send time.
- **L8.**
  - Web saw `7E 01` for a frame without the bubble begin/end bits (`docs/phase3-ergebnis.md:31-40`). G2fresh always
    sets them (`bubble.cpp:61-65`).
  - JB saw `7E 3` for a too-large frame, a module index of 128 and out-of-order sections.
  - `exceptionText` has no text for 1, matching Clavia (none for 0-2). Nothing to change.
- **JB's claim that `10` and `81` replies come on header `0x04` "in steady state"** (`g2_device.c:357-375`) is
  contradicted by every Clavia capture: always `0x0C`, also after a performance change (`capture-003`, `-005`).
  Their client is not stop-and-wait and probably took a pushed `29`/`11` for the reply. No change.

---------------------------------------------------------------------------------------------------

## 6. The emulators against the hardware (M5)

The session tests run against `proto/src/emulator.cpp`; `test_g2emu` runs the real OS behind a host model. These
differ from the captured hardware in ways that hide H1-H3 and M1:

| behaviour | proto emulator | g2emu host model | hardware (Cap) |
|---|---|---|---|
| empty bulk-IN completions | never | dropped (`emu/g2emu/src/machine.cpp:839`) | 1-2 before 996 of 1002 extended messages |
| `3C` reply | all 18 sections plus `27` (`emulator.cpp:177-187`) | real OS | `21 2D …` without `27`, `69`, `6F` |
| reply to a patch upload | ack, then an unsolicited `38` (`:149`) | real OS | `36 S sess` in the reply |
| reply to a performance upload | ack, then `1F` (`:160`) | real OS | 5 × `36` in the reply |
| `0A` load | ack, then an unsolicited `38`/`1F` (`:309-323`) | real OS | `38`/`1F` + `0D` in the reply |
| `17` read | `18 00`, then `19` | real OS | `19` only |
| after `7D 00` / after an upload | nothing | real OS | `05`, then `72`+`72` per slot |
| version reply | 35 info bytes | real OS | 63 info bytes, synth name at offset 10 |
| `81` reply | one `80` | real OS | four `80` |

Bringing the proto emulator to the captured behaviour is the way to keep these fixed.

A cheap extra test: replay `poweron2.pcapng` and `capture-003/005/007/008` as scripts against `Client`, the way
g2fx's `CaptureSender` does. The captures are BSD-3 data; either keep them out of the repo or keep only the
necessary bytes with attribution.

---------------------------------------------------------------------------------------------------

## 7. Low findings

| # | finding | right answer | fix |
|---|---|---|---|
| L1 | No `T: 70` after an unsolicited load report, and no `70` + `2E` after an upload. LEDs flow without it (Cap: slot B's LEDs came before its `70`); its effect is unknown. | Orig (`CPatch::CleanDirtyData` 0xd601e) | 30 min |
| L2 | `08` versus `09 00` slot focus. | both Clavia | none |
| L3 | An extra `58` per slot. | Clavia skips it when `3C` carried `2D` | goes away with the dirty model |
| L4 | After a panel `1F`, G2fresh repeats the whole sync, including `02`, `81`, focus, `04`, `3B` and both name lists. Sync traffic has priority over user edits (`client.cpp:178-179`), so edits wait. On a full flash the lists alone are ~360 requests × 12 ms ≈ 4 s. | Cap: Clavia re-reads only the performance and the slots | 1 h |
| L5 | After a store, both name lists are re-read. The reply already carries the stored entry as a `16` fragment (flag 0), also seen by Web (`Entries.java:146-163` in their copy). | Cap | 1 h |
| L6 | `0D` result: 0, 2 and 5 mean success. | Orig 0x11b062 | comment |
| L7 | The reply timeout never fires while LEDs stream. | Orig (same) | opt-in only |
| L8 | `7E` codes 1 and 3 seen on hardware. | - | none |
| L9 | The interrupt type is tested with `& 3`; the other editors use `& 0x0F`. | Orig `& 3`; captures only show `x1`/`x2` | none |
| L10 | Linux udev rule: prefer `uaccess` to `0666`. | Web | doc |
| L11 | Windows: Zadig replaces Clavia's driver; Windows 11 signing caveat. | JB doc | doc |
| L12 | The version reply carries the synth's name (offset 10), which the editor could show. G2fresh's notes still list the reply length as unknown. | Cap | doc + 15 min |

---------------------------------------------------------------------------------------------------

## 8. What the other editors have that G2fresh lacks

### Recovery from a stuck synth

- **USB reset at open.** The original does it (`ResetDevice`); Web runs `usbreset` before every start. See M2.
- **Desync detection.** None of the others check the bulk length either. JB's "one message behind until a power
  cycle" is the case to detect; telling the user to power-cycle beats silent garbage.
- **Last-frames dump.** JB dumps the last 64 frames on disconnect (`g2_io.c:96-136`). G2fresh's full USB log
  (`proto/src/logging_transport.cpp`) already covers this, and goes further.
- **Reconnect.**
  - JB reconnects automatically: it polls after NO_DEVICE, redoes `80`, `7D 01`, `7D 00`, and its editor re-uploads
    its slots (`daemon.c:1025-1058`, `CHANGELOG.md:40`).
  - G2fresh reconnects on re-enumeration and offers `restart()`. After a fatal status Clavia waits for a replug;
    keep that, and offer the button.
- **Documented recovery.** Web documents "stop, power-cycle, start" (`docs/phase4b-ergebnis.md:221-228`). A line in
  G2fresh's user docs would help.

### Firmware and version checks

None of the three parses the version reply. G2fresh checks model, mode and protocol as Clavia does; keep it. It
could also show the OS version ("V1.40") and the synth name (offset 10).

### Bank operations

- Delete and delete range (`0C`/`0E`, JB `g2_device.c:1490-1523`).
- Bank file read and write (`17`/`19`, JB `g2_device.c:2963-3343`). JB matches replies on (bank, location) because
  the G2 can answer one request late (2026-09-14, hardware). G2fresh has the molecules but no UI.

### Guards

- The frame-size cap (M4).
- Refusing module index > 127 (JB `defs.h:124-128`); this belongs to the live-editing work.

---------------------------------------------------------------------------------------------------

## 9. Hardware facts about edit messages, for the live-editing work

These are only relayed here. Each comes from the other editors' hardware tests; check them against Clavia's code.

- **`6A` variation select** must be a request frame (`0x28|s`). As realtime (`0x38|s`) the G2 ignores it. Without
  bubble flags it is answered `7E 01` (Web `docs/phase3-ergebnis.md:31-40`).
- **`40` sent as a request** (`0x28|s`) bumps the session and is acked every time, which "destabilizes the
  connection" (JB `g2_device.c:2311-2315`). Keep `40` realtime, as Clavia does.
- **A standalone `6F` patch-notes edit** hung the G2 twice (JB `g2_device.c:1627-1630`). G2fresh's notes list
  `T: 6F` as an edit (§9). Check Clavia's callers of `CMNotePadDump::WriteStream` before using it.
- **Module index 128** gets `7E 3`, and a later edit on it wedges the G2 (JB `defs.h:124-128`).
- **Edits with a stale session** are ignored by the G2 (JB `g2_io.c:468-470`); this is why M1 matters for live
  editing.
- **`1C` / `1D` global knob edits** are answered by a bare `7F`; re-request `5E` to see the result (Web report).
- **Moving a morph to another group** needs an explicit range-0 delete of the old group (Web `4a3da6b`).
- **`25` location byte** is `loc << 6` (Web `e519f4e`).
- **Realtime bursts:** 175 `40` writes in bursts of 32 with 8 ms pauses were all applied (Web `d4b8888`).

---------------------------------------------------------------------------------------------------

## 10. Corrections to `re/notes/usb-protocol.md` (not made; this note is the only file written)

- **§1.3.** Bulk-IN: one or two zero-length packets usually precede an extended message's data (996 of 1002 in the
  captures); the host must retry empty reads.
- **§1.2 / §14.1.** The descriptors are now known: USB 1.1, EP0 16 bytes, 1 configuration (self-powered), one
  vendor interface FF/FF/FF, 0x81 interrupt 16 bytes with bInterval 3, 0x82 and 0x03 bulk 64. There is no USB-MIDI
  interface (this answers §12.2).
- **§5.2 / §14.2.** The version reply is 66 bytes. Model 1 = G2X. The synth name is at offset 10.
- **§6.3.** Add the observed sequence (section 2 above): `09 00` is possible, `28` follows `3C`, `58` is skipped, and
  after `7D 00` come `05` and `72` pairs, each answered with `70`.
- **§8.1 / §16.3.**
  - Patch upload: the reply carries `36 S sess`; there is no `38`.
  - Performance upload: the reply carries 5 × `36`; there is no `1F`.
- **§8.2.** The `3C` reply is `21 2D 4A 4A 52 52 4D 4D 4D 65 62 60 5A 5A 5B 5B 5B`, with no `27`, `69`, `6F` or `7F`.
- **§8.3 / §16.6.**
  - `0A`: `38`/`1F` and `0D` come in the reply.
  - `0B`: the reply carries the `16` fragment and `0D` (2 = ok).
  - `17`: `19` only.
  - `0D`: 0, 2 and 5 mean ok.
- **§10 / §14.4 / §16.5.** `0x3A` is little-endian; the value byte comes first.
- **§14.3.** No captured reply carries data together with `7F` (checked over all 18 files). Plain commands get a
  bare `7F`.
- **§14.8.** Each of the 654 captured requests got exactly one response message before the next request; no reply
  was split. The high nibble of an extended announcement is always 0 (`b0 = 0x01` in all 1002).
