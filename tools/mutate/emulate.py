#!/usr/bin/env python3
"""Run the original G2 editor's Patch Mutator code in a CPU emulator and write
golden data for tests/test_mutate.cpp (re/notes/randomize-mutate.md).

Reads the user's own copy of the Mac editor binary (original/mac/G2Editor_i386,
Mach-O i386) at run time; nothing from that binary is stored in this script.

Runs, unmodified: CMutaSynthData::IsEnabled, CopyRandomizeContext,
CopyMutateContext, RecombineContext, ApplyCurve*/ApplySingleCurve/
ApplyDualCurve and SetMutation{Prob,Range}From{Range,Prob}. Their data
structures (CModuleParamData<double>, CMorphMapData_11, CModule, CPatch) are
replaced by Python stand-ins through hooks; random() is BSD random() seeded
with 1 (the editor never seeds it), pow() is Python's.

Usage (python3 -I, or a venv with `pip install unicorn`):
  emulate.py BINARY OUT.txt        tests/golden/mutate.txt
"""
import importlib.util
import json
import math
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
_spec = importlib.util.spec_from_file_location('paramtext_emulate', os.path.join(HERE, '..', 'paramtext', 'emulate.py'))
_pt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_pt)
MachO = _pt.MachO

STACK = 0x800000
STUB = 0x900000      # fldl [STUB+0x100]; ret
RET = 0x900800
FAKE = 0xB00000      # fake objects
END_NODE = 0xBFFFF0

# Function addresses (v1.62, G2Editor_i386).
A = {
    'IsEnabled': 0x141796, 'CopyRandomizeContext': 0x1425e8, 'CopyMutateContext': 0x142984,
    'RecombineContext': 0x142288, 'ApplyCurve': 0x1d199c, 'ApplyDualCurve': 0x1d1278,
    'ApplySingleCurve': 0x1d11ba, 'SetProbFromRange': 0x140fe4, 'SetRangeFromProb': 0x141050,
    # stand-ins
    'GetMutaSynthData': 0xd36e0, 'IsLocked': 0xa9d2c, 'GetType': 0xaa660, 'GetModule': 0x1d13b4,
    'ItCtor': 0x1a4b5a, 'ItIsAtEnd': 0x1a4c2c, 'DoesParamSettingExist': 0x1a4c9e,
    'DoesModuleExist': 0x1a4d28, 'AddModule': 0x1a5ace, 'AddParamSetting': 0x1a5b82,
    'SetParamValue': 0x1a5bfc, 'GetParamValue': 0x1a5c6c, 'RemoveParamSetting': 0x1d1d34,
    'MorphAddParamSetting': 0x661ca, 'MorphAddMorph': 0x674a8, 'MorphGetDelta': 0x690d4,
    'MorphIsMorphed': 0x6912a, 'MorphDoesParamSettingExist': 0x692f2,
}


class BsdRandom:
    """libc random() (TYPE_3), as tests check against macOS srandom/random."""

    def __init__(self, seed=1):
        s = [0] * 31
        s[0] = seed & 0xffffffff
        for i in range(1, 31):
            x = s[i - 1]
            x = x - (1 << 32) if x & 0x80000000 else x
            if x == 0:
                x = 123459876
            hi = int(x / 127773)
            lo = x - hi * 127773
            x = 16807 * lo - 2836 * hi
            if x < 0:
                x += 0x7fffffff
            s[i] = x & 0xffffffff
        self.s, self.f, self.r = s, 3, 0
        for _ in range(310):
            self.next()

    def next(self):
        s = self.s
        s[self.f] = (s[self.f] + s[self.r]) & 0xffffffff
        v = (s[self.f] >> 1) & 0x7fffffff
        self.f += 1
        if self.f >= 31:
            self.f = 0
            self.r += 1
        else:
            self.r += 1
            if self.r >= 31:
                self.r = 0
        return v


