#!/usr/bin/env python3
"""Test cases for the module graph port: value tuples per Graph Func id.

The ranges are those of the parameters / modes listed in the panel's
Dependencies (data/modules.json). Graph ids used by panels of modules that the
editor never instantiates (unmapped panels) get plausible ranges.

  cases.py [IDS] [--random N]   prints "ID V0 V1 ..." lines
"""
import argparse

# id -> list of variants; a variant is the list of inclusive maxima (min 0)
# of the dependencies, in Dependencies order.
R = 127
RANGES = {
    0: [[R, R, 3]],                                   # LfoD (unmapped), draws nothing
    1: [[R, R, 3, 1, 3]],                             # EnvADR
    3: [[R, R, R, R, 3, 5]],                          # EnvADSR
    4: [[R, R, R, R, 5]],                             # ModADSR
    5: [[R, R, 3, 1, 3]],                             # AR-Env (unmapped)
    6: [[R, 3]],                                      # EnvD
    7: [[R, 3]],                                      # EnvH
    8: [[16] * 16],                                   # Vocoder bands
    9: [[R, R, R]],                                   # ShelvEQ (unmapped)
    10: [[R, R, R]],                                  # EqPeak
    11: [[R, R, R]],                                  # LevScaler
    12: [[R]],                                        # Mux8-1X
    13: [[R, R, 5, 2]],                               # FltPhase
    14: [[1, R]],                                     # Clip
    15: [[1, R]],                                     # Overdrive
    16: [[R]],                                        # WaveWrap
    17: [[R] * 8 + [3, 4, 3]],                        # EnvMulti
    18: [[R, 7]],                                     # OscShpB
    19: [[R, 7, R, R, 3]],                            # PulseOsc (unmapped): shape, wave
    20: [[R, R, 2, 1]],                               # FltClassic
    21: [[R, R, 3, 1, 1, 1]],                         # FltNord
    23: [[R] * 6 + [3, 1, 5]],                        # EnvADDSR
    24: [[R, R, 5, 9]],                               # CPnlLfoGraph (no panel): shape, phase, out type, wave (10 = random, not tested)
    28: [[R, R, R, 3, 3], [R, R, R, 3]],              # EnvAHD, ModAHD
    29: [[99] * 8],                                   # Operator, EnvDX
    30: [[R, 1, 5]],                                  # FltLP
    31: [[R, 1, 5]],                                  # FltHP
    32: [[R, 5]],                                     # OscShpA
    33: [[3, R, 5]],                                  # LfoB
    34: [[5, R, R, 5]],                               # LfoShpA
    35: [[R, 2]],                                     # FltComb
    36: [[R, R]],                                     # Eq2Band
    37: [[R, R, R, R]],                               # Eq3band
    38: [[R, 3]],                                     # ShpExp
    39: [[R, 3]],                                     # Saturate
    40: [[R, R, 2, 1, 1]],                            # FltStatic
    41: [[99, 3, 99, 3, 99]],                         # Operator keyboard scaling
    42: [[31]],                                       # DXRouter
    43: [[R]],                                        # OscNoise
    44: [[R]],                                        # CPnlRndDistributionGraph (no panel)
    45: [[R]],                                        # RndTrig
}


class Lcg:
    """Numerical Recipes LCG; tests/test_graphs.cpp uses the same sequence."""

    def __init__(self, seed):
        self.state = seed & 0xffffffff

    def below(self, n):
        self.state = (self.state * 1664525 + 1013904223) & 0xffffffff
        return (self.state >> 8) % n


def cases(gid, n_random=40):
    """Value tuples for one id, in a fixed order: per variant all-0, all-max,
    all-mid, then every dependency swept over its range (step 3 above 31)
    with the others at mid and at a random base, then n_random random tuples.
    Duplicates are dropped (first occurrence kept)."""
    out = []
    for vi, var in enumerate(RANGES[gid]):
        rng = Lcg(gid * 7919 + vi * 131 + 1)
        out.append([0] * len(var))
        out.append(list(var))
        mid = [m // 2 for m in var]
        out.append(mid)
        bases = [mid, [rng.below(m + 1) for m in var]]
        for base in bases:
            for i, m in enumerate(var):
                step = 1 if m <= 31 else 3
                for v in range(0, m + 1, step):
                    t = list(base)
                    t[i] = v
                    out.append(t)
        for _ in range(n_random):
            out.append([rng.below(m + 1) for m in var])
    seen = set()
    res = []
    for t in out:
        key = tuple(t)
        if key not in seen:
            seen.add(key)
            res.append(t)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ids', nargs='*', type=int)
    ap.add_argument('--random', type=int, default=40)
    a = ap.parse_args()
    for gid in a.ids or sorted(RANGES):
        for t in cases(gid, a.random):
            print(gid, ' '.join(map(str, t)))


if __name__ == '__main__':
    main()
