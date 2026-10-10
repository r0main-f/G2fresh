#!/usr/bin/env python3
"""Compare two recordings of the emulated G2's DACs, e.g. the Python reference (g2blackbox.py) against the C++
emulator (g2emurun --wav): level, pitch, onset and spectrum per output.

  g2emucompare.py REF.wav OTHER.wav [--skip S]

Both are 4-channel float WAVs at 96 kHz (outputs 1-4). The machines start their oscillators at different phases,
so the comparison is phase-blind: per output the peak and RMS, the strongest frequency (g2hostemu.analyse), the
first sample above 10 % of the peak, and the difference of the two magnitude spectra (Hann window, 1/3-octave
bands from 50 Hz to 20 kHz, in dB relative to each recording's total) as its RMS over the bands that carry signal.
Needs numpy (build/venv).
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g2hostemu as H  # noqa: E402

RATE = 96000


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos, fmt, chans = 12, None, 0
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt, chans = struct.unpack('<HH', body[:4])
        elif cid == b'data':
            x = np.frombuffer(body, dtype=np.float32 if fmt == 3 else np.int16).astype(np.float64)
            if fmt != 3:
                x /= 32768.0
            return x.reshape(-1, chans)
        pos += 8 + size + (size & 1)
    raise ValueError(f'{path}: no data')


def bands(x):
    w = np.hanning(len(x))
    s = np.abs(np.fft.rfft((x - x.mean()) * w)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / RATE)
    edges = 50 * 2 ** (np.arange(0, 30) / 3)
    e = np.array([s[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges, edges[1:])])
    return e / max(e.sum(), 1e-30)


def onset(x):
    peak = np.max(np.abs(x))
    if peak < 1e-7:
        return None
    return int(np.argmax(np.abs(x) > 0.1 * peak)) / RATE


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ref')
    ap.add_argument('other')
    ap.add_argument('--skip', type=float, default=0.0, help='seconds left out at the start of both')
    args = ap.parse_args(argv)
    a, b = read_wav(args.ref), read_wav(args.other)
    k = int(args.skip * RATE)
    n = min(len(a), len(b))
    print(f'{os.path.basename(os.path.dirname(args.ref)) or args.ref}: {len(a) / RATE:.3f} s vs {len(b) / RATE:.3f} s')
    for c in range(min(a.shape[1], b.shape[1])):
        x, y = a[k:n, c], b[k:n, c]
        ra, rb = H.analyse(x), H.analyse(y)
        if ra['peak'] < 1e-7 and rb['peak'] < 1e-7:
            continue
        line = (f'  out {c + 1}: peak {ra["peak"]:.6f} / {rb["peak"]:.6f} ({20 * np.log10((rb["peak"] + 1e-12) / (ra["peak"] + 1e-12)):+.2f} dB), '
                f'rms {ra.get("rms", 0):.6f} / {rb.get("rms", 0):.6f}, '
                f'freq {ra.get("freq_hz", 0):.2f} / {rb.get("freq_hz", 0):.2f} Hz, onset {onset(x)} / {onset(y)} s')
        ea, eb = bands(x), bands(y)
        used = (ea > 1e-4) | (eb > 1e-4)
        if used.any():
            d = 10 * np.log10((eb[used] + 1e-12) / (ea[used] + 1e-12))
            line += f', spectrum difference {np.sqrt(np.mean(d ** 2)):.2f} dB rms over {used.sum()} bands'
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
