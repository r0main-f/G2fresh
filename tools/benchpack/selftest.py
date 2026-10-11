#!/usr/bin/env python3
"""Checks the bench pack's analysis before real recordings arrive: makes "recordings" with known faults from renders,
runs analyse.py on them and checks that the report finds the faults.

    python3 tools/benchpack/selftest.py                  stand-in renders made here (no emulator, no firmware)
    python3 tools/benchpack/selftest.py --emulator       the emulated G2's renders (bench/hardware-pack/expected/renders,
                                                         written by expected.py) against the committed expected JSON
    options: --work DIR (keep the files there), --plain (only the checks that need no numpy)

The fake G2 and interface: each output at its own gain, a clock 37 ppm fast, 48 kHz 24-bit files, Out 1 and 2 swapped
at the interface, inverted polarity, a little noise and hum, a gentle treble loss, an analogue stage that clips below
the DAC's full scale, a master clock 0.6 % fast, and for B10 an input whose ADC takes the interface's output 3 dB
above its full scale, clips there and is 1.23 ms from the DAC, the loopback's cable on the other channel.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import benchaudio as ba  # noqa: E402
import benchfacts as bf  # noqa: E402
import analyse  # noqa: E402
import make_stimulus  # noqa: E402

np = ba.np
ROOT = os.path.dirname(os.path.dirname(HERE))
PACK = os.path.join(ROOT, "bench", "hardware-pack")

FAULTS = {
    "gain_db": {1: -7.30, 2: -7.10, 3: -6.90, 4: -7.55},  # per G2 output, to the file
    "clock_ppm": 37.0,
    "rate": 48000,
    "swap": [1, 0, 2, 3],            # file channel -> G2 output index: Out 1 and 2 crossed
    "polarity": -1.0,
    "noise_dbfs": -92.0,
    "hum_dbfs": -96.0,
    "treble_hz": 22000.0,            # one-pole low-pass
    "clip": 0.60,                    # the analogue stage clips at this DAC level (0 dBFS = 1.0)
    "master_clock_ppm": 6000.0,      # B08's master clock against the sample clock
    "lead_in_s": 0.37,
    # B10: the interface's 0 dBFS reaches the ADC `adc_db` above its full scale, where it clips; the ADC's word is at
    # the DAC `delay_ms` later; the input stage does not invert; the loopback cable loses `loopback_db`
    "input": {"adc_db": 3.0, "delay_ms": 1.23, "polarity": 1.0, "loopback_db": -0.5},
}
INPUT_GAIN = 0.4987    # a G2's DAC word over its ADC word through B10, in the sign the renders have (expected/B10)
INPUT_FRAMES = 20      # and its way from the ADC to the DAC in the stand-in render, in frames of 96 kHz
FORM = """Q01. Name
Answer: Selftest
Q02. Model
Answer: G2X
Q03. Expansion
Answer: built in
Q04. OS
Answer: 1.62
Q05. Interface
Answer: Fake 4x4
Q06. Input reference
Answer: +4 dBu
Q07. Gain
Answer: minimum
Q08. Full scale
Answer: +18 dBu
Q09. Format
Answer: 48 kHz, 24 bit
Q12. B11
Réponse : 32 (32)
Q13. B12
Answer: 24 (32)
Q14. B13
Answer: 07 (32)
Q15. Clock
Answer: 120 BPM, Run
Q19. Store
Answer: Memory is
  protected!
