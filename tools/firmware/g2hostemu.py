#!/usr/bin/env python3
"""Run the G2's ColdFire OS in a CPU emulator with modelled hardware and four
emulated DSP56367s on its host ports; capture what it sends to the DSPs and,
optionally, talk to it over its USB chip with our own protocol client.

  g2hostemu.py [--steps N] [--progress N] [--out DIR]          boot and capture
  g2hostemu.py --patch FILE.pch2 [--usb-start SLICE] ...        then upload a patch
      [--patch-to B:FILE ...] [--settle S] [--seconds S]        more slots; record the DACs (dac.wav, dac.json)
  audio links between the DSPs: --chain, --ring-prefill, --link-queue, --no-links, --no-idle-skip, --throttle
  debugging: --break PC, --probe PC, --watch ADDR[:LEN], --trace-mmio, --debug-flash

Needs a venv with `pip install unicorn` (2.1.x) and the libraries built by
`cmake --build build --target g2dspbridge g2protobridge` (-DG2_BUILD_EMU=ON).

--fw (default original/firmware) holds the user's own firmware, unpacked by
g2os.py: os-v162-unpacked/CODE_30000400.bin, os-v162-unpacked/SRAM_20000800.bin
and mac-updater-rsrc/BOOT/128_Loader.bin. Nothing from Clavia is stored in this
script; its output (host-port streams, DSP images, audio) goes to --out
(default original/firmware/dsp-boot, gitignored).

CPU: Unicorn's m68k target (QEMU) with the ColdFire V4e model, with workarounds
for what Unicorn lacks (re/notes/g2-hardware-and-emulation.md 3.6.1): MOVEC to
RAMBAR/MBAR skipped, RTE performed in the interrupt hook, interrupts entered
through a guest-code stub (SR is never read from Python), slices ended at
translation-block boundaries.

Memory map (from the boot loader's chip-select set-up, notes 2.3 and 3.6.2):
  0x00000000  boot flash (CS0)           the Loader image
  0x10000000  MCF5407 MBAR peripherals   SIM/interrupts, timers, UARTs, I2C (ADC stub), GPIO
  0x11000000  DSP host ports (CS1)       HDI08 of DSPs on A3..A6 (g2dspbridge), stubs for A7..A10
  0x12000000  8 MB flash (CS2)           AMD command set with CFI and unlock bypass, erased at start
  0x13000000  USB (CS3)                  ISP1181 model, IRQ3; host side = proto::Client (g2protobridge)
  0x14000000-0x17000000  CS4..CS7         latches (panel), read 0
  0x20000000  internal SRAM (RAMBAR)
  0x30000000  4 MB SDRAM                  CODE loaded at 0x30000400, entry there
"""
import argparse
import collections
import json
import os
import struct
import sys
import time

os.environ.setdefault('UC_IGNORE_REG_BREAK', '1')
from unicorn import Uc, UcError, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED  # noqa: E402
from unicorn import UC_HOOK_MEM_WRITE, UC_HOOK_INTR, UC_HOOK_BLOCK  # noqa: E402
from unicorn.m68k_const import (UC_CPU_M68K_CFV4E, UC_M68K_REG_PC, UC_M68K_REG_SR, UC_M68K_REG_A7,  # noqa: E402
                                UC_M68K_REG_D0, UC_M68K_REG_A0)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

CODE_BASE = 0x30000400
SRAM_BASE = 0x20000800


class Log:
    """Prints each kind of MMIO access once: per (device, direction, offset), or also
    per PC with trace=True (slower: reading the PC costs a Unicorn call)."""

    def __init__(self, trace=False):
        self.trace = trace
        self.seen = set()
        self.emu = None

    def once(self, key, text):
        if self.trace:
            key = key + (self.emu.pc(),)
        if key in self.seen:
            return
        self.seen.add(key)
        print(text() if callable(text) else text)


# --------------------------------------------------------------------------
# HDI08 host side (one per DSP). Host registers (8-bit bus, A0-A2):
#   0 ICR  1 CVR  2 ISR  3 IVR  4 -  5 TXH/RXH  6 TXM/RXM  7 TXL/RXL
# ISR: RXDF 0x01, TXDE 0x02, TRDY 0x04, HF2 0x08, HF3 0x10, HREQ 0x80.
# ICR: RREQ 0x01, TREQ 0x02, HDRQ 0x04, HF0 0x08, HF1 0x10, HLEND 0x20, INIT 0x80.
class DspBridge:
    """ctypes wrapper of emu/dspbridge (libg2dspbridge): one emulated DSP56367."""
    lib = None

    @classmethod
    def load(cls, path):
        import ctypes
        lib = ctypes.CDLL(path)
        vp, u32, i = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int
        u64 = ctypes.c_uint64
        for name, res, args in (('g2dsp_create', vp, []), ('g2dsp_destroy', None, [vp]),
                                ('g2dsp_host_read', u32, [vp, i]), ('g2dsp_host_write', None, [vp, i, u32]),
                                ('g2dsp_run', None, [vp, u32]), ('g2dsp_booting', i, [vp]),
                                ('g2dsp_boot_count', i, [vp]), ('g2dsp_boot_info', u32, [vp, i]),
                                ('g2dsp_pc', u32, [vp]), ('g2dsp_instructions', u64, [vp]),
                                ('g2dsp_hcr', u32, [vp]), ('g2dsp_rx_queued', u32, [vp]),
                                ('g2dsp_mem_read', u32, [vp, i, u32]), ('g2dsp_reg', u32, [vp, i]),
                                ('g2dsp_disasm', i, [vp, u32, ctypes.c_char_p, i]),
                                ('g2dsp_capture', None, [vp, i]), ('g2dsp_tx_frames', u64, [vp, i]),
                                ('g2dsp_tx_take', u32, [vp, i, ctypes.POINTER(u32), u32]),
                                ('g2dsp_cycles', u64, [vp]), ('g2dsp_esai_reg', u32, [vp, i, i]),
                                ('g2dsp_mem_write', None, [vp, i, u32, u32]),
                                ('g2dsp_rx_frames', u64, [vp, i]), ('g2dsp_waited_ns', u64, [vp]),
                                ('g2dsp_thread_cpu_ns', u64, [vp]), ('g2dsp_shutdown_all', None, []),
                                ('g2dsp_clock', None, [vp, u32, u32, u32, u32, u32]),
                                ('g2dsp_link', i, [vp, i, i, vp, i, u32, u32, u32]),
                                ('g2dsp_link_stats', u64, [i, i]),
                                ('g2dsp_sink_record', None, [vp, u64]),
                                ('g2dsp_sink_take', u32, [vp, i, ctypes.POINTER(ctypes.c_int32), u32]),
                                ('g2dsp_throttle', None, [vp, ctypes.c_double, u32]),
                                ('g2dsp_idle_loop', None, [vp, u32, u32, u32, u32]),
                                ('g2dsp_skipped', u64, [vp]), ('g2dsp_jit', None, [vp, i])):
            f = getattr(lib, name)
            f.restype, f.argtypes = res, args
        cls.lib = lib

    def __init__(self):
        self.h = self.lib.g2dsp_create()

    def dis(self, addr):
        import ctypes
        buf = ctypes.create_string_buffer(200)
        n = self.lib.g2dsp_disasm(self.h, addr, buf, 200)
        return buf.value.decode(), n

    def __getattr__(self, name):
        f = getattr(self.lib, 'g2dsp_' + name)
        return lambda *a: f(self.h, *a)


class HostPort:
    def __init__(self, index):
        self.index = index
        self.icr = 0
        self.cvr = 0
        self.ivr = 0x0f
        self.tx = [0, 0, 0]
        self.hf23 = 0          # what the DSP reports in ISR HF2/HF3
        self.rx = collections.deque()  # 24-bit words from the DSP
        self.rxlatch = [0, 0, 0]
        self.events = []       # ('w', word) / ('icr', v) / ('cvr', v)
        self.dsp = None

    def isr(self):
        v = 0x02 | 0x04 | self.hf23
        if self.rx:
            v |= 0x01 | 0x80
        return v

    def read(self, reg):
        if self.dsp is not None:
            v = self.dsp.host_read(reg)
            if reg in (5, 6, 7):
                self.events.append(('r%d' % reg, v))
            return v
        if reg == 0:
            return self.icr
        if reg == 1:
            return self.cvr
        if reg == 2:
            return self.isr()
        if reg == 3:
            return self.ivr
        if reg in (5, 6, 7):
            if reg == 5 and self.rx:  # (inferred) latch a new word when its high byte is read
                w = self.rx[0]
                self.rxlatch = [(w >> 16) & 0xff, (w >> 8) & 0xff, w & 0xff]
            v = self.rxlatch[reg - 5]
            if reg == 7 and self.rx:
                self.rx.popleft()
            return v
        return 0

    def write(self, reg, val):
        if self.dsp is not None:
            self.dsp.host_write(reg, val)
            if reg in (5, 6, 7):
                self.tx[reg - 5] = val
                if reg == 7:
                    self.events.append(('w', (self.tx[0] << 16) | (self.tx[1] << 8) | self.tx[2]))
            elif reg in (0, 1):
                self.events.append(('icr' if reg == 0 else 'cvr', val))
            return
        if reg == 0:
            self.icr = val & 0x7f   # INIT completes at once (it self-clears)
            self.events.append(('icr', val))
            if self.dsp is None:   # stub DSP: echo HF0 as HF2 (inferred handshake)
                self.hf23 = 0x08 if val & 0x08 else 0
        elif reg == 1:
            self.cvr = val
            self.events.append(('cvr', val))
            if self.dsp is None:   # stub DSP: host command taken at once
                self.cvr &= 0x7f
        elif reg == 3:
            self.ivr = val
        elif reg in (5, 6, 7):
            self.tx[reg - 5] = val
            if reg == 7:  # big-endian mode (HLEND=0): writing TXL sends the word
                if self.icr & 0x20:
                    w = (self.tx[2] << 16) | (self.tx[1] << 8) | self.tx[0]
                else:
                    w = (self.tx[0] << 16) | (self.tx[1] << 8) | self.tx[2]
                self.events.append(('w', w))


