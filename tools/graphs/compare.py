#!/usr/bin/env python3
"""Compare the C++ graph port with the original code run in the emulator.

  compare.py BINARY DUMP IDS... [--random N] [--cache DIR] [--show K]

DUMP is a program that reads "ID V0 V1 ..." lines on stdin and prints the
port's primitives in the emulator's format (see emulate.py). The emulator's
output for each id is cached in DIR (default: no cache).
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cases as casesmod  # noqa: E402

IGNORE = ('DEP', 'SIZE')


def parse(text):
    res = {}
    cur = None
    for line in text.splitlines():
        if line.startswith('# '):
            cur = line[2:].strip()
            res[cur] = []
        elif line and cur is not None and not line.startswith(IGNORE):
            res[cur].append(line.strip())
    return res


def emulate(binary, gid, lines, cache):
    path = None
    if cache:
        os.makedirs(cache, exist_ok=True)
        path = os.path.join(cache, '%d.txt' % gid)
        if os.path.exists(path):
            got = parse(open(path).read())
            if all(ln in got for ln in lines):
                return got
    from emulate import GraphEmu
    emu = GraphEmu(binary)
    out = []
    for ln in lines:
        nums = [int(x) for x in ln.split()]
        out.append('# ' + ln)
        try:
            ops = emu.draw(nums[0], nums[1:])
            out.extend(' '.join(str(x) for x in op) for op in ops if op[0] not in IGNORE)
        except Exception as e:  # noqa: BLE001
            out.append('ERROR %s' % e)
    text = '\n'.join(out) + '\n'
    if path:
        open(path, 'w').write(text)
    return parse(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('dump')
    ap.add_argument('ids', nargs='+', type=int)
    ap.add_argument('--random', type=int, default=40)
    ap.add_argument('--cache')
    ap.add_argument('--show', type=int, default=2)
    a = ap.parse_args()
    total_bad = 0
    for gid in a.ids:
        lines = ['%d %s' % (gid, ' '.join(map(str, t))) for t in casesmod.cases(gid, a.random)]
        ref = emulate(a.binary, gid, lines, a.cache)
        p = subprocess.run([a.dump], input='\n'.join(lines) + '\n', capture_output=True, text=True)
        got = parse(p.stdout)
        bad = 0
        shown = 0
        for ln in lines:
            r, g = ref.get(ln, []), got.get(ln, [])
            if r != g:
                bad += 1
                if shown < a.show:
                    shown += 1
                    print('--- id %d case: %s' % (gid, ln))
                    n = max(len(r), len(g))
                    first = next((i for i in range(n) if (r[i] if i < len(r) else None) !=
                                  (g[i] if i < len(g) else None)), n)
                    for i in range(max(0, first - 3), min(n, first + 8)):
                        rr = r[i] if i < len(r) else '-'
                        gg = g[i] if i < len(g) else '-'
                        print('  %s %-32s | %s' % ('*' if rr != gg else ' ', rr, gg))
        total_bad += bad
        print('id %d: %d/%d cases match' % (gid, len(lines) - bad, len(lines)))
    sys.exit(1 if total_bad else 0)


if __name__ == '__main__':
    main()
