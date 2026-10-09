#!/usr/bin/env python3
"""Compare a native render (g2audio) with the emulated G2's DAC recording
(tools/firmware/g2blackbox.py): latency and gain alignment, residual error,
harmonic levels and envelope timing.

  g2compare.py NATIVE.wav EMU.wav [--chan N] [--f0 HZ] [--skip S] [--env HZ] [--json]

* Alignment: the lag (in samples) maximising the cross-correlation, then the
  least-squares gain native -> emulator. The gain is the machine's output
  scaling (see re/notes/native-engine.md).
* Residual: rms(emu - gain * native) / rms(emu), in dB, over the aligned part
  (after --skip seconds).
* Harmonics (--f0, or the strongest peak): levels of H1..H10 in dB re the
  fundamental for both, from a Blackman-Harris FFT.
* --env HZ: the signal is a sine of HZ under an envelope; compares the two
  envelopes (demodulated) and their 10/90 % crossing times.
"""
import argparse
import json
import sys

import numpy as np

RATE = 96000


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], 'little')
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = (int.from_bytes(body[0:2], 'little'), int.from_bytes(body[2:4], 'little'))
        elif cid == b'data':
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch = fmt
    assert tag == 3, 'float WAV expected'
    return np.frombuffer(raw, dtype='<f4').reshape(-1, ch).astype(np.float64)


def align(a, b, max_lag=20000):
    """Lag L such that b[n] ~ a[n - L] (b later than a for L > 0), by FFT cross-correlation."""
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    m = 1 << int(np.ceil(np.log2(2 * n)))
    c = np.fft.irfft(np.fft.rfft(b, m) * np.conj(np.fft.rfft(a, m)), m)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    vals = np.concatenate([c[:max_lag + 1], c[-max_lag:]])
    return int(lags[np.argmax(np.abs(vals))])


def frac_shift(a, d):
    """a delayed by d samples (fractional), through the FFT (circular)."""
    n = len(a)
    m = 1 << int(np.ceil(np.log2(n + 64)))
    f = np.fft.rfftfreq(m)
    return np.fft.irfft(np.fft.rfft(a, m) * np.exp(-2j * np.pi * f * d), m)[:n]


def fit_window(x, b, w):
    """Least-squares gain of x to b over the window w (a slice), and the residual energy."""
    xx, bb = x[w], b[w]
    g = np.dot(xx, bb) / (np.dot(xx, xx) + 1e-30)
    return g, np.mean((bb - g * xx) ** 2)


def fine_lag(a, b, lag, w):
    """Refines an integer lag to a fraction of a sample: the delay of a that fits b best on window w."""
    from scipy.optimize import minimize_scalar
    r = minimize_scalar(lambda d: fit_window(frac_shift(a, d), b, w)[1], bounds=(lag - 1.0, lag + 1.0),
                        method='bounded', options={'xatol': 1e-4})
    return float(r.x)


def shifted(a, lag, n):
    out = np.zeros(n)
    if lag >= 0:
        k = min(n - lag, len(a))
        if k > 0:
            out[lag:lag + k] = a[:k]
    else:
        k = min(n, len(a) + lag)
        if k > 0:
            out[:k] = a[-lag:-lag + k]
    return out


def harmonics(x, f0=None, count=10):
    w = np.blackman(len(x))  # Blackman: -58 dB sidelobes, enough for these comparisons
    s = np.abs(np.fft.rfft((x - np.mean(x)) * w))
    if f0 is None:
        k = int(np.argmax(s[2:])) + 2
        f0 = k * RATE / len(x)

    def level(f):
        j = int(round(f * len(x) / RATE))
        lo, hi = max(1, j - 4), min(len(s), j + 5)
        return float(np.max(s[lo:hi])) if lo < hi else 0.0
    ref = level(f0) + 1e-30
    return f0, [20 * np.log10(level(h * f0) / ref + 1e-12) for h in range(1, count + 1) if h * f0 < RATE / 2]


def envelope(x, f, smooth=None):
    """Amplitude envelope of a sine of frequency f under modulation: I/Q
    demodulation, averaged over whole periods."""
    t = np.arange(len(x)) / RATE
    i, q = x * np.cos(2 * np.pi * f * t), x * np.sin(2 * np.pi * f * t)
    n = smooth or max(1, int(round(RATE / f)))
    k = np.ones(n) / n
    return 2 * np.sqrt(np.convolve(i, k, 'same') ** 2 + np.convolve(q, k, 'same') ** 2)


