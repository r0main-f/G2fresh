#!/usr/bin/env python3
"""Classic Mac OS resource map reader for the G2 editor's data-fork .rsrc file.

    rsrc.py list <file.rsrc>
    rsrc.py dump <file.rsrc> <outdir>      # <outdir>/<TYPE>/<id>[_<name>].bin + index.json

Format reference: Inside Macintosh: More Macintosh Toolbox, "Resource Manager".
"""
import json
import os
import re
import struct
import sys
from dataclasses import dataclass


@dataclass
class Resource:
    type: str
    id: int
    name: str | None
    attrs: int
    data: bytes


def parse(blob: bytes) -> list[Resource]:
    data_off, map_off, _data_len, map_len = struct.unpack(">IIII", blob[:16])
    m = blob[map_off:map_off + map_len]
    type_list_off, name_list_off = struct.unpack(">HH", m[24:28])
    ntypes = struct.unpack(">H", m[type_list_off:type_list_off + 2])[0] + 1
    out = []
    for i in range(ntypes):
        p = type_list_off + 2 + 8 * i
        rtype, count, ref_off = struct.unpack(">4sHH", m[p:p + 8])
        for j in range(count + 1):
            q = type_list_off + ref_off + 12 * j
            rid, name_off, attr_and_off = struct.unpack(">hhI", m[q:q + 8])
            attrs = attr_and_off >> 24
            d = data_off + (attr_and_off & 0xFFFFFF)
            size = struct.unpack(">I", blob[d:d + 4])[0]
            name = None
            if name_off != -1:
                n = name_list_off + name_off
                name = m[n + 1:n + 1 + m[n]].decode("mac_roman")
            out.append(Resource(rtype.decode("mac_roman"), rid, name, attrs, blob[d + 4:d + 4 + size]))
    return out


def _safe(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", s)


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] not in ("list", "dump"):
        print(__doc__)
        return 2
    with open(argv[2], "rb") as f:
        res = parse(f.read())
    if argv[1] == "list":
        for r in res:
            print(f"{r.type} {r.id:6d} {len(r.data):8d}  {r.name or ''}")
        return 0
    outdir = argv[3]
    index = []
    for r in res:
        d = os.path.join(outdir, _safe(r.type))
        os.makedirs(d, exist_ok=True)
        fn = f"{r.id}" + (f"_{_safe(r.name)}" if r.name else "") + ".bin"
        with open(os.path.join(d, fn), "wb") as f:
            f.write(r.data)
        index.append({"type": r.type, "id": r.id, "name": r.name, "attrs": r.attrs,
                      "size": len(r.data), "file": f"{_safe(r.type)}/{fn}"})
    with open(os.path.join(outdir, "index.json"), "w") as f:
        json.dump(index, f, indent=1)
    print(f"dumped {len(res)} resources to {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
