# Nord Modular G2: hardware, firmware, and running it inside the plugin

Feasibility study, 2026-10-09. The question: can G2fresh make the G2's sound without the hardware?

How claims are marked:
* Every claim gives its source: a URL, a file and offset, or **(inferred)** for my own deduction.
* **[C]** means read directly from Clavia's own code or data in our copies.
* Confidence is **high**, **medium** or **low**. When sources disagree, all of them are listed.

Firmware-derived files stay in the gitignored `original/firmware/` and `original/demo/`. The repository gets only the
extraction tool `tools/firmware/g2os.py` and this note. No Clavia bytes are committed.

## 0. Executive summary

**The hardware.**
* A Freescale ColdFire host CPU (MCF5407 V4 per a teardown; the boot code fits V4) drives 4 Motorola DSP56300-family
  DSPs. The expansion board, standard in the G2X, adds 4 more.
* Community teardowns name the DSPs as DSP56367 at up to 150 MHz. Clavia's documents never name the chip. The
  firmware is consistent with a 56367: about 1371 patch cycles per 96 kHz sample means a clock above about 132 MHz,
  which the 100/120 MHz DSP56362 cannot reach.
* The DSPs are loaded through 8-bit HDI08 host ports.
* The USB chip is a Philips ISP1181-family full-speed device.

**The firmware.**
* We have it. It lives in our own copy of Clavia's free updater, in the resources of
  `Nord Modular G2 OS Update.app`: `NMG2 128 "OS"` (533 KB) and `BOOT 128 "Loader"` (97 KB).
* I decoded the whole OS: header, checksums, LZO1X compression. It unpacks to:
  * 1.22 MB of ColdFire code and data, loaded at `0x30000400`;
  * a small internal-SRAM section.
* Inside the OS is a **library of DSP56300 code fragments**: at least 259 descriptors (P code plus X/Y data init),
  about 7,950 program words and 3,800 data words.
* ColdFire C++ code picks these fragments, patches them and loads them into the DSPs when a patch is loaded. So **the
  synth compiles each patch itself; the editor sends only module lists and parameters.** The editor binary contains
  no DSP code.

**Key finding: Clavia already ran this code on a PC.**
* Clavia's free **G2 Demo (2004)** is a software G2. Its Mac binary contains **212 of the 224 larger OS DSP fragments,
  byte-identical**.
* So Clavia ran this same DSP56300 module code on a desktop CPU (one voice, in 2004).
* This is strong evidence that running the G2's real DSP code in software is practical.

**Recommendation.**
* Main path: **low-level emulation (approach 1)**. Run the user-supplied OS on an emulated ColdFire plus 4 emulated
  DSP56367s.
  * Use Gearmulator's GPL-3 `dsp56300` library, which is compatible with our AGPL-3-or-later.
  * The plugin would never ship Clavia code. It would unpack the user's own updater with `g2os.py` logic, as
    Gearmulator does with ROMs.
  * Precedent: **G1-Emu** (Sept 2026) already does this for the original Nord Modular (MC68331 plus 4× DSP56303) on
    Gearmulator's core.
* Cheap first step, also the building block for approach 3: a **DSP-only harness**. Load one OS fragment (for example
  an oscillator) into an emulated DSP56367 with a tiny hand-written sample loop, and get a waveform out.
* Native C++ re-implementation (approach 3) stays the long-term option for a light, portable engine. Fragments run in
  the harness become its golden reference.

## 1. Hardware (Part A)

### 1.1 Summary table