class HostPorts:
    """CS1, 0x11000000-0x110007FF: A3..A10 one-hot active-low DSP selects."""

    def __init__(self, emu, n=8, dsps=0):
        self.emu = emu
        self.ports = [HostPort(i) for i in range(n)]
        for p in self.ports[:dsps]:
            p.dsp = DspBridge()
        self.order = []  # (selected-mask, reg, value) for writes, in order

    def selected(self, off):
        sel = (~off >> 3) & 0xff
        return [p for p in self.ports if sel & (1 << p.index)]

    # CS1 is an 8-bit port: the bus controller splits word/long accesses into
    # byte cycles at off, off+1, ... (big-endian).
    def read(self, uc, off, size, ud):
        v = 0
        for k in range(size):
            v = (v << 8) | self.read8(off + k)
        self.emu.log.once(('HR', off,), lambda: f'HOST R +{off:03x}/{size} = {v:x} pc={self.emu.pc():x}')
        return v

    def write(self, uc, off, size, val, ud):
        self.emu.log.once(('HW', off,), lambda: f'HOST W +{off:03x}/{size} = {val:x} pc={self.emu.pc():x}')
        for k in range(size):
            self.write8(off + k, (val >> (8 * (size - 1 - k))) & 0xff)

    def read8(self, off):
        v = 0xff
        for p in self.selected(off):
            v &= p.read(off & 7)  # (inferred) AND when several are selected
        return v

    def write8(self, off, val):
        ps = self.selected(off)
        for p in ps:
            p.write(off & 7, val)
        self.order.append((sum(1 << p.index for p in ps), off & 7, val))


# --------------------------------------------------------------------------
# Decoding a host-port stream. The OS first boots each DSP through its boot ROM
# (count, address, P words), with a small program ("stage 1") whose host-command
# vectors are memory primitives [C] (P:$80..$B6 of that program; HV = vector/2):
MONITOR = {  # HV: (action, space) ; 'w' = write HORX, 'r' = HOTX read-back
    0x40: ('w+', 'X'), 0x41: ('w+', 'Y'), 0x42: ('w+', 'P'),
    0x43: ('w', 'X'), 0x44: ('w', 'Y'), 0x45: ('w', 'P'),
    0x46: ('rw', 'X'), 0x47: ('rw', 'Y'), 0x48: ('r+', 'P'),
    0x49: ('r', 'X'), 0x4A: ('r', 'Y'), 0x4B: ('r', 'P'),
    0x4C: ('la', None), 0x4D: ('r0', None), 0x4F: ('r', 'x:$44'),
    0x50: ('dor0', None), 0x51: ('dor1', None), 0x52: ('dco1', None), 0x55: ('tlr0', None),
    0x56: ('r', 'r0'), 0x57: ('echo', None), 0x58: ('ep', None), 0x59: ('wy+ep', None),
    0x5A: ('r', 'ep'), 0x5B: ('r', 'ep'),
}


def decode_stream(events):
    """events: [(kind, value)] as recorded per DSP ('w' word written, 'cvr', 'icr').
    Returns (boot, mem, log): the boot-ROM load (address, words), the memory images
    written through the monitor {space: {addr: word}}, and a list of command records."""
    words = [v for k, v in events if k == 'w']
    i = 0
    boot = None
    pend = collections.deque()
    mem = {'P': {}, 'X': {}, 'Y': {}}
    log = []
    r0 = 0
    it = iter(events)
    # boot ROM: count, address, words
    for k, v in it:
        if k != 'w':
            continue
        pend.append(v)
        if len(pend) >= 2 and len(pend) == pend[0] + 2:
            n, a = pend.popleft(), pend.popleft()
            boot = (a, list(pend))
            pend.clear()
            break
    for k, v in it:
        if k == 'w':
            pend.append(v)
            continue
        if k != 'cvr' or not v & 0x80:
            continue
        hv = v & 0x7f
        act, space = MONITOR.get(hv, ('?', None))
        rec = {'hv': hv, 'act': act, 'space': space, 'r0': r0}
        take = act in ('w+', 'w', 'rw', 'la', 'r0', 'dor0', 'dor1', 'dco1', 'tlr0', 'echo', 'ep', 'wy+ep')
        val = pend.popleft() if take and pend else None
        rec['val'] = val
        if act in ('w+', 'w', 'rw') and val is not None:
            mem[space][r0] = val
        if act == 'r0':
            r0 = val
        elif act in ('w+', 'r+'):
            r0 = (r0 + 1) & 0xffffff
        log.append(rec)
    return boot, mem, log


def runs(addrs):
    """Contiguous ranges of a set of addresses."""
    out = []
    for a in sorted(addrs):
        if out and a == out[-1][1] + 1:
            out[-1][1] = a
        else:
            out.append([a, a])
    return out


# --------------------------------------------------------------------------
class Flash:
    """CS2 8 MB, 16-bit AMD-style flash: read array, CFI query, autoselect, program,
    sector erase. Erased (0xFF) at power-on unless an image is given.

    The OS identifies the chip by CFI [C] (0x3000430E): 0x98 at word 0x55, "QRY" at
    words 0x10-0x12, then the primary command set at 0x13: 1 (Intel, 64 x 128 KB
    blocks) or 2 (AMD, 128 x 64 KB sectors); anything else shows "FLASH FAILURE /
    UNKNOWN CHIP". We answer 2, the command set modelled here (which chip the G2
    really has is not known)."""
    CFI = {0x10: ord('Q'), 0x11: ord('R'), 0x12: ord('Y'), 0x13: 0x02, 0x14: 0x00,
           0x27: 23,  # 2^23 bytes
           0x28: 0x01, 0x29: 0x00,  # x16
           0x2C: 1, 0x2D: 127, 0x2E: 0, 0x2F: 0, 0x30: 1}  # one region: 128 sectors of 256*256 bytes

    def __init__(self, emu, size=0x800000):
        self.emu = emu
        self.mem = bytearray(b'\xff' * size)
        self.state = 0
        self.mode = 'read'
        self.writes = 0
        self.bypass = False
        self.verbose = False

    def read(self, uc, off, size, ud):
        v = self._read(off, size)
        if self.verbose:
            print(f'flash r {off:06x}/{size} = {v:x} pc={self.emu.pc():x}')
        return v

    def _read(self, off, size):
        if self.mode == 'cfi':
            w = self.CFI.get((off >> 1) & 0xff, 0)
            v = w if size == 2 else (0 if not off & 1 else w)
        elif self.mode == 'id':
            w = {0: 0x0001, 1: 0x22F9}.get((off >> 1) & 0xff, 0)  # (inferred) AMD, some 16-bit part
            v = w if size == 2 else (w >> 8 if not off & 1 else w & 0xff)
        elif self.mode == 'status':
            v = 0x0080 if size == 2 else 0x80  # DQ7 = data done
            self.mode = 'read'
        else:
            v = int.from_bytes(self.mem[off:off + size], 'big')
        self.emu.log.once(('FR', off >> 16,), lambda: f'FLASH R +{off:06x}/{size} = {v:x} ({self.mode}) pc={self.emu.pc():x}')
        return v

    def write(self, uc, off, size, val, ud):
        if self.verbose:
            print(f'flash w {off:06x}/{size} = {val:x} pc={self.emu.pc():x}')
        w = off >> 1
        self.emu.log.once(('FW', off >> 16, val,), lambda: f'FLASH W +{off:06x}/{size} = {val:x} pc={self.emu.pc():x}')
        if self.state == 'program':
            old = int.from_bytes(self.mem[off:off + size], 'big')
            self.mem[off:off + size] = (old & val).to_bytes(size, 'big')
            self.state = 0
            self.writes += 1
            return
        val &= 0xff
        if self.bypass:  # unlock bypass (AA 55 20): A0 data, 80 30/10 erase, 90 00 exit [C] 0x30003DA0
            if self.state == 'bx':
                self.bypass = val != 0x00
                self.state = 0
            elif self.state == 'be' and val in (0x30, 0x10):
                self.erase(off, val == 0x10)
                self.state = 0
            elif val == 0xA0:
                self.state = 'program'
            elif val == 0x80:
                self.state = 'be'
            elif val == 0x90:
                self.state = 'bx'
            else:
                self.state = 0
            return
        if val in (0xF0, 0xFF):
            self.state, self.mode = 0, 'read'
        elif self.state == 2 and val == 0x20:
            self.state, self.bypass = 0, True
            if self.emu.debug_flash:
                self.emu.block_trace()
                self.verbose = True
        elif val == 0x98 and w & 0xff == 0x55:
            self.state, self.mode = 0, 'cfi'
        elif self.state == 0 and w & 0x7ff == 0x555 and val == 0xAA:
            self.state = 1
        elif self.state == 1 and w & 0x7ff == 0x2AA and val == 0x55:
            self.state = 2
        elif self.state == 2 and val == 0x90:
            self.state, self.mode = 0, 'id'
        elif self.state == 2 and val == 0xA0:
            self.state = 'program'
        elif self.state == 2 and val == 0x80:
            self.state = 3
        elif self.state == 3 and val == 0xAA:
            self.state = 4
        elif self.state == 4 and val == 0x55:
            self.state = 5
        elif self.state == 5 and val in (0x30, 0x10):
            self.erase(off, val == 0x10)
            self.state = 0
        else:
            self.state = 0

    def erase(self, off, chip):
        if chip:
            self.mem[:] = b'\xff' * len(self.mem)
        else:
            base = off & ~0xffff
            self.mem[base:base + 0x10000] = b'\xff' * 0x10000


