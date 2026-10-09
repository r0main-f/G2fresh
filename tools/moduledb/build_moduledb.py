#!/usr/bin/env python3
"""Builds data/modules.json and data/params.json.

Sources, in order of authority:
  1. tools/moduledb/ghidra_facts.json: facts read from the original editor
     v1.62 (module tables, factory registration, PANL facts), produced by
     extract_ghidra_facts.py. Wins whenever it has the information.
  2. Bruno Verhue's ModuleDef.xml / ParamDef.xml (GPL-2-or-later) from
     third_party/nord_g2_editor/Gen1/Bin: connector names, range types
     (RangeType, ButtonText), default param labels, and the 4 modules the
     editor no longer knows (Driver, Resonator, Red2Blue, Blue2Red).

Stdlib only.

    build_moduledb.py [--repo DIR]
"""
import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET

NS = {"x": "http://www.yourtargetnamespace.com"}

# Connector names Verhue gives in an order that disagrees with the editor.
# The editor's PANL CodeRef/Type is authoritative: PitchTrack output 1 is the
# Control "Pitch" jack, output 2 the Logic "Gate" jack (Verhue swaps them).
CONNECTOR_NAME_FIXES = {
    (198, "outputs"): {1: "Pitch", 2: "Gate"},
}

COLORS = {("Audio", "Static"): "red", ("Audio", "Dynamic"): "blue_red",
          ("Control", "Static"): "blue", ("Control", "Dynamic"): "blue_red",
          ("Logic", "Static"): "yellow", ("Logic", "Dynamic"): "yellow_orange"}

CONTEXT_BITS = ["fx", "va", "patchSettings"]   # NSFile_V7::EContext 0/1/2 = patch location

PATCH_SETTINGS_NAMES = {   # CBModuleToolbar label -> ModuleInfo symbol is in ghidra_facts
    6: "Morph", 95: "Gain", 135: "Glide", 137: "Bend", 138: "Vibrato",
    136: "Arpeggiator", 153: "Misc",
}


def text(el, tag):
    x = el.find("x:" + tag, NS)
    return None if x is None else x.text


def load_verhue(root):
    base = os.path.join(root, "third_party/nord_g2_editor/Gen1/Bin")
    mods = {}
    for md in ET.parse(os.path.join(base, "ModuleDef.xml")).getroot().findall("x:ModuleDef", NS):
        def conns(tag):
            x = md.find("x:" + tag, NS)
            return [] if x is None else [{"name": text(c, "Name").strip(), "type": text(c, "Type")}
                                         for c in x.findall("x:Connector", NS)]

        def params(tag):
            x = md.find("x:" + tag, NS)
            out = []
            for p in ([] if x is None else x.findall("x:Param", NS)):
                out.append({"name": text(p, "Name"), "default": int(text(p, "DefaultValue")),
                            "rangeId": int(text(p, "Id")), "label": text(p, "ParamLabel")})
            return out

        t = int(text(md, "ModuleType"))
        mods[t] = {"type": t, "page": text(md, "Page"), "pageIndex": int(text(md, "PageIndex")),
                   "shortName": text(md, "ShortName"), "longName": text(md, "LongName"),
                   "height": int(text(md, "Height")), "isLed": int(text(md, "IsLed")),
                   "uprate": int(text(md, "Uprate")),
                   "inputs": conns("Inputs"), "outputs": conns("Outputs"),
                   "params": params("Params"), "modes": params("Modes")}
    ranges = {}
    for pd in ET.parse(os.path.join(base, "ParamDef.xml")).getroot().findall("x:ParamDef", NS):
        i = int(text(pd, "Id"))
        bt = text(pd, "ButtonText")
        ranges[i] = {"id": i, "rangeType": text(pd, "RangeType"),
                     "low": int(text(pd, "LowValue")), "high": int(text(pd, "HighValue")),
                     "default": int(text(pd, "DefaultValue")), "verhueParamType": int(text(pd, "ParamType")),
                     "buttonText": [s for s in bt.split(";") if s] if bt else None,
                     "comment": text(pd, "Comments")}
    return mods, ranges


