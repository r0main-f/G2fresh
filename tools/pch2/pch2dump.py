#!/usr/bin/env python3
"""Reference decoder/encoder for Clavia Nord Modular G2 patch (.pch2) and
performance (.prf2) files, file format version 23 (editor v1.62) and the
binary-compatible versions 13..22.

    pch2dump.py dump <file>                 # JSON to stdout
    pch2dump.py encode <file.json> <out>    # JSON back to binary
    pch2dump.py roundtrip <files...>        # decode+encode, byte compare; exit 0 iff all OK
    pch2dump.py selftest <patch files...>   # also builds a synthetic .prf2 from 4 patches

Spec: re/notes/pch2-format.md. Field names follow the Clavia classes
(NSFile_V11::C*Data_11) where the binary gave them away; everything that is
not understood is still kept (reserved fields, padding bits, trailing bytes)
so that encode(decode(x)) == x for every input. A section that the field
decoder cannot reproduce exactly is kept as raw hex ("raw") instead.

Python 3 standard library only. Licence: same as the G2fresh repository (see LICENSE).
"""
import json
import sys

# --------------------------------------------------------------------------
# Bit streams (MSB first, like CBitStream::GetUBits / PutUBits)
# --------------------------------------------------------------------------


class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0  # in bits

    def bits_left(self) -> int:
        return len(self.data) * 8 - self.pos

    def read(self, n: int) -> int:
        if n > self.bits_left():
            raise ValueError("read past end of section (%d bits wanted, %d left)" % (n, self.bits_left()))
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def read_s(self, n: int) -> int:
        v = self.read(n)
        return v - (1 << n) if v & (1 << (n - 1)) else v

    def pad_bits(self) -> int:
        return (8 - (self.pos & 7)) & 7

    def align(self) -> int:
        """Skip to the next byte boundary; return the skipped bits' value."""
        return self.read(self.pad_bits())

    def u8(self) -> int:
        """CBitStream::GetUByte: aligns first. Callers here only use it when aligned."""
        if self.pos & 7:
            raise ValueError("unaligned byte read")
        return self.read(8)

    def u16(self) -> int:
        """CBitStream::GetUWord: two aligned bytes, big endian."""
        return (self.u8() << 8) | self.u8()


class BitWriter:
    def __init__(self):
        self.bits = []

    def write(self, n: int, v: int):
        if not 0 <= v < (1 << n):
            raise ValueError("value %r does not fit in %d bits" % (v, n))
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def write_s(self, n: int, v: int):
        if not -(1 << (n - 1)) <= v < (1 << (n - 1)):
            raise ValueError("value %r does not fit in signed %d bits" % (v, n))
        self.write(n, v & ((1 << n) - 1))

    def pad_bits(self) -> int:
        return (8 - (len(self.bits) & 7)) & 7

    def align(self, value: int = 0):
        self.write(self.pad_bits(), value)

    def u8(self, v: int):
        if len(self.bits) & 7:
            raise ValueError("unaligned byte write")
        self.write(8, v)

    def u16(self, v: int):
        self.u8(v >> 8)
        self.u8(v & 0xFF)

    def raw(self, b: bytes):
        for x in b:
            self.write(8, x)

    def getvalue(self) -> bytes:
        if len(self.bits) & 7:
            raise ValueError("bit stream not byte aligned")
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            v = 0
            for b in self.bits[i:i + 8]:
                v = (v << 1) | b
            out.append(v)
        return bytes(out)


