#!/usr/bin/env python3
"""The stand-alone app has G2fresh's own icon (plugin/icon/G2fresh.png), not none: without an icon macOS shows a
blank application in the Dock, the Finder and the Applications folder.

  app_icon_check.py --app build/plugin/G2Editor_artefacts/Release/Standalone/G2fresh.app

Checks that the bundle's Info.plist names an icon file, that the file is in the bundle's Resources, and that it holds
the 1024-pixel image macOS uses for the largest Finder views (the "ic10" entry of an .icns). Exit 0 when it does, 1
when not.
"""
import argparse
import os
import plistlib
import struct
import sys


def icns_types(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"icns":
        return None
    types, i = [], 8
    while i + 8 <= len(data):
        kind, size = struct.unpack(">4sI", data[i:i + 8])
        if size < 8:
            break
        types.append(kind.decode("latin-1"))
        i += size
    return types


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    a = ap.parse_args()
    with open(os.path.join(a.app, "Contents", "Info.plist"), "rb") as f:
        info = plistlib.load(f)
    name = info.get("CFBundleIconFile")
    if not name:
        print("the app's Info.plist names no icon (CFBundleIconFile)")
        return 1
    icon = os.path.join(a.app, "Contents", "Resources", name if name.endswith(".icns") else name + ".icns")
    if not os.path.isfile(icon):
        print(f"the app's icon {name} is not in its Resources")
        return 1
    types = icns_types(icon)
    if types is None:
        print(f"{icon} is not an .icns file")
        return 1
    if "ic10" not in types:
        print(f"the app's icon has no 1024-pixel image (it has {', '.join(types)})")
        return 1
    print(f"the app's icon: {name}, {', '.join(types)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
