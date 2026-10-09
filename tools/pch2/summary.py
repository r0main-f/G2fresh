#!/usr/bin/env python3
"""Writes a plain-text field listing of a .pch2/.prf2 file using the reference
decoder (pch2dump.py). tests/test_file.cpp prints the same listing from the
C++ codec, so the two implementations are compared field by field.

    summary.py <file> [<file>...]          # listing to stdout
    summary.py --golden <outdir> <files>   # one <name>.txt per file
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch2dump as P  # noqa: E402

CABLES = ["red", "blue", "yellow", "orange", "green", "purple", "white"]


def hx(s):
    return s.encode("latin-1").hex() or "-"


def listing(data):
    j = P.decode_file(data)
    out = ["F %d %d %d %s" % (j["version"], j["type"], int(j["crc_valid"]), " ".join(hx(l) for l in j["text_header"]))]
    for s in j["sections"]:
        sid = int(s["id"], 16)
        if "raw" in s:
            out.append("RAW %02X %s" % (sid, s["raw"]))
            continue
        out.append("S %02X %d %s" % (sid, s.get("_pad", 0), s.get("_trailing", "") or "-"))
        if sid == 0x21:
            out.append("H %s %d %d %d %s %d %d %d %d %d" % (
                " ".join(str(s[n]) for n, _ in P.HDR_LEGACY), s["voice_count"], s["splitter_pos"],
                s["octave_shift"], "".join(str(s["cables_visible"][c]) for c in CABLES), s["mono_mode"],
                s["active_variation"], s["category"], s["pad"], s["reserved_byte"]))
        elif sid == 0x4A:
            for m in s["modules"]:
                out.append("M %d %d %d %d %d %d %d %d %d %s" % (
                    s["location"], m["type"], m["index"], m["col"], m["row"], m["color"], m["uprate"],
                    m["is_led"], m["reserved"], " ".join(map(str, m["modes"])) or "-"))
        elif sid == 0x69:
            for v in [s["last"]] + s["notes"]:
                out.append("V %d %d %d" % (v["note"], v["attack_vel"], v["release_vel"]))
        elif sid == 0x52:
            out.append("CL %d %d %d" % (s["location"], s["pad"], len(s["cables"])))
            for c in s["cables"]:
                out.append("C %d %d %d %d %d %d" % (c["color"], c["from_module"], c["from_conn"],
                                                     c["from_is_output"], c["to_module"], c["to_conn"]))
        elif sid == 0x4D:
            out.append("PL %d %d %d" % (s["location"], s["variation_count"], len(s["modules"])))
            for m in s["modules"]:
                for v in m["variations"]:
                    out.append("P %d %d %d %s" % (m["index"], m["param_count"], v["variation"],
                                                  " ".join(map(str, v["values"])) or "-"))
        elif sid == 0x65:
            out.append("ML %d %s" % (s["morph_count"], " ".join(map(str, s["keyboard_assign"])) or "-"))
            for v in s["variations"]:
                out.append("MV %d %s" % (v["variation"], " ".join(map(str, v["legacy_dials"])) or "-"))
                for m in v["morphs"]:
                    out.append("MA %d %d %d %d %d" % (m["location"], m["module"], m["param"], m["morph"], m["range"]))
        elif sid in (0x62, 0x5F):
            for i, k in enumerate(s["knobs"]):
                if k is None:
                    out.append("K %d -" % i)
                else:
                    out.append("K %d %d %d %d %d %d" % (i, k["location"], k["module"], k["is_button"],
                                                        k["param"], k.get("slot", 0)))
        elif sid == 0x60:
            for c in s["controllers"]:
                out.append("R %d %d %d %d" % (c["cc"], c["location"], c["module"], c["param"]))
        elif sid == 0x5B:
            out.append("DL %d" % s["location"])
            for m in s["modules"]:
                data = P._unrecords(m["records"]) if "records" in m else bytes.fromhex(m["bytes"])
                out.append("D %d %s" % (m["index"], data.hex() or "-"))
        elif sid == 0x5A:
            out.append("NL %d %d" % (s["location"], s["reserved"]))
            for n in s["names"]:
                out.append("N %d %s" % (n["index"], hx(n["name"])))
        elif sid == 0x6F:
            out.append("T %s" % hx(s["text"]))
        elif sid == 0x11:
            out.append("PH %d %d %d %d %d %d %d %d %d" % tuple(s[k] for k in (
                "unknown_08", "focused_slot", "global_pages", "kbd_range_enabled", "master_clock_bpm",
                "unknown_18", "master_clock_run", "reserved_1", "reserved_2")))
            for sl in s["slots"]:
                out.append("PS %s %s" % (hx(sl["patch_name"]), " ".join(str(sl[f]) for f in P.SLOT_FIELDS)))
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) >= 3 and argv[1] == "--golden":
        os.makedirs(argv[2], exist_ok=True)
        for f in argv[3:]:
            name = os.path.splitext(os.path.basename(f))[0] + ".txt"
            with open(f, "rb") as fi, open(os.path.join(argv[2], name), "w") as fo:
                fo.write(listing(fi.read()))
        return 0
    for f in argv[1:]:
        with open(f, "rb") as fi:
            sys.stdout.write(listing(fi.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
