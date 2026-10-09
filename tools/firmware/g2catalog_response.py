#!/usr/bin/env python3
"""Measure each catalogued module's response offline, on the DSP memories that
`g2catalog.py --response` dumped (OscA -> module input 0 -> 2-Out, compiled by
the G2's own OS), with emu/dspframe/g2dspframe.

  g2catalog_response.py [--catalog DIR] [--frame PATH] [MODULE...]

For each <Module>/resp/: one traced frame finds the cable the module reads (the
one the source OscA writes) and the cable(s) the 2-Out reads; then the module's
input is driven, right before the instruction that reads it, with sines at a
few frequencies and with DC, and the output cable is recorded. Writes
<Module>/resp/response.json: gain in dB per frequency, DC gain, and the
harmonics of the 1 kHz response (for shapers). Frames are taken as 96 kHz
samples (one frame per sample, see re/notes/g2-hardware-and-emulation.md 3.6.6).
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FS = 96000
SHORT = re.compile(r'x:\$([0-9a-f]+)(?![0-9a-f])')


def trace(frame, rdir, dsp):
    # four frames: control-rate code (every 4th sample) runs in one of them
    r = subprocess.run([frame, rdir, str(dsp), '4', 'trace'], capture_output=True, text=True)
    steps = []
    for line in r.stderr.splitlines():
        m = re.match(r'^([0-9a-f]+): (.*)$', line)
        if m:
            steps.append((int(m.group(1), 16), m.group(2)))
    return steps


BITOPS = ('btst', 'jclr', 'jset', 'brclr', 'brset', 'bsclr', 'bsset', 'jsclr', 'jsset')


def cable_access(text):
    """[(slot, 'load'|'store')] for the zero-page X words an instruction touches.
    Bit tests and conditional jumps on a bit read their word."""
    out = []
    mnemonic = text.split()[0] if text.split() else ''
    if mnemonic in BITOPS:
        return [(int(m.group(1), 16), 'load') for m in SHORT.finditer(text) if int(m.group(1), 16) < 0x40]
    for m in SHORT.finditer(text):
        slot = int(m.group(1), 16)
        if slot >= 0x40:
            continue
        before = text[:m.start()].rstrip()
        out.append((slot, 'store' if before.endswith(',') else 'load'))
    return out


def find_cables(steps):
    """(input slot, PC of its first read by the module, output slots read last by the 2-Out)."""
    acc = [(pc, slot, kind) for pc, t in steps for slot, kind in cable_access(t)]
    stores = [(pc, s) for pc, s, k in acc if k == 'store']
    if not stores:
        return None
    src_pc, src = stores[0]
    first_read = next((pc for pc, s, k in acc if k == 'load' and s == src and pc != src_pc), None)
    loads = [s for pc, s, k in acc if k == 'load' and s != src]
    outs = []
    for s in reversed(loads):
        if s not in outs:
            outs.append(s)
        if len(outs) == 2:
            break
    return src, first_read, outs[::-1]


def run(frame, rdir, dsp, frames, src, at, outs, signal):
    dump = os.path.join(rdir, '_tmp.f32')
    cmd = [frame, rdir, str(dsp), str(frames), f'in=X:{src:x}:{signal}', f'inat={at:x}', f'dump={dump}']
    cmd += [f'watch=X:{o:x}' for o in outs]
    subprocess.run(cmd, capture_output=True, text=True)
    import array
    a = array.array('f')
    with open(dump, 'rb') as f:
        a.frombytes(f.read())
    os.remove(dump)
    n = len(outs)
    return [list(a[k::n]) for k in range(n)]


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def tone_level(x, freq):
    """Amplitude of one frequency component (single-bin DFT over whole periods)."""
    n = len(x)
    c = sum(v * math.cos(2 * math.pi * freq * k / FS) for k, v in enumerate(x))
    s = sum(v * math.sin(2 * math.pi * freq * k / FS) for k, v in enumerate(x))
    return 2 * math.hypot(c, s) / n


def analyse(frame, rdir):
    info = json.load(open(os.path.join(rdir, 'resp.json')))
    res = {'source': info.get('source')}
    for dsp in info.get('dsps', []):
        steps = trace(frame, rdir, dsp)
        cab = find_cables(steps)
        if not cab or cab[1] is None:
            continue
        src, at, outs = cab
        res.update({'dsp': dsp, 'inputCable': src, 'readAt': at, 'outputCables': outs,
                    'frameInstructions': round(len(steps) / 4, 1)})
        gains = {}
        for f in (50, 100, 300, 1000, 3000, 10000, 20000):
            ys = run(frame, rdir, dsp, 19200, src, at, outs, f'sine:{f}')
            y = ys[0][9600:]
            gains[f] = round(20 * math.log10(max(tone_level(y, f), 1e-9) / 0.5), 2)
        res['gainDb'] = gains
        ys = run(frame, rdir, dsp, 9600, src, at, outs, 'dc:0.25')
        res['dcOut'] = [round(sum(y[4800:]) / 4800, 5) for y in ys]
        ys = run(frame, rdir, dsp, 19200, src, at, outs, 'sine:1000')
        y = ys[0][9600:]
        fund = tone_level(y, 1000)
        res['harmonicsDb1k'] = [round(20 * math.log10(max(tone_level(y, 1000 * h), 1e-9) / max(fund, 1e-9)), 1)
                                for h in range(2, 7)]
        res['rms1k'] = round(rms(y), 5)
        break
    json.dump(res, open(os.path.join(rdir, 'response.json'), 'w'), indent=1)
    return res


def frame_instructions(frame, d, dsp=3):
    r = subprocess.run([frame, d, str(dsp), '4'], capture_output=True, text=True)
    m = re.search(r'\(([0-9.]+) per frame\)', r.stdout)
    return float(m.group(1)) if m else None


def costs(frame, catalog):
    """Instructions per frame (one sample) of each module's patch minus those of the
    baseline patch (2-Out alone), from the memories g2catalog.py dumped: the module's
    audio-rate cost in DSP instructions (control-rate parts run every 4th frame)."""
    base = {tag: frame_instructions(frame, os.path.join(catalog, f'_baseline_{tag}'))
            for tag in ('VA', 'FX') if os.path.exists(os.path.join(catalog, f'_baseline_{tag}', 'dsp3_live_P.bin'))}
    out = {}
    for d in sorted(os.listdir(catalog)):
        mdir = os.path.join(catalog, d)
        if d.startswith('_') or not os.path.exists(os.path.join(mdir, 'module.json')) \
                or not os.path.exists(os.path.join(mdir, 'dsp3_live_P.bin')):
            continue
        e = json.load(open(os.path.join(mdir, 'module.json')))
        b = base.get(e.get('area'))
        # four frames: the control-rate (every 4th sample) code runs once in them
        n = frame_instructions(frame, mdir)
        if n is None or b is None:
            continue
        out[d] = {'instructionsPerFrame': round(n - b, 2), 'estimateCycles': (e.get('estimate') or {}).get('cyclesA'),
                  'estimateCyclesB': (e.get('estimate') or {}).get('cyclesB')}
    json.dump({'baseline': base, 'modules': out}, open(os.path.join(catalog, 'costs.json'), 'w'), indent=1)
    return base, out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--catalog', default=os.path.join(ROOT, 'original', 'firmware', 'catalog'))
    ap.add_argument('--frame', default=os.path.join(ROOT, 'build', 'emu', 'g2dspframe'))
    ap.add_argument('--costs', action='store_true', help='measure instructions per frame of every module instead')
    ap.add_argument('modules', nargs='*')
    a = ap.parse_args(argv)
    if a.costs:
        base, out = costs(a.frame, a.catalog)
        print('baseline', base)
        for k, v in out.items():
            print(f'{k:<12} {v}')
        return 0
    for d in sorted(os.listdir(a.catalog)):
        rdir = os.path.join(a.catalog, d, 'resp')
        if not os.path.exists(os.path.join(rdir, 'resp.json')) or (a.modules and d not in a.modules):
            continue
        try:
            r = analyse(a.frame, rdir)
        except Exception as ex:
            r = {'error': repr(ex)}
        print(f'{d:<12} {r}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
