#!/usr/bin/env python3
"""Extracts the module Replace database of the original editor (v1.62) from the
disassembly of CReplaceDataBase::AddData @ 0010ae74, as printed by
re/ghidra_scripts/ReplaceDisasm.java, into tools/moduledb/replace_facts.json.

    extract_replace_facts.py <0010ae74.asm> [-o tools/moduledb/replace_facts.json]

AddData is straight-line code: every record is built with `MOV byte ptr
[EBP + off],imm` stores, then passed by address (`LEA EAX,[EBP + off]`) to one of
the CReplaceDataBase builder calls below. See re/notes/module-replace.md.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

CALLS = {
    "0x0010a160": "Module_Begin",   # (const char* name)          new CModuleGroup
    "0x0010a20a": "AddType",        # Add(unsigned char const&)    module type of the group
    "0x0010a46e": "Begin_Input",    # (const char* name)           new CItemGroup<CInput>
    "0x0010a4b8": "Begin_Output",   # (const char* name)           new CItemGroup<COutput>
    "0x0010a230": "Begin_Param",    # (const char* name)           new CItemGroup<CParam>
    "0x0010adba": "Begin_Value",    # (const char* name)           new CItemGroup<CValue> (never called)
    "0x00109bca": "AddInput",       # Add(CInput const&)   4 bytes: type, connector, amount param, on param
    "0x00109bee": "AddOutput",      # Add(COutput const&)  2 bytes: type, connector
    "0x00109bb8": "AddParam",       # Add(CParam const&)   2 bytes: type, param
    "0x00109bdc": "AddValue",       # Add(CValue const&)   (never called)
    "0x001096c4": "Module_End",
}
SIZES = {"AddType": 1, "AddInput": 4, "AddOutput": 2, "AddParam": 2}


def off(s):
    return int(s, 16) if not s.startswith("-") else -int(s[1:], 16)


def parse(path):
    mem, lea, last_str = {}, None, None
    groups, cur, sub = [], None, None
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        addr = line.split()[0]
        m = re.search(r'; "([^"]*)"', line)
        code = line.split("  ;")[0].rstrip()
        if m and "MOV dword ptr [ESP + 0x4]" in code:
            last_str = m.group(1)
        m = re.search(r"MOV byte ptr \[EBP \+ (-?0x[0-9a-f]+)\],(0x[0-9a-f]+)$", code)
        if m:
            mem[off(m.group(1)) & 0xFFFFFFFF] = int(m.group(2), 16)
            continue
        m = re.search(r"LEA EAX,\[EBP \+ (-?0x[0-9a-f]+)\]", code)
        if m:
            lea = off(m.group(1)) & 0xFFFFFFFF
            continue
        m = re.search(r"CALL (0x[0-9a-f]+)", code)
        if not m:
            continue
        kind = CALLS.get(m.group(1))
        if kind is None:
            raise SystemExit(f"{addr}: unknown call {m.group(1)}")
        if kind == "Module_Begin":
            cur = {"name": last_str, "addr": addr, "types": [], "inputs": [], "outputs": [], "params": []}
            groups.append(cur)
            sub = None
        elif kind == "Module_End":
            cur, sub = None, None
        elif kind.startswith("Begin_"):
            key = {"Begin_Input": "inputs", "Begin_Output": "outputs", "Begin_Param": "params"}[kind]
            sub = {"name": last_str, "addr": addr, "items": []}
            cur[key].append(sub)
            sub_key = key
        else:
            n = SIZES[kind]
            rec = [mem[(lea + i) & 0xFFFFFFFF] for i in range(n)]
            if kind == "AddType":
                cur["types"].append(rec[0])
            else:
                want = {"AddInput": "inputs", "AddOutput": "outputs", "AddParam": "params"}[kind]
                if sub is None or sub_key != want:
                    raise SystemExit(f"{addr}: {kind} outside a {want} item group")
                sub["items"].append(rec)
    return groups


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("asm")
    ap.add_argument("-o", default=os.path.join(ROOT, "tools", "moduledb", "replace_facts.json"))
    a = ap.parse_args(argv)
    groups = parse(a.asm)
    out = {
        "comment": "Module Replace database of the G2 editor v1.62 (Mac i386), from the "
                   "disassembly of CReplaceDataBase::AddData @ 0010ae74 "
                   "(re/ghidra_scripts/ReplaceDisasm.java, tools/moduledb/extract_replace_facts.py). "
                   "inputs items: [type, connector, amountParam, onParam] (0xFF = none); "
                   "outputs items: [type, connector]; params items: [type, param]. "
                   "See re/notes/module-replace.md.",
        "source": {"function": "CReplaceDataBase::AddData", "addr": "0010ae74"},
        "groups": groups,
    }
    # Compact one-line items for readability.
    text = json.dumps(out, indent=1)
    text = re.sub(r"\[\s+(\d+),\s+(\d+)(?:,\s+(\d+),\s+(\d+))?\s+\]",
                  lambda m: "[" + ", ".join(g for g in m.groups() if g is not None) + "]", text)
    text = re.sub(r'"types": \[[\s\d,]+\]',
                  lambda m: re.sub(r"\s+", " ", m.group(0)).replace("[ ", "[").replace(" ]", "]"), text)
    with open(a.o, "w") as f:
        f.write(text + "\n")
    print(f"{len(groups)} groups -> {a.o}", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1:])
