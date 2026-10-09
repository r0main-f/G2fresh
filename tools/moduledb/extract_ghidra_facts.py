#!/usr/bin/env python3
"""Extracts the module tables of the original G2 editor (v1.62, Mac i386) into
tools/moduledb/ghidra_facts.json, so that build_moduledb.py runs without
Ghidra or any Clavia file.

Inputs (all gitignored, regenerate them from your own copy of the editor):

  --dump    output dir of re/ghidra_scripts/ModuleDbDump.java
            (blocks.json, block_N.bin, datasyms.tsv)
  --decomp  re/out/decomp (ExportAll.java), for CModuleFactory.c,
            CBuildModule.c and CBModuleName.c
  --replace output of ModuleDbDisasm.java run on CReplaceDataBase::AddData
            (0x0010ae74); optional
  --panels  original/derived/panels.json (tools/rsrc/panl.py)
  --cbmp    original/derived/CBMP (CBMP resources as <id>_*.png); optional,
            only their sizes are recorded (to find module faces)

Only facts are written (ids, counts, names, ranges, flags); no panel layout
or artwork.

    extract_ghidra_facts.py --dump D --decomp re/out/decomp \\
        --replace adddata.asm --panels original/derived/panels.json \\
        --cbmp original/derived/CBMP -o tools/moduledb/ghidra_facts.json
"""
import argparse
import bisect
import json
import os
import re
import struct
import sys

PARAM_SPEC_SIZE = 0x1C
CONTROL_KINDS = {"Knob", "ButtonText", "ButtonFlat", "ButtonIncDec", "ButtonRadio",
                 "ButtonRadioEdit", "ButtonPush", "LevelShift", "TextEdit", "EnvCurve"}
LED_KINDS = {"Led", "MiniVU"}


class Image:
    """The program's initialized memory plus its non-function symbols."""

    def __init__(self, d):
        self.blocks = []
        for b in json.load(open(os.path.join(d, "blocks.json"))):
            with open(os.path.join(d, b["file"]), "rb") as f:
                self.blocks.append((int(b["start"], 16), f.read()))
        self.by_name = {}
        self.at = {}
        for line in open(os.path.join(d, "datasyms.tsv"), encoding="utf-8"):
            a, _t, n = line.rstrip("\n").split("\t")
            a = int(a, 16)
            self.by_name.setdefault(n, a)
            self.at.setdefault(a, []).append(n)

    def read(self, addr, n):
        for s, d in self.blocks:
            if s <= addr < s + len(d):
                return d[addr - s:addr - s + n]
        raise KeyError(hex(addr))

    def u8(self, a):
        return self.read(a, 1)[0]

    def u32(self, a):
        return struct.unpack("<I", self.read(a, 4))[0]

    def sym(self, a):
        for n in self.at.get(a, []):
            if n.startswith("_k"):
                return n
        return None

    def cstr(self, a, maxlen):
        return self.read(a, maxlen).split(b"\0")[0].decode("mac_roman")


# --------------------------------------------------------------------------
# CModuleFactory::CModuleFactory (000adce0): categories and builders

def parse_factory(decomp):
    text = open(os.path.join(decomp, "CModuleFactory.c"), encoding="utf-8").read()
    start = text.index("// 000adce0  CModuleFactory::CModuleFactory")
    end = text.index("\n// ", start + 10)
    body = text[start:end].split("\n")
    strs, cats, builders = {}, [], []
    color = bitmap = None
    for l in body:
        m = re.search(r"CRGBColor::CRGBColor\(\w+,(\w+),(\w+),(\w+)\)", l)
        if m:
            color = [int(x, 0) >> 8 for x in m.groups()]
        m = re.search(r'std::string::string\((\w+),"([^"]*)"', l)
        if m:
            strs[m.group(1)] = m.group(2)
        m = re.search(r"RegisterCategory\(this,(\w+),", l)
        if m:
            cats.append({"index": len(cats), "name": strs[m.group(1)], "color": color, "slots": 0})
            continue
        if re.search(r"\*pcVar2 = \*pcVar2 \+ '\\x01'", l):   # RegisterSpacing (inlined)
            cats[-1]["slots"] += 1
            continue
        m = re.search(r"TBitmapID::TBitmapID\(\w+,(\w+)\)", l)
        if m:
            bitmap = int(m.group(1), 0)
        m = re.search(r"CMBGeneric::CMBGeneric\(pCVar1,(\w+),\w+,&(_k\w+ModuleInfo)\)", l)
        if m:
            builders.append({"builder": "CMBGeneric", "panelResId": int(m.group(1), 0),
                             "faceResId": bitmap, "info": m.group(2),
                             "category": len(cats) - 1, "toolbarSlot": cats[-1]["slots"]})
            cats[-1]["slots"] += 1
            continue
        if "CBModuleName::CBModuleName" in l:
            builders.append({"builder": "CBModuleName", "panelResId": None, "info": None,
                             "category": len(cats) - 1, "toolbarSlot": cats[-1]["slots"]})
            cats[-1]["slots"] += 1
            continue
        m = re.search(r"CBModuleToolbar::CBModuleToolbar\(pCVar3,&(_k\w+ModuleInfo),(\w+)\)", l)
        if m:
            builders.append({"builder": "CBModuleToolbar", "panelResId": None, "info": m.group(1),
                             "label": strs[m.group(2)], "category": None, "toolbarSlot": None})
    return cats, builders