def crc16(data: bytes, crc: int = 0) -> int:
    """CRC_Get16 (Global.c @0014fd40): poly 0x1021, init 0, MSB first = CRC-16/XMODEM."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def s_latin1(b: bytes) -> str:
    return b.decode("latin-1")


def b_latin1(s: str) -> bytes:
    return s.encode("latin-1")


# --------------------------------------------------------------------------
# Section codecs. Each dec_* reads the section payload (after id + length)
# and returns a dict of fields; enc_* writes the same fields back.
# --------------------------------------------------------------------------

# $21 CPatchHeaderData_11 (ReadStream @000664ae, WriteStream @00065564)
HDR_LEGACY = [  # 12 fields always written as 0 by v1.62 (leftovers of the V7 header)
    ("legacy_kb_range_min", 7), ("legacy_kb_range_max", 7),
    ("legacy_vel_range_min", 7), ("legacy_vel_range_max", 7),
    ("legacy_bend_range", 5), ("legacy_a5", 5), ("legacy_b2", 2),
    ("legacy_c7", 7), ("legacy_d2", 2), ("legacy_e7", 7), ("legacy_f2", 2),
    ("legacy_g1", 1),
]
CABLE_COLORS = ["red", "blue", "yellow", "orange", "green", "purple", "white"]


def dec_patch_header(r, ctx):
    d = {}
    for name, n in HDR_LEGACY:
        d[name] = r.read(n)
    d["voice_count"] = r.read(7) + 1          # stored as count-1
    hi = r.read(7)
    lo = r.read(7)
    d["splitter_pos"] = (hi << 7) | lo        # 14 bits, two 7-bit halves
    d["octave_shift"] = r.read(3) - 2         # stored as shift+2
    d["cables_visible"] = {c: r.read(1) for c in CABLE_COLORS}
    d["mono_mode"] = r.read(2)
    d["active_variation"] = r.read(8)
    d["category"] = r.read(8)
    d["pad"] = r.align()
    d["reserved_byte"] = r.u8()               # PutUByte(0) at the end of WriteStream
    return d


def enc_patch_header(w, d, ctx):
    for name, n in HDR_LEGACY:
        w.write(n, d[name])
    w.write(7, d["voice_count"] - 1)
    w.write(7, d["splitter_pos"] >> 7)
    w.write(7, d["splitter_pos"] & 0x7F)
    w.write(3, d["octave_shift"] + 2)
    for c in CABLE_COLORS:
        w.write(1, d["cables_visible"][c])
    w.write(2, d["mono_mode"])
    w.write(8, d["active_variation"])
    w.write(8, d["category"])
    w.align(d["pad"])
    w.u8(d["reserved_byte"])


# $4A CModuleData_11 (ReadStream @0006774a, WriteStream @00067968)
def dec_module_list(r, ctx):
    d = {"location": r.read(2), "modules": []}
    for _ in range(r.read(8)):
        m = {"type": r.read(8), "index": r.read(8), "col": r.read(7), "row": r.read(7),
             "color": r.read(8), "uprate": r.read(1), "is_led": r.read(1),
             "reserved": r.read(6)}
        m["modes"] = [r.read(6) for _ in range(r.read(4))]
        d["modules"].append(m)
    return d


def enc_module_list(w, d, ctx):
    w.write(2, d["location"])
    w.write(8, len(d["modules"]))
    for m in d["modules"]:
        for k, n in (("type", 8), ("index", 8), ("col", 7), ("row", 7), ("color", 8),
                     ("uprate", 1), ("is_led", 1), ("reserved", 6)):
            w.write(n, m[k])
        w.write(4, len(m["modes"]))
        for v in m["modes"]:
            w.write(6, v)


# $69 CCurrentVoiceData_11 (ReadStream @000672c4, WriteStream @00064aca)
def _voice(r):
    return {"note": r.read(7), "attack_vel": r.read(7), "release_vel": r.read(7)}


def dec_current_note(r, ctx):
    d = {"last": _voice(r)}
    d["notes"] = [_voice(r) for _ in range(r.read(5) + 1)]
    return d


def enc_current_note(w, d, ctx):
    def v(x):
        w.write(7, x["note"])
        w.write(7, x["attack_vel"])
        w.write(7, x["release_vel"])
    v(d["last"])
    if not 1 <= len(d["notes"]) <= 32:
        raise ValueError("current_note needs 1..32 notes")
    w.write(5, len(d["notes"]) - 1)
    for x in d["notes"]:
        v(x)


# $52 CCableData_11 (ReadStream @00065eb8, WriteStream @00064d9a)
def dec_cable_list(r, ctx):
    d = {"location": r.read(2), "pad": r.align(), "cables": []}
    for _ in range(r.u16()):
        d["cables"].append({"color": r.read(3), "from_module": r.read(8), "from_conn": r.read(6),
                            "from_is_output": r.read(1), "to_module": r.read(8), "to_conn": r.read(6)})
    return d


def enc_cable_list(w, d, ctx):
    w.write(2, d["location"])
    w.align(d["pad"])
    w.u16(len(d["cables"]))
    for c in d["cables"]:
        for k, n in (("color", 3), ("from_module", 8), ("from_conn", 6), ("from_is_output", 1),
                     ("to_module", 8), ("to_conn", 6)):
            w.write(n, c[k])


# $4D CModuleParamData_11 (ReadStream @00068620, WriteStream @00068928)
def dec_param_list(r, ctx):
    d = {"location": r.read(2)}
    nmod = r.read(8)
    d["variation_count"] = nvar = r.read(8)
    d["modules"] = []
    for _ in range(nmod):
        m = {"index": r.read(8)}
        m["param_count"] = npar = r.read(7)
        m["variations"] = []
        for _ in range(nvar):
            var = r.read(8)
            m["variations"].append({"variation": var, "values": [r.read(7) for _ in range(npar)]})
        d["modules"].append(m)
    return d


def enc_param_list(w, d, ctx):
    w.write(2, d["location"])
    w.write(8, len(d["modules"]))
    w.write(8, d["variation_count"])
    for m in d["modules"]:
        w.write(8, m["index"])
        w.write(7, m["param_count"])
        if len(m["variations"]) != d["variation_count"]:
            raise ValueError("variation list length != variation_count")
        for v in m["variations"]:
            w.write(8, v["variation"])
            if len(v["values"]) != m["param_count"]:
                raise ValueError("value list length != param_count")
            for x in v["values"]:
                w.write(7, x)


# $65 CMorphMapData_11 (ReadStream @00069326, WriteStream @00069c58)
def dec_morph_map(r, ctx):
    nvar = r.read(8)
    d = {"morph_count": r.read(4)}
    nm = d["morph_count"]
    d["keyboard_assign"] = [r.read(2) for _ in range(nm)]
    d["variations"] = []
    for _ in range(nvar):
        v = {"variation": r.read(8), "legacy_dials": [r.read(7) for _ in range(nm)], "morphs": []}
        for _ in range(r.read(8)):
            v["morphs"].append({"location": r.read(2), "module": r.read(8), "param": r.read(7),
                                "morph": r.read(4), "range": r.read_s(8)})
        d["variations"].append(v)
    return d


def enc_morph_map(w, d, ctx):
    w.write(8, len(d["variations"]))
    w.write(4, d["morph_count"])
    if len(d["keyboard_assign"]) != d["morph_count"]:
        raise ValueError("keyboard_assign length != morph_count")
    for x in d["keyboard_assign"]:
        w.write(2, x)
    for v in d["variations"]:
        w.write(8, v["variation"])
        if len(v["legacy_dials"]) != d["morph_count"]:
            raise ValueError("legacy_dials length != morph_count")
        for x in v["legacy_dials"]:
            w.write(7, x)
        w.write(8, len(v["morphs"]))
        for m in v["morphs"]:
            w.write(2, m["location"])
            w.write(8, m["module"])
            w.write(7, m["param"])
            w.write(4, m["morph"])
            w.write_s(8, m["range"])


# $62 / $5F CKnobMapData_11 (ReadStream @00069ef8, WriteStream @00069642)
def dec_knob_map(r, ctx):
    is_global = ctx["sid"] == 0x5F
    knobs = []
    for _ in range(r.u16()):
        if not r.read(1):
            knobs.append(None)
            continue
        k = {"location": r.read(2), "module": r.read(8), "is_button": r.read(2), "param": r.read(7)}
        if is_global:
            k["slot"] = r.read(2)
        knobs.append(k)
    return {"knobs": knobs}


def enc_knob_map(w, d, ctx):
    is_global = ctx["sid"] == 0x5F
    w.u16(len(d["knobs"]))
    for k in d["knobs"]:
        if k is None:
            w.write(1, 0)
            continue
        w.write(1, 1)
        w.write(2, k["location"])
        w.write(8, k["module"])
        w.write(2, k["is_button"])
        w.write(7, k["param"])
        if is_global:
            w.write(2, k["slot"])


# $60 CCtrlMapData_11 (ReadStream @0006a0f6, WriteStream @0006882a)
def dec_ctrl_map(r, ctx):
    return {"controllers": [{"cc": r.read(7), "location": r.read(2), "module": r.read(8), "param": r.read(7)}
                            for _ in range(r.read(7))]}


def enc_ctrl_map(w, d, ctx):
    w.write(7, len(d["controllers"]))
    for c in d["controllers"]:
        w.write(7, c["cc"])
        w.write(2, c["location"])
        w.write(8, c["module"])
        w.write(7, c["param"])


# $5B CModuleCustomData_11 (ReadStream @00066b3a, WriteStream @00064c1a)
# Each module's byte string is a list of records [kind, len, payload(len)];
# kind 1 = parameter label: payload = [param, label bytes...] (CPnlLabelButton::
# SetCustomData reads 7 label chars); kind 0 = editor value (e.g. NoteSeq zoom).
def _records(data):
    out, i = [], 0
    while i < len(data):
        if i + 2 > len(data) or i + 2 + data[i + 1] > len(data):
            return None
        kind, ln = data[i], data[i + 1]
        payload = data[i + 2:i + 2 + ln]
        rec = {"kind": kind}
        if kind == 1 and ln >= 1:
            rec["param"] = payload[0]
            rec["label"] = s_latin1(payload[1:])
        else:
            rec["data"] = list(payload)
        out.append(rec)
        i += 2 + ln
    return out


def _unrecords(recs):
    b = bytearray()
    for rec in recs:
        if "label" in rec:
            payload = bytes([rec["param"]]) + b_latin1(rec["label"])
        else:
            payload = bytes(rec["data"])
        b += bytes([rec["kind"], len(payload)]) + payload
    return bytes(b)


def dec_custom_data(r, ctx):
    d = {"location": r.read(2), "modules": []}
    for _ in range(r.read(8)):
        idx = r.read(8)
        data = bytes(r.read(8) for _ in range(r.read(8)))
        recs = _records(data)
        m = {"index": idx}
        if recs is not None and _unrecords(recs) == data:
            m["records"] = recs
        else:
            m["bytes"] = data.hex()
        d["modules"].append(m)
    return d


def enc_custom_data(w, d, ctx):
    w.write(2, d["location"])
    w.write(8, len(d["modules"]))
    for m in d["modules"]:
        data = _unrecords(m["records"]) if "records" in m else bytes.fromhex(m["bytes"])
        w.write(8, m["index"])
        w.write(8, len(data))
        w.raw(data)


# $5A CModuleNameData_11 (ReadStream @00068b3e, WriteStream @000660ac)
def _get_string(r, maxlen):
    """CBitStream::GetString: bytes up to NUL (consumed) or maxlen."""
    b = bytearray()
    while len(b) < maxlen:
        c = r.u8()
        if c == 0:
            return bytes(b)
        b.append(c)
    return bytes(b)


def _put_string(w, s, maxlen):
    """CBitStream::PutString: the bytes, plus a NUL if shorter than maxlen."""
    if len(s) > maxlen or b"\0" in s:
        raise ValueError("bad string")
    w.raw(s)
    if len(s) < maxlen:
        w.u8(0)


def dec_module_names(r, ctx):
    d = {"location": r.read(2), "reserved": r.read(6), "names": []}
    for _ in range(r.read(8)):
        idx = r.u8()
        d["names"].append({"index": idx, "name": s_latin1(_get_string(r, 16))})
    return d


def enc_module_names(w, d, ctx):
    w.write(2, d["location"])
    w.write(6, d["reserved"])
    w.write(8, len(d["names"]))
    for n in d["names"]:
        w.u8(n["index"])
        _put_string(w, b_latin1(n["name"]), 16)


# $6F CTextpadData_11 (ReadStream @00068d80, WriteStream @00064632): raw text, no NUL
def dec_textpad(r, ctx):
    return {"text": s_latin1(bytes(r.u8() for _ in range(r.bits_left() // 8)))}


def enc_textpad(w, d, ctx):
    w.raw(b_latin1(d["text"]))


# $11 CPerformanceHeader_11 (ReadStream @00068e38, WriteStream @00064254)
SLOT_FIELDS = ["enabled", "keyboard", "hold", "bank", "program", "kbd_range_lower",
               "kbd_range_upper", "midi_channel", "reserved_1", "reserved_2"]


def dec_perf_header(r, ctx):
    d = {"unknown_08": r.u8(), "focused_slot": r.read(6), "global_pages": r.read(2),
         "kbd_range_enabled": r.u8(), "master_clock_bpm": r.u8(), "unknown_18": r.u8(),
         "master_clock_run": r.u8(), "reserved_1": r.u8(), "reserved_2": r.u8(), "slots": []}
    for _ in range(4):
        s = {"patch_name": s_latin1(_get_string(r, 16))}
        for f in SLOT_FIELDS:
            s[f] = r.u8()
        d["slots"].append(s)
    return d


def enc_perf_header(w, d, ctx):
    w.u8(d["unknown_08"])
    w.write(6, d["focused_slot"])
    w.write(2, d["global_pages"])
    for f in ("kbd_range_enabled", "master_clock_bpm", "unknown_18", "master_clock_run",
              "reserved_1", "reserved_2"):
        w.u8(d[f])
    if len(d["slots"]) != 4:
        raise ValueError("perf_header needs 4 slots")
    for s in d["slots"]:
        _put_string(w, b_latin1(s["patch_name"]), 16)
        for f in SLOT_FIELDS:
            w.u8(s[f])


SECTIONS = {
    0x21: ("patch_header", "CPatchHeaderData_11", dec_patch_header, enc_patch_header),
    0x4A: ("module_list", "CModuleData_11", dec_module_list, enc_module_list),
    0x69: ("current_note", "CCurrentVoiceData_11", dec_current_note, enc_current_note),
    0x52: ("cable_list", "CCableData_11", dec_cable_list, enc_cable_list),
    0x4D: ("param_list", "CModuleParamData_11", dec_param_list, enc_param_list),
    0x65: ("morph_map", "CMorphMapData_11", dec_morph_map, enc_morph_map),
    0x62: ("knob_map", "CKnobMapData_11", dec_knob_map, enc_knob_map),
    0x60: ("ctrl_map", "CCtrlMapData_11", dec_ctrl_map, enc_ctrl_map),
    0x5B: ("custom_data", "CModuleCustomData_11", dec_custom_data, enc_custom_data),
    0x5A: ("module_names", "CModuleNameData_11", dec_module_names, enc_module_names),
    0x6F: ("textpad", "CTextpadData_11", dec_textpad, enc_textpad),
    0x11: ("perf_header", "CPerformanceHeader_11", dec_perf_header, enc_perf_header),
    0x5F: ("global_knob_map", "CKnobMapData_11", dec_knob_map, enc_knob_map),
}
_META = ("id", "name", "class", "slot")


def encode_payload(sid: int, d: dict) -> bytes:
    if "raw" in d:
        return bytes.fromhex(d["raw"])
    w = BitWriter()
    SECTIONS[sid][3](w, d, {"sid": sid})
    w.align(d.get("_pad", 0))
    return w.getvalue() + bytes.fromhex(d.get("_trailing", ""))


def decode_payload(sid: int, payload: bytes) -> dict:
    if sid in SECTIONS:
        try:
            r = BitReader(payload)
            d = SECTIONS[sid][2](r, {"sid": sid})
            pad = r.align()
            if pad:
                d["_pad"] = pad
            rest = payload[r.pos // 8:]
            if rest:
                d["_trailing"] = rest.hex()
            if encode_payload(sid, d) == payload:
                return d
        except (ValueError, KeyError, IndexError, UnicodeError):
            pass
    return {"raw": payload.hex()}


# --------------------------------------------------------------------------
# File level
# --------------------------------------------------------------------------

def decode_file(data: bytes) -> dict:
    out = {}
    # Text header: CRLF-terminated lines (CFileReader_G2_1::CreateDataFile @00074ef8 reads 4).
    # The binary part starts with the 16-bit version word whose high byte is 0x00.
    p, lines = 0, []
    while p < len(data) and data[p] != 0:
        e = data.find(b"\r\n", p)
        if e < 0:
            break
        lines.append(s_latin1(data[p:e]))
        p = e + 2
    if p >= len(data) - 5 or data[p] != 0:
        return {"raw_file": data.hex()}
    out["text_header"] = lines
    body_start = p
    # CFileHeader_11::ReadStream @00051920: UWord version, UByte type (0 patch, 1 performance)
    out["version"] = (data[p] << 8) | data[p + 1]
    out["type"] = data[p + 2]
    p += 3
    end = len(data) - 2  # CRC is the final big-endian word
    sections, slot = [], -1
    while p < end:
        if p + 3 > end:
            break
        sid, ln = data[p], (data[p + 1] << 8) | data[p + 2]
        if p + 3 + ln > end:
            break
        d = {"id": "0x%02X" % sid}
        if sid in SECTIONS:
            d["name"], d["class"] = SECTIONS[sid][0], SECTIONS[sid][1]
        if out["type"] == 1:
            if sid == 0x21:
                slot += 1
            if sid not in (0x11, 0x5F):
                d["slot"] = slot
        d.update(decode_payload(sid, data[p + 3:p + 3 + ln]))
        sections.append(d)
        p += 3 + ln
    out["sections"] = sections
    if p < end:
        out["unparsed_tail"] = data[p:end].hex()
    stored = (data[end] << 8) | data[end + 1]
    out["crc"] = "0x%04X" % stored
    out["crc_valid"] = crc16(data[body_start:end]) == stored
    return out


def encode_file(j: dict) -> bytes:
    if "raw_file" in j:
        return bytes.fromhex(j["raw_file"])
    head = b"".join(b_latin1(l) + b"\r\n" for l in j["text_header"])
    body = bytearray([j["version"] >> 8, j["version"] & 0xFF, j["type"]])
    for d in j["sections"]:
        sid = int(d["id"], 16)
        payload = encode_payload(sid, {k: v for k, v in d.items() if k not in _META})
        if len(payload) > 0xFFFF:
            raise ValueError("section 0x%02X too long" % sid)
        body += bytes([sid, len(payload) >> 8, len(payload) & 0xFF]) + payload
    body += bytes.fromhex(j.get("unparsed_tail", ""))
    crc = crc16(bytes(body))
    if j.get("crc_valid") is False:
        crc = int(j["crc"], 16)  # keep a broken checksum as found
    return head + bytes(body) + bytes([crc >> 8, crc & 0xFF])


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _roundtrip(path: str) -> bool:
    data = open(path, "rb").read()
    j = decode_file(data)
    j2 = json.loads(to_json(j))  # through the dump's JSON text, as a user would
    ok = encode_file(j2) == data
    raw = [d["id"] for d in j.get("sections", []) if "raw" in d]
    notes = []
    if "raw_file" in j:
        notes.append("whole file kept raw")
    if raw:
        notes.append("raw sections: " + ",".join(raw))
    if j.get("crc_valid") is False:
        notes.append("stored CRC invalid")
    if "unparsed_tail" in j:
        notes.append("unparsed tail")
    print("%s %s%s" % ("OK  " if ok else "FAIL", path, (" (" + "; ".join(notes) + ")") if notes else ""))
    return ok


def _synth_perf(paths):
    """Build a .prf2 from up to 4 patches (for testing the performance path)."""
    pats = [decode_file(open(p, "rb").read()) for p in paths]
    while len(pats) < 4:
        pats.append(pats[-1])
    slots = []
    for i in range(4):
        slots.append({"patch_name": "Slot %c" % "ABCD"[i], "enabled": 1, "keyboard": int(i == 0),
                      "hold": 0, "bank": 0, "program": i, "kbd_range_lower": 0,
                      "kbd_range_upper": 127, "midi_channel": i, "reserved_1": 0, "reserved_2": 0})
    hdr = {"id": "0x11", "unknown_08": 0, "focused_slot": 0, "global_pages": 0,
           "kbd_range_enabled": 0, "master_clock_bpm": 120, "unknown_18": 0,
           "master_clock_run": 0, "reserved_1": 0, "reserved_2": 0, "slots": slots}
    secs = [hdr]
    for p in pats[:4]:
        secs += [s for s in p["sections"]]
    knobs = [None] * 120
    knobs[0] = {"location": 1, "module": 1, "is_button": 0, "param": 0, "slot": 2}
    secs.append({"id": "0x5F", "knobs": knobs})
    j = {"text_header": ["Version=Nord Modular G2 File Format 1", "Type=Performance",
                         "Version=23", "Info=BUILD 320"],
         "version": 23, "type": 1, "sections": secs}
    return encode_file(j)


def to_json(o, ind=0) -> str:
    """json.dumps(indent=1) but with scalar-only lists and small flat dicts on one line."""
    pad, pad1 = " " * ind, " " * (ind + 1)
    if isinstance(o, list):
        if all(not isinstance(x, (list, dict)) for x in o):
            return json.dumps(o)
        return "[\n" + ",\n".join(pad1 + to_json(x, ind + 1) for x in o) + "\n" + pad + "]"
    if isinstance(o, dict):
        flat = all(not isinstance(v, dict) and not (isinstance(v, list) and any(
            isinstance(x, (list, dict)) for x in v)) for v in o.values())
        if flat and len(json.dumps(o)) <= 160:
            return json.dumps(o)
        return "{\n" + ",\n".join(pad1 + json.dumps(k) + ": " + to_json(v, ind + 1)
                                  for k, v in o.items()) + "\n" + pad + "}"
    return json.dumps(o)


def main(argv):
    if len(argv) < 2 or argv[1] not in ("dump", "encode", "roundtrip", "selftest"):
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "dump" and len(argv) == 3:
        j = decode_file(open(argv[2], "rb").read())
        sys.stdout.write(to_json(j) + "\n")
        return 0
    if cmd == "encode" and len(argv) == 4:
        with open(argv[2]) as f:
            j = json.load(f)
        with open(argv[3], "wb") as f:
            f.write(encode_file(j))
        return 0
    if cmd == "roundtrip" and len(argv) >= 3:
        results = [_roundtrip(p) for p in argv[2:]]
        return 0 if all(results) else 1
    if cmd == "selftest" and len(argv) >= 3:
        ok = all(_roundtrip(p) for p in argv[2:])
        prf = _synth_perf(argv[2:6])
        j = decode_file(prf)
        good = (encode_file(json.loads(to_json(j))) == prf and j["crc_valid"]
                and not any("raw" in d for d in j["sections"])
                and [d.get("slot") for d in j["sections"] if d["id"] == "0x21"] == [0, 1, 2, 3])
        print("%s synthetic performance (%d bytes, %d sections)" % ("OK  " if good else "FAIL", len(prf),
                                                                   len(j["sections"])))
        return 0 if ok and good else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