"""


# ---- stand-in renders: a toy G2, enough like the real one for the analysis to chew on ---------------------------

def toy_render(entry, rate=96000):
    kind, full = entry["measure"], 0.12467
    seconds = max(4.0, 3.4 * entry["loop_seconds"] * 1.002 + 1.5) if entry["loop_seconds"] else {"clicks": 30.0, "noise": 12.0}.get(kind, 6.0)
    n = int(seconds * rate)
    t = np.arange(n) / float(rate)
    out = [np.zeros(n) for _ in range(4)]
    rng = np.random.default_rng(sum(map(ord, entry["id"])))
    stretch = 1.0015  # the G2's own 120 BPM clock runs this much slow
    phase0 = 0.31 * entry["loop_seconds"]  # the render starts somewhere inside a loop

    def steps_signal(make):
        steps = entry["steps"]
        step_s = entry["step_seconds"] * stretch
        pos = (t + phase0) % (step_s * len(steps))
        idx = np.minimum((pos / step_s).astype(int), len(steps) - 1)
        return [make(i, s, idx == i) for i, s in enumerate(steps)]

    if kind == "tone":
        out[0] = out[1] = full * np.sin(2 * math.pi * 999.876 * t)
    elif kind == "tone_map":
        for o, hz in entry["tones_hz"].items():
            out[int(o) - 1] = full * np.sin(2 * math.pi * hz * t)
    elif kind == "noise":
        out[0] = out[1] = np.clip(rng.normal(0.0, 0.055, n), -0.112, 0.112)
    elif kind == "level_steps":
        tone = np.sin(2 * math.pi * 999.876 * t)
        for o, pad in ((0, 1.0), (1, 2.0)):
            x = np.zeros(n)
            for i, s, mask in steps_signal(lambda i, s, mask: (i, s, mask)):
                if s.get("nominal_db") is not None:
                    x[mask] = np.clip(tone[mask] * 10 ** (s["nominal_db"] / 20.0), -4.0, 4.0) * full * pad
            out[o] = x
    elif kind == "pad_steps":
        tone = full * np.sin(2 * math.pi * 999.876 * t)
        x = np.zeros(n)
        for i, s, mask in steps_signal(lambda i, s, mask: (i, s, mask)):
            x[mask] = tone[mask] * {"silence": 0.0, "pad 0 dB": 1.0, "pad +6 dB": 2.0}[s["label"]]
        out[0] = out[1] = x
    elif kind == "saw_steps":
        x = np.zeros(n)
        for i, s, mask in steps_signal(lambda i, s, mask: (i, s, mask)):
            if "midi_note" in s:
                hz = 440.0 * 2 ** ((s["midi_note"] - 69) / 12.0)
                # band-limited up to 40 kHz, plus one folded partial as a stand-in for aliasing
                k = np.arange(1, int(40000 / hz) + 1)
                seg = np.zeros(int(mask.sum()))
                ts = t[mask]
                for h in k:
                    seg += np.sin(2 * math.pi * hz * h * ts) / h
                seg += 0.003 * np.sin(2 * math.pi * abs(96000 - hz * math.ceil(60000 / hz)) * ts)
                x[mask] = 0.07 * seg
        out[0] = out[1] = x
    elif kind == "clicks":
        for o, period in ((0, 48000.27), (1, 48072.2)):
            x = np.zeros(n)
            pos = 75.0 + 84 * o
            while pos < n - 2000:
                a = int(pos) & ~3
                x[a:a + 4] = full
                x[a + 4:a + 1000] = full * np.exp(-np.arange(996) / 110.0)
                pos += period
            out[o] = x
    elif kind == "phrase":
        loop = entry["loop_seconds"] * stretch
        step = loop / 16.0
        # {step: (steps it lasts, decay in seconds)}: twelve short notes, or B09d's two long tails
        events = {i: (1, 0.06) for i in range(12)} if entry["id"] != "B09d" else {0: (6, 0.5), 8: (4, 0.3)}
        notes = [110.0 * 2 ** (int(v) / 12.0) for v in rng.integers(0, 36, 16)]
        pos = (t + phase0) % loop
        idx = np.minimum((pos / step).astype(int), 15)
        x = np.zeros(n)
        for i, (span, tau) in events.items():
            mask = (idx >= i) & (idx < i + span)
            age = pos[mask] - i * step
            x[mask] += 0.08 * np.exp(-age / tau) * np.sin(2 * math.pi * notes[i] * age) * (1.0 + 0.3 * np.sin(2 * math.pi * 3 * notes[i] * age))
        out[0], out[1] = x, x * 0.9 if entry["id"] == "B09d" else x
    return rate, out


# ---- the fake G2 hardware and interface ------------------------------------------------------------------------

def resample(x, rate_in, rate_out, ratio=1.0):
    """Windowed-sinc resampling of x to rate_out, the source running `ratio` times fast (its samples closer together)."""
    step = rate_in * ratio / float(rate_out)  # input samples per output sample
    count = int((len(x) - 1) / step)
    pos = np.arange(count) * step
    base = np.floor(pos).astype(np.int64)
    frac = pos - base
    cutoff = min(1.0, 0.94 / step)  # relative to the input's Nyquist
    half = 24
    padded = np.concatenate([np.zeros(half), x, np.zeros(half + 2)])
    y = np.zeros(count)
    for k in range(-half + 1, half + 1):
        d = k - frac
        w = np.where(np.abs(d) < half, 0.5 + 0.5 * np.cos(math.pi * d / half), 0.0)
        y += padded[base + k + half] * cutoff * np.sinc(cutoff * d) * w
    return y


def one_pole(x, rate, hz):
    """A one-pole low-pass's amplitude response, applied by FFT (no phase shift: fine for a fake)."""
    size = 1 << int(math.ceil(math.log2(len(x))))
    spec = np.fft.rfft(x, size)
    f = np.fft.rfftfreq(size, 1.0 / rate)
    return np.fft.irfft(spec / np.sqrt(1.0 + (f / hz) ** 2), size)[:len(x)]