| Item | Finding | Source | Confidence |
|---|---|---|---|
| DSP count | 4, or 8 with the expansion board. "a single voice cannot exceed the code that fits in one DSP-chip… The FX Area can also use up to one DSP" | G2 User Manual v1.4x p.138, https://www.nordkeyboards.com/wt/documents/280/Nord%20Modular%20G2%20English%20User%20Manual%20v1.4%20Edition%201.4x.pdf | high |
| | Service manual rev 1.2 p.8: "four DSP´s U16-U19 (23280), which is controlled by a host processor U14 (23180)". Error codes list "Mnb U16-U20" (U20 is probably a typo) and "Exp U5-U8" | https://electro-music.com/forum/download.php?id=50120 | high |
| | The OS fills a table of 8 host-port addresses: 4 used on a base unit, 8 when the expansion is present (§2.6) | [C] OS CODE @0x300391e8 | high |
| DSP part | **Variant 1:** "4x Motorola 56367 @ 150MHz" (jksuperstar, 2006, teardown-style list) | https://electro-music.com/forum/post-73022.html | medium |
| | **Variant 2:** "56362", seen only in search-engine summaries, never on a loadable page; probably confused with the Nord Lead 2X/3 | — | low |
| | **Variant 3:** Gearspace snippets: "speculated… DSP56367 @ 150 MHz" (page returns 403) | gearspace thread 1241432 | low |
| | Clavia part "23280 DSP NMG2/NS" is the same part as the Nord Stage's; a 1.8 V rail powers "Host and DSP´s" (the 56367 core is 1.8 V, the 56362 is 3.3 V) | service manual; DSP56367 datasheet (mouser), DSP56362 product brief (NXP) | medium (inferred) |
| | Firmware is consistent with a 56367 at about 150 MHz. The editor's limit is 1371 cycles per sample for a patch (`re/notes/patch-load.md`). 1371 × 96 kHz ≈ 132 MHz, before the kernel's own cycles. A DSP56362 (100–120 MHz) cannot do that | (inferred) | medium |
| DSP family | Confirmed DSP56300 by the code itself: it uses 56300-only instructions (`IFcc` conditional execution, `normf`, `cmpm`, `Tcc`, `lua`) and the HDI08 host port | [C] OS CODE, disassembled with dsp56300's `dsp56kDisassemble` | high |
| DSP clock | The OS's DSP loader writes `PCTL = $040003`: PLL enabled, multiply by 4. The EXTAL frequency is unknown | [C] OS @0x300EC5CC | high (multiplier) / clock unknown |
| | Claims: "150MHz" (the part's rating, not a measurement); "double mhz (160)" (sequencer.de) | post-73022; http://www.sequencer.de/clavia_synthesizer/clavia_g2_modular.html | low |
| Host CPU | "1x Freescale ColdFire MCF5407 @ 162MHz (233 MIPS)" | post-73022 | medium |
| | Firmware fits: the code is ColdFire (MOVEC to MBAR `$C0F`); chip-select, SDRAM controller, UART and GPIO registers sit at the MCF5307/5407 offsets; CACR writes use branch-cache bits (BEC/BCINVA) that exist on the V4 MCF5407, not on the V3 MCF5307 | [C] BOOT @0x16..0x200 | medium-high |
| Host RAM | 4 MB SDRAM at `0x30000000`. Boot code: DACR0 base `0x30000000`, DMR0 `0x003C0001` (4 MB block), stack at `0x30400000` | [C] BOOT @0x17c..0x1a4 | high |
| | Service manual: "two DRAM circuits U12 and U13 (23170) (1M*16 bits)" | service manual p.8 | high |
| Boot flash | "BootPROM U21 (23890) 512k*8". Boot chip select CS0 at address 0, 8-bit port, 512 KB window | service manual; [C] BOOT CSMR0=`0x00070001`, CSCR0=`0x1940` | high |
| OS/patch flash | "Flash memory U7/U37 (24000) (16M*16 bits)". The OS uses AMD-style command cycles at `0x12000554/0x12000AAA` on CS2 (`0x12000000`, 8 MB window, 16-bit), and has the strings "UNKNOWN CHIP" and "INIT FLASH" | service manual; [C] OS (e.g. @0x30003CDE) | high |
| DSP external RAM | 256 kWord per DSP ("256 kWord of 24-bit RAM memory", user manual). Physically 16 bits wide: "262,144 words x 16 bits… AS7C34098" (varice), which is why delays and reverbs are 16-bit | user manual; https://electro-music.com/forum/post-281859.html | high / medium |
| | Other variant: "4x 256Kx16 SDRAM" (jksuperstar). The AS7C34098 is an asynchronous SRAM, so this is probably a mislabel | post-73022 | low |
| EEPROM | U36 (24147), 2k×8 | service manual | high |
| USB | USB 1.1 (official specs). Chip claimed as "Philips ISP1181B" (jksuperstar; "ISP1181BDGG", Estarriol); service manual: "USB circuit U24 (23190)", a 6 MHz crystal in the parts list | https://www.nordkeyboards.com/legacy-products/nord-modular-g2/specifications/; https://electro-music.com/forum/topic-52940.html | medium |
| | Firmware agrees: a command/data port pair at `0x13000010/0x13000000`. The command bytes used (B0 unlock, B2/B3 scratch, B4 frame no., B5 chip ID, B6/B7 address, B8/B9 mode, BA/BB HW config, C0 + 32-bit read = interrupt register, C2/C3 interrupt enable, F0–F3 DMA, F4 ack setup, F6 reset, 40 stall, 50 status) match the ISP1181 command set | [C] OS @0x30053C54, 0x300547B4, … | medium-high |
| Converters | Official: 24-bit 96 kHz ADC and DAC. Community: "2x CS4392K" DAC, "2x CS5341CZ" ADC, LM833 op-amps. Service manual: "D/A… U32 and U33 (23430)", "23430 Dac N2X/NMG2/NS", "23540 ADC NMG2" | specs page; post-73022; service manual | high (count) / medium (part numbers) |
| Sample rate | "Modules can process and output signals at two sample rates: 96kHz and 24kHz" (user manual); control rate = every 4th sample | user manual p.~70; editor (`patch-load.md`: "B code… every 4th sample") | high |
| | Open: the parts list has a 56.620363 MHz oscillator; 56.620363 MHz / 576 = 98.30 kHz, the measured rate of the sibling Nord Lead 2X. The real G2 rate may be about 98.3 kHz, not 96.000 | service manual; Gearmulator n2xtypes.h (NL2X measured 98.2 kHz) | low (unverified) |
| Inter-DSP links | "the interslot connections are in fact a real hardware connection between the dsps… 24 samples of latency (@96 kHz)" (measured behaviour); service manual error "Serial bus error" on "SDO 1-SDO 3" (serial audio links) | https://electro-music.com/forum/topic-69081.html, post-280718; service manual | medium / low |
| | **The real topology is not documented anywhere I found.** It must be reverse-engineered from the OS and the DSP kernel | — | — |
| Panel | Panel board: LCDs, encoders, MAX1039 ADC for the pedals, 74HC/LCX logic, no separate microcontroller in the parts list. The OS drives byte latches at `0x15000000..07` (CS5) and a 16-bit device at `0x14000000` (CS4) | service manual; [C] BOOT @0x118, OS @0x3005BD7A, @0x30029D8E | medium |
| FPGA | None in the parts list, none mentioned anywhere | service manual | medium |
| Expansion board | Part 60179, 4 DSPs ("23280") plus "Sram NMG2/NS 4MB" (probably 4 Mbit per DSP), plugs into two sockets, auto-detected by the OS ("Exp"). Standard in the G2X | https://www.nordkeyboards.com/wt/documents/236/…Expansion…Installation…pdf; https://www.encoreelectronics.com/cont_dsp1.html; service manual | high |
| G2 Engine vs keyboard vs G2X | Same main board family. Same OS and boot image for all; the OS has the strings "Nord Modular G2", "G2X", "G2 Rack", "G2 Engine" | [C] OS strings @0x300E9F86, 0x300EC05C | high |

### 1.2 Original Nord Modular (1997), for comparison
* Rack and keyboard: 4× DSP56303, 8 with the expansion; host CPU MC68331. Sources: https://www.gearnews.com/motorola-synths,
  https://theusualsuspects.io/docs/faqs. **High.**
* Micro Modular: 1× DSP56303. Same sources. **High.**
* G1-Emu's reverse-engineering notes (https://github.com/animatek/G1-Emu, NOTES.md). **Medium-high.**
  * DSP clock 82.944 MHz, which is 864 cycles per sample at 96 kHz.
  * DSPs chained DSP0→DSP1→DSP2→DSP3→codec over ESSI.
  * 1 MB RAM.
* The G2 is the same concept with about 1.75× the DSP clock per chip and a much bigger host.

### 1.3 Synths emulated by Gearmulator (dsp56300 core)

| Device (plugin) | DSPs | Host CPU and how it is handled | Source |
|---|---|---|---|
| Virus A (Osirus) | 1× DSP56303, emulated at 72 MHz (stock 66) | 8051-family MCU, reimplemented in C++ | https://theusualsuspects.io (osirus-c page) |
| Virus B (Osirus) | 1× DSP56311, 108 MHz (stock 100) | same | same |
| Virus C (Osirus) | 1× DSP56362, 136 MHz (stock 120), Fs 46,875 Hz | same | same |
| Virus TI/TI2 (OsTIrus) | 2× DSP56367 at about 133–153 MHz | 8051 (UPSD3212), reimplemented | same |
| Virus TI Snow | 1× DSP56367, 163 MHz | same | same |
| Waldorf microQ (Vavra) | 1–3× DSP56362, 101.6 MHz | MC68331, emulated | TUS vavra page |
| Waldorf MW II/XT (Xenia) | 1–3× DSP56303 | MC68331, emulated | TUS xenia page |
| Nord Lead/Rack 2X (Nodal Red 2x) | 2× DSP56362, Fs 98,304 Hz in code | MC68331, emulated; three HDI08 windows (A, B, "Both" broadcast for booting) | `source/claudia/n2x/n2xLib/n2xtypes.h`; https://theusualsuspects.io/technical/nodalred2x |
| Nord Modular G1 (G1-Emu, third party) | 4× DSP56303 | MC68331 (Musashi) | https://github.com/animatek/G1-Emu |

All sources are high confidence. The **G2 is not on Gearmulator's list**, and no G2 emulator exists that I could find
(GitHub searches; Discord not readable).

Gearmulator facts:
* **License:** GPL-3.0. The README badge says "GPLv3"; source files have no headers. **I found no "or later"
  statement.** (Checked in our clone of `dsp56300/dsp56300`, HEAD 123804394c16, 2026-10-09.)
* **Architecture:**
  * `dsp56300`: C++17 static library, interpreter plus JIT on asmjit, backends `jitops_*_x64.cpp` and
    `jitops_*_aarch64.cpp`.
  * Peripheral models: `Peripherals56303` (ESSI, HI08), `Peripherals56362` (ESAI, HDI08), `Peripherals56367`.
  * Other cores: `source/cpu/mc68k` (Musashi, MC68331) and `source/cpu/coldfire` ("ColdFire V2 core as in the
    MCF5206e"; **V2, not the G2's V4**).
  * Frameworks: synthLib, hardwareLib; JUCE plugins (VST3/AU/CLAP/LV2).
  * Sources: our clone; https://github.com/dsp56300/gearmulator/blob/main/CLAUDE.md.
* **Speed:** `doc/dsp_performance_history.md` gives, for Virus C, pure JIT throughput:
  * 1482 MIPS on a Ryzen 9 7950X3D;
  * 421 MIPS on a Cortex-A76 at 2.4 GHz.
  * FAQ: real time "on mid range CPUs… (Core i7 4790K)"; the TI2 mode with 2 DSPs "uses much more CPU".
  * I found no published CPU-percentage figure for the NL2X.

### 1.4 Clavia's G2 Demo (2004) is a software G2
* Official free download "Nord Modular G2 Demo v1.40", for Windows and for Mac (PowerPC). Released January 2004.
  * Limits: one voice, a few modules disabled, no audio in or MIDI out.
  * Sources: https://www.nordkeyboards.com/legacy-products/nord-modular-g2/downloads/,
    https://www.audiomasterclass.com/blog/clavia-releases-software-demo-of-nord-modular-g2. **High.**
* Community claims: "an entirely unoptimised emulator wrapping the actual g2 code… runs at an internal rate of 96KHz"
  (https://www.kvraudio.com/forum/viewtopic.php?t=70567, 2005). **Low-medium** on its own.
* **My check [C]:** `NMG2Demo` (Mach-O PPC, 5,590,348 bytes, in `original/demo/`) contains **212 of the 224 OS DSP
  fragments of 8 or more words, byte-identical**, as 32-bit big-endian containers, around file offsets
  0x299388–0x29B6F4.
  * So Clavia's software G2 executes the same DSP56300 module code on the host CPU.
  * The interpreter itself is not identified yet (the binary is stripped). That it is one is **(inferred)**, with
    medium-high confidence.

## 2. The OS / firmware (Part B)

### 2.1 Where it is
| File (our copies, in gitignored `original/`) | Content |
|---|---|
| `~/Downloads/Nord Modular G2 OS v1.62 Update.dmg` → `Nord Modular G2 OS Update.app` (copy in `original/mac/`) | The updater app "Nord Modular G2 Updater version 1.10" (`se.clavia.modular.dumptool`, fat i386+ppc; `original/mac/G2Updater_i386` is the i386 slice) |
| `…/Contents/Resources/Nord Modular G2 Updater.rsrc` (640,070 B, resource map in the data fork) | **`NMG2` id 128 "OS"**, 532,974 B, SHA-1 `8099aab372eb9a3fa7f147de0b931b96516f8017`. **`BOOT` id 128 "Loader"**, 97,456 B, SHA-1 `69380304039f1e1685fdd0351a08b13bc7aa50f5` |
| `~/Downloads/Nord Modular G2 OS v1.62 Update.zip` → `Nord Modular G2 v1.62 Setup.exe` (Wise installer, `vise32ex.dll`) | Installs the editor and "Update %ProductName% OS v1.62.exe", which presumably carries the same resources. **Not extracted**: Wise compression is not plain deflate and is not needed, since the Mac copy is complete |
| Editor binary `G2Editor_i386` / `.rsrc` | **No DSP code.** None of the fragments occurs in it in any byte order. It holds only the per-module resource estimates (`patch-load.md`) |

`tools/firmware/g2os.py unpack <Updater.rsrc> <outdir>` reproduces everything below. It verifies every checksum and
was run on our copies (`original/firmware/mac-updater-rsrc/`, `original/firmware/os-v162-unpacked/`).

### 2.2 OS image format [C]
Sources:
* Updater: `SwapAndCheckOSdata` @0x5e38, `SendNextUSBsect` @0x6140 (i386 slice).
* Boot loader: functions @0x8d86 (header check), @0x8ddc (section sanity), @0x9074 (load and unpack), @0x94ee (find
  "CODE" and jump), error strings @0x12c50.
* Layout: see the docstring of `tools/firmware/g2os.py`.

Contents of the 1.62 image:
* **Header:** version 162, format 1, word 0x0140, header checksum OK.
* **`SRAM`:** 1,625 B packed → 1,946 B, loaded at `0x20000800` (ColdFire internal SRAM).
* **`CODE`:** 530,624 B packed → 1,220,560 B, loaded at `0x30000400` (SDRAM). The boot loader jumps here.
* **Compression is LZO1X.** The boot loader's routine @0x6bf0 is the classic `lzo1x_decompress`.
* **Checksums** are 32-bit one's-complement byte sums (`~Σ`), of both the packed and the unpacked data.
* The boot loader accepts load addresses in `0x30000400..0x303C0000` or `0x20000000..0x20001000`, and sizes up to
  0x3BFC00.

**Update protocol.** Messages are framed with a 2-byte length and a CRC-16/CCITT (`SendUSBdata` @0x58fa):
* `0x0a` section header: version, word 4, load address, size;
* `0x0b` data chunks of up to 0x4000 bytes;
* `0x0c` section end with checksum;
* `0x0d` and `0x0e` finish;
* `0x80` and `0x81` single-byte requests.

The OS image is sent packed and unpacked on the synth. This matches the "update mode" status in `usb-protocol.md`.

### 2.3 BOOT "Loader" [C]
* **Raw ColdFire image linked at address 0** (the boot flash, CS0). Vector: SSP `0x30400000`, PC `0x00000016`.
* **What the init code does:**
  * Sets MBAR = `0x10000000` (MOVEC `$C0F`).
  * Programs chip selects CS0–CS7:

    | Chip select | Address | Use |
    |---|---|---|
    | CS0 | 0 | boot flash, 8-bit |
    | CS1 | `0x11000000` | DSP host ports, 8-bit, 1 wait state |
    | CS2 | `0x12000000` | flash, 8 MB, 16-bit |
    | CS3 | `0x13000000` | USB |
    | CS4 | `0x14000000` | |
    | CS5 | `0x15000000` | latches |
    | CS6 | `0x16000000` | |
    | CS7 | `0x17000000` | |

  * Programs the SDRAM controller (4 MB at `0x30000000`), the caches and ACR0 (peripheral space uncached), UART0 and
    GPIO.
* **Then:** validates the OS stored in flash, LZO-unpacks each section to its load address, and jumps to `CODE`.
  * On failure it reports an error, from "Header type err" to "No start code".
  * It also has its own USB path for update mode.
* **Code style:** C compiled with `link a6` frames, 517 functions (Ghidra headless, ColdFire language).
* **The updater carries it as well.** `CUSBHandler(CMemoryMedia* os, CMemoryMedia* boot)`; `SendDump` sends it.
  **(inferred)**: the updater can also refresh the boot flash.

### 2.4 CODE section map (1.22 MB at `0x30000400`)

| Range | Content | How established |
|---|---|---|
| `0x30000400–~0x300E6400` (≈942 KB) | ColdFire code: C++ built with gcc 2.95-era tools (RTTI strings `17__class_type_info`, `9bad_alloc`; static-init functions using the `0xFFFF` priority test) | [C] disassembly (`original/firmware/os-v162-unpacked/code68k.asm`, 292k lines, capstone 68040 mode) |
| `0x300E6400` | Sorted table of 216 function pointers (static constructors, about one per translation unit) | [C], purpose (inferred) |
| `0x300E6xxx–0x300ED56x` | Read-only data: sine and other tables, front-panel LCD strings, the font, product strings | [C] |
| `0x300EC5B8–0x300EC664` | **DSP bootstrap loader** (about 43 DSP words), see §2.6 | [C] |
| `0x300ED56C–0x301080F4` (≈106 KB) | **DSP fragment library**, interleaved with per-module LCD text ("Semi Freq Factor Partial", "Sine Tri Saw", "12dB 18dB 24dB"…) | [C] §2.5 |
| `0x30109xxx–0x3012A3D0` | Lookup tables (e.g. an exponential/pitch table at about `0x3010A470`), string tables, C++ vtables and `.data` | [C] |

### 2.5 The DSP fragment library: how the synth "compiles" a patch [C] / (inferred)

**Descriptor format.** Each fragment has a descriptor (40 bytes):
* `ptrX, ptrY, ptrP`: big-endian pointers;
* a marker `0xFF000000`, `0xFE000000` or `0xFD000000`;
* `u16 Plen, u16 flags`;
* `u32 nX, u32 nY`;
* `u32 cycles`;
* two zero words.

**What the pointers hold.**
* Each pointer leads to 24-bit DSP words stored in 32-bit big-endian containers (top byte 0).
* `ptrX` and `ptrY` hold the X and Y initial data (`nX`, `nY` words). `ptrP` holds `Plen` words of DSP56300 code.
* The `cycles` field is usually `Plen + 1`, sometimes larger, which suggests loops. **(inferred)**: it is the cycle
  cost used for the load meter.
* The OS has debug strings `DPS:%ld DSS:%ld DCS:%ld` (program size, data size, cycle sum). **(inferred)**

**Census** (my scan, which may miss odd layouts):
* 259 descriptors in total: 171 marked FF, 84 FE, 4 FD.
* 7,952 P words and 3,821 X/Y words altogether.
* The largest fragment, at `0x300FB2A0`, has 1,156 P words, 331 X and 349 Y, and flags 0x20. **(inferred)**: it is a
  per-DSP kernel or a big effect.
* Typical module fragments are 5–155 words.

**Code style.** The fragments are clearly hand-written DSP56300 assembly.
* Example: the code at `0x300EE400` uses `normf`, `cmpm`, `tlt`, `mac … ifec/ifes` and `asr #2`.
* They contain **placeholder operands** such as `move b,x:$0` and `move x1,x:(r6)+`.
  **(inferred)**: the host patches in addresses when it links a patch.

**Who uses them.**
* 251 of the 259 descriptors are referenced from ColdFire code (0x3006F7E2–0x300E5BB4).
* The references sit in static initialisers that build C++ wrapper objects: `{descriptor, descriptor+0xE, 0, vtable
  0x30128658}` (e.g. @0x3006F7E0).
* Per-module host code then uses these objects.

**Conclusion** (high confidence on the structure, inferred on the details):
* The synth holds every module's DSP code in its OS.
* The ColdFire OS is the **patch compiler**. It allocates voices to DSPs and links fragments with patched addresses.
  It sizes X/Y/P memory against limits that the editor mirrors (cycles 1371 per sample; X 4336, Y 2992, P 6498 words;
  ZP 128; dynamic RAM 131,072; Q 260,096; R 256; `patch-load.md`). Then it loads the result through the host ports.

### 2.6 How the DSPs are driven [C]
**Host-port table.** `0x300391E8` fills a table at `0x30116970` with 8 host-port base addresses plus one broadcast
address, choosing by an argument (expansion present):
* **expanded:** `0x110007B8, 0x110003F8, 0x110005F8, 0x110006F8, 0x11000778, 0x110007D8, 0x110007E8, 0x110007F0`,
  broadcast `0x11000000`;
* **base:** `0x110007B8, 0x110007D8, 0x110007E8`, then `0x110007F0` five times, broadcast `0x11000780`.

How to read the table:
* Address lines A3–A10 are one-hot, active-low chip selects. A3–A6 are the 4 main-board DSPs; A7–A10 are the 4
  expansion DSPs.
* A0–A2 select one of 8 byte registers. That is the HDI08 layout: the callers poll `2(a0) & 2`, which is ISR.TXDE.
* The broadcast address writes to several DSPs at once. The NL2X does the same (its "Both" window).

**DSP bootstrap loader** (`0x300EC5B8`, disassembled with `dsp56kDisassemble`):
* Sets OMR, then `movep #$040003,x:PCTL`.
* Reads a count and an address from HDI08 `HORX` and streams that many words into **X** memory.
* Does the same for **Y** memory. Host flag HF0 aborts either loop.
* Ends with `jmp $FF0000`: re-entering the DSP's boot ROM, which then loads **P** memory through the host port.

An emulator therefore needs:
* the DSP56367 bootstrap ROM behaviour in host-boot mode (dsp56300 has `dspBootCode.cpp`);
* HDI08 timing that is good enough.

### 2.7 Files produced (all gitignored)
* `original/firmware/mac-updater-rsrc/`: every updater resource (`NMG2/128_OS.bin`, `BOOT/128_Loader.bin`, …).
* `original/firmware/os-v162-unpacked/`:
  * `CODE_30000400.bin`, `SRAM_20000800.bin`;
  * `code68k.asm` (ColdFire disassembly);
  * `dataregion_24bit_from_300e6b4c.bin` and `dataregion.dis` (data region as packed 24-bit words and its DSP
    disassembly; only the fragment parts are real code).
* `original/demo/`: the official G2 Demo v1.40 (`demo-win.zip`, `demo-mac.dmg`, `NMG2Demo` PPC binary, `.rsrc`).
* Tools used: Ghidra 12.1.4 headless (ColdFire language) for BOOT; capstone (M68K) in a scratch venv; Gearmulator's
  `dsp56kDisassemble` built from https://github.com/dsp56300/dsp56300 (HEAD 123804394c16).

## 3. Feasibility of making G2 sound in the plugin (Part C)

### 3.1 Approach 1: full low-level emulation with the user's own firmware

**What it is.** Emulate the ColdFire (MCF5407) running Clavia's OS, plus 4 (or 8) DSP56367s running what the OS
loads into them, plus enough glue (host ports, flash, USB, panel, audio links) that the OS believes it is on a G2.

**What is needed.**
* **ColdFire V4 core.**
  * Option A: extend Gearmulator's V2 core (MCF5206e: ISA_A, hardware divide, MAC).
  * Option B: a Musashi-derived core.
  * Missing pieces: V4 extras, the cache/ACR registers as no-ops, and whichever ISA_B or EMAC instructions the OS
    really uses (to be measured on `code68k.asm`).
* **MCF5407 peripherals:** SIM, interrupt controller, timers, UART0/1, DMA, chip selects and SDRAM controller
  (mostly pass-through), GPIO.
* **Glue and peripherals:**
  * 4/8 HDI08 host ports wired to dsp56300 HDI08 models, including the broadcast addresses.
  * AMD-style flash on CS2: command set, chip ID, erase and program. Back it with a file holding the user's patches.
  * EEPROM.
  * The ISP1181 USB device, modelled at its command level.
    * Bonus: our existing USB protocol client (`proto/`) can then talk to the **real** OS through it.
    * That gives patch editing, the synth's own load meters and banks in the emulator, with no extra work.
  * Panel latches and LCDs: stubs at first.
* **Audio path:**
  * Find in the DSP kernel how the DSPs pass audio to each other and to the DACs: ESAI/ESAI_1 serial links ("SDO
    1–3"), and the voice-to-FX routing.
  * Capture the DAC stream.
  * Resample 96 kHz (or 98.3 kHz) to the host rate.
* **User experience:** the user points the plugin at their own `Nord Modular G2 OS v1.62 Update.dmg`, `.zip` or `.rsrc`.
  We unpack it at run time with the `g2os.py` logic ported to C++, and never ship it.

**Performance.**
* **Worst case:** 4 DSPs × up to about 150 M instructions/s each.
  * Emulated DSPs run their idle and wait loops at full speed, so the cost does not depend on the patch.
* **Against measured JIT throughput** (about 1.5 G DSP instructions/s on a 7950X3D core, about 0.42 G on a
  Cortex-A76), **(inferred)**:
  * 1 DSP takes about 10% of a fast desktop core;
  * a base G2 is about 2× an emulated Virus TI2;
  * the G2X (8 DSPs) is about 4×.
  * That is feasible on a modern multi-core desktop with one thread per DSP; hard on low-end machines.
* **ColdFire:** the 162 MHz host, emulated with an interpreter, is about one more core. Much of its time is idle
  loops; idle detection helps.
* **Synchronisation:** DSPs and host must stay in lockstep per audio block, as Gearmulator does for NL2X/microQ.

**Effort.** Rough, for one developer working with AI assistance:
* ColdFire core and SIM: 4–6 weeks.
* Glue (HDI08, flash, USB, panel stubs): 3–5 weeks.
* Audio-link reverse engineering and the output path: 3–6 weeks.
* Plugin integration (threads, timing, state save): 3–4 weeks.
* **Total: about 3–5 months to a first playable G2.**

**Risks.**
1. Undocumented inter-DSP topology and timing (high).
2. ColdFire V4 fidelity: unusual instructions, interrupt timing (medium).
3. The OS may block on hardware it cannot see: panel ADC, EEPROM contents, expansion detection (medium; stubbable).
4. CPU load for 8 DSPs (medium).
5. Self-modifying DSP code: Gearmulator's JIT handles it (low).

**Legal.**
* The firmware stays Clavia's. It is user-supplied and never redistributed, which is Gearmulator's model.
* `dsp56300` and Gearmulator are GPL-3.0; G2fresh is AGPL-3.0-or-later. GPLv3 §13 and AGPLv3 §13 explicitly allow
  combining them; the combined plugin is distributed with the AGPL terms covering the whole and the GPL code keeping
  its license. **Compatible (high).**
* Keep trademarks out of the product name, as Gearmulator does.

**First proof of concept** (two independent spikes):
* **(a) DSP side:** link `dsp56300` (DSP56367 peripherals) into a small test tool.
  * Load an oscillator fragment's P/X/Y from `CODE_30000400.bin` into internal memory.
  * Add a 10-line hand-written sample loop: call the fragment, write its output register to ESAI TX.
  * Run it for 96,000 samples and plot. **Sine out = done.**
* **(b) Host side:** run `CODE_30000400.bin` from `0x30000400` on a ColdFire interpreter, with stubbed peripherals.
  * Log every write to `0x11000000–0x110007FF`.
  * Milestone: the captured X/Y/P boot streams for the 4 DSPs at power-on.
  * Then feed them to 4 dsp56300 instances.

### 3.2 Approach 2: partial emulation (DSPs only; our own host-side patch compiler)

**What it is.** Run only DSP56367 emulators with Clavia's fragments (taken from the user's firmware at run time). Our
own C++ replaces the ColdFire patch compiler: voice allocation, fragment linking, address patching, parameter and
morph updates, inter-DSP routing.

**What is needed.**
* Full reverse engineering of the patch compiler: about 942 KB of ColdFire C++, with per-module code for about 170
  module types.
* The DSP kernel and its runtime protocol (parameter writes, LED and meter readback).
* The same audio-link work as approach 1.

**Effort and risk.** About 4–8 months. High risk: every module's linking rules must be right. The upside is a lighter
runtime: no host emulation, and only the DSPs a patch needs.

**Note.** This is exactly what Clavia's Demo did (§1.4). Its PPC binary (stripped, CodeWarrior) is a second
implementation of the same compiler to compare against.

**Proof of concept.** Same as 1(a), then link two fragments by hand (oscillator → filter) with patched addresses.

### 3.3 Approach 3: native C++ re-implementation of the modules

**What it is.** Write each G2 module's algorithm in C++ (float or exact fixed-point). Use what we already have:
* `data/modules.json`, `params.json`, `param_text.json` (parameter ranges and display formulas);
* the uprate rules;
* the module graphs.

Clavia's DSP fragments serve as the specification.

**What is needed.**
* Map fragments to modules: descriptor ↔ module type, via the static-init objects and the per-module ColdFire code.
* Disassemble each fragment and port it.
* A golden-reference harness, which is approach 1(a) run per module, for bit-exact or tolerance tests.
* Listening tests where exactness is impractical: noise generators, 16-bit delay memory.

**Effort.**
* A first useful subset (oscillators, filters, envelopes, VCA, mixers, LFOs, about 40 modules): 2–3 months.
* Full coverage of about 170 types with exact behaviour: 6–12 months.

**Risks.** Fidelity (fixed-point saturation, 16-bit delay RAM, control-rate details), sheer volume, and keeping parity
with quirks users rely on.

**Upside.** No Clavia code at run time, so works without the firmware. Low CPU. Easiest to ship on every platform
(including iOS and Linux ARM), and polyphony is no longer bound by hardware.

**Proof of concept.** Port one oscillator fragment, then compare against 1(a) sample by sample.

### 3.4 Recommendation
1. **Now:** build the **DSP harness (1a)**. It is small (days), proves we can run Clavia's module code, and it is the
   golden-reference tool both approach 1 and approach 3 need.
2. **Next:** the **host-side spike (1b)**: boot the OS far enough to capture the DSP boot streams. This answers the
   biggest unknowns cheaply: the ColdFire ISA subset in use, the peripherals the OS waits on, and the kernel layout.
3. **Then decide.**
   * If 1b boots cleanly, pursue **approach 1** as the "authentic" engine. Its USB model reuses our protocol stack.
   * Keep **approach 3** as the long-term engine for a firmware-free, lightweight mode, verified module by module
     against the emulator.
   * Approach 2 is not recommended as a primary path. It needs the most reverse engineering for the least
     authenticity gain over 1.

## 4. Open questions
1. **DSP part number and clock.** The firmware is consistent with a 56367 at about 150 MHz, but the DSP EXTAL source
   is unknown. The 56.620363 MHz oscillator, the PCTL ×4 and the cycle budget need reconciling. Read the board markings
   from a high-resolution photo, or the oscillator net in the service manual's schematics if any.
2. **Exact sample rate:** 96.000 kHz or about 98.3 kHz (as on the NL2X)? It matters for pitch accuracy in any engine.
3. **Inter-DSP audio topology:** which ESAI/ESAI_1 (or other) links carry voices to the FX DSP and to the DACs, in
   what slot format. The "24-sample latency" between slots suggests block buffering.
4. **The DSP kernel:** where it lives (the 1,156-word FE/0x20 fragment?), its per-sample cycle overhead, how
   parameters, morphs, LEDs and meters are exchanged over HDI08.
5. **ColdFire ISA subset** used by the OS (ISA_A only, or ISA_B/EMAC instructions?), and MCF5407 vs MCF5307
   confirmation. The CACR branch-cache bits point to V4.
6. **The 12 OS fragments not found in the Demo:** modules missing from the Demo, or OS 1.62 changes after Demo 1.40?
   Mapping descriptors to module types would answer this and is needed for approach 3 anyway.
7. **The Windows updater payload** (Wise) is not extracted. The Mac copy is enough, but users on Windows will have the
   `.zip`/`.exe`. The plugin's firmware import must handle the Wise package, or ask for the DMG.
8. **Gearmulator license:** "GPL-3.0-only" or "or later"? Not stated. Either is compatible with AGPL-3.0-or-later via
   §13, but record whichever applies.
9. **The G2 Demo's DSP interpreter:** whether it emulates DSP56300 instructions generically or uses a reduced
   interpreter. Worth a look in the PPC binary (`original/demo/NMG2Demo`) when approach 2 or 3 starts.
