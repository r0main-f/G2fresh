#!/usr/bin/env python3
"""Run the original G2 editor's patch-load code (CPatchLoad) in a CPU emulator.

Reads the user's own copy of the Mac editor binary (original/mac/G2Editor_i386,
Mach-O i386) at run time; nothing from that binary is stored in this script.
For each patch, the module list and the cable count of each area are fed to
the original CPatchLoad::AddModule / AddCable exactly as CPatch does when it
builds a patch (InternalNewModule -> AddModuleToPatchLoad, HandleCableDump ->
AddCableToPatchLoad), then the original getters are called. See
re/notes/patch-load.md.

The fake CModule only has the fields CPatchLoad reads: +0 builder pointer
(builder+0xC = the type's _k<Name>ModuleSize table, as CBuildModule stores
it), +0x34 context (0 FX, 1 VA), +0x40 bandwidth (1 = uprated).

Usage (a venv with `pip install unicorn`):
  emulate.py BINARY golden OUT.json PATCH...   per-patch figures (JSON)
  emulate.py BINARY tsv OUT.tsv PATCH...       the same as a table, percentages as
                                               exact hex floats; this is the format of
                                               tests/golden/patch_load.tsv
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'paramtext'))
sys.path.insert(0, os.path.join(HERE, '..', 'pch2'))

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP  # noqa: E402

from emulate import MachO  # noqa: E402  (tools/paramtext/emulate.py)
import pch2dump  # noqa: E402

STACK = 0x800000
TRAMP = 0x900000
RES = 0x900800
HEAP = 0xA00000

FX, VA = 0, 1

FUNCS = {
    'ctor': '__ZN10CPatchLoadC1Ev',
    'AddModule': '__ZN10CPatchLoad9AddModuleEP7CModule',
    'AddCable': '__ZN10CPatchLoad8AddCableEN9NSFile_V78EContextE',
    'GetFreeDynamicRam': '__ZNK10CPatchLoad17GetFreeDynamicRamEv',
    'GetCycles': '__ZNK10CPatchLoad9GetCyclesEN9NSFile_V78EContextE',
    'GetXmem': '__ZNK10CPatchLoad7GetXmemEN9NSFile_V78EContextE',
    'GetYmem': '__ZNK10CPatchLoad7GetYmemEN9NSFile_V78EContextE',
    'GetPmem': '__ZNK10CPatchLoad7GetPmemEN9NSFile_V78EContextE',
    'GetZPmem': '__ZNK10CPatchLoad8GetZPmemEN9NSFile_V78EContextE',
    'GetRAM': '__ZNK10CPatchLoad6GetRAMEN9NSFile_V78EContextE',
    'GetQmem': '__ZNK10CPatchLoad7GetQmemEN9NSFile_V78EContextE',
    'GetRmem': '__ZNK10CPatchLoad7GetRmemEN9NSFile_V78EContextE',
    'GetCriticalResource': '__ZNK10CPatchLoad19GetCriticalResourceEN9NSFile_V78EContextE',
    'GetCriticalResourceType': '__ZNK10CPatchLoad23GetCriticalResourceTypeEN9NSFile_V78EContextE',
}


def module_size_symbols():
    """type id -> _k<Name>ModuleSize symbol, from data/modules.json (+ the Name bar)."""
    path = os.path.join(HERE, '..', '..', 'data', 'modules.json')
    mods = json.load(open(path))['modules']
    mods = mods if isinstance(mods, list) else list(mods.values())
    out = {m['typeId']: m['resources']['symbol'] for m in mods if m.get('resources')}
    out[126] = '_kNameModuleSize'  # CBModuleName::CBModuleName 000bb4e2
    return out


class PatchLoadEmu:
    def __init__(self, binary):
        self.m = MachO(binary)
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        top = max(a + s for _, a, s, _, _, _ in self.m.sections)
        uc.mem_map(0, (top + 0xfff) & ~0xfff)
        for _name, addr, size, off, _r1, _r2 in self.m.sections:
            if off and size:
                uc.mem_write(addr, self.m.data[off:off + size])
        uc.mem_map(STACK - 0x100000, 0x100000)
        uc.mem_map(TRAMP, 0x1000)
        uc.mem_map(HEAP, 0x100000)
        self.fn = {k: self.m.symbol(v) for k, v in FUNCS.items()}
        self.sizes = {t: self.m.symbol(s) for t, s in module_size_symbols().items()}
        self.tramps = {}

    def call(self, name, *args, ret='int'):
        # One fixed trampoline per (function, return kind), so that no translated
        # code is ever overwritten: mov eax, fn; call eax; [fstp dword [RES]]; hlt
        key = (name, ret)
        if key not in self.tramps:
            code = b'\xB8' + struct.pack('<I', self.fn[name]) + b'\xFF\xD0'
            if ret == 'float':
                code += b'\xD9\x1D' + struct.pack('<I', RES)
            code += b'\xF4'
            at = TRAMP + 0x20 * len(self.tramps)
            self.uc.mem_write(at, code)
            self.tramps[key] = (at, at + len(code) - 1)
        start, stop = self.tramps[key]
        sp = STACK - 0x1000
        self.uc.mem_write(sp, b''.join(struct.pack('<I', a & 0xffffffff) for a in args))
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.emu_start(start, stop)
        if ret == 'float':
            return struct.unpack('<f', bytes(self.uc.mem_read(RES, 4)))[0]
        return self.uc.reg_read(UC_X86_REG_EAX) & 0xffffffff

    def run(self, areas):
        """areas: {context: (list of (type, uprate), cable count)} -> figures."""
        self.uc.mem_write(HEAP, b'\xAA' * 0x100000)
        pl = HEAP
        self.call('ctor', pl)
        nxt = HEAP + 0x1000
        skipped = []
        for ctx, (mods, ncables) in sorted(areas.items()):
            for typ, uprate in mods:
                spec = self.sizes.get(typ)
                if spec is None:
                    skipped.append(typ)  # the original editor would refuse the patch
                    continue
                builder, mod = nxt, nxt + 0x40
                nxt += 0x100
                self.uc.mem_write(builder + 0xC, struct.pack('<I', spec))
                self.uc.mem_write(mod, struct.pack('<I', builder))
                self.uc.mem_write(mod + 0x34, struct.pack('<I', ctx))
                self.uc.mem_write(mod + 0x40, struct.pack('<I', 1 if uprate else 0))
                self.call('AddModule', pl, mod)
            for _ in range(ncables):
                self.call('AddCable', pl, ctx)
        out = {'freeDynamicRam': self.call('GetFreeDynamicRam', pl)}
        if skipped:
            out['skippedTypes'] = skipped
        for ctx, key in ((VA, 'va'), (FX, 'fx')):
            base = pl + (0x3C if ctx == VA else 0x1C)
            raw = bytes(self.uc.mem_read(base, 0x20))
            a = {'raw': {
                'cyclesA': struct.unpack_from('<H', raw, 0)[0],
                'cyclesB': struct.unpack_from('<H', raw, 2)[0],
                'zp': raw[4],
                'mem': list(struct.unpack_from('<6H', raw, 8)),  # xA yA pA xB yB pB
                'dynRam': struct.unpack_from('<I', raw, 0x14)[0],
                'q': struct.unpack_from('<I', raw, 0x18)[0],
                'r': struct.unpack_from('<I', raw, 0x1C)[0],
            }}
            for g in ('GetCycles', 'GetXmem', 'GetYmem', 'GetPmem', 'GetZPmem', 'GetRAM',
                      'GetQmem', 'GetRmem', 'GetCriticalResource'):
                a[g[3:]] = self.call(g, pl, ctx, ret='float')
            a['CriticalResourceType'] = self.call('GetCriticalResourceType', pl, ctx)
            out[key] = a
        return out


def patch_areas(path):
    """{context: ([(type, uprate)], cable count)} for each patch in the file."""
    d = pch2dump.decode_file(open(path, 'rb').read())
    patches, cur = [], None
    for s in d['sections']:
        if s.get('name') == 'patch_header':
            cur = {FX: ([], 0), VA: ([], 0)}
            patches.append(cur)
        if cur is None or s.get('location') not in (FX, VA):
            continue
        loc = s['location']
        mods, nc = cur[loc]
        if s.get('name') == 'module_list':
            mods = mods + [(m['type'], m['uprate']) for m in s['modules']]
        elif s.get('name') == 'cable_list':
            nc += len(s['cables'])
        cur[loc] = (mods, nc)
    return patches


GETTERS = ('Cycles', 'Xmem', 'Ymem', 'Pmem', 'ZPmem', 'RAM', 'Qmem', 'Rmem', 'CriticalResource')
TSV_HEADER = ('# file\tslot\tarea\tcyclesA\tcyclesB\tzp\txA\tyA\tpA\txB\tyB\tpB\tdynRam\tq\tr\t'
              + '\t'.join(GETTERS) + '\tcriticalType\tfreeDynamicRam\n')


def tsv_lines(name, results):
    """One line per (patch, area); percentages as exact hex floats."""
    out = []
    for slot, res in enumerate(results):
        for key in ('va', 'fx'):
            a = res[key]
            r = a['raw']
            cols = [name, str(slot), key, str(r['cyclesA']), str(r['cyclesB']), str(r['zp'])]
            cols += [str(v) for v in r['mem']] + [str(r['dynRam']), str(r['q']), str(r['r'])]
            cols += [float(a[g]).hex() for g in GETTERS]
            cols += [str(a['CriticalResourceType']), str(res['freeDynamicRam'])]
            out.append('\t'.join(cols) + '\n')
    return out


def main():
    if len(sys.argv) < 4 or sys.argv[2] not in ('golden', 'tsv'):
        raise SystemExit(__doc__)
    emu = PatchLoadEmu(sys.argv[1])
    results = {}
    for p in sys.argv[4:]:
        try:
            pats = patch_areas(p)
        except Exception as e:  # noqa: BLE001  undecodable file
            print('skip %s: %s' % (p, e), file=sys.stderr)
            continue
        results[p] = [emu.run(a) for a in pats]
    with open(sys.argv[3], 'w') as f:
        if sys.argv[2] == 'golden':
            json.dump({os.path.basename(k): v for k, v in results.items()}, f, indent=1, sort_keys=True)
            f.write('\n')
        else:
            f.write(TSV_HEADER)
            for p in sorted(results):
                f.writelines(tsv_lines(os.path.basename(p), results[p]))


if __name__ == '__main__':
    main()
