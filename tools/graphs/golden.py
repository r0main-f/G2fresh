#!/usr/bin/env python3
"""Generate the expected values of tests/test_graphs.cpp from the original code.

  golden.py BINARY [--cache DIR]  > table.inc

For every Graph Func id in cases.RANGES, runs the original graph code in the
emulator (emulate.py) on the cases of cases.py and prints one C++ initializer
per id: number of cases, FNV-1a 64 digest of the whole output, and a spot
check (the all-mid case: number of primitives, first and last lines).

Digest input, per case: "ID V0 V1 ...\\n" then every primitive line followed
by "\\n", in the format of emulate.py (DEP/SIZE lines excluded).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cases as casesmod  # noqa: E402
from compare import emulate  # noqa: E402

HEAD, TAIL = 8, 4


def fnv1a64(data, h=0xcbf29ce484222325):
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('--cache')
    a = ap.parse_args()
    for gid in sorted(casesmod.RANGES):
        tuples = casesmod.cases(gid)
        lines = ['%d %s' % (gid, ' '.join(map(str, t))) for t in tuples]
        ref = emulate(a.binary, gid, lines, a.cache)
        h = 0xcbf29ce484222325
        for ln in lines:
            if any(op.startswith('ERROR') for op in ref[ln]):
                raise SystemExit('emulator error for %s' % ln)
            h = fnv1a64((ln + '\n').encode(), h)
            for op in ref[ln]:
                h = fnv1a64((op + '\n').encode(), h)
        spot = tuples[2]
        ops = ref[lines[2]]
        head = ops[:HEAD]
        tail = ops[-TAIL:] if len(ops) > HEAD else []
        print('    {%d, %d, 0x%016xull, {%s}, %d,' % (gid, len(lines), h, ', '.join(map(str, spot)), len(ops)))
        print('     {%s},' % ', '.join(cstr(x) for x in head))
        print('     {%s}},' % ', '.join(cstr(x) for x in tail))


if __name__ == '__main__':
    main()
