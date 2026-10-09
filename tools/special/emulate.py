#!/usr/bin/env python3
"""Run the original G2 editor's special panel controls in a CPU emulator.

Reads the user's own copy of the Mac editor binary (original/mac/G2Editor_i386,
Mach-O i386) at run time; nothing from that binary is stored in this script.
See re/notes/special-controls.md. The controls' own code runs unmodified in
unicorn; the framework calls around it (containers, views, buttons, libc
rand) are replaced by Python stubs.

Usage (python3 -I, or a venv with `pip install unicorn`):
  emulate.py BINARY vocoder OP [SEED]   CPnlVocoderPreset::GetRequestedChange for
                                        bands 0..15; OP 0..6 = -2 -1 0 +1 +2 Inv Rnd;
                                        SEED is the Rnd_GetC state (gRandC)
  emulate.py BINARY window              CSeqSliderGUI zoom/offset state for every
                                        zoom 0..2, offset 0..9: low high range res topcorr
  emulate.py BINARY click Z O Y         CPnlSeqSlider::OnClick at local y -> value
  emulate.py BINARY clicks              every zoom/offset, y = -4..95: "z o y value"
  emulate.py BINARY zoomclick Z         CPnlNoteSeqZoom::OnClick: new zoom
  emulate.py BINARY offset DIR O        CPnlNoteSeqOffset::HandleChangeRequests
                                        (DIR 1 = right, 2 = left): new offset
  emulate.py BINARY drumcmp V0..V14     CPnlDrumPresetSelector::PresetComparison:
                                        "index matched"
  emulate.py BINARY drumget INDEX       GetRequestedChange for deps 0..14
  emulate.py BINARY drumstep DIR I M    HandleChangeRequests (DIR 1 = up, 2 = down)
                                        with index I, matched M: "index matched applied"
  emulate.py BINARY drumnames           the preset names (fNames)
  emulate.py BINARY octs                CPnlNoteSeqOffset::fOcts
  emulate.py BINARY noteclr             CPnlNoteSeqClrRnd "Clr": 16 values
  emulate.py BINARY noternd LOW RANGE R...  CPnlNoteSeqClrRnd "Rnd" with the slider's
                                        low limit / range and rand() results R...
"""
import importlib.util
import os
import struct
import sys

_spec = importlib.util.spec_from_file_location(
    'paramtext_emulate', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'paramtext', 'emulate.py'))
_pt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_pt)
MachO = _pt.MachO

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP  # noqa: E402

STACK = 0x800000
RET = 0x900800
STUBS = 0x910000   # fake vtable targets, one per (container, slot)
HEAP = 0xA00000