def fake_recording(entry, rate, outs, faults, rng):
    """What a friend's interface would write for this patch: [channel, ...] at faults['rate']."""
    kind = entry["measure"]
    recorded = entry["outputs"] if kind == "tone_map" else [1, 2]
    chans = []
    for file_channel in range(len(recorded)):
        o = faults["swap"][file_channel] if file_channel < len(faults["swap"]) else file_channel
        x = np.array(outs[o], dtype=float)
        x = np.clip(x, -faults["clip"], faults["clip"])                      # the analogue stage's limit
        x = one_pole(x, rate, faults["treble_hz"])
        x = x[int(faults["lead_in_s"] * rate):]
        ratio = 1.0 + faults["clock_ppm"] * 1e-6
        if kind == "clicks" and o == 0:
            ratio *= 1.0 + faults["master_clock_ppm"] * 1e-6                 # only the master clock's clicks
        y = resample(x, rate, faults["rate"], ratio)
        y *= faults["polarity"] * 10 ** (faults["gain_db"][o + 1] / 20.0)
        tt = np.arange(len(y)) / float(faults["rate"])
        y += rng.normal(0.0, 10 ** (faults["noise_dbfs"] / 20.0), len(y))
        y += math.sqrt(2.0) * 10 ** (faults["hum_dbfs"] / 20.0) * np.sin(2 * math.pi * 50.0 * tt + 0.3)
        chans.append(y)
    return chans


def stimulus_96k():
    return resample(np.array(make_stimulus.stimulus(), dtype=float), bf.STIMULUS["rate"], 96000)


def toy_input_prediction():
    """B10's stand-in expected values: a G2 whose ADC clips at its full scale and whose DAC gives INPUT_GAIN of it
    INPUT_FRAMES later, played as expected.py plays the emulated one (the stimulus's full scale at the ADC's, then
    10 dB above)."""
    runs = []
    for level_db in (0.0, 10.0):
        came_in = stimulus_96k() * 10 ** (level_db / 20.0)
        through = INPUT_GAIN * np.clip(np.concatenate([np.zeros(INPUT_FRAMES), came_in])[:len(came_in)], -1.0, 1.0)
        runs += [level_db, through, came_in]
    return bf.input_prediction(96000, *runs)


