# Nord Modular G2 patch / performance file format (.pch2 / .prf2)

Reverse-engineered from the Mac G2 editor v1.62 (`original/mac/G2Editor_i386`, Ghidra
project `re/ghidra-db`, export in `re/out/decomp/`), cross-checked against Bruno Verhue's
reader (`references/nord_g2_editor/Gen2/Common/BVE.NMG2File.pas`, `BVE.NMG2Types.pas`)
and against every patch we have (11 files in `tests/corpus/pch2csd/`, 4 private patches,
`references/nord_g2_editor/.../Gen3/Patch/hi_hat_machine.pch2`, which is version 22).

Reference implementation: `tools/pch2/pch2dump.py` (decode to JSON, encode back,
byte-identical round trip on all of the above). Every address below is a Ghidra
virtual address in `G2Editor_i386`. The copies of the same names in the 0x0021xxxx to
0x0023xxxx range and the 4-byte stubs at 0x005ccxxx are exception-handling clones
and import stubs that Ghidra decompiled as junk. Ignore them.

Confidence markers: **[C]** read directly from Clavia's code. **[V]** from Verhue only.
**[E]** inferred from the corpus. **[?]** open question.

---

## 1. Container

```
+------------------------------------------------------------+
| text header: 4 lines, each terminated by CR LF (0D 0A)     |
+------------------------------------------------------------+  <- CRC starts here
| UWord  file version (big endian, = 23 for v1.62)            |
| UByte  file type: 0 = patch, 1 = performance                |
| section, section, ... (id U8, length U16 BE, payload)       |
+------------------------------------------------------------+  <- CRC ends here
| UWord  CRC-16 (big endian)                                 |
+------------------------------------------------------------+
```

### 1.1 Text header [C]
Written by `CFileReader_G2_1::WriteDataFile` @00074b36 through `CFileByteStream::PutLine`
@0006b72a (each line is followed by `\r\n`):

| line | content |
|---|---|
| 1 | `Version=Nord Modular G2 File Format 1` |
| 2 | `Type=Patch` or `Type=Performance` |
| 3 | `Version=%d` = file version (23) |
| 4 | `Info=BUILD %d` (v1.62 writes `BUILD 320`; the corpus also has 266 and 234) |