class Emu:
    def __init__(self, path):
        self.m = MachO(path)
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        top = max(a + s for _, a, s, _, _, _ in self.m.sections)
        uc.mem_map(0, (top + 0xfff) & ~0xfff)
        for _name, addr, size, off, _r1, _r2 in self.m.sections:
            if off and size:
                uc.mem_write(addr, self.m.data[off:off + size])
        uc.mem_map(STACK - 0x100000, 0x100000)
        uc.mem_map(RET & ~0xfff, 0x1000)
        uc.mem_write(RET, b'\xF4')
        uc.mem_map(STUBS, 0x10000)
        uc.mem_write(STUBS, b'\xC3' * 0x10000)
        uc.mem_map(HEAP, 0x100000)
        self.hooks = {}
        self.next_stub = STUBS
        self.imports = {n: a for a, n in self.m.imports.items()}
        uc.hook_add(UC_HOOK_CODE, self._hook)
        # Rnd_GetC starts with "imul eax, [gRandC], 0x0bb38435" after its prologue.
        rnd = self.sym('__Z8Rnd_GetCv')
        code = self.rd(rnd + 3, 10)
        assert code[:2] == b'\x69\x05', code.hex()
        self.grandc = struct.unpack_from('<I', code, 2)[0]

    # memory
    def rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def u32(self, a):
        return struct.unpack('<I', self.rd(a, 4))[0]

    def w8(self, a, v):
        self.uc.mem_write(a, bytes([v & 0xff]))

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def r8(self, a):
        return self.rd(a, 1)[0]

    def cstr(self, a):
        out = b''
        while self.rd(a, 1) != b'\0':
            out += self.rd(a, 1)
            a += 1
        return out.decode('mac_roman')

    def sym(self, name):
        return self.m.symbols[name]

    def arg(self, k):
        return self.u32(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k)

    # hooks: fn(emu) -> eax; `pop` extra bytes for struct-return callees
    def hook(self, addr, fn, pop=0):
        self.hooks[addr] = (fn, pop)

    def hook_sym(self, name, fn, pop=0):
        self.hook(self.sym(name), fn, pop)

    def hook_import(self, name, fn):
        self.hook(self.imports[name], fn)

    def stub(self, fn, pop=0):
        a = self.next_stub
        self.next_stub += 0x10
        self.hook(a, fn, pop)
        return a

    def vtable(self, slots):
        """A fake vtable: {offset: fn}; returns its address."""
        vt = self.alloc(0x200)
        for off, fn in slots.items():
            self.w32(vt + off, self.stub(fn))
        return vt

    def reset(self):
        """Frees the fake heap and stubs (call before each emulated operation)."""
        self._heap = HEAP
        self.next_stub = STUBS

    def alloc(self, n):
        a = getattr(self, '_heap', HEAP)
        self._heap = a + ((n + 15) & ~15)
        self.uc.mem_write(a, b'\0' * n)
        return a

    def _hook(self, uc, addr, _size, _user):
        h = self.hooks.get(addr)
        if h is None:
            return
        fn, pop = h
        eax = fn(self) or 0
        esp = uc.reg_read(UC_X86_REG_ESP)
        ra = self.u32(esp)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        uc.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        uc.reg_write(UC_X86_REG_EIP, ra)

    def call(self, addr, args):
        sp = STACK - 0x2000 - 4 * len(args)
        for i, v in enumerate(args):
            self.w32(sp + 4 * i, v)
        sp -= 4
        self.w32(sp, RET)
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.emu_start(addr, RET, count=200000)
        return self.uc.reg_read(UC_X86_REG_EAX)


def s8(v):
    v &= 0xff
    return v - 256 if v >= 128 else v


# ---- CPnlVocoderPreset ----------------------------------------------------------

def vocoder(e, op, seed=0):
    e.reset()
    this = e.alloc(0x400)
    e.w8(this + 0x3d8 + op, 1)
    e.w8(this + 0x3df, 0)
    e.w32(e.grandc, seed)
    out = e.alloc(4)
    fn = e.sym('__ZN17CPnlVocoderPreset18GetRequestedChangeEhRh')
    vals = []
    for i in range(16):
        assert e.call(fn, [this, i, out]) & 0xff == 1
        vals.append(e.r8(out))
    return vals, e.u32(e.grandc)


# ---- CSeqSliderGUI / CPnlSeqSlider ------------------------------------------------

def gui(e, z, o):
    g = e.alloc(0x24)
    e.call(e.sym('__ZN13CSeqSliderGUIC1EPN10NSSmuggler5CViewE'), [g, 0])
    e.call(e.sym('__ZN13CSeqSliderGUI9SetOffsetEh'), [g, o])
    e.call(e.sym('__ZN13CSeqSliderGUI7SetZoomEh'), [g, z])
    return g


def window(e, z, o):
    e.reset()
    g = gui(e, z, o)
    low = s8(e.call(e.sym('__ZN13CSeqSliderGUI11GetLowLimitEv'), [g]))
    rng = e.call(e.sym('__ZN13CSeqSliderGUI8GetRangeEv'), [g]) & 0xff
    res = e.call(e.sym('__ZN13CSeqSliderGUI13GetResolutionEv'), [g]) & 0xff
    top = s8(e.call(e.sym('__ZN13CSeqSliderGUI10GetTopCorrEv'), [g]))
    return low, e.r8(g + 0x20), rng, res, top