def fake_input_recording(faults, rng):
    """B10: the stimulus through the fake G2 (IN 1 to OUT 1) and looped back. OUT 1 is on the interface input it is
    on for every other patch: with Out 1 and 2 crossed that is the file's second channel, the loopback its first."""
    f = faults["input"]
    up = stimulus_96k()
    shift = int(round(f["delay_ms"] * 96.0))
    adc = np.clip(np.concatenate([np.zeros(shift), up])[:len(up)] * f["polarity"] * 10 ** (f["adc_db"] / 20.0), -1.0, 1.0)
    through = resample(adc * INPUT_GAIN * faults["polarity"] * 10 ** (faults["gain_db"][1] / 20.0), 96000, faults["rate"])
    loop = resample(up, 96000, faults["rate"]) * 10 ** (f["loopback_db"] / 20.0)
    n = min(len(through), len(loop))
    lead = np.zeros(int(0.8 * faults["rate"]))
    noise = lambda: rng.normal(0.0, 10 ** (faults["noise_dbfs"] / 20.0), n + len(lead))  # noqa: E731
    chans = [np.concatenate([lead, through[:n]]) + noise(), np.concatenate([lead, loop[:n]]) + noise()]
    return chans[::-1] if faults["swap"][0] == 1 else chans


# ---- checks ----------------------------------------------------------------------------------------------------

class Checks:
    def __init__(self):
        self.failed = 0

    def near(self, what, got, want, tol):
        ok = got is not None and abs(got - want) <= tol
        print("%s %-58s %s (want %s +/- %s)" % ("ok  " if ok else "FAIL", what, got, want, tol))
        self.failed += not ok

    def same(self, what, got, want):
        ok = got == want
        print("%s %-58s %r (want %r)" % ("ok  " if ok else "FAIL", what, got, want))
        self.failed += not ok

    def true(self, what, ok, detail=""):
        print("%s %-58s %s" % ("ok  " if ok else "FAIL", what, detail))
        self.failed += not ok


