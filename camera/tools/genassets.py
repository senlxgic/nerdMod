#!/usr/bin/env python3
"""
Generates the pre-rendered 15-bit UI art of the nerdMod Camera.

  python3 camera/tools/genassets.py

writes
  camera/assets/ui.bin               all images, raw RGB555 (bit 15 set = opaque, 0 = transparent), back to back
  camera/arm9/source/ui_assets.h     offsets/sizes of every image inside ui.bin
  camera/icon.bmp                    32x32 4bpp icon of the Camera tile (also its menu banner icon)
  camera/tools/preview/*.png         what the screens look like (not used by the build)

Everything is drawn from scratch here (original art, no third-party graphics): aqua / sky-blue gradients,
glass panels, glossy buttons and bubbles in the spirit of the late-2000s "Frutiger Aero" look.
The bottom-screen buttons are rendered *on top of* the background at their final position, so their anti-aliased
edges are exact and the program only has to copy opaque rectangles (no per-frame PNG decoding, no blending).
Needs: Python 3, Pillow, NumPy, the DejaVu Sans font.
"""
import os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_BIN = os.path.join(ROOT, "assets", "ui.bin")
OUT_H = os.path.join(ROOT, "arm9", "source", "ui_assets.h")
OUT_ICON = os.path.join(ROOT, "icon.bmp")
PREVIEW = os.path.join(ROOT, "tools", "preview")
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

SS = 4  # supersampling factor

# ---------------------------------------------------------------------------------------------- palette
SKY_TOP = (168, 232, 255)
SKY_BOT = (52, 176, 232)
AQUA = (64, 214, 240)
DEEP = (10, 104, 168)
TEAL_TEXT = (6, 74, 112)
WHITE = (255, 255, 255)
GREEN = (170, 240, 196)
RED_TOP = (255, 130, 110)
RED_BOT = (190, 20, 30)

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

def vgrad(w, h, top, bot):
    t = np.linspace(0, 1, h)[:, None, None]
    top = np.array(top, float)[None, None, :]
    bot = np.array(bot, float)[None, None, :]
    arr = top + (bot - top) * t
    arr = np.repeat(arr, w, axis=1)
    return Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGB")

def font(path, px):
    return ImageFont.truetype(path, px)

def canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))

