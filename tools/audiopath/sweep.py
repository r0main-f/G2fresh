#!/usr/bin/env python3
"""The Runner's buffer behind a real-time callback, over block sizes, patches, DSP threads and reserves.

  sweep.py --fw PATH [--tool build/tools/audiopath/g2audiopath] [--blocks 64,128,...] [--buffers 20] [--threads 2]
           [--patches light,heavy,slots4] [--load N] [--seconds 60] [--out runs.jsonl] [-- EXTRA g2audiopath ARGS]
  sweep.py --table runs.jsonl [more.jsonl ...]

Each run is `g2audiopath runner` (one line of JSON, appended to --out and summarised on the terminal). --blocks are
96 kHz frames per callback (a host block of N samples at R Hz is N * 96000 / R frames). --load N keeps N busy
processes (`yes`) beside it. On macOS the tool runs under `taskpolicy -a` (an application's scheduling policies,
as in a DAW; --no-app: not). The patches are the bench pack's: light = B01 (one sine), heavy = B13 (3 voices of 8
filter chains and a reverb, a chord held), slots4 = B12 in all four slots, a chord on each.

--table prints the runs of result files as a Markdown table.
"""
import argparse
import json
import os
import platform
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PACK = os.path.join(ROOT, "bench", "hardware-pack", "patches")
PATCHES = {
    "light": (os.path.join(PACK, "B01 Level Ref.pch2"), 1, ""),
    "heavy": (os.path.join(PACK, "B13 Voices Heavy.pch2"), 1, "57,60,64"),
    "slots4": (os.path.join(PACK, "B12 Voices Mid.pch2"), 4, "57,60,64"),
}


def table(files):
    rows = []
    for f in files:
        for line in open(f):
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    print("| Patch | DSP threads | Load | Block (frames, ms) | Reserve ms | Missing frames (callbacks short) | Reserve taken ms: p50 / p99 / p99.9 / max | Chunk speed x: p0.1 / p1 / p50 | Refill speed x: min / p1 | Wake-up ms: p99 / max | Callback late ms: p99.9 / max | Longest chunk ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        t, c, f, w, l = r["reserve_taken_ms"], r["chunk_speed"], r["refill_speed"], r["wake_ms"], r["callback_late_ms"]
        print("| %s | %d | %s | %d, %.2f | %g | %d (%d of %d) | %.2f / %.2f / %.2f / %.2f | %.2f / %.2f / %.2f | %.2f / %.2f | %.3f / %.3f | %.2f / %.2f | %.1f |" % (
            r["patch"], r["threads"], r.get("load", 0), r["block"], r["block_ms"], r["buffer_ms"], r["frames_missing"],
            r["short_reads"], r["callbacks"], max(0.0, t["p50"]), max(0.0, t["p99"]), max(0.0, t["p99.9"]), t["max"],
            c["p0.1"], c["p1"], c["p50"], f["min"], f["p1"], w["p99"], w["max"], l["p99.9"], l["max"], r["longest_chunk_ms"]))


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        at = argv.index("--")
        argv, extra = argv[:at], argv[at + 1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", nargs="+")
    ap.add_argument("--fw", default=os.environ.get("G2_FIRMWARE", ""))
    ap.add_argument("--tool", default=os.path.join(ROOT, "build", "tools", "audiopath", "g2audiopath"))
    ap.add_argument("--blocks", default="64,128,256,512,1024,2048")
    ap.add_argument("--buffers", default="20")
    ap.add_argument("--threads", default="2")
    ap.add_argument("--patches", default="light,heavy,slots4")
    ap.add_argument("--load", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--out", default="")
    ap.add_argument("--no-app", dest="app", action="store_false")
    a = ap.parse_args(argv)
    if a.table:
        table(a.table)
        return 0
    if not a.fw:
        ap.error("no firmware: --fw PATH or G2_FIRMWARE")
    busy = [subprocess.Popen(["yes"], stdout=subprocess.DEVNULL) for _ in range(a.load)]
    try:
        for patch in a.patches.split(","):
            path, slots, chord = PATCHES[patch]
            for threads in a.threads.split(","):
                for block in a.blocks.split(","):
                    for buffer in a.buffers.split(","):
                        cmd = [a.tool, "runner", "--fw", a.fw, "--patch", path, "--slots", str(slots), "--threads", threads,
                               "--block", block, "--buffer-ms", buffer, "--seconds", str(a.seconds), "--name", patch] + extra
                        if chord:
                            cmd += ["--chord", chord]
                        if a.app and platform.system() == "Darwin":
                            cmd = ["taskpolicy", "-a"] + cmd
                        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                        try:
                            r = json.loads(p.stdout)
                        except ValueError:
                            print("failed: %s\n%s" % (" ".join(cmd), p.stdout), file=sys.stderr)
                            continue
                        r["patch"] = patch
                        r["load"] = a.load
                        r["loadavg"] = os.getloadavg()[0] if hasattr(os, "getloadavg") else 0
                        t = r["reserve_taken_ms"]
                        print("%-7s threads %s block %5s (%6.2f ms) reserve %5s ms: missing %6d in %3d callbacks; reserve taken p99.9 %6.2f max %6.2f ms; "
                              "chunk speed p1 %.2f p50 %.2f; wake max %.3f ms; late max %.2f ms; load avg %.1f" % (
                                  patch, threads, block, r["block_ms"], buffer, r["frames_missing"], r["short_reads"], t["p99.9"], t["max"],
                                  r["chunk_speed"]["p1"], r["chunk_speed"]["p50"], r["wake_ms"]["max"], r["callback_late_ms"]["max"], r["loadavg"]),
                              flush=True)
                        if a.out:
                            with open(a.out, "a") as f:
                                f.write(json.dumps(r) + "\n")
    finally:
        for b in busy:
            b.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
