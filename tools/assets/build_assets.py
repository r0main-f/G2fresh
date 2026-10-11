#!/usr/bin/env python3
"""Builds the module panel layouts (assets/clavia/panels.json) from the original
editor's resource file, without any of its pixels.

    build_assets.py <Nord Modular G2 Editor.rsrc>
    build_assets.py --check        # panels.json holds no pixels, assets/ no images

Output (committed): assets/clavia/panels.json, the PANL resources as compact
JSON: per module its elements' kinds, positions, sizes, captions and codes. The
pixels in them are left out: a Bitmap element's "Data" and the "Image" icon
strip of the ButtonText, ButtonFlat, ButtonRadio and PartSelector elements
(colon-separated "rrggbb" values).

No image of Clavia's is in the repository or the build. The Classic look takes
the bitmaps (CBMP, JPEG, SMTB) and those inline images from the user's own copy
of the download at run time (clavia/, ui/Skin.cpp), as the Emulated G2 takes
the G2 OS from it.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "rsrc"))
from panl import parse_panl  # noqa: E402
from rsrc import parse  # noqa: E402

OUT = os.path.join(ROOT, "assets", "clavia", "panels.json")
PIXEL_FIELDS = ("Data", "Image")
IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".icns", ".ico", ".pict")


def strip_pixels(node: dict) -> dict:
    for key in PIXEL_FIELDS:
        node.pop(key, None)
    for child in node.get("children", []):
        strip_pixels(child)
    return node


def pixel_fields(node: dict) -> int:
    return sum(key in node for key in PIXEL_FIELDS) + sum(pixel_fields(c) for c in node.get("children", []))


def check() -> int:
    with open(OUT, encoding="utf-8") as f:
        panels = json.load(f)
    problems = []
    if n := sum(pixel_fields(p) for p in panels):
        problems.append(f"{OUT}: {n} pixel fields ({', '.join(PIXEL_FIELDS)})")
    for d, _, files in os.walk(os.path.join(ROOT, "assets")):
        problems += [os.path.join(d, f) for f in files if f.lower().endswith(IMAGE_EXTENSIONS)]
    for p in problems:
        print("Clavia pixels in the repository:", p)
    return 1 if problems else 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--check":
        return check()
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        resources = parse(f.read())
    panels = []
    for r in resources:
        if r.type == "PANL":
            p = strip_pixels(parse_panl(r.data.decode("mac_roman")))
            p["resId"] = r.id
            panels.append(p)
    panels.sort(key=lambda p: p["resId"])
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(panels, f, separators=(",", ":"), ensure_ascii=False)
    print(f"{len(panels)} panels -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
