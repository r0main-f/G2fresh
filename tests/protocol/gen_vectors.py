#!/usr/bin/env python3
"""Generate the golden USB-protocol test vectors in tests/protocol/*.json.

Spec: re/notes/usb-protocol.md. Run with `python3 -I tests/protocol/gen_vectors.py`
from the repository root. The script is deterministic; re-running it must not
change any JSON file (a diff means the codec rules in this file changed).

Wire rules implemented here (see usb-protocol.md §3):
  host->device (bulk OUT)  : [len_hi len_lo] [0x01] [hdr] [sess] payload... [crc_hi crc_lo]
                             len = whole frame length incl. len and CRC bytes
                             crc = CRC-16/XMODEM over bytes[2 : len-2]
  version request          : [00 05] [80] [crc]  (crc over the single 0x80)
  device->host             : 16-byte interrupt packet, byte0 & 3:
                               2 = embedded: n = byte0 >> 4 bytes follow (incl. CRC)
                               1 = extended: bytes 1..2 = BE length of a bulk-IN transfer
                             message body = [0x01] [hdr] [sess] payload... [crc]  (or [0x80]... for version)
                             crc over body[:-2]
  hdr byte (out)           : bit5 0x20 first chunk of bubble, bit4 0x10 realtime,
                             bit3 0x08 last chunk of bubble, bits2..0 slot (0..3, 4 = synth/perf)
  hdr byte (in)            : bit3 0x08 = response to a request, bits2..0 slot
  sess byte                : 0x40 | molecule_count ("void session") or session number 0..0x3f
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def crc16(data, crc=0):
    """CRC-16/XMODEM (poly 0x1021, init 0, no reflection, no xorout) = Clavia CRC_Get16 @0x14fd40."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def hx(b):
    return " ".join("%02X" % x for x in b)


def out_frame(hdr, sess, payload):
    body = bytes([0x01, hdr, sess]) + bytes(payload)
    n = len(body) + 4
    c = crc16(body)
    return bytes([n >> 8, n & 0xFF]) + body + bytes([c >> 8, c & 0xFF])


def version_request():
    c = crc16(b"\x80")
    return bytes([0x00, 0x05, 0x80, c >> 8, c & 0xFF])


def in_body(hdr, sess, payload):
    body = bytes([0x01, hdr, sess]) + bytes(payload)
    c = crc16(body)
    return body + bytes([c >> 8, c & 0xFF])


def in_version_body(payload):
    body = bytes([0x80]) + bytes(payload)
    c = crc16(body)
    return body + bytes([c >> 8, c & 0xFF])


def embedded(body):
    """Interrupt packet carrying the whole message (len(body) <= 15)."""
    assert len(body) <= 15, len(body)
    pkt = bytes([(len(body) << 4) | 0x02]) + body
    return pkt + bytes(16 - len(pkt))


def extended_irq(body_len):
    """Interrupt packet announcing a bulk-IN transfer of body_len bytes.
    High nibble of byte 0 is unknown (Clavia and Verhue ignore it); we emit 0."""
    pkt = bytes([0x01, body_len >> 8, body_len & 0xFF])
    return pkt + bytes(16 - len(pkt))


def device_msg(body, note=""):
    """Return the step list for a device->host message: embedded if it fits."""
    if len(body) <= 15:
        return [step("device->host", "interrupt_in", embedded(body), "embedded message" + note)]
    return [
        step("device->host", "interrupt_in", extended_irq(len(body)), "extended announcement, len=%d" % len(body) + note),
        step("device->host", "bulk_in", body, "extended message body" + note),
    ]


def step(direction, endpoint, data, annotation, **extra):
    d = {"dir": direction, "endpoint": endpoint, "hex": hx(data), "annotation": annotation}
    d.update(extra)
    return d


def tx(hdr, sess, payload, annotation, **extra):
    return step("host->device", "bulk_out", out_frame(hdr, sess, payload), annotation, **extra)


def str16(s):
    b = s.encode("latin-1")[:16]
    return b if len(b) == 16 else b + b"\x00"


SYS_REQ = 0x2C      # first|last|slot4  (Verhue CMD_REQ+CMD_SYS)
def SLOT_REQ(s):    # first|last|slot s (Verhue CMD_REQ+CMD_SLOT+s)
    return 0x28 | s
def SLOT_RT(s):     # first|realtime|last|slot s (Verhue CMD_NO_RESP+CMD_SLOT+s)
    return 0x38 | s