def connectors(panel_jacks, verhue_jacks, fixes):
    out = []
    for j in panel_jacks:
        i = j["index"]
        name = fixes.get(i) or (verhue_jacks[i]["name"] if i < len(verhue_jacks) else None)
        out.append({"index": i, "name": name, "type": j["type"].lower(),
                    "bandwidth": j["bandwidth"].lower(), "color": COLORS[(j["type"], j["bandwidth"])]})
    return out


def verhue_connectors(vj):
    rev = {v: k for k, v in COLORS.items() if k[1] == "Static"}
    out = []
    for i, c in enumerate(vj):
        dyn = c["type"] in ("blue_red", "yellow_orange")
        kind = {"red": "audio", "blue": "control", "yellow": "logic", "yellow_orange": "logic"}.get(c["type"])
        out.append({"index": i, "name": c["name"], "type": kind, "bandwidth": "dynamic" if dyn else "static",
                    "color": c["type"]})
    return out


def build_params(mi, panel, vparams, ranges):
    out = []
    for i, p in enumerate(mi["params"]):
        v = vparams[i] if i < len(vparams) else None
        ctl = panel["controls"].get(str(i)) if panel else None
        e = {"index": i, "name": p["name"], "min": 0, "max": p["max"], "count": p["max"] + 1,
             "default": p["default"], "paramClass": p["paramClass"],
             "rangeId": v["rangeId"] if v else None,
             "rangeType": ranges[v["rangeId"]]["rangeType"] if v else None,
             "morphable": p["morphable"], "midiAssignable": p["midiAssignable"],
             "momentary": p["momentary"],
             "textFunc": p["textFunc"], "textFuncDeps": p["textFuncDeps"],
             "infoFunc": ctl["infoFunc"] if ctl else None,
             "control": ctl["kinds"][0] if ctl else None,
             "knobButtonParam": p["knobButtonParam"], "affects": p["affects"]}
        if v and v["name"] != p["name"]:
            e["verhueName"] = v["name"]
        if v and v["label"]:
            e["defaultLabels"] = v["label"].split(";")
        out.append(e)
    return out


