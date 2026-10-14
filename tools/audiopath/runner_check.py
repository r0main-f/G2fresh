#!/usr/bin/env python3
"""`g2audiopath runner` measures the machine the plugin runs: four DSPs (a G2X without its voice expansion board,
plugin/Source/EmulatedSoundEngine.cpp), not a G2X as built (eight).

  runner_check.py --tool build/tools/audiopath/g2audiopath

Runs the light patch for a second and reads the DSP count from the runner's JSON. Exit 0 when it is the plugin's,
1 when not, 77 without G2_FIRMWARE (Clavia's OS update .dmg): a skipped test.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PATCH = os.path.join(ROOT, "bench", "hardware-pack", "patches", "B01 Level Ref.pch2")
PLUGIN_DSPS = 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    a = ap.parse_args()
    if not os.environ.get("G2_FIRMWARE"):
        print("no G2_FIRMWARE: skipped")
        return 77
    out = subprocess.run([a.tool, "runner", "--patch", PATCH, "--seconds", "1", "--settle", "1"],
                         capture_output=True, text=True, check=True).stdout
    dsps = json.loads(out).get("dsps")
    if dsps != PLUGIN_DSPS:
        print("g2audiopath runner ran %s DSPs, the plugin runs %d" % (dsps, PLUGIN_DSPS))
        return 1
    print("g2audiopath runner ran %d DSPs, as the plugin" % dsps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