def crossings(env, levels):
    out = {}
    for lv in levels:
        idx = np.nonzero(env >= lv)[0]
        out[lv] = float(idx[0] / RATE) if len(idx) else None
    return out


def compare(native, emu, chan=0, f0=None, skip=0.0, env_hz=None, frac=True, dur=None):
    """dur: length of the compared window (seconds from skip); a short window keeps a
    few-ppm pitch difference from showing as a phase drift."""
    a, b = native[:, chan], emu[:, chan]
    n = min(len(a), len(b))
    res = {}
    if np.max(np.abs(b)) < 1e-9:
        return {'emu_silent': True, 'native_peak': float(np.max(np.abs(a)))}
    a, b = a[:n], b[:n]
    lag = align(a, b)
    # Leave out the shifted edges (the fractional shift rings near the ends).
    s0 = max(int(skip * RATE), abs(lag) + 2000)
    s1 = n - 2000 if dur is None else min(n - 2000, s0 + int(dur * RATE))
    w = slice(s0, s1)
    if frac:
        d = fine_lag(a, b, lag, w)
        a2 = frac_shift(a, d)
        res['lag_frac'] = d
    else:
        a2 = shifted(a, lag, n)
    aa, bb = a2[s0:s1], b[s0:s1]
    gain = float(np.dot(aa, bb) / (np.dot(aa, aa) + 1e-30))
    err = bb - gain * aa
    res['lag'] = lag
    res['gain'] = gain
    res['gain_db'] = 20 * np.log10(abs(gain) + 1e-30)
    res['residual_db'] = 10 * np.log10(np.mean(err ** 2) / (np.mean(bb ** 2) + 1e-30) + 1e-30)
    res['emu_rms'] = float(np.sqrt(np.mean(bb ** 2)))
    # Phase-blind comparison: difference of the magnitude spectra (Blackman-Harris
    # window, so the leakage floor is below -90 dB), relative to the emulator's.
    win = 0.35875 - 0.48829 * np.cos(2 * np.pi * np.arange(len(bb)) / len(bb)) + \
        0.14128 * np.cos(4 * np.pi * np.arange(len(bb)) / len(bb)) - 0.01168 * np.cos(6 * np.pi * np.arange(len(bb)) / len(bb))
    ma, mb = np.abs(np.fft.rfft(gain * aa * win)), np.abs(np.fft.rfft(bb * win))
    res['spectral_residual_db'] = 10 * np.log10(np.sum((ma - mb) ** 2) / (np.sum(mb ** 2) + 1e-30) + 1e-30)
    fa, ha = harmonics(aa, f0)
    fb, hb = harmonics(bb, f0 or fa)
    res['f0_native'], res['f0_emu'] = fa, fb
    res['harm_native_db'] = [round(v, 2) for v in ha]
    res['harm_emu_db'] = [round(v, 2) for v in hb]
    if env_hz:
        ea, eb = envelope(a2, env_hz) * gain, envelope(b, env_hz)
        peak = max(np.max(eb), 1e-30)
        res['env_peak_emu'] = float(peak)
        res['env_peak_native'] = float(np.max(ea))
        res['env_rms_err_db'] = 10 * np.log10(np.mean((ea - eb) ** 2) / np.mean(eb ** 2))
        res['env_max_err_rel'] = float(np.max(np.abs(ea - eb)) / peak)
        res['env_cross_emu'] = crossings(eb / peak, [0.1, 0.5, 0.9])
        res['env_cross_native'] = crossings(ea / peak, [0.1, 0.5, 0.9])
    return res


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('native')
    ap.add_argument('emu')
    ap.add_argument('--chan', type=int, default=0)
    ap.add_argument('--f0', type=float)
    ap.add_argument('--skip', type=float, default=0.0)
    ap.add_argument('--env', type=float)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-frac', action='store_true', help='integer-sample alignment only')
    ap.add_argument('--dur', type=float, help='compared window length in seconds (from --skip)')
    args = ap.parse_args(argv)
    r = compare(read_wav(args.native), read_wav(args.emu), args.chan, args.f0, args.skip, args.env, not args.no_frac,
                args.dur)
    if args.json:
        print(json.dumps(r))
    else:
        for k, v in r.items():
            print(f'{k}: {v}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