def click(e, z, o, y):
    e.reset()
    g = gui(e, z, o)
    this = e.alloc(0x80)
    e.w32(this, e.vtable({0xe8: lambda e: 1}))  # MapToTargetArea: 1 = slider area
    e.w32(this + 0x54, g)
    rect = e.alloc(8)  # left 0, top 0
    written = []
    cont = e.alloc(16)
    e.w32(cont, e.vtable({
        0x10: lambda e: 0,
        0x34: lambda e: 0xff,  # current value: never equal, so the setter is called
        0x38: lambda e: 0,
        0x08: lambda e: written.append(e.arg(2) & 0xff),
        0x0c: lambda e: written.append(('rel', e.arg(2))),
    }))
    e.hook_sym('__ZN11CPnlControl13TryKnobAssignEv', lambda e: 0)
    e.hook_sym('__ZNK11CPnlControl12GetContainerEv', lambda e: cont)
    e.hook_sym('__ZN10NSSmuggler5CView10GetRectPtrEv', lambda e: rect)
    e.hook_sym('__ZN10NSSmuggler18CModifierKeyStatus16GetCurrentStatusEv', lambda e: e.arg(0), pop=4)
    e.hook_sym('__ZNK10NSSmuggler18CModifierKeyStatus17IsDragCopyKeyDownEv', lambda e: 0)
    e.hook_sym('__ZN10NSSmuggler18CModifierKeyStatusD1Ev', lambda e: 0)
    e.hook_sym('__ZN11CPnlControl23ClickDragOnClickHandlerEss', lambda e: 0)
    e.hook_import('___dynamic_cast', lambda e: e.arg(0))
    e.call(e.sym('__ZN13CPnlSeqSlider7OnClickEss'), [this, 0, y & 0xffff])
    assert len(written) == 1, written
    return written[0]


# ---- CPnlNoteSeqZoom / CPnlNoteSeqOffset ----------------------------------------------

def zoomclick(e, z):
    e.reset()
    this = e.alloc(0x80)
    e.w8(this + 0x76, z)
    cont = e.alloc(16)
    e.w32(cont, e.vtable({4: lambda e: 0}))
    e.hook_sym('__ZNK10CPnlCustom12GetContainerEv', lambda e: cont)
    e.hook_sym('__ZN10NSSmuggler5CView10InvalidateEb', lambda e: 0)
    e.call(e.sym('__ZN15CPnlNoteSeqZoom7OnClickEss'), [this, 0, 0])
    return e.r8(this + 0x76)


def offset(e, d, o):
    e.reset()
    this = e.alloc(0x140)
    e.w8(this + 0x136, o)
    e.w32(this + 0xe4 + 0x3c, d)
    e.w8(this + 0xe4 + 0x40, 1)
    cont = e.alloc(16)
    e.w32(cont, e.vtable({4: lambda e: 0}))
    e.hook_sym('__ZNK10CPnlCustom12GetContainerEv', lambda e: cont)
    e.hook_sym('__ZN16CIncDecButtonABC8SetStateEN10PanelTypes14EClickedUpDownE', lambda e: 0)
    e.call(e.sym('__ZN17CPnlNoteSeqOffset20HandleChangeRequestsEv'), [this])
    return e.r8(this + 0x136)


def octs(e):
    e.reset()
    a = e.sym('__ZN17CPnlNoteSeqOffset5fOctsE')
    return [e.cstr(a + 4 * i) for i in range(10)]


# ---- CPnlDrumPresetSelector ----------------------------------------------------------

def drumcmp(e, values):
    e.reset()
    this = e.alloc(0x140)
    e.w8(this + 0x136, 0xee)
    e.hook_sym('__ZNK10CPnlCustom18GetDependencyValueEh', lambda e: values[e.arg(1) & 0xff])
    e.call(e.sym('__ZN22CPnlDrumPresetSelector16PresetComparisonEv'), [this])
    return e.r8(this + 0x136), e.r8(this + 0x137)


def drumget(e, index):
    e.reset()
    this = e.alloc(0x140)
    e.w8(this + 0x136, index)
    e.w8(this + 0x138, 0)
    out = e.alloc(4)
    fn = e.sym('__ZN22CPnlDrumPresetSelector18GetRequestedChangeEhRh')
    vals = []
    for i in range(15):
        assert e.call(fn, [this, i, out]) & 0xff == 1
        vals.append(e.r8(out))
    assert e.call(fn, [this, 15, out]) & 0xff == 0
    return vals


