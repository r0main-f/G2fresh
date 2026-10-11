"""Audio measurements shared by the bench pack's scripts (expected.py, analyse.py, selftest.py).

Uses numpy when it is installed; without it everything still works in plain Python, slowly, and with shorter
spectra (the third-octave figures of the lowest bands are then coarser). BENCH_NO_NUMPY=1 forces that path. A signal is one channel: a numpy array, or a list of floats. Levels are in dB relative to
full scale (dBFS: a sample value of 1.0), amplitudes are peak values unless a name says rms.
"""
import cmath
import math
import os
import struct

try:
    if os.environ.get("BENCH_NO_NUMPY"):
        raise ImportError
    import numpy as np
except ImportError:  # plain Python
    np = None

SILENCE_DB = -200.0


def db(x):
    """20 log10, with a floor instead of minus infinity."""
    return 20.0 * math.log10(x) if x > 1e-10 else SILENCE_DB


# ---- WAV files ---------------------------------------------------------------------------------------------

def read_wav(path):
    """Returns (sample rate, [channel, ...]). PCM 8/16/24/32 bit and 32/64-bit float, plain or WAVE_FORMAT_EXTENSIBLE."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise ValueError("%s is not a WAV file" % path)
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if tag == b"fmt ":
            fmt = data[pos + 8:pos + 8 + size]
        elif tag == b"data":
            body = data[pos + 8:pos + 8 + size] if size not in (0, 0xFFFFFFFF) else data[pos + 8:]
            break
        pos += 8 + size + (size & 1)
    if fmt is None or body is None:
        raise ValueError("%s has no audio" % path)
    code, channels, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if code == 0xFFFE and len(fmt) >= 26:  # extensible: the real format is the sub-format's first two bytes
        code = struct.unpack_from("<H", fmt, 24)[0]
    width = align // channels
    if code not in (1, 3) or width not in (1, 2, 3, 4, 8) or (code == 3 and width not in (4, 8)):
        raise ValueError("%s: unsupported WAV format %d, %d bits" % (path, code, bits))
    frames = len(body) // align
    body = body[:frames * align]
    if np is not None:
        if code == 3:
            x = np.frombuffer(body, dtype="<f4" if width == 4 else "<f8").astype(np.float64)
        elif width == 1:
            x = (np.frombuffer(body, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
        elif width == 3:
            b = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            x = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8).astype(np.float64) / 8388608.0
        else:
            x = np.frombuffer(body, dtype="<i2" if width == 2 else "<i4").astype(np.float64) / float(1 << (8 * width - 1))
        x = x.reshape(-1, channels)
        return rate, [np.ascontiguousarray(x[:, c]) for c in range(channels)]
    if code == 3:
        x = struct.unpack("<%d%s" % (frames * channels, "f" if width == 4 else "d"), body)
    elif width == 1:
        x = [(v - 128) / 128.0 for v in body]
    elif width == 3:
        x = [int.from_bytes(body[i:i + 3], "little", signed=True) / 8388608.0 for i in range(0, len(body), 3)]
    else:
        scale = float(1 << (8 * width - 1))
        x = [v / scale for v in struct.unpack("<%d%s" % (frames * channels, "h" if width == 2 else "i"), body)]
    return rate, [list(x[c::channels]) for c in range(channels)]


def write_wav(path, rate, channels, bits=24):
    """Writes PCM (bits 16 or 24) or 32-bit float (bits 32). Samples beyond full scale are clipped in PCM."""
    count, frames = len(channels), min(len(c) for c in channels)
    if np is not None:
        x = np.stack([np.asarray(c[:frames], dtype=np.float64) for c in channels], axis=1)
        if bits == 32:
            body = x.astype("<f4").tobytes()
        elif bits == 16:
            body = np.clip(np.round(x * 32768.0), -32768, 32767).astype("<i2").tobytes()
        else:
            v = np.clip(np.round(x * 8388608.0), -8388608, 8388607).astype("<i4")
            body = v.view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    else:
        flat = [channels[c][i] for i in range(frames) for c in range(count)]
        if bits == 32:
            body = struct.pack("<%df" % len(flat), *flat)
        elif bits == 16:
            body = struct.pack("<%dh" % len(flat), *[max(-32768, min(32767, int(round(v * 32768.0)))) for v in flat])
        else:
            body = b"".join(max(-8388608, min(8388607, int(round(v * 8388608.0)))).to_bytes(3, "little", signed=True) for v in flat)
    width = bits // 8
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 3 if bits == 32 else 1, count, rate, rate * width * count, width * count, bits) + b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(header + body)


# ---- levels ------------------------------------------------------------------------------------------------

def part(x, rate, start_s, stop_s):
    """The samples from start_s to stop_s (clamped to the signal)."""
    a = max(0, int(round(start_s * rate)))
    b = min(len(x), int(round(stop_s * rate)))
    return x[a:max(a, b)]


def mean(x):
    if len(x) == 0:
        return 0.0
    return float(x.mean()) if np is not None else math.fsum(x) / len(x)


def rms(x, remove_dc=True):
    if len(x) == 0:
        return 0.0
    m = mean(x) if remove_dc else 0.0
    if np is not None:
        return float(math.sqrt(max(0.0, float(np.mean(np.square(x))) - m * m)))
    return math.sqrt(max(0.0, math.fsum(v * v for v in x) / len(x) - m * m))


def peak(x):
    if len(x) == 0:
        return 0.0
    return float(np.max(np.abs(x))) if np is not None else max(abs(v) for v in x)


def envelope(x, rate, block_s=0.005, offset_s=0.0):
    """RMS of consecutive blocks of block_s seconds, starting offset_s into the signal."""
    n = max(1, int(round(block_s * rate)))
    a = max(0, int(round(offset_s * rate)))
    count = (len(x) - a) // n
    if count <= 0:
        return []
    if np is not None:
        return np.sqrt(np.mean(np.square(x[a:a + count * n]).reshape(count, n), axis=1)).tolist()
    return [math.sqrt(math.fsum(v * v for v in x[a + i * n:a + (i + 1) * n]) / n) for i in range(count)]


# ---- tones -------------------------------------------------------------------------------------------------

def tone(x, rate, hz, window=True):
    """The component of x at hz: (peak amplitude, phase in radians at the first sample). Hann-windowed by default,
    which keeps other components out as long as they are a few cycles of the window away."""
    n = len(x)
    if n < 8:
        return 0.0, 0.0
    w = 2.0 * math.pi * hz / rate
    if np is not None:
        k = np.arange(n)
        win = 0.5 - 0.5 * np.cos(2.0 * math.pi * (k + 0.5) / n) if window else np.ones(n)
        s = complex(np.dot(x * win, np.exp(-1j * w * k)))
        total = float(win.sum())
    else:
        step, rot = cmath.exp(-1j * w), 1 + 0j
        wstep, wrot = cmath.exp(2j * math.pi / n), cmath.exp(1j * math.pi / n)
        s, total = 0j, 0.0
        for i in range(n):
            g = 0.5 - 0.5 * wrot.real if window else 1.0
            s += x[i] * g * rot
            total += g
            rot *= step
            wrot *= wstep
            if (i & 4095) == 4095:  # keep the rotating phasors on the unit circle
                rot /= abs(rot)
                wrot /= abs(wrot)
    return 2.0 * abs(s) / total, cmath.phase(s)


def tone_frequency(x, rate, guess_hz, segment_s=0.02):
    """The exact frequency of a steady tone near guess_hz (within 1 / (2 * segment_s) of it), from the drift of its
    phase over the signal. 0 if there is no such tone."""
    hz = guess_hz
    for seg_s in (segment_s, segment_s * 4):
        n = max(16, int(round(seg_s * rate)))
        count = len(x) // n
        if count < 3:
            return hz if count else 0.0
        count = min(count, 4000)
        phases, weights = [], []
        for i in range(count):
            a, p = tone(x[i * n:(i + 1) * n], rate, hz)
            # the phase at the segment's own first sample, taken back to the signal's time base
            phases.append(p)
            weights.append(a)
        top = max(weights)
        if top <= 1e-9:
            return 0.0
        # unwrap the phase advance between neighbours, after removing what guess_hz itself advances
        expected = 2.0 * math.pi * hz * n / rate
        total, ks, vals = 0.0, [], []
        for i in range(1, count):
            d = phases[i] - phases[i - 1] - expected
            d = (d + math.pi) % (2.0 * math.pi) - math.pi
            total += d
            if weights[i] > 0.25 * top and weights[i - 1] > 0.25 * top:
                ks.append(float(i))
                vals.append(total)
        if len(ks) < 2:
            return hz
        km, vm = math.fsum(ks) / len(ks), math.fsum(vals) / len(vals)
        den = math.fsum((k - km) ** 2 for k in ks)
        slope = math.fsum((k - km) * (v - vm) for k, v in zip(ks, vals)) / den if den else 0.0
        hz += slope * rate / (2.0 * math.pi * n)
    return hz


def fft_power(seg):
    """Power spectrum of a Hann-windowed segment (its length a power of two in plain Python): bins 0..n/2,
    scaled so that a sine of peak amplitude A reads A*A/2 summed over its bins."""
    n = len(seg)
    if np is not None:
        win = 0.5 - 0.5 * np.cos(2.0 * math.pi * (np.arange(n) + 0.5) / n)
        spec = np.fft.rfft(np.asarray(seg) * win)
        return (np.abs(spec) ** 2) * (2.0 / (n * float(np.sum(win * win))))
    a = [seg[i] * (0.5 - 0.5 * math.cos(2.0 * math.pi * (i + 0.5) / n)) for i in range(n)]
    wsum = math.fsum((0.5 - 0.5 * math.cos(2.0 * math.pi * (i + 0.5) / n)) ** 2 for i in range(n))
    # iterative radix-2 FFT
    j = 0
    a = [complex(v) for v in a]
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        wlen = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            w = 1 + 0j
            for k in range(size // 2):
                u, v = a[start + k], a[start + k + size // 2] * w
                a[start + k], a[start + k + size // 2] = u + v, u - v
                w *= wlen
        size <<= 1
    return [abs(a[i]) ** 2 * (2.0 / (n * wsum)) for i in range(n // 2 + 1)]


def spectrum(x, rate, size=None):
    """Averaged power spectrum (Welch, Hann, half-overlapped): (bin width in Hz, [power per bin])."""
    if size is None:
        size = 32768 if np is not None else 4096
        while size > 256 and size * 2 > len(x):
            size //= 2
    if len(x) < size:
        return rate / float(size), [0.0] * (size // 2 + 1)
    starts = list(range(0, len(x) - size + 1, size // 2))
    total = None
    for s in starts:
        p = fft_power(x[s:s + size])
        total = p if total is None else (total + p if np is not None else [a + b for a, b in zip(total, p)])
    if np is not None:
        return rate / float(size), (total / len(starts)).tolist()
    return rate / float(size), [v / len(starts) for v in total]


THIRD_OCTAVES = [31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150,
                 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000]


def band_levels(x, rate, centers=None):
    """Level in dBFS (rms) of each third-octave band; None for a band the sample rate or the resolution cannot give."""
    centers = centers or THIRD_OCTAVES
    width, power = spectrum(x, rate)
    out = []
    for c in centers:
        lo, hi = c / 2 ** (1 / 6.0), c * 2 ** (1 / 6.0)
        a, b = int(math.ceil(lo / width)), int(math.floor(hi / width))
        if hi > 0.45 * rate or b - a < 2:
            out.append(None)
            continue
        out.append(round(10.0 * math.log10(max(math.fsum(power[a:b + 1]), 1e-20)), 2))
    return out


def peaks(x, rate, floor_db=-80.0, low_hz=20.0, size=None):
    """The spectral peaks of a steady sound: [(hz, dB relative to the strongest)], strongest first, down to floor_db.
    Frequencies are interpolated between bins (good to a few percent of a bin)."""
    width, power = spectrum(x, rate, size)
    first = max(2, int(low_hz / width))
    top = max(power[first:]) if len(power) > first else 0.0
    if top <= 0:
        return []
    limit = top * 10 ** (floor_db / 10.0)
    found = []
    for i in range(first, len(power) - 1):
        p = power[i]
        if p >= limit and p > power[i - 1] and p >= power[i + 1]:
            # parabolic interpolation on the log spectrum (exact enough for a Hann window)
            la, lb, lc = (math.log(max(v, 1e-30)) for v in (power[i - 1], p, power[i + 1]))
            den = la - 2 * lb + lc
            shift = 0.5 * (la - lc) / den if den else 0.0
            found.append(((i + shift) * width, 10.0 * math.log10(math.fsum(power[max(0, i - 2):i + 3]) / top)))
    # a Hann peak spreads over three bins: the sum above counts them, and the strongest reads about +1.8 dB
    best = max(v for _, v in found) if found else 0.0
    found = [(hz, v - best) for hz, v in found if v - best >= floor_db]
    found.sort(key=lambda t: -t[1])
    return found


# ---- timing ------------------------------------------------------------------------------------------------

def correlate(a, b):
    """Pearson correlation of two equally long lists."""
    n = min(len(a), len(b))
    if n < 2:
        return 0.0
    if np is not None:
        u, v = np.asarray(a[:n], dtype=float), np.asarray(b[:n], dtype=float)
        u, v = u - u.mean(), v - v.mean()
        den = math.sqrt(float(np.dot(u, u)) * float(np.dot(v, v)))
        return float(np.dot(u, v)) / den if den > 0 else 0.0
    ma, mb = math.fsum(a[:n]) / n, math.fsum(b[:n]) / n
    suv = math.fsum((a[i] - ma) * (b[i] - mb) for i in range(n))
    den = math.sqrt(math.fsum((a[i] - ma) ** 2 for i in range(n)) * math.fsum((b[i] - mb) ** 2 for i in range(n)))
    return suv / den if den > 0 else 0.0


def loop_offset(env, loop_env):
    """Where a loop starts in a recording's envelope: (block index of the first loop start, correlation there).
    env and loop_env are envelopes on the same block size; loop_env is one loop from its start."""
    n = len(loop_env)
    if n < 4 or len(env) < n:
        return 0, 0.0
    best, best_c = 0, -2.0
    if np is not None:
        e, l = np.asarray(env, dtype=float), np.asarray(loop_env, dtype=float)
        reps = int(len(e) // n) + 2
        tiled = np.tile(l, reps)
        for off in range(n):
            ref = tiled[n - off:n - off + len(e)] if off else tiled[:len(e)]
            c = correlate(e, ref)
            if c > best_c:
                best, best_c = off, c
        return best, best_c
    # plain Python: a coarse pass on every 8th block, then around its best
    def at(off, stride):
        idx = range(0, len(env), stride)
        return correlate([env[i] for i in idx], [loop_env[(i - off) % n] for i in idx])
    for off in range(0, n, 8):
        c = at(off, 8)
        if c > best_c:
            best, best_c = off, c
    centre = best
    for off in range(centre - 8, centre + 9):
        c = at(off % n, 1)
        if c > best_c:
            best, best_c = off % n, c
    return best, best_c


def onsets(x, rate, ratio=0.5, min_gap_s=0.1, top=None):
    """Times (seconds, interpolated between samples) where |x| rises through ratio * top (default: its peak), at
    least min_gap_s apart, and the sign of the signal as it does."""
    top = peak(x) if top is None else top
    if top <= 0:
        return [], []
    thr, gap = ratio * top, int(min_gap_s * rate)
    if np is not None:
        mag = np.abs(x)
        idx = np.nonzero((mag[1:] >= thr) & (mag[:-1] < thr))[0] + 1
        idx = idx.tolist()
    else:
        idx = [i for i in range(1, len(x)) if abs(x[i]) >= thr > abs(x[i - 1])]
    times, signs, last = [], [], -gap - 1
    for i in idx:
        if i - last <= gap:
            continue
        last = i
        a, b = abs(x[i - 1]), abs(x[i])
        times.append((i - 1 + (thr - a) / (b - a)) / rate if b > a else i / float(rate))
        signs.append(1 if x[i] >= 0 else -1)
    return times, signs


def fit_period(times):
    """A steady pulse train t = t0 + k * period, with missed pulses allowed: (period, t0, rms of the residuals, count)."""
    if len(times) < 3:
        return 0.0, times[0] if times else 0.0, 0.0, len(times)
    gaps = sorted(b - a for a, b in zip(times, times[1:]))
    period = gaps[len(gaps) // 2]
    ks = [round((t - times[0]) / period) for t in times]
    for _ in range(2):  # refine: the median gap is a little off when pulses are missing
        n = len(ks)
        km, tm = math.fsum(ks) / n, math.fsum(times) / n
        den = math.fsum((k - km) ** 2 for k in ks)
        period = math.fsum((k - km) * (t - tm) for k, t in zip(ks, times)) / den
        t0 = tm - period * km
        ks = [round((t - t0) / period) for t in times]
    res = math.sqrt(math.fsum((t - (t0 + period * k)) ** 2 for k, t in zip(ks, times)) / len(times))
    return period, t0, res, len(times)
