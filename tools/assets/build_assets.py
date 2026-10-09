#!/usr/bin/env python3
"""Builds the bundled Clavia skin (assets/clavia/) from the original editor's
resource file, so the app looks like the original out of the box.

    build_assets.py <Nord Modular G2 Editor.rsrc>

Outputs (committed):
    assets/clavia/panels.json      module panel layouts (PANL), compact JSON
    assets/clavia/cbmp/<id>.png    module faces and control sprites (CBMP)
    assets/clavia/jpeg/<id>.jpg    small UI sprites (LEDs, cable swatches, ...)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "rsrc"))
from bmp2png import bmp_to_rgb, write_png  # noqa: E402
from panl import parse_panl  # noqa: E402
from rsrc import parse  # noqa: E402

OUT = os.path.join(ROOT, "assets", "clavia")
# Control sprites used by the module renderer, besides the module faces.
CBMP_SPRITES = [199, 200, 201, 202, 203, 204, 205, 531, 532]
JPEG_SPRITES = [115, 116, 117, 118, 212, 213, 214, 215, 216, 217, 240, 241, 242, 243, 244, 245,
                246, 247, 248, 250, 251, 268, 269, 272, 273, 874, 877]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        resources = parse(f.read())
    with open(os.path.join(ROOT, "data", "modules.json")) as f:
        faces = {m["faceResId"] for m in json.load(f)["modules"] if m.get("faceResId")}

    os.makedirs(os.path.join(OUT, "cbmp"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "jpeg"), exist_ok=True)
    panels = []
    counts = {"panels": 0, "cbmp": 0, "jpeg": 0}
    for r in resources:
        if r.type == "PANL":
            p = parse_panl(r.data.decode("mac_roman"))
            p["resId"] = r.id
            panels.append(p)
            counts["panels"] += 1
        elif r.type == "CBMP" and (r.id in faces or r.id in CBMP_SPRITES):
            w, h, rgb = bmp_to_rgb(r.data)
            write_png(os.path.join(OUT, "cbmp", f"{r.id}.png"), w, h, rgb)
            counts["cbmp"] += 1
        elif r.type == "JPEG" and r.id in JPEG_SPRITES:
            with open(os.path.join(OUT, "jpeg", f"{r.id}.jpg"), "wb") as f:
                f.write(r.data)
            counts["jpeg"] += 1
    panels.sort(key=lambda p: p["resId"])
    with open(os.path.join(OUT, "panels.json"), "w") as f:
        json.dump(panels, f, separators=(",", ":"), ensure_ascii=False)
    print(counts)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