VOID1 = 0x41        # void session, 1 molecule


def write(name, scenario):
    path = os.path.join(HERE, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scenario, f, indent=2, ensure_ascii=False)
        f.write("\n")


COMMON = {
    "spec": "re/notes/usb-protocol.md",
    "format": "Each step: dir (host->device | device->host), endpoint (bulk_out=EP 0x03, bulk_in=EP 0x82, "
              "interrupt_in=EP 0x81, 16-byte packets), hex (exact bytes of one USB transfer), annotation. "
              "provenance: 'verhue-capture' = hex dump found in Verhue's sources (probably captured from the "
              "official editor), 'clavia-derived' = built from the Clavia v1.62 serializers, 'verhue-derived' = "
              "built from Verhue's builders, 'synthetic' = device reply invented to match the decoded layout "
              "(field values arbitrary but legal).",
}


# --------------------------------------------------------------------------------------------
def framing():
    steps = [
        step("host->device", "bulk_out", version_request(),
             "Version request (CSynthPort::SendVersionRequest @0x10496c, CSynthPortUSB::BuildOutputMessage "
             "@0x10691a): len=5, 0x80, CRC over [80] = 0x9188", provenance="clavia-derived"),
        tx(SYS_REQ, VOID1, [0x35, 0x04],
           "Smallest normal bubble: 1 molecule (0x35 session-number request, slot 4). hdr 0x2C = first(0x20)|"
           "last(0x08)|slot 4; sess 0x41 = void|count 1", provenance="clavia-derived"),
        tx(SLOT_RT(0), 0x00, [0x40, 0x01, 0x01, 0x00, 0x40, 0x00],
           "Realtime message: hdr 0x38 = first|realtime|last|slot A; sess = slot session number (0 here)",
           provenance="clavia-derived"),
    ]
    steps += device_msg(in_body(0x04, 0x00, [0x80, 0x00, 0x3F]),
                        " -- Verhue capture (Gen2 BVE.NMG2Mess.pas:2124): 82 01 04 00 80 00 3F 30 40: n=8, "
                        "hdr 0x04 (slot 4, not a response), sess 0, molecule 0x80 MidiLearn slot 0 CC 63")
    steps[-1]["provenance"] = "verhue-capture"
    return {
        **COMMON,
        "scenario": "framing",
        "description": "Frame/CRC sanity vectors. CRC-16/XMODEM check values: '123456789' -> 0x31C3, "
                       "'1234' -> 0xD789 (the value asserted by Clavia CRC_UnitTest @0x14fe5e).",
        "crc_checks": [
            {"ascii": "123456789", "crc": "%04X" % crc16(b"123456789")},
            {"ascii": "1234", "crc": "%04X" % crc16(b"1234")},
            {"hex": "80", "crc": "%04X" % crc16(b"\x80")},
        ],
        "steps": steps,
    }


# --------------------------------------------------------------------------------------------
def version_reply_payload(model=0, mode=0, os_version=150, proto=0x12, w4=0x0000, serial=0x00000000, w32=0x0000):
    """Synthetic SVersionMessage (CSynthPort::CheckVersionMessage @0x104d2c, CSynthInfo ctor @0x1038e8).
    Returned bytes start after the leading 0x80."""
    p = bytearray(34)  # offsets relative to the 0x80 byte; index 0 = 0x80 (not included)
    full = bytearray(36)
    full[0] = 0x80
    full[1] = 0x0A
    full[2] = model
    full[3] = mode
    full[4:6] = w4.to_bytes(2, "big")
    full[6:8] = os_version.to_bytes(2, "big")
    full[8:10] = proto.to_bytes(2, "big")
    full[26:30] = serial.to_bytes(4, "big")
    full[32:34] = w32.to_bytes(2, "big")
    return bytes(full[1:])