# --------------------------------------------------------------------------
class Mbar:
    """MCF5407 internal peripherals at MBAR = 0x10000000 (offsets per the MCF5307/5407 manuals)."""
    IPR, IMR, AVCR = 0x40, 0x44, 0x4B
    ICR0 = 0x4C  # ICR0..: SWT, TIMER0, TIMER1, I2C, UART0, UART1, DMA0..3

    def __init__(self, emu):
        self.emu = emu
        self.r = bytearray(0x1000)
        self.r[0x44:0x48] = b'\xff\xff\xff\xfe'  # IMR: all masked at reset
        self.uart = {0x1C0: bytearray(), 0x200: bytearray()}
        self.timer_tcn = [0, 0]
        self.timer_pending = [False, False]
        self.i2c = I2C(emu)

    def get(self, off, size):
        return int.from_bytes(self.r[off:off + size], 'big')

    def set(self, off, size, v):
        self.r[off:off + size] = (v & ((1 << (8 * size)) - 1)).to_bytes(size, 'big')

    def read(self, uc, off, size, ud):
        v = self.get(off, size)
        # timers: TCN counts (driven by tick())
        for i, base in enumerate((0x140, 0x180)):
            if off == base + 0xC:
                v = self.timer_tcn[i] & 0xffff
            elif off == base + 0x11:
                v = self.r[off]
        for base in (0x1C0, 0x200):
            if off == base + 0x4:      # USR: TxRDY, TxEMP; RxRDY never
                v = 0x0C
            elif off == base + 0x14:   # UISR
                v = 0x01 & self.r[base + 0x14]
        if 0x280 <= off < 0x2A0:
            v = self.i2c.read(off, size, v)
        self.emu.log.once(('MR', off,), lambda: f'MBAR R +{off:03x}/{size} = {v:x} pc={self.emu.pc():x}')
        return v

    def write(self, uc, off, size, val, ud):
        self.emu.log.once(('MW', off,), lambda: f'MBAR W +{off:03x}/{size} = {val:x} pc={self.emu.pc():x}')
        for base in (0x1C0, 0x200):
            if off == base + 0xC:
                self.uart[base].append(val & 0xff)
                if val in (10, 13) or len(self.uart[base]) > 200:
                    line = self.uart[base].decode('latin-1').strip()
                    if line:
                        print(f'UART{0 if base == 0x1C0 else 1}: {line}')
                    self.uart[base].clear()
                return
        for i, base in enumerate((0x140, 0x180)):
            if off == base + 0x11:   # TER: write 1 to clear
                self.r[off] &= ~val & 0xff
                return
            if off == base + 0xC:
                self.timer_tcn[i] = 0
                return
        if 0x280 <= off < 0x2A0:
            self.i2c.write(off, size, val)
        self.set(off, size, val)

    def tick(self, cycles):
        """Advance the two general-purpose timers by `cycles` bus clocks."""
        for i, base in enumerate((0x140, 0x180)):
            tmr = self.get(base, 2)
            if not tmr & 1 or (tmr >> 1) & 3 == 0:
                continue
            pre = (tmr >> 8) + 1
            if (tmr >> 1) & 3 == 2:
                pre *= 16
            trr = self.get(base + 4, 2)
            ticks = cycles // pre
            if ticks <= 0:
                continue
            n = self.timer_tcn[i] + ticks
            if trr and n >= trr:
                self.r[base + 0x11] |= 2  # REF
                n = (n % (trr + 1)) if tmr & 8 else n & 0xffff
                if tmr & 0x10:
                    self.timer_pending[i] = True
            self.timer_tcn[i] = n & 0xffff

    def pending_irq(self):
        """Highest-level pending internal interrupt not masked: (level, vector) or None."""
        imr = self.get(self.IMR, 4)
        best = None
        sources = []
        # IMR bits (MCF5307/5407): 1..7 external IRQ1..7, then 8 + n for ICRn:
        # 8 SWT, 9 TIMER0, 10 TIMER1, 11 I2C, 12 UART0, 13 UART1, 14-17 DMA0-3
        for i, bit in ((0, 9), (1, 10)):
            if self.r[(0x140, 0x180)[i] + 0x11] & 2 and self.get((0x140, 0x180)[i], 2) & 0x10:
                sources.append((bit, self.ICR0 + 1 + i))
        # external IRQ lines (autovectored when their AVCR bit is set): the ISP1181 on IRQ3
        avcr = self.r[self.AVCR]
        for level in self.emu.external_irqs():
            if not imr & (1 << level) and avcr & (1 << level):
                if best is None or level > best[0]:
                    best = (level, 24 + level)
        for (bit, icr_off) in sources:
            if imr & (1 << bit):
                continue
            icr = self.r[icr_off]
            level = (icr >> 2) & 7
            if level == 0:
                continue
            vec = 24 + level if icr & 0x80 else None
            if vec is None:
                continue
            if best is None or level > best[0]:
                best = (level, vec)
        return best


# --------------------------------------------------------------------------
class Isp1181:
    """Philips ISP1181 USB device controller on CS3 (command port +0x10, data port +0),
    as the OS drives it [C] (0x30053C38 interrupt handler, helpers 0x30053E72..0x300547FE).

    Commands: B0 unlock, B2/B3 scratch, B4 frame, B5 chip ID, B6/B7 address, B8/B9 mode,
    BA/BB hardware config, C0 interrupt register (4 bytes, LSB first), C2/C3 interrupt
    enable, F0-F3 DMA, F4 acknowledge setup, F6 reset; per endpoint index i (0 = EP0 OUT,
    1 = EP0 IN, 2.. = EP1..EP14): 0x00+i write buffer (length LE16, data), 0x10+i read
    buffer, 0x20+i/0x30+i write/read config, 0x40+i stall, 0x50+i read status (clears its
    interrupt bit), 0x60+i validate, 0x70+i clear, 0x80+i unstall, 0xD0+i check status.
    Interrupt register: bit 0 bus reset, ..., bit 8+i endpoint i. (Bit meanings from the
    datasheet as remembered: inferred.)

    The host side: `out(i, data)` queues host->device packets (a SETUP packet with
    setup=True on EP0 OUT); validated IN buffers are handed to `on_in(i, data)`."""

    def __init__(self, emu):
        self.emu = emu
        self.cmd = None
        self.wbuf = []
        self.rbuf = collections.deque()
        self.intreg = 0
        self.inten = 0
        self.epcfg = [0] * 16
        self.outq = [collections.deque() for _ in range(16)]
        self.outbuf = [None] * 16      # (data, setup)
        self.inbuf = [None] * 16
        self.on_in = None
        self.held = [[] for _ in range(16)]
        self.bulk_in = collections.deque()

    def poll_bulk_in(self):
        """An IN token on bulk-IN: the oldest validated packet, then its completion interrupt."""
        if not self.bulk_in:
            return None
        self.intreg |= 1 << (8 + 3)
        return self.bulk_in.popleft()

    def attach(self, on_in):
        """A host starts polling: IN buffers validated so far are taken."""
        self.on_in = on_in
        for i in range(16):
            for d in self.held[i]:
                on_in(i, d)
                self.intreg |= 1 << (8 + i)
            self.held[i] = []

    def irq(self):
        return bool(self.intreg & self.inten)

    def out(self, i, data, setup=False):
        self.outq[i].append((bytes(data), setup))
        self._load(i)

    def _load(self, i):
        if self.outbuf[i] is None and self.outq[i]:
            self.outbuf[i] = self.outq[i].popleft()
            self.intreg |= 1 << (8 + i)

    def bus_reset(self):
        self.intreg |= 1

    def read(self, uc, off, size, ud):
        v = 0
        for _ in range(size):
            v = (v << 8) | (self.rbuf.popleft() if (off & 0x10) == 0 and self.rbuf else 0)
        return v

    def write(self, uc, off, size, val, ud):
        for k in range(size):
            b = (val >> (8 * (size - 1 - k))) & 0xff
            if off & 0x10:
                self.command(b)
            else:
                self.data(b)

    def command(self, c):
        self.cmd = c
        self.wbuf = []
        self.rbuf.clear()
        i = c & 0x0f
        if c == 0xC0:
            v = self.intreg
            self.rbuf.extend([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff])
            self.intreg &= ~0xff  # bus events clear on read; endpoint bits on status read
        elif c == 0xB5:
            self.rbuf.extend([0x81, 0x81])  # (inferred) chip ID 0x8181
        elif c in (0xB4, 0xB3, 0xBB):
            self.rbuf.extend([0, 0])
        elif c in (0xB7, 0xB9):
            self.rbuf.append(0)
        elif c == 0xC3:
            v = self.inten
            self.rbuf.extend([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff])
        elif 0x10 <= c <= 0x1F:
            data, setup = self.outbuf[i] or (b'', False)
            self.rbuf.extend([len(data) & 0xff, len(data) >> 8])
            self.rbuf.extend(data)
        elif 0x30 <= c <= 0x3F:
            self.rbuf.append(self.epcfg[i])
        elif 0x50 <= c <= 0x5F or 0xD0 <= c <= 0xDF:
            st = 0
            if self.outbuf[i] is not None:
                st |= 0x20 | (0x04 if self.outbuf[i][1] else 0)  # EPFULL0, SETUPT
            self.rbuf.append(st)
            if c < 0xD0:
                self.intreg &= ~(1 << (8 + i))
        elif 0x60 <= c <= 0x6F:  # validate an IN buffer: "sent" at once once a host listens
            data = bytes(self.wbuf_in(i))
            if i == 3:
                # bulk-IN: the host polls it only while it expects a message (UsbHost.step)
                self.bulk_in.append(data)
            elif self.on_in:
                self.on_in(i, data)
                self.intreg |= 1 << (8 + i)
            else:
                self.held[i].append(data)  # no host: the buffer stays full, no completion
        elif 0x70 <= c <= 0x7F:  # clear an OUT buffer: next packet
            self.outbuf[i] = None
            self.intreg &= ~(1 << (8 + i))
            self._load(i)
        elif c == 0xF6:
            self.intreg = 0
        elif c == 0xF4 and self.outbuf[0] is not None and self.outbuf[0][1]:
            # acknowledge setup: the SETUP packet is consumed
            self.outbuf[0] = None
            self._load(0)
        self.emu.log.once(('USBC', c), lambda: f'USB cmd {c:02x}')

    def wbuf_in(self, i):
        b = self.inbuf[i] or []
        self.inbuf[i] = None
        if len(b) >= 2:
            n = b[0] | (b[1] << 8)
            return b[2:2 + n]
        return []

    def data(self, b):
        c = self.cmd
        if c is None:
            return
        self.wbuf.append(b)
        if 0x00 <= c <= 0x0F:
            self.inbuf[c] = list(self.wbuf)
        elif 0x20 <= c <= 0x2F:
            self.epcfg[c & 0x0f] = b
            self.emu.log.once(('USBEP', c, b), lambda: f'USB endpoint {c & 15} config {b:02x}')
        elif c == 0xC2 and len(self.wbuf) == 4:
            self.inten = self.wbuf[0] | (self.wbuf[1] << 8) | (self.wbuf[2] << 16) | (self.wbuf[3] << 24)
            self.emu.log.once(('USBIE', self.inten), lambda: f'USB interrupt enable {self.inten:08x}')


