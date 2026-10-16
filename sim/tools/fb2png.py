#!/usr/bin/env python3
"""Convert raw 2bpp BWRY framebuffer dumps (*.fb) to PNG. Usage: fb2png.py W H file.fb ..."""
import sys
from PIL import Image

PALETTE = {0: (0, 0, 0), 1: (255, 255, 255), 2: (255, 214, 0), 3: (220, 30, 30)}


def convert(path, width, height):
    data = open(path, "rb").read()
    bpr = (width * 2 + 7) >> 3
    img = Image.new("RGB", (width, height))
    px = img.load()
    for y in range(height):
        for x in range(width):
            px[x, y] = PALETTE[(data[y * bpr + (x >> 2)] >> (6 - (x & 3) * 2)) & 3]
    out = path[:-3] + ".png"
    img.save(out)
    return out


if __name__ == "__main__":
    w, h = int(sys.argv[1]), int(sys.argv[2])
    for f in sys.argv[3:]:
        print(convert(f, w, h))
