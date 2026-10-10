#!/usr/bin/env python3
"""Speed benchmark of the C++ emulator (g2emurun) over a set of patches and thread settings
(re/notes/g2-hardware-and-emulation.md §3.9.7).

  g2emubench.py [--threads 0,1,2] [--seconds 3] [--patches A.pch2,B.pch2] [--chord 60,64,67] [--repeat N]
                [--load N] [--realtime S] [--json OUT.json] [-- EXTRA g2emurun ARGS]

Per patch and thread setting it runs `g2emurun --model g2x --kbd PATCH` (the patch in slot A on the keyboard) with
a chord held through MIDI over the whole recording, one run at a time, and reports the speed (emulated seconds per
wall second), the CPU per emulated second (all threads), the busy CPU (waits taken out) per emulated second of the
ColdFire thread and of each DSP thread, and the
share of ColdFire time in skipped polls. --repeat keeps the fastest of N runs (the least disturbed). --realtime S
instead plays S seconds through Runner + LocalLink at the wall clock (`g2emurun --realtime`) and reports the frames
the reader found missing. --load N keeps N busy processes (`yes > /dev/null`) running meanwhile, killed at the end.
On macOS the emulator runs under `taskpolicy -a` (an application's scheduling policies, as in a DAW; --no-app: not).

Measure with nothing else heavy running; one emulator at a time.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EMU = os.path.join(ROOT, 'build', 'emu', 'g2emurun')
TESTS = os.path.join(ROOT, 'original', 'firmware', 'g2emu-tests')
FACTORY = os.path.join(ROOT, 'corpus-external', 'nord-factory-bank-v1.24', 'Nord Modular G2 Factory Bank v1.24 revA')
DEFAULT_PATCHES = [
    os.path.join(TESTS, 'drone.pch2'),        # the user's Drone (light)
    os.path.join(TESTS, '1khz_on.pch2'),
    os.path.join(TESTS, 'kbd_sine.pch2'),
    os.path.join(TESTS, 'organ.pch2'),
    os.path.join(TESTS, 'compkeys.pch2'),
    os.path.join(TESTS, 'bigsynth.pch2'),     # heavy
    os.path.join(FACTORY, 'PatchBank1', 'Vangel.pch2'),
    os.path.join(FACTORY, 'PatchBank2', 'Comp Keys1.pch2'),
    os.path.join(FACTORY, 'PatchBank2', 'VintageOrgan3.pch2'),
]


def parse(out):
    r = {}
    m = re.search(r'speed: ([\d.]+) emulated s in ([\d.]+) wall s = ([\d.]+)x real time, ([\d.]+) CPU s', out)
    if m:
        r['speed'] = float(m.group(3))
        r['cpu'] = float(m.group(4))
    m = re.search(r'ColdFire \(and the caller\) ([\d.]+) CPU s per emulated s(, of which waiting ([\d.]+) s)?', out)
    if m:
        r['cf'] = float(m.group(1))
        r['cf_wait'] = float(m.group(3) or 0)
    r['dsp'] = [(float(a), float(b)) for a, b in re.findall(r'DSP thread \d ([\d.]+) CPU s, of which waiting ([\d.]+) s', out)]
    m = re.search(r'host port: (\d+) reads/s.*skipped polls ([\d.]+)%', out)
    if m:
        r['reads'] = int(m.group(1))
        r['skipped'] = float(m.group(2))
    m = re.search(r'recorded:.*underruns (\d+)', out)
    if m:
        r['underruns'] = int(m.group(1))
    m = re.search(r'realtime: ([\d.]+) s of audio, (\d+) frames missing \(([\d.]+)%\) in (\d+) of (\d+) blocks.*?speed while running ([\d.]+)x', out)
    if m:
        r['missing'] = int(m.group(2))
        r['missing_pct'] = float(m.group(3))
        r['gaps'] = int(m.group(4))
        r['blocks'] = int(m.group(5))
        r['rt_speed'] = float(m.group(6))
    else:
        # g2emurun before 2026-10-10's speed work: no block count
        m = re.search(r'realtime: ([\d.]+) s of audio, (\d+) frames missing \(([\d.]+)%\)', out)
        if m:
            r.update(missing=int(m.group(2)), missing_pct=float(m.group(3)), gaps=-1, blocks=int(float(m.group(1)) * 100),
                     rt_speed=0.0)
    m = re.search(r'out 1: peak ([\d.]+) rms ([\d.]+)', out)
    if m:
        r['peak'] = float(m.group(1))
    r['wild'] = 'WARNING' in out
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--threads', default='0,1,2')
    ap.add_argument('--seconds', type=float, default=3)
    ap.add_argument('--patches', default='')
    ap.add_argument('--chord', default='60,64,67')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--load', type=int, default=0)
    ap.add_argument('--realtime', type=float, default=0)
    ap.add_argument('--emu', default=EMU)
    ap.add_argument('--no-app', dest='app', action='store_false', help='macOS: not under taskpolicy -a')
    ap.add_argument('--json')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    patches = [p for p in (a.patches.split(',') if a.patches else DEFAULT_PATCHES) if p]
    missing = [p for p in patches if not os.path.exists(p)]
    for p in missing:
        print(f'(skipped, not found: {p})', file=sys.stderr)
    patches = [p for p in patches if os.path.exists(p)]
    threads = [t for t in a.threads.split(',') if t != '']
    load = [subprocess.Popen(['yes'], stdout=subprocess.DEVNULL) for _ in range(a.load)]
    results = []
    try:
        for p in patches:
            name = os.path.splitext(os.path.basename(p))[0]
            for t in threads:
                best = None
                for _ in range(a.repeat):
                    cmd = [a.emu, '--model', 'g2x', '--kbd', p, '--threads', t]
                    if a.app and sys.platform == 'darwin':
                        # an application's scheduling policies, as a DAW has them: without them (a process started by
                        # a daemon, an agent) macOS ignores the threads' QoS class and they compete with any process
                        cmd = ['taskpolicy', '-a'] + cmd
                    if a.realtime:
                        cmd += ['--realtime', str(a.realtime)]
                        end = a.realtime
                    else:
                        cmd += ['--seconds', str(a.seconds)]
                        end = a.seconds + 1
                    for n in a.chord.split(','):
                        if n:
                            cmd += ['--midi', f'{n}@0-{end}:1']
                    cmd += a.extra
                    pr = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
                    r = parse(pr.stdout + pr.stderr)
                    r['rc'] = pr.returncode
                    key = 'missing' if a.realtime else 'speed'
                    if key not in r:
                        sys.stderr.write(f'{name} threads {t}: no result (rc {pr.returncode})\n{pr.stderr[-2000:]}\n')
                        continue
                    better = (r[key] < best[key]) if a.realtime and best else (best is None or r[key] > best[key])
                    if best is None or better:
                        best = r
                if best is None:
                    continue
                best.update(patch=name, threads=t)
                results.append(best)
                if a.realtime:
                    print(f'{name:16} t{t:>2}  missing {best["missing"]:7d} frames ({best["missing_pct"]:5.2f}%) in {best["gaps"]} of '
                          f'{best["blocks"]} blocks  speed while running {best.get("rt_speed", 0):.2f}x  peak {best.get("peak", 0):.4f}', flush=True)
                else:
                    dsp = ' '.join(f'{c - w:.2f}' for c, w in best['dsp'])
                    print(f'{name:16} t{t:>2}  {best["speed"]:5.2f}x  cpu {best["cpu"]:.2f}  busy: cf {best.get("cf", 0) - best.get("cf_wait", 0):.2f} '
                          f'dsp {dsp:15}  polls {best.get("skipped", 0):4.1f}%  reads/s {best.get("reads", 0):7d}'
                          f'{"  WILD" if best["wild"] else ""}', flush=True)
    finally:
        for pr in load:
            pr.kill()
            pr.wait()
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(results, f, indent=1)


if __name__ == '__main__':
    main()