class UsbHost:
    """Our editor's protocol client (emu/protobridge, proto::Client) on the other end
    of the cable: bulk-OUT frames go to the OUT endpoint, interrupt-IN packets and
    bulk-IN transfers come back (re/notes/usb-protocol.md: 0x81 interrupt IN, 0x82
    bulk IN, 0x03 bulk OUT = ISP1181 indexes 2, 3, 4 (inferred))."""
    EP_INT, EP_BULK_IN, EP_BULK_OUT = 2, 3, 4

    def __init__(self, emu, lib_path):
        import ctypes
        self.ct = ctypes
        lib = ctypes.CDLL(lib_path)
        vp, i, u8p = ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)
        for name, res, args in (('g2p_create', vp, []), ('g2p_arrive', None, [vp]),
                                ('g2p_interrupt', None, [vp, ctypes.c_char_p, i]),
                                ('g2p_bulk_in', None, [vp, ctypes.c_char_p, i]),
                                ('g2p_take_out', i, [vp, ctypes.c_char_p, i]),
                                ('g2p_tick', None, [vp, ctypes.c_uint64]), ('g2p_status', i, [vp]),
                                ('g2p_synced', i, [vp]), ('g2p_idle', i, [vp]),
                                ('g2p_status_line', None, [vp, ctypes.c_char_p, i]),
                                ('g2p_send_patch', i, [vp, i, ctypes.c_char_p, ctypes.c_char_p])):
            f = getattr(lib, name)
            f.restype, f.argtypes = res, args
        self.lib = lib
        self.h = lib.g2p_create()
        self.emu = emu
        self.usb = emu.usb
        self.bulk = bytearray()
        self.expect = 0
        self.announce = b''
        self.ints = collections.deque()
        self.ms = 0.0
        self.started = False
        self.traffic = []

    def start(self):
        """Plug in: bus reset now; SETUPs and the client follow in step(), once the OS
        has handled the reset (its reset handler re-initialises the endpoints and
        clears their buffers, so anything queued with the reset is lost)."""
        self.started = True
        self.age = 0
        self.usb.attach(self.on_in)
        self.usb.bus_reset()

    def on_in(self, i, data):
        self.traffic.append(('in', i, data.hex()))
        if len(self.traffic) <= 60:
            print(f'USB in  ep{i}: {data.hex()}', flush=True)
        if i == self.EP_INT:
            self.ints.append(data)

    def step(self, ms):
        if not self.started:
            return
        self.age += 1
        if self.age == 50:
            # what a host does before using the pipes: SET_ADDRESS 1, SET_CONFIGURATION 1
            self.usb.out(0, bytes([0x00, 0x05, 0x01, 0, 0, 0, 0, 0]), setup=True)
            self.usb.out(0, bytes([0x00, 0x09, 0x01, 0, 0, 0, 0, 0]), setup=True)
        if self.age == 150:
            self.lib.g2p_arrive(self.h)
        if self.age < 150:
            return
        # interrupt-IN is polled all the time; bulk-IN only while an extended
        # announcement (b0 & 3 == 1, BE16 length) waits for its data (empty packets skipped)
        while True:
            if self.expect:
                d = self.usb.poll_bulk_in()
                if d is None:
                    break
                self.traffic.append(('in', 3, d.hex()))
                self.bulk += d
                if len(self.bulk) >= self.expect or (d and len(d) < 64):
                    self.lib.g2p_interrupt(self.h, self.announce, len(self.announce))
                    self.lib.g2p_bulk_in(self.h, bytes(self.bulk), len(self.bulk))
                    if len(self.traffic) <= 80:
                        print(f'USB in  bulk: {bytes(self.bulk).hex()}', flush=True)
                    self.bulk, self.expect = bytearray(), 0
                continue
            if not self.ints:
                break
            p = self.ints.popleft()
            if p[0] & 3 == 1:
                self.announce, self.expect = p, (p[1] << 8) | p[2]
            else:
                self.lib.g2p_interrupt(self.h, p, len(p))
        self.ms += ms
        self.lib.g2p_tick(self.h, int(self.ms))
        buf = self.ct.create_string_buffer(65536)
        while True:
            n = self.lib.g2p_take_out(self.h, buf, 65536)
            if not n:
                break
            frame = buf.raw[:n]
            self.traffic.append(('out', frame.hex()))
            if len(self.traffic) <= 60:
                print(f'USB out: {frame.hex()}', flush=True)
            for k in range(0, n, 64):
                self.usb.out(self.EP_BULK_OUT, frame[k:k + 64])
            if n % 64 == 0:
                self.usb.out(self.EP_BULK_OUT, b'')

    def status(self):
        buf = self.ct.create_string_buffer(200)
        self.lib.g2p_status_line(self.h, buf, 200)
        return buf.value.decode()


class I2C:
    """MCF5407 I2C master; every addressed slave acknowledges, reads return 0xFF
    unless a device model supplies data. Transactions are logged."""

    def __init__(self, emu):
        self.emu = emu
        self.msr = 0x81  # ICF | RXAK
        self.mcr = 0
        self.trans = []
        self.cur = None

    def read(self, off, size, v):
        if off == 0x28C:
            return self.msr
        if off == 0x288:
            return self.mcr
        if off == 0x290:
            # receive: hand out a byte and complete the next transfer. 0x80 (mid-scale):
            # the OS servoes DSP timer 0 until a histogram of these readings peaks at 0x80
            # (0x30055F28); a constant 0x80 satisfies it in 3 rounds instead of 10 (stub).
            self.msr |= 0x82
            return 0x80
        return v

    def write(self, off, size, val):
        if off == 0x288:
            start = (val & 0x20) and not (self.mcr & 0x20)
            stop = (self.mcr & 0x20) and not (val & 0x20)
            rsta = val & 0x04
            self.mcr = val & ~0x04
            if start or rsta:
                self.msr |= 0x20   # IBB
                self.cur = []
                self.trans.append(self.cur)
            if stop:
                self.msr &= ~0x20
                if self.cur is not None:
                    print('I2C', ' '.join(f'{b:02x}' for b in self.cur))
                self.cur = None
        elif off == 0x28C:
            self.msr = (self.msr & ~0x12) | (val & 0x12)
            self.msr = val | (self.msr & 0xa0) if False else (self.msr & ~0x02) | (val & 0x02)
        elif off == 0x290:
            if self.cur is not None:
                self.cur.append(val & 0xff)
            self.msr = (self.msr | 0x82) & ~0x01  # ICF, IIF, ack


