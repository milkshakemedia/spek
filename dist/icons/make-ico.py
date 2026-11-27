#!/usr/bin/env python3
"""Build the Windows toolbar .ico files from the 4x renders made by render.js.

Usage: python3 dist/icons/make-ico.py RENDERDIR dist/win
Icons are dark grey at 80% opacity, for Windows' light toolbars.
"""
import sys
from PIL import Image


def tint(im, opacity=0.8, level=0x20):
    im = im.convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            px[x, y] = (level, level, level, int(px[x, y][3] * opacity + 0.5))
    return im


src, out = sys.argv[1], sys.argv[2]
for name in ("open", "save", "help", "close"):
    big = tint(Image.open(f"{src}/{name}@4x.png"))
    sizes = [16, 24, 32, 64] if name == "close" else [16, 24, 32, 48, 96]
    big.save(f"{out}/{name}.ico", sizes=[(s, s) for s in sizes])
