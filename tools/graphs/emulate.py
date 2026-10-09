#!/usr/bin/env python3
"""Run the original G2 editor's module graph drawing code in a CPU emulator.

Reads the user's own copy of the Mac editor binary (original/mac/G2Editor_i386,
Mach-O i386) at run time; nothing from that binary is stored in this script.
See re/notes/module-graphs.md.

For a graph id (PANL "Graph Func") and the dependency values, the script calls
CCustomObjectFactory::CreateCustom(id, ...) to build the graph object, then
its virtual UpdateGraphics() and Draw(). The drawing primitives
(NSSmuggler::CABCCanvas::DrawLine/DrawRect/DrawText/..., the column fills
CPnlGraphABC::FillUnderGraph/FillEnvelopeGraph) and the parameter access
(CPnlCustom::GetDependencyValue) are intercepted and recorded; everything in
between (the graph code itself, Bezier/ArcTo helpers, ...) runs unmodified.

Output, one primitive per line (coordinates are the original integers):
  L x0 y0 x1 y1 ink        line into the 200x100 scratch bitmap
  R x0 y0 x1 y1 ink        filled rectangle (left top right bottom)
  T x y ink text           text (Arial 9), x from GetTextWidth (see -w)
  FU w h / FE w h          FillUnderGraph / FillEnvelopeGraph over w x h
  BLIT x y sx sy w h       scratch (sx,sy,w,h) copied to the view at (x,y)
  FRAME x0 y0 x1 y1        CGraphGUI frame of the view rect
  SIZE w h                 view size set by the constructor

Usage (python3 -I, or a venv with `pip install unicorn`):
  emulate.py BINARY info                 graph id -> class, size, functions
  emulate.py BINARY draw ID V0 V1 ...    primitives for one set of values
  emulate.py BINARY batch FILE           one "ID V0 V1 ..." per line; prints
                                         "# ID V0 V1 ..." then the primitives
"""
import math
import os
import re
import struct
import sys

import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    'paramtext_emulate', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'paramtext', 'emulate.py'))
_pt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_pt)
MachO = _pt.MachO  # Mach-O loader of tools/paramtext/emulate.py

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_BLOCK  # noqa: E402
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_EIP,  # noqa: E402
                               UC_X86_REG_ESP)

STACK = 0x800000
STUB = 0x900000
RET = 0x900800
HEAP = 0xA00000
SCRATCH = 0x9F0000   # fake CBitmap32 used as CPnlGraphABC::fScratchPad
TEXTW_PER_CHAR = 5   # GetTextWidth model (Arial 9 digits are 5 px wide)

INKS = {  # CRGBColor values (16 bit per channel) of the graph colours
    (0, 0, 0): 'border',
    (0, 0xff00, 0): 'single',
    (0xc000, 0xc000, 0xc000): 'backline',
    (0x4b00, 0x6300, 0x6300): 'shade',
    (0, 0xff00, 0x8000): 'fill',
    (0, 0x8000, 0x8000): 'back',
    (0xff00, 0xff00, 0): 'base',   # kGraphBaseLineColor == kGraphSustainColor
    (0, 0xa400, 0xa400): 'envfill',
}
COLOR_SYMS = {
    '_kGraphBorderLineColor': (0, 0, 0), '_kGraphSingleLineColor': (0, 0xff00, 0),
    '_kGraphBackLineColor': (0xc000, 0xc000, 0xc000), '_kGraphShadeColor': (0x4b00, 0x6300, 0x6300),
    '_kGraphFillColor': (0, 0xff00, 0x8000), '_kGraphBackColor': (0, 0x8000, 0x8000),
    '_kGraphBaseLineColor': (0xff00, 0xff00, 0), '_kGraphSustainColor': (0xff00, 0xff00, 0),
    '_kGraphEnvFillColor': (0, 0xa400, 0xa400), '_kGraphFilterTextColor': (0xff00, 0xff00, 0),
    '__ZN5Color11kGraphFrameE': (0x6400, 0x6400, 0x6400),
}


# Graph Func ids whose class has its own Draw() (0 and unknown ids are a bare
# CPnlGraphABC; 2, 22, 25, 26, 27, 46 are interactive controls).
GRAPH_IDS = set(range(1, 46)) - {2, 22, 25, 26, 27}


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