# --------------------------------------------------------------------------
class Emu:
    def __init__(self, fw, trace=False, dsps=0):
        self.log = Log(trace)
        self.log.emu = self
        code = open(os.path.join(fw, 'os-v162-unpacked/CODE_30000400.bin'), 'rb').read()
        sram = open(os.path.join(fw, 'os-v162-unpacked/SRAM_20000800.bin'), 'rb').read()
        boot = open(os.path.join(fw, 'mac-updater-rsrc/BOOT/128_Loader.bin'), 'rb').read()
        self.code = code
        uc = self.uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        uc.ctl_set_cpu_model(UC_CPU_M68K_CFV4E)
        uc.mem_map(0, 0x80000)
        uc.mem_write(0, boot)
        uc.mem_map(0x20000000, 0x1000)
        uc.mem_write(SRAM_BASE, sram)
        uc.mem_map(0x30000000, 0x400000)
        uc.mem_write(CODE_BASE, code)

        self.mbar = Mbar(self)
        uc.mmio_map(0x10000000, 0x1000, self.mbar.read, None, self.mbar.write, None)
        self.host = HostPorts(self, dsps=dsps)
        uc.mmio_map(0x11000000, 0x1000, self.host.read, None, self.host.write, None)
        self.flash = Flash(self)
        uc.mmio_map(0x12000000, 0x800000, self.flash.read, None, self.flash.write, None)
        self.latch = collections.defaultdict(int)
        self.latch_reads = collections.defaultdict(lambda: 0)
        self.usb = Isp1181(self)
        uc.mmio_map(0x13000000, 0x10000, self.usb.read, None, self.usb.write, None)
        for cs in (4, 5, 6, 7):
            r, w = self._latch(cs)
            uc.mmio_map(0x10000000 + cs * 0x1000000, 0x10000, r, None, w, None)
        uc.mem_map(self.STUB_BASE, 0x4000)  # interrupt entry stubs (see interrupt())
        uc.hook_add(UC_HOOK_BLOCK, self._block)
        self.budget = 0
        self.stubs = {}
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._intr)
        # MOVEC to RAMBAR0/1 and MBAR: not in QEMU; skip them.
        for off in range(0, len(code) - 4, 2):
            if code[off:off + 2] == b'\x4e\x7b' and (code[off + 3] & 0xf0) == 0 and code[off + 2] & 0x0f == 0x0c:
                a = CODE_BASE + off
                uc.hook_add(UC_HOOK_CODE, self._movec, begin=a, end=a)
        self.stopped = False
        self.progress = 0  # print the PC every n slices
        self.debug_flash = False
        self.trace_ring = None
        self.exceptions = 0

    def pc(self):
        return self.uc.reg_read(UC_M68K_REG_PC)

    def external_irqs(self):
        return (3,) if self.usb.irq() else ()

    def _latch(self, cs):
        def rd(uc, off, size, ud):
            v = self.latch_in(cs, off, size)
            self.log.once(('LR', cs, off,), lambda: f'CS{cs} R +{off:x}/{size} = {v:x} pc={self.pc():x}')
            return v

        def wr(uc, off, size, val, ud):
            self.latch[(cs, off)] = val
            self.log.once(('LW', cs, off,), lambda: f'CS{cs} W +{off:x}/{size} = {val:x} pc={self.pc():x}')
        return rd, wr

    def latch_in(self, cs, off, size):
        return 0

    def _unmapped(self, uc, access, addr, size, value, ud):
        print(f'UNMAPPED access={access} addr={addr:x} size={size} value={value:x} pc={self.pc():x}')
        self.backtrace()
        return False

    def _intr(self, uc, intno, ud):
        # Unicorn hands every m68k exception to this hook and performs none of them,
        # not even RTE (QEMU's EXCP_RTE, 0x100): pop the ColdFire frame here. Writing
        # SR is safe (it is reading it that is not, see interrupt()).
        if intno == 0x100:
            sp = uc.reg_read(UC_M68K_REG_A7)
            fmt, pc = struct.unpack('>II', uc.mem_read(sp, 8))
            uc.reg_write(UC_M68K_REG_A7, sp + 8 + ((fmt >> 28) & 3))
            uc.reg_write(UC_M68K_REG_SR, fmt & 0xffff)
            uc.reg_write(UC_M68K_REG_PC, pc)
            return
        print(f'CPU exception {intno} at pc={self.pc():x}')
        self.exceptions += 1

    def block_trace(self):
        """Keep the last 200 basic-block addresses; dump them when execution reaches
        the boot flash area (0..0x7FFFF), which the OS never runs."""
        if self.trace_ring is not None:
            return
        self.trace_ring = collections.deque(maxlen=200)
        self.uc.hook_add(UC_HOOK_BLOCK, lambda uc, a, sz, ud: self.trace_ring.append(a))

        def low(uc, a, sz, ud):
            print('reached low memory; last blocks:', ' '.join(f'{x:x}' for x in self.trace_ring))
            print('regs', ' '.join(f'{n}={uc.reg_read(r):x}' for n, r in (
                ('a6', UC_M68K_REG_A0 + 6), ('a7', UC_M68K_REG_A7), ('a0', UC_M68K_REG_A0), ('d0', UC_M68K_REG_D0))))
            sp = uc.reg_read(UC_M68K_REG_A7)
            print('stack', bytes(uc.mem_read(sp, 64)).hex())
            uc.emu_stop()
            self.exceptions = 1000
        self.uc.hook_add(UC_HOOK_CODE, low, begin=0, end=0x7FFFF)

    def backtrace(self):
        """Registers and the call stack through the `link a6` frame chain (gcc frames)."""
        uc = self.uc
        regs = [('d%d' % i, UC_M68K_REG_D0 + i) for i in range(8)] + [('a%d' % i, UC_M68K_REG_A0 + i) for i in range(8)]
        print('regs', ' '.join(f'{n}={uc.reg_read(r):08x}' for n, r in regs))
        a6 = uc.reg_read(UC_M68K_REG_A0 + 6)
        chain = []
        for _ in range(16):
            if not 0x30000000 <= a6 < 0x303FFFF8:
                break
            try:
                nxt, ret = struct.unpack('>II', uc.mem_read(a6, 8))
            except UcError:
                break
            chain.append(ret)
            a6 = nxt
        print('calls', ' <- '.join(f'{x:08x}' for x in chain))

    def add_watch(self, addr, length):
        def wr(uc, access, a, size, value, ud):
            print(f'watch: write {a:08x}/{size} = {value:x} pc={self.pc():08x} slice={self.slices}')
        self.uc.hook_add(UC_HOOK_MEM_WRITE, wr, begin=addr, end=addr + length - 1)

    def add_probe(self, addr):
        def hit(uc, a, sz, ud):
            r = {n: uc.reg_read(x) for n, x in (('d0', UC_M68K_REG_D0), ('d2', UC_M68K_REG_D0 + 2),
                                              ('d3', UC_M68K_REG_D0 + 3), ('a2', UC_M68K_REG_A0 + 2))}
            try:
                mem = bytes(uc.mem_read(r['a2'], 16)).hex()
            except UcError:
                mem = '?'
            print(f'probe {a:08x} slice={self.slices} ' + ' '.join(f'{k}={v:x}' for k, v in r.items()) + f' (a2)={mem}')
        self.uc.hook_add(UC_HOOK_CODE, hit, begin=addr, end=addr)

    def add_break(self, addr):
        def hit(uc, a, sz, ud):
            print(f'break at {a:08x}')
            self.backtrace()
            uc.emu_stop()
            self.exceptions = 1000
        self.uc.hook_add(UC_HOOK_CODE, hit, begin=addr, end=addr)

    def _movec(self, uc, addr, size, ud):
        uc.reg_write(UC_M68K_REG_PC, addr + 4)

    # Interrupt entry runs as guest code. Unicorn's m68k SR read evaluates the lazy
    # condition codes wrongly after a stop and writes them back (a following Bcc
    # then goes the wrong way), so SR is never read from here. Instead the return
    # PC is pushed and a stub at STUB_BASE runs:
    #   lea -12(sp),sp; movem.l d0-d1,(sp); move.w sr,d0; move.l d0,d1
    #   andi.l #$700,d1; cmpi.l #level<<8,d1; bcs.b take
    #   move.w d0,sr; movem.l (sp),d0-d1; lea 12(sp),sp; rts        (masked: back, untouched)
    # take: andi.l #$ffff,d0; ori.l #fmt/vector,d0; move.l d0,8(sp)
    #   movem.l (sp),d0-d1; lea 8(sp),sp; move.w #$2000|level<<8,sr; jmp handler
    # leaving the ColdFire exception frame (format/vector/SR, PC) for the handler's RTE.
    STUB_BASE = 0x0F000000

    def _stub(self, level, vector, handler):
        fmtvec = (4 << 28) | (vector << 18)
        code = struct.pack('>HH HH H H HI HI H', 0x4FEF, 0xFFF4, 0x48D7, 0x0003, 0x40C0, 0x2200,
                           0x0281, 0x0700, 0x0C81, level << 8, 0x650C)
        code += struct.pack('>H HH HH H', 0x46C0, 0x4CD7, 0x0003, 0x4FEF, 0x000C, 0x4E75)
        code += struct.pack('>HI HI HH HH HH HH HI', 0x0280, 0xFFFF, 0x0080, fmtvec, 0x2F40, 0x0008,
                            0x4CD7, 0x0003, 0x4FEF, 0x0008, 0x46FC, 0x2000 | (level << 8), 0x4EF9, handler)
        return code

    def interrupt(self, level, vector):
        """Request an interrupt: taken by the stub if the mask allows, else retried later."""
        uc = self.uc
        handler = struct.unpack('>I', uc.mem_read(0x30000000 + 4 * vector, 4))[0]  # VBR = 0x30000000 [C]
        key = (level, vector, handler)
        addr = self.STUB_BASE + vector * 0x40
        if self.stubs.get(vector) != key:
            uc.mem_write(addr, self._stub(level, vector, handler))
            self.stubs[vector] = key
        sp = uc.reg_read(UC_M68K_REG_A7) - 4
        uc.mem_write(sp, struct.pack('>I', self.pc()))
        uc.reg_write(UC_M68K_REG_A7, sp)
        uc.reg_write(UC_M68K_REG_PC, addr)
        return True

    def start(self):
        self.uc.reg_write(UC_M68K_REG_SR, 0x2700)
        self.uc.reg_write(UC_M68K_REG_A7, 0x30400000)
        self.uc.reg_write(UC_M68K_REG_PC, CODE_BASE)
        self.slices = 0

    def _block(self, uc, addr, size, ud):
        self.budget -= max(1, size // 3)  # about one instruction per 3 bytes
        if self.budget <= 0:
            uc.emu_stop()

    def run(self, steps, slice_instr=20000, dsp_instr=20000, hist=None, until=None):
        """Run about `steps` instructions in slices; between slices: timers, DSPs,
        interrupts, `until(emu)` (stops when true).

        Slices end at translation-block boundaries, counted by a block hook. Ending
        them by instruction count (Unicorn's `count`) or by time (`timeout`) stops
        mid-block, where QEMU has not stored its lazy condition-code state: a Bcc
        after the stop could go the wrong way (seen: a counter wrap skipped at
        0x30027260), and timeouts re-executed MMIO writes."""
        done = 0
        while done < steps:
            self.budget = slice_instr
            try:
                self.uc.emu_start(self.pc(), 0xffffffff)
            except UcError as e:
                print(f'emulation error {e} at pc={self.pc():x}')
                return False
            done += slice_instr
            self.slices += 1
            self.mbar.tick(slice_instr)
            for p in self.host.ports:
                if p.dsp is not None:
                    p.dsp.run(dsp_instr)
            if hist is not None or self.progress:
                pc = self.pc()
                if hist is not None:
                    hist[pc] += 1
                if self.progress and self.slices % self.progress == 0:
                    print(f'[slice {self.slices}] pc={pc:08x}', flush=True)
            irq = self.mbar.pending_irq()
            if irq:
                self.interrupt(*irq)
            if until is not None and until(self):
                break
            if self.exceptions >= 1000:
                return False
            if self.exceptions > 20:
                print(f'stopping: repeated CPU exceptions (last pc={self.pc():x})')
                return False
        return True


# --------------------------------------------------------------------------
# Serial audio between the DSPs (re/notes/g2-hardware-and-emulation.md 3.7).
# Index n here = the DSP on chip select A(3+n) = OS DSP 3-n. From the stage-1
# programs [C]: A6 (n=3) is the clock master of the 8-slot frames and receives
# the ADCs on its ESAI (2 slots of 32 bits); A3 (n=0) sends the DACs on its ESAI
# (2 slots of 32 bits); every other ESAI side is an 8-slot, 24-bit TDM slave.
# Every DSP passes the buffer it received on to its transmitters two frames
# later, so the DSPs form a chain on ESAI (TX0/TX1 -> RX0/RX1), ending in A3's
# DACs, and a ring on ESAI_1 (TX2/TX3 -> RX0/RX1), back to A6. The order of A5
# and A4 along it is not given by the code: OS order is assumed (--chain).
FRAME_RATE = 96000          # frames (samples) per second, as the OS's pitch tables assume
CYCLES_PER_TICK = 192       # 1536 DSP clocks per frame / 8 slots [C]
# Stage 1's background loop [C]: P:$222-$22A spins while the frame counter X:$43 <= 3
# (every 4th frame it falls through into the patch's control-rate code). Same on all four.
IDLE_LOOP = (0x222, 0x22A, 0x43, 3)


def add_audio_args(ap):
    ap.add_argument('--chain', default='3,2,1,0',
                    help='DSP indexes (chip select A3+n) along the serial chain, first = clock master/ADC, '
                         'last = DACs (default: OS order A6,A5,A4,A3)')
    ap.add_argument('--no-links', action='store_true', help='no serial links (receivers get silence)')
    ap.add_argument('--ring-prefill', type=int, default=4,
                    help='frames of head start on the ESAI_1 line that closes the ring (last -> first DSP)')
    ap.add_argument('--link-queue', type=int, default=8, help='frames a sender may run ahead of its receiver')
    ap.add_argument('--link-batch', type=int, default=2,
                    help='a receiver that finds its line empty waits for this many frames (or 0.5 ms)')
    ap.add_argument('--throttle', type=float, default=0.0,
                    help='limit the DACs to this many times real time (0: as fast as the DSPs go)')
    ap.add_argument('--no-idle-skip', action='store_true',
                    help='run the DSPs\' idle background loop instead of skipping to the next slot')
    ap.add_argument('--jit', action='store_true', help='experimental, does not work yet: run the DSPs with dsp56300\'s JIT')


def wire_audio(dsps, args):
    """Clocks and links of the four DSPs; returns {name: link index}."""
    lib = DspBridge.lib
    chain = [int(x) for x in args.chain.split(',')]
    first, last = chain[0], chain[-1]
    for n, d in enumerate(dsps):
        # the converters' ESAI sides have 2 slots per frame: 4 ticks per slot
        lib.g2dsp_clock(d.h, CYCLES_PER_TICK, 3 if n == last else 0, 3 if n == first else 0, 0, 0)
        if not args.no_idle_skip:
            lib.g2dsp_idle_loop(d.h, *IDLE_LOOP)
        if args.jit:
            lib.g2dsp_jit(d.h, 1)
    links = {}
    if args.no_links:
        return links
    for a, b in zip(chain, chain[1:]):
        links[f'esai {a}->{b}'] = lib.g2dsp_link(dsps[a].h, 0, 0, dsps[b].h, 0, 0, args.link_queue, args.link_batch)
        links[f'esai1 {a}->{b}'] = lib.g2dsp_link(dsps[a].h, 1, 2, dsps[b].h, 1, 0, args.link_queue, args.link_batch)
    links[f'esai1 {last}->{first}'] = lib.g2dsp_link(dsps[last].h, 1, 2, dsps[first].h, 1,
                                                      args.ring_prefill, args.link_queue, args.link_batch)
    if args.throttle > 0:
        lib.g2dsp_throttle(dsps[last].h, args.throttle, FRAME_RATE)
    return links


class AudioRecorder:
    """Records the DACs (the last DSP's ESAI transmit frames) and reports speed."""

    def __init__(self, dsps, links, args, host_cpu=None):
        self.dsps = dsps
        self.links = links
        self.last = [int(x) for x in args.chain.split(',')][-1]
        self.host_cpu = host_cpu or (lambda: 0.0)
        self.samples = []
        self.report = {}

    def frames(self):
        return self.dsps[self.last].tx_frames(0)

    def snapshot(self):
        return {'wall': time.time(), 'host_cpu': self.host_cpu(), 'dac': self.frames(),
                'dsp': [(d.instructions(), d.skipped(), d.thread_cpu_ns(), d.waited_ns()) for d in self.dsps]}

    def start(self, frames):
        self.t0 = self.snapshot()
        self.want = frames
        DspBridge.lib.g2dsp_sink_record(self.dsps[self.last].h, frames)

    def poll(self):
        """Collects recorded words; True once all frames are in."""
        import ctypes
        buf = (ctypes.c_int32 * 65536)()
        while True:
            n = DspBridge.lib.g2dsp_sink_take(self.dsps[self.last].h, 0, buf, 65536)
            if not n:
                break
            self.samples.extend(buf[:n])
        return len(self.samples) >= 4 * self.want

    def stop(self):
        t1 = self.snapshot()
        t0 = self.t0
        wall = t1['wall'] - t0['wall']
        emulated = (t1['dac'] - t0['dac']) / FRAME_RATE
        rep = {'wall_s': wall, 'emulated_s': emulated, 'speed': emulated / wall if wall else 0,
               'host_cpu_s': t1['host_cpu'] - t0['host_cpu'], 'dsps': []}
        for n, (a, b) in enumerate(zip(t0['dsp'], t1['dsp'])):
            ins, skipped, cpu, wait = (y - x for x, y in zip(a, b))
            frames = emulated * FRAME_RATE
            rep['dsps'].append({'index': n, 'instructions': ins, 'skipped': skipped, 'cpu_s': cpu / 1e9,
                                'waited_s': wait / 1e9, 'mips': (ins - skipped) / wall / 1e6 if wall else 0,
                                'instructions_per_frame': ins / frames if frames else 0,
                                'executed_per_frame': (ins - skipped) / frames if frames else 0})
        rep['links'] = {k: {f: DspBridge.lib.g2dsp_link_stats(v, i) for i, f in enumerate(
            ('sent', 'received', 'dropped', 'zeros', 'timeouts', 'queued', 'active'))} for k, v in self.links.items()}
        self.report = rep
        print(f'audio: {emulated:.3f} emulated s in {wall:.2f} wall s = {rep["speed"]:.3f}x real time; '
              f'host thread CPU {rep["host_cpu_s"]:.2f} s', flush=True)
        for d in rep['dsps']:
            print(f'  A{3 + d["index"]}: {d["mips"]:.1f} M instr/s executed, {d["instructions_per_frame"]:.0f} '
                  f'instr/frame of which {d["executed_per_frame"]:.0f} executed (rest: idle skipped), '
                  f'thread CPU {d["cpu_s"]:.2f} s, waiting on links {d["waited_s"]:.2f} s', flush=True)
        for k, v in rep['links'].items():
            print(f'  link {k}: ' + ', '.join(f'{a} {b}' for a, b in v.items()), flush=True)

    def run(self, settle, seconds, step=None):
        """Waits `settle` emulated seconds, then records `seconds`. `step()` is called while
        waiting (the host CPU's slices, in the full machine); without it this sleeps."""
        target = self.frames() + int(settle * FRAME_RATE)
        while self.frames() < target:
            step() if step else time.sleep(0.01)
        self.start(int(seconds * FRAME_RATE))
        last = time.time()
        while not self.poll():
            step() if step else time.sleep(0.01)
            if time.time() - last > 5:
                last = time.time()
                print(f'  recording: {len(self.samples) // 4}/{self.want} frames; ' + '; '.join(
                    f'A{3 + n}: pc {d.pc():06x} tx {d.tx_frames(0)}/{d.tx_frames(1)} rx {d.rx_frames(0)}/{d.rx_frames(1)}'
                    for n, d in enumerate(self.dsps)), flush=True)
        self.stop()

    def save(self, out, name='dac'):
        """WAV of the 4 DAC channels (float32, 96 kHz) and a spectral check of each."""
        import numpy as np
        x = np.array(self.samples[:4 * self.want], dtype=np.float64).reshape(-1, 4) / 8388608.0
        # word order per frame: slot 0 TX0, slot 0 TX1, slot 1 TX0, slot 1 TX1
        # = DAC 1 left, DAC 2 left, DAC 1 right, DAC 2 right (inferred: outputs 1, 3, 2, 4)
        chans = x[:, [0, 2, 1, 3]]
        path = os.path.join(out, name + '.wav')
        write_wav(path, chans.astype(np.float32), FRAME_RATE)
        self.report['wav'] = path
        self.report['outputs'] = [analyse(chans[:, c]) for c in range(4)]
        for c, a in enumerate(self.report['outputs']):
            print(f'  out {c + 1}: ' + ', '.join(f'{k} {v}' for k, v in a.items()), flush=True)
        with open(os.path.join(out, name + '.json'), 'w') as f:
            json.dump(self.report, f, indent=1)


def write_wav(path, data, rate):
    """float32 WAV (format 3), interleaved channels."""
    n, ch = data.shape
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + data.nbytes) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 3, ch, rate, rate * 4 * ch, 4 * ch, 32) +
                b'data' + struct.pack('<I', data.nbytes))
        f.write(data.tobytes())


