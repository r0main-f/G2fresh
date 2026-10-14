#!/usr/bin/env python3
"""What a sample rate conversion between the G2's 96 kHz and a host's rate adds: aliases and images.

  aliasing.py tones [--tool build/tools/audiopath/g2audiopath] [--rates 48000,44100]
      Sine tones through both ways of the conversion: the level each comes out with, and the level of everything
      that was not in the input.
  aliasing.py material RENDER.wav... [--tool ...] [--rates 48000,44100] [--b06]
      96 kHz recordings of the emulated G2 (tools/benchpack/g2benchrender; their first channel): how much of what
      a host gets is aliasing. The conversion is linear, so the part of the 96 kHz signal above the host's
      Nyquist frequency, converted alone, is the aliasing; the part below it, converted alone and compared with
      an ideal conversion, is what the conversion does to the band it should pass. --b06: the file is the bench
      pack's B06 Bright Saw, measured note by note.

Two conversions are measured: "lagrange", JUCE's 5-point Lagrange interpolator without a filter, which G2fresh used
until 0.10.2 (a numpy copy of juce::Interpolators::Lagrange), and "converter", plugin/Source/Resampler.h through
`g2audiopath resample`. Needs numpy. Results: re/notes/audio-path.md.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

G2 = 96000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def read_wav(path):
    b = open(path, "rb").read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", path
    at, fmt = 12, None
    while at + 8 <= len(b):
        cid, size = b[at:at + 4], struct.unpack("<I", b[at + 4:at + 8])[0]
        body = b[at + 8:at + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            tag, ch, rate, _, _, bits = fmt
            assert tag == 3 and bits == 32, "32-bit float WAV files only"
            return rate, np.frombuffer(body[:len(body) // (4 * ch) * 4 * ch], dtype="<f4").astype(np.float64).reshape(-1, ch)
        at += 8 + size + (size & 1)
    raise ValueError("no audio in " + path)


def write_wav(path, rate, a):
    data = np.asarray(a, dtype="<f4").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, rate, rate * 4, 4, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def lagrange(x, src, dst):
    """juce::Interpolators::Lagrange::process(src / dst, ...): returns (y, delay in input samples)."""
    speed = src / dst
    n_out = int((len(x) - 8) / speed)
    # subSamplePos starts at 1; before each output, while pos >= 1 a sample is pushed and pos -= 1; the output is
    # the polynomial through the last 5 pushed samples (nodes -2..2, oldest first) at pos
    t = 1.0 + np.arange(n_out) * speed
    pushed = np.floor(t).astype(np.int64)
    frac = t - pushed
    xp = np.concatenate([np.zeros(5), np.asarray(x, dtype=np.float64), np.zeros(8)])
    y = np.zeros(n_out)
    for node in range(5):
        c = np.ones(n_out)
        for j in range(5):
            if j != node:
                c *= ((j - 2) - frac) / (j - node)
        y += c * xp[pushed + node]
    return y, 2.0


class Converter:
    def __init__(self, tool):
        self.tool = tool

    def __call__(self, x, src, dst):
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, "in.wav"), os.path.join(d, "out.wav")
            write_wav(a, src, x)
            out = subprocess.run([self.tool, "resample", a, b, str(dst)], check=True, stdout=subprocess.PIPE, text=True).stdout
            delay = float(re.search(r"delay (\d+) input samples", out).group(1))
            return read_wav(b)[1][:, 0], delay


def db(x):
    return 20 * np.log10(max(float(x), 1e-12))


def fit(y, rate, hz):
    """The tone at hz in y by least squares: (amplitude, what is left)."""
    n = np.arange(len(y))
    m = np.stack([np.sin(2 * np.pi * hz * n / rate), np.cos(2 * np.pi * hz * n / rate)], axis=1)
    c, *_ = np.linalg.lstsq(m, y, rcond=None)
    return float(np.hypot(*c)), y - m @ c


def strongest(y, rate, count=1, lo=20.0, hi=None):
    w = np.kaiser(len(y), 20)
    s = np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2)
    f = np.fft.rfftfreq(len(y), 1 / rate)
    s[(f < lo) | (f > (hi or rate))] = 0
    out = []
    for _ in range(count):
        i = int(np.argmax(s))
        if s[i] <= 0:
            break
        out.append((float(f[i]), float(s[i])))
        s[max(0, i - 40): i + 41] = 0
    return out


def tones(convs, rates):
    for host in rates:
        for src, dst, freqs in ((G2, host, [1000, 10000, 15000, 18000, 20000, 22000, 25000, 28000, 30000, 34000, 38000, 43000, 47000]),
                                (host, G2, [1000, 5000, 10000, 15000, 18000, 20000])):
            print("\n%d Hz -> %d Hz: a tone of 0 dB at f comes out at f with ... dB; everything else that comes out" % (src, dst))
            print("| f (Hz) | " + " | ".join(name for name, _ in convs) + " |")
            print("|---|" + "---|" * len(convs))
            for f in freqs:
                if f >= src / 2:
                    continue
                x = np.sin(2 * np.pi * f * np.arange(int(src * 0.5)) / src)
                cells = []
                for _, conv in convs:
                    y, _ = conv(x, src, dst)
                    y = y[3000:-3000]
                    if f < dst / 2:
                        level, rest = fit(y, dst, f)
                        wanted = "%.2f dB" % db(level)
                    else:
                        rest, wanted = y, "(above the output's Nyquist)"
                    # everything that is not the tone, as the level of one sine of that power, and where most of it is
                    others = db(np.sqrt(2 * np.mean(rest ** 2)))
                    top = strongest(rest, dst)
                    cells.append("%s; %s" % (wanted, "%.1f dB, mostly at %.0f Hz" % (others, top[0][0]) if others > -150 else "below -150 dB"))
                print("| %d | " % f + " | ".join(cells) + " |")


def split(x, cut):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / G2)
    lo = X.copy()
    lo[f >= cut] = 0
    return np.fft.irfft(lo, len(x)), np.fft.irfft(X - lo, len(x))


def ideal(x, host, delay):
    """x delayed by `delay` input samples, band-limited to the host's Nyquist frequency, at the host's rate."""
    n = len(x)
    m = n * host // G2
    X = np.fft.rfft(x) * np.exp(-2j * np.pi * np.fft.rfftfreq(n, 1 / G2) * delay / G2)
    Y = X[: m // 2 + 1].copy()
    Y[-1] = 0
    return np.fft.irfft(Y, m) * (m / n)


def band_rms(y, rate, lo=20.0, hi=20000.0):
    Y = np.fft.rfft(y)
    f = np.fft.rfftfreq(len(y), 1 / rate)
    k = (f >= lo) & (f <= hi)
    return float(np.sqrt(2 * np.sum(np.abs(Y[k]) ** 2)) / len(y)) + 1e-30


def measure(name, x, host, convs):
    x = x[: len(x) // 320 * 320].copy()  # a whole number of samples at 44.1 kHz too
    # faded in and out over a tenth of a second: the split below and the ideal conversion work on the whole stretch
    # as one period, and a jump from its end to its start would count as sound
    fade = np.sin(np.linspace(0.0, np.pi / 2, G2 // 10)) ** 2
    x[: len(fade)] *= fade
    x[-len(fade):] *= fade[::-1]
    lo, hi = split(x, host / 2)
    whole = np.sqrt(np.mean(x ** 2)) + 1e-30
    first = strongest(x, G2, 1, 20.0, G2 / 2)[0]
    cells = ["%s | %.1f dB" % (name, db(np.sqrt(np.mean(hi ** 2)) / whole))]
    for _, conv in convs:
        alias, delay = conv(hi, G2, host)
        passed, _ = conv(lo, G2, host)
        ref = ideal(x, host, delay)
        n = min(len(alias), len(passed), len(ref))
        e = slice(0, n)
        signal = band_rms(ref[e], host)
        if signal < 1e-3 * whole:  # nothing audible in it: against the whole
            signal = whole
        a = band_rms(alias[:n][e], host)
        d = band_rms((passed[:n] - ref[:n])[e], host)
        tops = strongest(alias[:n][e], host, 3, 20.0, 20000.0)
        cells.append("%.1f dB (%s) | %.1f dB" % (
            db(a / signal), ", ".join("%.0f Hz %.1f" % (f, db(v / first[1])) for f, v in tops if db(v / first[1]) > -120) or "-",
            db(d / signal)))
    print("| " + " | ".join(cells) + " |")


def b06_start(x):
    """Where B06's first note starts: after its second of silence."""
    win = 960
    e = np.abs(x[: len(x) // win * win]).reshape(-1, win).max(axis=1)
    quiet = e < 1e-4
    run = 50 if quiet[0] else 0
    for i, q in enumerate(quiet):
        if q:
            run += 1
        elif run >= 30:
            return i * win
        else:
            run = 0
    return None


def material(files, convs, rates, b06):
    for host in rates:
        print("\nAt %d Hz. Aliasing: its level between 20 Hz and 20 kHz against the signal's there (against the whole 96 kHz signal where"
              " there is none), and its strongest partials against the signal's strongest (dB). Pass band: the error between 20 Hz and"
              " 20 kHz of what should pass." % host)
        print("| Sound | Above the host's Nyquist (of the 96 kHz signal) | " + " | ".join("%s: aliasing | %s: pass band" % (n, n) for n, _ in convs) + " |")
        print("|---|---|" + "---|---|" * len(convs))
        for path in files:
            rate, a = read_wav(path)
            assert rate == G2, path + " is not at 96 kHz"
            x = a[:, 0]
            name = os.path.splitext(os.path.basename(path))[0]
            start = b06_start(x) if b06 else None
            if start is not None:
                for k, note in enumerate(["C5", "C6", "C7", "G7", "C8", "G8", "C9", "E9"]):
                    seg = x[start + k * G2 + 9600: start + k * G2 + 86400]
                    if len(seg) == 76800:
                        measure("%s %s" % (name, note), seg, host, convs)
            else:
                measure(name, x[G2 // 4:], host, convs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["tones", "material"])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--tool", default=os.path.join(ROOT, "build", "tools", "audiopath", "g2audiopath"))
    ap.add_argument("--rates", default="48000,44100")
    ap.add_argument("--b06", action="store_true")
    ap.add_argument("--only", choices=["lagrange", "converter"])
    a = ap.parse_args()
    convs = [("Lagrange", lagrange), ("converter", Converter(a.tool))]
    if a.only:
        convs = [c for c in convs if c[0].lower() == a.only]
    rates = [int(r) for r in a.rates.split(",")]
    if a.mode == "tones":
        tones(convs, rates)
    else:
        material(a.files, convs, rates, a.b06)
    return 0


if __name__ == "__main__":
    sys.exit(main())