def check_results(c, res, faults, b10_expected, b10_rate, b10_through):
    """b10_expected: B10's expected JSON; b10_through: the channel of its recording that holds the G2's Out 1."""
    g = faults["gain_db"]
    p = res["patches"]
    c.near("B01 chain gain, Out 1", res["gain_db"].get("1"), g[1], 0.05)
    c.near("B01 chain gain, Out 2", res["gain_db"].get("2"), g[2], 0.05)
    c.near("B01 clock (ppm)", res["clock_ppm"], faults["clock_ppm"], 1.0)
    c.same("B03 channel -> output", [ch["output"] for ch in p["B03"]["channels"]], [o + 1 for o in faults["swap"]])
    for ch in p["B03"]["channels"]:
        c.near("B03 gain of Out %d" % ch["output"], ch["gain_db"], g[ch["output"]], 0.1)
    # B02: Out 1 (Pad 0 dB) stays under the analogue limit, Out 2 (Pad +6 dB) hits it at the +9 dB step (0.70 > 0.60)
    b02 = p["B02"]["outputs"]
    c.same("B02 Out 1 first clipped step = ours", b02["1"]["first_clipped"], b02["1"]["our_first_clipped"])
    c.same("B02 Out 2 first clipped step", b02["2"]["first_clipped"], "+9 dB")
    c.same("B02 Out 2 last clean step", b02["2"]["last_clean"], "+6 dB")
    c.near("B02 Out 1 gain of the clean steps", b02["1"]["step_gain_db"], g[1], 0.1)
    c.near("B04 pad step (dB)", p["B04"]["outputs"]["1"]["pad_step_db"], 6.02, 0.06)
    b05 = p["B05"]["outputs"]["1"]
    c.near("B05 noise level deviation, 100 Hz - 16 kHz (dB)", b05["deviation_db"], -0.5, 0.5)  # the treble loss takes a little
    at = dict((b[0], b[1]) for b in b05["response_db"])
    want = -10 * math.log10(1 + (16000 / faults["treble_hz"]) ** 2) + 10 * math.log10(1 + (1000 / faults["treble_hz"]) ** 2)
    c.near("B05 response at 16 kHz (dB)", at.get(16000), want, 0.6)
    c.near("B05 response at 100 Hz (dB)", at.get(100), 0.0, 0.6)
    b06 = p["B06"]["outputs"]["1"]["steps"]
    c.same("B06 notes measured", len(b06), 8)
    c.near("B06 clock from the first note (ppm)", b06[0]["ppm"], faults["clock_ppm"], 5.0)
    c.near("B06 clock from the last note (ppm)", b06[-1]["ppm"], faults["clock_ppm"], 5.0)
    c.true("B06 harmonics compared", all(s["harmonics_compared"] >= 1 for s in b06), str([s["harmonics_compared"] for s in b06]))
    mid = min((h for h in b06[0]["harmonics"] if h[3] is not None), key=lambda h: abs(h[0] - 10000.0))
    c.near("B06 a harmonic near 10 kHz, first note (dB)", mid[3], -10 * math.log10(1 + (mid[0] / faults["treble_hz"]) ** 2), 0.6)
    b07 = p["B07"]["outputs"]["1"]
    c.near("B07 noise floor (dBFS)", b07["rms_dbfs"], 10 * math.log10(10 ** (faults["noise_dbfs"] / 10) + 10 ** (faults["hum_dbfs"] / 10)), 1.0)
    c.near("B07 50 Hz hum (dBFS)", b07["hum_dbfs"]["50"], faults["hum_dbfs"], 1.5)
    b08 = p["B08"]
    c.same("B08 click polarity", b08["outputs"]["1"]["polarity"], -1)
    c.near("B08 master clock against the patch's own (ppm)", b08["master_vs_own_ppm"], faults["master_clock_ppm"], 20.0)
    c.near("B08 own clock (ppm)", b08["outputs"]["2"]["ppm"], faults["clock_ppm"], 5.0)
    for pid in ("B09a", "B09b", "B09c", "B09d"):
        for o, v in p[pid]["outputs"].items():
            c.true("%s Out %s envelope correlation > 0.97" % (pid, o), v["envelope_correlation"] > 0.97, str(v["envelope_correlation"]))
            # the recording's Out 1 and 2 were swapped back by B03; the remaining level error is the treble loss and noise
            c.near("%s Out %s level deviation (dB)" % (pid, o), v["level_deviation_db"], 0.0, 0.6)
    b10 = p["B10"]
    f = faults["input"]
    ours = ba.db(INPUT_GAIN)
    c.true("B10 the loopback found on channel 1", any("taken as the G2's output" in n for n in res["notes"]) == (faults["swap"][0] == 1))
    c.near("B10 delay (ms)", b10.get("delay_ms"), round(f["delay_ms"] * 96.0) / 96.0, 0.005)
    c.near("B10 delay from the bursts (ms)", b10.get("burst_delay_ms"), f["delay_ms"], 0.03)
    c.near("B10 gain in volts (dB)", b10.get("small_signal_gain_db"), f["adc_db"] + ours + g[1] - f["loopback_db"], 0.1)
    c.same("B10 polarity", b10.get("polarity"), int(f["polarity"] * faults["polarity"]))
    c.same("B10 polarity of the input itself", b10.get("input_polarity"), int(f["polarity"]))
    c.near("B10 the DAC's full scale over the ADC's (dB)", b10.get("dac_over_adc_full_scale_db"), f["adc_db"] + g[1] - f["loopback_db"], 0.1)
    c.near("B10 the stimulus's full scale at the ADC (dB)", b10.get("stimulus_full_scale_re_adc_db"), f["adc_db"], 0.1)
    c.near("B10 the ADC's full scale (dBu, Q08 = 18)", b10.get("adc_full_scale_dbu"), 18.0 - f["adc_db"] + f["loopback_db"], 0.1)
    clip, last = b10.get("first_compressed_step") or {}, b10.get("last_clean_step") or {}
    c.same("B10 first compressed step (dBFS sent)", clip.get("stimulus_dbfs"), -1.0)  # 2 dB over the fake ADC's full scale
    c.near("B10 that step at the ADC (dB re full scale)", clip.get("adc_dbfs"), -1.0 + f["adc_db"], 0.1)
    c.near("B10 the last clean step at the ADC (dB re full scale)", last.get("adc_dbfs"), -6.0 + f["adc_db"], 0.1)
    c.near("B10 our gain (dB)", (b10.get("expected") or {}).get("gain_db"), ours, 0.02)
    # the same recording without the loopback cable: no gain and no delay, but still where the input gives up
    alone = analyse.compare_input({"gain": res["gain_db"]}, b10_expected, bf.input_facts(b10_rate, [b10_through]))
    c.same("B10 alone: no loopback, no gain", (alone.get("loopback"), alone.get("small_signal_gain_db"), alone.get("delay_ms")), (False, None, None))
    c.same("B10 alone: first compressed step (dBFS sent)", (alone.get("first_compressed_step") or {}).get("stimulus_dbfs"), -1.0)
    c.near("B10 alone: the stimulus's full scale at the ADC (dB)", alone.get("stimulus_full_scale_re_adc_db"), f["adc_db"], 0.1)
    c.same("form: model", res["form"].get("model"), "G2X")
    c.same("form: an answer in French", res["form"].get("voices_b11"), "32 (32)")
    c.same("form: an answer on two lines", res["form"].get("store_protected"), "Memory is protected!")


