#!/usr/bin/env python3
"""Run the original G2 editor's file conversion in a CPU emulator.

Reads the user's own copy of the Mac editor binary (original/mac/G2Editor_i386,
Mach-O i386) at run time; nothing from that binary is stored in this script.

For a .pch2/.prf2 file, runs unmodified: CFileByteStream::GetLine (the text
header), the CPatchFile_N / CPerformanceFile_N reader of the declared version
(vtable +0x0c, Read), CFileConverter::CFileConverter (which converts to version
23: CFileConverter::Convert @00051224) and the CPatchFile_23 /
CPerformanceFile_23 writer (vtable +0x10, Write). The libstdc++/libc imports
they call (operator new, std::string, the red-black tree and list primitives,
memcpy ...) are implemented here; every static initializer of the binary runs
once first (constants such as NSFile_V11::kPatchHeaderSize). Heap memory is
zeroed, so the bits Clavia's writer leaves uninitialised (the 6 reserved bits
of each 5A section) come out as 0. See re/notes/pch2-format.md §7.

Usage (a venv with `pip install unicorn`):
  emulate.py BINARY golden OUT.tsv FILE...
      every FILE (a version 23 patch) declared as each version 12..22 and
      converted: SHA-256 of the result, from the version word on, with the
      release velocities the v1.62 writer writes. This is tests/golden/convert.tsv.
  emulate.py BINARY compare G2TOOL FILE... [--list FILE] [--as V,V,...] [--jobs N]
      convert each FILE (and each file named in the --list file, one per line;
      as found, or declared as the versions given) with the
      original and with `G2TOOL convert`, and compare the results byte for byte.
"""
import hashlib
import multiprocessing
import os
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UcError
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'paramtext'))
sys.path.insert(0, os.path.join(HERE, '..', 'pch2'))

from emulate import MachO  # noqa: E402  (tools/paramtext/emulate.py)
import pch2dump  # noqa: E402

STACK = 0x08000000
STACK_SIZE = 0x400000
HEAP = 0x10000000
HEAP_SIZE = 0x4000000
STUB = 0x0F000000
RET = STUB + 0x10
EMPTY_REP = STUB + 0x100  # std::string::_Rep::_S_empty_rep_storage (16 bytes)


class Thrown(Exception):
    pass