class MutaEmu:
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
        uc.mem_write(RET, b'\xF4')
        uc.mem_map(FAKE, 0x100000)
        self.rng = BsdRandom(1)
        self.guard = set()
        imports = {
            '_random': lambda: self._ret(self.rng.next()),
            '_pow': lambda: self._ret_fpu(self._pow(self._d(0), self._d(2))),
            '___cxa_guard_acquire': self.h_guard_acquire,
            '___cxa_guard_release': lambda: self._ret(0),
            '__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base': self.h_increment,
        }
        self.hooks = {}
        for a, n in self.m.imports.items():
            if n in imports:
                self.hooks[a] = imports[n]
        for name, fn in {
            'GetMutaSynthData': lambda: self._ret(self.muta),
            'IsLocked': lambda: self._ret(self.modules_by_ptr[self._arg(0)]['locked']),
            'GetType': lambda: self._ret(self.modules_by_ptr[self._arg(0)]['type']),
            'GetModule': self.h_get_module,
            'ItCtor': self.h_it_ctor,
            'ItIsAtEnd': lambda: self._ret(int(self._u32(self._arg(0) + 4) == END_NODE)),
            'DoesParamSettingExist': lambda: self._ret(int(self._arg(0) in self.sources)),
            'DoesModuleExist': lambda: self._ret(0),
            'AddModule': lambda: self._ret(0),
            'AddParamSetting': lambda: self._ret(0),
            'RemoveParamSetting': lambda: self._ret(0),
            'SetParamValue': self.h_set,
            'GetParamValue': self.h_get,
            'MorphAddParamSetting': lambda: self._ret(0),
            'MorphDoesParamSettingExist': lambda: self._ret(0),
            'MorphIsMorphed': lambda: self._ret(int(self._morph_key() in self.morphs.get(self._arg(0), {}))),
            'MorphGetDelta': lambda: self._ret(self.morphs[self._arg(0)][self._morph_key()] & 0xff),
            'MorphAddMorph': self.h_add_morph,
        }.items():
            self.hooks[A[name]] = fn
        for a in self.hooks:
            uc.hook_add(UC_HOOK_CODE, self._hook, begin=a, end=a)
        self.heap = FAKE
        self.muta = self._alloc(0x400)
        # loaded per scenario
        self.sources = {}       # CModuleParamData ptr -> (context, [(id, values)])
        self.modules = {}       # (context, id) -> dict(type, locked, ptr)
        self.modules_by_ptr = {}
        self.nodes = {}         # node ptr -> next node ptr
        self.node_owner = {}
        self.out = {}           # dest ptr -> {(id, param): value}
        self.morphs = {}        # morph map ptr -> {(ctx, id, param, group): delta}
        self.added_morphs = []
        self.infos = {}
        mods = json.load(open(os.path.join(ROOT, 'data', 'modules.json')))['modules']
        for md in mods:
            if md.get('typeId') is not None and md.get('moduleInfoSymbol') and md['params']:
                sym = '_' + md['moduleInfoSymbol'] if not md['moduleInfoSymbol'].startswith('_') else md['moduleInfoSymbol']
                if sym in self.m.symbols:
                    self.infos[md['typeId']] = (md, self._u32(self.m.symbols[sym] + 4))

    # --- helpers ---
    def _alloc(self, n):
        a = self.heap
        self.heap += (n + 15) & ~15
        self.uc.mem_write(a, b'\0' * n)
        return a

    def _rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def _u32(self, a):
        return struct.unpack('<I', self._rd(a, 4))[0]

    def _esp(self):
        return self.uc.reg_read(UC_X86_REG_ESP)

    def _arg(self, k):
        return self._u32(self._esp() + 4 + 4 * k)

    def _d(self, k):
        return struct.unpack('<d', self._rd(self._esp() + 4 + 4 * k, 8))[0]

    @staticmethod
    def _pow(x, y):
        try:
            return math.pow(x, y)
        except (ValueError, OverflowError):
            return float('nan')

    def _ret(self, eax):
        esp = self._esp()
        ra = self._u32(esp)
        self.uc.reg_write(UC_X86_REG_ESP, esp + 4)
        self.uc.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        self.uc.reg_write(UC_X86_REG_EIP, ra)

    def _ret_fpu(self, val):
        self.uc.mem_write(STUB + 0x100, struct.pack('<d', val))
        self.uc.reg_write(UC_X86_REG_EIP, STUB)

    def _hook(self, _uc, addr, _size, _user):
        self.hooks[addr]()

    def _run(self, addr, words):
        sp = STACK - 0x2000
        self.uc.mem_write(sp, b''.join(struct.pack('<I', w & 0xffffffff) for w in words))
        sp -= 4
        self.uc.mem_write(sp, struct.pack('<I', RET))
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.emu_start(addr, RET, count=5000000)
        return self.uc.reg_read(UC_X86_REG_EAX)

    def _run_fpu(self, addr, words):
        """Calls a function returning a double on the x87 stack."""
        self._run(addr, words)
        # fstp qword [STUB+0x200]; hlt
        code = b'\xDD\x1D' + struct.pack('<I', STUB + 0x200) + b'\xF4'
        self.uc.mem_write(STUB + 0x300, code)
        self.uc.emu_start(STUB + 0x300, STUB + 0x300 + len(code) - 1)
        return struct.unpack('<d', self._rd(STUB + 0x200, 8))[0]

    @staticmethod
    def _dw(x):
        return list(struct.unpack('<2I', struct.pack('<d', x)))

    # --- stand-ins ---
    def h_guard_acquire(self):
        g = self._arg(0)
        first = g not in self.guard
        self.guard.add(g)
        self._ret(int(first))

    def h_get_module(self):
        ctx, mid = self._arg(1), self._arg(2) & 0xff
        mod = self.modules.get((ctx, mid))
        self._ret(mod['ptr'] if mod else 0)

    def h_it_ctor(self):
        it, data = self._arg(0), self._arg(1)
        first = self.first_node.get(data, END_NODE)
        self.uc.mem_write(it + 4, struct.pack('<I', first))
        self._ret(it)

    def h_increment(self):
        self._ret(self.nodes[self._arg(0)])

    def h_get(self):
        data, mid, _setting, p = self._arg(0), self._arg(1) & 0xff, self._arg(2) & 0xff, self._arg(3) & 0xff
        self._ret_fpu(self.values[data][(mid, p)])

    def h_set(self):
        data, mid, _setting, p = self._arg(0), self._arg(1) & 0xff, self._arg(2) & 0xff, self._arg(3) & 0xff
        self.out.setdefault(data, {})[(mid, p)] = self._d(4)
        self._ret(0)

    def _morph_key(self):
        spec, grp = self._arg(2), self._arg(3)
        ctx = self._u32(spec)
        mid, p = self._rd(spec + 4, 2)
        return (ctx, mid, p, self._rd(grp, 1)[0])

    def h_add_morph(self):
        d = self._arg(4) & 0xff
        self.added_morphs.append(self._morph_key() + (d - 256 if d & 0x80 else d,))
        self._ret(0)

    # --- scenario setup ---
    def make_module(self, ctx, mid, typ, locked):
        _md, specs = self.infos[typ]
        builder = self._alloc(0x20)
        self.uc.mem_write(builder + 0x10, struct.pack('<I', specs))
        ptr = self._alloc(0x80)
        self.uc.mem_write(ptr, struct.pack('<I', builder))
        mod = {'type': typ, 'locked': int(locked), 'ptr': ptr}
        self.modules[(ctx, mid)] = mod
        self.modules_by_ptr[ptr] = mod

    def make_data(self, ctx, genes):
        """A CModuleParamData<double> stand-in; genes = [(id, [values])]."""
        data = self._alloc(0x80)
        self.uc.mem_write(data + 0x3c, struct.pack('<I', ctx))
        nodes = []
        for mid, vals in genes:
            n = self._alloc(0x20)
            self.uc.mem_write(n + 0x10, bytes([mid, len(vals)]))
            nodes.append(n)
        for a, b in zip(nodes, nodes[1:] + [END_NODE]):
            self.nodes[a] = b
        self.first_node[data] = nodes[0] if nodes else END_NODE
        self.sources[data] = True
        self.values[data] = {(mid, p): v for mid, vals in genes for p, v in enumerate(vals)}
        return data

    def reset(self):
        self.first_node = {}
        self.values = {}
        self.sources = {}
        self.out = {}
        self.added_morphs = []
        self.morphs = {}

    def set_muta(self, unlocked, solo):
        self.uc.mem_write(self.muta + 0x390, struct.pack('<2I', unlocked, solo))

    def dest(self):
        return self._alloc(0x80)


