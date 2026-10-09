#!/usr/bin/env python3
"""Catalog what the G2's OS builds on its DSPs for each module type.

  nice -n 10 g2catalog.py [--modules OscA,OscB,...|all] [--out DIR] [--sweep N]

Boots the user's own OS once in the emulator of g2hostemu.py (four emulated
DSP56367s, our protocol client on the emulated USB), then for each module:
  1. uploads a patch with that one module (and a 2-Out fed by its outputs) into
     slot A, in the VA area (FX when the module is FX-only);
  2. records what the OS writes to each DSP through the host ports (the stage-1
     monitor's host commands, decoded to X/Y/P writes), and dumps the DSPs'
     memories; the linked program is compared with the baseline (a patch with
     only the 2-Out) and with the OS's fragment library (g2frags.py output);
  3. changes each parameter and mode live (proto Client::setParam/setMode) to a
     few values and records the DSP words the OS writes for each.
Output (derived from Clavia's firmware: keep it out of git) under --out
(default original/firmware/catalog): <Module>/module.json, <Module>/dspN_live_*.bin,
and catalog.json (an index). See re/notes/dsp-module-catalog.md.

Needs the venv and libraries of g2hostemu.py, and original/firmware/fragments
(tools/firmware/g2frags.py).
"""
import argparse
import collections
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import g2hostemu as H  # noqa: E402

ROOT = H.ROOT
VA, FX = 1, 0


# --------------------------------------------------------------------------
class MonitorDecoder:
    """Incremental decoder of one DSP's host-port stream (see g2hostemu.decode_stream):
    the boot-ROM load, then the stage-1 monitor's host commands."""

    def __init__(self):
        self.pos = 0
        self.boot = []
        self.booted = False
        self.pend = collections.deque()
        self.r0 = 0
        self.slips = 0  # words that no command took

    def feed(self, events):
        """Decode events[self.pos:]; returns the new records (hv, act, space, addr, value)."""
        out = []
        for k, v in events[self.pos:]:
            if not self.booted:
                if k == 'w':
                    self.boot.append(v)
                    if len(self.boot) >= 2 and len(self.boot) == self.boot[0] + 2:
                        self.booted = True
                continue
            if k == 'w':
                self.pend.append(v)
                continue
            if k != 'cvr' or not v & 0x80:
                continue
            hv = v & 0x7f
            act, space = H.MONITOR.get(hv, ('?', None))
            take = act in ('w+', 'w', 'rw', 'la', 'r0', 'dor0', 'dor1', 'dco1', 'tlr0', 'echo', 'ep', 'wy+ep')
            # the OS writes a command's word right before the command: take the newest
            # word and drop older ones, so that one stray word cannot shift the rest
            val = self.pend[-1] if take and self.pend else None
            self.slips += max(0, len(self.pend) - (1 if take else 0))
            self.pend.clear()
            if act == 'r0':
                self.r0 = val if val is not None else self.r0
            out.append((hv, act, space, self.r0, val))
            if act in ('w+', 'r+'):
                self.r0 = (self.r0 + 1) & 0xffffff
        self.pos = len(events)
        return out


def writes_of(records, include_rw=False):
    """{(space, addr): value} for the memory writes among decoded records (last one wins)."""
    w = {}
    for hv, act, space, addr, val in records:
        if val is None:
            continue
        if act in ('w+', 'w') or (include_rw and act == 'rw'):
            w[(space, addr)] = val
        elif act in ('dor0', 'dor1', 'dco1', 'la', 'tlr0'):
            w[(act, 0)] = val
    return w


