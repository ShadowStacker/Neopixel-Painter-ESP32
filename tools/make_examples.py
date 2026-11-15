#!/usr/bin/env python3
"""Generate example 24-bit BMP images for the NeoPixel Painter.

No dependencies beyond the Python standard library. Images are HEIGHT pixels
tall (one pixel per LED) and drawn the natural way round: each column is one
painted line, played left to right.

    python3 tools/make_examples.py              # writes to ./examples
    python3 tools/make_examples.py out_dir 60   # 60-pixel strip
"""
import colorsys
import math
import os
import struct
import sys


def write_bmp(path, width, height, pixel):
    """pixel(x, y) -> (r, g, b), with y = 0 at the top."""
    row_bytes = width * 3
    pad = b"\0" * ((4 - row_bytes % 4) % 4)
    rows = []
    for y in range(height - 1, -1, -1):  # BMP rows are stored bottom-up
        row = bytearray()
        for x in range(width):
            r, g, b = pixel(x, y)
            row += bytes((b, g, r))
        rows.append(bytes(row) + pad)
    data = b"".join(rows)
    header = b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                       len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + data)
    print(f"wrote {path} ({width}x{height})")


def hsv(h, s=1.0, v=1.0):
    return tuple(int(c * 255) for c in colorsys.hsv_to_rgb(h % 1.0, s, v))


def rainbow(h):
    w = 300
    return w, lambda x, y: hsv(x / w + y / (h * 3))


def sine_wave(h):
    w = 400

    def px(x, y):
        centre = h / 2 + math.sin(x / w * 4 * math.pi) * h * 0.35
        d = abs(y - centre)
        if d < h * 0.06:
            return hsv(x / w, 1, 1 - d / (h * 0.06) * 0.6)
        return (0, 0, 0)
    return w, px


def heart(h):
    w = h

    def px(x, y):
        # Classic implicit heart curve, scaled to the image
        u = (x / w - 0.5) * 2.6
        v = (0.5 - y / h) * 2.6 + 0.125
        if (u * u + v * v - 1) ** 3 - u * u * v ** 3 <= 0:
            return (255, 0, 40)
        return (0, 0, 0)
    return w, px


def stars(h):
    w = 360
    import random
    rnd = random.Random(42)
    pts = {(rnd.randrange(w), rnd.randrange(h)): hsv(rnd.random(), 0.3)
           for _ in range(140)}
    return w, lambda x, y: pts.get((x, y), (0, 0, 0))


def checker(h):
    w = 240
    cell = max(1, h // 8)

    def px(x, y):
        on = (x // cell + y // cell) % 2
        return hsv(y / h) if on else (0, 0, 0)
    return w, px


EXAMPLES = [("01_rainbow", rainbow), ("02_sine_wave", sine_wave),
            ("03_heart", heart), ("04_stars", stars), ("05_checker", checker)]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "examples"
    height = int(sys.argv[2]) if len(sys.argv) > 2 else 144
    os.makedirs(out, exist_ok=True)
    for name, fn in EXAMPLES:
        width, px = fn(height)
        write_bmp(os.path.join(out, name + ".bmp"), width, height, px)