def drumstep(e, d, index, matched):
    e.reset()
    this = e.alloc(0x140)
    e.w8(this + 0x136, index)
    e.w8(this + 0x137, matched)
    e.w32(this + 0xe4 + 0x3c, d)
    e.w8(this + 0xe4 + 0x40, 1)
    applied = []
    cont = e.alloc(16)
    e.w32(cont, e.vtable({8: lambda e: applied.append(1)}))
    e.hook_sym('__ZNK10CPnlCustom12GetContainerEv', lambda e: cont)
    e.hook_sym('__ZN16CIncDecButtonABC8SetStateEN10PanelTypes14EClickedUpDownE', lambda e: 0)
    e.hook_sym('__ZN10NSSmuggler12CApplication3GetEv', lambda e: 0)
    e.hook_sym('__ZN10NSSmuggler12CApplication10ForceSetupEv', lambda e: 0)
    e.call(e.sym('__ZN22CPnlDrumPresetSelector20HandleChangeRequestsEv'), [this])
    return e.r8(this + 0x136), e.r8(this + 0x137), len(applied)


def drumnames(e):
    e.reset()
    a = e.sym('__ZN22CPnlDrumPresetSelector6fNamesE')
    return [e.cstr(a + 8 * i) for i in range(30)]


# ---- CPnlNoteSeqClrRnd ---------------------------------------------------------------

def noteclr(e):
    e.reset()
    this = e.alloc(0x170)
    e.w8(this + 0x16c, 1)
    e.w8(this + 0x16e, 0)
    out = e.alloc(4)
    fn = e.sym('__ZN17CPnlNoteSeqClrRnd18GetRequestedChangeEhRh')
    vals = []
    for i in range(16):
        assert e.call(fn, [this, i, out]) & 0xff == 1
        vals.append(e.r8(out))
    return vals


def noternd(e, low, rng, rands):
    e.reset()
    this = e.alloc(0x170)
    e.w8(this + 0x16d, 1)
    e.w8(this + 0x16e, 0)
    out = e.alloc(4)
    seq = list(rands)
    cont = e.alloc(16)
    e.w32(cont, e.vtable({0x1c: lambda e: 0x1234}))
    e.hook_sym('__ZNK10CPnlCustom12GetContainerEv', lambda e: cont)
    e.hook_sym('__ZNK10CPnlCustom18GetDependencyValueEh', lambda e: 64)
    e.hook_sym('__ZN13CPnlSeqSlider11GetLowLimitEv', lambda e: low)
    e.hook_sym('__ZN13CPnlSeqSlider8GetRangeEv', lambda e: rng)
    e.hook_import('___dynamic_cast', lambda e: e.arg(0))
    e.hook_import('_rand', lambda e: seq.pop(0))
    fn = e.sym('__ZN17CPnlNoteSeqClrRnd18GetRequestedChangeEhRh')
    vals = []
    for i in range(len(rands)):
        assert e.call(fn, [this, i, out]) & 0xff == 1
        vals.append(e.r8(out))
    return vals


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    e = Emu(argv[1])
    cmd, a = argv[2], [int(x, 0) for x in argv[3:]]
    if cmd == 'vocoder':
        vals, state = vocoder(e, a[0], a[1] if len(a) > 1 else 0)
        print(' '.join(map(str, vals)), '# gRandC', hex(state))
    elif cmd == 'window':
        for z in range(3):
            for o in range(10):
                print(z, o, *window(e, z, o))
    elif cmd == 'click':
        print(click(e, *a))
    elif cmd == 'clicks':
        for z in range(3):
            for o in range(10):
                for y in range(-4, 96):
                    print(z, o, y, click(e, z, o, y))
    elif cmd == 'zoomclick':
        print(zoomclick(e, a[0]))
    elif cmd == 'offset':
        print(offset(e, a[0], a[1]))
    elif cmd == 'drumcmp':
        print(*drumcmp(e, a))
    elif cmd == 'drumget':
        print(' '.join(map(str, drumget(e, a[0]))))
    elif cmd == 'drumstep':
        print(*drumstep(e, *a))
    elif cmd == 'drumnames':
        print(drumnames(e))
    elif cmd == 'octs':
        print(octs(e))
    elif cmd == 'noteclr':
        print(' '.join(map(str, noteclr(e))))
    elif cmd == 'noternd':
        print(' '.join(map(str, noternd(e, a[0], a[1], a[2:]))))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