# --------------------------------------------------------------------------
class Fragments:
    """The OS's DSP fragment library (g2frags.py output)."""

    def __init__(self, path):
        self.frags = {}
        for f in glob.glob(os.path.join(path, '*.frag')):
            lines = open(f).read().split('\n')
            P = [int(x, 16) for x in lines[0].split()[2:]]
            if len(P) >= 3:
                self.frags[os.path.basename(f)[:-5]] = P
        self.index = json.load(open(os.path.join(path, 'index.json')))
        self.meta = {e['descriptor']: e for e in self.index}

    def match(self, prog, base):
        """Greedy cover of `prog` (list of words at P:base) by fragments: each match
        allows up to a quarter of differing words (the OS patches addresses and
        constants into them). Returns [{fragment, at, len, patched: [[i, lib, linked]]}]."""
        cands = []
        n = len(prog)
        pos = collections.defaultdict(list)
        for i, w in enumerate(prog):
            pos[w].append(i)
        for name, F in self.frags.items():
            L = len(F)
            if L > n:
                continue
            starts = set()
            for j in range(min(4, L)):
                for i in pos.get(F[j], ()):
                    if 0 <= i - j <= n - L:
                        starts.add(i - j)
            for s in starts:
                diff = [k for k in range(L) if prog[s + k] != F[k]]
                if len(diff) * 4 <= L:
                    cands.append((L - len(diff), L, s, name, diff))
        cands.sort(key=lambda c: (-c[0], c[2]))
        used = [False] * n
        out = []
        for score, L, s, name, diff in cands:
            if any(used[s:s + L]):
                continue
            for k in range(s, s + L):
                used[k] = True
            out.append({'fragment': name, 'at': base + s, 'len': L, 'cycles': self.meta.get(name, {}).get('cycles'),
                        'patched': [[k, F_k, prog[s + k]] for k, F_k in ((k, self.frags[name][k]) for k in diff)]})
        out.sort(key=lambda m: m['at'])
        return out, sum(used)


