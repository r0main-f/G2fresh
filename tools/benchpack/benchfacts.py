"""What the bench pack measures in each patch's audio: the same code for the emulated G2's renders (expected.py) and
for the recordings of real G2s (analyse.py), so that the two sets of numbers compare like with like.

measure(entry, rate, channels, clock=1.0, reference=None) returns the facts of one patch:
  entry      its record in patches/manifest.json (what the patch is designed to do)
  channels   the audio, one list or array per recorded channel (a render: Out 1-4)
  clock      the recording's clock relative to ours (1.0001 = the G2 runs 100 ppm fast): times of looped patches scale
  reference  the expected facts; with them a looped patch is aligned on its loop envelope, without them (a render)
             on its silent gap
"""
import json
import math

import benchaudio as ba

BLOCK_S = 0.005   # envelope resolution
GUARD_S = 0.15    # left out at both ends of a step
PREROLL_S = 0.002 # a loop starts this long before its first sound, so that the onset is never cut in two
FLOOR_DB = -120.0
STIMULUS = {      # B10: what "B10 Stimulus.wav" plays (make_stimulus.py), times in seconds
    "rate": 48000, "tone_hz": 1000.0,
    "bursts": {"times": [1.0, 1.5, 2.0], "seconds": 0.02, "dbfs": -6.0},
    "steady": {"start": 3.0, "seconds": 3.0, "dbfs": -20.0},
    "steps": {"start": 6.5, "seconds": 1.0, "gap": 0.2, "dbfs": [-40.0, -30.0, -20.0, -12.0, -6.0, -1.0]},
    "total": 14.0,
}


def dumps(obj, indent=0):
    """JSON with one key per line and lists of plain values on one line (the envelopes are long)."""
    pad = " " * (indent + 1)
    if isinstance(obj, dict):
        flat = json.dumps(obj)
        if len(flat) <= 110 and all(not isinstance(v, (dict, list, tuple)) for v in obj.values()):
            return flat
        return "{\n" + ",\n".join(pad + json.dumps(str(k)) + ": " + dumps(v, indent + 1) for k, v in obj.items()) + "\n" + " " * indent + "}"
    if isinstance(obj, (list, tuple)):
        if all(not isinstance(v, (dict, list, tuple)) or (isinstance(v, (list, tuple)) and len(v) <= 2) for v in obj):
            return json.dumps(obj)
        return "[\n" + ",\n".join(pad + dumps(v, indent + 1) for v in obj) + "\n" + " " * indent + "]"
    return json.dumps(obj)


def r(x, digits=3):
    return None if x is None else round(float(x), digits)


def to_db(values):
    return [round(max(ba.db(v), FLOOR_DB), 1) for v in values]


def from_db(values):
    return [10.0 ** (v / 20.0) if v > FLOOR_DB else 0.0 for v in values]


def smooth(env, half=3):
    """An envelope's power through a triangular window of 2 * half + 1 blocks: a click that falls just before a block
    boundary in one recording and just after it in another then counts nearly the same."""
    n = len(env)
    sq = [v * v for v in env]
    total = float((half + 1) ** 2)
    out = []
    for i in range(n):
        acc = 0.0
        for k in range(-half, half + 1):
            if 0 <= i + k < n:
                acc += (half + 1 - abs(k)) * sq[i + k]
        out.append(math.sqrt(acc / total))
    return out


# ---- single measurements -------------------------------------------------------------------------------------

def tone_facts(x, rate, guess_hz):
    """A steady sine: frequency, level and harmonic distortion (harmonics 2-5 that fit under 0.45 x the sample rate)."""
    if ba.peak(x) < 1e-7:
        return {"hz": None, "rms_dbfs": None, "peak_dbfs": None, "thd_db": None}
    hz = ba.tone_frequency(x, rate, guess_hz)
    amp, _ = ba.tone(x, rate, hz)
    harmonics = [ba.tone(x, rate, hz * k)[0] for k in range(2, 6) if hz * k < 0.45 * rate]
    thd = math.sqrt(math.fsum(a * a for a in harmonics)) / amp if amp > 0 and harmonics else 0.0
    return {"hz": r(hz, 4), "rms_dbfs": r(ba.db(ba.rms(x)), 2), "tone_dbfs": r(ba.db(amp), 2), "peak_dbfs": r(ba.db(ba.peak(x)), 2),
            "thd_db": r(max(ba.db(thd), -140.0), 1)}