def build_modes(count, panel, vmodes, ranges):
    out = []
    pm = panel["modes"] if panel else []
    for i in range(count):
        v = vmodes[i] if i < len(vmodes) else None
        sel = pm[i] if i < len(pm) else None
        n = sel["count"] if sel else (ranges[v["rangeId"]]["high"] + 1 if v else None)
        out.append({"index": i, "name": v["name"] if v else None, "min": 0,
                    "max": n - 1 if n else None, "count": n, "default": 0,
                    "rangeId": v["rangeId"] if v else None,
                    "rangeType": ranges[v["rangeId"]]["rangeType"] if v else None,
                    "infoFunc": sel["infoFunc"] if sel else None})
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
    a = ap.parse_args(argv)
    root = a.repo
    facts = json.load(open(os.path.join(root, "tools/moduledb/ghidra_facts.json"), encoding="utf-8"))
    vmods, ranges = load_verhue(root)
    panels = facts["panels"]
    cats = facts["factory"]["categories"]
    locked = set(facts["defaultLocked"])
    group_of = {t: g["name"] for g in facts["replaceGroups"] for t in g["types"]}

    # pageIndex = rank inside the category (Verhue's numbering, spacers removed)
    browser = [b for b in facts["factory"]["builders"] if b["category"] is not None]
    rank = {}
    for c in cats:
        slots = sorted(b["toolbarSlot"] for b in browser if b["category"] == c["index"])
        for b in browser:
            if b["category"] == c["index"]:
                rank[b["type"]] = slots.index(b["toolbarSlot"])

    modules, used_panels = [], set()
    faces = {int(k): v for k, v in facts.get("faceBitmaps", {}).items()}
    used_faces = {b["faceResId"] for b in facts["factory"]["builders"] if b.get("faceResId") is not None}
    used_faces.add(facts["nameModule"]["faceResId"])

    def face_candidate(resid, height):
        """Unproven face for a panel with no builder: the unused face-shaped CBMP at
        PANL id + 1 with the right height (the most common pairing among builders)."""
        f = faces.get(resid + 1)
        if f and resid + 1 not in used_faces and f[1] == 15 * height:
            return resid + 1
        return None
    for b in facts["factory"]["builders"]:
        t = b["type"]
        v = vmods.get(t)
        m = {"typeId": t}
        if b["builder"] == "CBModuleName":
            nb = facts["nameModule"]
            m.update({"kind": "name", "shortName": nb["shortName"], "longName": nb["longName"],
                      "fileName": None, "panelResId": None, "faceResId": nb["faceResId"],
                      "height": nb["height"], "inputs": [], "outputs": [], "params": [], "modes": []})
            mi = None
        else:
            mi = facts["moduleInfo"][str(t)]
            if b["builder"] == "CBModuleToolbar":
                m.update({"kind": "patchSettings", "shortName": b["label"], "longName": b["label"],
                          "fileName": None, "panelResId": None, "faceResId": None, "height": None})
                panel = None
            else:
                panel = panels[str(b["panelResId"])]
                used_panels.add(b["panelResId"])
                m.update({"kind": "module", "shortName": panel["name"], "longName": panel["tooltip"],
                          "fileName": panel["fileName"], "panelResId": b["panelResId"],
                          "faceResId": b["faceResId"], "height": panel["height"]})
            vin = v["inputs"] if v else []
            vout = v["outputs"] if v else []
            m["inputs"] = connectors(panel["inputs"], vin, CONNECTOR_NAME_FIXES.get((t, "inputs"), {})) if panel else []
            m["outputs"] = connectors(panel["outputs"], vout, CONNECTOR_NAME_FIXES.get((t, "outputs"), {})) if panel else []
            m["params"] = build_params(mi, panel, v["params"] if v else [], ranges)
            m["modes"] = build_modes(mi["modeCount"], panel, v["modes"] if v else [], ranges)
            m["editOrder"] = mi["editOrder"]
        if v and v["shortName"] != m["shortName"]:
            m["verhueShortName"] = v["shortName"]
        if v and v["longName"] != m["longName"]:
            m["verhueLongName"] = v["longName"]
        if b["category"] is not None:
            m.update({"category": cats[b["category"]]["name"], "categoryIndex": b["category"],
                      "pageIndex": rank[t], "toolbarSlot": b["toolbarSlot"]})
        else:
            m.update({"category": None, "categoryIndex": None, "pageIndex": None, "toolbarSlot": None})
        ctx = mi["contextMask"] if mi else 7
        dyn = any(c["bandwidth"] == "dynamic" for c in m["inputs"] + m["outputs"])
        m["flags"] = {
            "selectable": b["category"] is not None,
            "inEditor": True,
            "defaultLocked": t in locked,          # Verhue: IsLed
            "uprate": 0,                           # default bandwidth; CBuildModule::GetBandWidth == 0 for all
            "hasDynamicConnectors": dyn,
            "keepIfUnused": mi["keepIfUnused"] if mi else False,
            "contexts": [n for i, n in enumerate(CONTEXT_BITS) if ctx >> i & 1],
        }
        if mi:
            m["ledNames"] = mi["ledNames"]
            m["resources"] = mi.get("resources")
            m["moduleInfoSymbol"] = mi["symbol"]
        if panel_leds := (panels[str(m["panelResId"])]["leds"] if m.get("panelResId") else None):
            m["leds"] = panel_leds
        m["replaceGroup"] = group_of.get(t)
        modules.append(m)

    # Modules Verhue lists but the v1.62 editor has no builder/table for.
    known = {m["typeId"] for m in modules}
    by_name = {}
    for r, p in panels.items():
        by_name.setdefault(p["name"], []).append(int(r))
    for t, v in sorted(vmods.items()):
        if t in known:
            continue
        cand = [r for r in by_name.get(v["shortName"], []) if r not in used_panels]
        panel = panels[str(cand[0])] if len(cand) == 1 else None
        if panel:
            used_panels.add(cand[0])
        m = {"typeId": t, "kind": "module",
             "shortName": panel["name"] if panel else v["shortName"],
             "longName": panel["tooltip"] if panel else v["longName"],
             "fileName": panel["fileName"] if panel else None,
             "panelResId": cand[0] if panel else None, "panelResIdSource": "name match (no builder)",
             "faceResId": None,
             "faceResIdCandidate": face_candidate(cand[0], panel["height"]) if panel else None,
             "height": panel["height"] if panel else v["height"],
             "inputs": verhue_connectors(v["inputs"]), "outputs": verhue_connectors(v["outputs"])}
        if panel:   # take signal type/bandwidth from the panel when counts agree
            for kind in ("inputs", "outputs"):
                if len(panel[kind]) == len(m[kind]):
                    m[kind] = connectors(panel[kind], v[kind], {})
        m["params"] = [{"index": i, "name": p["name"], "min": ranges[p["rangeId"]]["low"],
                        "max": ranges[p["rangeId"]]["high"],
                        "count": ranges[p["rangeId"]]["high"] - ranges[p["rangeId"]]["low"] + 1,
                        "default": p["default"], "paramClass": None, "rangeId": p["rangeId"],
                        "rangeType": ranges[p["rangeId"]]["rangeType"],
                        "infoFunc": (panel["controls"].get(str(i)) or {}).get("infoFunc") if panel else None}
                       for i, p in enumerate(v["params"])]
        m["modes"] = build_modes(len(v["modes"]), panel, v["modes"], ranges)
        m.update({"category": None, "categoryIndex": None, "pageIndex": None, "toolbarSlot": None,
                  "verhuePage": v["page"], "verhuePageIndex": v["pageIndex"]})
        m["flags"] = {"selectable": False, "inEditor": False, "defaultLocked": bool(v["isLed"]),
                      "uprate": v["uprate"],
                      "hasDynamicConnectors": any(c["bandwidth"] == "dynamic" for c in m["inputs"] + m["outputs"]),
                      "keepIfUnused": False, "contexts": None}
        m["replaceGroup"] = None
        m["source"] = "verhue"
        modules.append(m)

    unmapped = []
    for r, p in sorted(panels.items(), key=lambda kv: int(kv[0])):
        if int(r) in used_panels:
            continue
        unmapped.append({"panelResId": int(r), "shortName": p["name"], "longName": p["tooltip"],
                         "fileName": p["fileName"], "height": p["height"], "version": p["version"],
                         "inputCount": len(p["inputs"]), "outputCount": len(p["outputs"]),
                         "paramIndices": sorted(int(k) for k in p["controls"]),
                         "modeCount": len(p["modes"]), "typeId": None, "inEditor": False,
                         "faceResIdCandidate": face_candidate(int(r), p["height"])})

    modules.sort(key=lambda m: m["typeId"])
    out = {
        "comment": "Nord Modular G2 module database. Facts from the original editor v1.62 "
                   "(tools/moduledb/ghidra_facts.json) merged with Bruno Verhue's ModuleDef.xml "
                   "(GPL-2-or-later). Generated by tools/moduledb/build_moduledb.py; do not edit.",
        "categories": [{"index": c["index"], "name": c["name"], "color": c["color"],
                        "toolbarSlots": c["slots"]} for c in cats],
        "modules": modules,
        "unmappedPanels": unmapped,
    }
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    with open(os.path.join(root, "data/modules.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
        f.write("\n")

    classes = {}
    for m in modules:
        for p in m["params"]:
            if p.get("paramClass") is None:
                continue
            c = classes.setdefault(p["paramClass"], {"paramClass": p["paramClass"], "uses": 0,
                                                     "rangeIds": set(), "maxValues": set()})
            c["uses"] += 1
            c["maxValues"].add(p["max"])
            if p["rangeId"] is not None:
                c["rangeIds"].add(p["rangeId"])
    params = {
        "comment": "Parameter range definitions (Verhue's ParamDef.xml, GPL-2-or-later; referenced by "
                   "modules.json params[].rangeId) and the editor's own parameter classes "
                   "(param spec byte 0x12, referenced by params[].paramClass). Generated by "
                   "tools/moduledb/build_moduledb.py; do not edit.",
        "ranges": [ranges[k] for k in sorted(ranges)],
        "paramClasses": [{"paramClass": k, "uses": c["uses"], "rangeIds": sorted(c["rangeIds"]),
                          "maxValues": sorted(c["maxValues"])} for k, c in sorted(classes.items())],
    }
    with open(os.path.join(root, "data/params.json"), "w", encoding="utf-8") as f:
        json.dump(params, f, indent=1, ensure_ascii=False)
        f.write("\n")
    sel = sum(1 for m in modules if m["flags"]["selectable"])
    print(f"{len(modules)} modules ({sel} selectable), {len(unmapped)} unmapped panels, "
          f"{len(ranges)} ranges, {len(classes)} param classes -> data/")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
