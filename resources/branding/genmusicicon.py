#!/usr/bin/env python3
"""Generates music/icon.bmp (32x32, 4bpp, palette index 0 = transparent key) - original artwork: a glossy aqua tile with a white music note."""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "music", "icon.bmp")
SS = 8
N = 32 * SS

img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
# gradient tile
tile = Image.new("RGBA", (N, N))
px = tile.load()
for y in range(N):
    t = y / N
    c = (int(40 + 40 * t), int(170 + 40 * t), int(240 - 10 * t), 255)
    for x in range(N):
        px[x, y] = c
mask = Image.new("L", (N, N), 0)
ImageDraw.Draw(mask).rounded_rectangle((1 * SS, 1 * SS, 31 * SS - 1, 31 * SS - 1), radius=7 * SS, fill=255)
img.paste(tile, (0, 0), mask)
d = ImageDraw.Draw(img)
# gloss
gloss = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ImageDraw.Draw(gloss).ellipse((-6 * SS, -18 * SS, 38 * SS, 15 * SS), fill=(255, 255, 255, 90))
gm = Image.new("L", (N, N), 0)
gm.paste(mask)
img = Image.composite(Image.alpha_composite(img, gloss), img, gm)
d = ImageDraw.Draw(img)
white = (255, 255, 255, 255)
# note: two heads, two stems, beam
d.ellipse((7 * SS, 19 * SS, 14 * SS, 25 * SS), fill=white)
d.ellipse((18 * SS, 17 * SS, 25 * SS, 23 * SS), fill=white)
d.rectangle((13 * SS, 7 * SS, 14.6 * SS, 22 * SS), fill=white)
d.rectangle((24 * SS, 5 * SS, 25.6 * SS, 20 * SS), fill=white)
d.polygon([(13 * SS, 6.5 * SS), (25.6 * SS, 4 * SS), (25.6 * SS, 8 * SS), (13 * SS, 10.5 * SS)], fill=white)
img = img.resize((32, 32), Image.LANCZOS)

# quantise: 15 colours + transparent key (index 0, magenta)
alpha = img.split()[3]
opaque = img.convert("RGB")
q = opaque.quantize(colors=15, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
pal = q.getpalette()[:45]
palette = [(255, 0, 255)] + [tuple(pal[i * 3:i * 3 + 3]) for i in range(15)]
idx = []
qp = q.load()
ap = alpha.load()
for y in range(32):
    row = []
    for x in range(32):
        row.append(0 if ap[x, y] < 128 else qp[x, y] + 1)
    idx.append(row)

rows = b""
for y in range(31, -1, -1):
    r = idx[y]
    rows += bytes((r[i] << 4) | r[i + 1] for i in range(0, 32, 2))
pal_bytes = b"".join(bytes((c[2], c[1], c[0], 0)) for c in palette)
hdr_size = 14 + 40 + len(pal_bytes)
size = hdr_size + len(rows)
bmp = b"BM" + size.to_bytes(4, "little") + bytes(4) + hdr_size.to_bytes(4, "little")
bmp += (40).to_bytes(4, "little") + (32).to_bytes(4, "little") + (32).to_bytes(4, "little") + (1).to_bytes(2, "little") + (4).to_bytes(2, "little")
bmp += bytes(4) + len(rows).to_bytes(4, "little") + (2835).to_bytes(4, "little") * 2 + (16).to_bytes(4, "little") + (16).to_bytes(4, "little")
open(OUT, "wb").write(bmp + pal_bytes + rows)
print("wrote", OUT, len(bmp + pal_bytes + rows), "bytes")
