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

**Update (§3.6, same day): the host side runs.**
* The user's OS 1.62 boots on Unicorn (ColdFire V4e) with four emulated DSP56367s on its host ports.
* G2fresh's own protocol client connects to it through an emulated ISP1181 and syncs.
* An uploaded patch is compiled by Clavia's OS onto a DSP. Its OscA, run frame by frame, gives a 1000.34 Hz sine
  for the "1 kHz reference" patch.
* What remains for real-time sound: inter-DSP audio links (they pace the frames) and speed.

**Update (§3.7): the inter-DSP audio links run.**
* The DSPs form a serial chain A6 → A5 → A4 → A3 on ESAI (A6 is the frame-clock master and takes the ADCs, A3 sends
  the DACs) and a ring on ESAI_1 (back from A3 to A6). One frame = one sample = 1536 DSP clocks on every interface.
* In the full machine, the frame programs run paced by the emulated serial frames, and the G2's output is A3's DAC
  transmitter: the 1 kHz patch gives 1000.33 Hz there (H3 −106 dB, H5 −82 dB). Patches the OS put on each of the
  four DSPs, an FX send and an inter-slot bus (once around the ring) all reach the DACs.
* Speed: 0.07–0.6 × real time on a loaded M1, with the interpreter.

**Update (§3.8): control-rate timing fixed.**
* The DSPs' control-rate code ran 1.9–2.2 × too often (LFOs fast, envelope stages 0.45 × their displayed time).
  Cause: a bug in the dsp56300 library. Interrupts are run by its JIT even on the interpreter, and the JIT stacked an
  SR without the condition codes the interpreter computes lazily. So a branch right after a compare went the wrong
  way when an interrupt fell in between, and the background loop's "every 4th frame" test passed early.
* `emu/CMakeLists.txt` patches the fetched library. Now LFO rates and envelope times match the editor's display to
  within 0.6 %, and the control-rate code runs exactly once per 4 frames on all four DSPs.

**Update (§3.9, 2026-10-10): a real-time C++ emulator.**
* `emu/g2emu` runs the user's OS on Gearmulator's ColdFire core (unchanged OS, plain ISA_A) and the four DSPs on
  dsp56300's JIT, with no Python and no Unicorn. Our protocol client talks to it over the emulated USB chip; notes
  play through the OS's MIDI IN (UART0, 31250 baud).