def parse_name_builder(decomp):
    text = open(os.path.join(decomp, "CBModuleName.c"), encoding="utf-8").read()
    t = re.search(r"this\[8\] = \(CBModuleName\)(0x[0-9a-f]+);", text).group(1)
    bmp = re.search(r"TBitmapID\(\(TBitmapID \*\)&local_18,(0x[0-9a-f]+)\)", text).group(1)
    h = re.search(r"this\[0x3c\] = \(CBModuleName\)(0x[0-9a-f]+);", text).group(1)
    return {"type": int(t, 16), "faceResId": int(bmp, 16), "height": int(h, 16),
            "shortName": "Name", "longName": "Name Bar", "resources": "_kNameModuleSize"}


def parse_default_locked(decomp):
    """CBuildModule::BuildNewModule (000ab6c2): types whose CModule is created locked."""
    text = open(os.path.join(decomp, "CBuildModule.c"), encoding="utf-8").read()
    start = text.index("// 000ab6c2  CBuildModule::BuildNewModule")
    end = text.index("\n// ", start + 10)
    return sorted(int(x, 16) for x in re.findall(r"case \(CBuildModule\)(0x[0-9a-f]+):", text[start:end]))


# --------------------------------------------------------------------------
# SModuleInfo and the tables it points to

def read_module_info(img, sym):
    a = img.by_name[sym]
    d = img.read(a, 0x24)
    pdata, order, defs, leds, size = struct.unpack("<5I", d[4:24])
    np_, ni, no, nm = d[0x18:0x1C]
    nled, = struct.unpack("<H", d[0x1C:0x1E])
    ctx, = struct.unpack("<I", d[0x20:0x24])
    info = {"symbol": sym, "address": "%08x" % a, "type": d[0],
            "paramCount": np_, "inputCount": ni, "outputCount": no, "modeCount": nm,
            "keepIfUnused": bool(d[0x1E]), "contextMask": ctx}
    if size:
        raw = img.read(size, 0x20)
        info["resources"] = {"symbol": img.sym(size),
                             "s16": list(struct.unpack("<10h", raw[:20])),
                             "s32": list(struct.unpack("<3i", raw[20:]))}
    info["ledNames"] = []
    for i in range(nled):
        e = img.read(leds + 16 * i, 16)
        info["ledNames"].append({"name": e[:8].split(b"\0")[0].decode("mac_roman"),
                                 "kind": struct.unpack("<i", e[8:12])[0],
                                 "count": struct.unpack("<i", e[12:16])[0]})
    params = []
    for i in range(np_):
        p = pdata + i * PARAM_SPEC_SIZE
        s = img.read(p, PARAM_SPEC_SIZE)
        aff = []
        ap = img.u32(p + 0x18)
        if ap:
            while img.u8(ap) != 0xFF:
                aff.append(img.u8(ap))
                ap += 1
        if s[0xC] != img.u8(defs + i):
            raise ValueError(f"{sym} param {i}: spec default != ParamDefValue")
        params.append({
            "name": s[:11].split(b"\0")[0].decode("mac_roman"),
            "max": s[0xB],
            "default": s[0xC],
            "morphable": bool(s[0xD]),
            "knobButtonParam": None if s[0xE] == 0xFF else s[0xE],
            "momentary": bool(s[0xF]),
            "midiAssignable": bool(s[0x10]),
            "textFunc": None if s[0x11] == 0xFF else s[0x11],
            "paramClass": s[0x12],
            "textFuncDeps": [x for x in s[0x13:0x15] if x != 0xFF],
            "affects": aff,
        })
    info["params"] = params
    info["editOrder"] = list(img.read(order, np_)) if order and np_ else []
    return info


# --------------------------------------------------------------------------
# CReplaceDataBase::AddData (0010ae74), from its disassembly

