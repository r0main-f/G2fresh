#!/usr/bin/env python3
"""Export the DSP fragments of the G2 OS for emu/dsppoc (g2dsppoc).

  g2frags.py CODE_30000400.bin OUTDIR

CODE_30000400.bin is the OS's CODE section unpacked by g2os.py from the
user's own updater. Each fragment descriptor (40 bytes; see
re/notes/g2-hardware-and-emulation.md 2.5) gives pointers to its X, Y and P
words (24-bit, in 32-bit big-endian containers), a marker 0xFF/0xFE/0xFD000000,
u16 P length, u16 flags, u32 X and Y counts, u32 cycles and two zero words.
For each one this writes OUTDIR/<descriptor address>.frag ("P n words", "X n
words", "Y n words", hex) and OUTDIR/index.json. The scan is strict (counts in
range, zero padding words), so it may skip odd layouts.

Nothing from Clavia is stored in this script; keep its output out of the
repository (original/ is gitignored).
"""
import json
import os
import struct
import sys

BASE = 0x30000400


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    code = open(argv[1], "rb").read()
    out = argv[2]
    os.makedirs(out, exist_ok=True)

    def u32(off):
        return struct.unpack(">I", code[off:off + 4])[0]

    def words(ptr, n):
        o = ptr - BASE
        return [u32(o + 4 * i) & 0xFFFFFF for i in range(n)]

    index = []
    for off in range(0, len(code) - 40, 2):
        if u32(off + 12) not in (0xFF000000, 0xFE000000, 0xFD000000):
            continue
        px, py, pp = u32(off), u32(off + 4), u32(off + 8)
        plen, flags = struct.unpack(">HH", code[off + 16:off + 20])
        nx, ny, cycles = u32(off + 20), u32(off + 24), u32(off + 28)
        if not (all(BASE <= p < BASE + len(code) for p in (px, py, pp)) and 0 < plen < 2000
                and nx < 4000 and ny < 4000 and u32(off + 32) == 0 and u32(off + 36) == 0):
            continue
        name = f"{BASE + off:08x}"
        blocks = (("P", words(pp, plen)), ("X", words(px, nx)), ("Y", words(py, ny)))
        with open(os.path.join(out, name + ".frag"), "w") as f:
            for tag, ws in blocks:
                f.write(f"{tag} {len(ws)} " + " ".join(f"{w:06x}" for w in ws) + "\n")
        index.append(dict(descriptor=name, p=plen, x=nx, y=ny, flags=flags, cycles=cycles,
                          marker=u32(off + 12) >> 24))
    json.dump(index, open(os.path.join(out, "index.json"), "w"), indent=1)
    print(f"{len(index)} fragments written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