def analyse(x):
    """Peak, RMS, DC, the strongest frequency (parabolic peak of a Hann FFT) and its
    harmonics 2..5 relative to it, in dB."""
    import numpy as np
    out = {'peak': round(float(np.max(np.abs(x))), 6) if len(x) else 0.0}
    if len(x) < 1024 or out['peak'] < 1e-7:
        return out
    out['rms'] = round(float(np.sqrt(np.mean(x ** 2))), 6)
    out['dc'] = round(float(np.mean(x)), 7)
    w = np.hanning(len(x))
    s = np.abs(np.fft.rfft((x - np.mean(x)) * w))
    k = int(np.argmax(s[1:])) + 1
    d = 0.0
    if 1 <= k < len(s) - 1:
        a, b, c = np.log(s[k - 1] + 1e-30), np.log(s[k] + 1e-30), np.log(s[k + 1] + 1e-30)
        if a - 2 * b + c != 0:
            d = 0.5 * (a - c) / (a - 2 * b + c)
    f0 = (k + d) * FRAME_RATE / len(x)
    out['freq_hz'] = round(float(f0), 3)

    def level(f):
        j = int(round(f * len(x) / FRAME_RATE))
        lo, hi = max(1, j - 3), min(len(s), j + 4)
        return float(np.max(s[lo:hi])) if lo < hi else 0.0
    ref = level(f0)
    for h in range(2, 6):
        if h * f0 < FRAME_RATE / 2:
            out[f'H{h}_db'] = round(20 * np.log10(level(h * f0) / ref + 1e-30), 1)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fw', default=os.path.join(ROOT, 'original', 'firmware'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'original', 'firmware', 'dsp-boot'))
    ap.add_argument('--steps', type=int, default=600_000_000, help='ColdFire instructions to run (about)')
    ap.add_argument('--slice', type=int, default=20000, help='ColdFire instructions per slice')
    ap.add_argument('--dsp-slice', type=int, default=20000, help='unused: the DSPs run freely on their own threads')
    ap.add_argument('--patch', help='after boot, connect our protocol client over the emulated USB and upload this .pch2')
    ap.add_argument('--usb-start', type=int, default=12000, help='slice at which the USB cable is plugged in')
    ap.add_argument('--ms-per-slice', type=float, default=0.125, help='client clock per slice (ms)')
    ap.add_argument('--patch-to', action='append', default=[], metavar='SLOT:FILE',
                    help='after the first patch, also upload FILE to slot A-D (repeatable)')
    ap.add_argument('--audio-slices', type=int, default=1000,
                    help='slices to run after the upload when nothing is recorded (--seconds 0)')
    ap.add_argument('--seconds', type=float, default=0.5,
                    help='after the upload, record this many emulated seconds of the DACs (0: none)')
    ap.add_argument('--settle', type=float, default=0.05,
                    help='emulated seconds to let the DSPs run after the upload before recording')
    ap.add_argument('--capture-esai', action='store_true',
                    help='also record every DSP\'s ESAI transmit slots after the upload (slow)')
    ap.add_argument('--protobridge', default=os.path.join(ROOT, 'build', 'emu', 'libg2protobridge.dylib'))
    ap.add_argument('--break', dest='brk', action='append', help='stop at this PC (hex) and print a backtrace')
    ap.add_argument('--probe', action='append', help='print registers and 16 bytes at a2 when this PC (hex) runs')
    ap.add_argument('--watch', action='append', help='log writes to ADDR[:LEN] (hex)')
    ap.add_argument('--debug-flash', action='store_true', help='trace blocks once flash programming starts')
    ap.add_argument('--progress', type=int, default=0, help='print the PC every n slices')
    ap.add_argument('--trace-mmio', action='store_true', help='log each MMIO access kind once per PC')
    ap.add_argument('--dsps', type=int, default=4, help='emulated DSPs on the host ports (0: stubs)')
    ap.add_argument('--bridge', default=os.path.join(ROOT, 'build', 'emu', 'libg2dspbridge.dylib'))
    add_audio_args(ap)
    args = ap.parse_args(argv)
    if args.dsps:
        DspBridge.load(args.bridge)
    emu = Emu(args.fw, trace=args.trace_mmio, dsps=args.dsps)
    emu.audio_links = {}
    if args.dsps == 4:
        emu.audio_links = wire_audio([p.dsp for p in emu.host.ports[:4]], args)
    hist = collections.Counter()
    emu.progress = args.progress
    for w in args.watch or []:
        a, _, n = w.partition(':')
        emu.add_watch(int(a, 16), int(n or '4', 16))
    for b in args.probe or []:
        emu.add_probe(int(b, 16))
    for b in args.brk or []:
        emu.add_break(int(b, 16))
    emu.debug_flash = args.debug_flash
    emu.start()
    if args.patch:
        usb_session(emu, args)
        if args.dsps:
            DspBridge.lib.g2dsp_shutdown_all()
        return 0
    else:
        emu.run(steps=args.steps, slice_instr=args.slice,
                dsp_instr=args.dsp_slice, hist=hist)
    print(f'stopped at pc={emu.pc():x}')
    for k, v in hist.most_common(12):
        print(f'  {k:08x} {v}')
    save(emu, args.out)
    return 0


