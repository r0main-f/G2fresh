#!/usr/bin/env python3
"""The emulated G2's buffer behind a real-time callback: how much reserve it takes on this computer.

  sweep.py --fw PATH [--out DIR] [--seconds 60] [--quick] [--force] [--only PLAN,...] [--patches P,...]
           [--blocks N,...] [--append] [--tool PATH]
  sweep.py --table DIR/runs.jsonl [more.jsonl ...]

One command runs the whole plan (about an hour; --quick: 20 s per run, 20 minutes) and writes DIR/runs.jsonl (one
line of JSON per run, as `g2audiopath runner` prints it, plus the patch, the plan, the load beside it and the
computer's 1, 5 and 15-minute load averages before and after the run) and DIR/table.md (the table and the verdict per buffer setting). DIR defaults to
build/audio-path-sweep; runs of an earlier sweep there are set aside under another name (--append: kept, and the new
runs added). --fw is Clavia's G2 OS updater (the .dmg), or G2_FIRMWARE.

The computer must be quiet: the sweep measures what the emulated G2 needs, and other work beside it changes the
answer. It refuses to start while the 1-minute load average is above 2 (--force: runs anyway), waits before each run
for the load to fall back under 2 (up to two minutes), and marks every run that started above it. --patches and
--blocks keep only those of a plan (one short point, to try the command: --only quiet --patches light --blocks 512
--seconds 5).

Each run is the emulated G2 behind a callback thread with a host's real-time scheduling (tools/audiopath/
g2audiopath.cpp), as the plugin runs it (real-time threads, the stretch), with Safe's reserve, so that what a run
took of its reserve shows what each smaller reserve would have done. Blocks are 96 kHz frames per callback: a host block of N samples at R Hz is N * 96000 / R frames
(64 frames: 32 samples at 48 kHz, 64 at 96 kHz; 512: 256 at 48 kHz; 2048: 1024 at 48 kHz).

The plans:
  quiet     light, heavy and four-slot patches over blocks of 32 to 2048 frames
  threads1  the light patch with one DSP thread (computers with fewer than 4 performance cores)
  busy      beside 6 threads that compute without a pause at an application's priority (`g2audiopath burn 6`)
  audio     beside 4 threads that work as a host's audio workers (`g2audiopath burn 4 rt`)
  two       two emulated G2s at once (two instances in a host)
  low       the reserves of the Low and Normal settings as they are (2 and 6 ms), quiet and busy
  before    as 0.10.2 ran it (ordinary priority, a start for every block), quiet and busy

The patches are the bench pack's: light = B01 (one sine), heavy = B13 (3 voices of 8 filter chains and a reverb, a
chord held), slots4 = B12 in all four slots, a chord on each. On macOS the tool runs under `taskpolicy -a` (an
application's scheduling policies, as in a DAW).
"""
import argparse
import json
import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PACK = os.path.join(ROOT, "bench", "hardware-pack", "patches")
PATCHES = {
    "light": (os.path.join(PACK, "B01 Level Ref.pch2"), 1, ""),
    "heavy": (os.path.join(PACK, "B13 Voices Heavy.pch2"), 1, "57,60,64"),
    "slots4": (os.path.join(PACK, "B12 Voices Mid.pch2"), 4, "57,60,64"),
}
# The buffer settings' reserves in milliseconds (plugin/Source/AudioIo.h).
SETTINGS = (("Low", 2.0), ("Normal", 6.0), ("Safe", 21.0))
SAFE = SETTINGS[-1][1]
QUIET_LOAD = 2.0
ALL_BLOCKS = [32, 64, 128, 256, 512, 1024, 2048]
OLD = ["--rt", "0", "--stretch-ms", "0"]
# plan: (patches, blocks, reserve ms, extra arguments, burners, burners real-time, instances at once)
PLANS = {
    "quiet": (["light", "heavy", "slots4"], ALL_BLOCKS, SAFE, [], 0, False, 1),
    "threads1": (["light"], [64, 512], SAFE, ["--threads", "1"], 0, False, 1),
    "busy": (["light", "heavy"], [64, 256, 512, 2048], SAFE, [], 6, False, 1),
    "audio": (["heavy"], [64, 512], SAFE, [], 4, True, 1),
    "two": (["heavy"], [64, 512], SAFE, [], 0, False, 2),
    "low": (["heavy"], [64, 256, 512], None, [], 0, False, 1),       # None: each of Low's and Normal's reserves
    "low-busy": (["heavy"], [64, 256, 512], None, [], 6, False, 1),
    "before": (["light", "heavy"], [64, 512], SAFE, OLD, 0, False, 1),
    "before-busy": (["light", "heavy"], [64, 512], SAFE, OLD, 6, False, 1),
}