# --------------------------------------------------------------------------
class Session:
    def __init__(self, args):
        H.DspBridge.load(args.bridge)
        self.emu = H.Emu(args.fw, dsps=4)
        self.emu.log.once = lambda *a, **k: None  # quiet
        self.host = H.UsbHost(self.emu, args.protobridge)
        lib = self.host.lib
        import ctypes
        vp, i = ctypes.c_void_p, ctypes.c_int
        for name, res, argt in (('g2p_send_module_patch', i, [vp, i, i, i, i]), ('g2p_status', i, [vp]),
                                ('g2p_send_chain_patch', i, [vp, i, i, i, i, i]),
                                ('g2p_set_param', None, [vp, i, i, i, i, i, i]),
                                ('g2p_set_mode', None, [vp, i, i, i, i, i]),
                                ('g2p_play_note', None, [vp, i, i]),
                                ('g2p_module_cost', i, [i, i, ctypes.POINTER(ctypes.c_uint32)])):
            f = getattr(lib, name)
            f.restype, f.argtypes = res, argt
        self.ct = ctypes
        self.dec = {p.index: MonitorDecoder() for p in self.emu.host.ports if p.dsp is not None}
        self.ms = args.ms_per_slice
        self.emu.start()

    def advance(self, n, cond=None):
        """Run up to n slices (stepping the USB host each slice), stop early when cond()."""
        hit = [False]

        def until(em):
            self.host.step(self.ms)
            if cond is not None and cond():
                hit[0] = True
                return True
            return False
        self.emu.run(steps=n * 20000, slice_instr=20000, until=until)
        return hit[0]

    def drain(self):
        """New decoded records per DSP since the last call."""
        return {p.index: self.dec[p.index].feed(p.events) for p in self.emu.host.ports if p.dsp is not None}

    def boot(self, usb_start):
        t = time.time()
        self.advance(usb_start)
        self.host.start()
        ok = self.advance(3000, lambda: self.host.lib.g2p_synced(self.host.h))
        self.drain()
        print(f'booted and synced in {time.time() - t:.0f} s (slice {self.emu.slices}), sync={ok}', flush=True)
        return ok

    def settle(self, minimum, maximum=400):
        """Wait until the client is idle, then `minimum` more slices."""
        self.advance(maximum, lambda: self.host.lib.g2p_idle(self.host.h))
        self.advance(minimum)

    def upload(self, loc, type_, with_output=True):
        idx = self.host.lib.g2p_send_module_patch(self.host.h, 0, loc, type_, 1 if with_output else 0)
        if idx < 0:
            return idx, None
        self.settle(60)
        return idx, self.drain()

    def learn_noise(self, slices):
        """(port, space, addr) written during `slices` quiet slices."""
        self.drain()
        recs = {}
        for _ in range(slices // 10):
            self.advance(10)
            for port, r in self.drain().items():
                recs.setdefault(port, []).extend(r)
        return {(port, sp, a) for port, r in recs.items() for (sp, a) in writes_of(r)}

    def settle_writes(self, noise, quiet=30, maximum=800, summary=False):
        """Run until no write outside `noise` for `quiet` slices (the OS ramps some
        parameter words over many ticks). Returns the final value of each written word
        per DSP, how many times it was written (a ramp) and its first value."""
        final, count, first = {}, collections.Counter(), {}
        idle, n = 0, 0
        while n < maximum:
            self.advance(10)
            n += 10
            new = False
            for port, r in self.drain().items():
                for hv, act, sp, a, val in r:
                    if val is None:
                        continue
                    key = (port, sp if act in ('w+', 'w') else act, a if act in ('w+', 'w') else 0)
                    if act not in ('w+', 'w', 'dor0', 'dor1', 'dco1', 'la', 'tlr0') or (port, sp, a) in noise:
                        continue
                    final[key] = val
                    count[key] += 1
                    first.setdefault(key, val)
                    new = True
            idle = 0 if new else idle + 10
            if idle >= quiet and n >= 20:
                break
        out = {}
        for (port, sp, a), val in final.items():
            out.setdefault(str(port), []).append([sp, a, val, count[(port, sp, a)], first[(port, sp, a)]])
        for v in out.values():
            v.sort(key=lambda x: (str(x[0]), x[1]))
        res = {'writes': out, 'slices': n}
        if summary:
            res['summary'] = {port: summarize({(sp, a): val for sp, a, val, c, f0 in w}) for port, w in out.items()}
        return res

    def live(self, port, ranges):
        d = self.emu.host.ports[port].dsp
        return {sp: [d.mem_read(area, a) for a in range(lo, hi)] for sp, area, lo, hi in ranges}

    def cost(self, type_):
        buf = (self.ct.c_uint32 * 12)()
        if not self.host.lib.g2p_module_cost(type_, 0, buf):
            return None
        keys = ['cyclesA', 'cyclesB', 'zp', 'xA', 'yA', 'pA', 'xB', 'yB', 'pB', 'dynRam', 'qMem', 'rMem']
        return dict(zip(keys, list(buf)))


def program_range(writes):
    """The P writes as {addr: word}."""
    return {a: v for (sp, a), v in writes.items() if sp == 'P'}


def summarize(writes):
    """Writes grouped as contiguous ranges per space: {space: [[lo, hi, [words]]]}"""
    out = {}
    for sp in ('P', 'X', 'Y'):
        m = {a: v for (s, a), v in writes.items() if s == sp}
        out[sp] = [[lo, hi, [m[a] for a in range(lo, hi + 1)]] for lo, hi in H.runs(m.keys())]
    out['regs'] = {s: v for (s, a), v in writes.items() if s in ('dor0', 'dor1', 'dco1', 'la', 'tlr0')}
    return out


def sweep_values(lo, hi, n):
    if hi - lo + 1 <= n:
        return list(range(lo, hi + 1))
    vals = {lo, hi, lo + (hi - lo) // 4, lo + (hi - lo) // 2, lo + 3 * (hi - lo) // 4}
    if n > 5:
        step = (hi - lo) / (n - 1)
        vals |= {int(round(lo + k * step)) for k in range(n)}
    return sorted(vals)


# --------------------------------------------------------------------------
def catalog_module(S, mod, frags, base, out_dir, args):
    name, tid = mod['shortName'], mod['typeId']
    ctx = mod['flags'].get('contexts', [])
    loc = VA if 'va' in ctx else FX
    entry = {'type': tid, 'name': name, 'longName': mod['longName'], 'area': 'VA' if loc == VA else 'FX',
             'estimate': S.cost(tid)}
    t0 = time.time()
    idx, recs = S.upload(loc, tid)
    if idx < 0:
        entry['error'] = 'patch not built'
        return entry
    entry['moduleIndex'] = idx
    dsps = {}
    raw = {}
    for port, r in recs.items():
        w = writes_of(r)
        if not w:
            continue
        raw[port] = w
        bw = base[loc]['writes'].get(port, {})
        changed = {k: v for k, v in w.items() if bw.get(k) != v}
        dsps[port] = {'writes': summarize(w), 'changedVsBaseline': summarize(changed),
                      'commands': dict(collections.Counter(f'{hv * 2:02X}:{a}' for hv, a, s, ad, v in r))}
    # the DSP(s) whose program changed against the baseline
    mdir = os.path.join(out_dir, name.replace('/', '_'))
    os.makedirs(mdir, exist_ok=True)
    for port, info in dsps.items():
        if not info['changedVsBaseline']['P']:
            continue
        live = S.live(port, (('P', 0, 0, 0x1000), ('X', 1, 0, 0x2000), ('Y', 2, 0, 0x2000)))
        for sp, ws in live.items():
            with open(os.path.join(mdir, f'dsp{port}_live_{sp}.bin'), 'wb') as f:
                f.write(H.words_bin(ws))
        yext = S.live(port, (('Yext', 2, 0x800000, 0x800800),))['Yext']
        with open(os.path.join(mdir, f'dsp{port}_live_Yext.bin'), 'wb') as f:
            f.write(H.words_bin(yext))
        P = program_range(raw[port])
        if P and max(P) - min(P) > 0x2000:
            info['program'] = {'error': f'P writes span ${min(P):X}-${max(P):X}: not one program'}
            P = None
        if P:
            lo, hi = min(P), max(P)
            prog = [P.get(a, 0) for a in range(lo, hi + 1)]
            bprog = base[loc]['program'].get(port, [])
            info['program'] = {'start': lo, 'end': hi, 'words': len(prog),
                               'baselineWords': len(bprog), 'extraWords': len(prog) - len(bprog)}
            m, covered = frags.match(prog, lo)
            bset = {(x['fragment']) for x in base[loc]['fragments'].get(port, [])}
            info['fragments'] = [x for x in m if x['fragment'] not in bset]
            info['fragmentCoverage'] = covered
    entry['dsps'] = dsps
    entry['decoderSlips'] = {p: d.slips for p, d in S.dec.items()}
    entry['uploadSeconds'] = round(time.time() - t0, 1)

    # live parameter and mode changes. Words written while nothing changes (the
    # output DSP's level ramp, constant rewrites, polling) are learnt first and ignored.
    noise = S.learn_noise(args.quiet_slices)
    entry['noise'] = sorted(f'dsp{p}:{sp}:{a:04x}' for p, sp, a in noise)
    t1 = time.time()
    params = []
    for pi, p in enumerate(mod.get('params', [])):
        lo, hi = p.get('min', 0), p.get('max', 127)
        res = {'index': pi, 'name': p.get('name'), 'min': lo, 'max': hi, 'default': p.get('default'), 'values': []}
        for v in sweep_values(lo, hi, args.sweep):
            S.host.lib.g2p_set_param(S.host.h, 0, loc, idx, pi, v, 0)
            res['values'].append({'value': v, **S.settle_writes(noise)})
        S.host.lib.g2p_set_param(S.host.h, 0, loc, idx, pi, p.get('default', lo), 0)
        S.settle_writes(noise)
        params.append(res)
    entry['params'] = params
    modes = []
    for mi, md in enumerate(mod.get('modes', [])):
        lo, hi = md.get('min', 0), md.get('max', 0)
        res = {'index': mi, 'name': md.get('name'), 'min': lo, 'max': hi, 'values': []}
        for v in range(lo, hi + 1):
            S.host.lib.g2p_set_mode(S.host.h, 0, loc, idx, mi, v)
            S.advance(200, lambda: S.host.lib.g2p_idle(S.host.h))
            res['values'].append({'value': v, **S.settle_writes(noise, summary=True)})
        S.host.lib.g2p_set_mode(S.host.h, 0, loc, idx, mi, md.get('default', lo))
        S.advance(200, lambda: S.host.lib.g2p_idle(S.host.h))
        S.settle_writes(noise)
        modes.append(res)
    entry['modes'] = modes
    entry['paramSeconds'] = round(time.time() - t1, 1)
    with open(os.path.join(mdir, 'module.json'), 'w') as f:
        json.dump(entry, f, indent=1)
    return entry


def response_dump(S, mod, out_dir, src_type):
    """Upload source -> module input 0 -> 2-Out and dump the changed DSPs' memories
    into <Module>/resp/ for g2catalog_response.py."""
    name, tid = mod['shortName'], mod['typeId']
    loc = VA if 'va' in mod['flags'].get('contexts', []) else FX
    idx = S.host.lib.g2p_send_chain_patch(S.host.h, 0, loc, src_type, tid, 0)
    if idx < 0:
        return {'error': 'chain patch not built'}
    S.settle(60)
    recs = S.drain()
    rdir = os.path.join(out_dir, name.replace('/', '_'), 'resp')
    os.makedirs(rdir, exist_ok=True)
    ports = [p for p, r in recs.items() if any(sp == 'P' for (sp, a) in writes_of(r))]
    for port in ports:
        live = S.live(port, (('P', 0, 0, 0x1000), ('X', 1, 0, 0x2000), ('Y', 2, 0, 0x2000),
                             ('Yext', 2, 0x800000, 0x800800)))
        for sp, ws in live.items():
            with open(os.path.join(rdir, f'dsp{port}_live_{sp}.bin'), 'wb') as f:
                f.write(H.words_bin(ws))
    info = {'source': src_type, 'input': 0, 'moduleIndex': idx, 'dsps': ports}
    json.dump(info, open(os.path.join(rdir, 'resp.json'), 'w'))
    return info


def baseline(S, frags, loc):
    """A patch with only a 2-Out: what every module patch is compared against."""
    idx, recs = S.upload(loc, 0)
    b = {'writes': {}, 'program': {}, 'fragments': {}}
    for port, r in recs.items():
        w = writes_of(r)
        b['writes'][port] = w
        P = program_range(w)
        if P and max(P) - min(P) <= 0x2000:
            lo, hi = min(P), max(P)
            prog = [P.get(a, 0) for a in range(lo, hi + 1)]
            b['program'][port] = prog
            b['fragments'][port] = frags.match(prog, lo)[0]
    return b


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fw', default=os.path.join(ROOT, 'original', 'firmware'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'original', 'firmware', 'catalog'))
    ap.add_argument('--fragments', default=os.path.join(ROOT, 'original', 'firmware', 'fragments'))
    ap.add_argument('--modules', default='OscA,OscB,FltLP,FltNord,EnvADSR,LfoA,Mix4-1A,DlySingleA,ShpExp',
                    help='comma-separated short names, or "all"')
    ap.add_argument('--connected', action='store_true',
                    help='instead of cataloguing: re-sweep the parameters that wrote nothing, with OscA on every '
                         'input (the OS links unconnected inputs as constants and skips their parameters)')
    ap.add_argument('--response', action='store_true',
                    help='instead of cataloguing: for modules with inputs and outputs, upload OscA -> module -> '
                         '2-Out and dump the memories for g2catalog_response.py')
    ap.add_argument('--skip-done', action='store_true', help='skip modules already in catalog.json without error')
    ap.add_argument('--sweep', type=int, default=9, help='values per parameter (at least 0, 1/4, 1/2, 3/4, max)')
    ap.add_argument('--quiet-slices', type=int, default=100, help='slices to learn the words written without changes')
    ap.add_argument('--usb-start', type=int, default=11000)
    ap.add_argument('--ms-per-slice', type=float, default=0.125)
    ap.add_argument('--bridge', default=os.path.join(ROOT, 'build', 'emu', 'libg2dspbridge.dylib'))
    ap.add_argument('--protobridge', default=os.path.join(ROOT, 'build', 'emu', 'libg2protobridge.dylib'))
    args = ap.parse_args(argv)
    args.ms_per_slice = args.ms_per_slice

    mods = [m for m in json.load(open(os.path.join(ROOT, 'data', 'modules.json')))['modules']
            if m.get('kind') == 'module' and m['flags'].get('selectable')
            and ('va' in m['flags'].get('contexts', []) or 'fx' in m['flags'].get('contexts', []))]
    if args.modules != 'all':
        want = args.modules.split(',')
        mods = [m for m in mods if m['shortName'] in want]
        missing = set(want) - {m['shortName'] for m in mods}
        if missing:
            print('unknown or unplaceable modules:', ', '.join(sorted(missing)))
    os.makedirs(args.out, exist_ok=True)
    frags = Fragments(args.fragments)
    S = Session(args)
    if not S.boot(args.usb_start):
        print('no sync; giving up')
        return 1
    base = {VA: baseline(S, frags, VA), FX: baseline(S, frags, FX)}
    with open(os.path.join(args.out, 'baseline.json'), 'w') as f:
        json.dump({('VA' if k else 'FX'): {'program': v['program'], 'fragments': v['fragments'],
                                           'writes': {p: summarize(w) for p, w in v['writes'].items()}}
                   for k, v in base.items()}, f, indent=1, default=str)
    index_path = os.path.join(args.out, 'catalog.json')
    index = json.load(open(index_path)) if os.path.exists(index_path) else {}
    if args.connected:
        src = next(m['typeId'] for m in json.load(open(os.path.join(ROOT, 'data', 'modules.json')))['modules']
                   if m['shortName'] == 'OscA')
        for m in mods:
            f = os.path.join(args.out, m['shortName'].replace('/', '_'), 'module.json')
            if not m.get('inputs') or not os.path.exists(f):
                continue
            e = json.load(open(f))
            silent = [p['index'] for p in e.get('params', []) if not any(v['writes'] for v in p['values'])]
            if not silent:
                continue
            t = time.time()
            loc = VA if e.get('area') == 'VA' else FX
            idx = S.host.lib.g2p_send_chain_patch(S.host.h, 0, loc, src, m['typeId'], -1)
            if idx < 0:
                continue
            S.settle(60)
            S.drain()
            noise = S.learn_noise(args.quiet_slices)
            res = []
            for pi in silent:
                p = m['params'][pi]
                lo, hi = p.get('min', 0), p.get('max', 127)
                r = {'index': pi, 'name': p.get('name'), 'min': lo, 'max': hi, 'default': p.get('default'), 'values': []}
                for v in sweep_values(lo, hi, args.sweep):
                    S.host.lib.g2p_set_param(S.host.h, 0, loc, idx, pi, v, 0)
                    r['values'].append({'value': v, **S.settle_writes(noise)})
                S.host.lib.g2p_set_param(S.host.h, 0, loc, idx, pi, p.get('default', lo), 0)
                S.settle_writes(noise)
                res.append(r)
            e['paramsConnected'] = {'source': 'OscA on every input', 'moduleIndex': idx, 'params': res}
            json.dump(e, open(f, 'w'), indent=1)
            print(f"{m['shortName']:<12} connected sweep of {len(silent)} params: "
                  f"{sum(1 for r in res if any(v['writes'] for v in r['values']))} now write, {time.time() - t:.1f} s",
                  flush=True)
        if not args.response:
            return 0
    if args.response:
        for loc, tag in ((VA, 'VA'), (FX, 'FX')):  # the 2-Out alone, for instruction counts
            S.host.lib.g2p_send_module_patch(S.host.h, 0, loc, 0, 1)
            S.settle(60)
            S.drain()
            bdir = os.path.join(args.out, f'_baseline_{tag}')
            os.makedirs(bdir, exist_ok=True)
            for port in range(4):
                live = S.live(port, (('P', 0, 0, 0x1000), ('X', 1, 0, 0x2000), ('Y', 2, 0, 0x2000),
                                     ('Yext', 2, 0x800000, 0x800800)))
                for sp, ws in live.items():
                    with open(os.path.join(bdir, f'dsp{port}_live_{sp}.bin'), 'wb') as f:
                        f.write(H.words_bin(ws))
        src = next(m['typeId'] for m in json.load(open(os.path.join(ROOT, 'data', 'modules.json')))['modules']
                   if m['shortName'] == 'OscA')
        for m in mods:
            if not m.get('inputs') or not m.get('outputs'):
                continue
            t = time.time()
            try:
                info = response_dump(S, m, args.out, src)
            except Exception as ex:
                info = {'error': repr(ex)}
            print(f"{m['shortName']:<12} response dump {info} {time.time() - t:.1f} s", flush=True)
        return 0
    for m in mods:
        if args.skip_done and m['shortName'] in index and not index[m['shortName']].get('error'):
            continue
        t = time.time()
        try:
            e = catalog_module(S, m, frags, base, args.out, args)
        except Exception as ex:  # keep going: one module must not stop the run
            e = {'name': m['shortName'], 'type': m['typeId'], 'error': repr(ex)}
        dsps = e.get('dsps', {})
        prog = {p: d.get('program', {}).get('extraWords') for p, d in dsps.items() if 'program' in d}
        index[m['shortName']] = {'type': m['typeId'], 'area': e.get('area'), 'error': e.get('error'),
                                 'dspsChanged': sorted(prog), 'extraWords': prog,
                                 'fragments': sum(len(d.get('fragments', [])) for d in dsps.values()),
                                 'seconds': round(time.time() - t, 1)}
        print(f"{m['shortName']:<12} {index[m['shortName']]}", flush=True)
        with open(index_path, 'w') as f:
            json.dump(index, f, indent=1)
        status = S.host.lib.g2p_status(S.host.h)
        if status != 13 or S.emu.exceptions:  # 13 = Connected
            print(f'stopping: client status {status}, CPU exceptions {S.emu.exceptions} after {m["shortName"]}')
            return 2
    return 0


if __name__ == '__main__':
    rc = main(sys.argv[1:])
    sys.stdout.flush()
    os._exit(rc)  # the DSP threads are not joined: a DO FOREVER loop never ends
