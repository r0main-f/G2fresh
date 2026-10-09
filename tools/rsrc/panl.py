#!/usr/bin/env python3
"""Converts the editor's PANL resources (text module-panel definitions) to JSON.

    panl.py <file.rsrc> <out.json>

A PANL resource is a tree of blocks:

    <#Module
      Name:"Scratch"
      XPos:104
      <#Knob
        ID:0
        CodeRef:0
        InfoFunc:209
      #>
    #>

Values are integers or double-quoted strings; keys may contain spaces
("Text Func").
"""
import json
import re
import sys

from rsrc import parse

_KV = re.compile(r'^\s*([A-Za-z][A-Za-z ]*?):(".*"|-?\d+)\s*$')


def parse_panl(text: str) -> dict:
    root = {"type": "root", "children": []}
    stack = [root]
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("<#"):
            node = {"type": line[2:].strip(), "children": []}
            stack[-1]["children"].append(node)
            stack.append(node)
        elif line == "#>":
            stack.pop()
        else:
            m = _KV.match(raw)
            if not m:
                raise ValueError(f"unparsed PANL line: {raw!r}")
            key, val = m.group(1), m.group(2)
            stack[-1][key] = val[1:-1] if val.startswith('"') else int(val)
    if len(stack) != 1 or len(root["children"]) != 1:
        raise ValueError("unbalanced PANL blocks")
    return root["children"][0]


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        resources = parse(f.read())
    panels = []
    for r in resources:
        if r.type != "PANL":
            continue
        module = parse_panl(r.data.decode("mac_roman"))
        module["resId"] = r.id
        panels.append(module)
    panels.sort(key=lambda m: m["resId"])
    with open(argv[2], "w") as f:
        json.dump(panels, f, indent=1, ensure_ascii=False)
    print(f"{len(panels)} panels -> {argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
