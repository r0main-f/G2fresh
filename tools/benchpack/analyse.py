#!/usr/bin/env python3
"""Compares recordings of the hardware bench pack, made on a real G2, with what the emulated G2 gives.

    python3 tools/benchpack/analyse.py RECORDINGS_DIR [--form FILE] [--out report.md] [--json results.json]

RECORDINGS_DIR holds the WAV files a friend sent back, named like the patches ("B01 Level Ref.wav", ...; a patch
recorded as several files, e.g. "B03 Output Map 1-2.wav" and "... 3-4.wav", is read in name order), and their filled
form (form.txt or form.fr.txt; or --form). The expected values are bench/hardware-pack/expected/*.json (--expected).

Writes a short report in Markdown (to --out, default RECORDINGS_DIR/report.md, and to the terminal) and, with
--json, the numbers behind it. Needs only Python; numpy makes it fast.
"""
import argparse
import glob
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchaudio as ba  # noqa: E402
import benchfacts as bf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACK = os.path.join(ROOT, "bench", "hardware-pack")
DAC_TO_G2FRESH_DB = 20.0 * math.log10(8.0)

# The form's questions (form.txt / form.fr.txt: "Q07. ..." then "Answer:" / "Réponse :")
QUESTIONS = {
    "Q01": "name", "Q02": "model", "Q03": "expansion", "Q04": "os_version", "Q05": "interface", "Q06": "input_reference",
    "Q07": "input_gain", "Q08": "full_scale_dbu", "Q09": "recording_format", "Q10": "outputs_recorded", "Q11": "master_level",
    "Q12": "voices_b11", "Q13": "voices_b12", "Q14": "voices_b13", "Q15": "clock_at_power_on", "Q16": "pitch_stick",
    "Q17": "g_wheel_1", "Q18": "g_wheel_2", "Q19": "store_protected", "Q20": "comments",
}


def fmt(x, digits=2, unit=""):
    return "n/a" if x is None else ("%+.*f%s" if unit in (" dB", " ppm") else "%.*f%s") % (digits, x, unit)


def diff(a, b):
    return None if a is None or b is None else a - b


# ---- inputs --------------------------------------------------------------------------------------------------

def read_form(path):
    answers, key, running = {}, None, False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"(Q\d\d)\b", line)
            if m:
                key, running = m.group(1), False
                continue
            m = re.match(r"(Answer|R[ée]ponse)\s*:\s*(.*)$", line, re.IGNORECASE)
            if m and key:
                answers[key], running = m.group(2), True
            elif running and line:  # an answer that runs on, up to the next empty line
                answers[key] += " " + line
            else:
                running = False
    answers = {k: v.strip(" _.") for k, v in answers.items()}
    return {QUESTIONS.get(k, k): v for k, v in answers.items() if v}


def number_in(text):
    m = re.search(r"[-+]?\d+(?:[.,]\d+)?", text or "")
    return float(m.group(0).replace(",", ".")) if m else None


def find_recordings(folder, entry):
    """The WAV files of one patch, in name order."""
    wanted = entry["id"].lower()
    files = []
    for path in sorted(glob.glob(os.path.join(folder, "*"))):
        base = os.path.basename(path).lower()
        if base.endswith((".wav", ".wave")) and re.match(re.escape(wanted) + r"(?![0-9a-z])", base) and "stimulus" not in base:
            files.append(path)
    return files


def load_recording(files):
    rate, channels, notes = None, [], []
    for path in files:
        r, ch = ba.read_wav(path)
        if rate is not None and r != rate:
            notes.append("%s has another sample rate (%d Hz): left out" % (os.path.basename(path), r))
            continue
        rate = r
        channels.extend(ch)
    return rate, channels, notes


# ---- per patch -----------------------------------------------------------------------------------------------

def compare_tone(exp, rec):
    out = {}
    for o, e in exp["outputs"].items():
        m = rec["outputs"].get(o)
        if not m or m.get("hz") is None or e.get("hz") is None:
            continue
        out[o] = {"hz": m["hz"], "ppm": (m["hz"] / e["hz"] - 1.0) * 1e6, "tone_dbfs": m["tone_dbfs"], "expected_dbfs": e["tone_dbfs"],
                  "gain_db": m["tone_dbfs"] - e["tone_dbfs"], "thd_db": m["thd_db"], "expected_thd_db": e["thd_db"]}
    return {"outputs": out}