class GraphEmu:
    def __init__(self, path):
        self.m = MachO(path)
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        top = max(a + s for _, a, s, _, _, _ in self.m.sections)
        uc.mem_map(0, (top + 0xfff) & ~0xfff)
        for _n, addr, size, off, _r1, _r2 in self.m.sections:
            if off and size:
                uc.mem_write(addr, self.m.data[off:off + size])
        uc.mem_map(STACK - 0x100000, 0x100000)
        uc.mem_map(STUB, 0x1000)
        uc.mem_write(STUB, b'\xDD\x05' + struct.pack('<I', STUB + 0x100) + b'\xC3')
        uc.mem_write(STUB + 0x10, b'\xD9\x05' + struct.pack('<I', STUB + 0x100) + b'\xC3')
        uc.mem_write(RET, b'\xF4')
        uc.mem_map(SCRATCH, 0x10000)
        uc.mem_map(HEAP, 0x400000)
        sym = self.m.symbols
        for name, rgb in COLOR_SYMS.items():
            uc.mem_write(sym[name], struct.pack('<3H', *rgb))
        uc.mem_write(sym['__ZN12CPnlGraphABC11fScratchPadE'], struct.pack('<I', SCRATCH))
        self.addr_name = {}
        for n, a in sym.items():
            if not n.endswith('.eh'):
                self.addr_name.setdefault(a, n)
        self.text_lo = min(a for n, a, s, _, _, _ in self.m.sections if n == '__text')
        self.text_hi = max(a + s for n, a, s, _, _, _ in self.m.sections if n == '__text')

        H = {}

        def by(name, fn):
            H[sym[name]] = fn
        # graph object plumbing
        by('__ZN10CPnlCustomC2EPN10NSSmuggler10CGroupViewEiih', self.h_noop)
        by('__ZN10CPnlCustomC1EPN10NSSmuggler10CGroupViewEiih', self.h_noop)
        by('__ZNK10CPnlCustom18GetDependencyValueEh', self.h_dep)
        by('__ZN10CPnlCustom19IsDirtyDependenciesEv', lambda: self._ret(1))
        by('__ZN10CPnlCustom22CleanDirtyDependenciesEv', self.h_noop)
        by('__ZN10NSSmuggler5CView10InvalidateEb', self.h_noop)
        by('__ZN10NSSmuggler5CView7SetSizeEss', self.h_setsize)
        by('__ZNK10NSSmuggler5CView13GetCanvasSpecEv', lambda: self._ret(0))
        by('__ZN10NSSmuggler7CCanvasC1EPNS_19CCanvasToWindowLinkE', self.h_noop)
        by('__ZN10NSSmuggler7CCanvasD1Ev', self.h_noop)
        by('__ZN9CGraphGUI4DrawERKN10NSSmuggler10CLocalRectE', self.h_frame)
        by('__ZNK10NSSmuggler10CABCBitmap7GetRectEv', self.h_bmrect)
        by('__ZN10NSSmuggler9CRGBColorD1Ev', self.h_noop)
        # primitives
        by('__ZN10NSSmuggler10CABCCanvas8DrawLineEssssRKNS_9CRGBColorE', self.h_line)
        by('__ZN10NSSmuggler10CABCCanvas8DrawRectEPKNS_10CLocalRectERKNS_9CRGBColorE', self.h_rect)
        by('__ZN10NSSmuggler10CABCCanvas9DrawFrameEPKNS_10CLocalRectERKNS_9CRGBColorE', self.h_frame_rect)
        by('__ZN10NSSmuggler10CABCCanvas8DrawTextEssPKcRKNS_9CFontSpecERKNS_9CRGBColorE', self.h_text)
        by('__ZN10NSSmuggler10CABCCanvas12GetTextWidthEPKcRKNS_9CFontSpecE', self.h_textw)
        by('__ZN10NSSmuggler10CABCCanvas10DrawBitmapEssPKNS_10CABCBitmapEssss', self.h_blit)
        by('__ZN12CPnlGraphABC14FillUnderGraphEtt', lambda: self.h_fill('FU'))
        by('__ZN12CPnlGraphABC17FillEnvelopeGraphEtt', lambda: self.h_fill('FE'))
        by('__ZN5Utils8IntToStrEl', self.h_inttostr)
        by('__ZN10NSSmuggler10CABCCanvas10DrawBitmapEssNS_9TBitmapIDEssss', self.h_bitmap)
        # number of dependency items (the CPnlCustom constructor is not run)
        by('__ZNKSt6vectorI15CDependencyItemSaIS0_EE4sizeEv', lambda: self._ret(len(self.values)))
        self.hooks = H
        self.imports = {
            '__ZdlPv': self.h_noop, '__Znwm': self.h_new,
            '__ZNSsD1Ev': self.h_noop,
            '_atan': lambda: self._ret_fpu(math.atan(self._d(0))),
            '_sin': lambda: self._ret_fpu(math.sin(self._d(0))),
            '_cos': lambda: self._ret_fpu(math.cos(self._d(0))),
            '_floor': lambda: self._ret_fpu(math.floor(self._d(0))),
            '_ceil': lambda: self._ret_fpu(math.ceil(self._d(0))),
            '_floorf': lambda: self._ret_fpu(math.floor(self._f(0)), True),
            '_ceilf': lambda: self._ret_fpu(math.ceil(self._f(0)), True),
            '___powidf2': lambda: self._ret_fpu(self._powi(self._d(0), struct.unpack('<i', struct.pack('<I', self._arg(2)))[0])),
            '_modf': self.h_modf,
            '_exp': lambda: self._ret_fpu(math.exp(self._d(0))),
            '_pow': lambda: self._ret_fpu(math.pow(self._d(0), self._d(2))),
            '_log': lambda: self._ret_fpu(math.log(self._d(0)) if self._d(0) > 0 else float('-inf')),
            '_log10': lambda: self._ret_fpu(math.log10(self._d(0)) if self._d(0) > 0 else float('-inf')),
            '_sqrt': lambda: self._ret_fpu(math.sqrt(self._d(0))),
            '_tan': lambda: self._ret_fpu(math.tan(self._d(0))),
            '_sinf': lambda: self._ret_fpu(math.sin(self._f(0)), True),
            '_cosf': lambda: self._ret_fpu(math.cos(self._f(0)), True),
            '_expf': lambda: self._ret_fpu(math.exp(self._f(0)), True),
            '_powf': lambda: self._ret_fpu(math.pow(self._f(0), self._f(1)), True),
            '_logf': lambda: self._ret_fpu(math.log(self._f(0)), True),
            '_rand': self.h_rand,
            '_random': self.h_rand,
        }
        lo = min(self.m.imports)
        hi = max(self.m.imports) + 5
        uc.hook_add(UC_HOOK_CODE, self._hook_import, begin=lo, end=hi)
        uc.hook_add(UC_HOOK_BLOCK, self._hook_block, begin=self.text_lo, end=self.text_hi)
        # hooked functions outside __text (e.g. coalesced template code)
        for a in H:
            if not self.text_lo <= a < self.text_hi:
                uc.hook_add(UC_HOOK_BLOCK, self._hook_block, begin=a, end=a)
        self.allowed = re.compile(
            r'Graph|CPnlFeedbackFlt|CCustomObjectFactory|CRect|CView7GetRect|CGraphGUIC|CPnlCustom12GetContainer|'
            r'CDependencyItem|9TBitmapID|CABCCanvas18DrawHorizontalLine|CABCCanvas16DrawVerticalLine|^__Z7(Sin2Saw|TriBell|Tri2Saw|CosBell)dd|CPnl\w*(Preset|Selector|SeqClr|Zoom|Offset)')
        self.values = []
        self.ops = []
        self.heap = HEAP
        self.rand_state = 1

    # --- helpers ---
    def _rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def _u32(self, a):
        return struct.unpack('<I', self._rd(a, 4))[0]

    def _arg(self, k):
        return self._u32(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k)

    def _sarg(self, k):
        return s16(self._arg(k))

    def _d(self, k):
        return struct.unpack('<d', self._rd(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k, 8))[0]

    def _f(self, k):
        return struct.unpack('<f', self._rd(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k, 4))[0]

    def _cstr(self, a):
        out = b''
        while True:
            c = self._rd(a, 1)
            if c == b'\0':
                return out
            out += c
            a += 1

    @staticmethod
    def _powi(x, n):
        r = 1.0
        neg = n < 0
        n = abs(n)
        while True:
            if n & 1:
                r *= x
            n >>= 1
            if not n:
                break
            x *= x
        return 1.0 / r if neg else r

    def _ret(self, eax, edx=None):
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        ra = self._u32(esp)
        self.uc.reg_write(UC_X86_REG_ESP, esp + 4)
        self.uc.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        if edx is not None:
            self.uc.reg_write(UC_X86_REG_EDX, edx & 0xffffffff)
        self.uc.reg_write(UC_X86_REG_EIP, ra)

    def _ret_fpu(self, val, single=False):
        self.uc.mem_write(STUB + 0x100, struct.pack('<f' if single else '<d', val))
        self.uc.reg_write(UC_X86_REG_EIP, STUB + (0x10 if single else 0))

    def ink(self, ptr):
        rgb = struct.unpack('<3H', self._rd(ptr, 6))
        return INKS.get(rgb, '%04x%04x%04x' % rgb)

    # --- hooks ---
    def h_noop(self):
        self._ret(0)

    def h_new(self):
        n = (self._arg(0) + 15) & ~15
        a = self.heap
        self.heap += n
        self.uc.mem_write(a, b'\0' * n)
        self._ret(a)

    def h_rand(self):
        self.rand_state = (self.rand_state * 1103515245 + 12345) & 0x7fffffff
        self._ret(self.rand_state)

    def h_modf(self):
        x = self._d(0)
        ip = self._arg(2)
        frac, whole = math.modf(x)
        self.uc.mem_write(ip, struct.pack('<d', whole))
        self._ret_fpu(frac)

    def h_dep(self):
        i = self._arg(1) & 0xff
        self.ops.append(('DEP', i))
        self._ret(self.values[i] if i < len(self.values) else 0)

    def h_setsize(self):
        this = self._arg(0)
        w, h = self._sarg(1), self._sarg(2)
        self.uc.mem_write(this + 10, struct.pack('<4h', 0, 0, w, h))
        self.ops.append(('SIZE', w, h))
        self._ret(0)

    def h_frame(self):
        r = struct.unpack('<4h', self._rd(self._arg(1), 8))
        self.ops.append(('FRAME',) + r)
        self._ret(0)

    def h_bmrect(self):
        self._ret(0, (100 << 16) | 200)

    def h_line(self):
        c = self._arg(0)
        self.ops.append(('L' if c == SCRATCH else 'VL', self._sarg(1), self._sarg(2), self._sarg(3), self._sarg(4),
                         self.ink(self._arg(5))))
        self._ret(0)

    def h_rect(self):
        c = self._arg(0)
        r = struct.unpack('<4h', self._rd(self._arg(1), 8))
        self.ops.append(('R' if c == SCRATCH else 'VR',) + r + (self.ink(self._arg(2)),))
        self._ret(0)

    def h_frame_rect(self):
        c = self._arg(0)
        r = struct.unpack('<4h', self._rd(self._arg(1), 8))
        self.ops.append(('FR' if c == SCRATCH else 'VFR',) + r + (self.ink(self._arg(2)),))
        self._ret(0)

    def h_text(self):
        c = self._arg(0)
        s = self._cstr(self._arg(3)).decode('mac_roman')
        self.ops.append(('T' if c == SCRATCH else 'VT', self._sarg(1), self._sarg(2), self.ink(self._arg(5)), s))
        self._ret(0)

    def h_textw(self):
        s = self._cstr(self._arg(1))
        self._ret(TEXTW_PER_CHAR * len(s))

    def h_blit(self):
        # DrawBitmap(view, x, y, fScratchPad, srcX, srcY, w, h)
        self.ops.append(('BLIT', self._sarg(1), self._sarg(2), self._sarg(4), self._sarg(5), self._sarg(6),
                         self._sarg(7)))
        self._ret(0)

    def h_bitmap(self):
        # DrawBitmap(canvas, x, y, TBitmapID const* (by reference), srcX, srcY, w, h)
        rid = self._u32(self._arg(3))
        self.ops.append(('BMP', rid, self._sarg(1), self._sarg(2), self._sarg(4), self._sarg(5),
                         self._sarg(6), self._sarg(7)))
        self._ret(0)

    def h_fill(self, kind):
        self.ops.append((kind, self._arg(1) & 0xffff, self._arg(2) & 0xffff))
        self._ret(0)

    def h_inttostr(self):
        out = self._arg(0)
        v = struct.unpack('<i', struct.pack('<I', self._arg(1)))[0]
        s = str(v).encode() + b'\0'
        a = self.heap
        self.heap += 32
        self.uc.mem_write(a, s)
        self.uc.mem_write(out, struct.pack('<I', a))
        self._ret(out)

    def _hook_import(self, _uc, addr, _size, _user):
        name = self.m.imports.get(addr)
        if name is None:
            return
        h = self.imports.get(name)
        if h is None:
            raise RuntimeError('unhandled import %s' % name)
        h()

    def _hook_block(self, _uc, addr, _size, _user):
        h = self.hooks.get(addr)
        if h is not None:
            h()
            return
        name = self.addr_name.get(addr)
        if name is not None and name.startswith('__Z') and not self.allowed.search(name):
            raise RuntimeError('unexpected call to %s (0x%x)' % (name, addr))

    def call(self, addr, args):
        sp = STACK - 0x1000 - 4 * len(args)
        for i, v in enumerate(args):
            self.uc.mem_write(sp + 4 * i, struct.pack('<I', v & 0xffffffff))
        sp -= 4
        self.uc.mem_write(sp, struct.pack('<I', RET))
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.emu_start(addr, RET, count=20000000)
        return self.uc.reg_read(UC_X86_REG_EAX)

    # --- graph objects ---
    def create(self, gid, ndeps):
        self.heap = HEAP
        self.ops = []
        obj = self.call(self.m.symbols['__ZN20CCustomObjectFactory12CreateCustomEiP6CPaneliih'],
                        [gid, 0, 0, 0, ndeps])
        return obj

    def vfunc(self, obj, suffix):
        vt = self._u32(obj)
        for k in range(120):
            a = self._u32(vt + 4 * k)
            n = self.addr_name.get(a, '')
            if n.endswith(suffix):
                return a, n
        return None, None

    def draw(self, gid, values):
        self.values = list(values)
        obj = self.create(gid, len(values))
        self.uc.mem_write(obj + 0x4c, b'\x01')
        up, _ = self.vfunc(obj, '14UpdateGraphicsEv')
        dr, _ = self.vfunc(obj, '4DrawEv')
        if up:
            self.call(up, [obj])
        ops = []
        self.ops = []
        if dr and gid in GRAPH_IDS:
            self.call(dr, [obj])
        ops = self.ops
        return ops


