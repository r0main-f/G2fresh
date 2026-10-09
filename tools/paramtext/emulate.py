#!/usr/bin/env python3
"""Run the original G2 editor's ParamText functions in a CPU emulator.

Reads the user's own copy of the Mac editor binary (original/mac/G2Editor_i386,
Mach-O i386) at run time; nothing from that binary is stored in this script.
The display functions are executed with unicorn; the libc/libm imports they
call (sprintf, strcpy, strcat, memcpy, floor, exp, log, log10, pow, logf, expf)
are implemented here in Python. See re/notes/param-display.md, section 4.

Usage (python3 -I, or a venv with `pip install unicorn`):
  emulate.py BINARY tables                 dispatch tables (id -> function)
  emulate.py BINARY call ID V [D1 [D2]]    one string; the number of values
                                           selects the single/dual/triple table
  emulate.py BINARY digests OUT.json       tests/golden/param_text_digests.json
  emulate.py BINARY dump OUT.tsv           every string of the digest domain
"""
import hashlib
import json
import math
import re
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP


# --- Mach-O (32-bit, little endian) --------------------------------------------

class MachO:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        magic, _cpu, _sub, _ft, ncmds, _sz, _fl = struct.unpack_from('<7I', self.data, 0)
        if magic != 0xfeedface:
            raise SystemExit('%s: not a 32-bit little-endian Mach-O file' % path)
        self.sections = []  # (sectname, addr, size, fileoff, reserved1, reserved2)
        symtab = dysymtab = None
        off = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<2I', self.data, off)
            if cmd == 1:  # LC_SEGMENT
                nsects = struct.unpack_from('<I', self.data, off + 48)[0]
                so = off + 56
                for _ in range(nsects):
                    name = self.data[so:so + 16].rstrip(b'\0').decode()
                    addr, ssize, soff = struct.unpack_from('<3I', self.data, so + 32)
                    r1, r2 = struct.unpack_from('<2I', self.data, so + 60)
                    self.sections.append((name, addr, ssize, soff, r1, r2))
                    so += 68
            elif cmd == 2:  # LC_SYMTAB
                symtab = struct.unpack_from('<4I', self.data, off + 8)
            elif cmd == 0xb:  # LC_DYSYMTAB
                dysymtab = struct.unpack_from('<2I', self.data, off + 56)
            off += size
        symoff, nsyms, stroff, _strsize = symtab
        self.names = []
        self.symbols = {}
        for i in range(nsyms):
            strx, ntype, _sect, _desc, value = struct.unpack_from('<IBBhI', self.data, symoff + 12 * i)
            end = self.data.index(b'\0', stroff + strx)
            name = self.data[stroff + strx:end].decode('latin-1')
            self.names.append(name)
            if ntype & 0x0e == 0x0e:  # N_SECT
                self.symbols.setdefault(name, value)
        indoff, nind = dysymtab
        indirect = struct.unpack_from('<%dI' % nind, self.data, indoff)
        self.imports = {}  # stub address -> symbol name
        for name, addr, size, _off, r1, r2 in self.sections:
            if name == '__jump_table' and r2:
                for i in range(size // r2):
                    idx = indirect[r1 + i]
                    if idx < len(self.names):
                        self.imports[addr + i * r2] = self.names[idx]

    def symbol(self, name):
        return self.symbols[name]


# --- emulator --------------------------------------------------------------------

STACK = 0x800000
STUB = 0x900000      # fldl/flds; ret  (returns a value on the x87 stack)
RET = 0x900800
BUF = 0xA00000


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class Emulator:
    def __init__(self, path):
        self.m = MachO(path)
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        top = max(a + s for _, a, s, _, _, _ in self.m.sections)
        uc.mem_map(0, (top + 0xfff) & ~0xfff)
        for _name, addr, size, off, _r1, _r2 in self.m.sections:
            if off and size:
                uc.mem_write(addr, self.m.data[off:off + size])
        uc.mem_map(STACK - 0x100000, 0x100000)
        uc.mem_map(STUB, 0x1000)
        uc.mem_write(STUB, b'\xDD\x05' + struct.pack('<I', STUB + 0x100) + b'\xC3')
        uc.mem_write(STUB + 0x10, b'\xD9\x05' + struct.pack('<I', STUB + 0x100) + b'\xC3')
        uc.mem_write(RET, b'\xF4')
        uc.mem_map(BUF, 0x1000)
        self.handlers = {
            '_sprintf': self._sprintf, '_strcpy': self._strcpy, '_strcat': self._strcat,
            '_memcpy': self._memcpy,
            '_floor': lambda: self._ret_fpu(math.floor(self._d(0))),
            '_exp': lambda: self._ret_fpu(math.exp(self._d(0))),
            '_log': lambda: self._ret_fpu(self._log(math.log, self._d(0))),
            '_log10': lambda: self._ret_fpu(self._log(math.log10, self._d(0))),
            '_pow': lambda: self._ret_fpu(math.pow(self._d(0), self._d(2))),
            # float functions: double result rounded to float
            '_logf': lambda: self._ret_fpu(f32(math.log(self._f(0))), True),
            '_expf': lambda: self._ret_fpu(self._expf(self._f(0)), True),
        }
        lo = min(self.m.imports) if self.m.imports else 0
        hi = max(self.m.imports) + 5 if self.m.imports else 0
        uc.hook_add(UC_HOOK_CODE, self._hook, begin=lo, end=hi)
        self.names = {}
        for name, addr in self.m.symbols.items():
            mt = re.fullmatch(r'__ZN9ParamText(\d+)(\w+)', name)
            if mt:
                n = int(mt.group(1))
                self.names.setdefault(addr, mt.group(2)[:n])

    # memory helpers
    def _rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def _u32(self, a):
        return struct.unpack('<I', self._rd(a, 4))[0]

    def _cstr(self, a):
        out = b''
        while True:
            c = self._rd(a, 1)
            if c == b'\0':
                return out
            out += c
            a += 1

    def _arg(self, k):
        return self._u32(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k)

    def _d(self, k):
        return struct.unpack('<d', self._rd(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k, 8))[0]

    def _f(self, k):
        return struct.unpack('<f', self._rd(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k, 4))[0]

    @staticmethod
    def _log(fn, x):
        return fn(x) if x > 0 else (float('-inf') if x == 0 else float('nan'))

    @staticmethod
    def _expf(x):
        try:
            return f32(math.exp(x))
        except OverflowError:
            return float('inf')

    def _ret(self, eax):
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        ra = self._u32(esp)
        self.uc.reg_write(UC_X86_REG_ESP, esp + 4)
        self.uc.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        self.uc.reg_write(UC_X86_REG_EIP, ra)

    def _ret_fpu(self, val, single=False):
        self.uc.mem_write(STUB + 0x100, struct.pack('<f' if single else '<d', val))
        self.uc.reg_write(UC_X86_REG_EIP, STUB + (0x10 if single else 0))

    # libc
    def _format(self, fmt, sp):
        out = b''
        i = 0
        while i < len(fmt):
            if fmt[i:i + 1] != b'%':
                out += fmt[i:i + 1]
                i += 1
                continue
            mt = re.match(rb'%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d+))?(l?)([dicufsx%])', fmt[i:])
            if not mt:
                raise RuntimeError('unsupported format %r' % fmt)
            flags, width, prec, _l, conv = mt.groups()
            i += mt.end()
            if conv == b'%':
                out += b'%'
                continue
            if width == b'*':
                width = str(struct.unpack('<i', self._rd(sp, 4))[0]).encode()
                sp += 4
            if prec == b'*':
                prec = str(struct.unpack('<i', self._rd(sp, 4))[0]).encode()
                sp += 4
            spec = '%' + flags.decode() + (width or b'').decode() + ('.' + prec.decode() if prec is not None else '')
            if conv in b'di':
                out += ((spec + 'd') % struct.unpack('<i', self._rd(sp, 4))[0]).encode()
                sp += 4
            elif conv == b'u':
                out += ((spec + 'd') % self._u32(sp)).encode()
                sp += 4
            elif conv == b'x':
                out += ((spec + 'x') % self._u32(sp)).encode()
                sp += 4
            elif conv == b'c':
                out += bytes([self._u32(sp) & 0xff])
                sp += 4
            elif conv == b's':
                out += ((spec + 's') % self._cstr(self._u32(sp)).decode('latin-1')).encode('latin-1')
                sp += 4
            else:  # f: printf rounding of the exact binary value, as C does
                out += ((spec + 'f') % struct.unpack('<d', self._rd(sp, 8))[0]).encode()
                sp += 8
        return out

    def _sprintf(self):
        s = self._format(self._cstr(self._arg(1)), self.uc.reg_read(UC_X86_REG_ESP) + 12)
        self.uc.mem_write(self._arg(0), s + b'\0')
        self._ret(len(s))

    def _strcpy(self):
        self.uc.mem_write(self._arg(0), self._cstr(self._arg(1)) + b'\0')
        self._ret(self._arg(0))

    def _strcat(self):
        self.uc.mem_write(self._arg(0), self._cstr(self._arg(0)) + self._cstr(self._arg(1)) + b'\0')
        self._ret(self._arg(0))

    def _memcpy(self):
        self.uc.mem_write(self._arg(0), self._rd(self._arg(1), self._arg(2)))
        self._ret(self._arg(0))

    def _hook(self, _uc, addr, _size, _user):
        name = self.m.imports.get(addr)
        if name is None:
            return
        if name not in self.handlers:
            raise RuntimeError('unhandled import %s at 0x%x' % (name, addr))
        self.handlers[name]()

    def _run(self, addr, args):
        sp = STACK - 0x1000 - 4 * len(args)
        for i, v in enumerate(args):
            self.uc.mem_write(sp + 4 * i, struct.pack('<I', v & 0xffffffff))
        sp -= 4
        self.uc.mem_write(sp, struct.pack('<I', RET))
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.emu_start(addr, RET, count=500000)
        return self.uc.reg_read(UC_X86_REG_EAX)

    def text(self, fn_addr, values):
        """Calls f(values..., buf); returns the string (Mac Roman -> str)."""
        self.uc.mem_write(BUF, b'\0' * 64)
        self._run(fn_addr, list(values) + [BUF])
        return self._cstr(BUF).decode('mac_roman')

    def table(self, arity):
        getter = self.m.symbol({1: '__ZN9ParamText31GetSingelDependencyTextFunctionEi',
                                2: '__ZN9ParamText29GetDualDependencyTextFunctionEi',
                                3: '__ZN9ParamText31GetTripleDependencyTextFunctionEi'}[arity])
        return {i: self._run(getter, [i]) for i in range(256)}


# --- digest domain ----------------------------------------------------------------

DEFAULTS = {1: 'Default', 2: 'DualDefault', 3: 'TripleDefault'}
R = [0, 127]
# Inclusive [lo, hi] per argument, own value first. Anything not listed:
# single [R], dual [R, R], triple [R, R, R].
DOMAINS = {
    'DualDefault': [R, [0, 3]],
    'TripleDefault': [R, [0, 1], [0, 1]],
    'DelayTimeTap8': [R, [0, 6]],
    'DelayTime2': [R, [0, 6]],
    'LFOFreq': [R, [0, 7]],
    'ReverbTime': [R, [0, 7]],
    'OscFreqDep': [R, R, [0, 5]],
    'ClkGenTempo': [R, [0, 2], [0, 2]],
    'DelayTimeTap': [R, [0, 2], [0, 6]],
    'DelayTimeFx': [R, [0, 2], [0, 3]],
    'DelayTimeStereo': [R, [0, 2], [0, 2]],
    'DXOscFreq': [R, R, [0, 1]],
}


def domain_tuples(dom):
    if not dom:
        yield ()
        return
    lo, hi = dom[0]
    for v in range(lo, hi + 1):
        for rest in domain_tuples(dom[1:]):
            yield (v,) + rest


def entries(emu):
    """(arity, id, function name, function address, domain) for every assigned
    id plus id 255 as a representative of each table's default."""
    out = []
    for arity in (1, 2, 3):
        for i, addr in emu.table(arity).items():
            name = emu.names.get(addr, '0x%x' % addr)
            if name == DEFAULTS[arity] and i != 255:
                continue
            out.append((arity, i, name, addr, DOMAINS.get(name, [R] * arity)))
    return out


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    emu = Emulator(argv[1])
    cmd = argv[2]
    if cmd == 'tables':
        for arity, i, name, _addr, _dom in entries(emu):
            print('%d\t%d\t%s' % (arity, i, name))
    elif cmd == 'call':
        vals = [int(x) for x in argv[4:]]
        print(emu.text(emu.table(len(vals))[int(argv[3])], vals))
    elif cmd in ('digests', 'dump'):
        rows = []
        dump = open(argv[3], 'w', encoding='utf-8') if cmd == 'dump' else None
        for arity, i, name, addr, dom in entries(emu):
            h = hashlib.sha256()
            for vals in domain_tuples(dom):
                s = emu.text(addr, vals)
                h.update(s.encode('utf-8') + b'\n')
                if dump:
                    dump.write('%d\t%d\t%s\t%s\t%s\n' % (arity, i, name, ','.join(map(str, vals)), s))
            rows.append({'table': arity, 'id': i, 'function': name, 'domain': dom, 'sha256': h.hexdigest()})
        if dump:
            dump.close()
            return
        with open(argv[3], 'w') as f:
            f.write('{\n "description": "SHA-256 per (table, id) of the original ParamText output, '
                    'generated by tools/paramtext/emulate.py from G2Editor_i386 (Mac v1.62).",\n')
            f.write(' "method": "For every tuple of the domain (inclusive [lo, hi] per argument, own value '
                    'first; nested loops with the first argument outermost) the UTF-8 output string '
                    'followed by a newline is hashed. Mac Roman 0xB1 is written as U+00B1. table: 1 single, '
                    '2 dual, 3 triple. id 255 stands for the table default.",\n')
            f.write(' "entries": [\n')
            f.write(',\n'.join('  ' + json.dumps(r, separators=(', ', ': ')) for r in rows))
            f.write('\n ]\n}\n')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
