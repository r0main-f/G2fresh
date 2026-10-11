#!/usr/bin/env python3
"""Fingerprints of the Classic look's images, for tests/test_clavia.cpp to check
that the run-time extraction (clavia/) gives exactly the pixels the reference
decoders (tools/rsrc/) give, without any of them in the repository.

    skin_fingerprints.py <Nord Modular G2 Editor.rsrc> > tests/clavia_skin_fingerprints.txt

One line per image: "cbmp|smtb <id> <width> <height> <fnv>" (the decoded
top-down RGB bytes), "jpeg <id> <size> <fnv>" (the file as stored) and
"inline <panel> <element> <fnv>" (the element's "rrggbb:..." pixel text).
<fnv> is the 64-bit FNV-1a hash, in hex.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "rsrc"))
from bmp2png import bmp_to_rgb  # noqa: E402
from panl import parse_panl  # noqa: E402
from rsrc import parse  # noqa: E402

CBMP_SPRITES = [199, 200, 201, 202, 203, 204, 205, 531, 532]
JPEG_SPRITES = [115, 116, 117, 118, 212, 213, 214, 215, 216, 217, 240, 241, 242, 243, 244, 245,
                246, 247, 248, 250, 251, 268, 269, 272, 273, 874, 877]
SMTB_SPRITES = [0xD01, 0xD02, 0xD03]


def fnv1a64(data: bytes) -> str:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def unfiltered(width: int, filtered: bytes) -> bytes:
    # bmp_to_rgb returns PNG rows (a filter byte, then RGB)
    stride = 1 + 3 * width
    return b"".join(filtered[i + 1:i + stride] for i in range(0, len(filtered), stride))


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        resources = parse(f.read())
    with open(os.path.join(ROOT, "data", "modules.json")) as f:
        faces = {m["faceResId"] for m in json.load(f)["modules"] if m.get("faceResId")}
    lines = []
    for r in resources:
        if (r.type == "CBMP" and (r.id in faces or r.id in CBMP_SPRITES)) or (r.type == "SMTB" and r.id in SMTB_SPRITES):
            w, h, rgb = bmp_to_rgb(r.data)
            lines.append(f"{r.type.lower()} {r.id} {w} {h} {fnv1a64(unfiltered(w, rgb))}")
        elif r.type == "JPEG" and r.id in JPEG_SPRITES:
            lines.append(f"jpeg {r.id} {len(r.data)} {fnv1a64(r.data)}")
        elif r.type == "PANL":
            panel = parse_panl(r.data.decode("mac_roman"))
            for i, e in enumerate(panel["children"]):
                pixels = e.get("Data") if e["type"] == "Bitmap" else e.get("Image")
                if pixels:
                    lines.append(f"inline {r.id} {i} {fnv1a64(pixels.encode('ascii'))}")
    print("\n".join(sorted(lines, key=lambda s: (s.split()[0], [int(x) for x in s.split()[1:3]]))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