def load_average():
    return os.getloadavg()[0] if hasattr(os, "getloadavg") else 0.0


def load_averages():
    return [round(x, 2) for x in os.getloadavg()] if hasattr(os, "getloadavg") else []


def read(files):
    rows = []
    for f in files:
        for line in open(f):
            if line.strip():
                rows.append(json.loads(line))
    return rows


def table(rows, out=sys.stdout):
    w = out.write
    w("| Plan | Patch | Block (frames, ms) | Reserve ms | Load average | Missing frames (blocks short) | Reserve taken ms: p50 / p99 / p99.9 / max "
      "| Speed of a chunk x: p0.1 / p1 / p50 | Speed of a refill x: min / p1 | Wake-up ms: p99 / max | Callback late ms: p99.9 / max "
      "| Longest chunk ms | CPU cores |\n")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    for r in rows:
        t, c, f, k, l = r["reserve_taken_ms"], r["chunk_speed"], r["refill_speed"], r["wake_ms"], r["callback_late_ms"]
        w("| %s | %s | %d, %.2f | %g | %.1f%s | %d (%d of %d) | %.2f / %.2f / %.2f / %.2f | %.2f / %.2f / %.2f | %.2f / %.2f | %.3f / %.3f "
          "| %.2f / %.2f | %.1f | %.2f |\n" % (
              r.get("plan", "-"), r.get("patch", r.get("name", "")), r["block"], r["block_ms"], r["buffer_ms"], r.get("loadavg", 0),
              " (not quiet)" if r.get("loadavg", 0) > QUIET_LOAD and not r.get("burners") else "", r["frames_missing"], r["short_reads"],
              r["callbacks"], max(0.0, t["p50"]), max(0.0, t["p99"]), max(0.0, t["p99.9"]), max(0.0, t["max"]), c["p0.1"], c["p1"], c["p50"],
              f["min"], f["p1"], k["p99"], k["max"], l["p99.9"], l["max"], r["longest_chunk_ms"], r.get("cpu_cores", 0)))
    # What each setting's reserve would have done: a run whose reserve was at least the setting's shows it directly
    w("\nPer buffer setting: the runs (with a reserve of at least the setting's) that took more of their reserve than the setting has, "
      "so would have dropped out with it.\n\n")
    w("| Setting | Reserve ms | Quiet runs | Would have dropped out | Worst reserve taken, quiet (ms) | Runs beside other work | Would have dropped out | Worst (ms) |\n")
    w("|---|---|---|---|---|---|---|---|\n")
    new = [r for r in rows if not str(r.get("plan", "")).startswith("before")]
    for name, reserve in SETTINGS:
        cells = []
        for busy in (False, True):
            runs = [r for r in new if r["buffer_ms"] >= reserve and bool(r.get("burners") or r.get("instances", 1) > 1) == busy]
            worst = max((r["reserve_taken_ms"]["max"] for r in runs), default=0.0)
            cells += [str(len(runs)), str(sum(1 for r in runs if r["reserve_taken_ms"]["max"] >= reserve)), "%.2f" % max(0.0, worst)]
        w("| %s | %g | %s |\n" % (name, reserve, " | ".join(cells)))
    noisy = [r for r in rows if r.get("loadavg", 0) > QUIET_LOAD and not r.get("burners")]
    if noisy:
        w("\n**%d of %d runs started with a load average above %g: the computer was not quiet, and their figures say little about "
          "what the emulated G2 needs.**\n" % (len(noisy), len(rows), QUIET_LOAD))


