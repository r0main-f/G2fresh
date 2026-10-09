#!/usr/bin/env python3
"""Unpack the Nord Modular G2 synth OS from the user's own copy of Clavia's updater.

The Mac "Nord Modular G2 OS Update.app" (in the "Nord Modular G2 OS v1.62 Update" DMG)
keeps the synth firmware in its resource file
`Contents/Resources/Nord Modular G2 Updater.rsrc`:

  NMG2 128 "OS"      the OS image, sent to the synth over USB in update mode
  BOOT 128 "Loader"  the boot-flash image (ColdFire code linked at address 0)

OS image layout (all big-endian; verified against the updater's SwapAndCheckOSdata
@0x5e38 and the boot loader's checks @0x8d86/@0x8ddc/@0x9074, see
re/notes/g2-hardware-and-emulation.md):

  0x000 u16  version (162 = 1.62)
  0x002 u8   format, must be 1
  0x004 u16  (0x0140 in 1.62; sent to the synth with each section header)
  0x006 u16  ~sum(bytes 0x008..0x2d3) & 0xffff
  0x013 u8   section count
  0x014      sections, 0x2c bytes each:
             +0x00 char[4] name ("SRAM", "CODE")    +0x04 u32 file offset
             +0x08 u32 unpacked size                +0x0c u32 load address
             +0x10 u32 ~sum(unpacked bytes)          +0x14 u32 packed size (0 = stored)
             +0x18 u32 ~sum(packed bytes)
  0x2d4      section data, LZO1X compressed

The boot loader jumps to the load address of the section named "CODE".

Nothing from Clavia is stored in this script. Output goes where you tell it; keep it
out of the repository (original/ is gitignored).

Usage:
  g2os.py unpack <Nord Modular G2 Updater.rsrc> <outdir>
      writes <outdir>/NMG2_OS.bin, BOOT_Loader.bin, <name>_<addr>.bin per section
  g2os.py info <NMG2_OS.bin>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "rsrc"))
import rsrc  # noqa: E402


def lzo1x_decompress(src: bytes) -> bytes:
    """LZO1X decompression, same semantics as lzo1x_decompress() (and the G2 boot loader @0x6bf0)."""
    out = bytearray()
    ip = 0

    def run_length(base):
        nonlocal ip
        n = 0
        while src[ip] == 0:
            n += 255
            ip += 1
        n += base + src[ip]
        ip += 1
        return n

    def copy_match(dist, length):
        p = len(out) - dist
        if p < 0:
            raise ValueError("LZO: match before start of output")
        for k in range(length):
            out.append(out[p + k])

    state = 0  # literals copied after the last match: 0..3, or 4 after a literal run
    t = src[ip]
    if t > 17:
        ip += 1
        t -= 17
        out += src[ip:ip + t]
        ip += t
        state = 4 if t >= 4 else t
    while True:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:
                if t == 0:
                    t = run_length(15)
                t += 3
                out += src[ip:ip + t]
                ip += t
                state = 4
                continue
            if state < 4:
                copy_match(1 + (t >> 2) + (src[ip] << 2), 2)
            else:
                copy_match(1 + 0x800 + (t >> 2) + (src[ip] << 2), 3)
            ip += 1
            state = src[ip - 2] & 3
        elif t >= 64:
            copy_match(1 + ((t >> 2) & 7) + (src[ip] << 3), (t >> 5) + 1)
            ip += 1
            state = src[ip - 2] & 3
        elif t >= 32:
            length = t & 31
            if length == 0:
                length = run_length(31)
            d = src[ip] | (src[ip + 1] << 8)
            ip += 2
            copy_match(1 + (d >> 2), length + 2)
            state = d & 3
        else:  # 16..31: far match, or end of stream
            length = t & 7
            if length == 0:
                length = run_length(7)
            d = src[ip] | (src[ip + 1] << 8)
            ip += 2
            dist = ((t & 8) << 11) + (d >> 2)
            if dist == 0:
                if ip != len(src):
                    raise ValueError(f"LZO: {len(src) - ip} bytes after end marker")
                return bytes(out)
            copy_match(dist + 0x4000, length + 2)
            state = d & 3
        if state:
            out += src[ip:ip + state]
            ip += state


def parse_os(blob: bytes):
    version, fmt, _pad, w4, hsum = struct.unpack(">HBBHH", blob[:8])
    if fmt != 1:
        raise ValueError(f"OS header format {fmt}, expected 1")
    if (~sum(blob[8:8 + 0x2cc])) & 0xFFFF != hsum:
        raise ValueError("OS header checksum mismatch")
    sections = []
    for i in range(blob[0x13]):
        e = blob[0x14 + 0x2C * i:0x14 + 0x2C * (i + 1)]
        name = e[:4].decode("ascii")
        off, usize, addr, usum, csize, csum = struct.unpack(">IIIIII", e[4:28])
        raw = blob[off:off + (csize or usize)]
        if (~sum(raw)) & 0xFFFFFFFF != (csum if csize else usum):
            raise ValueError(f"{name}: stored checksum mismatch")
        data = lzo1x_decompress(raw) if csize else raw
        if len(data) != usize or (~sum(data)) & 0xFFFFFFFF != usum:
            raise ValueError(f"{name}: unpacked size/checksum mismatch")
        sections.append(dict(name=name, offset=off, size=usize, packed=csize, addr=addr, data=data))
    return dict(version=version, word4=w4, sections=sections)


def info(blob: bytes) -> None:
    os_ = parse_os(blob)
    v = os_["version"]
    print(f"OS version {v // 100}.{v % 100:02d}, word4=0x{os_['word4']:04x}, {len(blob)} bytes, checksums OK")
    for s in os_["sections"]:
        print(f"  {s['name']}: file 0x{s['offset']:x}, {s['packed']} packed -> {s['size']} bytes at 0x{s['addr']:08x}")


def main(argv):
    if len(argv) == 4 and argv[1] == "unpack":
        res = {(r.type, r.id): r.data for r in rsrc.parse(open(argv[2], "rb").read())}
        os.makedirs(argv[3], exist_ok=True)
        blob = res[("NMG2", 128)]
        open(os.path.join(argv[3], "NMG2_OS.bin"), "wb").write(blob)
        if ("BOOT", 128) in res:
            open(os.path.join(argv[3], "BOOT_Loader.bin"), "wb").write(res[("BOOT", 128)])
        for s in parse_os(blob)["sections"]:
            open(os.path.join(argv[3], f"{s['name']}_{s['addr']:08x}.bin"), "wb").write(s["data"])
        info(blob)
        return 0
    if len(argv) == 3 and argv[1] == "info":
        info(open(argv[2], "rb").read())
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
