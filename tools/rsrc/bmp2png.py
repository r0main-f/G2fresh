#!/usr/bin/env python3
"""Converts the editor's uncompressed 24-bit BMP resources to PNG (stdlib only).

    bmp2png.py <in.bmp|in.bin> <out.png>

Handles the Windows (40-byte) and OS/2 v2 (64-byte) info headers; both
store bottom-up BGR rows padded to 4 bytes.
"""
import struct
import sys
import zlib


def bmp_to_rgb(data: bytes) -> tuple[int, int, bytes]:
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    pixel_off = struct.unpack("<I", data[10:14])[0]
    hdr_size = struct.unpack("<I", data[14:18])[0]
    if hdr_size not in (40, 64, 108, 124):
        raise ValueError(f"unsupported BMP header size {hdr_size}")
    width, height = struct.unpack("<ii", data[18:26])
    bpp, compression = struct.unpack("<HI", data[28:34])
    if bpp != 24 or compression != 0:
        raise ValueError(f"unsupported BMP: {bpp} bpp, compression {compression}")
    top_down = height < 0
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    rows = []
    for y in range(height):
        src_y = y if top_down else height - 1 - y
        row = data[pixel_off + src_y * stride: pixel_off + src_y * stride + width * 3]
        rgb = bytearray(len(row))
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::3], row[1::3], row[0::3]
        rows.append(bytes(rgb))
    return width, height, b"".join(b"\x00" + r for r in rows)


def write_png(path: str, width: int, height: int, filtered_rgb: bytes) -> None:
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(filtered_rgb, 9)) + chunk(b"IEND", b""))


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        w, h, rgb = bmp_to_rgb(f.read())
    write_png(argv[2], w, h, rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