def parse_replace(path):
    groups, last_str, bytes_at, lea = [], None, {}, None
    for l in open(path, encoding="utf-8"):
        m = re.search(r'; "([^"]*)"', l)
        if m:
            last_str = m.group(1)
        if "CReplaceDataBase::Module_Begin" in l:
            groups.append({"name": last_str, "types": []})
            continue
        code = l.split("  ;")[0].rstrip()
        m = re.search(r"MOV byte ptr \[EBP \+ (-?0x[0-9a-f]+)\],(0x[0-9a-f]+)$", code)
        if m:
            bytes_at[m.group(1)] = int(m.group(2), 16)
        m = re.search(r"LEA EAX,\[EBP \+ (-?0x[0-9a-f]+)\]", code)
        if m:
            lea = m.group(1)
        if "CALL 0x0010a20a" in code:   # CReplaceDataBase::Add(unsigned char const&)
            groups[-1]["types"].append(bytes_at[lea])
    return groups


# --------------------------------------------------------------------------
# PANL facts (no layout)

def panel_facts(p):
    kids = p["children"]

    def jacks(kind):
        return [{"index": e["CodeRef"], "type": e["Type"], "bandwidth": e["Bandwidth"]}
                for e in sorted((e for e in kids if e["type"] == kind), key=lambda e: e["CodeRef"])]

    controls = {}
    for e in kids:
        if e["type"] in CONTROL_KINDS:
            controls.setdefault(e["CodeRef"], {"infoFunc": e["InfoFunc"], "kinds": []})
            controls[e["CodeRef"]]["kinds"].append(e["type"])
    modes = [{"index": e["CodeRef"], "count": e["ImageCount"], "infoFunc": e["InfoFunc"]}
             for e in sorted((e for e in kids if e["type"] == "PartSelector"), key=lambda e: e["CodeRef"])]
    leds = [e for e in kids if e["type"] in LED_KINDS]
    return {
        "name": p["Name"], "fileName": p["FileName"], "tooltip": p["Tooltip"],
        "height": p["Height"], "version": p["Version"],
        "inputs": jacks("Input"), "outputs": jacks("Output"),
        "controls": {str(k): v for k, v in sorted(controls.items())},
        "modes": modes,
        "leds": {"count": len(leds),
                 "groups": (max(e["GroupId"] for e in leds) + 1) if leds else 0,
                 "kinds": sorted({e["type"] for e in leds})},
    }


def face_sizes(d):
    """Width/height of every module-face-shaped CBMP (255/256 px wide, 15 px rows)."""
    out = {}
    for n in os.listdir(d):
        m = re.match(r"(\d+)_.*\.png$", n)
        if not m:
            continue
        with open(os.path.join(d, n), "rb") as f:
            w, h = struct.unpack(">II", f.read(24)[16:24])
        if w in (255, 256) and h % 15 == 0:
            out[str(int(m.group(1)))] = [w, h]
    return dict(sorted(out.items(), key=lambda kv: int(kv[0])))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--panels", required=True)
    ap.add_argument("--replace")
    ap.add_argument("--cbmp")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args(argv)

    img = Image(a.dump)
    cats, builders = parse_factory(a.decomp)
    name_builder = parse_name_builder(a.decomp)
    infos = {}
    all_info_syms = sorted(n for n in img.by_name if re.fullmatch(r"_k\w+ModuleInfo", n))
    for b in builders:
        if b["info"]:
            mi = read_module_info(img, b["info"])
            b["type"] = mi["type"]
            infos[str(mi["type"])] = mi
        else:
            b["type"] = name_builder["type"]
    registered = {b["info"] for b in builders if b["info"]}
    unregistered = [s for s in all_info_syms if s not in registered]

    panels = {p["resId"]: p for p in json.load(open(a.panels, encoding="utf-8"))}
    facts = {
        "comment": "Facts extracted from the Nord Modular G2 Editor v1.62 (Mac, i386) by "
                   "tools/moduledb/extract_ghidra_facts.py. See re/notes/module-db.md.",
        "factory": {"function": "CModuleFactory::CModuleFactory @ 000adce0",
                    "vectorSize": 0xDC, "categories": cats, "builders": builders},
        "nameModule": name_builder,
        "defaultLocked": parse_default_locked(a.decomp),
        "moduleInfo": infos,
        "unregisteredModuleInfo": unregistered,
        "replaceGroups": parse_replace(a.replace) if a.replace else [],
        "faceBitmaps": face_sizes(a.cbmp) if a.cbmp else {},
        "panels": {str(r): panel_facts(p) for r, p in sorted(panels.items())},
    }
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(facts, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"{len(builders)} builders, {len(infos)} SModuleInfo, {len(panels)} panels -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
