#!/usr/bin/env python3
"""Draws Gesher's icon and the test apps' launcher icons (needs Pillow with WebP).

    python3 tools/icons/make-icons.py

Writes, relative to the repository:
  tools/package/gesher.ico   Gesher's icon (a bridge): the installer, its Start menu
                             entries and the .apk file type (docs/installer.md)
  tests/apps/lifecycle/res/mipmap-*/ic_launcher.png
                             a legacy launcher icon: grey at mdpi and xhdpi, red at
                             xxxhdpi, so the tests see which density was picked
  tests/apps/components/res/mipmap-*/ic_launcher_foreground.webp
                             an adaptive icon's foreground (WebP): a white square at
                             xxxhdpi, grey at mdpi; its background is a color resource

The outputs are checked in; this script is how they were made.
"""
import io
import math
import os
import struct
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def gesher_master(size=1024):
    """A white bridge (an arch over a deck, hangers, two piers) on a blue rounded square."""
    s = size / 1024
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    # A vertical gradient, then the rounded square as its mask.
    grad = Image.new("RGBA", (size, size))
    top, bottom = (0x1E, 0x88, 0xE5), (0x0D, 0x47, 0xA1)
    gd = ImageDraw.Draw(grad)
    for y in range(size):
        t = y / (size - 1)
        gd.line([(0, y), (size, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=int(225 * s), fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    cx, deck_y, rx, ry = 512, 640, 332, 340
    # The arch: the upper half of an ellipse.
    d.arc([(cx - rx) * s, (deck_y - ry) * s, (cx + rx) * s, (deck_y + ry) * s], 180, 360,
          fill=white, width=int(70 * s))
    # The hangers, from the arch down to the deck.
    for x in (300, 406, 512, 618, 724):
        y = deck_y - ry * math.sqrt(max(0.0, 1 - ((x - cx) / rx) ** 2))
        d.line([(x * s, y * s), (x * s, deck_y * s)], fill=white, width=int(26 * s))
    # The deck and the piers.
    d.rectangle([130 * s, 615 * s, 894 * s, 690 * s], fill=white)
    for x in (180, 844):
        d.rectangle([(x - 32) * s, 690 * s, (x + 32) * s, 800 * s], fill=white)
    return img


def dib(img):
    """A 32-bit BITMAPINFOHEADER image with its AND mask, as .ico entries hold them."""
    w, h = img.size
    px = img.load()
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            rows += bytes((b, g, r, a))
    stride = (w + 31) // 32 * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if px[x, y][3] == 0:
                row[x >> 3] |= 0x80 >> (x & 7)
        mask += row
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(rows) + len(mask), 0, 0, 0, 0)
    return header + bytes(rows) + bytes(mask)


def write_ico(path, master):
    """DIBs up to 48 pixels, PNGs from 64 on (as AppIcon.java writes them)."""
    images = []
    for size in ICO_SIZES:
        img = master.resize((size, size), Image.LANCZOS)
        if size >= 64:
            buf = io.BytesIO()
            img.save(buf, "PNG", optimize=True)
            images.append((size, buf.getvalue()))
        else:
            images.append((size, dib(img)))
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        b = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    with open(path, "wb") as f:
        f.write(out)


def circle(size, color, scale=4):
    big = Image.new("RGBA", (size * scale, size * scale), (0, 0, 0, 0))
    m = size * scale * 0.04
    ImageDraw.Draw(big).ellipse([m, m, size * scale - m, size * scale - m], fill=color)
    return big.resize((size, size), Image.LANCZOS)


def square(size, color):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(img).rectangle([size // 3, size // 3, size - size // 3 - 1, size - size // 3 - 1],
                                  fill=color)
    return img


def save(img, rel, fmt, **kw):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, fmt, **kw)
    print(rel, os.path.getsize(path), "bytes")


def main():
    write_ico(os.path.join(ROOT, "tools/package/gesher.ico"), gesher_master())
    print("tools/package/gesher.ico", os.path.getsize(os.path.join(ROOT, "tools/package/gesher.ico")), "bytes")
    grey, red, white = (0x80, 0x80, 0x80, 255), (0xD3, 0x2F, 0x2F, 255), (255, 255, 255, 255)
    # lifecycle.apk: 48dp legacy icons.
    for density, size, color in (("mdpi", 48, grey), ("xhdpi", 96, grey), ("xxxhdpi", 192, red)):
        save(circle(size, color), f"tests/apps/lifecycle/res/mipmap-{density}/ic_launcher.png", "PNG",
             optimize=True)
    # components.apk: an adaptive icon's 108dp foreground, in WebP.
    for density, size, color in (("mdpi", 108, grey), ("xxxhdpi", 432, white)):
        save(square(size, color), f"tests/apps/components/res/mipmap-{density}/ic_launcher_foreground.webp",
             "WEBP", lossless=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