def words_bin(ws):
    return b''.join((w & 0xffffff).to_bytes(3, 'big') for w in ws)


def usb_session(emu, args):
    """Boot, plug in our protocol client, wait for its sync, upload a patch to slot A (and
    more with --patch-to), then let the DSPs run and record the DACs (--seconds)."""
    host = UsbHost(emu, args.protobridge)
    ms = args.ms_per_slice
    state = {'phase': 'boot', 'n': 0}
    uploads = [('A', args.patch)] + [tuple(x.split(':', 1)) for x in args.patch_to]
    dsps = [p.dsp for p in emu.host.ports if p.dsp is not None]
    rec = AudioRecorder(dsps, emu.audio_links, args, host_cpu=time.thread_time) if len(dsps) == 4 else None

    def until(em):
        st = state
        st['n'] += 1
        if st['phase'] == 'boot' and em.slices >= args.usb_start:
            print(f'[slice {em.slices}] USB: device plugged in', flush=True)
            host.start()
            st['phase'] = 'sync'
        host.step(ms)
        if (st['phase'] == 'sync' and host.lib.g2p_synced(host.h)) or (st['phase'] == 'upload' and
                                                                     host.lib.g2p_idle(host.h) and uploads):
            slot, path = uploads.pop(0)
            print(f'[slice {em.slices}] USB: {host.status()}; sending {path} to slot {slot}', flush=True)
            rc = host.lib.g2p_send_patch(host.h, 'ABCD'.index(slot.upper()), path.encode(),
                                         os.path.basename(path)[:-5].encode())
            if rc:
                print('USB: patch not sent', rc)
                return True
            st['phase'] = 'upload'
        elif st['phase'] == 'upload' and host.lib.g2p_idle(host.h):
            for p in em.host.ports:
                if p.dsp is not None and args.capture_esai:
                    p.dsp.capture(1)
            if rec and args.seconds > 0:
                st['phase'] = 'settle'
                st['settle_end'] = rec.frames() + int(args.settle * FRAME_RATE)
                print(f'[slice {em.slices}] USB: upload done; DACs at frame {rec.frames()}', flush=True)
            else:
                print(f'[slice {em.slices}] USB: upload done; running {args.audio_slices} more slices', flush=True)
                st['phase'] = 'audio'
                st['audio_end'] = em.slices + args.audio_slices
        elif st['phase'] == 'settle' and rec.frames() >= st['settle_end']:
            print(f'[slice {em.slices}] recording {args.seconds} s of the DACs', flush=True)
            rec.start(int(args.seconds * FRAME_RATE))
            st['phase'] = 'record'
        elif st['phase'] == 'record' and st['n'] % 50 == 0 and rec.poll():
            rec.stop()
            return True
        elif st['phase'] == 'audio' and em.slices >= st['audio_end']:
            return True
        if em.slices % 2500 == 0:
            print(f'[slice {em.slices}] USB {st["phase"]}: {host.status()}, '
                  f'{sum(1 for t in host.traffic if t[0] == "out")} out / {sum(1 for t in host.traffic if t[0] == "in")} in'
                  + (f'; DAC frames {rec.frames()}' if rec else ''), flush=True)
        return False

    emu.run(steps=args.steps, slice_instr=args.slice, dsp_instr=args.dsp_slice, until=until)
    os.makedirs(args.out, exist_ok=True)
    if rec and rec.report:
        rec.save(args.out)
    save(emu, args.out)
    snapshot_buffers(emu, args.out)
    with open(os.path.join(args.out, 'usb_traffic.txt'), 'w') as f:
        for t in host.traffic:
            f.write(' '.join(str(x) for x in t) + '\n')
    if args.capture_esai:
        save_audio(emu, args.out)


