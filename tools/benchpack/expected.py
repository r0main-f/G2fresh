#!/usr/bin/env python3
"""Our predictions for the hardware bench pack: plays every patch on the emulated G2 and writes what it measures.

    python3 tools/benchpack/expected.py [--build build] [--fw PATH] [--only B01,B09a] [--jobs 4] [--check]

Needs a build with -DG2_BUILD_EMU=ON (g2benchrender) and Clavia's OS (--fw or $G2_FIRMWARE: the OS update .dmg).
Writes bench/hardware-pack/expected/<patch name>.json (committed: our measurements of the emulator's output) and the
renders themselves into expected/renders/ (96 kHz float WAV, Out 1-4; not committed: they are the output of
Clavia's OS). Two runs give the same files: the emulator runs on one thread.

--check renders into a temporary folder and compares with the committed JSON instead of writing it (exit 1 if they
differ, 77 if there is no firmware or no emulator build: a skipped test).
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchaudio as ba  # noqa: E402
import benchfacts as bf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACK = os.path.join(ROOT, "bench", "hardware-pack")
DAC_TO_G2FRESH_DB = 20.0 * __import__("math").log10(8.0)  # G2fresh plays the DAC words times 8

LEVELS = ("Levels are in dBFS of the G2's DAC words (1.0 = DAC full scale), Master Level full. G2fresh plays the words "
          "8 times louder (+18.06 dB), so that a full-level signal inside the G2 (64 units) is 0 dBFS in the host.")

# B10: where the stimulus file's full scale is put, in dB relative to the ADCs' full scale. On a G2 that is the
# interface's output level against the G2's input sensitivity, which nobody knows beforehand (NOTES.md): at 0 the
# loudest step (-1 dBFS) is just under the ADC's full scale, at +10 the last two steps are over it.
INPUT_DB = 0.0
INPUT_HOT_DB = 10.0
INPUT_LEVELS = ("A level at the ADC (loopback_dbfs) is in dB relative to the ADC's full scale, a word of +-2^23: a signal of 4.0 inside "
                "the G2, 12 dB above an oscillator's. A level at Out 1 (through_dbfs) is in dBFS of the DAC's words, Master Level "
                "full. A gain is the second minus the first. The stimulus file's full scale is at the ADC's full scale in "
                "'facts' and %g dB above it in 'facts.hot'." % INPUT_HOT_DB)
INPUT_NOTE = ("The delay is the emulated G2's own way from an ADC word to the DAC word it becomes; converters' filters are not in "
              "it. Polarity 1: Out 1 follows In 1 in the sign G2fresh plays the DAC's words with (the DAC's word itself is "
              "-0.4987 times the ADC's). Nothing between the ADC and the DAC clips before the ADC's full scale; the clip above "
              "it in 'facts.hot' is the render tool's stand-in for the converter (it stops at its largest word), not the OS's.")


def render_seconds(entry):
    kind = entry["measure"]
    if kind in ("level_steps", "pad_steps", "saw_steps", "phrase"):
        return 3.4 * entry["loop_seconds"] * 1.002 + 1.5
    if kind == "input":
        return bf.STIMULUS["total"] + 0.5
    return {"tone": 10.0, "tone_map": 6.0, "noise": 20.0, "silence": 2.0, "clicks": 60.0, "voices": 0.2, "controls": 0.5}.get(kind, 1.0)


def render(tool, fw, entry, out_dir, extra=(), tag=""):
    wav = os.path.join(out_dir, entry["name"] + tag + ".wav")
    info = os.path.join(out_dir, entry["name"] + tag + ".render.json")
    cmd = [tool, "--fw", fw, "--seconds", "%g" % render_seconds(entry), "--wav", wav, "--json", info, *extra,
           os.path.join(PACK, "patches", entry["file"])]
    done = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if done.returncode != 0:
        raise RuntimeError("%s: g2benchrender failed:\n%s" % (entry["name"], done.stderr[-2000:]))
    with open(info) as f:
        info = json.load(f)
    if info["emulator"]["exceptions"] or info["emulator"]["dsps_wild"]:
        raise RuntimeError("%s: the emulated G2 went wrong: %s" % (entry["name"], info["emulator"]))
    return wav, info


def emulator_facts(info):
    keep = ("model", "os_version", "voices", "requested_voices", "master_clock", "display", "display_knobs_1_2", "load_va", "load_fx")
    return {k: info[k] for k in keep if k in info}


def summarise(entry, facts):
    """The few numbers a person wants first."""
    kind, outs, s = entry["measure"], facts.get("outputs", {}), {}
    one = outs.get("1", {})
    if kind == "tone":
        s = {"hz": one["hz"], "peak_dbfs": one["peak_dbfs"], "rms_dbfs": one["rms_dbfs"], "g2fresh_peak_dbfs": round(one["peak_dbfs"] + DAC_TO_G2FRESH_DB, 2)}
    elif kind == "tone_map":
        s = {"out_%s_hz" % o: f["hz"] for o, f in outs.items()}
    elif kind == "level_steps":
        for o, f in outs.items():
            levels = [st.get("tone_dbfs") for st in f["steps"]]
            clipped = [st["label"] for st in f["steps"] if st.get("thd_db") is not None and st["thd_db"] > -40.0]
            s["out_%s_step_tone_dbfs" % o] = levels
            s["out_%s_first_clipped_step" % o] = clipped[0] if clipped else None
    elif kind == "pad_steps":
        st = one["steps"]
        s = {"pad_0_dbfs": st[1]["tone_dbfs"], "pad_6_dbfs": st[4]["tone_dbfs"], "pad_step_db": round(st[4]["tone_dbfs"] - st[1]["tone_dbfs"], 2)}
    elif kind == "noise":
        s = {"rms_dbfs": one["rms_dbfs"], "peak_dbfs": one["peak_dbfs"]}
    elif kind == "saw_steps":
        s = {"step_hz": [st.get("hz") for st in one["steps"]], "step_rms_dbfs": [st.get("rms_dbfs") for st in one["steps"]]}
    elif kind == "clicks":
        s = {"master_clock_bpm": one.get("bpm"), "own_clock_bpm": outs.get("2", {}).get("bpm"), "click_polarity": one.get("polarity")}
    elif kind == "phrase":
        s = {"loop_s": facts.get("loop_s"), "rms_dbfs": one.get("rms_dbfs"), "peak_dbfs": one.get("peak_dbfs"),
             "loop_to_loop_correlation": {o: f.get("loop_to_loop_correlation") for o, f in outs.items()}}
    return s


def differences(a, b, where=""):
    """Where two results differ: numbers by more than 0.11 (a tenth of a dB, a tenth of a hertz), the rest at all."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append("%s/%s: only in one" % (where, k))
            else:
                out += differences(a[k], b[k], "%s/%s" % (where, k))
        return out
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return ["%s: %d and %d items" % (where, len(a), len(b))]
        out = []
        for i, (x, y) in enumerate(zip(a, b)):
            out += differences(x, y, "%s[%d]" % (where, i))
        return out[:5]
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        # near the floor of an envelope the last digit is the emulator's noise, not a fact
        return [] if abs(a - b) <= 0.11 or (a < -90 and b < -90) else ["%s: %s and %s" % (where, a, b)]
    return [] if a == b else ["%s: %r and %r" % (where, a, b)]