def init_sequence():
    """Clavia v1.62 order after the handshake (usb-protocol.md §6.3), slot A shown, other slots elided."""
    s = []

    def reply(body, note):
        for st in device_msg(body, note):
            st["provenance"] = "synthetic"
            s.append(st)

    def req(hdr, sess, pl, ann):
        s.append(tx(hdr, sess, pl, ann, provenance="clavia-derived"))

    s.append(step("host->device", "bulk_out", version_request(),
                  "1. Version request (CSynthPort::CleanDirtyData @0x104b7c -> SendVersionRequest @0x10496c). "
                  "Without a reply within 10 s the port goes to status 2 'Still looking...' and the request is "
                  "re-sent; forever.", provenance="clavia-derived"))
    reply(in_version_body(version_reply_payload(model=0, mode=0, os_version=150, proto=0x12)),
          " -- version reply: 80 0A, model 00 (G2), mode 00 (normal), [4..5] unknown, [6..7] 0x0096 = OS 1.50 "
          "(shown 'V1.50'), [8..9] 0x0012 (required), [26..29] unknown BE32, [32..33] unknown. Total length of the "
          "real reply is unknown (>= 34 bytes + CRC); 36 used here")
    ACK_SYS = in_body(0x0C, VOID1, [0x7F])
    req(SYS_REQ, VOID1, [0x7D, 0x01], "2. CMSynthUnlockEditorSync(true): editor sync starts (Verhue 'stop comm')")
    reply(ACK_SYS, " -- response (0x7F ack assumed)")
    req(SYS_REQ, VOID1, [0x35, 0x04], "3. CMSessionNumberRequest slot 4 = performance session")
    reply(in_body(0x0C, VOID1, [0x36, 0x04, 0x05]), " -- 0x36 CMSessionNumberDump slot 4 session 5")
    req(SYS_REQ, VOID1, [0x02], "4. CMSynthDataRequest")
    reply(in_body(0x0C, VOID1, [0x03] + list(str16("G2 Engine")) +
                  # sort modes (2), focus bank, prog, memory protect, MIDI channels A-D, global, sysex id,
                  # local on, program change / controller modes, clock bits, tune, octave shift enable,
                  # octave shift, transpose, vibrato rate?, pedal bits, pedal gain (CSynthMap::WriteStream
                  # @0x120410: 38 bytes after the name)
                  [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x0F, 0x10, 0x80, 0x01, 0x01, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x40, 0x00] + [0x00] * 16),
          " -- 0x03 CMSynthDataDump (CSynthMap): name, flags, bank/prog, protect, MIDI ch A-D 0..3, global 15, "
          "sysex id 16, local on, ... 16 reserved zero bytes. Field values illustrative")
    req(SYS_REQ, VOID1, [0x81], "5. CMMidiLearnRequest")
    reply(in_body(0x0C, VOID1, [0x80, 0x00, 0x00]), " -- 0x80 CMMidiLearn slot 0 cc 0")
    req(SYS_REQ, 0x05, [0x10], "6. CMPerformanceHeaderRequest (perf bubble: sess = perf session 5)")
    req(SYS_REQ, 0x05, [0x59], "7. CMGlobalParameterPageFocusRequest -> reply 0x1E page")
    req(SYS_REQ, 0x05, [0x08], "8. CMSlotFocusRequest (only the lowest-numbered connected port) -> reply 0x09")
    req(SYS_REQ, VOID1, [0x35, 0x00], "9a. slot A: CMSessionNumberRequest (system bubble, slot in payload)")
    reply(in_body(0x0C, VOID1, [0x36, 0x00, 0x11]), " -- slot A session = 0x11")
    req(SLOT_REQ(0), 0x11, [0x3C], "9b. CMCompletePatchRequest (hdr 0x28 slot A, sess 0x11) -> patch sections")
    req(SLOT_REQ(0), 0x11, [0x58], "9c. CMParameterPageFocusRequest -> 0x2D")
    req(SLOT_REQ(0), 0x11, [0x68], "9e. CMCurrentNotesRequest -> 0x69 section")
    req(SLOT_REQ(0), 0x11, [0x6E], "9f. CMTextpadRequest -> 0x6F section")
    req(SLOT_REQ(0), 0x11, [0x71, 0x01], "9h. CMPatchLoadRequest VA -> 0x72")
    req(SLOT_REQ(0), 0x11, [0x71, 0x00], "9h. CMPatchLoadRequest FX -> 0x72 (LED data accepted from now on)")
    req(SLOT_REQ(0), 0x11, [0x70], "9i. CMFlushBlink")
    req(SLOT_REQ(0), 0x11, [0x2E], "9j. CMParamFocusRequest -> 0x2F")
    s.append({"dir": "note", "annotation": "steps 9a-9j repeat for slots B, C, D"})
    req(SYS_REQ, VOID1, [0x04], "10. CMNumberOfVoicesRequest -> 0x05 v0..v3")
    req(SYS_REQ, VOID1, [0x3B], "11. CMClockInfoRequest -> 0x5D")
    req(SYS_REQ, 0x05, [0x5E], "12. CMPerfKnobMapRequest (perf session) -> 0x5F section")
    req(SYS_REQ, VOID1, [0x14, 0x00, 0x00, 0x00], "13. CMFlashDataRequest patch bank 0 prog 0 (repeated with the "
        "cursor from each 0x16 reply, then for performances)")
    req(SYS_REQ, VOID1, [0x7D, 0x00], "14. CMSynthUnlockEditorSync(false): sync done (Verhue 'start comm')")
    reply(ACK_SYS, " -- response")
    return {
        **COMMON,
        "scenario": "init_sequence",
        "description": "Connection sequence of the Clavia editor (typical 'read everything from the synth' case). "
                       "Request frames are exact; device replies are synthetic and only shown where small. Every "
                       "normal bubble is answered (response flag 0x08 in reply hdr) before the next is sent.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def verhue_init_frames():
    rows = [
        ("Init", version_request()),
        ("Stop comm 7D 01", out_frame(SYS_REQ, VOID1, [0x7D, 0x01])),
        ("Get perf version 35 04", out_frame(SYS_REQ, VOID1, [0x35, 0x04])),
        ("Get synth settings 02", out_frame(SYS_REQ, VOID1, [0x02])),
        ("Unknown1 81 (= Clavia MidiLearnRequest)", out_frame(SYS_REQ, VOID1, [0x81])),
        ("Get perf settings 10 (PV=0)", out_frame(SYS_REQ, 0x00, [0x10])),
        ("Unknown2 59 (= Clavia GlobalParameterPageFocusRequest) PV=0", out_frame(SYS_REQ, 0x00, [0x59])),
        ("Get slot A version 35 00", out_frame(SYS_REQ, VOID1, [0x35, 0x00])),
        ("Get patch slot A 3C", out_frame(SLOT_REQ(0), 0x00, [0x3C])),
        ("Get patch name slot A 28", out_frame(SLOT_REQ(0), 0x00, [0x28])),
        ("Get current note slot A 68", out_frame(SLOT_REQ(0), 0x00, [0x68])),
        ("Get patch text slot A 6E", out_frame(SLOT_REQ(0), 0x00, [0x6E])),
        ("Get resources VA slot A 71 01", out_frame(SLOT_REQ(0), 0x00, [0x71, 0x01])),
        ("Get resources FX slot A 71 00", out_frame(SLOT_REQ(0), 0x00, [0x71, 0x00])),
        ("Unknown6 70 (= Clavia FlushBlink)", out_frame(SLOT_REQ(0), 0x00, [0x70])),
        ("Get selected param 2E", out_frame(SLOT_REQ(0), 0x00, [0x2E])),
        ("Get assigned voices 04", out_frame(SYS_REQ, VOID1, [0x04])),
        ("Get global knobs 5E (PV=0)", out_frame(SYS_REQ, 0x00, [0x5E])),
        ("Get master clock 3B", out_frame(SYS_REQ, VOID1, [0x3B])),
        ("List names 14 00 00 00", out_frame(SYS_REQ, VOID1, [0x14, 0x00, 0x00, 0x00])),
        ("Start comm 7D 00", out_frame(SYS_REQ, VOID1, [0x7D, 0x00])),
    ]
    expected = {  # values printed in Verhue analysis (scratch), re-derived here as a cross-check
        "Init": "00 05 80 91 88",
        "Stop comm 7D 01": "00 09 01 2C 41 7D 01 96 94",
        "Get perf version 35 04": "00 09 01 2C 41 35 04 42 54",
        "Get synth settings 02": "00 08 01 2C 41 02 9B AC",
        "Unknown1 81 (= Clavia MidiLearnRequest)": "00 08 01 2C 41 81 3A 47",
        "Get assigned voices 04": "00 08 01 2C 41 04 FB 6A",
        "Get master clock 3B": "00 08 01 2C 41 3B 3C D6",
        "List names 14 00 00 00": "00 0B 01 2C 41 14 00 00 00 EC E5",
        "Start comm 7D 00": "00 09 01 2C 41 7D 00 86 B5",
    }
    steps = []
    for name, b in rows:
        if name in expected:
            assert hx(b) == expected[name], (name, hx(b))
        steps.append(step("host->device", "bulk_out", b, name, provenance="verhue-derived"))
    return {
        **COMMON,
        "scenario": "init_sequence_verhue",
        "description": "Request frames of Verhue's (Gen2) init sequence in order, slot A only, all version bytes 0. "
                       "Each must be answered before the next one is sent.",
        "steps": steps,
    }


# --------------------------------------------------------------------------------------------
def param_changes():
    s = []
    s.append(tx(SLOT_RT(0), 0x00, [0x40, 0x01, 0x01, 0x00, 0x40, 0x00],
                "CMParamChange (realtime) slot A: loc 1 (VA) module 1 param 0 value 64 variation 0. No reply.",
                provenance="clavia-derived", expect="00 0D 01 38 00 40 01 01 00 40 00 08 1C"))
    s.append(tx(SLOT_RT(0), 0x00, [0x2F, 0x00, 0x01, 0x01, 0x00],
                "CMParamFocusDump (realtime): flag 0, loc VA, module 1, param 0 (select parameter)",
                provenance="clavia-derived", expect="00 0C 01 38 00 2F 00 01 01 00 E4 BF"))
    s.append(tx(SLOT_RT(0), 0x00, [0x43, 0x01, 0x01, 0x00, 0x00, 0x2B, 0x00, 0x00],
                "CMMorphChange realtime (while dragging): loc VA, module 1, param 0, morph 0, |range| 0x2B, "
                "positive, variation 0", provenance="clavia-derived", expect="00 0F 01 38 00 43 01 01 00 00 2B 00 00 8A CA"))
    s.append(tx(SLOT_REQ(2), 0x07, [0x43, 0x00, 0x05, 0x02, 0x03, 0x10, 0x01, 0x02],
                "CMMorphChange normal (mouse-up) slot C sess 7: FX module 5 param 2 morph 3 range -16 variation 2",
                provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(1), 0x03, [0x6A, 0x02],
                "CMParamSettingFocus slot B: select variation 3 (index 2)", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x44, 0x00, 0x08],
                "CMParamSettingCopy slot A: copy variation 0 -> 8 (Verhue capture 'copy variation 1 -> init')",
                provenance="verhue-capture", expect="00 0A 01 28 00 44 00 08 0F 5C"))
    for st in device_msg(in_body(0x01, 0x03, [0x40, 0x01, 0x04, 0x02, 0x7F, 0x00]),
                         " -- unsolicited front-panel param change slot B sess 3: VA module 4 param 2 value 127 var 0"):
        st["provenance"] = "synthetic"
        s.append(st)
    for st in device_msg(in_body(0x00, 0x00, [0x2F, 0x01, 0x01, 0x03, 0x05]),
                         " -- unsolicited param focus from panel: slot A, VA module 3 param 5"):
        st["provenance"] = "synthetic"
        s.append(st)
    for x in s:
        if "expect" in x:
            assert x["hex"] == x["expect"], (x["annotation"], x["hex"], x["expect"])
    return {
        **COMMON,
        "scenario": "param_changes",
        "description": "Live parameter, morph and variation messages.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def patch_edits():
    s = []
    s.append(tx(SLOT_REQ(0), 0x00, [0x44, 0x08, 0x01], "Init variation 2 (copy 8 -> 1)",
                provenance="verhue-capture", expect="00 0A 01 28 00 44 08 01 17 DC"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x6F, 0x00, 0x01, 0x68], "Patch notes section 0x6F len 1 'h'",
                provenance="verhue-capture", expect="00 0B 01 28 00 6F 00 01 68 D3 88"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x6F, 0x00, 0x02, 0x68, 0x61], "Patch notes 'ha'",
                provenance="verhue-capture", expect="00 0C 01 28 00 6F 00 02 68 61 56 C9"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x6F, 0x00, 0x03, 0x68, 0x61, 0x6C], "Patch notes 'hal'",
                provenance="verhue-capture", expect="00 0D 01 28 00 6F 00 03 68 61 6C 28 AD"))
    s.append(tx(SLOT_REQ(0), 0x00, bytes.fromhex("4D000F404040458 0D980040000000002020 0".replace(" ", "")),
                "Param paste: one 0x4D section (len 15) VA, 1 module, 1 variation", provenance="verhue-capture",
                expect="00 19 01 28 00 4D 00 0F 40 40 40 45 80 D9 80 04 00 00 00 00 02 02 00 53 D3"))
    s.append(tx(SLOT_REQ(0), 0x00, bytes.fromhex(
        "4301030000 2B0000 4301030600 1E0000 4301030800 040000 4D000F404040C580554A040000000002 0200".replace(" ", "")),
        "Param paste with morphs: 3 x 0x43 + one 0x4D section in a single bubble. NOTE: Verhue sends sess byte 0x00 "
        "(slot session) although the bubble has 4 molecules; Clavia would also send the slot session here",
        provenance="verhue-capture",
        expect="00 31 01 28 00 43 01 03 00 00 2B 00 00 43 01 03 06 00 1E 00 00 43 01 03 08 00 04 00 00 4D 00 0F 40 40 "
               "40 C5 80 55 4A 04 00 00 00 00 02 02 00 91 00"))
    # Clavia-derived edit molecules
    s.append(tx(SLOT_REQ(0), 0x00, [0x34, 0x01, 0x02, 0x03, 0x04],
                "CMModuleMove: VA module 2 -> column 3 row 4", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x50, 0x10 | (1 << 3) | 0, 0x02, 0x40 | 0x00, 0x03, 0x01],
                "CMCableConnect: flags 0x18 (not-dump=1, VA=1, color 0 red) from module 2 output 0 (0x40|0) to "
                "module 3 input 1", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x51, 0x02 | 1, 0x02, 0x40, 0x03, 0x01],
                "CMCableDelete: flags 0x03 (not-dump, VA) same cable", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x25, 0x02, 0x00, (1 << 6), 0x00, 0x05],
                "CMKnobAssign: module 2 param 0, loc VA (b2=1 -> 0x40), assignType 0, knob 5 (u16be)",
                provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x26, 0x00, 0x05], "CMKnobDeassign knob 5", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x22, 0x01, 0x02, 0x00, 0x07],
                "CMCtrlAssign: VA module 2 param 0 -> MIDI CC 7", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x23, 0x07], "CMCtrlDeassign CC 7", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x33, 0x01, 0x02] + list(str16("Osc1")),
                "CMModuleName: VA module 2 'Osc1' (str16, NUL-terminated because < 16 chars)", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x27] + list(str16("SixteenCharsName")),
                "CMPatchNameDump: name of exactly 16 chars -> no NUL", provenance="clavia-derived"))
    for st in device_msg(in_body(0x08, 0x00, [0x7F]), " -- generic ack (response bit 0x08, slot A)"):
        st["provenance"] = "synthetic"
        s.append(st)
    for x in s:
        if "expect" in x:
            assert x["hex"] == x["expect"], (x["annotation"], x["hex"], x["expect"])
    return {
        **COMMON,
        "scenario": "patch_edits",
        "description": "Patch-edit bubbles. Each needs a reply (0x7F ack or a dump) before the next normal bubble.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def led_vu():
    s = []
    # 0x39: start index 0, 40 LEDs x 2 bits, LED0 in bits 1..0 of first data byte
    leds = [0] * 40
    leds[0] = 1
    leds[1] = 2
    leds[5] = 3
    data = bytearray(10)
    for i, v in enumerate(leds):
        data[i // 4] |= (v & 3) << (2 * (i % 4))
    body = in_body(0x00, 0x11, [0x39, 0x00] + list(data))
    for st in device_msg(body, " -- 0x39 dual-bit LED blink, slot A sess 0x11, start 0; LED0=1 LED1=2 LED5=3"):
        st["provenance"] = "synthetic"
        s.append(st)
    words = [0x0000] * 4
    words[0] = 0x0010
    words[1] = 0x0203
    pl = [0x3A, 0x00]
    for w in words:
        pl += [w >> 8, w & 0xFF]
    body = in_body(0x01, 0x03, pl)
    for st in device_msg(body, " -- 0x3A multi-bit (VU/strip) data, slot B sess 3, start 0, 4 entries of 2 bytes. "
                               "Byte order of each entry UNCERTAIN (Clavia reads a raw i386 ushort = little-endian; "
                               "Verhue uses the 2nd byte as the value)"):
        st["provenance"] = "synthetic"
        s.append(st)
    s.append(tx(SLOT_REQ(0), 0x11, [0x70], "CMFlushBlink slot A", provenance="clavia-derived"))
    return {
        **COMMON,
        "scenario": "led_vu",
        "description": "Unsolicited LED / meter data. Not responses (hdr bit 0x08 clear); never acknowledged.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def sessions():
    s = []
    s.append(tx(SYS_REQ, VOID1, [0x35, 0x02], "Session request slot C", provenance="clavia-derived"))
    for st in device_msg(in_body(0x0C, VOID1, [0x36, 0x02, 0x21]), " -- 0x36 slot 2 session 0x21"):
        st["provenance"] = "synthetic"
        s.append(st)
    for st in device_msg(in_body(0x04, 0x40, [0x38, 0x01, 0x05]),
                         " -- unsolicited 0x38 patch release: slot B now has session 5 (patch changed on synth, "
                         "editor must re-read slot B)"):
        st["provenance"] = "synthetic"
        s.append(st)
    for st in device_msg(in_body(0x04, 0x40, [0x1F, 0x06]),
                         " -- unsolicited 0x1F performance release: new perf session 6 (whole performance re-sync)"):
        st["provenance"] = "synthetic"
        s.append(st)
    for st in device_msg(in_body(0x0C, 0x40, [0x1F, 0x06, 0x36, 0x00, 0x01, 0x36, 0x01, 0x02, 0x36, 0x02, 0x03,
                                               0x36, 0x03, 0x04]),
                         " -- Verhue-described compound form '1F PV {36 slot ver}x4' (Gen2 Mess:1614)"):
        st["provenance"] = "synthetic"
        s.append(st)
    return {
        **COMMON,
        "scenario": "session_numbers",
        "description": "Per-slot session ('patch version') counters. Slot bubbles carry the slot session in byte 4; "
                       "a mismatch makes the synth/editor discard the bubble.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def midi_and_notes():
    s = []
    s.append(tx(SYS_REQ, VOID1, [0x56, 0x00, 0x3C], "CMPlayNote note-on middle C (60). No velocity/channel field; "
                "plays on the keyboard-focused slot(s) (inferred)", provenance="clavia-derived",
                expect="00 0A 01 2C 41 56 00 3C C5 A6"))
    s.append(tx(SYS_REQ, VOID1, [0x56, 0x01, 0x3C], "CMPlayNote note-off 60", provenance="clavia-derived"))
    s.append(tx(SLOT_REQ(0), 0x00, [0x55], "CMSendCtrlSnap: synth sends values of assigned MIDI CCs (out its MIDI port)",
                provenance="clavia-derived"))
    s.append(tx(SYS_REQ, VOID1, [0x81], "CMMidiLearnRequest", provenance="clavia-derived"))
    for st in device_msg(in_body(0x04, 0x00, [0x80, 0x00, 0x3F]),
                         " -- 0x80 CMMidiLearn slot 0 CC 63 (Verhue capture 82 01 04 00 80 00 3F 30 40)"):
        st["provenance"] = "verhue-capture"
        s.append(st)
    for x in s:
        if "expect" in x:
            assert x["hex"] == x["expect"], (x["annotation"], x["hex"], x["expect"])
    return {
        **COMMON,
        "scenario": "midi_and_notes",
        "description": "Everything MIDI-like the USB protocol can carry (see usb-protocol.md §12): note on/off "
                       "without velocity, controller snapshot trigger, MIDI-learn reporting.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def errors_and_acks():
    s = []
    for st in device_msg(in_body(0x0C, VOID1, [0x7F]), " -- 0x7F kMesAck: end of reply, no data"):
        st["provenance"] = "synthetic"
        s.append(st)
    for st in device_msg(in_body(0x08 | 2, 0x07, [0x7E, 0x03]),
                         " -- 0x7E kMesException code 3 for slot C (Clavia throws XMHostMidi(3) -> major error)"):
        st["provenance"] = "synthetic"
        s.append(st)
    bad = bytearray(in_body(0x0C, VOID1, [0x7F]))
    bad[-1] ^= 0xFF
    s.append(step("device->host", "interrupt_in", embedded(bytes(bad)),
                  "Same ack with corrupted CRC: Clavia silently drops it (CSynthPortUSB::InterruptHandleData "
                  "@0x106f72); Verhue only logs", provenance="synthetic", valid=False))
    return {
        **COMMON,
        "scenario": "errors_and_acks",
        "description": "Acknowledge / exception terminators and a CRC failure.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def bank_ops():
    s = []
    s.append(tx(SYS_REQ, VOID1, [0x14, 0x00, 0x00, 0x00], "CMFlashDataRequest: list patch names from bank 0 prog 0",
                provenance="clavia-derived", expect="00 0B 01 2C 41 14 00 00 00 EC E5"))
    names = [0x16, 0x01, 0x00, 0x03, 0x00, 0x00] + list(str16("Init")) + [0x00] + [0x02] + list(str16("Bass 1")) + [0x03, 0x05]
    for st in device_msg(in_body(0x0C, VOID1, names),
                         " -- 0x16 CMFlashDataDump: flag 1, type 0 (patch), tag 03 bank 0 prog 0, 'Init' cat 0, "
                         "tag 02 (empty prog 1), 'Bass 1' cat 3, tag 05 (chunk end: request again)"):
        st["provenance"] = "synthetic"
        s.append(st)
    s.append(tx(SYS_REQ, VOID1, [0x0A, 0x00, 0x01, 0x02], "CMFlashLoadCommand: load bank 1 prog 2 into slot A",
                provenance="clavia-derived"))
    s.append(tx(SYS_REQ, VOID1, [0x0B, 0x04, 0x00, 0x07], "CMFlashStoreCommand: store performance (slot 4) to bank 0 prog 7",
                provenance="clavia-derived"))
    for st in device_msg(in_body(0x0C, VOID1, [0x0D, 0x01, 0x00, 0x07, 0x00, 0x00]),
                         " -- 0x0D CMFlashCommandResult type 1 bank 0 prog 7 origin 0 result 0 (ok)"):
        st["provenance"] = "synthetic"
        s.append(st)
    s.append(tx(SYS_REQ, VOID1, [0x0C, 0x00, 0x02, 0x10, 0x00], "CMFlashDeleteCommand patch bank 2 prog 16 origin 0",
                provenance="clavia-derived"))
    for x in s:
        if "expect" in x:
            assert x["hex"] == x["expect"], (x["annotation"], x["hex"], x["expect"])
    return {
        **COMMON,
        "scenario": "bank_ops",
        "description": "Flash (bank) list, load, store, delete.",
        "steps": s,
    }


# --------------------------------------------------------------------------------------------
def patch_upload_header():
    """Upload bubble skeleton: molecule count byte proves the void-session encoding."""
    s = []
    hdr = [0x37, 0x00, 0x00, 0x00] + list(str16("MyPatch"))
    s.append({
        "dir": "host->device", "endpoint": "bulk_out", "hex": None,
        "annotation": "Patch upload into slot A = one bubble: hdr 0x28, sess 0x53 (void | 19 molecules: 37, 21, 4A x2, "
                      "69, 52 x2, 4D x3, 65, 62, 60, 5B x3, 5A x2, 6F), payload = '37 00 00 00 <str16 name>' followed "
                      "by the 18 .pch2 sections (each: id, u16be length, body; 4D sections carry 10 variations over USB). "
                      "Verhue hard-codes the 0x53 byte. Frame bytes depend on the patch, so only the prefix is given.",
        "prefix_hex": hx(bytes([0x01, 0x28, 0x53]) + bytes(hdr)),
        "provenance": "clavia-derived",
    })
    s.append({
        "dir": "host->device", "endpoint": "bulk_out", "hex": None,
        "annotation": "Performance upload: hdr 0x2C, sess 0x42 (void | 2 molecules: 37 + 1A). 1A wraps 29 name, 11 "
                      "section, 4 x patch sections, 5F section.",
        "prefix_hex": hx(bytes([0x01, 0x2C, 0x42]) + bytes(hdr)),
        "provenance": "clavia-derived",
    })
    s.append(tx(SLOT_REQ(0), 0x11, [0x3C], "Download: CMCompletePatchRequest slot A. Reply (hdr 0x08, sess 0x11) "
                "= 21 ... 6F sections (Verhue: optionally '2D 00' after 21)", provenance="clavia-derived"))
    return {
        **COMMON,
        "scenario": "patch_transfer",
        "description": "Patch / performance upload & download framing.",
        "steps": s,
    }


def main():
    files = {
        "framing.json": framing(),
        "init_sequence.json": init_sequence(),
        "init_sequence_verhue.json": verhue_init_frames(),
        "param_changes.json": param_changes(),
        "patch_edits.json": patch_edits(),
        "led_vu.json": led_vu(),
        "session_numbers.json": sessions(),
        "midi_and_notes.json": midi_and_notes(),
        "errors_and_acks.json": errors_and_acks(),
        "bank_ops.json": bank_ops(),
        "patch_transfer.json": patch_upload_header(),
    }
    for name, sc in files.items():
        write(name, sc)
    print("wrote %d files" % len(files))


if __name__ == "__main__":
    main()