def fmt(op):
    return ' '.join(str(x) for x in op)


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    emu = GraphEmu(argv[1])
    cmd = argv[2]
    if cmd == 'info':
        for gid in range(0, 48):
            try:
                obj = emu.create(gid, 16)
            except Exception as e:  # non-graph customs (buttons, selectors)
                print(gid, 'error', e)
                continue
            size = [o for o in emu.ops if o[0] == 'SIZE']
            vt = emu._u32(obj)
            up = emu.vfunc(obj, '14UpdateGraphicsEv')
            dr = emu.vfunc(obj, '4DrawEv')
            print(gid, emu.addr_name.get(emu._u32(vt + 0), '?'), size[-1][1:] if size else None,
                  '%s@%s' % (up[1], hex(up[0]) if up[0] else None), '%s@%s' % (dr[1], hex(dr[0]) if dr[0] else None))
    elif cmd == 'draw':
        gid = int(argv[3])
        for op in emu.draw(gid, [int(x) for x in argv[4:]]):
            print(fmt(op))
    elif cmd == 'batch':
        for line in open(argv[3]):
            parts = line.split()
            if not parts:
                continue
            nums = [int(x) for x in parts]
            print('#', ' '.join(parts))
            for op in emu.draw(nums[0], nums[1:]):
                if op[0] != 'DEP':
                    print(fmt(op))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