def one_patch(tool, fw, entry, renders):
    kind = entry["measure"]
    result = {"id": entry["id"], "name": entry["name"], "file": entry["file"], "measure": kind}
    if kind == "input":
        # B10: the stimulus into In 1, twice (the file's full scale at the ADC's, and INPUT_HOT_DB above it), measured
        # as a recording is: channel 1 the render's Out 1, channel 2 ("the loopback") what came to the ADC
        stimulus = os.path.join(PACK, entry["stimulus"])

        def run(tag, level_db):
            came_in = os.path.join(renders, entry["name"] + tag + ".input.wav")
            wav, info = render(tool, fw, entry, renders, ["--in1", stimulus, "--in-level", "%g" % level_db, "--in-ref", came_in], tag)
            rate, out = ba.read_wav(wav)
            return info, rate, level_db, out[0], ba.read_wav(came_in)[1][0]

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            (info, rate, *plain), (_, _, *hot) = pool.map(lambda a: run(*a), (("", INPUT_DB), (".hot", INPUT_HOT_DB)))
        facts, summary = bf.input_prediction(rate, *plain, *hot)
        result.update(emulator=emulator_facts(info), status="ok", levels=INPUT_LEVELS, sample_rate=rate, stimulus=bf.STIMULUS,
                      facts=facts, summary=summary, note=INPUT_NOTE)
        return result
    if kind == "controls":
        # B14 on the emulated G2X (four DSPs, as the plugin runs it): the tone with each control at rest and at the top
        # of its ADC range
        runs = {"rest": [], "pitch_stick_high": ["--analog", "stick=1"], "pitch_stick_low": ["--analog", "stick=0"],
                "g_wheel_1_high": ["--analog", "wheel1=1"], "g_wheel_2_high": ["--analog", "wheel2=1"]}
        facts = {}
        for name, extra in runs.items():
            wav, info = render(tool, fw, entry, renders, ["--model", "g2x", "--expansion", "off", *extra], "." + name)
            rate, ch = ba.read_wav(wav)
            x = ba.part(ch[0], rate, 0.1, 0.5)
            low = bf.tone_facts(x, rate, entry["tone_hz"])
            if name != "rest":  # the low tone may have moved: take the strongest partial below 1 kHz
                cands = [p for p, _ in ba.peaks(x, rate, -30.0, 100.0, 8192) if p < 1000.0]
                low = bf.tone_facts(x, rate, cands[0]) if cands else low
            facts[name] = {"low_tone_hz": low["hz"], "second_tone_dbfs": bf.r(ba.db(ba.tone(x, rate, entry["g_wheel_2_adds_hz"])[0]), 1)}
        result.update(status="ok", emulator=emulator_facts(info), facts=facts,
                      note="High = the top of the control's range as the OS reads it. Which physical direction that is on a G2 is the question.")
        return result
    wav, info = render(tool, fw, entry, renders)
    result["emulator"] = emulator_facts(info)
    if kind == "voices":
        # and on a G2X as built: eight DSPs, the voice expansion board's four with the main board's
        _, g2x = render(tool, fw, entry, renders, ["--model", "g2x"], ".g2x")
        result.update(status="ok", facts={"requested_voices": info["requested_voices"], "voices": info["voices"][0],
                                          "display_knobs_1_2": info.get("display_knobs_1_2"),
                                          "g2x": {"voices": g2x["voices"][0], "display_knobs_1_2": g2x.get("display_knobs_1_2"),
                                                  "load_va": g2x.get("load_va"), "load_fx": g2x.get("load_fx")}},
                      note="'facts' is a G2 or a G2 Engine without the voice expansion board (four DSPs); 'facts.g2x' a G2X, or "
                           "any G2 with the board (eight DSPs). The plugin's emulated G2X runs without the board: its voices "
                           "are the first figure.")
        return result
    rate, channels = ba.read_wav(wav)
    facts = bf.measure(entry, rate, channels)
    result.update(status="ok", levels=LEVELS, sample_rate=rate, facts=facts, summary=summarise(entry, facts))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"), help="the CMake build directory (default: build)")
    ap.add_argument("--fw", default=os.environ.get("G2_FIRMWARE", ""), help="Clavia's G2 OS update (.dmg); default: $G2_FIRMWARE")
    ap.add_argument("--only", default="", help="comma-separated patch ids (default: all)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--check", action="store_true", help="compare with the committed JSON instead of writing it")
    args = ap.parse_args()
    tool = os.path.join(args.build, "tools", "benchpack", "g2benchrender")
    for missing, what in ((not os.path.exists(tool), "no %s: build with -DG2_BUILD_EMU=ON (target g2benchrender)" % tool),
                          (not args.fw, "no firmware: pass --fw or set G2_FIRMWARE to Clavia's \"Nord Modular G2 OS v1.62 Update.dmg\"")):
        if missing:
            print(what + (" (skipped)" if args.check else ""))
            return 77 if args.check else 1
    with open(os.path.join(PACK, "manifest.json")) as f:
        entries = json.load(f)["patches"]
    only = {s.strip() for s in args.only.split(",") if s.strip()}
    entries = [e for e in entries if not only or e["id"] in only]
    out_dir = os.path.join(PACK, "expected")
    renders = tempfile.mkdtemp(prefix="benchpack-check-") if args.check else os.path.join(out_dir, "renders")
    os.makedirs(renders, exist_ok=True)
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        jobs = {pool.submit(one_patch, tool, args.fw, e, renders): e for e in entries}
        for job in concurrent.futures.as_completed(jobs):
            entry = jobs[job]
            try:
                result = job.result()
            except Exception as err:  # keep going: one patch failing should not lose the others
                print("%s: FAILED: %s" % (entry["name"], err))
                failed += 1
                continue
            load = result.get("emulator", {}).get("load_va", {}) or {}
            if entry["measure"] != "voices" and load.get("cycles_percent", 0) > 60:
                print("%s: FAILED: it takes %.0f %% of a DSP: too heavy for a bench patch" % (entry["name"], load["cycles_percent"]))
                failed += 1
            path = os.path.join(out_dir, entry["name"] + ".json")
            if args.check:
                with open(path) as f:
                    diffs = differences(json.load(f), json.loads(bf.dumps(result)))
                print("%s: %s" % (entry["name"], "as committed" if not diffs else "DIFFERS from %s:\n  %s" % (path, "\n  ".join(diffs[:12]))))
                failed += bool(diffs)
                continue
            with open(path, "w") as f:
                f.write(bf.dumps(result) + "\n")
            print("%s: %s %s" % (entry["name"], result["status"], json.dumps(result.get("summary", result.get("facts", "")))[:300]))
    if args.check:
        shutil.rmtree(renders, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
