#!/usr/bin/env python3
"""Summarise the DSP module catalog written by g2catalog.py.

  g2catalog_report.py [--catalog DIR] [--json OUT.json] [--md OUT.md] [MODULE...]

For each module: the DSP(s) it went to, its program size against the editor's
estimate, the library fragments it is made of (and how many of their words the
OS patched), the X/Y words it occupies, and for each parameter a fitted
conversion from the knob value to the DSP word(s) the OS writes:
  linear       w = a*v + b
  exp          w = a * 2^(v/k)  (k = steps per doubling; 12 = semitones)
  code         the OS rewrites program words (an instruction or a jump target)
  table        anything else (values listed)
and whether the OS slews the word (ramp of several writes). The Markdown output
holds formulas, sizes and short excerpts only, no data tables.
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def s24(w):
    return w - (1 << 24) if w & 0x800000 else w


def frac(w):
    return s24(w) / 8388608.0


def fit_linear(xs, ys):
    n = len(xs)
    if n < 2:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    b = my - a * mx
    err = max(abs(a * x + b - y) for x, y in zip(xs, ys))
    span = max(ys) - min(ys) or 1
    return a, b, err / span


def fit_exp(xs, ys):
    if any(y <= 0 for y in ys) or len(set(ys)) < 2:
        return None
    r = fit_linear(xs, [math.log2(y) for y in ys])
    if r is None or r[0] == 0:
        return None
    a, b, rel = r
    pred = [2 ** (a * x + b) for x in xs]
    err = max(abs(p - y) / y for p, y in zip(pred, ys))
    return 2 ** b, 1 / a, err


def mostly_exp(xs, ys):
    """Steps per doubling from the median of the successive ratios, when at least half
    of the points follow it within 2 %: (k, a, [values that deviate]) or None."""
    pts = [(x, y) for x, y in zip(xs, ys) if y > 0]
    if len(pts) < 5:
        return None
    ks = [(x1 - x0) / math.log2(y1 / y0) for (x0, y0), (x1, y1) in zip(pts, pts[1:]) if y1 != y0]
    if not ks:
        return None
    k = sorted(ks)[len(ks) // 2]
    if not math.isfinite(k) or k == 0:
        return None
    # anchor at the point that agrees best with its neighbours
    best = None
    for x0, y0 in pts:
        a = y0 / 2 ** (x0 / k)
        off = [x for x, y in pts if abs(a * 2 ** (x / k) - y) / y > 0.02]
        if best is None or len(off) < len(best[2]):
            best = (k, a, off)
    return best if len(best[2]) * 2 <= len(pts) else None


def word_series(param):
    """{(dsp, space, addr): [(value, final word, writes)]} over the sweep."""
    out = {}
    for v in param['values']:
        for dsp, ws in v['writes'].items():
            for sp, addr, val, count, first in ws:
                out.setdefault((dsp, sp, addr), []).append((v['value'], val, count))
    return out


def describe_param(param):
    series = word_series(param)
    selector = param['max'] - param['min'] < 8
    res = []
    for (dsp, sp, addr), pts in sorted(series.items(), key=lambda kv: (kv[0][0], str(kv[0][1]), kv[0][2])):
        if sp == 'X' and addr == 0x40:
            continue  # written once after every upload: not the parameter
        xs = [p[0] for p in pts]
        raw = [p[1] for p in pts]
        slewed = max(p[2] for p in pts) > 1
        d = {'dsp': int(dsp), 'space': sp, 'addr': addr, 'points': len(pts), 'slewed': slewed,
             'samples': [[x, f'{w:06x}'] for x, w, _ in pts]}
        if sp == 'P':
            d['kind'] = 'code'
        elif len(set(raw)) == 1:
            d['kind'] = 'constant'
        elif selector:
            d['kind'] = 'select'
        else:
            ys = [s24(w) for w in raw]
            lin = fit_linear(xs, ys)
            ex = fit_exp(xs, ys)
            if ex and lin and ex[2] < 0.01 and ex[2] < lin[2]:
                lin = None  # the exponential fits better (pitch-like words are both nearly)
            if lin and lin[2] < 0.01:
                d['kind'] = 'linear'
                d['formula'] = f'{lin[0] / 8388608:.6g}*v {lin[1] / 8388608:+.6g} (fraction)'
                d['fit'] = {'a': lin[0], 'b': lin[1], 'relErr': lin[2]}
            elif ex and ex[2] < 0.01:
                d['kind'] = 'exp'
                d['formula'] = f'{ex[0] / 8388608:.6g} * 2^(v/{ex[1]:.4g}) (fraction)'
                d['fit'] = {'a': ex[0], 'stepsPerDoubling': ex[1], 'relErr': ex[2]}
            else:
                approx = mostly_exp(xs, ys)
                if approx:
                    k, a, off = approx
                    d['kind'] = 'exp~'
                    d['formula'] = (f'about {a / 8388608:.6g} * 2^(v/{k:.4g}) (fraction), off by more than 2% at v='
                                    + ','.join(str(x) for x in off))
                    d['fit'] = {'a': a, 'stepsPerDoubling': k, 'offAt': off}
                else:
                    d['kind'] = 'table'
        res.append(d)
    return res


# words that change after every upload whatever the module: the output DSP's level
# ramp and a word written once on all DSPs
NOISE = {('X', 0x1739), ('X', 0x40)}


def module_summary(e, catalog=None, costs=None, db=None):
    s = {'name': e['name'], 'type': e['type'], 'area': e.get('area'), 'error': e.get('error'),
         'estimate': e.get('estimate'), 'dsps': {}}
    d = (db or {}).get(e['name'], {})
    s['longName'] = d.get('longName', e.get('longName'))
    s['category'] = d.get('category')
    s['io'] = {'in': [f"{c['name']}/{c.get('type', '')}" for c in d.get('inputs', [])],
               'out': [f"{c['name']}/{c.get('type', '')}" for c in d.get('outputs', [])]}
    s['cost'] = (costs or {}).get(e['name'].replace('/', '_'))
    rf = os.path.join(catalog or '', e['name'].replace('/', '_'), 'resp', 'response.json')
    if catalog and os.path.exists(rf):
        r = json.load(open(rf))
        if 'gainDb' in r:
            s['response'] = {k: r[k] for k in ('inputCable', 'outputCables', 'gainDb', 'dcOut', 'harmonicsDb1k')}
    for dsp, info in (e.get('dsps') or {}).items():
        ch = info.get('changedVsBaseline', {})
        if not info.get('program') and not any((sp, lo) not in NOISE for sp in ('X', 'Y') for lo, hi, _ in ch.get(sp, [])):
            continue
        frs = info.get('fragments', [])
        s['dsps'][dsp] = {
            'program': info.get('program'),
            'fragments': [{'fragment': f['fragment'], 'at': f['at'], 'len': f['len'], 'cycles': f.get('cycles'),
                           'patchedWords': len(f['patched'])} for f in frs],
            'fragmentWords': sum(f['len'] for f in frs),
            'data': {sp: [[lo, hi] for lo, hi, _ in ch.get(sp, []) if (sp, lo) not in NOISE] for sp in ('X', 'Y')},
            'regs': ch.get('regs'),
        }
    conn = {p['index']: p for p in e.get('paramsConnected', {}).get('params', [])}
    s['params'] = [{'name': p['name'], 'min': p['min'], 'max': p['max'],
                    'words': describe_param(conn.get(p['index'], p)), 'connected': p['index'] in conn}
                   for p in e.get('params', [])]
    s['modes'] = [{'name': m['name'], 'values': [
        {'value': v['value'], 'dsps': sorted(v['writes']),
         'P': sum(len(r[2]) for d in v.get('summary', {}).values() for r in d.get('P', [])),
         'XY': sum(len(r[2]) for d in v.get('summary', {}).values() for sp in ('X', 'Y') for r in d.get(sp, []))}
        for v in m['values']]} for m in e.get('modes', [])]
    return s


def md(summaries):
    out = []
    for s in summaries:
        out.append(f"### {s['name']}: {s.get('longName') or ''} (type {s['type']}, {s.get('category') or '?'}, {s['area']})")
        io = s.get('io') or {}
        out.append(f"* in: {', '.join(io.get('in', [])) or '-'}; out: {', '.join(io.get('out', [])) or '-'}")
        c = s.get('cost')
        if c:
            out.append(f"* cost [C]: {c['instructionsPerFrame']:g} DSP instructions per sample (average over 4 samples, "
                       f"so a control-rate part counts 1/4); editor estimate {c['estimateCycles']} + "
                       f"{c['estimateCyclesB']}/4 cycles")
        r = s.get('response')
        if r:
            g = r['gainDb']
            out.append(f"* response [C] (default settings, input 0 driven): gain {g.get('100', g.get(100))} / "
                       f"{g.get('1000', g.get(1000))} / {g.get('10000', g.get(10000))} dB at 100 Hz / 1 kHz / 10 kHz; "
                       f"DC 0.25 → {', '.join(str(v) for v in r['dcOut'])}; 1 kHz harmonics 2-6: "
                       f"{', '.join(str(v) for v in r['harmonicsDb1k'])} dB")
        if s.get('error'):
            out.append(f"* failed: {s['error']}")
            continue
        est = s.get('estimate') or {}
        for dsp, d in sorted(s['dsps'].items()):
            pr = d.get('program') or {}
            out.append(f"* DSP {dsp}: program +{pr.get('extraWords', 0)} words "
                       f"(estimate P {est.get('pA', '?')}+{est.get('pB', '?')}, cycles {est.get('cyclesA', '?')}"
                       f"+{est.get('cyclesB', '?')}/4); fragments: "
                       + (', '.join(f"{f['fragment']} ({f['len']} w, {f['patchedWords']} patched)" for f in d['fragments'])
                          or 'none matched')
                       + '; data ' + ', '.join(f"{sp}:${lo:X}-${hi:X}" for sp in ('X', 'Y') for lo, hi in d['data'][sp]))
        for p in s['params']:
            ws = [w for w in p['words'] if w['kind'] != 'constant']
            if not ws:
                out.append(f"* {p['name']} ({p['min']}..{p['max']}): no DSP word changes seen")
                continue
            parts = []
            for w in ws:
                loc = f"{w['space']}:${w['addr']:X}"
                if w['kind'] in ('linear', 'exp', 'exp~'):
                    parts.append(f"{loc} {w['kind']} {w['formula']}" + (' (slewed)' if w['slewed'] else ''))
                elif w['kind'] == 'select':
                    parts.append(f"{loc} one word per choice" + (' (slewed)' if w['slewed'] else ''))
                else:
                    parts.append(f"{loc} {w['kind']}" + (' (slewed)' if w['slewed'] else ''))
            out.append(f"* {p['name']} ({p['min']}..{p['max']}){' [inputs connected]' if p.get('connected') else ''}: "
                       + '; '.join(parts))
        for m in s['modes']:
            out.append(f"* mode {m['name']}: " + ', '.join(
                f"{v['value']}→{v['P']} P/{v['XY']} XY words on {','.join(v['dsps']) or '-'}" for v in m['values']))
        out.append('')
    return '\n'.join(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--catalog', default=os.path.join(ROOT, 'original', 'firmware', 'catalog'))
    ap.add_argument('--json')
    ap.add_argument('--md')
    ap.add_argument('modules', nargs='*')
    a = ap.parse_args(argv)
    sums = []
    cf = os.path.join(a.catalog, 'costs.json')
    costs = json.load(open(cf))['modules'] if os.path.exists(cf) else {}
    db = {m['shortName']: m for m in json.load(open(os.path.join(ROOT, 'data', 'modules.json')))['modules']}
    for d in sorted(os.listdir(a.catalog)):
        f = os.path.join(a.catalog, d, 'module.json')
        if not os.path.exists(f) or (a.modules and d not in a.modules):
            continue
        sums.append(module_summary(json.load(open(f)), a.catalog, costs, db))
    sums.sort(key=lambda s: s['type'])
    if a.json:
        json.dump(sums, open(a.json, 'w'), indent=1)
    text = md(sums)
    if a.md:
        open(a.md, 'w').write(text)
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