def one(tool, fw, patch, block, reserve, extra, seconds, app):
    path, slots, chord = PATCHES[patch]
    cmd = [tool, "runner", "--fw", fw, "--patch", path, "--slots", str(slots), "--block", str(block), "--buffer-ms", str(reserve),
           "--seconds", str(seconds), "--name", patch] + extra
    if chord:
        cmd += ["--chord", chord]
    if app and platform.system() == "Darwin":
        cmd = ["taskpolicy", "-a"] + cmd
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", nargs="+")
    ap.add_argument("--fw", default=os.environ.get("G2_FIRMWARE", ""))
    ap.add_argument("--tool", default=os.path.join(ROOT, "build", "tools", "audiopath", "g2audiopath"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "audio-path-sweep"))
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--quick", action="store_true", help="20 s per run")
    ap.add_argument("--force", action="store_true", help="run although the computer is not quiet")
    ap.add_argument("--only", default="", help="plans to run (default: all): " + ", ".join(PLANS))
    ap.add_argument("--patches", default="", help="only these patches of each plan (light, heavy, slots4)")
    ap.add_argument("--blocks", default="", help="only these blocks (96 kHz frames) of each plan")
    ap.add_argument("--append", action="store_true", help="add to DIR/runs.jsonl instead of starting it anew")
    ap.add_argument("--no-app", dest="app", action="store_false", help="macOS: not under taskpolicy -a")
    a = ap.parse_args()
    if a.table:
        table(read(a.table))
        return 0
    if not a.fw:
        ap.error("no firmware: --fw PATH or G2_FIRMWARE")
    if not os.path.exists(a.tool):
        ap.error("no " + a.tool + " (build it: cmake --build build --target g2audiopath, with -DG2_BUILD_EMU=ON)")
    seconds = 20 if a.quick else a.seconds
    load = load_average()
    if load > QUIET_LOAD and not a.force:
        print("The 1-minute load average is %.1f: this computer is not quiet (other programs are working), and the sweep would measure "
              "them. Close them and run it again, or pass --force to measure anyway (the runs are then marked)." % load, file=sys.stderr)
        return 3
    os.makedirs(a.out, exist_ok=True)
    runs = os.path.join(a.out, "runs.jsonl")
    if os.path.exists(runs) and not a.append:
        os.rename(runs, os.path.join(a.out, "runs-until-%s.jsonl" % time.strftime("%Y%m%d-%H%M%S")))
    plans = [p for p in a.only.split(",") if p] or list(PLANS)
    for plan in plans:
        if plan not in PLANS:
            ap.error("no plan %s (plans: %s)" % (plan, ", ".join(PLANS)))
    keep_patches = [p for p in a.patches.split(",") if p]
    keep_blocks = [int(b) for b in a.blocks.split(",") if b]
    for plan in plans:
        patches, blocks, reserve, extra, burners, realtime, instances = PLANS[plan]
        patches = [p for p in patches if not keep_patches or p in keep_patches]
        blocks = [b for b in blocks if not keep_blocks or b in keep_blocks]
        reserves = [reserve] if reserve is not None else [s[1] for s in SETTINGS[:2]]
        for patch in patches:
            for block in blocks:
                for res in reserves:
                    # the load of the run before must be gone (the average takes a minute to fall)
                    waited = 0
                    while not a.force and load_average() > QUIET_LOAD and waited < 120:
                        time.sleep(5)
                        waited += 5
                    before = load_average()
                    averages = load_averages()
                    busy = [subprocess.Popen([a.tool, "burn", str(burners)] + (["rt"] if realtime else []))] if burners else []
                    try:
                        procs = [one(a.tool, a.fw, patch, block, res, extra, seconds, a.app) for _ in range(instances)]
                        outs = [p.communicate()[0] for p in procs]
                    finally:
                        for b in busy:
                            b.kill()
                    for n, text in enumerate(outs):
                        try:
                            r = json.loads(text)
                        except ValueError:
                            print("failed: %s %s block %d\n%s" % (plan, patch, block, text[:300]), file=sys.stderr)
                            continue
                        r.update(plan=plan, patch=patch, burners=burners, burners_realtime=realtime, instances=instances, loadavg=before,
                                 loadavgs_before=averages, loadavgs_after=load_averages(), forced=a.force,
                                 when=time.strftime("%Y-%m-%d %H:%M:%S"))
                        t = r["reserve_taken_ms"]
                        print("%-11s %-6s block %5d (%6.2f ms) reserve %4g ms: missing %7d in %4d blocks; reserve taken p99.9 %5.2f max %5.2f ms; "
                              "chunk speed p1 %.2f p50 %.2f; load average %.1f%s" % (
                                  plan, patch, block, r["block_ms"], res, r["frames_missing"], r["short_reads"], max(0.0, t["p99.9"]),
                                  max(0.0, t["max"]), r["chunk_speed"]["p1"], r["chunk_speed"]["p50"], before,
                                  "  NOT QUIET" if before > QUIET_LOAD else ""), flush=True)
                        with open(runs, "a") as f:
                            f.write(json.dumps(r) + "\n")
    with open(os.path.join(a.out, "table.md"), "w") as f:
        f.write("Sweep of %s on %s (%s), %g s per run.\n\n" % (time.strftime("%Y-%m-%d"), platform.node(), platform.machine(), seconds))
        table(read([runs]), f)
    print("\n" + open(os.path.join(a.out, "table.md")).read())
    print("Written: %s and %s" % (runs, os.path.join(a.out, "table.md")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