class ConvEmu:
    def __init__(self, path):
        m = self.m = MachO(path)
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        top = max(a + s for _, a, s, _, _, _ in m.sections)
        uc.mem_map(0, (top + 0xfff) & ~0xfff)
        for _name, addr, size, off, _r1, _r2 in m.sections:
            if off and size:
                uc.mem_write(addr, m.data[off:off + size])
        uc.mem_map(STACK - STACK_SIZE, STACK_SIZE + 0x1000)
        uc.mem_map(HEAP, HEAP_SIZE)
        uc.mem_map(STUB, 0x1000)
        uc.mem_write(RET, b'\xF4')
        uc.mem_write(EMPTY_REP, b'\0' * 16)
        self._fix_pointers()
        self.handlers = self._make_handlers()
        lo = min(m.imports)
        hi = max(m.imports) + 5
        uc.hook_add(UC_HOOK_CODE, self._hook, begin=lo, end=hi)
        for local in ('___cxa_atexit', '_atexit'):
            a = m.symbols[local]
            uc.hook_add(UC_HOOK_CODE, lambda *_x: self.ret(0), begin=a, end=a)
        self.heap_reset()

    # --- non-lazy pointers to data imports
    def _fix_pointers(self):
        m = self.m
        off = 28
        ncmds = struct.unpack_from('<7I', m.data, 0)[4]
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<2I', m.data, off)
            if cmd == 0xb:
                indoff, nind = struct.unpack_from('<2I', m.data, off + 56)
            off += size
        indirect = struct.unpack_from('<%dI' % nind, m.data, indoff)
        for name, addr, size, foff, r1, _r2 in m.sections:
            if name != '__pointers':
                continue
            for i in range(size // 4):
                idx = indirect[r1 + i]
                if idx & 0xC0000000:
                    continue
                nm = m.names[idx]
                if nm in m.symbols:
                    continue
                if nm == '__ZNSs4_Rep20_S_empty_rep_storageE':
                    self.uc.mem_write(addr + 4 * i, struct.pack('<I', EMPTY_REP))
                else:
                    # unknown data import: point at a poison page
                    self.uc.mem_write(addr + 4 * i, struct.pack('<I', 0x0E000000))

    # --- heap
    heap_reset_base = HEAP + 0x100

    def heap_reset(self):
        self.brk = self.heap_reset_base

    def alloc(self, n):
        n = max(n, 1)
        a = (self.brk + 15) & ~15
        self.brk = a + n
        if self.brk >= HEAP + HEAP_SIZE:
            raise MemoryError('emulated heap exhausted')
        self.uc.mem_write(a, bytes(n))  # zeroed: deterministic uninitialised bytes
        return a

    # --- memory helpers
    def rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def u32(self, a):
        return struct.unpack('<I', self.rd(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def arg(self, k):
        return self.u32(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k)

    def ret(self, eax=0):
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        ra = self.u32(esp)
        self.uc.reg_write(UC_X86_REG_ESP, esp + 4)
        self.uc.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        self.uc.reg_write(UC_X86_REG_EIP, ra)

    def cstr(self, a):
        out = b''
        while True:
            c = self.rd(a, 64)
            i = c.find(b'\0')
            if i >= 0:
                return out + c[:i]
            out += c
            a += 64

    # --- std::string (libstdc++ COW): object = char* p; rep at p-12: len, cap, refcount
    def str_make(self, data):
        if not data:
            return EMPTY_REP + 12
        rep = self.alloc(12 + len(data) + 1)
        self.uc.mem_write(rep, struct.pack('<IIi', len(data), len(data), 0) + data + b'\0')
        return rep + 12

    def str_get(self, obj):
        p = self.u32(obj)
        n = self.u32(p - 12)
        return self.rd(p, n)

    def str_set(self, obj, data):
        self.w32(obj, self.str_make(data))

    # --- rb tree (libstdc++ tree.cc)
    # node: color +0 (0 red, 1 black), parent +4, left +8, right +0xc
    def _c(self, x):
        return self.u32(x)

    def _p(self, x):
        return self.u32(x + 4)

    def _l(self, x):
        return self.u32(x + 8)

    def _r(self, x):
        return self.u32(x + 12)

    def _sc(self, x, v):
        self.w32(x, v)

    def _sp(self, x, v):
        self.w32(x + 4, v)

    def _sl(self, x, v):
        self.w32(x + 8, v)

    def _sr(self, x, v):
        self.w32(x + 12, v)

    def rb_increment(self, x):
        if self._r(x):
            x = self._r(x)
            while self._l(x):
                x = self._l(x)
        else:
            y = self._p(x)
            while x == self._r(y):
                x = y
                y = self._p(y)
            if self._r(x) != y:
                x = y
        return x

    def rb_decrement(self, x):
        if self._c(x) == 0 and self._p(self._p(x)) == x:
            x = self._r(x)
        elif self._l(x):
            y = self._l(x)
            while self._r(y):
                y = self._r(y)
            x = y
        else:
            y = self._p(x)
            while x == self._l(y):
                x = y
                y = self._p(y)
            x = y
        return x

    def rot_left(self, x, root_ref):
        y = self._r(x)
        self._sr(x, self._l(y))
        if self._l(y):
            self._sp(self._l(y), x)
        self._sp(y, self._p(x))
        if x == self.u32(root_ref):
            self.w32(root_ref, y)
        elif x == self._l(self._p(x)):
            self._sl(self._p(x), y)
        else:
            self._sr(self._p(x), y)
        self._sl(y, x)
        self._sp(x, y)

    def rot_right(self, x, root_ref):
        y = self._l(x)
        self._sl(x, self._r(y))
        if self._r(y):
            self._sp(self._r(y), x)
        self._sp(y, self._p(x))
        if x == self.u32(root_ref):
            self.w32(root_ref, y)
        elif x == self._r(self._p(x)):
            self._sr(self._p(x), y)
        else:
            self._sl(self._p(x), y)
        self._sr(y, x)
        self._sp(x, y)

    def rb_insert_rebalance(self, insert_left, x, p, header):
        root_ref = header + 4
        self._sp(x, p)
        self._sl(x, 0)
        self._sr(x, 0)
        self._sc(x, 0)
        if insert_left:
            self._sl(p, x)
            if p == header:
                self._sp(header, x)
                self._sr(header, x)
            elif p == self._l(header):
                self._sl(header, x)
        else:
            self._sr(p, x)
            if p == self._r(header):
                self._sr(header, x)
        while x != self.u32(root_ref) and self._c(self._p(x)) == 0:
            xpp = self._p(self._p(x))
            if self._p(x) == self._l(xpp):
                y = self._r(xpp)
                if y and self._c(y) == 0:
                    self._sc(self._p(x), 1)
                    self._sc(y, 1)
                    self._sc(xpp, 0)
                    x = xpp
                else:
                    if x == self._r(self._p(x)):
                        x = self._p(x)
                        self.rot_left(x, root_ref)
                    self._sc(self._p(x), 1)
                    self._sc(xpp, 0)
                    self.rot_right(xpp, root_ref)
            else:
                y = self._l(xpp)
                if y and self._c(y) == 0:
                    self._sc(self._p(x), 1)
                    self._sc(y, 1)
                    self._sc(xpp, 0)
                    x = xpp
                else:
                    if x == self._l(self._p(x)):
                        x = self._p(x)
                        self.rot_right(x, root_ref)
                    self._sc(self._p(x), 1)
                    self._sc(xpp, 0)
                    self.rot_left(xpp, root_ref)
        self._sc(self.u32(root_ref), 1)

    def rb_rebalance_for_erase(self, z, header):
        root_ref = header + 4
        leftmost_ref = header + 8
        rightmost_ref = header + 12
        y = z
        x = 0
        x_parent = 0
        if not self._l(y):
            x = self._r(y)
        elif not self._r(y):
            x = self._l(y)
        else:
            y = self._r(y)
            while self._l(y):
                y = self._l(y)
            x = self._r(y)
        if y != z:
            self._sp(self._l(z), y)
            self._sl(y, self._l(z))
            if y != self._r(z):
                x_parent = self._p(y)
                if x:
                    self._sp(x, self._p(y))
                self._sl(self._p(y), x)
                self._sr(y, self._r(z))
                self._sp(self._r(z), y)
            else:
                x_parent = y
            if self.u32(root_ref) == z:
                self.w32(root_ref, y)
            elif self._l(self._p(z)) == z:
                self._sl(self._p(z), y)
            else:
                self._sr(self._p(z), y)
            self._sp(y, self._p(z))
            cy, cz = self._c(y), self._c(z)
            self._sc(y, cz)
            self._sc(z, cy)
            y = z
        else:
            x_parent = self._p(y)
            if x:
                self._sp(x, self._p(y))
            if self.u32(root_ref) == z:
                self.w32(root_ref, x)
            elif self._l(self._p(z)) == z:
                self._sl(self._p(z), x)
            else:
                self._sr(self._p(z), x)
            if self.u32(leftmost_ref) == z:
                if not self._r(z):
                    self.w32(leftmost_ref, self._p(z))
                else:
                    t = x
                    while self._l(t):
                        t = self._l(t)
                    self.w32(leftmost_ref, t)
            if self.u32(rightmost_ref) == z:
                if not self._l(z):
                    self.w32(rightmost_ref, self._p(z))
                else:
                    t = x
                    while self._r(t):
                        t = self._r(t)
                    self.w32(rightmost_ref, t)
        if self._c(y) != 0:
            while x != self.u32(root_ref) and (x == 0 or self._c(x) == 1):
                if x == self._l(x_parent):
                    w = self._r(x_parent)
                    if self._c(w) == 0:
                        self._sc(w, 1)
                        self._sc(x_parent, 0)
                        self.rot_left(x_parent, root_ref)
                        w = self._r(x_parent)
                    if (self._l(w) == 0 or self._c(self._l(w)) == 1) and (self._r(w) == 0 or self._c(self._r(w)) == 1):
                        self._sc(w, 0)
                        x = x_parent
                        x_parent = self._p(x_parent)
                    else:
                        if self._r(w) == 0 or self._c(self._r(w)) == 1:
                            self._sc(self._l(w), 1)
                            self._sc(w, 0)
                            self.rot_right(w, root_ref)
                            w = self._r(x_parent)
                        self._sc(w, self._c(x_parent))
                        self._sc(x_parent, 1)
                        if self._r(w):
                            self._sc(self._r(w), 1)
                        self.rot_left(x_parent, root_ref)
                        break
                else:
                    w = self._l(x_parent)
                    if self._c(w) == 0:
                        self._sc(w, 1)
                        self._sc(x_parent, 0)
                        self.rot_right(x_parent, root_ref)
                        w = self._l(x_parent)
                    if (self._r(w) == 0 or self._c(self._r(w)) == 1) and (self._l(w) == 0 or self._c(self._l(w)) == 1):
                        self._sc(w, 0)
                        x = x_parent
                        x_parent = self._p(x_parent)
                    else:
                        if self._l(w) == 0 or self._c(self._l(w)) == 1:
                            self._sc(self._r(w), 1)
                            self._sc(w, 0)
                            self.rot_left(w, root_ref)
                            w = self._l(x_parent)
                        self._sc(w, self._c(x_parent))
                        self._sc(x_parent, 1)
                        if self._l(w):
                            self._sc(self._l(w), 1)
                        self.rot_right(x_parent, root_ref)
                        break
            if x:
                self._sc(x, 1)
        return y

    # --- std::list node base: next +0, prev +4
    def list_hook(self, node, pos):
        prev = self.u32(pos + 4)
        self.w32(node, pos)
        self.w32(node + 4, prev)
        self.w32(prev, node)
        self.w32(pos + 4, node)

    def list_unhook(self, node):
        nxt = self.u32(node)
        prev = self.u32(node + 4)
        self.w32(prev, nxt)
        self.w32(nxt + 4, prev)

    def list_transfer(self, this, first, last):
        # move [first, last) before this
        if this != last:
            # remove [first, last) from its old position
            self.w32(self.u32(last + 4), this)
            self.w32(self.u32(first + 4), last)
            self.w32(self.u32(this + 4), first)
            tmp = self.u32(this + 4)
            self.w32(this + 4, self.u32(last + 4))
            self.w32(last + 4, self.u32(first + 4))
            self.w32(first + 4, tmp)

    def _make_handlers(self):
        h = {}

        def new():
            self.ret(self.alloc(self.arg(0)))
        h['__Znwm'] = new
        h['__Znam'] = new
        h['_malloc'] = new
        h['__ZdlPv'] = lambda: self.ret(0)
        h['__ZdaPv'] = lambda: self.ret(0)
        h['_free'] = lambda: self.ret(0)

        def memcpy():
            d, s, n = self.arg(0), self.arg(1), self.arg(2)
            if n:
                self.uc.mem_write(d, self.rd(s, n))
            self.ret(d)
        h['_memcpy'] = memcpy
        h['_memmove'] = memcpy

        def memset():
            d, c, n = self.arg(0), self.arg(1), self.arg(2)
            if n:
                self.uc.mem_write(d, bytes([c & 0xff]) * n)
            self.ret(d)
        h['_memset'] = memset

        def memcmp():
            a, b, n = self.rd(self.arg(0), self.arg(2)), self.rd(self.arg(1), self.arg(2)), self.arg(2)
            r = 0
            for x, y in zip(a, b):
                if x != y:
                    r = x - y
                    break
            self.ret(r)
        h['_memcmp'] = memcmp
        def strcpy():
            d, s = self.arg(0), self.cstr(self.arg(1))
            self.uc.mem_write(d, s + b'\0')
            self.ret(d)
        h['_strcpy'] = strcpy
        for upp in ('_NewDragTrackingHandlerUPP', '_NewControlActionUPP', '_NewTimerUPP'):
            h[upp] = lambda: self.ret(0)
        for a in ('__ZNSaIcEC1Ev', '__ZNSaIcED1Ev', '__ZNSaIcEC2Ev', '__ZNSaIcED2Ev'):
            h[a] = lambda: self.ret(0)
        h['_strlen'] = lambda: self.ret(len(self.cstr(self.arg(0))))
        h['_atoi'] = lambda: self.ret(int(self.cstr(self.arg(0)) or b'0'))
        h['__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base'] = lambda: self.ret(self.rb_increment(self.arg(0)))
        h['__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base'] = lambda: self.ret(self.rb_increment(self.arg(0)))
        h['__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base'] = lambda: self.ret(self.rb_decrement(self.arg(0)))
        h['__ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base'] = lambda: self.ret(self.rb_decrement(self.arg(0)))

        def ins():
            self.rb_insert_rebalance(self.arg(0) & 0xff, self.arg(1), self.arg(2), self.arg(3))
            self.ret(0)
        h['__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_'] = ins
        h['__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_'] = \
            lambda: self.ret(self.rb_rebalance_for_erase(self.arg(0), self.arg(1)))

        def hook():
            self.list_hook(self.arg(0), self.arg(1))
            self.ret(0)
        h['__ZNSt15_List_node_base4hookEPS_'] = hook

        def unhook():
            self.list_unhook(self.arg(0))
            self.ret(0)
        h['__ZNSt15_List_node_base6unhookEv'] = unhook

        def transfer():
            self.list_transfer(self.arg(0), self.arg(1), self.arg(2))
            self.ret(0)
        h['__ZNSt15_List_node_base8transferEPS_S0_'] = transfer

        def guard_acquire():
            g = self.arg(0)
            first = self.rd(g, 1) == b'\0'
            self.ret(1 if first else 0)
        h['___cxa_guard_acquire'] = guard_acquire

        def guard_release():
            self.uc.mem_write(self.arg(0), b'\1')
            self.ret(0)
        h['___cxa_guard_release'] = guard_release
        h['___cxa_guard_abort'] = lambda: self.ret(0)
        h['__ZNSt8ios_base4InitC1Ev'] = lambda: self.ret(0)
        h['___cxa_atexit'] = lambda: self.ret(0)
        h['_atexit'] = lambda: self.ret(0)
        h['___cxa_allocate_exception'] = lambda: self.ret(self.alloc(self.arg(0) + 64) + 64)

        def throw():
            obj = self.arg(0)
            msg = ''
            try:
                # XFileBase: vtable, code, char* message?  best effort
                for k in (4, 8):
                    p = self.u32(obj + k)
                    if 0x199d48 <= p < 0x19eb94 or HEAP <= p < HEAP + HEAP_SIZE:
                        s = self.cstr(p)
                        if s:
                            msg = s.decode('latin-1')
                code = self.u32(obj + 4)
            except Exception:  # noqa: BLE001
                code = -1
            raise Thrown('%s (code %d)' % (msg, code))
        h['___cxa_throw'] = throw
        h['___cxa_begin_catch'] = lambda: (_ for _ in ()).throw(Thrown('catch'))

        # std::string
        def s_default():
            self.w32(self.arg(0), EMPTY_REP + 12)
            self.ret(0)
        h['__ZNSsC1Ev'] = s_default
        h['__ZNSsC2Ev'] = s_default

        def s_cstr():
            self.str_set(self.arg(0), self.cstr(self.arg(1)))
            self.ret(0)
        h['__ZNSsC1EPKcRKSaIcE'] = s_cstr
        h['__ZNSsC2EPKcRKSaIcE'] = s_cstr

        def s_copy():
            self.str_set(self.arg(0), self.str_get(self.arg(1)))
            self.ret(0)
        h['__ZNSsC1ERKSs'] = s_copy
        h['__ZNSsC2ERKSs'] = s_copy

        def s_ptrlen():
            self.str_set(self.arg(0), self.rd(self.arg(1), self.arg(2)) if self.arg(2) else b'')
            self.ret(0)
        h['__ZNSsC1EPKcmRKSaIcE'] = s_ptrlen

        def s_assign():
            self.str_set(self.arg(0), self.str_get(self.arg(1)))
            self.ret(self.arg(0))
        h['__ZNSs6assignERKSs'] = s_assign
        h['__ZNSsaSERKSs'] = s_assign

        def s_assign_cstr():
            self.str_set(self.arg(0), self.cstr(self.arg(1)))
            self.ret(self.arg(0))
        h['__ZNSs6assignEPKc'] = s_assign_cstr
        h['__ZNSsaSEPKc'] = s_assign_cstr

        def s_assign_ptrlen():
            self.str_set(self.arg(0), self.rd(self.arg(1), self.arg(2)) if self.arg(2) else b'')
            self.ret(self.arg(0))
        h['__ZNSs6assignEPKcm'] = s_assign_ptrlen

        def s_push_back():
            obj = self.arg(0)
            self.str_set(obj, self.str_get(obj) + bytes([self.arg(1) & 0xff]))
            self.ret(0)
        h['__ZNSs9push_backEc'] = s_push_back

        def s_append_ptrlen():
            obj = self.arg(0)
            self.str_set(obj, self.str_get(obj) + (self.rd(self.arg(1), self.arg(2)) if self.arg(2) else b''))
            self.ret(obj)
        h['__ZNSs6appendEPKcm'] = s_append_ptrlen

        def s_append_str():
            obj = self.arg(0)
            self.str_set(obj, self.str_get(obj) + self.str_get(self.arg(1)))
            self.ret(obj)
        h['__ZNSs6appendERKSs'] = s_append_str

        def s_dtor():
            self.ret(0)
        h['__ZNSsD1Ev'] = s_dtor
        h['__ZNSsD2Ev'] = s_dtor
        h['__ZNSs4_Rep10_M_destroyERKSaIcE'] = s_dtor
        h['__ZNSs4_Rep9_S_createEmmRKSaIcE'] = lambda: (_ for _ in ()).throw(RuntimeError('_S_create'))

        def s_reserve():
            self.ret(0)
        h['__ZNSs7reserveEm'] = s_reserve

        def s_resize():
            obj, n, c = self.arg(0), self.arg(1), self.arg(2) & 0xff
            s = self.str_get(obj)
            s = s[:n] + bytes([c]) * max(0, n - len(s))
            self.str_set(obj, s)
            self.ret(0)
        h['__ZNSs6resizeEmc'] = s_resize

        def s_compare_cstr():
            a = self.str_get(self.arg(0))
            b = self.cstr(self.arg(1))
            self.ret(-1 if a < b else (1 if a > b else 0))
        h['__ZNKSs7compareEPKc'] = s_compare_cstr

        def s_compare():
            a = self.str_get(self.arg(0))
            b = self.str_get(self.arg(1))
            self.ret(-1 if a < b else (1 if a > b else 0))
        h['__ZNKSs7compareERKSs'] = s_compare

        def s_mutate():
            # _M_mutate(pos, len1, len2): make room; only used through other ops here
            raise RuntimeError('_M_mutate')
        h['__ZNSs9_M_mutateEmmm'] = s_mutate

        def s_leak():
            self.ret(0)
        h['__ZNSs12_M_leak_hardEv'] = s_leak

        def s_find_char():
            obj, c, pos = self.arg(0), self.arg(1) & 0xff, self.arg(2)
            i = self.str_get(obj).find(bytes([c]), pos)
            self.ret(i if i >= 0 else 0xffffffff)
        h['__ZNKSs4findEcm'] = s_find_char

        def s_substr():
            # returns by hidden pointer (arg0 = result, arg1 = this, pos, n)
            res, obj, pos, n = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
            self.str_set(res, self.str_get(obj)[pos:pos + n])
            self.ret(res)
        h['__ZNKSs6substrEmm'] = s_substr
        return h

    def _hook(self, uc, addr, _size, _user):
        name = self.m.imports.get(addr)
        if name is None:
            return
        fn = self.handlers.get(name)
        if fn is None:
            raise RuntimeError('unhandled import %s (from 0x%x)' % (name, self.u32(self.uc.reg_read(UC_X86_REG_ESP))))
        fn()

    def call(self, addr, *args):
        sp = STACK - 0x100 - 4 * len(args)
        sp &= ~0xf
        for i, v in enumerate(args):
            self.w32(sp + 4 * i, v)
        sp -= 4
        self.w32(sp, RET)
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        try:
            self.uc.emu_start(addr, RET, count=50_000_000)
        except UcError as e:
            eip = self.uc.reg_read(UC_X86_REG_EIP)
            raise RuntimeError('emulation error %s at 0x%x' % (e, eip)) from None
        return self.uc.reg_read(UC_X86_REG_EAX)

    def sym(self, name):
        return self.m.symbol(name)


# --- the original's read / convert / write ---------------------------------------

PATCH_SIZE = {v: 0x264 if v <= 12 else 0x284 for v in range(11, 24)}  # operator new sizes in
PERF_SIZE = {v: 0xa18 if v <= 12 else 0xa98 for v in range(11, 24)}   # CreateDataFile @00074ef8


class Converter:
    def __init__(self, binary):
        e = self.e = ConvEmu(binary)
        e.call(e.sym('__Z8CRC_Initv'))
        for name, addr, size, _off, _r1, _r2 in e.m.sections:
            if name != '__mod_init_func':
                continue
            for i in range(size // 4):
                try:
                    e.call(e.u32(addr + 4 * i))
                except Exception:  # noqa: BLE001  UI initializers; not needed here
                    pass
        e.heap_reset_base = e.brk

    def new_stream(self):
        e = self.e
        s = e.alloc(0x10020)  # CFileByteStream: 0x18 + 64 KiB buffer + 4
        e.call(e.sym('__ZN15CFileByteStreamC1Ev'), s)
        return s

    def convert(self, data):
        """-> (declared version, the converted file from the version word to the CRC)."""
        e = self.e
        e.heap_reset()
        if not data or len(data) >= 0x10000:
            raise Thrown('file size')  # CFileByteStream::ReadFromFile @0006c472
        s = self.new_stream()
        e.uc.mem_write(s + 0x18, data)
        e.w32(s + 0x10, len(data))
        lines = []
        for _ in range(4):
            so = e.alloc(4)
            e.w32(so, EMPTY_REP + 12)
            e.call(e.sym('__ZN15CFileByteStream7GetLineEPSs'), s, so)
            lines.append(e.str_get(so))
        kind = {b'Type=Patch': 'CPatchFile_%d', b'Type=Performance': 'CPerformanceFile_%d'}.get(lines[1])
        version = int(lines[2].split(b'=', 1)[1] or b'0') if b'=' in lines[2] else 0
        if kind is None or not 11 <= version <= 23:
            raise Thrown('Unsupported file version')
        cls = kind % version
        obj = e.alloc((PATCH_SIZE if kind.startswith('CPatch') else PERF_SIZE)[version])
        e.call(e.sym('__ZN%d%sC1Ev' % (len(cls), cls)), obj)
        e.call(e.u32(e.u32(obj) + 0xc), obj, s)  # Read
        holder = e.alloc(4)
        e.w32(holder, obj)
        conv = e.alloc(0x20)
        e.call(e.sym('__ZN14CFileConverterC1ERSt8auto_ptrI9CFileDataENS1_8EVersionE'), conv, holder, 23)
        out = e.call(e.sym('__ZN14CFileConverter16GetConvertedFileEv'), conv)
        s2 = self.new_stream()
        e.call(e.u32(e.u32(out) + 0x10), out, s2)  # Write
        n = e.call(e.sym('__ZNK10CBitStream12GetUsedBytesEv'), s2)
        return version, e.rd(s2 + 0x18, n)


# --- test inputs and comparison ------------------------------------------------------

def declared_as(data, version):
    """The same file declared as `version` (text header and version word; CRC
    redone). Before 13 a patch has no settings custom data (its first 5B)."""
    j = pch2dump.decode_file(data)
    j['text_header'][2] = 'Version=%d' % version
    j['version'] = version
    if version < 13:
        out, customs = [], 0
        for sec in j['sections']:
            if sec['id'] == '0x21':
                customs = 0
            if sec['id'] == '0x5B':
                customs += 1
                if customs == 1:
                    continue
            out.append(sec)
        j['sections'] = out
    j['crc_valid'] = True
    return pch2dump.encode_file(j)


def normalized(body):
    """A file body (version word .. CRC) with release velocity = attack velocity, as
    CCurrentVoiceData_11::WriteStream @00064aca writes them; CRC redone."""
    j = pch2dump.decode_file(b'X\r\n' + body)
    for sec in j['sections']:
        if sec['id'] == '0x69':
            for v in [sec['last']] + sec['notes']:
                v['release_vel'] = v['attack_vel']
    j['crc_valid'] = True
    j['text_header'] = []
    return pch2dump.encode_file(j)


def body_of(data):
    p = 0
    for _ in range(4):
        p = data.index(b'\r\n', p) + 2
    return data[p:]


GOLDEN_VERSIONS = range(12, 23)

_worker = {}


def _init(binary, g2tool):
    _worker['conv'] = Converter(binary)
    _worker['g2tool'] = g2tool


def _compare_one(item):
    path, version = item
    tag = '%s%s' % (path, '' if version is None else ' as %d' % version)
    try:
        data = open(path, 'rb').read()
        if version is not None:
            data = declared_as(data, version)
        try:
            original = normalized(_worker['conv'].convert(data)[1])
        except Exception as ex:  # noqa: BLE001
            original = 'original fails: %s' % ex
        with tempfile.TemporaryDirectory() as td:
            src, dst = os.path.join(td, 'in'), os.path.join(td, 'out')
            open(src, 'wb').write(data)
            r = subprocess.run([_worker['g2tool'], 'convert', src, dst], capture_output=True, text=True)
            ours = normalized(body_of(open(dst, 'rb').read())) if r.returncode == 0 else 'fails: ' + r.stderr.strip()
        if isinstance(original, str) or isinstance(ours, str):
            both = isinstance(original, str) and isinstance(ours, str)
            return ('both fail' if both else 'DIFFER', tag,
                    '%s | %s' % (original if isinstance(original, str) else 'original ok',
                                 ours if isinstance(ours, str) else 'ours ok'))
        if original == ours:
            return ('match', tag, '')
        k = next((i for i in range(min(len(original), len(ours))) if original[i] != ours[i]), None)
        return ('DIFFER', tag, 'original %d bytes, ours %d, first difference at %s' % (len(original), len(ours), k))
    except Exception as ex:  # noqa: BLE001
        return ('error', tag, repr(ex))


def main():
    args = sys.argv[1:]
    if len(args) < 3 or args[1] not in ('golden', 'compare'):
        raise SystemExit(__doc__)
    binary, mode = args[0], args[1]
    if mode == 'golden':
        conv = Converter(binary)
        with open(args[2], 'w') as out:
            out.write('# file\tversion\tsha256  (tools/convert/emulate.py golden: the original editor v1.62 under emulation)\n')
            for path in sorted(args[3:], key=os.path.basename):
                data = open(path, 'rb').read()
                for v in GOLDEN_VERSIONS:
                    raw = conv.convert(declared_as(data, v))[1]
                    if raw[:2] != b'\x00\x17':
                        # The conversion went through undefined behaviour: a
                        # parameter removed past the end of a module's list
                        # (list::erase(end()) in CModuleParamData_11::RemoveParam
                        # @0006831e), which a real file of that version cannot
                        # cause, corrupted the converted object (its version too).
                        out.write('# %s\t%d\tundefined in the original\n' % (os.path.basename(path), v))
                        continue
                    body = normalized(raw)
                    out.write('%s\t%d\t%s\n' % (os.path.basename(path), v, hashlib.sha256(body).hexdigest()))
        return
    rest = args[2:]
    versions, jobs = [None], os.cpu_count() or 4
    if '--as' in rest:
        i = rest.index('--as')
        versions = [int(x) for x in rest[i + 1].split(',')]
        del rest[i:i + 2]
    if '--jobs' in rest:
        i = rest.index('--jobs')
        jobs = int(rest[i + 1])
        del rest[i:i + 2]
    if '--list' in rest:
        i = rest.index('--list')
        rest += [line.rstrip('\n') for line in open(rest[i + 1]) if line.strip()]
        del rest[i:i + 2]
    g2tool, files = rest[0], rest[1:]
    items = [(f, v) for f in files for v in versions]
    with multiprocessing.Pool(jobs, initializer=_init, initargs=(binary, g2tool)) as pool:
        results = pool.map(_compare_one, items, chunksize=4)
    counts = {}
    for status, tag, detail in results:
        counts[status] = counts.get(status, 0) + 1
        if status != 'match':
            print('%s\t%s\t%s' % (status, tag, detail))
    print(' '.join('%s: %d' % kv for kv in sorted(counts.items())))
    sys.exit(0 if counts.get('match', 0) + counts.get('both fail', 0) == len(results) else 1)


if __name__ == '__main__':
    main()