def noise_facts(x, rate):
    level = ba.rms(x)
    if level < 1e-9:
        return {"rms_dbfs": None, "peak_dbfs": None, "crest_db": None, "bands_hz": ba.THIRD_OCTAVES, "bands_dbfs": None, "dc": 0.0}
    return {"rms_dbfs": r(ba.db(level), 2), "peak_dbfs": r(ba.db(ba.peak(x)), 2), "crest_db": r(ba.db(ba.peak(x) / level), 2),
            "bands_hz": ba.THIRD_OCTAVES, "bands_dbfs": ba.band_levels(x, rate), "dc": r(ba.mean(x), 6)}


def silence_facts(x, rate):
    f = noise_facts(x, rate)
    if f["rms_dbfs"] is not None:  # mains hum and its harmonics, 50 and 60 Hz systems
        f["hum_dbfs"] = {str(hz): r(ba.db(ba.tone(x, rate, hz)[0] / math.sqrt(2.0)), 1) for hz in (50, 60, 100, 120, 150, 180)}
    return f


def click_facts(x, rate):
    # against the usual click's height (the median of the half seconds' peaks), not the file's highest sample: a
    # thump when the patch loads must not hide the clicks
    n = int(0.5 * rate)
    tops = sorted(ba.peak(x[i:i + n]) for i in range(0, max(1, len(x) - n + 1), n))
    times, signs = ba.onsets(x, rate, 0.5, 0.2, tops[len(tops) // 2])
    if len(times) < 3:
        return {"clicks": len(times), "bpm": None}
    period, t0, res, count = ba.fit_period(times)
    return {"clicks": count, "period_s": r(period, 7), "bpm": r(60.0 / period, 4), "jitter_ms": r(res * 1000.0, 4), "first_s": r(t0, 5),
            "polarity": 1 if sum(signs) >= 0 else -1, "peak_dbfs": r(ba.db(ba.peak(x)), 2)}


# ---- loops ---------------------------------------------------------------------------------------------------

def origins_by_silence(x, rate, loop_s):
    """Loop starts of an emulator render: the first sound after the longest silence, then every loop_s after it
    (measured: the patch's own clock sets the length). Returns ([start_s, ...], loop_s as measured)."""
    env = ba.envelope(x, rate, BLOCK_S)
    top = max(env) if env else 0.0
    if top <= 0:
        return [], loop_s
    quiet = top * 1e-3
    events, run = [], 0  # (silence length in blocks, block index where sound starts)
    for i, v in enumerate(env):
        if v < quiet:
            run += 1
        else:
            if run * BLOCK_S >= 0.3 and i > run:  # a gap that began inside the render
                events.append((run, i))
            run = 0
    if not events:
        return [], loop_s
    longest = max(e[0] for e in events)
    marks = [i for n, i in events if n >= 0.8 * longest]
    # to the sample: the first one above the threshold in the two blocks around the mark
    n = int(round(BLOCK_S * rate))
    starts = []
    for i in marks:
        a = max(0, (i - 1) * n)
        seg = x[a:a + 2 * n]
        k = next((k for k in range(len(seg)) if abs(seg[k]) >= quiet), n)
        starts.append(max(0.0, (a + k) / float(rate) - PREROLL_S))
    if len(starts) >= 2:
        gaps = sorted(b - a for a, b in zip(starts, starts[1:]))
        loop_s = gaps[len(gaps) // 2]
    return [s for s in starts if s + loop_s <= len(x) / float(rate) + 1e-9], loop_s


def origins_by_reference(x, rate, align, loop_s, clock):
    """Loop starts of a recording, from the expected loop envelope: ([start_s, ...], correlation)."""
    block = align["block_s"] / clock
    ref = smooth(from_db(align["envelope_db"]))
    env = smooth(ba.envelope(x, rate, block))
    off, _ = ba.loop_offset(env, ref)
    # finer than a block: the envelope from shifted block grids, over the first loops, in tenths and then fiftieths
    # of a block. Smoothed less than above, so that edges count; not unsmoothed, or a train of clicks would rather
    # lose one click whole than have several sit on block boundaries
    ref = smooth(from_db(align["envelope_db"]), 1)
    period = loop_s / clock
    span = min(len(x), int((off * block + 2.2 * period + block) * rate))

    def quality(start):
        if start < 0:
            start += period
        e = smooth(ba.envelope(x[:span], rate, block, start), 1)
        return (ba.correlate(e, [ref[i % len(ref)] for i in range(len(e))]) if len(e) >= len(ref) // 2 else -2.0), start

    best_c, best = max(quality((off + step * 0.1) * block) for step in range(-10, 11))
    best_c, best = max([quality(best + step * 0.02 * block) for step in range(-5, 6)] + [(best_c, best)])
    while best - period >= 0:
        best -= period
    total = len(x) / float(rate)
    starts = []
    while best + period <= total + 1e-9:
        starts.append(best)
        best += period
    return starts, best_c


def loop_envelope(x, rate, start_s, loop_s, block_s=BLOCK_S):
    count = int(round(loop_s / block_s))
    return ba.envelope(ba.part(x, rate, start_s, start_s + loop_s + block_s), rate, block_s)[:count]


def average_db(values):
    values = [v for v in values if v is not None]
    return r(math.fsum(values) / len(values), 2) if values else None


def step_windows(entry, starts, period):
    """(step index, window start, window stop) of every sounding step of every complete loop of `period` seconds.
    The loop starts at the first sounding step: the silent step that opens the sequence closes the loop."""
    steps = entry["steps"]
    step_s = period / len(steps)
    lead = next(i for i, s in enumerate(steps) if s["label"] != "silence")
    out = []
    for start in starts:
        for i, s in enumerate(steps):
            if s["label"] == "silence":
                continue
            a = start + (i - lead) * step_s
            out.append((i, a + GUARD_S, a + step_s - GUARD_S))
    return out


# ---- per patch -----------------------------------------------------------------------------------------------

def measure(entry, rate, channels, clock=1.0, reference=None):
    kind = entry["measure"]
    outs = [o for o in entry.get("outputs", []) if o <= len(channels)]
    facts = {"outputs": {}}
    # a steady patch: its middle, away from whatever the start and the end of a take hold
    trim = lambda x: ba.part(x, rate, max(0.5, 0.1 * len(x) / rate), len(x) / float(rate) - max(0.2, 0.05 * len(x) / rate))  # noqa: E731

    if kind == "tone":
        for o in outs:
            facts["outputs"][str(o)] = tone_facts(trim(channels[o - 1]), rate, entry["tone_hz"] * clock)
    elif kind == "tone_map":
        for o in outs:
            facts["outputs"][str(o)] = tone_facts(trim(channels[o - 1]), rate, entry["tones_hz"][str(o)] * clock)
    elif kind == "noise":
        for o in outs:
            facts["outputs"][str(o)] = noise_facts(trim(channels[o - 1]), rate)
    elif kind == "silence":
        for o in outs:
            facts["outputs"][str(o)] = silence_facts(trim(channels[o - 1]), rate)
    elif kind == "clicks":
        for o in outs:
            facts["outputs"][str(o)] = click_facts(channels[o - 1], rate)
    elif kind in ("level_steps", "pad_steps", "saw_steps", "phrase"):
        loop_s = reference["loop_s"] if reference else entry["loop_seconds"]
        lead = channels[(reference["align"]["output"] if reference else outs[0]) - 1] if outs else []
        if reference:
            starts, quality = origins_by_reference(lead, rate, reference["align"], loop_s, clock)
            facts["alignment"] = r(quality, 4)
        else:
            starts, loop_s = origins_by_silence(lead, rate, loop_s)
        facts["loop_s"] = r(loop_s, 6)
        facts["loops"] = len(starts)
        facts["first_loop_s"] = r(starts[0], 5) if starts else None
        if not starts:
            return facts
        period = loop_s / clock if reference else loop_s
        if not reference:
            facts["align"] = {"block_s": BLOCK_S, "output": outs[0], "envelope_db": to_db(loop_envelope(lead, rate, starts[0], loop_s))}
        for o in outs:
            x = channels[o - 1]
            f = {}
            if kind == "phrase":
                block = BLOCK_S / clock if reference else BLOCK_S
                # the loops' mean power: what one loop adds of its own (a reverb's tail) weighs less
                loops = [loop_envelope(x, rate, start, period, block) for start in starts]
                count = min(len(e) for e in loops)
                f["envelope_db"] = to_db([math.sqrt(math.fsum(e[i] ** 2 for e in loops) / len(loops)) for i in range(count)])
                whole = ba.part(x, rate, starts[0], starts[0] + period * len(starts))
                f["rms_dbfs"], f["peak_dbfs"] = r(ba.db(ba.rms(whole)), 2), r(ba.db(ba.peak(whole)), 2)
                f["bands_hz"], f["bands_dbfs"] = ba.THIRD_OCTAVES, ba.band_levels(whole, rate)
                if len(starts) >= 2:  # how alike two successive loops are
                    a = ba.part(x, rate, starts[0], starts[0] + period)
                    b = ba.part(x, rate, starts[1], starts[1] + period)
                    n = min(len(a), len(b))
                    f["loop_to_loop_correlation"] = r(ba.correlate(a[:n], b[:n]), 5)
                    ea, eb = loop_envelope(x, rate, starts[0], period, block), loop_envelope(x, rate, starts[1], period, block)
                    f["loop_to_loop_envelope_correlation"] = r(ba.correlate(ea, eb), 5)
            else:
                per_step = {}
                for i, a, b in step_windows(entry, starts, period):
                    seg = ba.part(x, rate, a, b)
                    if kind == "saw_steps":
                        # the fundamental: where we found it, or (a render) the strongest partial; not the note's
                        # nominal pitch, which the G2 does not always play
                        found = ba.peaks(seg, rate, -70.0)[:40] if i not in per_step or not reference else []
                        ref_hz = reference["outputs"].get(str(o), {}).get("steps", [{}] * (i + 1))[i].get("hz") if reference else None
                        guess = ref_hz * clock if ref_hz else (found[0][0] if found else 440.0 * 2 ** ((entry["steps"][i]["midi_note"] - 69) / 12.0))
                        hz = ba.tone_frequency(seg, rate, guess)
                        m = {"hz": hz, "rms_dbfs": ba.db(ba.rms(seg)), "tone_dbfs": ba.db(ba.tone(seg, rate, hz)[0])}
                        if i not in per_step:  # the partials: once, from the first loop
                            m["peaks"] = [[r(p, 1), r(v, 1)] for p, v in found]
                    else:
                        t = tone_facts(seg, rate, entry["tone_hz"] * clock)
                        m = {"hz": t["hz"], "rms_dbfs": t["rms_dbfs"], "tone_dbfs": t.get("tone_dbfs"), "thd_db": t["thd_db"]}
                    per_step.setdefault(i, []).append(m)
                f["steps"] = []
                for i, s in enumerate(entry["steps"]):
                    ms = per_step.get(i)
                    if not ms:
                        f["steps"].append({"label": s["label"]})
                        continue
                    hzs = [m["hz"] for m in ms if m["hz"]]
                    row = {"label": s["label"], "hz": r(math.fsum(hzs) / len(hzs), 4) if hzs else None,
                           "rms_dbfs": average_db([m["rms_dbfs"] for m in ms]), "tone_dbfs": average_db([m["tone_dbfs"] for m in ms])}
                    if "thd_db" in ms[0]:
                        row["thd_db"] = average_db([m["thd_db"] for m in ms])
                    if "peaks" in ms[0]:
                        row["peaks"] = ms[0]["peaks"]
                    f["steps"].append(row)
            facts["outputs"][str(o)] = f
    elif kind == "input":
        facts.update(input_facts(rate, channels))
    return facts


def input_facts(rate, channels):
    """B10: channel 1 is the stimulus through the G2 (In 1 to Out 1), channel 2 the same stimulus looped back from
    the interface's second output to its second input. The loopback gives the time and level reference."""
    s = STIMULUS
    facts = {"channels": len(channels)}
    if not channels:
        return facts
    through = channels[0]
    loop = channels[1] if len(channels) > 1 and ba.peak(channels[1]) > 1e-4 else None
    facts["loopback"] = loop is not None

    def burst_times(x):
        times, _ = ba.onsets(x, rate, 0.15, 0.3)
        # the first three onsets half a second apart: the bursts
        for i in range(len(times) - 2):
            if abs(times[i + 1] - times[i] - 0.5) < 0.02 and abs(times[i + 2] - times[i + 1] - 0.5) < 0.02:
                # each one again against its own peak, so that the two channels' levels do not matter
                fine, signs = [], []
                for t in times[i:i + 3]:
                    a = max(0.0, t - 0.003)
                    local, sign = ba.onsets(ba.part(x, rate, a, t + s["bursts"]["seconds"]), rate, 0.5, 1.0)
                    fine.append(a + local[0] if local else t)
                    signs.append(sign[0] if sign else 1)
                return fine, signs
        return [], []

    t_through, sign_through = burst_times(through)
    if not t_through:
        facts["error"] = "the three bursts at the start were not found on channel 1"
        return facts
    t_ref, sign_ref = burst_times(loop) if loop is not None else ([], [])
    base = (t_ref[0] if t_ref else t_through[0]) - s["bursts"]["times"][0]  # where the stimulus starts in the file
    if t_ref:
        delays = [a - b for a, b in zip(t_through, t_ref)]
        facts["delay_ms"] = r(1000.0 * math.fsum(delays) / len(delays), 4)
        facts["delay_spread_ms"] = r(1000.0 * (max(delays) - min(delays)), 4)
        facts["polarity"] = 1 if sum(a * b for a, b in zip(sign_through, sign_ref)) >= 0 else -1
    delay = facts.get("delay_ms", 0.0) / 1000.0

    def level(x, start, seconds, shift):
        seg = ba.part(x, rate, base + shift + start + 0.1, base + shift + start + seconds - 0.1)
        hz = ba.tone_frequency(seg, rate, s["tone_hz"])
        return ba.db(ba.tone(seg, rate, hz)[0]), tone_facts(seg, rate, hz)["thd_db"]

    st = s["steady"]
    lt, _ = level(through, st["start"], st["seconds"], delay)
    facts["steady_through_dbfs"] = r(lt, 2)
    if loop is not None:
        ll, _ = level(loop, st["start"], st["seconds"], 0.0)
        facts["steady_loopback_dbfs"] = r(ll, 2)
        facts["gain_db"] = r(lt - ll, 2)
    rows = []
    for i, nominal in enumerate(s["steps"]["dbfs"]):
        start = s["steps"]["start"] + i * (s["steps"]["seconds"] + s["steps"]["gap"])
        lt, thd = level(through, start, s["steps"]["seconds"], delay)
        row = {"stimulus_dbfs": nominal, "through_dbfs": r(lt, 2), "thd_db": thd}
        if loop is not None:
            ll, _ = level(loop, start, s["steps"]["seconds"], 0.0)
            row["loopback_dbfs"], row["gain_db"] = r(ll, 2), r(lt - ll, 2)
        rows.append(row)
    facts["steps"] = rows
    return facts