def snapshot_buffers(emu, out, count=40):
    """Read the DSPs' audio buffers (X:$1C00-$1FFF, the DMA areas of stage 1) and the
    zero page X/Y:$0-$FF a number of times while they run (they run on their own
    threads; reads are not synchronised with them)."""
    import time
    snaps = {}
    for k in range(count):
        for p in emu.host.ports:
            if p.dsp is None:
                continue
            d = p.dsp
            snaps.setdefault(p.index, []).append({
                'x1c00': [d.mem_read(1, a) for a in range(0x1C00, 0x2000)],
                'x0': [d.mem_read(1, a) for a in range(0, 0x100)],
                'y0': [d.mem_read(2, a) for a in range(0, 0x100)],
                'frames': d.mem_read(1, 0x43)})
        time.sleep(0.002)
    with open(os.path.join(out, 'buffers.json'), 'w') as f:
        json.dump(snaps, f)
    # whole internal memories (P $0-$FFF, X/Y $0-$1FFF) of each DSP as it runs now,
    # for emu/dspframe (g2dspframe), which runs a DSP's frame program offline
    for p in emu.host.ports:
        if p.dsp is None:
            continue
        for sp, area, n in (('P', 0, 0x1000), ('X', 1, 0x2000), ('Y', 2, 0x2000)):
            with open(os.path.join(out, f'dsp{p.index}_live_{sp}.bin'), 'wb') as f:
                f.write(words_bin([p.dsp.mem_read(area, a) for a in range(n)]))
        with open(os.path.join(out, f'dsp{p.index}_live_Yext.bin'), 'wb') as f:
            f.write(words_bin([p.dsp.mem_read(2, a) for a in range(0x800000, 0x800800)]))


def save_audio(emu, out):
    """Per DSP and ESAI: the captured transmit slots as 24-bit words; per slot a WAV
    (float, 96 kHz assumed) and a pitch estimate from zero crossings."""
    import ctypes
    import wave
    for p in emu.host.ports:
        if p.dsp is None:
            continue
        p.dsp.capture(0)
        for esai in (0, 1):
            buf = (ctypes.c_uint32 * (1 << 22))()
            words = []
            while True:
                n = DspBridge.lib.g2dsp_tx_take(p.dsp.h, esai, buf, 1 << 22)
                if not n:
                    break
                words += buf[:n]
            frames, k = [], 0
            while k < len(words):
                cnt = words[k]
                if k + 1 + 6 * cnt > len(words):
                    break
                frames.append([words[k + 1 + 6 * j: k + 7 + 6 * j] for j in range(cnt)])
                k += 1 + 6 * cnt
            if not frames:
                continue
            slots = max(len(f) for f in frames)
            for sl in range(slots):
                for reg in range(6):
                    x = [((f[sl][reg] ^ 0x800000) - 0x800000) / 8388608.0 if sl < len(f) else 0.0 for f in frames]
                    if not any(abs(v) > 1e-6 for v in x):
                        continue
                    zc = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0))
                    peak = max(abs(v) for v in x)
                    name = f'dsp{p.index}_esai{esai}_slot{sl}_tx{reg}'
                    print(f'{name}: {len(x)} frames, peak {peak:.4f}, {zc} zero crossings '
                          f'(~{zc / 2 * 96000 / len(x):.1f} Hz at 96 kHz frames)')
                    with wave.open(os.path.join(out, name + '.wav'), 'wb') as w:
                        w.setnchannels(1)
                        w.setsampwidth(2)
                        w.setframerate(96000)
                        w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(v * 32767)))) for v in x))


def save(emu, out):
    """Per DSP n: dspN.events (the host-port stream), dspN_boot_P.bin (the program
    loaded through the boot ROM), dspN.json (decoded monitor writes: ranges per memory
    space) and, from the running emulated DSP, dspN_{P,X,Y}.bin images of the ranges
    the host wrote (24-bit big-endian words). Nothing here is ours: keep it out of git."""
    os.makedirs(out, exist_ok=True)
    summary = {}
    for p in emu.host.ports:
        if not p.events:
            continue
        n = p.index
        with open(os.path.join(out, f'dsp{n}.events'), 'w') as f:
            for kind, v in p.events:
                f.write(f'{kind} {v:06x}\n')
        boot, mem, log = decode_stream(p.events)
        info = {'events': len(p.events), 'words': sum(1 for e in p.events if e[0] == 'w')}
        if boot:
            info['boot'] = {'address': boot[0], 'words': len(boot[1])}
            with open(os.path.join(out, f'dsp{n}_boot_P.bin'), 'wb') as f:
                f.write(words_bin(boot[1]))
        info['commands'] = dict(collections.Counter(f"{r['hv'] * 2:02X}:{r['act']}" for r in log))
        info['writes'] = {sp: [[a, b] for a, b in runs(mem[sp].keys())] for sp in 'PXY'}
        if p.dsp is not None:
            d = p.dsp
            info['dsp'] = {'pc': d.pc(), 'boots': d.boot_count(), 'instructions': d.instructions()}
            area = {'P': 0, 'X': 1, 'Y': 2}
            for sp in 'PXY':
                img = {}
                for a, b in runs(mem[sp].keys()):
                    img[a] = [d.mem_read(area[sp], x) for x in range(a, b + 1)]
                    # the live image must hold what the stream wrote, unless the DSP changed it since
                    same = sum(1 for x in range(a, b + 1) if img[a][x - a] == mem[sp][x])
                    info.setdefault('live_matches', {})[f'{sp}:{a:06x}-{b:06x}'] = f'{same}/{b - a + 1}'
                with open(os.path.join(out, f'dsp{n}_{sp}.bin'), 'wb') as f:
                    for a in sorted(img):
                        f.write(a.to_bytes(4, 'big') + len(img[a]).to_bytes(4, 'big') + words_bin(img[a]))
        summary[f'dsp{n}'] = info
        print(f'DSP{n}: {info["words"]} words; boot {info.get("boot")}; writes ' +
              ', '.join(f'{sp} {len(v)} ranges' for sp, v in info['writes'].items()) +
              (f'; live pc={info["dsp"]["pc"]:06x}' if 'dsp' in info else ''))
    with open(os.path.join(out, 'summary.json'), 'w') as f:
        json.dump(summary, f, indent=1)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