* One clock: per 96 kHz frame the ColdFire runs 1687.5 cycles (162 MHz) and its timers 562.5 bus clocks (54 MHz,
  fixed by the OS's MIDI divider [C]); the OS ticks 8272 times per second. OS-timed behaviour is now right: note-on
  in 2 ms, the post-upload level in 50 ms.
* The JIT needed `dynamicFastInterrupts` (stage 1 keeps subroutines in the vector area): that was the "TFS poll".
* Real time on an M1: typical patches on two threads (1.4-1.5 ×), heavy patches with chords on three (1.5-1.8 ×,
  experimental: a JIT crash seen before a timing change, cause not found). Since the second pass (§3.9.7): three
  threads by default, light patches 2.7-2.8 ×, heavy ones 1.8-2.4 ×, no underruns beside three busy processes.

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
| Panel | Panel board: LCDs, encoders, MAX1039 ADC for the pedals, 74HC/LCX logic, no separate microcontroller in the parts list. The OS drives byte latches at `0x15000000..07` (CS5: LED matrix, five HD44780 16×2 LCDs, input multiplexer) and the keyboard matrix's 16-bit column latch at `0x14000000` (CS4), rows on the GPIO port: §3.10 | service manual; [C] BOOT @0x118, OS @0x3005BD7A, @0x30029D8E; [emulated] | high |
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

## 3.5 Proof of concept: Clavia's DSP code on the emulated DSP56300 (2026-10-09)

`emu/` (built with `-DG2_BUILD_EMU=ON`; fetches Gearmulator's `dsp56300` at commit 1238043, GPL-3.0) and
`tools/firmware/g2frags.py` (exports the fragments from the user's own firmware into `original/firmware/fragments/`).

**What a fragment needs to run** (established by running them [C]):
* Fragments are straight-line DSP56300 code: no calls, no returns.
* They reach their own state through **`r3` (their X block) and `r4` (their Y block)**: parameters and coefficients
  in X, filter/oscillator state in Y. Linking a module instance mostly means pointing `r3`/`r4` at its memory.
* Cables are **short absolute addresses** (6-bit `aa`), left as `x:$0` / `y:$0` in the OS and patched when the synth
  links a patch: loads are inputs, stores outputs. So signals live in a zero page of X memory, which is what the
  editor's "ZP 128" resource (`patch-load.md`) counts.
* Some fragments write results through a pointer kept in their X block (`move x:(r3)+,r1` … `move b,x:(r1)+`).

**Results.**
* `0x300F2772`, a **5-pole lowpass** (five cascaded one-poles `y += k·(x − y)`, k in X[0], state in Y[0..4]),
  input `x:$10`, output `x:$11`: a 220 Hz saw through it, 96 000 samples, matches a double-precision model of the
  same filter within 2.2·10⁻⁶ (about −113 dB; the expected 24-bit fixed-point difference), output at 219.5 Hz.
  `0x300F279A` is the 6-pole version.
* `0x300F9FCC` computes filter coefficients from a cutoff parameter (π/4, 2/π, √½ constants; output independent of
  the audio input): a control-rate helper.
* `0x300EDF0C` is a two-input rotation/filter stage writing its result into its X block.
* **No sine yet.** An automated run of every fragment ≤ 200 words with neutral settings found no free-running pitched
  oscillator. The likely reason: oscillators take their pitch and waveforms from **shared tables in DSP memory**
  (exponential pitch table, waveform tables) that the OS sets up at boot, and that are not in any fragment's data
  (the large X blocks of `0x3010079E`/`0x300FF8E6` are delay lines, mostly zeros). Those tables come with the DSP
  boot images.

**Next step:** step 2 of 3.4, running the ColdFire OS with stubbed hardware and logging what it writes to the DSP
host ports, gives the boot images (kernel, tables) and, with the OS's module→fragment links, the way to build a whole
patch. The emulator side is proven: Clavia's module code runs unchanged and exactly.

## 3.6 Host-side emulation: the ColdFire OS on Unicorn, driving 4 emulated DSPs (2026-10-09)

**Tools.**
* `tools/firmware/g2hostemu.py` runs the user's own `CODE_30000400.bin`, `SRAM_20000800.bin` and boot `Loader`
  (read at run time) on Unicorn 2.1.4 (QEMU m68k, `UC_CPU_M68K_CFV4E`). It needs a venv with `pip install unicorn`.
* `emu/dspbridge/g2dspbridge.cpp` (CMake target `g2dspbridge`, a shared library loaded with ctypes) provides 4 DSP56367s
  (dsp56300, `Peripherals56362` + `Peripherals56367`). The host-port bytes the OS reads and writes go to their HDI08s.
* `emu/protobridge/g2protobridge.cpp` (target `g2protobridge`) puts G2fresh's own `proto::Client` behind a C
  interface. The emulated USB chip talks to it (3.6.6).
* `emu/dspframe/g2dspframe.cpp` (target `g2dspframe`) runs one DSP's frame program offline from the memories
  dumped after a patch upload (3.6.6).
* Build: `cmake --build build --target g2dspbridge g2protobridge g2dspframe` (with `-DG2_BUILD_EMU=ON`).
* Output: `--out` (default `original/firmware/dsp-boot/`, gitignored).
  * **Naming.** `dspN` is the DSP on chip-select line A(3+N), which is OS DSP 3−N on a base unit.
  * **Per DSP:**
    * `dspN.events`: the host-port stream;
    * `dspN_boot_P.bin`: the program loaded through the boot ROM;
    * `dspN_{X,Y}.bin`: images read back from the emulated DSP over the ranges the OS wrote, as records of
      address (u32), count (u32) and 24-bit words;
    * `summary.json`.
  * The copy in `original/firmware/dsp-boot/` also has the disassemblies `dspN_boot_P.asm` and
    `dspN_frame_P_000235.{bin,asm}`.
* Typical run: `g2hostemu.py --steps 2000000000 --progress 5000`.

### 3.6.1 CPU emulation: Unicorn works, with four workarounds

**ISA used by the OS** [C] (opcode census of `code68k.asm`):
* Plain ColdFire ISA_A: `muls.l`, but no hardware divide (gcc library calls), no MAC/EMAC, no `mov3q`/`mvs`/`mvz`.
* The `.byte` entries in the listing are switch tables.
* Eleven `MOVEC`: CACR, ACR0, VBR = `0x30000000`, RAMBAR0/1 (`0x20000001`, `0x20000801`, …).
* QEMU's ColdFire model executes all of it.

The four workarounds, each found by a failure:
1. **MOVEC to RAMBAR/MBAR** (`$C04/$C05/$C0F`) is not in QEMU. Code hooks on those addresses skip it. CACR/ACR/VBR run
   natively.
2. **Unicorn performs no m68k exception, not even RTE.**
   * QEMU raises `EXCP_RTE` (0x100) for `rte`. Unicorn passes it to `UC_HOOK_INTR` and does nothing else; without a hook
     the run aborts.
   * The hook pops the ColdFire frame (format/vector/SR, PC) itself.
3. **Reading SR through Unicorn corrupts the condition codes.**
   * After a stop, and even in a block hook, `reg_read(SR)` evaluates the lazy CCR wrongly and writes it back. A
     `tst`/`beq` around the read then branches wrongly.
   * Found as one lost word in a DSP boot stream: the OS's send loop skipped it.
   * Writing SR is safe. So interrupts are entered through a guest-code stub (`STUB_BASE`, `interrupt()`): the return
     PC is pushed, then `move.w sr,d0` captures SR exactly. If the mask is too high it returns untouched; otherwise it
     builds the ColdFire frame, raises the IPL and jumps through the VBR vector.
4. **Slices must end at translation-block boundaries.**
   * Stopping by wall clock (`timeout`) re-executed MMIO writes: about 1.5% of host commands came out doubled.
   * Stopping by instruction count (`count`) stops mid-block, where QEMU has not stored its lazy condition-code
     state. Seen once in about 10,000 slices: the stop fell between `cmp` and `bcc` at `0x3002725E`, an interrupt was
     taken there, the `bcc` went the wrong way, a slot counter reached 4, and the OS crashed later on a garbage
     slot-object pointer.
   * So a block hook counts about one instruction per 3 bytes and calls `emu_stop()` at a block start: 20,000
     "instructions" per slice. Timers, interrupts, the USB host and logging run between slices.

**Speed.**
* About 5 M ColdFire instructions/s on average: 10⁹ instructions (50,000 timer ticks) take 3 minutes on an
  8-core Apple Silicon Mac.
* It is slower while the OS polls the host ports, since each access is one Python callback.
* The four DSP interpreter threads run alongside, about 500% CPU in total.

**Gearmulator's ColdFire V2 core and QEMU system emulation were not needed.**

### 3.6.2 Hardware stubs and models (what the OS needed to get this far)

| Address | Model | Why / finding |
|---|---|---|
| MBAR `0x10000000` SIM | IMR/IPR/ICR/AVCR registers; IMR bit = 8 + ICR index (TIMER0 9, TIMER1 10, I2C 11, UART0 12) [manual] | the OS unmasks TIMER1 (ICR2 = `0x84`, autovector level 1 → vector 25, the 128-slot tick scheduler at `0x30001894`), UART0 (vector 66) and external IRQ3 (AVCR bit 3, autovector 27 = the ISP1181 handler `0x30053C38`) [C] |
| MBAR timers | TMR/TRR/TCN/TER, timer 1 fires every slice (TRR 50, prescaler 128 ≈ 6.5k bus clocks per tick on the hardware) | timer 0 free-running (`TMR 0x2B`) for timing |
| MBAR UART0/1 | always ready, TX logged | nothing printed by OS 1.62 |
| MBAR I2C | master that every slave acknowledges; data reads return `0x80` (stub, see 3.6.3 step 5) | device `0xCA/0xCB` (7-bit `0x65`): `CA AA`, `CA 0D` at boot, then an interrupt/tick-driven read loop (`0x30056536`) [C] |
| CS1 `0x11000000` | 8 HDI08 ports (A3..A10 one-hot, active low, A0–A2 register), word/long accesses split into byte cycles (the OS writes words as `move.l d,4(a0)` → bytes 4..7) [C] | see 3.6.3 |
| CS2 `0x12000000` | 8 MB AMD-style flash, **CFI**, erased | the OS identifies the chip by CFI (`0x98` at word `0x55`, "QRY", then command set 1 = Intel 64×128 KB or 2 = AMD 128×64 KB) [C] `0x300042E6`; anything else gives "FLASH FAILURE / UNKNOWN CHIP" and a halt. We answer 2. Which chip the G2 has is not known |
| CS3 `0x13000000` | ISP1181 model (`Isp1181`): command port `+0x10`, data port `+0`, IRQ3 | standard ISP1181 command set used [C]: `B0` unlock (`AA37`), `B2/B3`, `B4`, `B5` chip ID, `B6/B7` address, `B8/B9` mode, `BA/BB` hardware config, `C0` interrupt register (4 bytes), `C2/C3` interrupt enable, `F0–F3` DMA, `F4` ack setup, `F6` reset, endpoint index i: `0x00+i` write / `0x10+i` read (LE16 length, data) / `0x20+i` config / `0x50+i` status / `0x60+i` validate / `0x70+i` clear. The OS configures index 2 = `E1` (IN, 16 B: interrupt-IN 0x81), 3 = `E3` (IN, 64 B: bulk-IN 0x82), 4 = `83` (OUT, 64 B: bulk-OUT 0x03) and enables interrupts `0x1F07`; the IRQ3 handler `0x30053C38` dispatches bit 8 → EP0 OUT, 9 → EP0 IN, 10+ → handlers at `0x30119C62` (registered by `0x300553C2`: bulk-OUT handler `0x30055D36` reads the LE16-prefixed buffer, gathers a frame by its BE16 length and posts it for parsing) [C] |
| CS4 `0x14000000`, CS5 `0x15000000` | latches, read 0 (the C++ emulator models them since: §3.10) | keyboard matrix columns (`CS4 +0` ← `7FFF/BFFF/DFFF…`), LEDs, LCDs, input multiplexer (`CS5 +0..7`) |

### 3.6.3 How the OS boots the DSPs [C]

All of this was observed in the emulator, with the emulated DSPs answering for real.

1. **Port table.** `0x300391E8` builds the base-unit table: OS DSP 0..3 = `0x7B8, 0x7D8, 0x7E8, 0x7F0`, i.e. chip
   selects A6, A5, A4, A3. Broadcast is `0x780`.
2. **Reset.** For each DSP: ICR = `0x80` (INIT); then wait for INIT to clear.
3. **Stage 1 through the boot ROM.** Per DSP: count `0x23D`, address 0, then 573 P words. The sender
   (`0x30058D40`) polls TXDE with a 400-tick timeout and **drops the word on timeout**.
4. **The stage-1 program.** About 573 words of hand-written DSP code; it is the same on all DSPs except the serial
   set-up.
   * **Init** (`P:$13B`, or `$12D` on A3):
     * PCTL `$1D000A`, OMR `$20038F`.
     * ESAI and ESAI_1 in network mode. TX slots 8 (`TCCR TDC=7`), RX 2 or 8.
     * Port C/E pin functions.
     * DMA 2..5 set up between ESAI and X:`$1C00–$1FFF`. Channels 4/5 restart from their done interrupts
       (`P:$20/$22`).
     * Timer 0.
     * AAR0 = `$800031`: external SRAM at `$800000` shared by X and Y; stage 1 clears X:`$800800–$83FFFF`.
     * HCR = HCIE.
     * `r6 = $1BC0`, `n2 = $18FC`, `n5 = $19C0`, `m0 = m3 = -1`. The module fragments use these (3.5).
   * **Main loop** (`P:$222`): spin. The ESAI_1 receive-last-slot interrupt (`P:$76: jsr $236`) counts frames in
     X:`$43`.
   * **HF0 handshake** (`P:$F4`). When the host sets HF0, the DSP:
     * clears the DMA buffers and X/Y `$0–$3F`;
     * sets HF2 and waits for HF0 to clear;
     * then writes **DOR0 into `P:$77`**, the target of the per-frame `jsr`.
     * (inferred) So the OS starts a patch program by setting DOR0 (host command `$A0`) and toggling HF0: from then on
       the patch code runs once per frame.
   * **Host-command vectors `P:$80–$B6` are memory primitives**:

     | Vector | Effect |
     |---|---|
     | `$9A` | `r0 = HORX` |
     | `$80/$82/$84` | X/Y/P:(r0)+ = HORX |
     | `$86/$88/$8A` | X/Y/P:(r0) = HORX |
     | `$8C/$8E` | read X/Y:(r0) back, then write |
     | `$90..$96` | read P/X/Y back |
     | `$98` | LA |
     | `$A0/$A2/$A4` | DOR0, DOR1, DCO1 |
     | `$AA` | TLR0 |
     | `$AC` | HOTX = r0 |
     | `$AE` | echo |
     | `$B0..$B6` | EP register |

5. **Three variants of stage 1.**
   * **A4 and A5** are identical.
   * **A6** differs only in clocking: it drives the ESAI transmit clock and frame sync.
   * **A3** (OS DSP 3, the last one) differs in 277 words. It is a frame-sync slave on ESAI receive, waits for RFS
     before enabling its transmitters, and starts timer 0 (TIO0).
   * **The A3 timer is calibrated** [C] `0x30055F28`. The OS reprograms that timer (TLR0 via `$AA`, `0x3005C0F0`) in
     a servo loop of up to 10 × 100 I2C-ADC samples until a 256-bin histogram peaks near the middle. **(inferred)**:
     the A3 timer output feeds something the ADC sees.
6. **Then data only.** About 6,400 host commands write tables, identical on the 4 DSPs. **No P code** is loaded at
   power-on: with an erased flash there is no patch to compile.

   | Range | Words | Content |
   |---|---|---|
   | X:`$40` | 1 | 0 |
   | X:`$14EF–$1737` | 585 | rising curve from 0 (exponential/envelope) |
   | X:`$173A–$173D` | 4 | 0.5 |
   | X:`$173F–$19BF` | 641 | falling curve from 0.1395 |
   | X:`$1A40–$1BBF` | 384 | includes 1/12, decay series, and at X:`$1AC0` **0.5·2^(n/12)** (semitone ratios) |
   | Y:`$1140–$1BBF` | 2,688 | starts with the same 4-tap coefficients, then further curves |
   | Y:`$800000–$8007FF` (external SRAM) | 2,048 | **512 × 4 cubic interpolation coefficients**: [0,1,0,0], …, at ½ [−1/16, 9/16, 9/16, −1/16] |

7. **After the I2C calibration and a flash format, a frame program** [C, emulated].
   * The OS treats the blank flash as corrupt: it erases every sector and writes a fresh layout using AMD unlock
     bypass (`AA 55 20`, then `A0`+word, exit `90 00`).
   * Then it loads each DSP's **per-frame program** for the empty patch at P:`$235`:
     * 71 words on A4/A5, 91 on A3, 106 on A6;
     * DOR0 = `$236` (the entry), LA = `$235`, DOR1 = DCO1 = `$50`.
   * Through the HF0 handshake, that entry becomes the target of the per-frame interrupt.
   * **What the frame program does:**
     * saves all registers on the r6 stack;
     * swaps the double-buffer pointers at L:`$45..`;
     * waits for DMA 2/3 (DSTR), drains the ESAI receivers and re-arms DMA 2/3 on the next buffers;
     * increments the frame counter, restores, `rti`.
   * **A3's version also scales the output buffer** by a level read from X:`$1739` (`mpy -x1,y0`, `asl #3`). So A3,
     OS DSP 3, is the output DSP **(inferred)**.
   * **(inferred)** The patch compiler inserts module code into this frame.
8. **Patch-time upload.** Inferred from the code first; seen running in 3.6.6. The OS uses `$9A` (set r0) at 909 sites, single X/Y/P
   word writes `$86/$88/$8A` at 320/204/149 sites (parameter and address patching), and streaming P (`$84`) at 5 sites:
   * `0x300327C8` streams a fragment's P words straight from its descriptor (the patch linker);
   * `0x30038D3C` streams a 258-byte (86-word) blob from `0x301095A6`;
   * DOR0 (`$A0`) at 7 sites.

### 3.6.4 The DSP side (g2dspbridge)

**How the DSPs run.** Each DSP runs free on its own thread with the dsp56300 interpreter, as Gearmulator's boards do.
Two reasons:
* asmjit's JIT and Unicorn's JIT in one thread crash with SIGBUS on Apple Silicon: both switch the thread's `MAP_JIT`
  write protection.
* The dsp56300 JIT never left the stage-1 poll `brset #TFS,x:SAISR,*` here (aarch64), while the interpreter did.
  The interpreter runs a whole DO FOREVER inside one step, so time slicing is not possible.

**Host side.** It uses only the library's thread-safe parts:
* the RX queue (the host may write ahead, order kept);
* `setPendingHostFlags01` for HF0/HF1;
* `injectHostCommand` for HC. The host still sees HC set while the command is pending.
* The HOTX queue for reads.

**Boot ROM.** Emulated on the DSP thread from the RX queue (count, address, words via `DspBoot`). A jump to
`$FF0000` re-enters it.

**ESAI receivers** got zeros here; transmit frames can be captured (`g2dsp_capture`, `g2dsp_tx_take`). Since §3.7 the
ESAIs are linked between the DSPs.

**Result** [C, emulated]:
* All 4 DSPs boot, answer the HF0/HF2 handshake and the read-back commands (`$AC`, `$AE`, `$B6`), consume every word
  (RX queue empty at the end), and sit in their main loop counting ESAI_1 frames.
* Reading their memories back gives exactly what the stream wrote (X/Y ranges 100%; external Y after the bridging fix).
* The default EXTAL of 12 MHz gives a 66 MHz core with PCTL `$1D000A` (MF = 11, PD = 3?). The real EXTAL is still open
  (§4.1).

### 3.6.5 Status, and what is not done

**How far the OS boots** [C, emulated]:
* reset and C runtime set-up, caches/SRAM/VBR, chip selects;
* UART0, timers, I2C codec/ADC set-up, flash identification (CFI);
* **all four DSPs booted and loaded with their tables**;
* the ISP1181 initialised;
* the DSP-timer/ADC calibration (about 7,500 ticks with the ADC stub);
* the flash formatted;
* the empty-patch frame programs loaded and started;
* then the normal main loop with the 128-slot tick scheduler, stable for 50,000 ticks (10⁹ instructions, nothing
  failing).

The panel (LCD, buttons, knobs on CS4/CS5) is unmodelled: reads give 0, and the OS does not wait on it.

**Stubbed** (so results that depend on them are not authentic):
* I2C slaves (always ACK, ADC reads `0x80`);
* panel latches;
* UART RX;
* ESAI receive data (zeros, no inter-DSP audio links; linked since §3.7);
* the DSP clock (dsp56300's default EXTAL gives 66 MHz; §3.7 sets 1536 instructions per frame);
* timer rate (one tick per 20,000 instructions; how that compares with DSP time: §3.8.5);
* the second (expansion) DSP bank: A7–A10 answer as stubs.

**Not done:**
* inter-DSP audio routing (ESAI/ESAI_1 links between the four DSPs and to the DACs; all four now transmit frames
  that go nowhere): done in §3.7;
* the expansion board;
* the DSP JIT (blocked by the TFS poll and the thread clash; the interpreter does about 110 M DSP instructions/s per
  thread);
* frames at real rate (below).

### 3.6.6 Our editor talks to the emulated G2 over USB; a patch compiles and its oscillator sounds (2026-10-09)

`g2hostemu.py --patch FILE.pch2` (about 2½ minutes in all).

**USB set-up.**
1. It boots.
2. At slice 11,000 it "plugs in" the cable: bus reset, then SET_ADDRESS and SET_CONFIGURATION as EP0 SETUP packets
   50 ticks later.
3. 100 ticks after that, G2fresh's own `proto::Client` (`emu/protobridge`) starts.

**Two traps** [emulator]:
* The OS's bus-reset handler clears the endpoint buffers, so anything sent together with the reset is lost. The OS
  then parses the empty buffer as a 0-length frame and answers `7E 01`.
* A host polls bulk-IN only after an extended announcement: zero-length packets the OS leaves there must not
  complete by themselves.

**What happens** [C, emulated]:
* **Version reply** `80 0A 00 00 00 1C 00 A2 00 12 …`: model `0A`, firmware `0x00A2` = 1.62, protocol `0x0012`.
  The client goes to **Connected**.
* **Full sync.** Synth settings, performance "New Performance", the four slots, flash names: 61 frames out,
  187 in, about 300 ticks.
* **Upload.** The client's `sendPatch` uploads `1khz_ref.pch2` (from `corpus-external/datanoisetv-…`; OscA → 4-Out)
  into slot A, and the OS acknowledges it.
* **The OS compiles the patch onto the DSP on A6 (OS DSP 0)** [C, emulated]:
  * the frame program grows to P:`$235–$351` (285 words): DOR0 → `$23E`, LA `$23D`, DOR1 `$5D`, DCO1 `$5B`;
  * module data: X:`$50–$5E`, Y:`$50–$5B`;
  * cables in the zero page X:`$0–$2`.
  * **OscA** is linked at P:`$293` with r3/r4 = X/Y:`$50`. It has its waveform branches (a polynomial sine at
    `$2D8`) and writes its output to **X:`$2`**.
  * Two output blocks copy X:`$2` into the double-buffered output area through `x:$45`.
  * **The pitch arrives as a phase increment.** X:`$51` = `0x02AAE4` = 1000.33 Hz at 96 kHz (`0x02AAAA` would be
    exactly 1 kHz).
  * Afterwards the OS polls the DSP continuously with read-back host commands (`$8C`/`$8E`, about 45,000 in 800
    ticks): meters and LEDs (inferred).
* **Frame rate is the gap.** In the full machine the oscillator's phase advances, but only a few frames run per
  second. The frame program waits for DMA 2/3, which stage 1 triggers from ESAI and ESAI_1 *receive* data
  (DCR2 DRS = `$0B`, DCR3 DRS = `$15`), and in this machine nothing sends that data at the real rate.
* **The samples.** `emu/dspframe/g2dspframe` takes the emulated DSP's memories after the upload
  (`dspN_live_*.bin`), removes the two DMA-done polls and runs the frame program once per sample.
  * X:`$2` over 96,000 frames is a **sine at 1000.34 Hz**, amplitude 0.250, DC −0.00004.
  * Harmonics: H3 −106 dB, H5 −82 dB, even harmonics below −170 dB. These are the marks of the polynomial sine.
  * Files: `original/firmware/dsp-boot/patch-1khz-ref/` (`dsp3_oscA_X2_1s.wav`, `dsp3_patch_P_000235.asm`, the
    streams, the live memories, `usb_traffic.txt`).

**This is Clavia's whole chain, emulated:** OS compiler, linker, parameter → pitch conversion, upload, and DSP
module code. It is driven by our own editor protocol, and only the frame pacing is bypassed.

### 3.6.7 Next steps (recommended order) and risks

1. **Frame pacing and audio links.** Model what really clocks the ESAI receivers: an inter-DSP TDM ring with A6 as
   clock master per its TCCR/RCCR, and A3 as the output DSP feeding the codec.
   * Wire each DSP's ESAI/ESAI_1 transmit frames to the next DSP's receivers.
   * Then frames run at the emulated sample rate, and A3's transmit slots are the audio out.
   * Risk: slot mapping and DMA modes need care (dsp56300 has the ESAI DMA sources; the TDM order is
     undocumented).
2. **Speed.** Python MMIO callbacks and four interpreter threads are fine for analysis, not for real time.
   * Port the host side to C++: Unicorn's C API, or Gearmulator's ColdFire core extended to V4, which would also
     remove the Unicorn workarounds.
   * Get the dsp56300 JIT to run the stage-1 TFS poll. Then a real-time engine is plausible (§3.1 estimate).
3. **Panel and flash persistence.** Keep the flash image between runs so that patches and settings persist.
4. **Patch-compiler RE for approach 3** is now much cheaper. Every module's linked code, data layout and parameter
   conversion can be captured by uploading one-module patches and diffing the streams.

**Risks still open:**
* The DSP EXTAL/sample rate (the pitch math assumes 96 kHz, and so does this check).
* The ADC calibration stub (if the real ADC feeds something audible, e.g. the input level, this matters).
* Unicorn quirks beyond the four found.

Step 1 is done in §3.7.

## 3.7 Inter-DSP audio links (2026-10-09)

**Result.** The four emulated DSPs now exchange their serial audio frames, so their frame programs run at the
emulated sample rate, paced by the serial frames, with no offline frame running. In the full machine (ColdFire OS on
Unicorn, our editor's protocol over the emulated USB), the 1 kHz reference patch comes out of A3's DAC transmitter.
Patches compiled onto each of the four DSPs all reach the DACs. Sources: the stage-1 programs
(`original/firmware/dsp-boot/dspN_boot_P.asm`), the frame programs of the live dumps, and emulated runs (data in
`original/firmware/esai-links/`, gitignored).

### 3.7.1 ESAI configuration per DSP [C]

Register values written by the stage-1 programs (the `movep`s at P:`$13B`, or `$12D` on A3). Bit fields per the
DSP56367 ESAI (the layout of dsp56300's `esai.h`). `dspN` is chip select A(3+N) = OS DSP 3−N (§3.6).

| | A6 (`dsp3`, OS DSP 0) | A5, A4 (`dsp2`, `dsp1`) | A3 (`dsp0`, OS DSP 3) |
|---|---|---|---|
| ESAI SAICR | `$0` (asynchronous) | `$40` (SYN: receiver on the transmitter's clocks) | `$0` |
| ESAI TX | TCR `$7903`: TE0, TE1, network, 24-bit slot/24-bit word. TCCR `$F40F03`: **master** (SCKT, FST, HCKT outputs), 8 slots (TDC 7), bit clock Fsys/(2·4·1) = Fsys/8 | TCR `$7903`, TCCR `$940F03`: slave, 8 slots | TCR `$7D03` (TE0, TE1, **32-bit slot**/24-bit word), TSMA `$3`, TCCR `$F48303`: **master**, 2 slots, bit clock Fsys/24: **the DACs** |
| ESAI RX | RCR `$7D03` (RE0, RE1, 32-bit slot), RSMA `$3`, RCCR `$F4C302`: **master**, 2 slots, bit clock Fsys/24: **the ADCs** | RCR `$7903` (RE0, RE1), RSMA `$FF`, 8 slots | RCR `$7903`, RSMA `$FF`, RCCR `$940F03`: slave, 8 slots |
| ESAI_1 SAICR_1 | `$0` | `$40` | `$40` |
| ESAI_1 TX | TCR_1 `$790C` (TE2, TE3), TCCR_1 `$840F03`: slave, 8 slots | same | same |
| ESAI_1 RX | RCR_1 `$7903` (RE0, RE1), RCCR_1 `$848F03`: slave, 8 slots | RCCR_1 `$E40F03` | RCCR_1 `$840F03` |
| pins | PCRC `$CFF` (SCKR, FSR, HCKR, SCKT, FST, HCKT, SDI0, SDI1, SDO1, SDO0); PCRE `$3DB` (SCKR_1, FSR_1, SCKT_1, FST_1, SDI0_1, SDI1_1, SDO3_1, SDO2_1); Y:`$FFFFAF` = `$C` | same | same |

What this gives:
* **One frame clock.** Every 8-slot side is 8 × 24 = 192 bits at Fsys/8: **1536 DSP clocks per frame**. The two
  2-slot converter sides (A6's ADC receiver, A3's DAC transmitter) are 2 × 32 = 64 bits at Fsys/24: also 1536 clocks.
  So one frame = one sample on every interface. At 96 kHz this is Fsys = 147.456 MHz; at the suspected 98.304 kHz it
  would be 151 MHz, above the 56367's 150 MHz rating. With PCTL `$1D000A` (×11/2, external clock: XTLD) EXTAL would
  be 26.81 MHz. The OS's pitch increments are exact for 96 kHz (§3.6.6), so 96 kHz is assumed (medium).
* **A6 is the clock master** of the 8-slot frames (its ESAI transmitter drives SCKT/FST); every other 8-slot side is
  a slave (inferred: they share A6's clock lines).
* **A3 waits for its ESAI receive frame sync**, then 950 instructions (`rep #$3B6`), before it enables its
  transmitters (TE0/TE1 to the DACs, TE2/TE3): it aligns the DAC frame to the TDM frame. It also starts timer 0
  (`TCSR0` bit 0), the one the OS calibrates through the I2C ADC (§3.6.3).

**DMA (all four)** [C], DCR decoded with dsp56300's `dma.cpp`:

| Channel | Source → destination | Count (DCO, 2-D) | Trigger, mode |
|---|---|---|---|
| 2 | ESAI RX0/RX1 (`$FFFFA8`, DOR2 = −1: RX0, RX1, back) → X:buffer | `$7001`: 8 lines of 2 (A6: `$1001`, 2 lines, into buffer+4) | ESAI receive data (DRS `$0B`), line, DE cleared at the end |
| 3 | ESAI_1 RX0/RX1 (Y:`$FFFF88`) → X:buffer+`$10` | `$7001` | ESAI_1 receive data (`$15`), line |
| 4 | X:buffer → ESAI TX0/TX1 (`$FFFFA0`) | `$7001` (A3: `$1001`, the DACs) | ESAI transmit data (`$0C`), line; done interrupt P:`$20` reloads DSR4 from X:`$46` |
| 5 | X:buffer+`$10` → ESAI_1 TX2/TX3 (Y:`$FFFF82`) | `$7001` | ESAI_1 transmit data (`$16`); P:`$22` reloads from Y:`$46` |

So a 32-word buffer holds one frame: word 2k is line 0 (RX0/TX0, or TX2) of slot k, word 2k+1 line 1. Words
`$0–$F` are ESAI, `$10–$1F` ESAI_1. Four buffers (X:`$1C00`, `$1D00`, `$1E00`, `$1F00`) rotate through X/Y:`$45..$48`
once per frame (§3.6.3): the buffer received in frame k is processed in place in frame k+1 (as `$45`) and
transmitted in frame k+2 (as `$46`). The frame interrupt is ESAI_1's receive-last-slot (P:`$76`). Every 4th frame
the background loop runs the control-rate code (frame counter X:`$43` > 3; details and an emulator fault in §3.8).

### 3.7.2 What travels in which slot [C, frame programs]

* **A6 starts each frame** (§3.6.3 frame program): it clears ESAI words `$0–$3` and `$8–$F` and ESAI_1 words
  `$10–$17`, keeps the ADC words `$4–$7` (DMA 2 writes them there; X:`$44` gets the first one), and moves ESAI_1
  words `$18–$1B` to `$1C–$1F` (clearing the source).
* **A5 and A4 pass the buffer through** untouched (their empty frame programs only rotate and re-arm).
* **A3** multiplies ESAI words `$0–$3` of the buffer it is about to send by −X:`$1739` × 8 (`mpy -x1,y0`,
  `asl #3`; X:`$1739` = 0.068 in our runs, a master level) and sends them to the DACs.
* **Output modules add into the slots.** The 2-Out/4-Out fragment reads the slot word, `mac`s the input times its
  level into it and writes it back (when the module is off, the OS clears the two `mac` opcodes' ALU bytes: the
  1 kHz reference patch has its 4-Out **off** in variation 0, which is why its outputs were silent; the tests below
  use a copy with it on). The word offsets are in the I/O modules' Y data (offset, then 2 for the right channel),
  read from the live memories of the runs below.

| Buffer words | Slots | Content |
|---|---|---|
| `$0, $2` | ESAI 0, 1 on line 0 (SDO0 → SDI0) | Out 1, Out 2: DAC 1 left/right on A3 [emulated] |
| `$1, $3` | ESAI 0, 1 on line 1 (SDO1 → SDI1) | Out 3, Out 4: DAC 2 [emulated] |
| `$4–$7` | ESAI 2, 3 | the 4 audio inputs (A6's ADC words), passed down the chain [C] |
| `$8–$F` | ESAI 4–7 | cleared by A6; not used by these tests |
| `$10, $12` | ESAI_1 0, 1 on line 0 | FX 1/2: written by a 2-Out (FX 1/2), read by the Fx-In of a DSP further down [emulated] |
| `$11, $13–$17` | ESAI_1 0–3 | cleared by A6; FX 3/4 presumably `$11, $13` (not tested) |
| `$18, $1A` | ESAI_1 4, 5 | Bus 1/2 as written by a 2-Out (Bus 1/2) [emulated] |
| `$1C, $1E` | ESAI_1 6, 7 | Bus 1/2 as read by a 2-In (Bus 1/2): A6 moves `$18–$1B` here, so a bus is read **one ring turn** after it is written, wherever the two slots sit [emulated] |

### 3.7.3 Wiring

| Line | From → to | Basis | Confidence |
|---|---|---|---|
| ADCs | → A6 ESAI RX0/RX1 (2 × 32-bit slots) | A6's 2-slot master receiver, its words kept in slots 2–3 | high |
| ESAI chain | A6 TX0/TX1 → A5 RX0/RX1 → A4 → A3 RX0/RX1 | only an ESAI→ESAI hop keeps a buffer word at the same offset, and A3 sends ESAI words `$0–$3` to the DACs; tested (below) | high (chain), medium (order of A5, A4) |
| DACs | A3 ESAI TX0/TX1 (2 × 32-bit slots) | A3's 2-slot master transmitter | high |
| ESAI_1 ring | A6 TX2/TX3 → A5 RX0/RX1 → A4 → A3 → back to A6 | same pass-through argument; A3's ESAI_1 transmitter and A6's ESAI_1 receiver have no other partner; a bus between two slots needs the A3 → A6 line (tested below) | high (ring), medium (order) |
| Frame clock | A6 ESAI SCKT/FST → all 8-slot sides | only A6 drives them | medium-high |

**The chain order** follows the OS's DSP numbering (A6, A5, A4, A3 = OS DSP 0–3). With the expansion board the OS
numbers the 8 DSPs A6, A10, A9, A8, A7, A5, A4, A3 (§2.6): the expansion DSPs come between A6 and A5, which is what
a chain that runs through the expansion connector between A6 and A5 would give (inferred). Whether A5 or A4 comes
first does not change what reaches the DACs (both pass through), only the latency between them.

### 3.7.4 The emulation (`emu/dspbridge`, `tools/firmware/g2hostemu.py`)

* **Links.** `g2dsp_link` connects one DSP's ESAI or ESAI_1 transmitter pair (TX0/TX1 or TX2/TX3) to another's
  receivers RX0/RX1, frame by frame, through a small queue. The library's ESAI hands frames over whole (at the end of
  the last transmit slot, at the start of the first receive slot), as Gearmulator's Nord Lead 2x connects its two
  DSPs (`source/claudia/n2x`, GPL-3; the approach is adapted, no code copied).
  * A receiver blocks until its frame is there; a sender blocks when the receiver is `--link-queue` (8) frames
    behind. Before both sides run (boot), a receiver gets silence and a sender's frames are dropped.
  * The ring needs a head start: `--ring-prefill` (4) empty frames on A3 → A6 when that line starts.
  * Each hop costs one frame more than on the board (frames are handed over whole), and the ring has the prefill on
    top. The frame offset between two DSPs is fixed once their link runs, but set by the boot race, not by the board.
* **Clock.** Each DSP's ESAI clock ticks once per slot, 192 per tick; the converter sides get divider 3 (2 slots per
  frame). The clock counts **instructions**, not cycles: the library's interpreter does not count cycles in a build
  that has the JIT (`DSP::execOp`, `if constexpr(!g_useJIT)`), so a clock on cycles stops after a few frames. The
  frame program thus gets 1536 instructions per frame, a little more room than the chip's 1536 cycles.
* **Idle skipping.** Stage 1's background loop (P:`$222–$22A`, spinning while X:`$43` ≤ 3) reads only; when a DSP
  sits in it with no interrupt pending and HF0 clear, its clock moves to the next slot instead of running the loop
  (`g2dsp_idle_loop`). It changes no state, and the DSPs then execute only the frame and control-rate code.
* **Converters.** A3's ESAI transmitter is recorded as the G2's output (`g2dsp_sink_record`); A6's ADC receiver
  gets silence. Options: `--chain`, `--no-links`, `--no-idle-skip`, `--throttle`, `--seconds`, `--settle`,
  `--patch-to SLOT:FILE`.
* **Bench.** `tools/firmware/g2dspreplay.py` replays recorded host-port streams into the four DSPs without the
  ColdFire OS (seconds instead of minutes), for work on the links.

### 3.7.5 Results [emulated]

All in the full machine: boot, USB sync, upload with our `proto::Client`, then the DACs recorded: 0.5 s settle
after the upload, then 1 s. The output level ramps up smoothly for about 1 s after an upload (seen in every run),
so the first half of each recording still rises slightly. The test patches are variants of the 1 kHz reference
patch made with `tools/pch2/pch2dump.py` (in `original/firmware/esai-links/test-patches/`, with the scripts that
build them); each run's recording, streams and memories are in a directory next to them.

| Test | Where the OS put it | DAC outputs |
|---|---|---|
| 1 kHz reference, 4-Out on | A6 | out 1–4: 1000.344 Hz, peak 0.0084 = 0.25 (OscA) × module level × 0.068 × 8; H3 −105.9 dB, H5 −82.3 dB, H2 −125 dB (the ramp). Last 0.5 s alone: 1000.35 Hz, H3 −107.9, H5 −82.2, H2/H4 −140 dB, as offline (§3.6.6). `original/firmware/esai-links/full-1khz-final/dac.wav` |
| Same patch in slots A–D at 1 kHz, 2 kHz, 3 kHz, 250 Hz | all four on A6 | all four tones, within 0.9 dB |
| Slots A–D: OscA → 14 × FltNord (open) → 2-Out (Out 1/2), one DSP each | A: A3, B: A4, C: A5, D: A6 | out 1/2: 1000.0, 2001.0, 2997.6 and 250.1 Hz within 1.3 dB (the 14 filters roll off a little); out 3/4 silent |
| Bus and FX: slot A OscA 1 kHz → 2-Out (Bus 1/2); slot B 2-In (Bus 1/2) → 2-Out (Out 1/2); slot C VA OscA 3 kHz → 2-Out (FX 1/2), FX area Fx-In → 2-Out (Out 3/4) | A and B: A6; C (VA and FX): A5 | out 1/2: 1000.34 Hz: the bus left A6 at `$18` and came back to A6 at `$1C` **around the whole ring**; out 3/4: 2997.6 Hz via the FX area |
| Same, each part made heavy (20 × FltNord) | A: A5, B: A3, C VA: A6, C FX: A4 | out 1/2: 1000.34 Hz (bus A5 → … → A3 → A6 → … → A3); out 3/4: 2997.6 Hz (FX A6 → A5 → A4) |

So signals made on every DSP cross the chain to A3's DACs, Out 1/2 and Out 3/4 land on the two DAC lines, the FX
sends travel down the ESAI_1 chain, a bus goes once around the ESAI_1 ring through A3 → A6, and a frame program fits:
the heavy tests executed 834–1119 instructions per frame per DSP (of 1536).

### 3.7.6 Speed [emulated, measured]

Apple M1 (4 performance + 4 efficiency cores). **Caveat:** another emulator session (the module catalog) used about
4.5 cores during all these measurements, so the coupled DSP threads were often descheduled.

| Run | Emulated s per wall s | Notes |
|---|---|---|
| Replay bench, 1 kHz patch, interpreter, no idle skip | 0.59 | 87 M instructions/s per DSP thread, 1536 per frame each |
| Replay bench, idle skip | 0.56–0.62 | 110–223 instructions per frame executed; threads wait on each other 35–60% of the time |
| Replay bench, idle skip, no links | 1.9 | the DSPs alone, uncoupled |
| Full machine, 1 kHz patch, no idle skip | 0.28 | host (Python + Unicorn) thread 100% busy; each DSP thread 41 M instr/s |
| Full machine, 1 kHz patch, idle skip (final run) | 0.38 | host thread 88% busy; DSP threads 30–45% busy, 54–72% waiting on each other |
| Full machine, four heavy slots | 0.16 | 834–1000 instructions per frame; host thread 75%, DSP threads ~55% busy |
| Full machine, heavy bus/FX test | 0.07 | about 1,100 instructions per frame on each DSP; host thread 89% busy, DSP threads 30% busy, 68% waiting |

What dominates:
* **The interpreter**: about 87 M DSP instructions/s per thread when not starved. Real time needs 1536 × 96,000 =
  147 M/s per DSP without idle skipping; with it, only the frame code (100–1,000 instructions per frame here).
* **Per-frame overhead** that idle skipping does not remove: 8 slot ticks per frame, each running the library's
  peripheral `exec` (ESAI clock, DMA transfers per slot, HDI08, timers). The DMA's `execTransfer` showed as much
  time in a profile as the interpreter loop.
* **Coupling**: the four threads hand frames to each other every frame; when one is descheduled the others wait. On a
  loaded machine this costs more than the DSP code.
* **The host**: the Python/Unicorn thread is busy all the time (it is the OS's own speed, §3.6.1).
* **The JIT** (`--jit`) is not usable yet: on the replay bench the DSPs never answered the HF0 handshake and stayed
  silent (not investigated further).

### 3.7.7 What is not done

* The A5/A4 order is inferred from the OS's numbering. The OS seems to place an FX area downstream of its voice
  area (A6 → A4 above) and reads buses a ring turn late, so the tests pass either way. A test: compare the
  inter-slot latency with a real G2 (24 samples reported, §1.1).
* Latency is not the board's: +1 frame per hop, the ring prefill, and a boot-dependent offset between DSPs. On the
  board a buffer spends 2 frames in each DSP (received, processed, sent), so a bus (one ring turn plus the hops to
  its reader) takes roughly 8–16 frames (inferred); the reported 24 samples may be this plus the converters.
* The clock counts instructions, not cycles: a patch the OS packs close to a DSP's limit may fit where the chip
  would overrun, or the other way round.
* The ADCs feed silence; there is no test signal input yet.
* The expansion board's four DSPs (A7–A10) are stubs; with them the chain would run A6, A10…A7, A5, A4, A3.
* Speed: about 0.15–0.6 × real time here. Next: run the four DSPs' links without per-frame thread hand-offs (larger
  batches, or all four on one thread with the JIT), port the host to C++ (§3.6.7), fix the JIT.

## 3.8 Control-rate timing: the emulator's fault and its fix (2026-10-10)

**Symptom** (`re/notes/native-engine.md` §3.3, measured as a black box). In the full machine, control-rate code ran
about 2 × too often:
* an LFO displayed as 10.30 Hz measured 19–23 Hz (it varied between runs);
* envelope stages took 0.45 × their displayed time;
* the Env output changed in bursts of about 4 consecutive samples, then held for about 4;
* audio-rate code (oscillator pitch) was exact.

`--no-idle-skip` did not change it.

**Cause: lost condition codes in the dsp56300 library, not a clock rate.** Details below. The DSP clock, the ESAI
frame clock, DSP timer 0 and the ColdFire timer ticks play no part.

### 3.8.1 How control-rate code is dispatched [C]

Seen in the stage-1 programs and the live dumps of patched DSPs.

* **The frame interrupt** (ESAI_1 receive-last-slot, P:`$76`: `jsr DOR0`) runs the patch's audio-rate code. It also
  increments the frame counter X:`$43`.
* **The background loop** is stage 1's `DO FOREVER` at P:`$220`. Its body starts at P:`$222`:
  * `bsset #HF0,x:HSR,…`: the HF0 handshake;
  * r3/r4 ← DOR1/DCO1;
  * `clr b x:(r1),a`, `cmp #3,a`, `ble $222`: spin while X:`$43` ≤ 3.
* **When the counter passes 3,** the loop does three things:
  * it sets X:`$43` to 0;
  * it adds X:`$40` to the 48-bit L:`$42`. This counts control ticks: X:`$40` = `$15D8`/`$15D9` ≈ 2²⁷/24000 with
    the empty patch, and `$5761` after a patch upload;
  * it falls through into the patch's **control-rate code at P:`$235`**.
* **The OS ends the loop body after that code.** It sets the loop's last address with host command `$98` (LA).
  Examples:
  * EnvADSR patch: control code P:`$235–$25A`, LA = `$25A`, frame code from DOR0 = `$25B`;
  * empty patch: LA = `$235`, DOR0 = `$236`.

So the control-rate code runs in the background, once every 4 frames, and the frame interrupts preempt it. That
gives a 24 kHz control rate at 96 kHz. No DSP timer and no host tick is involved.

### 3.8.2 What went wrong [emulated]

**Trace.** A slot-tick trace of PC and X:`$43` (`g2dsp_trace`, replay bench, 1 kHz patch) showed the counter at
0, 1, 2 (or 0, 1 on A6) when the background loop passed its `ble`.

**Measured from L:`$42`** (control ticks per frame, which should be 0.25 everywhere):

| DSP | Ticks per frame |
|---|---|
| A3 | 0.339 |
| A4 | 0.333 |
| A5 | 0.333 |
| A6 | 0.500 |

**Per-instruction trace** (`g2dsp_trace_fine`). Every early pass had an interrupt between `cmp` (P:`$228`) and
`ble` (P:`$229`). There are three interrupts per frame on every DSP: the frame interrupt and the DMA 4/5 done
interrupts (P:`$20/$22`, `jsr`).

**The mechanism, in the library at the pinned commit:**
1. The interpreter computes the CCR bits E, U and N lazily (`ccrCache`).
2. `DSP::execInterrupt` hands every interrupt vector to the JIT (`if(g_useJIT)`), also when the DSP runs on the
   interpreter. `g_useJIT` is `g_jitSupported`, so it is true on arm64 and x64.
3. The JIT's `jsr` stacks SR as stored, without the pending N of the `cmp`.
4. The `rti` restores that stale SR, and `ble` (Z | N ⊕ V) falls through.

**Reproduced in isolation.**
* A bench script (not kept) ran stage 1's loop on one DSP with X:`$43` held at 0, under a stream of host commands
  whose vector is `jsr $300; rti`. It counted 6,000–100,000 wrong passes per second.
* `g2dspccr` (§3.8.3) is the same check without firmware.

**Why the rate was about 2 × and varied.** Where an interrupt lands in the loop depends on the emulated timing (idle
skipping, link waits). So the loop passed every 2 or 3 frames instead of every 4, which gives 1.3–2 × on average.
Bursts of 4 Env changes appear when passes cluster.

**Effects beyond the loop.** The same fault hits any conditional in control-rate code (`Tcc`, `Bcc`, `IFcc`) that
follows an ALU op when an interrupt falls between the two. That corrupts module state now and then, which is a
likely cause of the gate dropouts in big patches (`native-engine.md` §3.3).

Audio-rate code runs inside the frame interrupt. The library runs no peripherals during a long interrupt, so no
other interrupt can land there, and pitch was never affected.

### 3.8.3 The fix

* **`emu/CMakeLists.txt` edits the fetched `dsp56kEmu/dsp.cpp` after population.**
  * It inserts `updateDirtyCCR();` before the JIT runs the vector in `DSP::execInterrupt`. This resolves the
    pending E/U/N into SR before anything stacks it.
  * The edit is idempotent and marked `G2FRESH`. Configure fails if the anchor line is missing (a new pin).
  * In a pure JIT run nothing is pending, so it changes nothing there.
* **`emu/dsptest/g2dspccr.cpp`** (target `g2dspccr`) checks the fix without firmware: a `cmp`/`ble` loop with an
  interrupt injected at each position.
  * Without the fix: 100 wrong branches in 400 loops (every interrupt that lands after the `cmp`).
  * With the fix: 0.
* **`g2dspreplay.py`** now prints the control ticks per frame from L:`$42`/X:`$40`. The bench gives **0.2500 on all
  four DSPs**, with and without idle skipping.
* **Debugging aids in the bridge:**
  * `g2dsp_trace`: per slot tick;
  * `g2dsp_trace_fine`: per instruction. It is blind inside long interrupts, where the library runs no peripherals.

### 3.8.4 Before and after [emulated, full machine, same patches, one session each]

Patches made with `g2mkpatch`:
* LfoA Rate 64, Range Hi (displayed 10.30 Hz) → 2-Out;
* OscA (KBT off, Coarse 84) → EnvADSR (Attack 60, Decay 60, Sustain 30, Release 55: displayed 748 ms, 748 ms,
  496 ms) → 2-Out, note 0.1–3.0 s;
* the 1 kHz reference (`1khz_on.pch2`).

Decay and release are fitted as exponentials and given as the time to 1 %.

| | Before | After | Displayed |
|---|---|---|---|
| LFO | 19.57 Hz (× 1.900; 19.06 Hz in another run) | **10.3009 Hz** (−0.002 %) | 10.3011 Hz |
| Attack 0 → 1 | 341 ms (× 0.456) | **749.9 ms** (+0.25 %) | 748 ms |
| Decay to 1 % | 339 ms (× 0.453) | **744.8 ms** (−0.4 %) | 748 ms |
| Release to 1 % | 226 ms (× 0.456) | **493.1 ms** (−0.6 %) | 496 ms |
| Env stepping during the attack | runs of 1–5 consecutive changes (mostly 4), 0.53 changes per sample | one change per 4 samples (0.24 per sample) | 0.25 |
| LFO harmonics | H2 −48 to −52 dB | H2 −84 dB | |
| 1 kHz patch | 1000.344 Hz | 1000.344 Hz, H3 −106 dB, H5 −82 dB (unchanged) | |
| Speed (emulated s per wall s) | 1.17 / 1.07 / 0.69 | 2.08 / 2.01 / 1.99 | |

**Speed caveat.** The two sessions ran under different background load (other builds on the machine), so the speed
difference is not attributable to the fix. The fix itself adds one call per interrupt.

**Other results:**
* **Frame repeats.** A sine predictor on the DAC found none, before or after:
  * in 1 s of the 1 kHz patch;
  * in the VCA output of the envelope job, around its note on/off;
  * after the fix, in 3.2 s of the 1 kHz patch with 30 note events.

  So the ~0.1 % repeats of `native-engine.md` §3.3 were not reproduced. Possibly they were the same fault seen
  through control-rate outputs; this is not confirmed.
* **The 26-module patch** (`native-engine.md` §4), note held 0.1–3.0 s, 50 ms RMS of Out 1:
  * an older recording jumps between levels (161 → 228 → 110 → 223 → 111, ×10⁻⁵), the gate dropouts;
  * after the fix, one run stays between 194 and 262 with no dropout.
* **The output ramp after an upload** (measured from the client going idle) reaches 50 % at 0.4 s, 95 % at 1.2 s
  and 99 % at 1.7 s. It is similar to before, so it is not DSP control-rate timing. It is possibly OS-timed (§3.8.5).

### 3.8.5 Still not right: OS time against DSP time [emulated, measured]

The ColdFire's timer 1 fires once per 20,000-instruction slice. The DSPs run freely, paced only by their links.

**Measured** (slices against A6 frames):
* **111** ticks per emulated DSP second over the first fixed session;
* **51–62** during boot and sync;
* **180–217** while recording;
* 105–357 in the unfixed session.

**Hardware estimate:** the OS programs TRR 50 with a ÷128 prescaler, 6,400 bus clocks per tick. At an assumed
54 MHz bus that is about **8,400 ticks/s**. The G2's ColdFire bus clock is not known: check this.

So relative to the audio, everything the OS times runs roughly 40–170 × slow, and the factor changes with load:
* the 64-step knob slews (about 130 ticks);
* note-on latency (33–93 ms measured);
* LED/meter polling;
* the post-upload ramp, if it is OS-driven.

DSP-side timing (rates, envelopes, pitch) is unaffected. A fix would gate the DSP frames by host ticks (about 11
frames per tick at 8.4 kHz). That makes the full machine about 30 × slower, so it is not done.

**Resolved in §3.9:** the C++ emulator drives the ColdFire's timers from the DSPs' frame clock (8272 ticks per
emulated second); the slews, the note latency and the post-upload ramp now take their real times (§3.9.2).

**DSP timer 0** (stage 1, A3) is the one the OS calibrates through the I2C ADC (§3.6.3).
* The frame and control code of the patches looked at here (empty, 1 kHz, EnvADSR) does not read it, and no timer
  vector is set.
* So it does not set the audio or control rate.
* Its own rate in the emulator was not checked.

### 3.8.6 Earlier measurements to revisit
* **`re/notes/native-engine.md`:**
  * §2.3 and §3.3: the 2.213 × time rescale is no longer needed, and the emulator is now a timing reference. The
    rescaled numbers were right, because the error was a pure speed-up of the control ticks.
  * The 0.5–3 ms stage errors near note events, the frame repeats and the gate dropouts need re-measuring.
* **`re/notes/dsp-module-catalog.md`** is largely unaffected:
  * parameter words and slews come from the OS's host-port writes;
  * responses and costs come from `g2dspframe`, which runs only the frame program, offline, without interrupts.
  * Two things to recheck:
    * module X/Y state in the `dsp3_live_*` dumps, which control-rate code may have corrupted (the initial
      values and OS-written words are not affected);
    * any statement that relies on running control-rate code in the full machine.
  * Its OS-timed figures (slew of about 130 ticks) are in OS ticks, so they hold, but they cannot be converted to
    DSP time in the emulator (§3.8.5).
* **§3.7.5 results** (pitch, routing) are audio-rate and stand.

## 3.9 Real-time C++ emulator (2026-10-10)

**Result.** `emu/g2emu` (library `g2emu`, built with `-DG2_BUILD_EMU=ON`) runs the user's own OS 1.62 and the four
DSPs in C++, with no Python and no Unicorn:
* it boots in 1.6 emulated seconds, our protocol client connects over the emulated USB chip and syncs in 74 ms;
* an uploaded patch sounds (the 1 kHz test patch: **1000.328 Hz** on all four DACs, peak 0.00838, as in §3.7.5);
* notes play through the OS's MIDI IN (UART0) and through the protocol's PlayNote;
* OS time and audio time come from one clock: the OS's timer ticks 8272 times per emulated second;
* real time on an M1 for typical and heavy patches: 2.7-2.8 × and 1.8-2.4 × with three threads (the ColdFire and
  two DSP threads, the default), §3.9.7.

Files:
* `emu/g2emu/include/g2emu/`: `firmware.hpp` (the updater's resources, the OS image, LZO1X: `g2os.py` in C++),
  `machine.hpp` (the machine), `transport.hpp` (a `proto::Transport` over the emulated USB, and
  `emulatedG2Link()`, a `LocalLink` to it), `runner.hpp` (the machine in real time for a sound engine).
* `emu/g2emu/src/`: `hw.*` (SIM, timers, UARTs, I2C, flash, ISP1181), `dsp.*` (a DSP56367 stepped on the JIT,
  host port, serial links), `machine.cpp` (memory map, scheduling, USB host), `runner.cpp`, `thread.hpp`.
* `emu/g2emu/tools/g2emurun.cpp`: boot, connect, upload (`--patch`, `--kbd`), notes (`--note` protocol, `--midi`
  UART), record (`--wav`), speed; `--realtime S` plays it at the wall clock through `Runner` and a `LocalLink`.
* `tests/test_g2emu.cpp` (`g2emutests`): firmware-free checks of every part, and one `[firmware]` case that boots the
  user's OS when it is there (`G2_FIRMWARE`, else `original/firmware`) and plays a MIDI note at 440 Hz.
* `tools/firmware/g2emucompare.py`: compares two DAC recordings (level, pitch, onset, band spectrum).

### 3.9.1 The ColdFire core

**Choice: Gearmulator's ColdFire core** (`source/cpu/coldfire/cfCpu.{h,cpp}`, GPL-3.0, "ColdFire V2 core as in the
MCF5206e: ISA_A, hardware divide, MAC unit"), fetched with `FetchContent` at commit `fd3777fb63ec` file by file with
SHA-256 pins (the repository's archive is 160 MB; we need two files).
* **It runs OS 1.62 unchanged** [emulated]. The OS is plain ISA_A (§3.6.1), which is what the core implements. In the
  runs checked (boot, sync, uploads of the test and corpus patches, notes): no unimplemented opcode, no error
  exception (vectors 2-15, counted when the core fetches them: 0), no unmapped access.
* **V4 differences that mattered: none.** MOVEC to CACR, ACR0-3, RAMBAR0/1 and MBAR goes to the bus, which ignores it
  (no caches; SRAM and MBAR are where the boot loader puts them). The exception frame (format 4, vector, SR, PC) is the
  same on V2 and V4. None of Unicorn's four workarounds (§3.6.1) is needed: the core takes interrupts and RTE itself,
  between instructions, so condition codes cannot get lost.
* **One patch** (`emu/CMakeLists.txt`, idempotent, fails on a moved anchor): a window of plain RAM (the SDRAM) that the
  core fetches from, reads and writes directly instead of through the virtual `Bus` calls. Everything else (MBAR, chip
  selects, exception vectors) still goes through the bus.
* **Speed:** about 90 M instructions/s on a tight loop and 65-75 M/s on the OS (one M1 performance core; about 10 ns
  an instruction: the table dispatch through member-function pointers and an indirect call per instruction).
* **Alternatives, not needed:** Musashi is a 68000-family core (ColdFire has different addressing restrictions, MAC,
  MOVEC registers; it would need the same glue plus fixes); an own ISA_A interpreter would mainly be faster (a 2-3x
  faster dispatch is plausible), at the cost of writing and validating some 1,600 lines. Unicorn is not used:
  QEMU-derived, probably GPL-2-only.

### 3.9.2 Timing model and its confidence

**One master clock: the DSPs' frames.** A frame is one 96 kHz sample, 1536 DSP clocks [C, §3.7.1]. Per frame:
* each DSP runs 1536 instructions (its clock counts instructions, §3.7.4);
* the ColdFire runs **1687.5 cycles** (162 MHz) of the core's own timing;
* the timers and the UARTs see **562.5 bus clocks** (54 MHz), derived from the master time whatever speed the core is
  given (`Options::cfHz`).

| Quantity | Value | Basis | Confidence |
|---|---|---|---|
| Bus clock | **54.000 MHz** | the OS programs UART0 for MIDI with UBG = 54 [C] (0x30038004 calls the UART set-up 0x300583E2 with divider $36); MIDI is 31250 baud = f / (32 × UBG), so f = 54,000,000 exactly. A 5 % different clock would break MIDI | high |
| UART0 is MIDI | vector $42, level 4, handler 0x3000286C parses status bytes $80-$E0, F1, F2, F3 [C] | [C] | high |
| Core clock | 162 MHz = 3 × bus | the teardown's "MCF5407 @ 162MHz" (§1.1) and the usual 3:1 core:bus ratio | medium |
| OS tick (timer 1) | TMR $7F3B (bus clock / 128, restart, interrupt), TRR 50 [C] (0x30038028 → 0x300582F0): 51 × 128 = 6528 bus clocks, **8272 Hz** (8437.5 Hz if the period is TRR rather than TRR + 1 counts) | [C] register values; the TRR + 1 period is from the timer's description (count reset after reaching TRR), not checked on hardware | high (values), medium (± 2 %) |
| Intent | the OS's delay routine 0x30002094 waits n × 8 ticks for n ms [C]: the designers meant "8 ticks per ms"; 8272 Hz makes its milliseconds 0.97 ms | [C] | high |
| Timer 0 | free-running counter of the bus clock (TMR $2B), read for short delays [C] | [C] | high |
| Core speed | the core charges the MCF5206e (V2) cycle tables. A real MCF5407 (V4) runs about 1.4-1.5 Dhrystone MIPS per MHz against about 0.9-1.0 for V2, so at 162 MHz the emulated CPU executes roughly 2/3 of the real one's instructions per second | Freescale's published figures, from memory: **check** | low-medium |

What depends on the core's speed and what does not:
* **Not:** everything the OS times by its ticks (knob slews of about 130 ticks = 16 ms, the 128-slot scheduler, LED
  and meter timing, timeouts), MIDI timing, audio. They follow the bus clock.
* **Does:** how much background work fits between ticks: the OS's main loop polls the DSPs for meters and LEDs as
  fast as it can (about 50,000-120,000 read-back host commands per emulated second), compiles uploaded patches, and
  answers USB. Interrupt level 1 (the tick) takes 7 % of the core's instructions at 162 MHz, 13 % at 81 MHz, 24 % at
  40 MHz [emulated], so the OS keeps up even at a quarter of the speed.

**Measured** [emulated, `g2emurun`]:
* timer ticks: 8272.0 per emulated second;
* note-on latency: MIDI 1.7 ms (3 bytes at 31250 baud take 0.96 ms), PlayNote over USB 2-3 ms (the Python emulator:
  33-108 ms, its OS time ran 40-170 × slow, §3.8.5);
* after an upload, the output reaches its level within 50 ms (the Python emulator: 0.4 s to 50 %, about 2.5 s to
  settle: **that ramp was the slow OS time**, as §3.8.5 suspected; `native-engine.md` §3.1's settle time is not the
  G2's);
* sync of our client: 74 ms; uploading a performance: 28 ms (162 MHz), 40 / 57 / 83 ms at 120 / 81 / 54 MHz.

§3.8.5 is resolved: the ColdFire and the DSPs now share one time base.

### 3.9.3 The DSPs on the JIT

dsp56300's JIT now runs the G2's DSP code. What it took:
* **`JitConfig::dynamicFastInterrupts`.** Stage 1 keeps subroutines inside the interrupt vector area (P:$B8, $D6, $F4;
  called with `jsr` and `bsset`) [C]. Without this option the JIT compiles every block there as a two-word fast
  interrupt that never advances the PC: the DSP sat on `brset #TFS,x:SAISR,*` at P:$B9 forever. **This was the "JIT never
  left the TFS poll" of §3.6.4**; the silent DSPs of §3.7.6's `--jit` probably had the same cause. (The library's own
  debug build asserts on it.)
* `JitTrampoline::exec(dsp, n)` takes a multiple of 8: n = 1 means 2³² rounds.
* `trackVolatilePMemory = false` (the OS loads whole routines into P on every upload; tracking them would slow the
  frame code down for good), `doLoopExitOnPendingInterrupt = true` (a frame interrupt can preempt control-rate loops,
  as on the chip).
* The JIT compiles on the thread that runs a DSP, and a block it has just compiled runs nested inside the compiler's
  call: the stack of the running thread must be big. Machine's DSP threads have 64 MB (address space); in
  single-thread mode the caller's thread needs 8 MB or more (a 512 KB `std::thread` overflowed during an upload).

**Idle skipping, corrected.** The skip (§3.7.4) moved a DSP's clock to one slot after the *instruction count* of the
last slot tick. After a long frame program the serial clock is behind and serves its slots late; each late tick then
also skipped a whole slot, so the clock never caught up and heavy patches lost about a quarter of their frames (link
underruns, a 1.5 s recording came out 1.15 s long). It now skips to one slot after the clock's own last slot (read
from `EsxiClock::getLastClock`). The Python bridge (`emu/dspbridge`) keeps the old logic; it did not show the loss
there, but it is the same code.

### 3.9.4 Scheduling

* **Single thread** (`threads = 0`, deterministic): per quantum (1 frame) the ColdFire runs to the end of the
  quantum, then the DSPs in chain order (A6, A5, A4, A3). Whenever the ColdFire touches a DSP's host port, that DSP and
  the ones upstream of it are first run up to the ColdFire's time, so host commands are taken and answered within the
  emulated microseconds the chip would need. **Poll skipping:** when the ColdFire reads the same status register of
  one DSP from the same instruction and gets the same value (a wait for a host command to be taken, or for data), the
  DSPs run ahead until it changes (at most a frame) and the ColdFire's clock moves on by as much: 18-38 % of the
  ColdFire's time was such loops.
* **Threads** (`threads = N`; `-1`: two where the computer has 4 performance cores, else one, §3.9.7): the DSPs
  run on N threads of their own, each a stretch of the chain, no further than the stretch upstream of it (and the
  first no further than the ring's prefill past the last); the ColdFire runs at most 2 frames ahead of the slowest
  DSP thread (`skew`) and the DSPs at most 8 frames ahead of the ColdFire (`dspLead`, 2 until 2026-10-10). The
  asymmetry is deliberate: while the ColdFire is ahead it waits for the DSPs' answers in emulated time (its OS gets
  less done), while the DSPs are ahead a host command reaches its DSP up to 83 us late, as jitter on a parameter or
  note; the DSPs' lead absorbs the two sides' uneven pace. A repeated poll moves the ColdFire's clock on by 128
  cycles. A thread that waits spins for 50 us, then sleeps until the other side rings (`Doorbell`, thread.hpp).
  Not deterministic (when a host access meets a DSP depends on the threads).
* **The host port, in order and paced as on the chip** (both modes). Words, host commands and host-flag changes go
  through one queue per DSP and reach the DSP on its own thread in the order the ColdFire wrote them; whatever follows
  a host command waits until the DSP has taken it, whatever follows a flag change until the DSP has read its status
  register (the same rules as Gearmulator's `HDI08Queue`). On the chip a host command is taken within a few clocks,
  before the host writes its next word, and the OS relies on it: it sends `$AE` (echo, which reads the host port)
  without writing a word first. Delivered at once from the ColdFire's thread, such a command could take the word
  meant for the next command. The ColdFire reads only the library's thread-safe parts (the transmit queue, HF2/HF3).
* **Links:** frame queues as in §3.7.4, non-blocking, with 2 frames of prefill per chain hop and **16 on the ring**
  (A3 → A6). The prefills add to the latency of an inter-slot bus (one ring turn, §3.7.2): 16 frames for the ring
  plus 2 per hop, on top of the frames the DSPs hold themselves (the board's own figure is not known; the community
  measured 24 samples for an inter-slot connection, §1.1). Underruns happen only while the OS reprograms the DSPs
  (boot, uploads).
* **USB:** the host side of §3.6.6 in C++ (SETUPs 5 ms after the bus reset, the client 15 ms later, bulk-IN polled
  only while an announcement waits). The cable goes in once the OS has run 2 s (`Options::usbAfter`): a host that
  connects while the OS boots loses its first request (the OS initialises the ISP1181 again at the end of its boot)
  and the client retries only after 10 s. The ISP1181's registers now read back what was written (the OS
  read-modify-writes the mode register).
* **MIDI IN** is UART0: bytes reach its receiver FIFO at 31250 baud, with RxRDY interrupts at level 4, vector $42 [C].
  MIDI OUT (UART0 transmitter) is captured; OS 1.62 sent nothing in these tests.
* **Sample-accurate MIDI:** `Machine::midiInAt(bytes, frame)` starts a message on the MIDI line at a given machine
  frame; `Runner::midiInAt(bytes, offset)` (lock-free, from the audio thread before its `read()`) puts it `offset`
  frames into the next block plus a fixed `latencyFrames()` (the buffer plus a chunk). Measured in real time
  (`g2emurun --realtime --timed-midi`, 8 notes at odd times, 20 ms buffer): note-on to sound **23.39 ms ± 0.06 ms**,
  against 22.8-31.7 ms when the bytes go in at once (`midiIn`).

### 3.9.5 Results [emulated, full machine]

| Test | Result |
|---|---|
| 1 kHz reference (`1khz_on.pch2`) | 1000.328 Hz on outputs 1-4, peak 0.008380 (Python: 1000.344 Hz, 0.0084) |
| Keyboard → OscA → EnvADSR → 2-Out, MIDI note 69 | 440.0 Hz, onset 1.7 ms after the bytes, release at note-off |
| The user's `Drone.pch2` | sounds; dominant 110.16 Hz in both emulators |
| `Comp Keys1`, `VintageOrgan3`, `BigDualVCFSynth3` (Clavia bank 2), chords | all play; recordings complete (no lost frames) |

Against the Python reference (`g2blackbox.py`, the same jobs, `g2emucompare.py`; the machines start their
oscillators and LFOs at different phases, so the comparison is phase-blind):

| Job | Level | Pitch | Onset | Band spectrum difference |
|---|---|---|---|---|
| drone | +1.8 dB | 110.16 / 110.16 Hz | - | 4.3 dB rms over 9 bands |
| kbd_sine (PlayNote 69) | +0.1 dB | 439.99 / 439.99 Hz | 108 ms / 2.7 ms after the request | 2.1 dB |
| organ (2 notes) | -1.0 / +2.8 dB | same partials | 46 / 3 ms | 3.0-3.6 dB |
| bigsynth (1 note) | -1.3 / -0.6 dB | 131.4 / 130.5 Hz (note 48, detuned oscillators) | 34 / 3 ms | 2.3-2.4 dB |
| compkeys (3 notes by PlayNote) | +1.3-1.6 dB | differs | 29 / 3 ms | 30 dB: different notes sound (below) |

**PlayNote is the editor's monophonic on-screen keyboard** (`usb-protocol.md` §13). Three overlapping PlayNotes on
`Comp Keys1` played a chord but left a note hanging after the three note-offs in the C++ emulator (they released in
the Python run, where each message took about 100 ms); two overlapping notes, or three in a row, released. Through
MIDI the same chord releases. So a sound engine sends its notes as MIDI (UART0), not as PlayNote.

### 3.9.6 Open issues and risks

* **More than one DSP thread is not proven safe.** With `threads = 2` a DSP sometimes jumped into P memory where
  there is no code. The zeros there are NOPs; the JIT links a run of them into one chain of blocks, so the DSP slid to
  the end of P memory within one dispatch and the JIT then recursed without end compiling past it (`JitBlockChain::
  create` runs the block it has just compiled; caught at P:$80000 with LA = 0 in a RelWithDebInfo build): a crash,
  in about half the patch uploads at first. Three changes since: poll skipping in the threaded mode, the ordered host
  port (above), and `bra *` written over the P memory the OS never uses (from $4000), so a DSP that strays stops on
  the first word (`Stats::dspsWild` counts them; the machine then needs a restart). Since then: 0 failures in 38 runs
  with 2-4 DSP threads; but without poll skipping a DSP still strayed in about 1 of 8 runs (stopped by the trap, the OS
  then lost USB contact). The cause is not found; it needs the DSPs on more than one thread and the ColdFire hammering
  the host ports. `threads = 1` showed nothing in any test (also without poll skipping); 2-4 stay experimental.
  Ruled out since: stale DSP answers (none: no answer was ever written over an unread one, in any mode). Tried:
  `Options::causalReads` makes a status read wait (wall time) until that DSP has reached the ColdFire's time, in case
  an OS wait counted in loop iterations expired before a lagging DSP thread got there; with it, 0 failures in 22 runs
  of the failing configuration, but also 0 in 20 without it (the ordered host port had made the failure rare), and it
  costs 15-25 % of speed: off by default.
  **2026-10-10, second pass: `threads = 2` is the default since** (§3.9.7). Evidence: 100 runs of three uploads each
  (a performance and two patches from the factory banks, different every run) with two DSP threads, plus the 100-odd
  benchmark and real-time runs of §3.9.7 (each with a performance upload): no DSP strayed (`dspsWild` 0), no run lost
  its sync. The cause of the old failure is still not known; if it comes back, the trap stops the DSP and the plugin
  has to restart the machine. 3 and 4 threads stay experimental (and are no faster).
* The OS's DSP code runs the undefined opcode `$000040`: harmless (dsp56300 runs it as ILLEGAL; its vector is empty),
  logged by the library when it compiles such a block; g2emu filters that line.
* The core counts V2 cycles (§3.9.2): the emulated CPU is probably slower than the real one; nothing seen depends on it
  beyond background throughput.
* The DSP clock still counts instructions, not cycles (§3.7.7).
* Stubs: the I2C ADC (the boot's calibration takes about 1 s against a constant 0x80), the analogue inputs
  (silence), the expansion board's DSPs. The panel is modelled since (§3.10).
* The flash is erased at power-on unless the caller loads an image (`Machine::flash()`, `g2emurun --flash`); with
  one, the OS does not format it again; the boot still takes about 1.6 s (the calibration).
* Not deterministic with threads; deterministic and slower without.
* macOS hardened runtime: the JIT needs `MAP_JIT` (a host application with the `allow-jit` entitlement); Gearmulator's
  plugins have the same requirement. **Check which DAWs allow it.**

### 3.9.7 Speed [measured, M1, 4 performance + 4 efficiency cores]

**Summary (2026-10-10, second pass).** Two DSP threads (`threads = 2`, the default since, `-1`: §3.9.4) run light
patches at **2.7-2.8 x** real time (before: 1.5-1.6 x) and heavy ones at **1.8-2.4 x** (before: 1.7-2.1 x). In real
time (Runner, a held chord, 20 ms buffer) the plugin's setting had underruns in 9 of 9 patches before (0.01-24 % of
the audio with one DSP thread, the old default), and has none in 7 of 9 now, a few blocks in the others (below);
with three busy processes beside it, 14-44 % of the audio was missing before, 0-0.04 % now. Nothing the machine
computes changed: `threads = 0` gives bit-identical DAC output, instruction counts and host traffic.

**How it is measured.** `tools/firmware/g2emubench.py` runs `g2emurun --model g2x --kbd PATCH` with a 3-note chord
held through MIDI over 3 emulated seconds (best of 2), per patch and DSP thread count, and reports x real time,
CPU per emulated second (all threads) and the *busy* CPU per thread (its waits taken out: `Stats::cfWaitNs`,
`dspThreadWaitNs`); `--realtime S` plays S seconds through Runner + LocalLink at the wall clock and counts the 10 ms
blocks with frames missing, `--load N` keeps N `yes` processes busy meanwhile. Two things matter on macOS:
* **The emulator must run with an application's scheduling policies** (`taskpolicy -a`, which the script uses; a
  DAW has them). A process started by a daemon (here: an agent's shell) runs its user-interactive threads at
  priority 31, like any process (`ps -M`: 31; with `-a`: 46), and then competes with every busy process.
* g2emurun's own thread (the ColdFire in all modes, the DSPs too with `--threads 0`) is set to user-interactive
  QoS; before, it could land on an efficiency core and the speeds varied by 30 %.
The machine was not idle (a system daemon held one core at 100 %, load average about 5); the before/after runs
alternated under the same conditions. Before = c8fbcf6 (with the waits counted), after = this commit.

| Patch | threads 0 before / after | 1 before / after | **2** before / after | busy CPU after, threads 2 (ColdFire; DSP threads) |
|---|---|---|---|---|
| Drone (the user's, light) | 0.72 / 0.91 | 1.51 / 1.66 | 1.50 / **2.67** | 0.37; 0.32, 0.30 |
| 1 kHz test | 0.75 / 0.94 | 1.54 / 1.69 | 1.64 / **2.84** | 0.35; 0.32, 0.30 |
| kbd_sine | 0.76 / 0.95 | 1.60 / 1.70 | 1.61 / **2.84** | 0.35; 0.31, 0.30 |
| organ (VintageOrgan3 test) | 0.72 / 0.78 | 1.02 / 1.05 | 1.97 / **2.24** | 0.22; 0.45, 0.44 |
| compkeys (test) | 0.60 / 0.63 | 0.87 / 0.85 | 1.78 / **2.00** | 0.21; 0.50, 0.47 |
| bigsynth (heavy) | 0.68 / 0.73 | 1.00 / 1.02 | 2.07 / **2.26** | 0.20; 0.44, 0.43 |
| Vangel (factory bank 1, the user's test patch) | 0.72 / 0.78 | 1.18 / 1.18 | 2.03 / **2.44** | 0.23; 0.41, 0.41 |
| Comp Keys1 (factory bank 2) | 0.58 / 0.61 | 0.81 / 0.79 | 1.70 / **1.84** | 0.21; 0.54, 0.50 |
| VintageOrgan3 (factory bank 2) | 0.72 / 0.77 | 1.03 / 1.03 | 1.93 / **2.24** | 0.22; 0.45, 0.44 |

With one DSP thread the four DSPs need 0.6 s per emulated second on a light patch and 0.85-1.26 s on a heavy one:
one thread caps light patches at about 1.7 x and does not keep up with heavy chords. Three or four DSP threads gain
nothing (three put two DSPs on one thread; four make five busy threads on four performance cores: 1.5-1.75 x).

Real time, 20 s each, a 3-note chord held, Runner with its 20 ms buffer, frames missing (blocks of 10 ms with a gap):

| Patch | before, threads 1 (old default) | before, threads 2 | **after, threads 2 (default)** | after, threads 1 |
|---|---|---|---|---|
| no other load: light (Drone, 1 kHz, kbd_sine) | 0.01-1.24 % | 0.10-0.46 % | 0 (one block in one run) | 0 |
| no other load: heavy (6 patches) | 0.13-23.6 % | 0-0.95 % | 0 (3 blocks, bigsynth) | 0-21.9 % |
| 3 x `yes`: light | 42-44 % | 21-24 % | **0** | 0 |
| 3 x `yes`: heavy | 14.5-37.9 % | 19.9-33.8 % | **0** (1 block Vangel, 3 Comp Keys1) | 0-23.9 % |

The remaining gaps are single stalls of 10-20 ms (`Runner::Stats::longestChunkMs`: a 1 ms chunk that took a
scheduler quantum), not a lack of speed; a 30 ms buffer would absorb them, at 10 ms more latency. CPU in real time
(18 s of a run): the light patch 1.3 cores with one or two DSP threads alike, the heavy one 1.55 cores with two (1.9
with one, overloaded). MIDI timing is unchanged: note-on to sound 22.5-23.6 ms, ±0.04 ms within a run
(`--timed-midi`, 8 notes; an occasional note 2.5 ms late in both versions).

What changed, and why it helped:
* **The waits.** A thread that waited spun 64 times and then called `sched_yield` until the other side moved. When
  the cores are taken, every yield hands the core to another process for a scheduler quantum (10 ms): that alone
  made the old version lose 14-44 % of the audio beside three busy processes, light patches included, and it is the
  likely cause of what was heard in Ableton (1.7 s missing in 20 s). Now a waiting thread spins 50 us and then sleeps
  on a condition variable until the other side rings (`Doorbell`, thread.hpp; a ring costs a fence and a load while
  nobody sleeps). Idle machine threads (the Runner's buffer full) cost no CPU.
* **The DSPs' lead.** The ColdFire and the DSP threads were held within 2 frames of each other in both directions,
  so each side waited for the other's slowest moments. The DSPs may now run 8 frames ahead (`dspLead`, §3.9.4);
  the ColdFire's own lead stays at 2 frames, because while it is ahead its OS waits for answers in emulated time
  (a symmetric 8 or 32 frames was as fast but cut the OS's host commands per emulated second by 15 or 30 %).
  Drone, 2 DSP threads: about 1.4 -> 1.5-1.75 x by itself (noisy).
* **The ColdFire core** (Gearmulator's, patched at configure time, `emu/g2emu/coldfire`): 76 -> **132 M
  instructions/s** (`G2EMU_TIMING`, threads 0, "ColdFire alone"). In order of gain: dispatch through a table of plain
  function pointers, one per handler, into which the compiler inlines the handler (the member function pointers
  took 16 bytes and a test per call: +19 %); the address decoding and memory helpers forced inline and a stepping
  loop inside the core (`Cpu::run`) with the bus's wait cycles in a field of the core instead of a virtual call per
  instruction (+25 %); specialised handlers (size and address mode as template arguments) for MOVE, MOVEA, CLR,
  TST, CMP, LEA, JSR, PEA, ADDQ/SUBQ and Bcc (+14 %). A test steps random code through both paths and compares
  every register, flag, cycle count and memory byte after each instruction.
* **Cache lines.** The atomics each thread writes (its time, the horizon, the host queue's two ends) on lines of their
  own; the DSP no longer stores its host queue's tail when nothing was taken. No measurable gain alone.
* **The choice of threads.** With the waits and the lead fixed, two DSP threads were faster than one for every
  patch, light ones included, at the same CPU per emulated second (the work is the same, spread on more cores), and
  were the only setting without underruns under load. A switch at run time from measured DSP load (the plan) would
  have nothing to choose: `threads = -1` takes 2 where there are 4 performance cores (`Machine::autoThreads`), 1
  elsewhere, and the Runner and the plugin use it.

Where the time goes now:
* **ColdFire:** the OS runs 45-50 M instructions per emulated second (its main loop polls; 20-30 % of its time is
  skipped host-port polls), 0.35-0.4 s of a core. 90 % of the thread is the interpreter; host-port and bus code, USB
  and the panel the rest. It limits light patches with two DSP threads (2.7-2.8 x).
* **DSPs:** the library's per-slot work, not the DSP code: per DSP and frame about 10 peripheral passes, 8 serial
  slots and 29 DMA word transfers (two-dimensional line transfers between X memory and the ESAI registers), about
  1.5 us; `DmaChannel::execTransfer` alone is 30 % of a DSP thread. Heavy patches add the JIT code. They limit heavy
  patches (0.4-0.55 s per DSP thread).
* **The front panel** (dea4aff): no measurable cost (the commit before it and c8fbcf6 both 1.51-1.53 x on Drone,
  one DSP thread).

Tried and dropped:
* Specialised MOVE handlers called through m_table's member function pointers: slower (80 against 92 M/s); through
  plain function pointers they gain 7 %.
* A two-level dispatch table (a byte per opcode, then the handler): slower (85 against 97 M/s): the extra load is
  on the critical path.
* Each DSP thread running its DSPs 8 frames at a time instead of one: 12 % less DSP CPU on heavy patches, but the
  last DSP of the chain lags and the ColdFire's skipped polls went from 45 to 63 % of its time (its OS gets less
  done).
* The Runner resting until a quarter of its buffer is used (fewer starts of the threads): no measurable gain, 5 ms
  less tolerance to a stall.
* A DSP running its own code asking for its peripherals every 128 or 256 instructions instead of 64 (fewer passes
  on heavy patches): no measurable change in DSP time (bigsynth, threads 0: 6.14 / 6.22 / 6.02 s).

Next, if more is needed: the DSPs' per-slot cost (a direct path for the G2's two-dimensional ESAI DMA in dsp56300;
it would make one DSP thread enough for light patches and speed heavy ones up), a three-thread split that balances
the chain (A6 | A5 A4 | A3 by load), more specialised ColdFire handlers (MOVEM, ADD/SUB, AND, shifts) or a
predecoded instruction cache.

The first version's speed (2026-10-10 morning, before the work above; other programs kept the machine busy, load
about 10, threads at priority 31):

`g2emurun --kbd PATCH --midi 60/64/67` (a 3-note chord through MIDI) over 3 emulated seconds; "x real time" is
emulated seconds per wall second; CPU is per emulated second (waits count as CPU: the threads spin, then yield).

| Patch | Threads (DSP) | ColdFire | x real time | CPU s per s | per thread (ColdFire; DSP threads) |
|---|---|---|---|---|---|
| 1 kHz test | 0 | 162 MHz | 0.72 | 1.34 | 1.34 |
| 1 kHz test | 1 | 162 MHz | **1.46** | 1.32 | 0.66; 0.66 |
| 1 kHz test | 1 | 81 MHz | 1.62 | 1.19 | 0.59; 0.60 |
| Drone | 1 | 162 MHz | 1.36 | 1.39 | 0.70; 0.70 |
| Comp Keys1, chord | 0 | 162 MHz | 0.55 | 1.75 | 1.75 |
| Comp Keys1, chord | 1 | 162 MHz | 0.77 | 2.44 | 1.20; 1.23 |
| Comp Keys1, chord | 2 | 162 MHz | **1.64** | 1.74 | 0.58; 0.58, 0.58 |
| Comp Keys1, chord | 2 | 81 MHz | 1.80 | 1.60 | 0.53; 0.54, 0.54 |
| BigDualVCFSynth3, chord | 1 | 162 MHz | 0.99 | 1.97 | 0.98; 0.99 |
| BigDualVCFSynth3, chord | 2 | 162 MHz | **1.76** | 1.61 | 0.53; 0.54, 0.54 |
| VintageOrgan3, chord | 2 | 162 MHz | 1.52 | 1.79 | 0.59; 0.60, 0.60 |

Where the time went:
* **ColdFire:** the interpreter at 65-75 M instructions/s; the OS runs 55-65 M instructions per emulated second at
  162 MHz, all of it busy (no idle loop: the main loop polls the DSPs). So the ColdFire thread alone needs about 0.6
  of a core at the nominal speed, roughly in proportion to `cfHz`.
* **DSPs, light patch:** about 0.15 of a core each, mostly the library's per-slot work (8 slots per frame: ESAI, DMA
  of every slot word; `DmaChannel::execTransfer` alone is 14 %).
* **DSPs, heavy patch:** the JIT code (about 1,000 instructions per frame on each DSP with a chord) is about 55 % of a
  saturated DSP thread, the DMA and ESAI most of the rest: four heavy DSPs need about 1.2 cores, hence 2 DSP threads.
* **Real time:** light and typical patches with two threads (`threads = 1`) at 1.4-1.5 ×; heavy patches with chords
  need three (`threads = 2`, experimental) at 1.5-1.8 ×, or a slower ColdFire.

### 3.9.8 What remains for a real-time `EmulatedSoundEngine` in the plugin

`plugin/Source/SoundEngine.h` is the interface; `NativeSoundEngine` the existing engine. An `EmulatedSoundEngine` would:
1. **Firmware:** let the user point at their updater: Clavia's `.dmg` as downloaded, the `.rsrc`, the `.app`, or
   `g2os.py`'s output (`Firmware::load`). The `.dmg` (UDIF, zlib chunks, an HFS volume) is put back together in memory
   and the updater's resource file is found on it by its resource map, checked by the OS image's own checksums
   (`emu/g2emu/src/dmg.cpp`; needs zlib, found by CMake on macOS and Linux; assumes the file is stored in one piece, as
   in Clavia's image). Still missing: the Windows `.zip`/`.exe` (Wise, §4.7).
2. **Run it:** `g2emu::Runner` (machine on its own threads, `threads = 1` or 2) from `prepare()`; the audio thread
   calls `Runner::read()` for 96 kHz frames (lock-free, real-time safe) and resamples to the host's rate (JUCE's
   `LagrangeInterpolator` or better); `bufferMs` (20 ms in the tests) is the extra latency; boot takes about 2 s of
   silence.
3. **Patch:** `setPatch()` → `sendPatch` (or `sendPerformance` with the keyboard on slot A) through a link from
   `emulatedG2Link()`, ticked off the audio thread; parameter changes as `setParam` (the OS slews them, as the G2 does).
   The editor's whole synth connection (SynthSync) could also attach to this link instead of a G2.
4. **MIDI:** the host's MIDI to `Runner::midiIn()` as raw bytes (UART0). Sample-accurate timing would need the MIDI
   bytes scheduled against the emulated clock (today: at the next 1 ms chunk).
5. **State:** keep the flash image (`Machine::flash()`) in the plugin state or a user file, so the synth's banks
   persist; a snapshot of the whole machine (CPU, memories, DSPs) would skip the boot (not done).
6. **Threads and CPU:** 2-3 threads of an M1's 4 performance cores; a "lighter" setting can lower `cfHz` (less meter
   and LED polling, slower uploads, same audio). Since 2026-10-10 (§3.9.7): three threads (the ColdFire, two DSP
   threads: `threads = -1`), about 1.3-1.55 cores per instance in real time.
7. **Risks:** the JIT entitlement in DAWs (§3.9.6), the multi-DSP-thread crash, CPU on smaller machines.

**Done (2026-10-10), as built:** `plugin/Source/EmulatedSoundEngine` (with `-DG2_BUILD_EMU=ON`). The editor's
whole synth connection attaches to it (point 3's second option): *Synth > Connect to Emulated G2* starts a `Runner`
from the updater chosen once (setting `emulatorFirmware`) and gives SynthSync an `emulatedG2Link()` (`Kind::Emulated`);
once synced, the document goes out with `SynthSync::sendPatchAlone()` (a performance from `Performance::playing()`:
slot A alone, on the keyboard; after a fresh boot slot A does not play from MIDI) and from then on every edit goes as
to a G2. The processor swaps the engine under its callback lock; while it exists it plays instead of the native
engine (outputs 1/2 = **words 0 and 2** of `Machine::run`'s frames, which are in DAC order 1, 3, 2, 4). Offline
bounces wait for the machine (up to 2 s per block). The flash is kept in `Emulated G2 flash.bin` in the settings
folder, written back when it changed. Test: `[firmware]` "the plugin's path" (440 Hz on Out 1 and Out 2, in real
time). Still open from the list: Windows firmware, a machine snapshot, the JIT in hardened hosts; and each
plugin instance runs its own G2 (2-3 cores each). The `.dmg` is read since (point 1). Sample-accurate MIDI is in g2emu since (`Runner::midiInAt`,
§3.9.4); the engine still uses `midiIn`.

**Master volume (2026-10-10).** The emulated G2 played 23 dB too quietly because the panel's master volume knob read
half-way. The knob is on the panel ADC (MAX1039, I2C `0x65`): the OS writes setup `0xAA` and configuration `0x0D`
(scan inputs 0–6), then reads one endless byte stream, one byte per input in turn [C, emulated]. Stream position 1 is
the knob: the OS uses byte >> 1 as the step in its 128-step level table at `0x3010C094` (0 → 0, 64 → `0x08BC40`
= 0.068, 127 → `0x7FAA80` = 0.997) and sends it to A3's X:`$1739` through `0x3001FDD4` [C]. Position 4 feeds the DSP
timer calibration and must read `0x80` (any other constant stalls the boot). Only a period of 7 gives table values
(6 and 8 give blends: the OS averages the knob). The other positions are identified in §3.10.4 (G.Wheel 2, control
pedal, aftertouch, pitch stick, mod wheel, G.Wheel 1; the OS's own index is the position − 1).
`Machine::Options::masterVolume` (default 1, the knob all the way up) sets position 1. At 1, a signal of 1.0 into an
Out module (Level 127, no pad) gives DAC words of about −0.125 (−18 dBFS, inverted: the internal scale is 1/64, then
A3's ×0.997×8). `EmulatedSoundEngine` multiplies by −8, so both engines output signal units: the Drone test patch
plays at −17.7 dBFS emulated and −17 dBFS native. [C] the table, its index and X:`$1739`; [emulated] the stream
position. A real G2's knob position is the user's: unknown whether Clavia's analog stage adds gain after the DACs.

### 3.9.9 Reproducing

```sh
cmake -S . -B build -G Ninja -DG2_BUILD_EMU=ON && cmake --build build --target g2emurun g2emutests
build/tests/g2emutests                               # the [firmware] case needs original/firmware or G2_FIRMWARE
nice -n 10 build/emu/g2emurun --threads 1 --patch original/firmware/esai-links/test-patches/1khz_on.pch2 --seconds 1
nice -n 10 build/emu/g2emurun --threads 1 --kbd PATCH.pch2 --midi 60@0.1-0.9:1 --wav out.wav
nice -n 10 build/emu/g2emurun --realtime 3 --kbd PATCH.pch2 --midi 69@0.5-2.5:1    # Runner + LocalLink at the wall clock
tools/firmware/g2emubench.py --threads 0,1,2                      # speed and busy CPU per thread over the test patches
tools/firmware/g2emubench.py --realtime 20 --threads 2 --load 3  # underruns in real time beside 3 busy processes
build/venv/bin/python tools/firmware/g2emucompare.py ref/dac.wav out.wav
```
Debugging: `G2EMU_CFPROFILE=S` (ColdFire PC profile after S seconds; with `G2EMU_CFPROFILE_EXACT=1` by
instruction), `G2EMU_TIMING=1` (time in the ColdFire alone and its instructions per second, the ColdFire's waits),
`G2EMU_GAPS=1` (`--realtime`: when a block had a gap), `G2EMU_USBLOG=FILE` (the protocol traffic), `--trace`
(unmapped accesses, exceptions). On macOS measure speed under `taskpolicy -a` (§3.9.7).

## 3.10 The front panel (2026-10-10)

**Result.** The panel board is emulated at the level the OS drives it, so Clavia's own menus, displays and LEDs work:
`emu/g2emu/src/panel.{hpp,cpp}` (the hardware), wired into `machine.cpp` in place of the CS4/CS5 stubs, with a
thread-safe API on `Machine` (and `Runner`) and a stable header for a UI, `emu/g2emu/include/g2emu/panel.hpp`
(the controls by the names printed on the panel). `g2emurun --panel` prints the displays, the LEDs by name and the
knobs' LED rings; `--press RAW@T[-T2]`, `--turn RAW:N@T`, `--adc POS:V@T`, `--key K@T-T2[:MS]`, `--var V@T` and
`--panel-at T` drive it from the command line (`G2EMU_CGRAM=1`, `G2EMU_LEDBUF=1`, `G2EMU_MEMDUMP=FILE`,
`G2EMU_PCWATCH=ADDR,...` for debugging). Tests: `Panel: ...` (hardware only) and the `[firmware]` case "The user's
G2 OS drives the emulated front panel".

The board has no processor (service manual): every LED, character and switch goes through latches the ColdFire
strobes from its timer tasks (the 8272 Hz tick scheduler, §3.9.2).

### 3.10.1 Bus map [C, emulated]

| Address | Direction | Function | Source |
|---|---|---|---|
| CS5 `0x15000000` | write | bits 0-6: LED rows 8-14; bit 7: enable of the input multiplexer (set while +7 selects a byte) | [C 0x3005bd74, 0x3005bf3e] |
| CS5 `+1` | write | LED rows 16-23 | [C 0x3005bf3e] |
| CS5 `+2` | write | LED rows 0-7 | [C] |
| CS5 `+3` | write | LED column strobe, active low; the OS strobes one column per call, 0x80 first, and writes `0xFF` (none) before changing the rows | [C] |
| CS5 `+4` | write | data bus of the five LCDs | [C 0x3005c046] |
| CS5 `+5` | write | LCD 1-4 control: RS/E = `0x80/0x40` (LCD 1), `0x20/0x10`, `0x08/0x04`, `0x02/0x01` (LCD 4) | [C table 0x3011A0A2: data latch, control latch, RS mask, E mask per LCD] |
| CS5 `+6` | write | LCD 0 control: RS `0x02`, E `0x01` | [C] |
| CS5 `+7` | write | input multiplexer select, active low: bytes 0-5 buttons, 6-7 encoders; `0xFF` with `+0` bit 7 clear: nothing | [C 0x3005bd74, schedule table 0x300EC760] |
| CS5 `+0` | read | the selected byte (active low); nothing selected: bits 6-7 the dial's quadrature (the OS swaps them), bits 4-5 the model | [C] |
| CS4 `0x14000000` | write (16 bit) | keyboard matrix column, active low, one of 16 | [C 0x30029d78] |
| PADAT `MBAR+0x248` | read | bits 0-7: keyboard matrix rows, active low; bit 12: high while no sustain pedal is plugged in; bit 13: sustain pedal contact (low = closed); bit 9: low = expansion board present; bits 14-15: the EEPROM's bit-banged lines (outputs) | [C 0x30029d78, 0x30027926, 0x30037f64, 0x30053218] |
| I2C `0x65` (MAX1039) | read | the analogue controls (below) | §3.9 "Master volume" |

Timing [emulated, measured at boot]: the multiplexer task reads one byte per tick in a 42-step cycle (table at
`0x300EC760`): the encoder bytes 6 and 7 and the no-select byte 12 times each, each button byte once (every 5 ms).
The LED task strobes about 460 columns per second (each column about 58 times a second). The keyboard task drives
all 16 columns about 3900 times a second. The ADC stream runs continuously.

### 3.10.2 Displays [C, emulated]

Five **HD44780-type character LCDs, 2 lines × 16 characters**, 8-bit interface, write-only (the OS never reads the
busy flag; it waits with delay loops). The OS initialises each one with `30 30 30 38 08 01 06 0C` [C 0x3005664c]
(function set: 8 bits, 2 lines, 5×8 dots; display on, cursor off), addresses characters with `0x80 + column` (line 1)
and `0xC0 + column` (line 2) [C 0x3005687c], keeps a 32-character shadow and a dirty mask per LCD
(`0x302A0DB8 + 298 × n`) and sends one changed character per call. `0x0E`/`0x0C` switch an underline cursor on and off
(text entry, e.g. Synth Name).

* **LCD 0 = the main display** (its own control latch `+6`), **LCDs 1-4 = the four assignable displays**, left to
  right: LCD 1 shows knobs 1-2 (8 characters each), LCD 4 knobs 7-8 [emulated: each knob turned changes the display
  above it]. Line 1: the module name (or a parameter value while a knob is turned); line 2: the two parameter names
  (Display Mode switches to values) [emulated].
* **Character set:** the controller's own ROM (A00, Japanese standard). The OS defines 8 **user characters** (CGRAM)
  on each LCD [C 0x30056bba]: the descender letters g, p, q, y, j (codes 0-4; the OS maps the ASCII letters to them
  through a 256-byte table per LCD, `+0x2A` in its LCD record), then ±, a symbol and a bar [emulated]. The model keeps
  the CGRAM; `PanelDisplay::text()` names the descender letters by their shape (no copy of the OS's dots in the source).
  A UI draws codes 0-15 from `cgram`, the rest with an A00 font (`hd44780Unicode()` maps them to Unicode).
* Seen [emulated]: during boot "Nord Modular G2" / "Version 1.62"; after boot "-:-       No Cat" (no stored patch)
  and the patch settings on the assignable displays (" 30 BPM", "1/16T", "50 cnt", "2 semi" ...); after an upload
  the patch name on line 2 of the main display; the System menu ("Master Tune  |Sy", "MIDI Local   |Sy" ...).

### 3.10.3 LEDs [C, emulated]

An **8 × 23 matrix** (184 LEDs; row 15 is the multiplexer enable bit), refreshed from a 32-byte buffer at
`0x302BCB42`, **active low** (0 = lit). The OS's LED number n is bit `n & 7` of byte `n >> 3` [C 0x30057224 and its
siblings]; byte `0x1E − 4c` goes to rows 0-7, `0x1D − 4c` to rows 8-14 and `0x1C − 4c` to rows 16-23 of column c
(strobe bit `7 − c`) [C 0x3005bf3e]. Bytes 3, 7, ... 31 are not shown. The model latches the rows at each strobe;
checked against the OS's buffer (`G2EMU_LEDBUF`): identical once a column has been refreshed. There is no dimming:
the LEDs are on or off, and blink by toggling (one blink group, every 3072 ticks = 0.37 s [C 0x3001b5ec,
0x30057810]). Column c is the column of knob 8 − c.

| LED (OS number) | Function | Evidence |
|---|---|---|
| 32k + 0..14 | knob k+1's LED graph (15 LEDs), lowest value first; rows 16-23 then 8-14; the middle one (7) at a centred value | [C 0x300ECD36: base per knob; emulated: knob 1 swept from −64 to +63 lit 0, 1, ... 14] |
| 32k + 21 | the LED above assignable button k+1 (a module LED or meter assigned to the knob) | [C table 0x300ECD26, modes at 0x30066daa] |
| 32k + 22 | assignable button k+1 | [C table 0x300ECD2E; emulated: buttons 1, 3, 5, 6 lit 22, 86, 150, 182] |
| 242, 19, 51, 83, 115, 147, 179, 211 | Variation 1-8 | [C table 0x300ECEB5; emulated: variations 1-8 selected over USB and with the buttons] |
| 243 | Morph | [emulated: Morph button] |
| 177, 209, 241, 18 | Slot A-D (slot active; blinks on the focused slot when several are active) | [C table 0x300ECE88 from the slot's "enabled" flag (performance header +0x2B), 0x3006a9d2] |
| 49, 81, 113, 145 | a second LED per slot: the keyboard plays the slot (performance header "keyboard" flag) | [C table 0x300ECE84, same function]; **its label on the panel is not known** |
| 50, 82, 114, 146, 178 | Octave shift −2, −1, 0, +1, +2 | [C table 0x300ECE8C; emulated: Octave Down lit 82, Up 146] |
| 87, 119, 151, 183, 215 | Parameter page rows A-E | [emulated: Page A-E buttons] |
| 20, 52, 84 | Parameter page columns 1-3 | [emulated] |
| 23 / 55 | Patch Settings / Global Panel | [emulated: Patch Settings, double press] |
| 144 / 176 / 208 | System / Patch / Store | [emulated] |
| 148 | Load Patch (blinks while a patch is selected but not loaded) | [emulated] |
| 17 | Perf.Mode | [emulated] |
| 210 / 112 | KB Hold / KB Split | [C 0x3006af44/af6a; emulated] |
| 116, 80, 247, 48 | the four split point LEDs above the keyboard, in the order Shift+KB Split steps through them; left to right is [inferred] from the split notes rising with each step (table at `0x300ECEA1`) | [emulated] |
| 240 | Sub Func. (Shift+Display Mode) | [emulated] |
| 16 | MIDI (incoming MIDI) | [emulated: a note on MIDI IN] |
| 244, 212, 180 | Mic/In 1 level −20, −12, 0 dB (0 dB held for 128 calls) | [C 0x3006bd98: thresholds 0.1, 0.25, 0.89 of full scale on DSP data] |

All 64 positions of rows 0-7 have a function; the G2X's wheel LEDs are not in the matrix (probably lit by the power
supply, not the OS) [inferred].

### 3.10.4 Inputs [C, emulated]

**Buttons:** 46 switches in multiplexer bytes 0-5 (scan position = byte × 8 + bit, the OS's button number is
byte × 16 + bit), active low, no debouncing in software. Changes become events (press, release, auto-repeat, double
press within 50 scans) [C 0x30057cc0]. Six are modifiers held in a mask the events carry [C table 0x3010A3D8]:
Shift (7, bit 0), Focus/Copy (6, bit 1), Slot A-D (40-43, bits 2-5).

| Scan | Button | Evidence |
|---|---|---|
| 0, 1, 2, 3 | Navigator Up, Down, Right, Left | [emulated: Down steps through the System menu; Right/Left step the variation in play mode] |
| 4 | Load Patch | [emulated: "No patches stored", Load LED] |
| 5 | KB Hold (Shift: Panic) | [C 0x3006af44: Shift → all notes off 0x30025f68] |
| 6, 7 | Focus/Copy, Shift | [C modifier table; emulated: Shift+Display Mode = Sub Func.] |
| 8, 9, 10 | System, Patch, Store | [emulated: "Master Tune |Sy", "Patch Parameters", "Store to 1:1"] |
| 11 | Display Mode | [emulated] |
| 12 | Performance mode (Shift: Perf. Transfer) | [emulated: "Empty Perf.", Perf.Mode LED] |
| 13 | KB Split (Shift: next split point) | [C 0x3006af6a; emulated] |
| 14 | Patch Settings (double press / Shift: Global Panel) | [emulated] |
| 15 | Morph (Shift: Vari.Init) | [emulated] |
| 16-20, 21-23 | Parameter pages A-E, 1-3 | [emulated] |
| 24-31 | Variation 1-8 | [emulated: the client hears variation 2, 3, 8] |
| 32-39 | assignable buttons 1-8 | [emulated: the parameter under the knob toggles] |
| 40-43 | Slot A-D | [emulated] |
| 44, 45 | Octave Shift Down, Up | [C 0x3006aefa/af2a; emulated] |
| 46, 47 | not connected (ignored by the OS) | [C dispatch table 0x3005d8c6] |

**Encoders** (quadrature, active low pairs):
* The 8 knobs: bytes 6-7, 4 pairs each (bits 7-6 first). Knob 1-8 = scan pairs 7, 3, 6, 2, 5, 1, 4, 0 (byte 7 holds
  the odd knobs) [emulated]. Every transition counts one step of the parameter [C 0x30057aac, table 0x300EC540;
  emulated: 10 transitions = 10 semitones], except the first after a change of direction, which the OS drops.
  Clockwise (increasing) is the pair sequence 0 → 2 → 3 → 1.
* The dial: no-select byte, bits 6-7. One step per detent of 4 transitions, counted from the rest position (both
  contacts open, pair value 3) [C 0x300580fa]; detents less than 30 scans apart accelerate (step size up to 15).
* `Machine::panelEncoder` makes one UI step one parameter step (adds the dropped transition) and one dial step one
  detent spaced 8 ms (no acceleration).

**Analogue (panel ADC stream positions; the OS's index is position − 1, after its dummy read)** [C 0x30056536,
0x30004e46, 0x30012588 and the morph groups their messages feed at 0x300124e2; emulated: each position's change
reaches its handler]:

| Position | Control | OS handling |
|---|---|---|
| 0 | G.Wheel 2 (G2X) | ignored unless the model is G2X; morph group 8 |
| 1 | Master Level | output level table, A3's X:$1739 (§3.9) |
| 2 | Control pedal | × (Ctrl Ped Gain + 64) / 64; morph group 6 (Ctrl.Pd) |
| 3 | Aftertouch | inverted (0xFF = no pressure) through a 128-step curve [table 0x30114E5C]; morph group 4 (Aft.Tch) |
| 4 | Pitch stick | raw, centred at 0x80 (which the boot calibration requires); morph group 7 (P.Stick) |
| 5 | Mod wheel | raw; morph group 1 (Wheel) |
| 6 | G.Wheel 1 (G2X) | ignored unless G2X; morph group 5 (G.Wh1/Sus.P) |

The values in this table's "Control" column are the matching of handlers to morph groups (the group list "Wheel,
Vel, Keyb, Aft.Tch, Sust.Pd, Ctrl.Pd, P.Stick, G.Wh 2" is in the OS); the Ctrl Ped Gain scaling and the centred
pitch stick confirm two of them independently. The direction of each control (e.g. which end of the pitch stick is
0xFF) is not verified. The model's rest values: 0, master, 0, 0xFF, 0x80, 0, 0.

**Model:** bits 4-5 of the no-select byte: 0 = Nord Modular G2, 3 = G2X, 2 = G2 Rack/Engine ("G2R"); 1 is not
handled; `MBAR+0x1D0` bit 0 set selects a fourth model (3) without a name [C 0x30050864, strings at 0x300E9F86].
`Machine::Options::model`.

**Keyboard:** 64 keys × 2 contacts on a 16 × 8 matrix: key k is on columns 2(k >> 3) + 1 (contact A, closes first) and
2(k >> 3) (contact B), row bit 7 − (k & 7) [C 0x30029d78, 0x30029468/0x30029570]. Note = key + 48 (`0x302437ED`, before
octave shift) on the G2: key 21 played 440 Hz [emulated]. Velocity: the ticks from A to B, halved, plus a per-key
offset, through a falling 128-entry table (127 at ≤ 6, about 64 at 31, 3 at 127) [C 0x300EA860]; the model's
velocity → contact time is a fit of that curve (127 × 0.97^(i − 6)) without the per-key offset: approximate.

**Sustain pedal:** GPIO bits 12/13; with "Sust Ped Pol" 0 the pedal is down while bit 13 is low [C 0x30027926]. Not
verified on the OS's sound.

### 3.10.5 What the emulator does differently, and what is unknown

* **MIDI Local is Off with an erased flash.** Variation buttons, KB Hold, octave shift and the keyboard then only
  go out as MIDI (and the OS sent nothing on MIDI OUT in our runs, so they seem to do nothing). The firmware test sets
  Local On through the System menu; a UI should keep the flash (`Machine::flash()`) so the setting persists.
* Not modelled: the LCD controller's timing and busy flag (unused), its 4-bit mode and font ROM (a UI draws A00),
  contact bounce, the encoders' detent feel. Rings and LEDs are latched at their strobes: a UI sees exactly what the
  OS last wrote.
* Unknown: the label of the second slot LED (49, 81, 113, 145); the physical left-to-right order of the split point
  LEDs; the direction of the pitch stick and wheels; whether an unplugged control pedal reads 0; the velocity
  table's per-key offsets (`0x302437EE`); MIDI OUT (nothing sent, so the panel's MIDI echo could not be used to
  check the controls).

## 4. Open questions
1. **DSP part number and clock.** The firmware is consistent with a 56367 at about 150 MHz, but the DSP EXTAL source
   is unknown. The 56.620363 MHz oscillator, the PCTL ×4 and the cycle budget need reconciling. Read the board markings
   from a high-resolution photo, or the oscillator net in the service manual's schematics if any.
2. **Exact sample rate:** 96.000 kHz or about 98.3 kHz (as on the NL2X)? It matters for pitch accuracy in any engine.
   §3.7.1: a frame is 1536 DSP clocks [C], so the rate is the DSP clock / 1536; the OS's pitch increments are exact
   at 96 kHz, which needs a 147.456 MHz DSP clock.
3. **Inter-DSP audio topology:** answered in §3.7 (chain on ESAI, ring on ESAI_1, slot map). Still open: the order of
   A5 and A4, and the latency on the board.
4. **The DSP kernel:** where it lives (the 1,156-word FE/0x20 fragment?), its per-sample cycle overhead, how
   parameters, morphs, LEDs and meters are exchanged over HDI08.
5. **ColdFire ISA subset.** Answered in §3.6.1: ISA_A only (no MAC/EMAC, no hardware divide), and QEMU's
   ColdFire V4e model runs it; so does Gearmulator's V2 core (§3.9.1). MCF5407 vs MCF5307 is still unconfirmed; the
   CACR branch-cache bits point to V4. The bus clock is 54 MHz [C, §3.9.2]; the core clock (162 MHz) is not
   confirmed.
6. **The 12 OS fragments not found in the Demo:** modules missing from the Demo, or OS 1.62 changes after Demo 1.40?
   Mapping descriptors to module types would answer this and is needed for approach 3 anyway.
7. **The Windows updater payload** (Wise) is not extracted. The Mac copy is enough, but users on Windows will have the
   `.zip`/`.exe`. The plugin's firmware import must handle the Wise package, or ask for the DMG.
8. **Gearmulator license:** "GPL-3.0-only" or "or later"? Not stated. Either is compatible with AGPL-3.0-or-later via
   §13, but record whichever applies.
9. **The G2 Demo's DSP interpreter:** whether it emulates DSP56300 instructions generically or uses a reduced
   interpreter. Worth a look in the PPC binary (`original/demo/NMG2Demo`) when approach 2 or 3 starts.