def down(img):
    return img.resize((img.width // SS, img.height // SS), Image.LANCZOS)

def S(v):
    return int(round(v * SS))

def paste_alpha(dst_rgb, layer_rgba, x, y):
    """dst_rgb (PIL RGB) <- layer (RGBA, already at final res) at x,y"""
    dst_rgb.paste(layer_rgba, (x, y), layer_rgba)

def radial(w, h, cx, cy, r, inner, outer, power=1.0):
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
    t = np.clip(d, 0, 1) ** power
    inner = np.array(inner, float); outer = np.array(outer, float)
    arr = inner[None, None, :] + (outer - inner)[None, None, :] * t[:, :, None]
    return arr

def gradient_layer(w, h, top, bot, alpha_top=255, alpha_bot=255):
    t = np.linspace(0, 1, h)[:, None, None]
    top = np.array(list(top) + [alpha_top], float)[None, None, :]
    bot = np.array(list(bot) + [alpha_bot], float)[None, None, :]
    arr = top + (bot - top) * t
    arr = np.repeat(arr, w, axis=1)
    return Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGBA")

def masked(grad_rgba, mask_l):
    out = grad_rgba.copy()
    a = np.array(out)[:, :, 3].astype(float) * (np.array(mask_l).astype(float) / 255.0)
    arr = np.array(out)
    arr[:, :, 3] = a.clip(0, 255).astype(np.uint8)
    return Image.fromarray(arr, "RGBA")

# ---------------------------------------------------------------------------------------------- shapes
def rr_mask(w, h, x0, y0, x1, y1, r):
    m = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([S(x0), S(y0), S(x1), S(y1)], radius=S(r), fill=255)
    return m

def ellipse_mask(w, h, x0, y0, x1, y1):
    m = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(m).ellipse([S(x0), S(y0), S(x1), S(y1)], fill=255)
    return m

def compose(layers, w, h):
    base = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    for l in layers:
        base = Image.alpha_composite(base, l)
    return base

def glow(layer_rgba, radius):
    return layer_rgba.filter(ImageFilter.GaussianBlur(radius * SS))

def drop_shadow(mask_l, w, h, dx, dy, blur, alpha):
    sh = Image.new("RGBA", (w * SS, h * SS), (0, 30, 70, 0))
    a = Image.new("L", (w * SS, h * SS), 0)
    a.paste(mask_l, (S(dx), S(dy)))
    a = a.filter(ImageFilter.GaussianBlur(blur * SS)).point(lambda v: int(v * alpha))
    sh.putalpha(a)
    return sh

def glass_shape(w, h, mask, top, bot, rim=(255, 255, 255), rim_alpha=230, gloss=0.55, bbox=None):
    """A glossy glass body for any mask: gradient fill, bright rim, upper-half specular sheen."""
    body = masked(gradient_layer(w * SS, h * SS, top, bot, 235, 245), mask)
    # rim: mask minus eroded mask
    er = mask.filter(ImageFilter.MinFilter(S(1.3) | 1))
    ring = Image.fromarray(np.clip(np.array(mask).astype(int) - np.array(er).astype(int), 0, 255).astype(np.uint8), "L")
    rimlayer = Image.new("RGBA", (w * SS, h * SS), rim + (0,))
    rimlayer.putalpha(ring.point(lambda v: int(v * rim_alpha / 255)))
    # sheen: upper half, bright to transparent
    if bbox is None:
        bbox = mask.getbbox()
    x0, y0, x1, y1 = bbox
    sheen_h = (y1 - y0) * 0.52
    sh_mask = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(sh_mask)
    d.rectangle([x0, y0, x1, y0 + sheen_h], fill=255)
    sh_mask = Image.fromarray((np.array(sh_mask).astype(float) * np.array(mask).astype(float) / 255).astype(np.uint8), "L")
    yy = np.linspace(0, 1, h * SS)[:, None]
    fall = np.clip(1 - (yy * (h * SS) - y0) / max(sheen_h, 1), 0, 1)
    sheen_a = (np.array(sh_mask).astype(float) * fall * gloss).clip(0, 255).astype(np.uint8)
    sheen = Image.new("RGBA", (w * SS, h * SS), (255, 255, 255, 0))
    sheen.putalpha(Image.fromarray(sheen_a, "L"))
    return [body, sheen, rimlayer]

# ---------------------------------------------------------------------------------------------- background
def make_bg():
    W, H = 256, 192
    img = vgrad(W, H, SKY_TOP, SKY_BOT).convert("RGBA")
    arr = np.array(img).astype(float)
    # soft diagonal light sweep
    yy, xx = np.mgrid[0:H, 0:W]
    sweep = np.exp(-((xx * 0.8 + yy * 0.6 - 150) ** 2) / (2 * 38.0 ** 2)) * 0.30
    arr[:, :, :3] = arr[:, :, :3] * (1 - sweep[:, :, None]) + 255 * sweep[:, :, None]
    # lower pale-green "meadow" glow
    g = np.clip((yy - 126) / 66.0, 0, 1) ** 1.6 * 0.55
    arr[:, :, :3] = arr[:, :, :3] * (1 - g[:, :, None]) + np.array(GREEN) * g[:, :, None]
    img = Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGBA")

    # bubbles (fixed pseudo-random placement, kept away from the buttons)
    bub = canvas(W, H)
    d = ImageDraw.Draw(bub)
    spots = [(22, 44, 9), (44, 30, 5), (236, 56, 7), (214, 44, 4), (24, 140, 6), (238, 148, 9), (252, 62, 4), (6, 112, 5),
             (96, 146, 4), (170, 146, 5), (118, 40, 3), (150, 46, 5), (60, 176, 7), (200, 172, 5), (240, 182, 6)]
    for (cx, cy, r) in spots:
        d.ellipse([S(cx - r), S(cy - r), S(cx + r), S(cy + r)], fill=(255, 255, 255, 38), outline=(255, 255, 255, 150), width=max(1, S(0.7)))
        d.ellipse([S(cx - r * 0.55), S(cy - r * 0.7), S(cx - r * 0.05), S(cy - r * 0.25)], fill=(255, 255, 255, 170))
    img = Image.alpha_composite(img, down(bub))

    # top glass bar
    bar = canvas(W, H)
    m = rr_mask(W, H, -8, -10, W + 8, 30, 12)
    layers = glass_shape(W, H, m, (255, 255, 255), (170, 232, 252), rim_alpha=200, gloss=0.35, bbox=(0, 0, W * SS, 30 * SS))
    img = Image.alpha_composite(img, down(compose([drop_shadow(m, W, H, 0, 1.5, 2.0, 0.35)] + layers, W, H)))
    return img.convert("RGB")

def text_layer(w, h, xy, text, px, fill, bold=True, shadow=None, anchor="la"):
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    f = font(FONT_BOLD if bold else FONT_REG, S(px))
    if shadow:
        d.text((S(xy[0] + shadow[0]), S(xy[1] + shadow[1])), text, font=f, fill=shadow[2], anchor=anchor)
    d.text((S(xy[0]), S(xy[1])), text, font=f, fill=fill, anchor=anchor)
    return lay

# ---------------------------------------------------------------------------------------------- icons (glyphs)
def glyph_album(d, cx, cy, col):
    # two stacked photo frames with a little sun + hill
    d.rounded_rectangle([S(cx - 12), S(cy - 8), S(cx + 8), S(cy + 8)], radius=S(2.5), outline=col + (255,), width=S(1.8), fill=(255, 255, 255, 120))
    d.rounded_rectangle([S(cx - 7), S(cy - 12), S(cx + 12), S(cy + 4)], radius=S(2.5), outline=col + (255,), width=S(1.8), fill=(255, 255, 255, 200))
    d.ellipse([S(cx + 4), S(cy - 9), S(cx + 8), S(cy - 5)], fill=col + (255,))
    d.polygon([(S(cx - 5), S(cy + 2)), (S(cx + 0), S(cy - 4)), (S(cx + 4), S(cy + 1)), (S(cx + 7), S(cy - 1)), (S(cx + 10), S(cy + 2))], fill=col + (255,))

def glyph_flip(d, cx, cy, col):
    # two curved arrows around a lens
    for a0, a1 in ((200, 330), (20, 150)):
        d.arc([S(cx - 11), S(cy - 11), S(cx + 11), S(cy + 11)], a0, a1, fill=col + (255,), width=S(2.4))
    import math
    for ang, sgn in ((330, 1), (150, 1)):
        rad = math.radians(ang)
        px, py = cx + 11 * math.cos(rad), cy + 11 * math.sin(rad)
        tx, ty = -math.sin(rad), math.cos(rad)  # tangent (clockwise)
        nx, ny = math.cos(rad), math.sin(rad)
        tip = (px + tx * 5, py + ty * 5)
        b1 = (px - nx * 4.2, py - ny * 4.2)
        b2 = (px + nx * 4.2, py + ny * 4.2)
        d.polygon([(S(tip[0]), S(tip[1])), (S(b1[0]), S(b1[1])), (S(b2[0]), S(b2[1]))], fill=col + (255,))
    d.ellipse([S(cx - 4), S(cy - 4), S(cx + 4), S(cy + 4)], fill=col + (255,))
    d.ellipse([S(cx - 1.6), S(cy - 2.4), S(cx + 0.4), S(cy - 0.4)], fill=(255, 255, 255, 230))

def glyph_chev(d, cx, cy, col, left=True, size=9, width=3.2):
    s = size if left else -size
    d.line([(S(cx + s * 0.45), S(cy - size)), (S(cx - s * 0.55), S(cy)), (S(cx + s * 0.45), S(cy + size))], fill=col + (255,), width=S(width), joint="curve")
    r = width / 2
    for (px, py) in ((cx + s * 0.45, cy - size), (cx + s * 0.45, cy + size)):
        d.ellipse([S(px - r), S(py - r), S(px + r), S(py + r)], fill=col + (255,))

def glyph_trash(d, cx, cy, col):
    d.rounded_rectangle([S(cx - 8), S(cy - 6), S(cx + 8), S(cy + 11)], radius=S(2.5), fill=col + (255,))
    d.rounded_rectangle([S(cx - 10), S(cy - 10), S(cx + 10), S(cy - 6.5)], radius=S(1.5), fill=col + (255,))
    d.rounded_rectangle([S(cx - 3.5), S(cy - 13), S(cx + 3.5), S(cy - 9.5)], radius=S(1.5), fill=col + (255,))
    for dx in (-4, 0, 4):
        d.line([(S(cx + dx), S(cy - 2)), (S(cx + dx), S(cy + 8))], fill=(255, 255, 255, 235), width=S(1.6))

def glyph_play(d, cx, cy, col, size=11):
    d.polygon([(S(cx - size * 0.6), S(cy - size)), (S(cx + size), S(cy)), (S(cx - size * 0.6), S(cy + size))], fill=col + (255,))

def glyph_pause(d, cx, cy, col, size=10):
    for dx in (-5.5, 1.5):
        d.rounded_rectangle([S(cx + dx), S(cy - size), S(cx + dx + 4.4), S(cy + size)], radius=S(1.4), fill=col + (255,))

def glyph_gear(d, cx, cy, col, r=8.5):
    import math
    for i in range(8):
        a = math.radians(i * 45)
        x0, y0 = cx + math.cos(a) * (r - 2), cy + math.sin(a) * (r - 2)
        x1, y1 = cx + math.cos(a) * (r + 2.2), cy + math.sin(a) * (r + 2.2)
        d.line([(S(x0), S(y0)), (S(x1), S(y1))], fill=col + (255,), width=S(3.6))
    d.ellipse([S(cx - r), S(cy - r), S(cx + r), S(cy + r)], fill=col + (255,))
    d.ellipse([S(cx - r * 0.42), S(cy - r * 0.42), S(cx + r * 0.42), S(cy + r * 0.42)], fill=(255, 255, 255, 240))

def glyph_info(d, cx, cy, col):
    d.ellipse([S(cx - 11), S(cy - 11), S(cx + 11), S(cy + 11)], fill=col + (255,))
    d.ellipse([S(cx - 2), S(cy - 7.5), S(cx + 2), S(cy - 3.5)], fill=(255, 255, 255, 245))
    d.rounded_rectangle([S(cx - 2), S(cy - 1.5), S(cx + 2), S(cy + 7.5)], radius=S(1.2), fill=(255, 255, 255, 245))

def glyph_camera(d, cx, cy, col, scale=1.0):
    k = scale
    d.rounded_rectangle([S(cx - 9 * k), S(cy - 5 * k), S(cx + 9 * k), S(cy + 7 * k)], radius=S(2.4 * k), fill=col + (255,))
    d.rounded_rectangle([S(cx - 4 * k), S(cy - 8 * k), S(cx + 3 * k), S(cy - 4 * k)], radius=S(1.5 * k), fill=col + (255,))
    d.ellipse([S(cx - 4.6 * k), S(cy - 3.2 * k), S(cx + 4.6 * k), S(cy + 6 * k)], fill=(255, 255, 255, 235))
    d.ellipse([S(cx - 2.8 * k), S(cy - 1.4 * k), S(cx + 2.8 * k), S(cy + 4.2 * k)], fill=col + (255,))

def glyph_film(d, cx, cy, col, scale=1.0):
    k = scale
    d.rounded_rectangle([S(cx - 9 * k), S(cy - 6 * k), S(cx + 5 * k), S(cy + 6 * k)], radius=S(2.2 * k), fill=col + (255,))
    d.polygon([(S(cx + 6 * k), S(cy - 2 * k)), (S(cx + 11 * k), S(cy - 5 * k)), (S(cx + 11 * k), S(cy + 5 * k)), (S(cx + 6 * k), S(cy + 2 * k))], fill=col + (255,))
    d.ellipse([S(cx - 6.4 * k), S(cy - 3.6 * k), S(cx - 1.6 * k), S(cy + 1.2 * k)], fill=(255, 255, 255, 235))

# ---------------------------------------------------------------------------------------------- buttons (opaque rects over the bg)
def crop_bg(bg, x, y, w, h):
    return bg.crop((x, y, x + w, y + h)).convert("RGBA")

def finish(base_rgba, layers, w, h):
    lay = down(compose(layers, w, h))
    out = Image.alpha_composite(base_rgba, lay)
    return out.convert("RGB")

def square_button(bg, x, y, w, h, glyph, label, pressed=False, tint=None, disabled=False, label_col=TEAL_TEXT):
    base = crop_bg(bg, x, y, w, h)
    inset = 3
    mask = rr_mask(w, h, inset, inset, w - inset, h - inset - (1 if not pressed else 0), 11)
    if tint == "red":
        top, bot = (255, 214, 210), (255, 130, 124)
    elif disabled:
        top, bot = (230, 240, 246), (188, 206, 218)
    else:
        top, bot = (255, 255, 255), (150, 226, 250)
    if pressed:
        top, bot = lerp(top, (60, 150, 200), 0.35), lerp(bot, (30, 110, 170), 0.35)
    layers = []
    if not pressed:
        layers.append(drop_shadow(mask, w, h, 0, 1.6, 1.8, 0.45))
    layers += glass_shape(w, h, mask, top, bot, rim_alpha=235 if not pressed else 150, gloss=0.50 if not pressed else 0.18)
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    gcol = (170, 60, 56) if tint == "red" else (TEAL_TEXT if not disabled else (120, 142, 158))
    off = 1 if pressed else 0
    glyph(d, w / 2, h / 2 - 6 + off, gcol)
    layers.append(lay)
    layers.append(text_layer(w, h, (w / 2, h - 11 + off), label, 8.6, label_col if not disabled else (120, 142, 158), anchor="mm"))
    return finish(base, layers, w, h)

def pill_button(bg, x, y, w, h, glyph, label, pressed=False):
    base = crop_bg(bg, x, y, w, h)
    inset = 2
    mask = rr_mask(w, h, inset, inset, w - inset, h - inset - (1 if not pressed else 0), h / 2 - 2)
    top, bot = (255, 255, 255), (150, 226, 250)
    if pressed:
        top, bot = lerp(top, (60, 150, 200), 0.35), lerp(bot, (30, 110, 170), 0.35)
    layers = []
    if not pressed:
        layers.append(drop_shadow(mask, w, h, 0, 1.4, 1.6, 0.45))
    layers += glass_shape(w, h, mask, top, bot, rim_alpha=235 if not pressed else 150, gloss=0.5 if not pressed else 0.18)
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    off = 1 if pressed else 0
    glyph(d, 17, h / 2 + off, TEAL_TEXT)
    layers.append(lay)
    layers.append(text_layer(w, h, (w / 2 + 8, h / 2 + off), label, 10.5, TEAL_TEXT, anchor="mm"))
    return finish(base, layers, w, h)

def row_button(bg, x, y, w, h, glyph=None, label=None):
    base = crop_bg(bg, x, y, w, h)
    mask = rr_mask(w, h, 2, 2, w - 2, h - 3, h / 2 - 2)
    layers = [drop_shadow(mask, w, h, 0, 1.2, 1.4, 0.40)]
    layers += glass_shape(w, h, mask, (255, 255, 255), (170, 230, 250), rim_alpha=235, gloss=0.5)
    if glyph or label:
        lay = canvas(w, h)
        d = ImageDraw.Draw(lay)
        if glyph:
            glyph(d, 14, h / 2, TEAL_TEXT)
        layers.append(lay)
        if label:
            layers.append(text_layer(w, h, (w / 2 + (7 if glyph else 0), h / 2), label, 10, TEAL_TEXT, anchor="mm"))
    return finish(base, layers, w, h)

def shutter(bg, x, y, size, kind, pressed=False, glowing=False):
    """kind: photo | video | rec"""
    w = h = size
    base = crop_bg(bg, x, y, w, h)
    cx = cy = size / 2
    R = size / 2 - 3
    layers = []
    outer = ellipse_mask(w, h, cx - R, cy - R, cx + R, cy + R)
    if glowing:
        halo = Image.new("RGBA", (w * SS, h * SS), (255, 70, 60, 0))
        halo.putalpha(ellipse_mask(w, h, cx - R - 1, cy - R - 1, cx + R + 1, cy + R + 1).filter(ImageFilter.GaussianBlur(S(4))).point(lambda v: int(v * 0.95)))
        layers.append(halo)
    layers.append(drop_shadow(outer, w, h, 0, 2.2, 2.6, 0.5))
    # white glass ring
    ring_top, ring_bot = (255, 255, 255), (186, 236, 252)
    layers += glass_shape(w, h, outer, ring_top, ring_bot, rim_alpha=255, gloss=0.55)
    # orb
    r2 = R * 0.74 - (1.2 if pressed else 0)
    orb = ellipse_mask(w, h, cx - r2, cy - r2, cx + r2, cy + r2)
    if kind == "photo":
        c_top, c_mid, c_bot = (120, 232, 255), (24, 150, 226), (8, 70, 160)
    else:
        c_top, c_mid, c_bot = (255, 150, 126), (230, 52, 48), (150, 8, 24)
    if pressed:
        c_top, c_mid, c_bot = lerp(c_top, (0, 0, 0), 0.22), lerp(c_mid, (0, 0, 0), 0.22), lerp(c_bot, (0, 0, 0), 0.22)
    # radial-ish orb: vertical gradient top->mid->bottom
    hh = h * SS
    t = np.linspace(0, 1, hh)[:, None]
    stops = np.array([c_top, c_mid, c_bot], float)
    rgb = np.zeros((hh, w * SS, 3))
    for i in range(hh):
        tt = i / (hh - 1)
        if tt < 0.55:
            c = stops[0] + (stops[1] - stops[0]) * (tt / 0.55)
        else:
            c = stops[1] + (stops[2] - stops[1]) * ((tt - 0.55) / 0.45)
        rgb[i, :, :] = c
    # soft reflected light near the bottom edge of the orb
    yy, xx = np.mgrid[0:hh, 0:w * SS]
    refl = np.exp(-(((xx / SS - cx) / (r2 * 0.75)) ** 2 + ((yy / SS - (cy + r2 * 0.80)) / (r2 * 0.34)) ** 2)) * 0.55
    rgb = rgb * (1 - refl[..., None]) + np.array(c_top, float) * refl[..., None]
    gradimg = Image.fromarray(np.dstack([rgb.clip(0, 255), np.full((hh, w * SS), 255.0)]).astype(np.uint8), "RGBA")
    layers.append(masked(gradimg, orb))
    # inner dark rim for depth
    er = orb.filter(ImageFilter.MinFilter(S(1.6) | 1))
    rimarr = np.clip(np.array(orb).astype(int) - np.array(er).astype(int), 0, 255).astype(np.uint8)
    rim = Image.new("RGBA", (w * SS, h * SS), (255, 255, 255, 0))
    rim.putalpha(Image.fromarray((rimarr * 0.55).astype(np.uint8), "L"))
    layers.append(rim)
    # specular gloss ellipse on the upper half
    spec = ellipse_mask(w, h, cx - r2 * 0.80, cy - r2 * 0.92, cx + r2 * 0.80, cy - r2 * 0.04)
    sg = gradient_layer(w * SS, h * SS, (255, 255, 255), (255, 255, 255), 215, 20)
    layers.append(masked(sg, spec))
    # glyph
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    gy = cy + (1 if pressed else 0)
    if kind == "photo":
        d.ellipse([S(cx - 11), S(gy - 11), S(cx + 11), S(gy + 11)], outline=(255, 255, 255, 235), width=S(2.6))
        d.ellipse([S(cx - 5), S(gy - 5), S(cx + 5), S(gy + 5)], fill=(255, 255, 255, 235))
    elif kind == "video":
        d.ellipse([S(cx - 9), S(gy - 9), S(cx + 9), S(gy + 9)], fill=(255, 255, 255, 240))
    else:
        d.rounded_rectangle([S(cx - 9), S(gy - 9), S(cx + 9), S(gy + 9)], radius=S(3), fill=(255, 255, 255, 240))
    layers.append(lay)
    return finish(base, layers, w, h)

def capsule(bg, x, y, w, h, selected):
    """PHOTO | VIDEO switch; selected = 0 (photo) or 1 (video)"""
    base = crop_bg(bg, x, y, w, h)
    mask = rr_mask(w, h, 2, 2, w - 2, h - 3, (h - 5) / 2)
    layers = [drop_shadow(mask, w, h, 0, 1.2, 1.4, 0.40)]
    layers += glass_shape(w, h, mask, (236, 250, 255), (170, 222, 244), rim_alpha=230, gloss=0.35)
    half = w / 2
    sel_x0 = 3 if selected == 0 else half
    sel_x1 = half if selected == 0 else w - 3
    selmask = rr_mask(w, h, sel_x0, 3, sel_x1, h - 4, (h - 7) / 2)
    if selected == 1:
        top, bot = (255, 150, 126), (214, 40, 46)
    else:
        top, bot = (120, 226, 255), (22, 130, 214)
    layers += glass_shape(w, h, selmask, top, bot, rim_alpha=210, gloss=0.5)
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    glyph_camera(d, 13, h / 2 + 0.5, (255, 255, 255) if selected == 0 else TEAL_TEXT, 0.8)
    glyph_film(d, half + 12, h / 2 + 0.5, (255, 255, 255) if selected == 1 else TEAL_TEXT, 0.8)
    layers.append(lay)
    layers.append(text_layer(w, h, (half / 2 + 11, h / 2 + 0.5), "PHOTO", 8.4, (255, 255, 255) if selected == 0 else TEAL_TEXT, anchor="mm"))
    layers.append(text_layer(w, h, (half + half / 2 + 11, h / 2 + 0.5), "VIDEO", 8.4, (255, 255, 255) if selected == 1 else TEAL_TEXT, anchor="mm"))
    return finish(base, layers, w, h)

def dialog_panel(bg, x, y, w, h):
    base = crop_bg(bg, x, y, w, h)
    mask = rr_mask(w, h, 3, 3, w - 3, h - 5, 14)
    layers = [drop_shadow(mask, w, h, 0, 3, 3.5, 0.55)]
    layers += glass_shape(w, h, mask, (255, 255, 255), (178, 232, 252), rim_alpha=255, gloss=0.38)
    return finish(base, layers, w, h)

def status_plate(bg, x, y, w, h):
    base = crop_bg(bg, x, y, w, h)
    mask = rr_mask(w, h, 1, 1, w - 1, h - 2, 8)
    layers = glass_shape(w, h, mask, (255, 255, 255), (214, 244, 255), rim_alpha=200, gloss=0.25)
    return finish(base, layers, w, h)

# ---------------------------------------------------------------------------------------------- top-screen sprites (alpha)
def sprite_bracket():
    w = h = 32  # hardware sprite size; the bracket itself uses the top-left 24x24
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    # soft shadow first
    sh = canvas(w, h)
    ds = ImageDraw.Draw(sh)
    for dd, col, wd in ((1.2, (0, 40, 80, 120), 4.6), (0, (255, 255, 255, 255), 3.0)):
        tgt = ds if dd else d
        tgt.line([(S(3 + dd), S(21 + dd)), (S(3 + dd), S(6 + dd)), (S(6 + dd), S(3 + dd)), (S(21 + dd), S(3 + dd))], fill=col, width=S(wd), joint="curve")
    d.line([(S(3), S(21)), (S(3), S(7)), (S(7), S(3)), (S(21), S(3))], fill=(255, 255, 255, 255), width=S(3.0), joint="curve")
    d.line([(S(3), S(20)), (S(3), S(8)), (S(8), S(3)), (S(20), S(3))], fill=(190, 240, 255, 255), width=S(1.2), joint="curve")
    sh = sh.filter(ImageFilter.GaussianBlur(S(0.8)))
    return down(Image.alpha_composite(sh, lay))

def sprite_pill(label, kind, w=64, h=16):
    lay = canvas(w, 32)
    mask = rr_mask(w, 32, 0.5, 0.5, w - 0.5, h - 0.5, (h - 1) / 2)
    if kind == "photo" or kind == "inner" or kind == "outer":
        top, bot = (150, 232, 255), (24, 130, 214)
    elif kind == "video":
        top, bot = (255, 160, 136), (206, 36, 44)
    layers = glass_shape(w, 32, mask, top, bot, rim_alpha=255, gloss=0.5, bbox=(0, 0, w * SS, h * SS))
    t = canvas(w, 32)
    d = ImageDraw.Draw(t)
    if kind == "photo":
        glyph_camera(d, 9, h / 2 + 0.5, (255, 255, 255), 0.62)
    elif kind == "video":
        glyph_film(d, 9, h / 2 + 0.5, (255, 255, 255), 0.62)
    elif kind in ("inner", "outer"):
        # a tiny face / landscape dot
        if kind == "inner":
            d.ellipse([S(6), S(3.5), S(12), S(9.5)], fill=(255, 255, 255, 255))
            d.pieslice([S(3.5), S(9.2), S(14.5), S(18.5)], 180, 360, fill=(255, 255, 255, 255))
        else:
            d.polygon([(S(3.5), S(12.5)), (S(7.5), S(6.5)), (S(10), S(10)), (S(12), S(8)), (S(15), S(12.5))], fill=(255, 255, 255, 255))
            d.ellipse([S(10.5), S(3.2), S(13.5), S(6.2)], fill=(255, 255, 255, 255))
    layers.append(t)
    layers.append(text_layer(w, 32, (w / 2 + 6, h / 2 + 0.5), label, 8.6, (255, 255, 255), shadow=(0.6, 0.6, (0, 50, 100, 200)), anchor="mm"))
    return down(compose(layers, w, 32))

def sprite_rec_plate():
    w = 64
    h = 16
    lay = canvas(w, 32)
    mask = rr_mask(w, 32, 0.5, 0.5, w - 0.5, h - 0.5, (h - 1) / 2)
    layers = glass_shape(w, 32, mask, (60, 70, 80), (18, 24, 32), rim_alpha=210, gloss=0.22, bbox=(0, 0, w * SS, h * SS))
    return down(compose(layers, w, 32))

def sprite_rec_dot():
    w = h = 16
    lay = canvas(w, h)
    d = ImageDraw.Draw(lay)
    halo = canvas(w, h)
    dh = ImageDraw.Draw(halo)
    dh.ellipse([S(2), S(2), S(14), S(14)], fill=(255, 40, 40, 200))
    halo = halo.filter(ImageFilter.GaussianBlur(S(1.4)))
    d.ellipse([S(4.2), S(4.2), S(11.8), S(11.8)], fill=(255, 60, 52, 255), outline=(255, 200, 190, 255), width=S(0.8))
    d.ellipse([S(5.6), S(5.0), S(8.4), S(7.4)], fill=(255, 255, 255, 190))
    return down(Image.alpha_composite(halo, lay))

def digits_atlas():
    """11 glyphs (0-9 and ':'), 8x12, white with a soft shadow, drawn on a transparent background"""
    gw, gh = 8, 12
    out = Image.new("RGBA", (gw * 11, gh), (0, 0, 0, 0))
    for i, ch in enumerate("0123456789:"):
        lay = canvas(gw, gh)
        d = ImageDraw.Draw(lay)
        f = font(FONT_BOLD, S(11))
        d.text((S(gw / 2), S(gh / 2 + 0.3)), ch, font=f, fill=(255, 255, 255, 255), anchor="mm")
        out.paste(down(lay), (i * gw, 0))
    return out

# ---------------------------------------------------------------------------------------------- icon
def make_icon():
    S2 = 8
    W = H = 32
    big = Image.new("RGBA", (W * S2, H * S2), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    def r(v): return int(round(v * S2))
    # rounded glass tile
    tile = Image.new("L", (W * S2, H * S2), 0)
    ImageDraw.Draw(tile).rounded_rectangle([r(1), r(1), r(31), r(31)], radius=r(7), fill=255)
    grad = vgrad(W * S2, H * S2, (140, 232, 255), (28, 140, 220)).convert("RGBA")
    big.paste(grad, (0, 0), tile)
    # camera body
    d.rounded_rectangle([r(5), r(10), r(27), r(25)], radius=r(3.2), fill=(250, 253, 255, 255), outline=(10, 80, 130, 255), width=r(1.0))
    d.rounded_rectangle([r(7.5), r(7), r(14.5), r(11.5)], radius=r(1.6), fill=(250, 253, 255, 255), outline=(10, 80, 130, 255), width=r(0.9))
    # lens
    d.ellipse([r(10.2), r(11.8), r(21.8), r(23.4)], fill=(10, 80, 130, 255))
    d.ellipse([r(11.6), r(13.2), r(20.4), r(22.0)], fill=(30, 160, 230, 255))
    d.ellipse([r(13.0), r(14.4), r(17.0), r(18.2)], fill=(200, 245, 255, 255))
    d.ellipse([r(15.6), r(19.2), r(18.0), r(21.2)], fill=(120, 220, 250, 255))
    # flash + shutter button
    d.rounded_rectangle([r(22.4), r(12), r(25.2), r(14.4)], radius=r(0.8), fill=(255, 190, 70, 255))
    # bubbles
    d.ellipse([r(3.2), r(3.6), r(7.6), r(8.0)], outline=(255, 255, 255, 255), width=r(0.8))
    d.ellipse([r(24.4), r(4.4), r(27.4), r(7.4)], outline=(255, 255, 255, 255), width=r(0.7))
    # gloss on the tile
    gl = Image.new("RGBA", (W * S2, H * S2), (0, 0, 0, 0))
    ImageDraw.Draw(gl).pieslice([r(-6), r(-18), r(38), r(14)], 0, 180, fill=(255, 255, 255, 70))
    gl.putalpha(Image.fromarray((np.array(gl)[:, :, 3].astype(int) * np.array(tile).astype(int) // 255).astype(np.uint8), "L"))
    big = Image.alpha_composite(big, gl)
    img = big.resize((W, H), Image.LANCZOS)
    # flatten on a key colour for the transparent corners (palette index 0 is transparent for banners)
    key = (255, 0, 255)
    flat = Image.new("RGB", (W, H), key)
    flat.paste(img.convert("RGB"), (0, 0), img.split()[3].point(lambda v: 255 if v > 110 else 0))
    pal = flat.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    # make the key colour palette entry 0
    p = pal.getpalette()[:48]
    kidx = None
    best = 1e9
    for i in range(16):
        dist = sum((p[i * 3 + c] - key[c]) ** 2 for c in range(3))
        if dist < best:
            best, kidx = dist, i
    if kidx != 0:
        remap = list(range(16)); remap[0], remap[kidx] = kidx, 0
        arr = np.array(pal)
        lut = np.array(remap, np.uint8)
        arr = lut[arr]
        p2 = list(p)
        for c in range(3):
            p2[0 * 3 + c], p2[kidx * 3 + c] = p[kidx * 3 + c], p[0 * 3 + c]
        pal = Image.fromarray(arr, "P"); pal.putpalette(p2 + [0] * (768 - 48))
        p = p2
    return pal, p

def write_bmp_4bpp(pal_img, palette, path):
    W, H = pal_img.size
    px = np.array(pal_img).astype(np.uint8)
    row_bytes = ((W * 4 + 31) // 32) * 4
    data = bytearray()
    for y in range(H - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(0, W, 2):
            row[x // 2] = (int(px[y, x]) << 4) | int(px[y, x + 1])
        data += row
    pal_bytes = bytearray()
    for i in range(16):
        r, g, b = palette[i * 3:i * 3 + 3]
        pal_bytes += bytes([b, g, r, 0])
    off = 14 + 40 + 64
    hdr = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 4, 0, len(data), 2835, 2835, 16, 16)
    open(path, "wb").write(hdr + info + bytes(pal_bytes) + bytes(data))

# ---------------------------------------------------------------------------------------------- pack
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], float) / 16.0

def to_rgb555(img, dither=True):
    """RGB(A) image -> list of u16. Alpha < 128 -> 0 (transparent); everything else has bit 15 set."""
    arr = np.array(img.convert("RGBA")).astype(float)
    h, w = arr.shape[:2]
    if dither:
        thr = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
    else:
        thr = np.full((h, w), 0.5)
    ch = []
    for c in range(3):
        v = arr[:, :, c] / 255.0 * 31.0
        q = np.floor(v + thr * 1.0 - 0.0)
        ch.append(np.clip(q, 0, 31).astype(np.uint16))
    out = ch[0] | (ch[1] << 5) | (ch[2] << 10) | 0x8000
    out = np.where(arr[:, :, 3] >= 128, out, 0).astype(np.uint16)
    return out

class Pack:
    def __init__(self):
        self.items = []  # (name, w, h, data)
    def add(self, name, img, dither=True):
        a = to_rgb555(img, dither)
        self.items.append((name, a.shape[1], a.shape[0], a))
        return img

def main():
    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    os.makedirs(PREVIEW, exist_ok=True)
    bg = make_bg()
    pack = Pack()
    pack.add("BG", bg)

    # --- layout (bottom screen, 256x192) -------------------------------------------------------
    L = dict(
        shutter=(80, 52, 96),   # x, y, size
        album=(14, 70, 56, 62),
        flip=(186, 70, 56, 62),
        back=(8, 156, 76, 30),
        capsule=(140, 3, 112, 26),
        status=(92, 158, 156, 26),
        prev=(4, 70, 56, 62), play=(68, 70, 56, 62), delete=(132, 70, 56, 62), next=(196, 70, 56, 62),
        dialog=(24, 44, 208, 104),
        dlg_yes=(40, 108, 84, 30), dlg_no=(132, 108, 84, 30),
        gear=(8, 33, 64, 22), fps=(184, 33, 64, 22), row=(20, 32, 216, 22),
    )
    # title in the bar
    bar = canvas(256, 192)
    bg_titled = bg.convert("RGBA")
    bg_titled = Image.alpha_composite(bg_titled, down(compose([text_layer(256, 192, (10, 15.5), "nerdMod", 13.5, DEEP, shadow=(0.8, 0.8, (255, 255, 255, 230)), anchor="lm"),
                                                     text_layer(256, 192, (80, 15.5), "Camera", 12, (60, 150, 200), bold=False, shadow=(0.8, 0.8, (255, 255, 255, 230)), anchor="lm")], 256, 192))).convert("RGB")
    pack.items[0] = ("BG", 256, 192, to_rgb555(bg_titled))
    bg = bg_titled

    x, y, s = L["shutter"]
    for name, kind, pr, gl in (("SHUTTER_PHOTO", "photo", False, False), ("SHUTTER_PHOTO_P", "photo", True, False),
                               ("SHUTTER_VIDEO", "video", False, False), ("SHUTTER_VIDEO_P", "video", True, False),
                               ("SHUTTER_REC", "rec", False, False), ("SHUTTER_REC_GLOW", "rec", False, True), ("SHUTTER_REC_P", "rec", True, False)):
        pack.add(name, shutter(bg, x, y, s, kind, pr, gl))

    x, y, w, h = L["album"]
    pack.add("BTN_ALBUM", square_button(bg, x, y, w, h, glyph_album, "ALBUM"))
    pack.add("BTN_ALBUM_P", square_button(bg, x, y, w, h, glyph_album, "ALBUM", pressed=True))
    x, y, w, h = L["flip"]
    pack.add("BTN_FLIP", square_button(bg, x, y, w, h, glyph_flip, "FLIP"))
    pack.add("BTN_FLIP_P", square_button(bg, x, y, w, h, glyph_flip, "FLIP", pressed=True))
    x, y, w, h = L["back"]
    pack.add("BTN_BACK", pill_button(bg, x, y, w, h, lambda d, cx, cy, c: glyph_chev(d, cx, cy, c, True, 7, 2.8), "BACK"))
    pack.add("BTN_BACK_P", pill_button(bg, x, y, w, h, lambda d, cx, cy, c: glyph_chev(d, cx, cy, c, True, 7, 2.8), "BACK", pressed=True))
    x, y, w, h = L["capsule"]
    pack.add("CAPSULE_PHOTO", capsule(bg, x, y, w, h, 0))
    pack.add("CAPSULE_VIDEO", capsule(bg, x, y, w, h, 1))
    x, y, w, h = L["status"]
    pack.add("STATUS_PLATE", status_plate(bg, x, y, w, h))

    # album screen buttons
    for key, glyph, label, tint in (("prev", lambda d, cx, cy, c: glyph_chev(d, cx, cy, c, True, 10, 3.4), "PREV", None),
                                    ("next", lambda d, cx, cy, c: glyph_chev(d, cx, cy, c, False, 10, 3.4), "NEXT", None),
                                    ("delete", glyph_trash, "DELETE", "red"),
                                    ("play", lambda d, cx, cy, c: glyph_play(d, cx + 1, cy, c, 11), "PLAY", None)):
        x, y, w, h = L[key]
        pack.add("BTN_" + key.upper(), square_button(bg, x, y, w, h, glyph, label, tint=tint))
        pack.add("BTN_" + key.upper() + "_P", square_button(bg, x, y, w, h, glyph, label, pressed=True, tint=tint))
    x, y, w, h = L["play"]
    pack.add("BTN_PLAY_OFF", square_button(bg, x, y, w, h, lambda d, cx, cy, c: glyph_play(d, cx + 1, cy, c, 11), "PLAY", disabled=True))
    pack.add("BTN_PAUSE", square_button(bg, x, y, w, h, glyph_pause, "PAUSE"))
    pack.add("BTN_PAUSE_P", square_button(bg, x, y, w, h, glyph_pause, "PAUSE", pressed=True))

    # dialog
    x, y, w, h = L["dialog"]
    pack.add("DIALOG", dialog_panel(bg, x, y, w, h))
    # dialog buttons are drawn over the dialog panel
    panel_full = bg.copy()
    panel_full.paste(dialog_panel(bg, x, y, w, h), (x, y))
    x, y, w, h = L["dlg_yes"]
    pack.add("DLG_YES", pill_button(panel_full, x, y, w, h, lambda d, cx, cy, c: glyph_trash(d, cx, cy + 0.5, (170, 60, 56)), "DELETE"))
    pack.add("DLG_YES_P", pill_button(panel_full, x, y, w, h, lambda d, cx, cy, c: glyph_trash(d, cx, cy + 0.5, (170, 60, 56)), "DELETE", pressed=True))
    x, y, w, h = L["dlg_no"]
    pack.add("DLG_NO", pill_button(panel_full, x, y, w, h, lambda d, cx, cy, c: glyph_chev(d, cx, cy, c, True, 7, 2.8), "CANCEL"))
    pack.add("DLG_NO_P", pill_button(panel_full, x, y, w, h, lambda d, cx, cy, c: glyph_chev(d, cx, cy, c, True, 7, 2.8), "CANCEL", pressed=True))

    # top-screen sprites
    pack.add("SPR_BRACKET", sprite_bracket(), dither=False)
    pack.add("SPR_INNER", sprite_pill("INNER", "inner"), dither=False)
    pack.add("SPR_OUTER", sprite_pill("OUTER", "outer"), dither=False)
    pack.add("SPR_PHOTO", sprite_pill("PHOTO", "photo"), dither=False)
    pack.add("SPR_VIDEO", sprite_pill("VIDEO", "video"), dither=False)
    pack.add("SPR_RECPLATE", sprite_rec_plate(), dither=False)
    pack.add("SPR_RECDOT", sprite_rec_dot(), dither=False)
    pack.add("DIGITS", digits_atlas(), dither=False)

    # Phase 2D: settings gear, FPS pills and settings-menu rows (appended so the earlier ids stay stable)
    x, y, w, h = L["gear"]
    pack.add("BTN_GEAR", row_button(bg, x, y, w, h, lambda d, cx, cy, c: glyph_gear(d, cx, cy, c, 6.5), "SET"))
    pack.add("BTN_GEAR_P", pill_button(bg, x, y, w, h, lambda d, cx, cy, c: glyph_gear(d, cx, cy, c, 6.5), "SET", pressed=True))
    x, y, w, h = L["fps"]
    for f in (10, 15, 20, 30):
        pack.add("BTN_FPS%d" % f, row_button(bg, x, y, w, h, None, "FPS: %d" % f))
    pack.add("BTN_FPS_P", pill_button(bg, x, y, w, h, lambda d, cx, cy, c: None, "FPS", pressed=True))
    x, y, w, h = L["row"]
    for i in range(5):
        pack.add("ROW%d" % i, row_button(bg, x, y + 24 * i, w, h))

    # Phase 2D: INFO button of the video player (same slot as the album's DELETE, which the player does not use)
    x, y, w, h = L["delete"]
    pack.add("BTN_INFO", square_button(bg, x, y, w, h, glyph_info, "INFO"))
    pack.add("BTN_INFO_P", square_button(bg, x, y, w, h, glyph_info, "INFO", pressed=True))

    # ---- write bin + header
    blob = bytearray()
    lines = ["// Generated by camera/tools/genassets.py - do not edit.", "#pragma once", "#include <stdint.h>", "",
             "struct UiImage { uint32_t offset; uint16_t w, h; };  // offset in u16 units into ui_bin", ""]
    names = []
    for (name, w, h, arr) in pack.items:
        off = len(blob) // 2
        blob += arr.astype("<u2").tobytes()
        names.append((name, off, w, h))
    lines.append("enum UiId {")
    for i, (name, off, w, h) in enumerate(names):
        lines.append(f"\tUI_{name} = {i},")
    lines.append("\tUI_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("static const UiImage UI_IMAGES[UI_COUNT] = {")
    for (name, off, w, h) in names:
        lines.append(f"\t{{{off}, {w}, {h}}}, // {name}")
    lines.append("};")
    lines.append("")
    lines.append("// Bottom-screen layout (x, y, w, h)")
    for k, v in L.items():
        if len(v) == 3:
            v = (v[0], v[1], v[2], v[2])
        lines.append(f"#define UI_RECT_{k.upper()} {v[0]}, {v[1]}, {v[2]}, {v[3]}")
    open(OUT_BIN, "wb").write(bytes(blob))
    open(OUT_H, "w").write("\n".join(lines) + "\n")
    print("ui.bin", len(blob), "bytes,", len(names), "images")

    # ---- previews
    def img_of(name):
        for (n, w, h, arr) in pack.items:
            if n == name:
                a = arr.astype(np.uint32)
                rgb = np.zeros((h, w, 4), np.uint8)
                rgb[:, :, 0] = ((a & 31) * 255 // 31)
                rgb[:, :, 1] = (((a >> 5) & 31) * 255 // 31)
                rgb[:, :, 2] = (((a >> 10) & 31) * 255 // 31)
                rgb[:, :, 3] = np.where(a & 0x8000, 255, 0)
                return Image.fromarray(rgb, "RGBA")
        raise KeyError(name)

    def screen(buttons):
        base = img_of("BG").convert("RGB")
        for name, key in buttons:
            x, y = L[key][0], L[key][1]
            im = img_of(name)
            base.paste(im.convert("RGB"), (x, y))
        return base

    screen([("SHUTTER_PHOTO", "shutter"), ("BTN_ALBUM", "album"), ("BTN_FLIP", "flip"), ("BTN_BACK", "back"), ("CAPSULE_PHOTO", "capsule"), ("STATUS_PLATE", "status")]).resize((768, 576), Image.NEAREST).save(os.path.join(PREVIEW, "bottom_photo.png"))
    screen([("SHUTTER_REC_GLOW", "shutter"), ("BTN_ALBUM_P", "album"), ("BTN_FLIP", "flip"), ("BTN_BACK", "back"), ("CAPSULE_VIDEO", "capsule"), ("STATUS_PLATE", "status")]).resize((768, 576), Image.NEAREST).save(os.path.join(PREVIEW, "bottom_video_rec.png"))
    screen([("BTN_PREV", "prev"), ("BTN_PLAY", "play"), ("BTN_DELETE", "delete"), ("BTN_NEXT", "next"), ("BTN_BACK", "back"), ("STATUS_PLATE", "status")]).resize((768, 576), Image.NEAREST).save(os.path.join(PREVIEW, "bottom_album.png"))
    d = screen([("DIALOG", "dialog"), ("DLG_YES", "dlg_yes"), ("DLG_NO", "dlg_no")])
    d.resize((768, 576), Image.NEAREST).save(os.path.join(PREVIEW, "bottom_dialog.png"))

    # top screen mock: noisy "video" with the overlays
    top = vgrad(256, 192, (90, 130, 90), (40, 60, 90)).convert("RGBA")
    br = img_of("SPR_BRACKET")
    for (px, py, fx, fy) in ((6, 6, 0, 0), (226, 6, 1, 0), (6, 162, 0, 1), (226, 162, 1, 1)):
        b = br
        if fx: b = b.transpose(Image.FLIP_LEFT_RIGHT)
        if fy: b = b.transpose(Image.FLIP_TOP_BOTTOM)
        top.alpha_composite(b, (px, py))
    top.alpha_composite(img_of("SPR_OUTER"), (12, 12))
    top.alpha_composite(img_of("SPR_VIDEO"), (180, 12))
    top.alpha_composite(img_of("SPR_RECPLATE"), (96, 12))
    top.alpha_composite(img_of("SPR_RECDOT"), (99, 12))
    top.convert("RGB").resize((768, 576), Image.NEAREST).save(os.path.join(PREVIEW, "top_overlay.png"))

    # icon
    pal_img, p = make_icon()
    write_bmp_4bpp(pal_img, p, OUT_ICON)
    prev = pal_img.convert("RGB").resize((256, 256), Image.NEAREST)
    prev.save(os.path.join(PREVIEW, "icon.png"))
    print("done")

if __name__ == "__main__":
    main()