Read by `CFileReader_G2_1::CreateDataFile` @00074ef8: exactly 4 `GetLine` calls
(@0006c282: reads to CR, requires LF next, max 1002 chars, else "File Corrupt or not a
Modular G2 File!"). Line 2 picks patch or performance (`::GetFileType(string)` @00074a1e).
Line 3: the `atoi` after `=` picks the class (`CPatchFile_11` ... `CPatchFile_23`,
`CPerformanceFile_11` ... `_23`). Other values give "Unsupported file version". Line 4
is ignored. `CFileReader_G2_1::GetFileType` @00074a74 only checks that line 1 contains
`Version=Nord Modular G2 File Format 1`.

There is no NUL terminator. The `00` byte that seems to end the text is the high byte of
the binary version word. Verhue's reader stops at the first `00`, which gives the same
result. The whole file must be shorter than 64 KiB (`CFileByteStream::ReadFromFile` @0006c472
rejects a size of 0 or a size of 0x10000 or more, because the buffer is 0xFFFF bytes).

### 1.2 Binary header [C]
`CFileHeader_11::ReadStream` @00051920 and `WriteStream` @000518e4: `PutUWord(version)`, then
`PutUByte(type == performance)`. On read, the type byte 0 maps to patch and 1 maps to
performance. Any other value gives "File corrupt.". `CPatchFile_13::Read` @00053c3e
requires a patch and `CPerformanceFile_N::Read` (for example `_14` @00053a40) requires a
performance. The binary version word is stored, but the class is chosen from the text
line, not from this word.

### 1.3 Sections [C]
Every section is `id:U8, length:U16 BE, payload[length]`. For bit-packed sections,
`CStreamSizer` writes the length: `TagWord` @00063b6c reserves the word,
`WriteSizeAtTag` @00063b9c pads the payload to a byte with zero bits and back-patches it.
`ReadSize` @00063c06 and `VerifySize` @00063c32 read it back. VerifySize moves to a byte
boundary and then requires that exactly `length` bytes were consumed (otherwise rethrow,
"File corrupt."). The patch header and the textpad write their length directly (header:
`kPatchHeaderSize>>3` = 15). Each reader first checks the id byte and throws
"File corrupt." on a mismatch.

**No section is optional and the order is fixed.** The readers are called in a fixed
sequence (§2) and each one requires its own id.

### 1.4 CRC [C]
`CFileByteStream::StartChecksumCalculation` @0006ae28 marks the position right after
the text header (the first byte of the version word). `GetCRC` @0006ae70 runs
`CRC_Get16(buf+mark, pos-mark, 0)` @0014fd40 over all bytes up to the end of the last
section. The table is built by `CRC_Init` @0014fce0 with polynomial 0x1021, MSB first.
This is **CRC-16/XMODEM** (poly 0x1021, init 0x0000, no reflection, no final XOR). The
result is appended with `PutUWord` (big endian). The reader compares it and throws
"Checksum error. Could not load file.". Because the initial value is 0, the leading
`00` byte does not change the CRC. Verhue's choice to start after the `00` is therefore
equivalent.

### 1.5 Bit conventions [C]
`CBitStream` (vtable @002b3028, `CFileByteStream` vtable @002a0588):

| vtable | method | behaviour |
|---|---|---|
| +08 | `GetBit` @001470e4 | 1 bit, MSB first |
| +0C | `GetUBits(n)` @00147622 | n ≤ 8 bits, MSB first, unaligned |
| +10 | `GetSBits(n)` @001476ae | same, sign-extended (two's complement) |
| +14 | `SkipBits(n)` @00146eb6 | = GetUBits(n) discarded. **Also used by writers** (see §5.10) |
| +1C | `GetUByte` @0014713a | **first moves to a byte boundary** (+64), then 8 bits |
| +2C | `GetString(max)` @001472c8 | aligns, reads bytes until NUL (consumed) or `max` bytes |
| +38/+3C/+40 | `PutBit`/`PutUBits`/`PutSBits` | mirror |
| +4C | `PutUByte` @001474d6 | **first zero-pads to a byte boundary** (+68), then 8 bits |
| +5C | `PutString(s,max)` @0014789e | aligns, writes the bytes, plus a NUL if `len < max` |
| +64 | `MoveToByteBoundary` @00146e70 | skip (reader) |
| +68 | `ZeroPadToByteBoundary` @0014701a | zero fill (writer) |

`GetUWord` @00146f74 and `PutUWord` @00146f40 are two aligned bytes, big endian.
Consequence: every UByte/UWord/string field is byte-aligned, and the skipped bits are
written as zeros. All other fields are packed MSB first with no alignment. Bits left
over at the end of a section are zero (WriteSizeAtTag zero-pads).

### 1.6 Location ("context", `NSFile_V7::EContext`) [C]+[V]
2-bit field. Values above 2 are clamped to 2 and counted in `gLimitCnt`.
**0 = FX area, 1 = VA (voice area), 2 = patch settings (morphs, volume, glide, ...).**
The VA objects are built with 1 and the FX objects with 0 (`CPatchFile_13::CPatchFile_13`
@00056a70), and VA always comes first in the file.

---

## 2. Patch layout (versions 13–23) [C]

`CPatchFile_13::ReadMolecules` @00052d3c, `WriteMolecules` @00051e7c (inherited unchanged
by `CPatchFile_14`…`_23` and `CPatchFileCurrent`):

| # | id | section | Clavia class | loc | reader / writer |
|---|---|---|---|---|---|
| 1 | `21` | patch header | `CPatchHeaderData_11` | – | @000664ae / @00065564 |
| 2 | `4A` | module list | `CModuleData_11` | 1 (VA) | @0006774a / @00067968 |
| 3 | `4A` | module list | `CModuleData_11` | 0 (FX) | 〃 |
| 4 | `69` | current (played) notes | `CCurrentVoiceData_11` | – | @000672c4 / @00064aca |
| 5 | `52` | cable list | `CCableData_11` | 1 | @00065eb8 / @00064d9a |
| 6 | `52` | cable list | `CCableData_11` | 0 | 〃 |
| 7 | `4D` | parameter values | `CModuleParamData_11` | 2 (settings) | @00068620 / @00068928 |
| 8 | `4D` | parameter values | `CModuleParamData_11` | 1 | 〃 |
| 9 | `4D` | parameter values | `CModuleParamData_11` | 0 | 〃 |
| 10 | `65` | morph map | `CMorphMapData_11` | – | @00069326 / @00069c58 |
| 11 | `62` | knob map | `CKnobMapData_11` (type 0) | – | @00069ef8 / @00069642 |
| 12 | `60` | MIDI controller map | `CCtrlMapData_11` | – | @0006a0f6 / @0006882a |
| 13 | `5B` | custom data (labels) | `CModuleCustomData_11` | 2 | @00066b3a / @00064c1a |
| 14 | `5B` | custom data | `CModuleCustomData_11` | 1 | 〃 |
| 15 | `5B` | custom data | `CModuleCustomData_11` | 0 | 〃 |
| 16 | `5A` | module names | `CModuleNameData_11` | 1 | @00068b3e / @000660ac |
| 17 | `5A` | module names | `CModuleNameData_11` | 0 | 〃 |
| 18 | `6F` | textpad (patch notes) | `CTextpadData_11` | – | @00068d80 / @00064632 |

The reader explicitly checks that the three `5B` sections carry locations 2, 1, 0 in
that order (else "File corrupt."). The other sections store whatever location they
read. Every corpus file has exactly this sequence. Sections are present even when they
are empty: an empty FX module list is `4A 00 02 00 00` (location 0, count 0).
`Format_11::CStreamParser::ParsePatchPriv` @000260d6, the parser used for clipboard
and performance transfers, expects the same order.

---

## 3. Patch sections

Widths are in bits. `aligned` means a byte-aligned UByte or UWord (§1.5). Counts are
written from the container size. Lists are written in container order, which is
ascending module index for the map-based classes.

### 3.1 `21` patch header — `CPatchHeaderData_11` (15 bytes)

| bits | field | notes |
|---|---|---|
| 7,7,7,7 | legacy | always written 0 [C]. Read and dropped. In the V7 header (`CPatchHeaderData_7::ReadStream` @000610ce) these positions are KB range min/max and velocity range min/max |
| 5,5,2,7,2,7,2,1 | legacy | always 0 [C]. Probably V7 bend range, portamento and pedal fields [?] |
| 7 | voice count − 1 | `this[4]`, clamped to 1..32. `SetVoiceCount` @00063dac |
| 7 + 7 | splitter position | hi 7 bits, then lo 7 bits, 14-bit value ≤ 0x3FFF (`+0x0e`). VA/FX divider in the editor. Default 600 |
| 3 | octave shift + 2 | `this[5]` = −2…+2 (raw 0–4, larger raw values → 0) |
| 1 ×7 | cable color visible | `this[6..12]`, order red, blue, yellow, orange, green, purple, white [V order] |
| 2 | mono mode | `+0x10`, `EMonoMode`, default 1. Corpus: 1 with 2 voices, 0 with 4–5 voices. Likely 0 = poly, 1 = mono, 2 = legato [E/?] |
| 8 | active variation | `this[0x15]` (0–8) [V name] |
| 8 | category | `+0x18`, `ECategory` 0–15, larger values → 0. Strings @0019a838: No Cat, Acoustic, Sequencer, Bass, Classic, Drum, Fantasy, FX, Lead, Organ, Pad, Piano, Synth, Audio In, User 1, User 2 |
| 4 | pad | zero (alignment done by the next PutUByte) |
| 8 | reserved byte | aligned. Writer: `PutUByte(0)` (tail call @000657bd). Reader: discarded |

Total is 108 bits of fields, then 4 pad bits, then 1 byte, giving 15 bytes. Verhue's
"Unknown1..8" are the legacy fields plus the top 2 bits of the 7-bit voice field. His
12-bit "Unknown10" is the pad plus the reserved byte. The `Format_11` parser
(`R_PatchHeaderData::Parse`) reads the reserved byte unaligned at bit 108. That makes no
difference while it is 0.

### 3.2 `4A` module list — `CModuleData_11`

| bits | field |
|---|---|
| 2 | location |
| 8 | module count |
| per module (`CModuleDumpItem`, ctor @000676d2): | |
| 8 | module type |
| 8 | module index (id within the location, 1-based in practice) |
| 7 | column ([2]) |
| 7 | row ([3]). The corpus has col 0–4 and row 0–85, which matches Verhue's col/row order [E] |
| 8 | color (`EModuleColor`) |
| 1 | uprate (`EModuleBandWidth`, written as `bandwidth == 1`) |
| 1 | "is_led" flag (`+0x18`), Verhue `IsLed`. It is really the module **lock** flag (excluded from randomizing): it equals the per-type `defaultLocked` of `data/modules.json` on all 210 corpus modules, and matches "has LEDs" on only 128. `ConvertPatch22to23` @0004f09c sets it from a fixed type list (3, 4, 17, 38, 42, …) = the default-locked set |
| 6 | reserved, written 0 |
| 4 | selector-value count *n* (Clavia's term for module "modes") |
| 6 ×n | selector values |

### 3.3 `69` current notes — `CCurrentVoiceData_11`

| bits | field |
|---|---|
| 7,7,7 | "mono" note: note, attack velocity, release velocity |
| 5 | poly note count − 1 (≤ 31, so 1..32 notes) |
| (7,7,7) ×n | poly notes |

Clavia quirk [C]: `WriteStream` writes byte `[1]` (attack velocity) into **both** the
attack and the release slots (verified in the disassembly @00064b2a/@00064b42). Files
saved by v1.62 therefore always have release = attack. Callback names
`Format_11::R_PlayedNotesData::RdMono/RdPoly`.

### 3.4 `52` cable list — `CCableData_11`

| bits | field |
|---|---|
| 2 | location |
| 6 | pad (zero). The next field is a UWord |
| 16 | cable count (aligned UWord, big endian) |
| per cable (`CCableSegmentSpec`): | |
| 3 | color (`ECableColor` 0–6, clamped to 6) |
| 8, 6 | source module, connector |
| 1 | source connector is an output (1 = out→in cable, 0 = in→in link) |
| 8, 6 | destination module, connector (always an input) |

The writer puts the end with the output flag first. Verhue's "Unknown 12 bits + 10-bit
count" is really the 6 pad bits plus the 16-bit word.

### 3.5 `4D` parameter values — `CModuleParamData_11`

| bits | field |
|---|---|
| 2 | location |
| 8 | module count (only modules with ≥ 1 parameter are written) |
| 8 | variation count (`this[0x40]`, written even when there are no modules. The corpus has 9, or 0 for an empty list) |
| per module: | |
| 8 | module index |
| 7 | parameter count *p* |
| per variation: 8 variation index, then *p* × 7-bit values | |

Variations 0–7 are user variations 1–8. Variation 8 is the "init" variation (§7, step
14→15) [V+C]. Verhue notes that patches received over USB can have 10.

In location 2, the "modules" are the patch settings, using fixed pseudo-module indices
[V names, C structure]:

| idx | params | meaning |
|---|---|---|
| 1 | 16 | morph: 8 dial values, then 8 morph modes (knob or controller) |
| 2 | 2 | patch volume, active/mute |
| 3 | 2 | glide type, glide time |
| 4 | 2 | bend on/off, bend range |
| 5 | 3 | vibrato mode, cents, rate |
| 6 | 4 | arpeggiator on/off, time, type, octaves |
| 7 | 2 | octave shift, sustain pedal |

The converters (§7) update these pseudo-modules with `UpdateModule(type, idx, …)` using
the internal types 6, 95, 135, 137, 138, 136, 153 for indices 1–7.

### 3.6 `65` morph map — `CMorphMapData_11`

| bits | field |
|---|---|
| 8 | variation count |
| 4 | morph count *m* (always 8) |
| 2 ×m | keyboard morph assign per morph group (`NSFile_V7::EKeyboardMorphAssign` 0/1/2, an NM1 legacy. Always 0 in the corpus) |
| per variation: | |
| 8 | variation index |
| 7 ×m | legacy morph dials. **Written as 0 and ignored on read.** The live dial values are in §3.5 location 2, module 1 |
| 8 | morph assignment count |
| per assignment (`CMorphSpec`): 2 location, 8 module, 7 param, 4 morph group (0–7), **8 signed** range (`PutSBits`/`GetSBits`, two's complement. The corpus has −32…127) | |

Verhue's `Reserved1:20 / VariationIndex:4 / Unknown1..8 / Reserved2:4` split is this
layout seen at the wrong bit offsets.

### 3.7 `62` knob map — `CKnobMapData_11` type 0 (`5F` = type 1, §4)

| bits | field |
|---|---|
| 16 | knob count (aligned UWord). Always written as 120 (0x78). Entries at index ≥ 120 are ignored on read |
| per knob: 1 bit "assigned"; if 1: | |
| 2 | location |
| 8 | module index |
| 2 | `EDeviceAssignType`, clamped to ≤ 1. Verhue calls it `IsLed`. 0 = a parameter, 1 = a module's LED: `CKnobMap::GetAssignKnobToLedMolecules @000f1848` builds `CBarbTargetSpec(..., 1)` (audit K3, 2026-10-16) |
| 7 | parameter index |
| 2 | slot (**only in `5F`**) |

Clavia calls a knob target a "barb" (`CBarbTargetSpec`, `R_KnobMapData::RdBarb/RdNoBarb`).
Knob 0–119 = 5 pages × 3 groups × 8 knobs? (page layout [?]. Corpus assignments start at 0 and at 72.)

### 3.8 `60` MIDI controller map — `CCtrlMapData_11`

| bits | field |
|---|---|
| 7 | count |
| per entry: 7 CC number, 2 location, 8 module, 7 param | |

Default patches map CC 7 → (2, 2, 0) (patch volume) and CC 17 → (2, 7, 0) (corpus).

### 3.9 `5B` custom data — `CModuleCustomData_11`

| bits | field |
|---|---|
| 2 | location |
| 8 | module count |
| per module: 8 module index, 8 byte count *k*, *k* × 8-bit bytes (unaligned, shifted by the 2 location bits) | |

The editor treats each module's bytes as a record stream (`CModuleCustomData_11::RemoveParam`
@00065012, `CPnlLabelButton::SetCustomData` @000c490c, `CPnlNoteSeqZoom::SetCustomData`
@000952e0): `kind:U8, len:U8, payload[len]`.
* kind 1 = parameter label: payload = `param:U8` + `len−1` label bytes. The panels read
  7 characters, NUL-padded. Location 2, module 1, params 8–15 hold the 8 morph-group
  names ("Wheel", "Vel", …).
* kind 0 = editor-only value, for example the NoteSeq zoom/offset `[0, 1, value]`.

Versions 11 and 12 have only two `5B` sections (VA, FX). Location 2 was added in 13 (§7).

### 3.10 `5A` module names — `CModuleNameData_11`

| bits | field |
|---|---|
| 2 | location |
| 6 | **not written**: the writer calls `SkipBits(6)` (vtable +14, @000660fe…0006610b), which only advances the position. These bits are whatever was in the 64 KiB stack buffer. The corpus has values 0–63 here |
| 8 | count |
| per name: 8 module index (aligned), name = `GetString(16)`: up to 16 bytes, NUL-terminated if shorter than 16 | |

### 3.11 `6F` textpad — `CTextpadData_11`
`id, UWord length, length bytes` of text, no NUL, at most 1024 bytes (the writer clamps).
The reader uses `GetString(length)` and does not verify the size. An embedded NUL would
leave the stream misaligned, and the CRC check would then fail.

---

## 4. Performance (.prf2) layout [C]

`CPerformanceFile_23::ReadMolecules` @00052f44 / `WriteMolecules` @00052898 (the same
in `_13`…`_23`), `Read` @00052fa2, `Write` @000528f6. Structure:

```
text header ("Type=Performance") | version | type=01
11  perf header (CPerformanceHeader_11)
4 × [the 18 patch sections of §2]  for slots A, B, C, D (no per-patch text header or CRC)
5F  global knob map (CKnobMapData_11 type 1)
CRC
```

This gives 74 sections. The performance name is **not** stored in the file:
`CPerformanceHeader_11` has a name string at `+0x04`, but ReadStream and WriteStream
never touch it. It is presumably taken from the file name [?]. Verhue's `$29`
"perf name" exists only as a USB message.

### 4.1 `11` perf header — `CPerformanceHeader_11` (ReadStream @00068e38, WriteStream @00064254)

| bits | field | notes |
|---|---|---|
| 8 | unknown (`+0x08`) | aligned. Clamped ≤ 15 on read. Ctor default 0. Verhue "Unknown2" [?] |
| 6 | focused slot (`+0x0c`) | 0–3 (`CRange<0,3,TagSlotNumber>`). `CPerformance::HandleSlotFocusDump` writes here |
| 2 | global pages (`+0x0d`) | bool. Header parameter 2, sent by the knob floater's "Global Pages" check button (`CKnobFloaterContainer::HandleChangeRequests` @00132420) |
| 8 | keyboard range enabled (`+0x0e`) | bool. Verhue's name [V] |
| 8 | master clock BPM (`+0x10`) | header parameter 1. Default 120 |
| 8 | unknown bool (`+0x18`) | Verhue "Unknown5" [?] |
| 8 | master clock run (`+0x14`) | header parameter 0, clamped ≤ 1 (`EMIDIClockState`) |
| 8, 8 | reserved | written 0 |
| per slot (×4, `CSlotSettings` ctor @00064138): | | |
| str16 | patch name | `GetString(16)` / `PutString(…,16)` |
| 8 | enabled | `EnableSlot` (`+0x20`) |
| 8 | keyboard | `EnableKeyboard` (`+0x21`) |
| 8 | hold | `EnableKeyboardHold` (`+0x22`) |
| 8 | flash bank | 0–31 (`TagFlashBankNumber`) |
| 8 | flash program | 0–127 (`TagFlashProgramNumber`) |
| 8 | keyboard range lower | `SetKbdRangeLower` (`+0x26`), default 0 |
| 8 | keyboard range upper | `SetKbdRangeUpper` (`+0x27`), default 127 |
| 8 | MIDI channel | `SetMidiChannel` (`+0x23`), clamped ≤ 16 (16 = off?) [?] |
| 8, 8 | reserved | written 0 |

All fields are byte-aligned except the 6+2 bit pair. Size: 10 + 4 × (name + 10) bytes.

### 4.2 `5F` global knob map
Same as §3.7, plus a 2-bit slot after the parameter index. 120 entries.

---

## 5. Quirks and differences from Verhue

1. The CRC covers the version word. Starting after the `00` gives the same value (init 0).
2. Byte-aligned primitives absorb padding: cable list (6 bits), patch header (4 bits).
3. `5A` reserved 6 bits are uninitialized memory when Clavia writes them (§3.10). A writer
   must preserve them for byte-identical output, or may write 0. The reader ignores them.
4. `69` release velocity is overwritten with attack velocity by the v1.62 writer (§3.3).
5. Morph range is signed 8-bit. Knob device type is a clamped 2-bit enum.
6. Verhue's `TPatchSettings` hard-codes section numbers and entry counts. In the file
   these are an ordinary `4D` list (module idx, param count), so the counts can vary.
7. Verhue stops a patch at `6F` or `5A` and accepts any order. Clavia requires the exact
   sequence of §2.

---

## 6. Field-level JSON produced by `pch2dump.py`

`text_header`, `version`, `type`, `sections[]` (each has `id`, `name`, `class`, then
the fields above using the names in this document, plus `slot` for performances),
`crc`, `crc_valid`. Fields that are not understood are kept explicitly: `legacy_*`,
`reserved*`, `pad`, `is_led`, `unknown_*`. Custom data is decoded into `records` when
it parses cleanly, else `bytes` (hex). A section whose field decode does not reproduce
the payload exactly is emitted as `{"raw": hex}`; none of the corpus files needs this.
Optional keys `_pad` and `_trailing` hold non-zero final pad bits and extra payload bytes.

---

## 7. Version history (`CFileConverter`, @00051224 loops `Convert` until the current version)

Ported as `g2::convert` (core/src/convert.cpp), golden-tested against the original under
emulation (tools/convert/emulate.py): all 2,375 old-version files of the public corpus
convert byte for byte as the original converts them.

`CPatchFilePort::Load` @0006cba4 reads the file into the class named by the **text
header's** `Version=` line (`CFileReader_G2_1::CreateDataFile` @00074ef8), then
`CFileConverter` converts it to 23. The binary layout is the same for versions 13–23
(`CPatchFile_14`…`_23` add no Read/Write, only `Convert`); 11 and 12 use
`CPatchFile_11::ReadMolecules` @0005196c (17 sections: no settings `5B`). Each step
`ConvertPatchNtoN+1` copies the molecules, then runs that step's `UpdateModule` on the
seven settings pseudo-modules — (type, index) (6,1) (95,2) (135,3) (137,4) (138,5)
(136,6) (153,7) — and on every VA, then FX module in list order. The performance
variants (`CFileConverter::operator()(CPerformanceFile_N)`) copy the header and global
knobs and convert the four slots; 20→21 and 21→22 also get the slot and the global
knob map (`CPerformanceData`).

Side effects of the copy (what the converted file differs in even when no module
changes): custom data of 0 bytes is dropped (`CModuleCustomData_11::operator=`
@000698ca → `CopyMap`/`AddCustomData`); maps are written in key order — parameters,
custom data and names by module index, morphs by (location, module, param, group),
controllers by (location, module, param), knobs by knob; a knob's target is unique
(the second knob on the same parameter is dropped on read), as is a controller's;
reserved bits and legacy fields are written 0.

| step | function / `UpdateModule` | changes |
|---|---|---|
| 11 → 12 | `CPatchFile_11::Convert` @00052c70 | throws "Unsupported file version" |
| 12 → 13 | @000480c6 / @00047fa6 | settings `5B` added, empty. Type 88: param 0 removed. Settings module 5 (vibrato): param 2 = 64 inserted |
| 13 → 14 | @000488f0 / @00048780 | type 102: params 9, 1, 0 removed, then 8, 9, 10 inserted (0). Types 3, 4: param 2 inserted (0). Type 123: param 9 inserted (0). Type 195: param 3 inserted (1) |
| 14 → 15 | @000490a0 / @00048f9c | one on/off parameter per type inverted (`InvertParam` @00048efa; 46 types). Morph variation 8 added if missing (also in every later step up to 20→21) |
| 15 → 16 | @0004a042 / @00049a50 | levels and times through the cubic `_gConvertTable` @001dcce0 (`HandleCubifyParam` @000497f8, morph ranges kept at the same end value), some only where another parameter is not 1; type 87: param 0 −5 (≤100) or −3 (≤110); type 108: param 16 removed; type 177: param 5 removed. `ValidateCables` @00049744: type 177 loses the cables of input 1 |
| 16 → 17 | @0004aaf0 / @0004a7b6 | `TruncParam` @0004a716 limits (types 24, 25, 26, 190 to 3; 175, 176, 177, 182 to 1); type 158: param 2 removed; type 94: param 0 halved (and its morph ranges). `ValidatePartSelectors` @0004aaba: types 9 and 96, selector 0 above 1 moves up one (5 → 2) |
| 17 → 18 | @0004b2c4 / @0004b226 | type 118: params 4, 3 removed; type 198: param 0 removed |
| 18 → 19 | @0004bc48 / @0004bb60 | types 175, 176, 177, 182: one parameter zeroed; types 3, 4: param 2 limited to 1 (`LimitParam` @0004b9d6, morph ranges to ±1) |
| 19 → 20 | @0004c3d8 / @0004c2ec | type 134: param 0 as type 87 in 15→16 |
| 20 → 21 | @0004d910 / @0004d066 | morph map rebuilt with groups 4 and 5 swapped (keyboard morph assignments not copied: they become 0); settings morph module: dials 4/5 and modes 12/13 swapped with their knobs, controllers and global knobs (`SwapParamsWithoutMorph` @0004cc10, `CPerformanceData::SwapParam` @001ab0dc); morphs removed from some parameters of 16 types (the module's higher morphs shift down, as `CMorphMapData_11::RemoveParam` @00066836 does); types 98, 202, 206: parameters removed (also from the slot's global knobs), type 98 and 140 gain one; `ValidateCables` @0004cb22 (types 98, 206) and `ValidatePartSelectors` @0004cbec (type 206: selector 0 ≤ 1) |
| 21 → 22 | @0004e8ae / @0004e646 | type 77 modules dropped (`CopyModules` @0004e350) with their cables and names (their parameters and custom data stay in the file); LFO rates of types 26 and 24 (selector 4/5) rescaled per rate mode (`Convert_LFORate*` @0004e1f0…), glide time of type 158 through `ScaleGlide` @0004e1dc when its mode is 1 (`ScaleParam` @0004e470); type 69 gets a second selector value (0) |
| 22 → 23 | @0004f09c | the lock flag (§3.2) set from the type (`GetDefaultMutationLock` @0004f008) |

Quirks of the original that only synthetic files reach: a parameter removed or
added on a module without parameter data, or past the end of its list
(`CModuleParamData_11::RemoveParam` @0006831e erases `end()`), corrupts the objects;
`g2::convert` skips such an operation instead.

The converted object is not what the original saves: `CPatch::LoadFromFile`
@000d9308 builds the patch from it and keeps the converted flag (`CPatch+0x54`);
`CPatch::SaveToFile` @000da3f4 then asks `CFilePortUtils::ConvertAlert` @000308c4
("[WARN][Convert File]File will be converted to new format.[Cancel][OK]"), clears the
flag on OK and writes a fresh `CPatchFileCurrent` (version 23, "Info=BUILD 320").
Performances are converted the same way but saved without asking
(`CPerformance::SaveToFile` @0010190c), and nothing is said when a file is opened
(`SFileLoadInfo` is filled but not shown).

Versions 1–7 (`CPatchFile_1`…`_7`, `CConvert1to2`…`5to6`) and `CFileReader_31` handle the
original Nord Modular text format (`Version=Nord Modular patch 3.0`, `NSFile_V1`/`V7`
classes), not .pch2. Bank dumps (`Version=Nord Modular G2 Bank Dump`) were not studied.

---

## 8. Open questions

* `21`: meaning of the 8 legacy fields after the four ranges, and the exact `EMonoMode` values.
* `65`: semantics of the `EKeyboardMorphAssign` values 1 and 2 in the G2 (never non-zero here).
* `62`/`5F`: `EDeviceAssignType` value names. Physical layout of knob indices 0–119.
* `11`: `unknown_08` (0–15) and `unknown_18` (bool). MIDI channel value 16 is probably "off".
* No real .prf2 was available. The performance path is checked only against Clavia's code
  and a synthetic file (`pch2dump.py selftest`).
* Patches with 10 variations (USB) are not in the corpus. The generic `4D` codec handles them.

---

## 9. Real-world files (5,593 public patches and performances)

Checked with `g2tool check` against Clavia's factory bank v1.24, the Mutator and
Richard Devine banks, the clavia.se v1.10 sets, the demo patches, and about 4,500
community files (electro-music forum archive, GitHub). Every genuine G2 file
loads. Variants the loader accepts (all kept, so re-saving is exact):

* **MacBinary wrapping**: a 128-byte MacBinary header (type `PCH2`, creator `NORD`) before the file.
* **Zero padding after the CRC**: e.g. a file padded with zeros to 3,584 bytes (7 × 512).
* **Headerless**: the patch name (16 bytes, NUL-terminated if shorter) replaces the
  text header. Seen in one third-party file, whose CRC is also wrong.
* **Non-zero padding bits** at section ends, in the Mutator bank, ScratchIt_DLX and
  synth-originated files. Clavia's reader skips them; Clavia's writer zeroes them.
* **Names, parameters and custom data in module-list order** instead of index order (7 files).
* **An empty `4D` list declaring 9 variations** instead of 0.
* **Extensions that don't match the content** (a performance saved as `.pch2` and the
  reverse): the file type byte decides.

Answers to §8 from the 1,101 real performances:
* `unknown_08`, `unknown_18` and the reserved bytes: always 0.
* MIDI channel: 0–16; 16 occurs in 134 slots, so it most likely means "off".
* Global pages = 1 in 91 files; keyboard range enabled = 1 in 65.
* Keyboard morph assign: never non-zero. 10-variation patches: none found.
* **`21` legacy fields are not always 0**: in 93 headers (synth-originated), the
  first two hold the slot's keyboard range (e.g. Smoke1.prf2: 0–59 and 60–127),
  and `legacy_b2` = `legacy_g1` = 1. The model keeps them as they are.
* Mono mode: 0, 1 and 2 all occur.
