#!/usr/bin/env python3
"""Writes "B10 Stimulus.wav", the test signal a friend plays into the G2's In 1 for patch B10 (and, at the same time,
straight back into the interface as a time and level reference).

    python3 tools/benchpack/make_stimulus.py [OUT.wav]      default: bench/hardware-pack/B10 Stimulus.wav
    python3 tools/benchpack/make_stimulus.py --check        exit 1 if the committed file is not what this writes

Mono, 48 kHz, 16 bit, 14 seconds, all at 1 kHz (benchfacts.STIMULUS): a second of silence; three 20 ms bursts at
-6 dBFS half a second apart (the delay through the G2); three seconds at -20 dBFS (the gain); then six one-second
steps from -40 to -1 dBFS (where the G2's input starts to clip). No dither: the same file every time.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchaudio as ba  # noqa: E402
import benchfacts as bf  # noqa: E402


def stimulus():
    s = bf.STIMULUS
    rate, hz = s["rate"], s["tone_hz"]
    x = [0.0] * int(s["total"] * rate)

    def tone(start_s, seconds, dbfs, fade_s):
        a, n, amp = int(round(start_s * rate)), int(round(seconds * rate)), 10 ** (dbfs / 20.0)
        fade = int(fade_s * rate)
        for i in range(n):
            g = 1.0
            if fade and i < fade:
                g = 0.5 - 0.5 * math.cos(math.pi * i / fade)
            elif fade and i >= n - fade:
                g = 0.5 - 0.5 * math.cos(math.pi * (n - i) / fade)
            x[a + i] = amp * g * math.sin(2.0 * math.pi * hz * i / rate)

    for t in s["bursts"]["times"]:
        tone(t, s["bursts"]["seconds"], s["bursts"]["dbfs"], 0.0)  # no fade: a sharp start, from zero
    tone(s["steady"]["start"], s["steady"]["seconds"], s["steady"]["dbfs"], 0.01)
    for i, dbfs in enumerate(s["steps"]["dbfs"]):
        tone(s["steps"]["start"] + i * (s["steps"]["seconds"] + s["steps"]["gap"]), s["steps"]["seconds"], dbfs, 0.01)
    return x


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    committed = os.path.join(root, "bench", "hardware-pack", "B10 Stimulus.wav")
    args = sys.argv[1:]
    if args == ["--check"]:
        rate, channels = ba.read_wav(committed)
        want = stimulus()
        same = rate == bf.STIMULUS["rate"] and len(channels) == 1 and len(channels[0]) == len(want) and all(
            abs(a - b) <= 1.0 / 32768.0 for a, b in zip(channels[0], want))
        if not same:
            print("%s is not what make_stimulus.py writes: run it again" % committed)
        return 0 if same else 1
    out = args[0] if args else committed
    ba.write_wav(out, bf.STIMULUS["rate"], [stimulus()], 16)
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