# The test patch: (context, id, type name, locked, values). Values are the
# parent's doubles; chosen to cover every curve, the partial-tune special case,
# locked modules, fixed classes, groups and the FX / Delay rules.
def scenario_modules(emu):
    by_name = {md['shortName']: t for t, (md, _s) in emu.infos.items()}
    spec = [
        (1, 1, 'OscB', False, {4: 3.4}),          # tune mode Partial
        (1, 2, 'OscB', False, {4: 0.6}),
        (1, 3, 'EnvADSR', False, {}),
        (1, 4, 'Mix4-1B', False, {}),
        (1, 5, 'SeqNote', False, {}),
        (1, 6, 'LfoC', False, {}),
        (1, 7, 'OscB', True, {}),                 # locked
        (1, 9, 'Operator', False, {}),
        (1, 10, 'DrumSynth', False, {}),
        (1, 12, 'FltNord', False, {}),
        (0, 1, 'Reverb', False, {}),
        (0, 2, 'DelayDual', False, {}),
        (0, 3, 'Compress', False, {}),
        (0, 4, 'EnvAHD', False, {}),
    ]
    out = []
    k = 0
    for ctx, mid, name, locked, override in spec:
        typ = by_name[name]
        md = emu.infos[typ][0]
        vals = []
        for p, pd in enumerate(md['params']):
            k += 1
            mx = pd['max']
            v = ((k * 37) % (mx + 1)) + ((k * 0.6180339887) % 1.0) * 0.999
            vals.append(override.get(p, v))
        out.append((ctx, mid, typ, locked, vals))
    return out