def compare_map(ctx, exp, rate, channels):
    """Which output each recorded channel carries, by the four tones' frequencies."""
    tones = {o: f["hz"] for o, f in exp["outputs"].items() if f.get("hz")}
    table = []
    for c, x in enumerate(channels):
        seg = ba.part(x, rate, max(0.5, 0.1 * len(x) / rate), len(x) / float(rate) - max(0.2, 0.05 * len(x) / rate))
        levels = {o: ba.db(ba.tone(seg, rate, ba.tone_frequency(seg, rate, hz * ctx["clock"]))[0]) for o, hz in tones.items()}
        best = max(levels, key=levels.get)
        others = [v for o, v in levels.items() if o != best]
        hz = ba.tone_frequency(seg, rate, tones[best] * ctx["clock"])
        table.append({"channel": c + 1, "output": int(best) if levels[best] > -70 else None, "level_dbfs": bf.r(levels[best], 2),
                      "hz": bf.r(hz, 4), "ppm": bf.r((hz / tones[best] - 1.0) * 1e6, 1), "crosstalk_db": bf.r(max(others) - levels[best], 1) if others else None,
                      "gain_db": bf.r(levels[best] - exp["outputs"][best]["tone_dbfs"], 2)})
    return {"channels": table}


def compare_steps(ctx, entry, exp, rec):
    out = {}
    for o, e in exp["outputs"].items():
        m = rec["outputs"].get(o)
        if not m or "steps" not in m:
            continue
        gain = ctx["gain"].get(o, ctx["gain"].get("1"))
        rows = []
        for es, ms in zip(e["steps"], m["steps"]):
            if es.get("tone_dbfs") is None or ms.get("tone_dbfs") is None:
                continue
            rows.append({"label": es["label"], "expected_dbfs": es["tone_dbfs"], "recorded_dbfs": ms["tone_dbfs"],
                         "gain_db": bf.r(ms["tone_dbfs"] - es["tone_dbfs"], 2),
                         "deviation_db": bf.r(ms["tone_dbfs"] - es["tone_dbfs"] - gain, 2) if gain is not None else None,
                         "thd_db": ms.get("thd_db"), "expected_thd_db": es.get("thd_db"), "hz": ms.get("hz")})
        out[o] = {"steps": rows}
        if entry["measure"] == "level_steps" and rows:
            # the reference: the gain of the quiet, clean steps of this very recording
            clean = [x["gain_db"] for x in rows if x["expected_thd_db"] is not None and x["expected_thd_db"] < -60 and x["expected_dbfs"] > -40]
            ref = sorted(clean)[len(clean) // 2] if clean else None
            ours = next((x["label"] for x in rows if x["expected_thd_db"] is not None and x["expected_thd_db"] > -40), None)
            # from the full-level step up: below it nothing clips, and noise would only confuse the distortion figure
            theirs = next((x for x in rows if ref is not None and x["expected_dbfs"] > -30
                           and (x["gain_db"] - ref < -0.5 or (x["thd_db"] is not None and x["thd_db"] > -40))), None)
            last_clean = None
            for x in rows:
                if theirs is not None and x is theirs:
                    break
                last_clean = x
            out[o].update(step_gain_db=bf.r(ref, 2), our_first_clipped=ours, first_clipped=theirs["label"] if theirs else None,
                          last_clean=last_clean["label"] if last_clean else None,
                          last_clean_dac_dbfs=last_clean["expected_dbfs"] if last_clean else None,
                          last_clean_recorded_dbfs=last_clean["recorded_dbfs"] if last_clean else None)
        if entry["measure"] == "pad_steps" and len(rows) >= 6:
            p0 = bf.average_db([x["recorded_dbfs"] for x in rows[:3]])
            p6 = bf.average_db([x["recorded_dbfs"] for x in rows[3:6]])
            e0 = bf.average_db([x["expected_dbfs"] for x in rows[:3]])
            e6 = bf.average_db([x["expected_dbfs"] for x in rows[3:6]])
            out[o].update(pad_step_db=bf.r(p6 - p0, 2), expected_pad_step_db=bf.r(e6 - e0, 2))
    return {"outputs": out, "alignment": rec.get("alignment"), "loops": rec.get("loops")}


def compare_noise(ctx, exp, rec, quiet=False):
    out = {}
    for o, e in exp["outputs"].items():
        m = rec["outputs"].get(o)
        if not m or m.get("rms_dbfs") is None:
            continue
        gain = ctx["gain"].get(o, ctx["gain"].get("1"))
        row = {"rms_dbfs": m["rms_dbfs"], "peak_dbfs": m["peak_dbfs"], "crest_db": m.get("crest_db")}
        if quiet:
            full = ctx["full_level_dbfs"].get(o, ctx["full_level_dbfs"].get("1"))
            row["below_full_level_db"] = bf.r(m["rms_dbfs"] - full, 1) if full is not None else None
            row["hum_dbfs"] = m.get("hum_dbfs")
            row["bands_hz"], row["bands_dbfs"] = m["bands_hz"], m["bands_dbfs"]
        elif e.get("rms_dbfs") is not None:
            row["expected_dbfs"] = e["rms_dbfs"]
            # the level in the bands both have (100 Hz to 16 kHz): the whole-band level depends on the recording's sample rate
            both = [(mb, eb) for hz, eb, mb in zip(e["bands_hz"], e["bands_dbfs"], m["bands_dbfs"]) if 100 <= hz <= 16000 and eb is not None and mb is not None]
            if both and gain is not None:
                power = lambda i: 10.0 * math.log10(math.fsum(10 ** (b[i] / 10.0) for b in both))  # noqa: E731
                row["deviation_db"] = bf.r(power(0) - power(1) - gain, 2)
            else:
                row["deviation_db"] = None
            # the response: band by band, relative to the 1 kHz band
            ref = e["bands_hz"].index(1000)
            bands = []
            for hz, eb, mb in zip(e["bands_hz"], e["bands_dbfs"], m["bands_dbfs"]):
                if eb is None or mb is None or e["bands_dbfs"][ref] is None or m["bands_dbfs"][ref] is None:
                    continue
                bands.append([hz, bf.r((mb - m["bands_dbfs"][ref]) - (eb - e["bands_dbfs"][ref]), 2)])
            row["response_db"] = bands
        out[o] = row
    return {"outputs": out}


def compare_saw(ctx, exp, rec, rate):
    out = {}
    for o, e in exp["outputs"].items():
        m = rec["outputs"].get(o)
        if not m or "steps" not in m:
            continue
        gain = ctx["gain"].get(o, ctx["gain"].get("1"))
        rows = []
        for es, ms in zip(e["steps"], m["steps"]):
            if not es.get("hz") or not ms.get("hz"):
                continue
            f0 = es["hz"]
            limit = 0.45 * rate / ctx["clock"]
            mine = [(p / ctx["clock"], v) for p, v in ms.get("peaks", [])]
            harmonics, aliases, extra = [], [], []
            used = set()
            for p, v in es.get("peaks", []):
                if p > limit:
                    continue
                k = p / f0
                is_harmonic = abs(k - round(k)) < 0.02
                tol = max(0.004 * p, 25.0)
                near = [(abs(q - p), i) for i, (q, _) in enumerate(mine) if abs(q - p) <= tol and i not in used]
                got = None
                if near:
                    i = min(near)[1]
                    used.add(i)
                    got = mine[i][1]
                (harmonics if is_harmonic else aliases).append([bf.r(p, 1), v, got, bf.r(got - v, 1) if got is not None else None])
            for i, (q, v) in enumerate(mine):
                k = q / f0
                if i not in used and q <= limit and abs(k - round(k)) >= 0.02 and v > -60.0:
                    extra.append([bf.r(q, 1), v])
            hd = [h[3] for h in harmonics if h[3] is not None]
            top = [h for h in harmonics if h[3] is not None]
            rows.append({"label": es["label"], "hz": ms["hz"], "ppm": bf.r((ms["hz"] / f0 - 1.0) * 1e6, 1),
                         "level_deviation_db": bf.r(ms["tone_dbfs"] - es["tone_dbfs"] - gain, 2) if gain is not None else None,
                         "harmonics_compared": len(hd), "harmonics_missing": len(harmonics) - len(hd),
                         "harmonic_deviation_max_db": bf.r(max(hd, key=abs), 1) if hd else None,
                         "highest_harmonic": max(top, key=lambda h: h[0]) if top else None, "harmonics": harmonics,
                         "our_aliases": aliases, "extra_partials": extra[:8]})
        out[o] = {"steps": rows}
    return {"outputs": out, "alignment": rec.get("alignment"), "loops": rec.get("loops")}


def compare_clicks(ctx, exp, rec):
    out = {}
    for o, e in exp["outputs"].items():
        m = rec["outputs"].get(o)
        if not m or not m.get("bpm"):
            out[o] = {"clicks": m.get("clicks") if m else 0}
            continue
        out[o] = {"clicks": m["clicks"], "bpm": m["bpm"], "expected_bpm": e.get("bpm"),
                  "ppm": bf.r((m["bpm"] / e["bpm"] - 1.0) * 1e6, 1) if e.get("bpm") else None, "jitter_ms": m["jitter_ms"],
                  "expected_jitter_ms": e.get("jitter_ms"), "polarity": m["polarity"], "expected_polarity": e.get("polarity"),
                  "peak_deviation_db": bf.r(m["peak_dbfs"] - e["peak_dbfs"] - ctx["gain"].get(o, ctx["gain"].get("1")), 2)
                  if e.get("peak_dbfs") is not None and ctx["gain"].get(o, ctx["gain"].get("1")) is not None else None}
    a, b = out.get("1", {}), out.get("2", {})
    res = {"outputs": out}
    if a.get("bpm") and b.get("bpm") and a.get("expected_bpm") and b.get("expected_bpm"):
        # the master clock against the patch's own clock, which is counted in samples: free of the recorder's clock
        res["master_vs_own_ppm"] = bf.r(((a["bpm"] / b["bpm"]) / (a["expected_bpm"] / b["expected_bpm"]) - 1.0) * 1e6, 1)
    return res


def compare_phrase(ctx, exp, rec):
    out = {}
    for o, e in exp["outputs"].items():
        m = rec["outputs"].get(o)
        if not m or "envelope_db" not in m:
            continue
        gain = ctx["gain"].get(o, ctx["gain"].get("1")) or 0.0
        n = min(len(e["envelope_db"]), len(m["envelope_db"]))
        el, ml = bf.from_db(e["envelope_db"][:n]), bf.from_db(m["envelope_db"][:n])
        # the shape: both through a short window (see benchfacts.smooth)
        shape = ba.correlate(bf.smooth(ml), bf.smooth(el))
        # the levels: in 50 ms stretches (a click on a block's edge then counts the same either side of it), where
        # ours is within 40 dB of its top: there the recording's noise does not matter
        k = 10
        ee = bf.to_db([math.sqrt(math.fsum(v * v for v in el[i:i + k]) / k) for i in range(0, n - k + 1, k)])
        me = bf.to_db([math.sqrt(math.fsum(v * v for v in ml[i:i + k]) / k) for i in range(0, n - k + 1, k)])
        top = max(ee)
        diffs = [me[i] - gain - ee[i] for i in range(len(ee)) if ee[i] > top - 40.0]
        offset = sorted(diffs)[len(diffs) // 2] if diffs else None
        rms = math.sqrt(math.fsum((d - offset) ** 2 for d in diffs) / len(diffs)) if diffs else None
        bands = []
        for hz, eb, mb in zip(e["bands_hz"], e["bands_dbfs"], m["bands_dbfs"]):
            if eb is None or mb is None or eb < max(v for v in e["bands_dbfs"] if v is not None) - 50.0:
                continue
            bands.append([hz, bf.r(mb - gain - eb, 2)])
        out[o] = {"envelope_correlation": bf.r(shape, 4),
                  "level_deviation_db": bf.r(m["rms_dbfs"] - gain - e["rms_dbfs"], 2), "envelope_offset_db": bf.r(offset, 2),
                  "envelope_rms_difference_db": bf.r(rms, 2), "bands_deviation_db": bands,
                  "bands_max_deviation_db": bf.r(max((b[1] for b in bands), key=abs), 2) if bands else None,
                  "our_loop_to_loop_correlation": e.get("loop_to_loop_envelope_correlation")}
    return {"outputs": out, "alignment": rec.get("alignment"), "loops": rec.get("loops"), "loop_s": rec.get("loop_s")}


def compare_input(exp, rec):
    res = dict(rec)
    steps = rec.get("steps") or []
    if steps and "gain_db" in steps[0]:
        base = sorted(s["gain_db"] for s in steps[:3])[1] if len(steps) >= 3 else steps[0]["gain_db"]
        clip = next((s for s in steps if s["gain_db"] - base < -0.5 or (s["thd_db"] is not None and s["thd_db"] > -40.0)), None)
        res["small_signal_gain_db"] = bf.r(base, 2)
        res["first_compressed_step"] = clip
    if exp.get("status") != "ok":
        res["expected"] = "pending: the emulated G2 has no audio inputs yet"
    return res


# ---- the report ----------------------------------------------------------------------------------------------

def analyse(folder, form_path=None, expected_dir=None):
    expected_dir = expected_dir or os.path.join(PACK, "expected")
    with open(os.path.join(PACK, "manifest.json")) as f:
        entries = json.load(f)["patches"]
    expected = {}
    for e in entries:
        path = os.path.join(expected_dir, e["name"] + ".json")
        if os.path.exists(path):
            with open(path) as f:
                expected[e["id"]] = json.load(f)
    if form_path is None:
        for name in ("form.txt", "form.fr.txt"):
            if os.path.exists(os.path.join(folder, name)):
                form_path = os.path.join(folder, name)
                break
    form = read_form(form_path) if form_path and os.path.exists(form_path) else {}

    results = {"folder": os.path.abspath(folder), "form": form, "files": {}, "patches": {}, "notes": []}
    ctx = {"clock": 1.0, "gain": {}, "full_level_dbfs": {}, "order": None}
    audio = {}
    for e in entries:
        if not e["record_seconds"]:
            continue
        files = find_recordings(folder, e)
        if not files:
            results["files"][e["id"]] = None
            continue
        rate, channels, notes = load_recording(files)
        results["notes"].extend(notes)
        peak = max(ba.peak(c) for c in channels) if channels else 0.0
        results["files"][e["id"]] = {"files": [os.path.basename(p) for p in files], "rate": rate, "channels": len(channels),
                                     "seconds": bf.r(len(channels[0]) / float(rate), 2) if channels else 0, "peak_dbfs": bf.r(ba.db(peak), 2)}
        if peak >= 0.999:
            results["notes"].append("%s reaches full scale: the recording itself may be clipped" % e["name"])
        if rate < 48000:
            results["notes"].append("%s is recorded at %d Hz: 48 kHz or more was asked" % (e["name"], rate))
        audio[e["id"]] = (rate, channels)
    by_id = {e["id"]: e for e in entries}

    def facts(pid):
        rate, channels = audio[pid]
        if ctx["order"]:  # recorded channels put in output order, from B03
            channels = [channels[c] for c in ctx["order"] if c < len(channels)] or channels
        return rate, channels, bf.measure(by_id[pid], rate, channels, ctx["clock"], expected[pid].get("facts"))

    # B03 first: which channel is which output (applies to every file, if the cabling is a clean permutation)
    if "B03" in audio and expected.get("B03", {}).get("status") == "ok":
        rate, channels = audio["B03"]
        res = compare_map(ctx, expected["B03"]["facts"], rate, channels)
        results["patches"]["B03"] = res
        outs = [c["output"] for c in res["channels"]]
        if all(o is not None for o in outs) and len(set(outs)) == len(outs):
            order = sorted(range(len(outs)), key=lambda c: outs[c])
            if order != list(range(len(outs))) and sorted(outs) == list(range(1, len(outs) + 1)):
                ctx["order"] = order
                results["notes"].append("the recorded channels are not in output order (channel -> output: %s): put back in order for every file"
                                        % ", ".join("%d -> Out %d" % (c + 1, o) for c, o in enumerate(outs)))
    # B01: the gain of the whole chain (G2 output stage, cable, interface) and the clock
    if "B01" in audio and expected.get("B01", {}).get("status") == "ok":
        rate, channels, rec = facts("B01")
        res = compare_tone(expected["B01"]["facts"], rec)
        results["patches"]["B01"] = res
        for o, v in res["outputs"].items():
            ctx["gain"][o] = v["gain_db"]
            ctx["full_level_dbfs"][o] = v["tone_dbfs"] - 3.01  # rms of the full-level sine
        if "1" in res["outputs"]:
            ctx["clock"] = 1.0 + res["outputs"]["1"]["ppm"] * 1e-6
    elif "B03" in results["patches"]:
        for c in results["patches"]["B03"]["channels"]:
            if c["output"] is not None:
                ctx["gain"][str(c["output"])] = c["gain_db"]
        results["notes"].append("no B01 recording: the gain is taken from B03, the clock is assumed exact")
    results["clock_ppm"] = bf.r((ctx["clock"] - 1.0) * 1e6, 1)
    results["gain_db"] = {o: bf.r(v, 2) for o, v in ctx["gain"].items()}

    for pid in audio:
        if pid in ("B01", "B03") or pid not in expected:
            continue
        exp = expected[pid]
        entry = by_id[pid]
        kind = entry["measure"]
        try:
            if kind == "input":
                rate, channels = audio[pid]
                results["patches"][pid] = compare_input(exp, bf.input_facts(rate, channels))
                continue
            if exp.get("status") != "ok":
                continue
            rate, channels, rec = facts(pid)
            if kind in ("level_steps", "pad_steps"):
                res = compare_steps(ctx, entry, exp["facts"], rec)
            elif kind == "noise":
                res = compare_noise(ctx, exp["facts"], rec)
            elif kind == "silence":
                res = compare_noise(ctx, exp["facts"], rec, quiet=True)
            elif kind == "saw_steps":
                res = compare_saw(ctx, exp["facts"], rec, rate)
            elif kind == "clicks":
                res = compare_clicks(ctx, exp["facts"], rec)
            elif kind == "phrase":
                res = compare_phrase(ctx, exp["facts"], rec)
            else:
                continue
            results["patches"][pid] = res
        except Exception as err:  # one bad file should not lose the rest of the report
            results["notes"].append("%s could not be analysed: %s: %s" % (entry["name"], type(err).__name__, err))
    results["expected"] = {pid: {"status": e.get("status"), "summary": e.get("summary"), "facts": e.get("facts") if e.get("measure") in ("voices", "controls") else None,
                                 "emulator": e.get("emulator")} for pid, e in expected.items()}
    return results


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return out


def report(results):
    p, form, L = results["patches"], results["form"], []
    exp = results["expected"]
    L.append("# G2 hardware bench: %s" % os.path.basename(results["folder"].rstrip("/")))
    L.append("")
    who = [form.get(k) for k in ("name", "model", "expansion", "os_version") if form.get(k)]
    L.append("Machine: " + ("; ".join(who) if who else "no form found") + ".")
    if form.get("interface"):
        L.append("Recorded with: %s; input: %s; gain: %s; format: %s." % (form.get("interface"), form.get("input_reference", "?"),
                                                                           form.get("input_gain", "?"), form.get("recording_format", "?")))
    L.append("")
    L.append("Levels are dBFS. \"Ours\" is the emulated G2's DAC (full scale 0 dBFS); G2fresh plays that 18.06 dB louder. "
             "\"Deviation\" is what is left after removing the gain measured with B01 (the G2's output stage, the cable and the interface).")
    L.append("")

    # ---- headline
    L.append("## Headline")
    L.append("")
    head = []
    b01 = p.get("B01", {}).get("outputs", {})
    if b01:
        one = b01.get("1") or next(iter(b01.values()))
        head.append("**Output level.** A full-level sine (64 units, B01) reads **%s dBFS** on Out 1 of this recording; ours is %s dBFS at the DAC: "
                    "chain gain **%s**." % (fmt(one["tone_dbfs"]), fmt(one["expected_dbfs"]), fmt(one["gain_db"], 2, " dB")))
        fs = number_in(form.get("full_scale_dbu"))
        if fs is not None:
            dbu = one["tone_dbfs"] + fs  # a full-scale sine is `fs` dBu rms
            head.append("With the interface's stated %.1f dBu at 0 dBFS: a full-level sine leaves the G2 at **%.2f dBu** (%.2f dBV, %.3f V rms); "
                        "a sine at the G2's DAC full scale would be %.2f dBu. A full-scale sine in G2fresh (0 dBFS, a full-level signal) therefore "
                        "stands for %.2f dBu on a G2." % (fs, dbu, dbu - 2.218, 0.7746 * 10 ** (dbu / 20.0), dbu - one["expected_dbfs"], dbu))
        else:
            head.append("No interface calibration in the form (Q08): the level is relative to this recording only.")
        if len(b01) > 1:
            head.append("Between outputs (B01): " + ", ".join("Out %s %s" % (o, fmt(v["gain_db"] - one["gain_db"], 2, " dB")) for o, v in sorted(b01.items()) if v is not one)
                        + " relative to Out 1.")
        head.append("**Sample clock.** The 1 kHz tone is %.4f Hz (ours %.4f): **%s** (the G2's clock and the interface's together)."
                    % (one["hz"], one["hz"] / (1 + one["ppm"] * 1e-6), fmt(one["ppm"], 1, " ppm")))
    b03 = p.get("B03")
    if b03:
        head.append("**Output map.** " + "; ".join("channel %d = %s (%s Hz, crosstalk %s)" % (
            c["channel"], "Out %d" % c["output"] if c["output"] else "nothing", fmt(c["hz"], 1), fmt(c["crosstalk_db"], 0, " dB")) for c in b03["channels"]) + ".")
        ordered = sorted((c for c in b03["channels"] if c["output"]), key=lambda c: c["output"])
        if len(ordered) > 1:
            base = ordered[0]["gain_db"]
            head.append("Level per output (B03): " + ", ".join("Out %d %s" % (c["output"], fmt(c["gain_db"] - base, 2, " dB")) for c in ordered[1:])
                        + " relative to Out %d." % ordered[0]["output"])
    b02 = p.get("B02", {}).get("outputs", {})
    for o, v in sorted(b02.items()):
        if v.get("step_gain_db") is None:
            continue
        pad = "Pad 0 dB" if o == "1" else "Pad +6 dB"
        if v["first_clipped"] is None:
            head.append("**Clip point, Out %s (%s).** No step is compressed or distorted in the recording, where ours clips from the %s step." % (o, pad, v["our_first_clipped"]))
        elif v["first_clipped"] == v["our_first_clipped"]:
            head.append("**Clip point, Out %s (%s).** Clean up to the %s step (%s dBFS at our DAC, %s dBFS recorded), clipped from the %s step: the same step as ours. "
                        "The analogue output passes everything the DAC gives." % (o, pad, v["last_clean"], fmt(v["last_clean_dac_dbfs"]), fmt(v["last_clean_recorded_dbfs"]), v["first_clipped"]))
        else:
            head.append("**Clip point, Out %s (%s).** The hardware clips from the **%s** step, ours from the %s step: last clean step %s (%s dBFS at our DAC, %s dBFS recorded)."
                        % (o, pad, v["first_clipped"], v["our_first_clipped"], v["last_clean"], fmt(v["last_clean_dac_dbfs"]), fmt(v["last_clean_recorded_dbfs"])))
    b04 = p.get("B04", {}).get("outputs", {})
    if b04.get("1", {}).get("pad_step_db") is not None:
        head.append("**Pad.** +6 dB position: **%s** (ours %s)." % (fmt(b04["1"]["pad_step_db"], 2, " dB"), fmt(b04["1"]["expected_pad_step_db"], 2, " dB")))
    b08 = p.get("B08", {})
    o1, o2 = b08.get("outputs", {}).get("1", {}), b08.get("outputs", {}).get("2", {})
    if o1.get("bpm"):
        head.append("**Master clock.** %.4f BPM (ours %.4f: %s), jitter %s ms rms. Clicks are %s (ours positive): the output is %s relative to G2fresh's."
                    % (o1["bpm"], o1["expected_bpm"], fmt(o1["ppm"], 1, " ppm"), fmt(o1["jitter_ms"], 3),
                       "positive" if o1["polarity"] > 0 else "negative", "in phase" if o1["polarity"] == o1["expected_polarity"] else "**inverted**"))
    if o2.get("bpm"):
        head.append("**The patch's own 120 BPM clock.** %.4f BPM (ours %.4f: %s; it is counted in samples, so this is the sample clock again)."
                    % (o2["bpm"], o2["expected_bpm"], fmt(o2["ppm"], 1, " ppm")))
    if b08.get("master_vs_own_ppm") is not None:
        head.append("Master clock against the patch's own clock: %s compared with ours (0 = the master clock is tied to the sample clock as in the emulator)."
                    % fmt(b08["master_vs_own_ppm"], 1, " ppm"))
    b07 = p.get("B07", {}).get("outputs", {})
    if b07:
        head.append("**Noise floor.** " + ", ".join("Out %s %s dBFS rms (%s dB below a full-level sine)" % (o, fmt(v["rms_dbfs"], 1), fmt(-v["below_full_level_db"], 1) if v.get("below_full_level_db") is not None else "n/a")
                                                    for o, v in sorted(b07.items())) + ".")
    b10 = p.get("B10")
    if b10 and "error" not in b10:
        s = "**Audio input (B10).** In 1 to Out 1: "
        if b10.get("loopback"):
            s += "gain **%s**, delay **%s ms** (spread %s ms), %s." % (fmt(b10.get("small_signal_gain_db", b10.get("gain_db")), 2, " dB"), fmt(b10.get("delay_ms"), 3),
                                                                   fmt(b10.get("delay_spread_ms"), 3), "in phase" if b10.get("polarity", 1) > 0 else "**inverted**")
            clip = b10.get("first_compressed_step")
            s += (" The gain drops from the step sent at %s dBFS (%s dBFS on the loopback)." % (fmt(clip["stimulus_dbfs"], 0), fmt(clip.get("loopback_dbfs"), 1))
                  if clip else " No step is compressed.")
        else:
            s += "no loopback on channel 2: level through the G2 %s dBFS for the steady tone; no gain or delay." % fmt(b10.get("steady_through_dbfs"))
        head.append(s + " Our prediction is pending (the emulated G2 has no inputs yet).")
    elif b10:
        head.append("**Audio input (B10).** " + b10["error"] + ".")
    L += ["- " + h for h in head] or ["Nothing to report: no recording was found."]
    L.append("")

    # ---- observations
    L.append("## What the owner saw")
    L.append("")
    rows = []
    for pid, key in (("B11", "voices_b11"), ("B12", "voices_b12"), ("B13", "voices_b13")):
        e = (exp.get(pid) or {}).get("facts") or {}
        rows.append([pid, form.get(key, "(no answer)"), "%s of %s (display \"%s\")" % (e.get("voices"), e.get("requested_voices"), (e.get("display_knobs_1_2") or "").strip())])
    L += table(["Patch", "Voices on the G2", "Emulated G2 (4 DSPs)"], rows)
    L.append("")
    for key, label, ours in (("clock_at_power_on", "Master clock at power-on", "120 BPM, Run"),
                             ("pitch_stick", "Pitch stick to the right (B14)", "emulator: the top of the stick's range raises the note"),
                             ("g_wheel_1", "G.Wheel 1 away from the player (B14)", "emulator: the top of the wheel's range raises the note"),
                             ("g_wheel_2", "G.Wheel 2 away from the player (B14)", "emulator: the top of the wheel's range brings the second note in"),
                             ("store_protected", "Store with Memory Protect on", "emulator: \"Memory is protected!\" for about two seconds, nothing stored"),
                             ("master_level", "Master Level fully up", None), ("outputs_recorded", "Outputs recorded", None), ("comments", "Comments", None)):
        if form.get(key) or ours:
            L.append("- %s: %s%s" % (label, form.get(key, "(no answer)"), " (%s)" % ours if ours else ""))
    L.append("")

    # ---- details
    L.append("## Details")
    L.append("")
    rows = []
    for pid, f in sorted(results["files"].items()):
        rows.append([pid, "missing", "", "", "", ""] if f is None else [pid, ", ".join(f["files"]), f["rate"], f["channels"], f["seconds"], fmt(f["peak_dbfs"], 1)])
    L += table(["Patch", "File", "Rate", "Channels", "Seconds", "Peak dBFS"], rows)
    L.append("")
    for o, v in sorted(b02.items()):
        L.append("B02 level steps, Out %s (median gain of the clean steps %s):" % (o, fmt(v.get("step_gain_db"), 2, " dB")))
        L.append("")
        L += table(["Step", "Ours dBFS", "Recorded dBFS", "Deviation dB", "THD dB", "Our THD dB"],
                   [[s["label"], fmt(s["expected_dbfs"]), fmt(s["recorded_dbfs"]), fmt(diff(s["gain_db"], v.get("step_gain_db")), 2), fmt(s["thd_db"], 1), fmt(s["expected_thd_db"], 1)]
                    for s in v["steps"]])
        L.append("")
    b05 = p.get("B05", {}).get("outputs", {})
    for o, v in sorted(b05.items()):
        L.append("B05 white noise, Out %s: %s dBFS rms; level from 100 Hz to 16 kHz: deviation %s. Response relative to the 1 kHz band, hardware minus ours:"
                 % (o, fmt(v["rms_dbfs"]), fmt(v.get("deviation_db"), 2, " dB")))
        L.append("")
        L += table(["Hz"] + [("%g" % b[0]) for b in v.get("response_db", [])], [["dB"] + [fmt(b[1], 1) for b in v.get("response_db", [])]])
        L.append("")
        break
    b06 = p.get("B06", {}).get("outputs", {})
    for o, v in sorted(b06.items()):
        L.append("B06 sawtooth, Out %s (partials relative to each note's fundamental, hardware minus ours):" % o)
        L.append("")
        rows = []
        for s in v["steps"]:
            hh = s["highest_harmonic"]
            al = [a for a in s["our_aliases"]]
            rows.append([s["label"], fmt(s["hz"], 2), fmt(s["ppm"], 0), fmt(s["level_deviation_db"], 2), "%d (%d not found)" % (s["harmonics_compared"], s["harmonics_missing"]),
                         fmt(s["harmonic_deviation_max_db"], 1), "%s Hz: %s" % (fmt(hh[0], 0), fmt(hh[3], 1)) if hh else "n/a",
                         "; ".join("%s Hz ours %s, theirs %s" % (fmt(a[0], 0), fmt(a[1], 0), fmt(a[2], 0) if a[2] is not None else "not found") for a in al[:4]) or "none above -70 dB",
                         "; ".join("%s Hz %s" % (fmt(x[0], 0), fmt(x[1], 0)) for x in s["extra_partials"][:4]) or "none"])
        L += table(["Note", "Hz", "ppm", "Level dev. dB", "Harmonics", "Largest dev. dB", "Highest harmonic", "Our aliases (dB)", "Partials only in the recording (dB)"], rows)
        L.append("")
        break
    rows = []
    for pid in ("B09a", "B09b", "B09c", "B09d"):
        for o, v in sorted(p.get(pid, {}).get("outputs", {}).items()):
            rows.append([pid, o, p[pid].get("loops"), fmt(p[pid].get("alignment"), 3), fmt(v["envelope_correlation"], 3), fmt(v["level_deviation_db"], 2),
                         fmt(v["envelope_rms_difference_db"], 2), fmt(v["bands_max_deviation_db"], 1), fmt(v.get("our_loop_to_loop_correlation"), 4)])
    if rows:
        L.append("B09 phrases (aligned on the loop envelope; level differences over 50 ms stretches; third-octave bands within 50 dB of the loudest):")
        L.append("")
        L += table(["Patch", "Out", "Loops", "Alignment", "Envelope correlation", "Level dev. dB", "50 ms levels, rms diff. dB", "Largest band dev. dB", "Ours, loop to loop"], rows)
        L.append("")
    if b07:
        for o, v in sorted(b07.items()):
            if v.get("hum_dbfs"):
                L.append("B07 silence, Out %s: mains components (dBFS rms) " % o + ", ".join("%s Hz %s" % (hz, fmt(d, 0)) for hz, d in v["hum_dbfs"].items()) + ".")
        L.append("")
    if b10 and b10.get("steps"):
        L.append("B10 input steps (1 kHz):")
        L.append("")
        L += table(["Sent dBFS", "Loopback dBFS", "Through the G2 dBFS", "Gain dB", "THD dB"],
                   [[fmt(s["stimulus_dbfs"], 0), fmt(s.get("loopback_dbfs")), fmt(s["through_dbfs"]), fmt(s.get("gain_db")), fmt(s["thd_db"], 1)] for s in b10["steps"]])
        L.append("")
    if results["notes"]:
        L.append("## Notes")
        L.append("")
        L += ["- " + n for n in results["notes"]]
        L.append("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recordings", help="the folder with the returned WAV files and the form")
    ap.add_argument("--form", help="the filled form (default: form.txt or form.fr.txt in the folder)")
    ap.add_argument("--expected", help="the expected JSON files (default: bench/hardware-pack/expected)")
    ap.add_argument("--out", help="the report (default: report.md in the folder)")
    ap.add_argument("--json", help="also write the numbers as JSON")
    args = ap.parse_args()
    results = analyse(args.recordings, args.form, args.expected)
    text = report(results)
    with open(args.out or os.path.join(args.recordings, "report.md"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    if args.json:
        with open(args.json, "w") as f:
            f.write(bf.dumps(results) + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