def plain_checks(c):
    """The measurements themselves, on short signals: these run without numpy too."""
    rate = 48000
    x = [0.25 * math.sin(2 * math.pi * 1000.37 * i / rate + 0.4) + 0.0025 * math.sin(2 * math.pi * 2000.74 * i / rate) for i in range(rate)]
    if np is not None:
        x = np.array(x)
    f = bf.tone_facts(x, rate, 1000.0)
    c.near("tone frequency (Hz)", f["hz"], 1000.37, 0.002)
    c.near("tone level (dBFS)", f["tone_dbfs"], ba.db(0.25), 0.02)
    c.near("distortion (dB)", f["thd_db"], -40.0, 0.3)
    env = ba.envelope(x, rate, 0.005)
    c.near("envelope block (rms)", env[10], 0.25 / math.sqrt(2), 0.002)
    loop = [0.0] * 20 + [1.0] * 30 + [0.5] * 50
    rec = (loop[63:] + loop * 3)[:300]
    c.same("loop offset (blocks)", ba.loop_offset(rec, loop)[0], 37)
    clicks = [0.0] * (rate * 3)
    for k in range(5):
        a = int(1000 + k * 24000.5)
        for i in range(40):
            clicks[a + i] = -0.5 * math.exp(-i / 10.0)
    if np is not None:
        clicks = np.array(clicks)
    k = bf.click_facts(clicks, rate)
    c.near("click tempo (BPM)", k["bpm"], 60.0 * rate / 24000.5, 0.01)
    c.same("click polarity", k["polarity"], -1)
    bands = ba.band_levels(x, rate)
    c.near("1 kHz third-octave band (dBFS)", bands[ba.THIRD_OCTAVES.index(1000)], ba.db(0.25 / math.sqrt(2)), 0.3)
    pk = ba.peaks(x, rate, -60.0)
    c.near("strongest spectral peak (Hz)", pk[0][0], 1000.37, 3.0)
    path = os.path.join(tempfile.gettempdir(), "benchpack-selftest-%d.wav" % os.getpid())
    for bits in (16, 24, 32):
        ba.write_wav(path, rate, [x[:1000], x[:1000]], bits)
        _, ch = ba.read_wav(path)
        c.near("WAV %d-bit round trip" % bits, max(abs(a - b) for a, b in zip(ch[1], x[:1000])), 0.0, 2.0 ** -(min(bits, 24) - 1))
    os.remove(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emulator", action="store_true", help="use the emulated G2's renders and the committed expected JSON")
    ap.add_argument("--work", help="keep the generated files in this folder")
    ap.add_argument("--plain", action="store_true", help="only the checks that need no numpy")
    args = ap.parse_args()
    c = Checks()
    plain_checks(c)
    if args.plain:
        return 1 if c.failed else 0
    # the same measurements in plain Python
    done = subprocess.run([sys.executable, os.path.abspath(__file__), "--plain"], env=dict(os.environ, BENCH_NO_NUMPY="1"),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    c.true("the measurements without numpy", done.returncode == 0, "" if done.returncode == 0 else "\n" + done.stdout)
    if np is None:
        print("numpy is not installed: the synthetic recordings are skipped")
        return 1 if c.failed else 0

    work = args.work or tempfile.mkdtemp(prefix="benchpack-selftest-")
    rec_dir = os.path.join(work, "recordings")
    os.makedirs(rec_dir, exist_ok=True)
    with open(os.path.join(PACK, "manifest.json")) as f:
        entries = json.load(f)["patches"]
    rng = np.random.default_rng(162)
    if args.emulator:
        expected_dir = os.path.join(PACK, "expected")
        renders = os.path.join(expected_dir, "renders")
        if not os.path.isdir(renders):
            sys.exit("no %s: run tools/benchpack/expected.py first" % renders)
    else:
        expected_dir = os.path.join(work, "expected")
        os.makedirs(expected_dir, exist_ok=True)
    for e in entries:
        if not e["record_seconds"]:
            if not args.emulator:  # voices and controls: what the form is compared with
                with open(os.path.join(expected_dir, e["name"] + ".json"), "w") as f:
                    json.dump({"id": e["id"], "name": e["name"], "measure": e["measure"], "status": "ok",
                               "facts": {"requested_voices": 32, "voices": 4, "display_knobs_1_2": "120 BPM  04 (32)"}}, f)
            continue
        if e["measure"] == "input":
            chans = fake_input_recording(FAULTS, rng)
            if not args.emulator:
                facts, summary = toy_input_prediction()
                with open(os.path.join(expected_dir, e["name"] + ".json"), "w") as f:
                    f.write(bf.dumps({"id": e["id"], "name": e["name"], "measure": "input", "status": "ok", "facts": facts, "summary": summary}) + "\n")
        else:
            if args.emulator:
                rate, outs = ba.read_wav(os.path.join(renders, e["name"] + ".wav"))
            else:
                rate, outs = toy_render(e)
                facts = bf.measure(e, rate, outs)
                with open(os.path.join(expected_dir, e["name"] + ".json"), "w") as f:
                    f.write(bf.dumps({"id": e["id"], "name": e["name"], "measure": e["measure"], "status": "ok", "facts": facts}) + "\n")
            chans = fake_recording(e, rate, outs, FAULTS, rng)
        if e["measure"] == "tone_map":  # as two stereo files
            ba.write_wav(os.path.join(rec_dir, e["name"] + " 1-2.wav"), FAULTS["rate"], chans[:2], 24)
            ba.write_wav(os.path.join(rec_dir, e["name"] + " 3-4.wav"), FAULTS["rate"], chans[2:], 24)
        else:
            ba.write_wav(os.path.join(rec_dir, e["name"] + ".wav"), FAULTS["rate"], chans, 24)
    with open(os.path.join(rec_dir, "form.txt"), "w", encoding="utf-8") as f:
        f.write(FORM)
    res = analyse.analyse(rec_dir, None, expected_dir)
    text = analyse.report(res)
    with open(os.path.join(work, "report.md"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    with open(os.path.join(work, "results.json"), "w") as f:
        f.write(bf.dumps(res) + "\n")
    for note in res["notes"]:
        print("note: " + note)
    c.true("no patch failed to analyse", not any("could not be analysed" in n for n in res["notes"]))
    b10 = next(e["name"] for e in entries if e["measure"] == "input")
    with open(os.path.join(expected_dir, b10 + ".json")) as f:
        b10_expected = json.load(f)
    b10_rate, b10_channels = ba.read_wav(os.path.join(rec_dir, b10 + ".wav"))
    check_results(c, res, FAULTS, b10_expected, b10_rate, b10_channels[1 if FAULTS["swap"][0] == 1 else 0])
    if not args.work and not c.failed:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("report: %s" % os.path.join(work, "report.md"))
    print("%d check(s) failed" % c.failed if c.failed else "all checks passed")
    return 1 if c.failed else 0


if __name__ == "__main__":
    sys.exit(main())