def father_of(mods):
    out = []
    k = 0
    for ctx, mid, typ, locked, vals in mods:
        fv = []
        for v in vals:
            k += 1
            fv.append(v * 0.5 + (k % 7) * 0.125)
        out.append((ctx, mid, typ, locked, fv))
    return out


CONFIGS = [
    ('default', 0xfe, 0x00, 0.3721, 0.189, 0.15),
    ('allopen', 0xff, 0x00, 1.0, 0.5, 0.5),
    ('locked', 0x00, 0x00, 0.9, 0.3, 0.05),
    ('soloenv', 0xfe, 0x08, 1.0, 0.25, 0.9),
    ('solofx', 0xfe, 0xc1, 0.6, 0.1, 0.3),
]


def fmt(x):
    return repr(float(x))


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    emu = MutaEmu(argv[1])
    lines = ['# Generated by tools/mutate/emulate.py from the original editor (v1.62). Do not edit.']

    # 1. libc random() reference: first values of the stream for seed 1.
    r = BsdRandom(1)
    lines.append('RAND 1 ' + ' '.join(str(r.next()) for _ in range(8)))

    # 2. Link curves.
    for x in [0.0, 0.01, 0.05, 0.1, 0.189, 0.25, 0.37, 0.5, 0.75, 1.0]:
        this = emu._alloc(0x400)
        emu.uc.mem_write(this + 0x370, struct.pack('<2d', x, x))
        emu._run(A['SetProbFromRange'], [this])
        p = struct.unpack('<d', emu._rd(this + 0x378, 8))[0]
        emu.uc.mem_write(this + 0x370, struct.pack('<2d', x, x))
        emu._run(A['SetRangeFromProb'], [this])
        rr = struct.unpack('<d', emu._rd(this + 0x370, 8))[0]
        lines.append('LINK %s %s %s' % (fmt(x), fmt(p), fmt(rr)))

    # 3. IsEnabled for every module type, parameter and quick-lock setting.
    masks = [(0xfe, 0, 0), (0xff, 0, 0), (0x00, 0, 0), (0xfe, 0x01, 0), (0xfe, 0x08, 0),
             (0xfe, 0x30, 0), (0xfe, 0x40, 0), (0xfe, 0x80, 0), (0xff, 0, 1)]
    emu.reset()
    for typ in sorted(emu.infos):
        md = emu.infos[typ][0]
        emu.make_module(1, 1, typ, False)
        for unlocked, solo, locked in masks:
            emu.set_muta(unlocked, solo)
            mod = emu.modules[(1, 1)]
            mod['locked'] = locked
            bits = ''.join(str(emu._run(A['IsEnabled'], [emu.muta, mod['ptr'], p]) & 1)
                           for p in range(len(md['params'])))
            lines.append('ENABLED %d %d %d %d %s' % (typ, unlocked, solo, locked, bits))

    # 4. Curves with given random values.
    for kind in [1, 2, 3, 4]:
        for n in [1, 3, 15, 99, 127]:
            for x, rg in [(-1.0, -1.0), (0.2, 0.189), (n * 0.5 + 0.3, 0.05), (n + 0.9, 0.5), (0.0, 0.5)]:
                emu.rng = BsdRandom(7 + kind + n)
                v = emu._run_fpu(A['ApplyCurve'], [kind, n] + emu._dw(x) + emu._dw(rg))
                lines.append('CURVE %d %d %s %s %s' % (kind, n, fmt(x), fmt(rg), fmt(v)))

    # 5. Whole contexts: randomize, mutate, recombine, as the Mutator calls them.
    mods = scenario_modules(emu)
    fath = father_of(mods)
    for ctx, mid, typ, locked, vals in mods:
        lines.append('MODULE %d %d %d %d %s' % (ctx, mid, typ, int(locked), ' '.join(fmt(v) for v in vals)))
    for ctx, mid, typ, locked, vals in fath:
        lines.append('FATHER %d %d %s' % (ctx, mid, ' '.join(fmt(v) for v in vals)))
    m_morphs = {(1, 1, 0, 2): 40, (1, 3, 1, 0): -12, (0, 1, 0, 7): 100, (1, 4, 2, 1): 5}
    f_morphs = {(1, 1, 0, 2): -20, (1, 3, 1, 3): 64, (1, 9, 0, 4): 33}
    for k, d in sorted(m_morphs.items()):
        lines.append('MMORPH %d %d %d %d %d' % (k + (d,)))
    for k, d in sorted(f_morphs.items()):
        lines.append('FMORPH %d %d %d %d %d' % (k + (d,)))
    for name, unlocked, solo, prob, rng_, cross in CONFIGS:
        lines.append('CONFIG %s %d %d %s %s %s' % (name, unlocked, solo, fmt(prob), fmt(rng_), fmt(cross)))
        emu.reset()
        emu.modules = {}
        emu.modules_by_ptr = {}
        for ctx, mid, typ, locked, _v in mods:
            emu.make_module(ctx, mid, typ, locked)
        emu.set_muta(unlocked, solo)
        emu.rng = BsdRandom(1)
        src = {c: emu.make_data(c, [(mid, v) for cc, mid, _t, _l, v in mods if cc == c]) for c in (2, 1, 0)}
        fsrc = {c: emu.make_data(c, [(mid, v) for cc, mid, _t, _l, v in fath if cc == c]) for c in (2, 1, 0)}
        mm, fm, dm = emu._alloc(0x40), emu._alloc(0x40), emu._alloc(0x40)
        emu.morphs = {mm: {k: v for k, v in m_morphs.items()}, fm: {k: v for k, v in f_morphs.items()}}

        def dump(op, dst, ctx):
            byid = {}
            for (mid, p), v in emu.out.get(dst, {}).items():
                byid.setdefault(mid, {})[p] = v
            for mid in sorted(byid):
                vals = [byid[mid][p] for p in sorted(byid[mid])]
                lines.append('RESULT %s %s %d %d %s' % (name, op, ctx, mid, ' '.join(fmt(v) for v in vals)))

        for op, fn in [('R', 'CopyRandomizeContext'), ('U', 'CopyMutateContext')]:
            for ctx in (1, 0):
                d = emu.dest()
                words = [0x1234, src[ctx], 0, d, 0]
                if op == 'U':
                    words += emu._dw(prob) + emu._dw(rng_)
                emu._run(A[fn], words)
                dump(op, d, ctx)
        for ctx in (2, 1, 0):
            d = emu.dest()
            emu._run(A['RecombineContext'], [src[ctx], mm, 0, fsrc[ctx], fm, 0, d, dm, 0] + emu._dw(cross))
            dump('X', d, ctx)
        for k in emu.added_morphs:
            lines.append('XMORPH %s %d %d %d %d %d' % ((name,) + k))
        lines.append('NEXTRAND %s %d' % (name, emu.rng.next()))

    with open(argv[2], 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
