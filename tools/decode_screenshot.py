#!/usr/bin/env python3
"""Decode a TabDOS serial screenshot dump into a BMP image (no dependencies).

The firmware dumps the current PC frame over the serial console when you hold a
finger in the top-right corner of the panel for ~1.5 s. The dump looks like:

    === TABDOS SCREENSHOT BEGIN w=720 h=400 fmt=rgb565le bytes=576000 ===
    SS:<base64>
    SS:<base64>
    ...
    === TABDOS SCREENSHOT END ===

Capture the monitor output to a file (e.g. `idf.py monitor | tee dump.txt`, or
just copy/save the terminal), then:

    python3 tools/decode_screenshot.py dump.txt screenshot.bmp

If the input contains several dumps, every complete one is decoded and written
to a numbered file (screenshot_000.bmp, screenshot_001.bmp, ...). Use --index N
(0-based) to decode a single dump to the exact output path instead.
"""

import argparse
import base64
import os
import re
import struct
import sys

BEGIN_RE = re.compile(r"TABDOS SCREENSHOT BEGIN w=(\d+) h=(\d+) fmt=(\S+)")
END_MARK = "TABDOS SCREENSHOT END"


def extract_dumps(text):
    """Return a list of (w, h, fmt, base64_payload) for each complete dump."""
    dumps = []
    cur = None
    for line in text.splitlines():
        m = BEGIN_RE.search(line)
        if m:
            cur = {"w": int(m.group(1)), "h": int(m.group(2)), "fmt": m.group(3), "b64": []}
            continue
        if cur is None:
            continue
        if END_MARK in line:
            dumps.append((cur["w"], cur["h"], cur["fmt"], "".join(cur["b64"])))
            cur = None
            continue
        idx = line.find("SS:")
        if idx != -1:
            cur["b64"].append(line[idx + 3:].strip())
    return dumps


def expand_rle(payload):
    """Expand a run-length stream of {u16 count, u16 value} LE pairs to raw
    RGB565 bytes. Truncated/partial dumps expand as far as the data allows."""
    if len(payload) % 4 != 0:
        payload = payload[: len(payload) - (len(payload) % 4)]
    out = bytearray()
    for count, value in struct.iter_unpack("<HH", payload):
        out += struct.pack("<H", value) * count
    return bytes(out)


def rgb565le_to_rgb888(data, w, h):
    """Convert little-endian RGB565 bytes to a flat list of (r, g, b) rows."""
    px = struct.unpack("<%dH" % (w * h), data)
    out = bytearray(w * h * 3)
    o = 0
    for v in px:
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[o] = (r << 3) | (r >> 2)
        out[o + 1] = (g << 2) | (g >> 4)
        out[o + 2] = (b << 3) | (b >> 2)
        o += 3
    return out


def write_bmp(path, rgb, w, h):
    """Write a 24-bit BMP. BMP rows are bottom-up and 4-byte aligned."""
    row_bytes = w * 3
    pad = (4 - row_bytes % 4) % 4
    stride = row_bytes + pad
    pixel_data_size = stride * h
    file_size = 54 + pixel_data_size
    with open(path, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", file_size, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, pixel_data_size, 2835, 2835, 0, 0))
        padding = b"\x00" * pad
        for y in range(h - 1, -1, -1):  # bottom-up
            start = y * row_bytes
            # BMP stores BGR
            row = rgb[start:start + row_bytes]
            f.write(bytes(row[i + 2 - 2 * (i % 3)] if i % 3 != 1 else row[i] for i in range(len(row))))
            f.write(padding)


def decode_dump(w, h, fmt, b64):
    """Return RGB888 bytes for one dump, or raise ValueError if it can't be used."""
    if fmt not in ("rgb565le", "rgb565le-rle"):
        raise ValueError("unsupported pixel format: %s" % fmt)
    payload = base64.b64decode(b64)
    data = expand_rle(payload) if fmt == "rgb565le-rle" else payload
    expected = w * h * 2
    if len(data) != expected:
        raise ValueError("decoded %d bytes but expected %d (%dx%d rgb565) — truncated/corrupted"
                         % (len(data), expected, w, h))
    return rgb565le_to_rgb888(data, w, h)


def numbered_path(base, index, pad):
    root, ext = os.path.splitext(base)
    return "%s_%0*d%s" % (root, pad, index, ext or ".bmp")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="serial log file containing the dump (or - for stdin)")
    ap.add_argument("output", nargs="?", default="screenshot.bmp", help="output BMP path (or naming stem for multiple dumps)")
    ap.add_argument("--index", type=int, default=None,
                    help="decode only this dump (0-based) to the exact output path; default: decode all")
    args = ap.parse_args()

    text = sys.stdin.read() if args.input == "-" else open(args.input, "r", errors="replace").read()
    dumps = extract_dumps(text)
    if not dumps:
        sys.exit("no complete TABDOS SCREENSHOT block found in input")

    if args.index is not None:
        try:
            selected = [(args.index, dumps[args.index])]
        except IndexError:
            sys.exit("--index %d out of range (%d dump(s) found)" % (args.index, len(dumps)))
    else:
        selected = list(enumerate(dumps))

    multiple = len(selected) > 1
    pad = max(3, len(str(len(dumps) - 1)))
    written = 0
    for idx, (w, h, fmt, b64) in selected:
        out = numbered_path(args.output, idx, pad) if multiple else args.output
        try:
            rgb = decode_dump(w, h, fmt, b64)
        except ValueError as e:
            print("skip dump #%d: %s" % (idx, e), file=sys.stderr)
            continue
        write_bmp(out, rgb, w, h)
        print("wrote %s (%dx%d) from dump #%d of %d" % (out, w, h, idx, len(dumps)))
        written += 1

    if written == 0:
        sys.exit("no dumps decoded successfully (all truncated/corrupted?)")


if __name__ == "__main__":
    main()
