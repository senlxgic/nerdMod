#!/usr/bin/env python3
"""
Generates the nerdMod branding art (original artwork, soft aqua / blue / white):

  title/nitrofiles/graphics/logo_nerdmod.png   256x192 splash logo (shown by the title app)
  booter/icon.bmp, booter_fc/icon.bmp          32x32 4bpp launcher icons (palette index 0 = transparent key)
  resources/branding/preview/*.png             previews

    python3 resources/branding/genbranding.py

It reuses the drawing helpers of camera/tools/genassets.py.
"""
import importlib.util
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

spec = importlib.util.spec_from_file_location("genassets", os.path.join(REPO, "camera", "tools", "genassets.py"))
ga = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ga)

S, SS = ga.S, ga.SS
PREVIEW = os.path.join(HERE, "preview")


def sky():
    W, H = 256, 192
    img = ga.vgrad(W, H, (196, 242, 255), (84, 190, 238)).convert("RGBA")
    arr = np.array(img).astype(float)
    yy, xx = np.mgrid[0:H, 0:W]
    glow = np.exp(-(((xx - 128) / 120.0) ** 2 + ((yy - 84) / 70.0) ** 2)) * 0.55
    arr[:, :, :3] = arr[:, :, :3] * (1 - glow[:, :, None]) + 255 * glow[:, :, None]
    g = np.clip((yy - 138) / 54.0, 0, 1) ** 1.6 * 0.5
    arr[:, :, :3] = arr[:, :, :3] * (1 - g[:, :, None]) + np.array(ga.GREEN) * g[:, :, None]
    img = Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGBA")
    bub = ga.canvas(W, H)
    d = ImageDraw.Draw(bub)
    spots = [(26, 36, 10), (52, 22, 5), (230, 40, 8), (208, 24, 4), (18, 120, 7), (238, 132, 11), (250, 78, 4), (8, 84, 5),
             (66, 160, 6), (190, 164, 7), (120, 24, 3), (150, 170, 4), (40, 178, 5), (226, 176, 5), (100, 172, 3)]
    for (cx, cy, r) in spots:
        d.ellipse([S(cx - r), S(cy - r), S(cx + r), S(cy + r)], fill=(255, 255, 255, 40), outline=(255, 255, 255, 170), width=max(1, S(0.7)))
        d.ellipse([S(cx - r * 0.55), S(cy - r * 0.7), S(cx - r * 0.05), S(cy - r * 0.25)], fill=(255, 255, 255, 180))
    return Image.alpha_composite(img, ga.down(bub))


def make_logo():
    W, H = 256, 192
    base = sky()
    # glass plate
    x0, y0, x1, y1 = 22, 52, 234, 124
    mask = ga.rr_mask(W, H, x0, y0, x1, y1, 26)
    layers = ga.glass_shape(W, H, mask, (255, 255, 255), (150, 224, 250), rim_alpha=230, gloss=0.5)
    plate = ga.compose([ga.drop_shadow(mask, W, H, 0, 3, 4.5, 0.45)] + layers, W, H)
    base = Image.alpha_composite(base, ga.down(plate))

    # wordmark
    txt = ga.canvas(W, H)
    d = ImageDraw.Draw(txt)
    f = ga.font(ga.FONT_BOLD, S(36))
    word = "nerdMod"
    bb = d.textbbox((0, 0), word, font=f)
    tw, th = (bb[2] - bb[0]) / SS, (bb[3] - bb[1]) / SS
    tx, ty = (W - tw) / 2 - bb[0] / SS, 88 - th / 2 - bb[1] / SS
    # "nerd" in deep blue, "Mod" in aqua-blue
    half = d.textlength("nerd", font=f) / SS
    d.text((S(tx + 1), S(ty + 2)), "nerd", font=f, fill=(255, 255, 255, 220))
    d.text((S(tx + half + 1), S(ty + 2)), "Mod", font=f, fill=(255, 255, 255, 220))
    d.text((S(tx), S(ty)), "nerd", font=f, fill=(8, 84, 140, 255))
    d.text((S(tx + half), S(ty)), "Mod", font=f, fill=(20, 150, 215, 255))
    base = Image.alpha_composite(base, ga.down(txt))

    # subtitle (attribution) under the plate
    sub = ga.canvas(W, H)
    d = ImageDraw.Draw(sub)
    f2 = ga.font(ga.FONT_REG, S(10))
    s = "Based on TWiLight Menu++"
    w2 = d.textlength(s, font=f2) / SS
    d.text((S((W - w2) / 2 + 0.7), S(139.7)), s, font=f2, fill=(255, 255, 255, 200))
    d.text((S((W - w2) / 2), S(139)), s, font=f2, fill=(10, 80, 130, 255))
    base = Image.alpha_composite(base, ga.down(sub))
    return base.convert("RGB")


def make_launcher_icon():
    S2 = 8
    W = H = 32
    big = Image.new("RGBA", (W * S2, H * S2), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    r = lambda v: int(round(v * S2))
    tile = Image.new("L", (W * S2, H * S2), 0)
    ImageDraw.Draw(tile).rounded_rectangle([r(1), r(1), r(31), r(31)], radius=r(7), fill=255)
    grad = ga.vgrad(W * S2, H * S2, (150, 236, 255), (30, 150, 224)).convert("RGBA")
    big.paste(grad, (0, 0), tile)
    f = ga.font(ga.FONT_BOLD, r(21))
    bb = d.textbbox((0, 0), "n", font=f)
    d.text((r(16) - (bb[0] + bb[2]) / 2, r(17) - (bb[1] + bb[3]) / 2), "n", font=f, fill=(8, 84, 140, 255))
    d.ellipse([r(3.4), r(3.6), r(7.4), r(7.6)], outline=(255, 255, 255, 255), width=r(0.8))
    d.ellipse([r(24.6), r(23.8), r(27.8), r(27.0)], outline=(255, 255, 255, 255), width=r(0.7))
    gl = Image.new("RGBA", (W * S2, H * S2), (0, 0, 0, 0))
    ImageDraw.Draw(gl).pieslice([r(-6), r(-18), r(38), r(14)], 0, 180, fill=(255, 255, 255, 70))
    gl.putalpha(Image.fromarray((np.array(gl)[:, :, 3].astype(int) * np.array(tile).astype(int) // 255).astype(np.uint8), "L"))
    big = Image.alpha_composite(big, gl)
    img = big.resize((W, H), Image.LANCZOS)
    key = (255, 0, 255)
    flat = Image.new("RGB", (W, H), key)
    flat.paste(img.convert("RGB"), (0, 0), img.split()[3].point(lambda v: 255 if v > 110 else 0))
    pal = flat.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    p = pal.getpalette()[:48]
    kidx, best = None, 1e9
    for i in range(16):
        dist = sum((p[i * 3 + c] - key[c]) ** 2 for c in range(3))
        if dist < best:
            best, kidx = dist, i
    if kidx != 0:
        remap = list(range(16))
        remap[0], remap[kidx] = kidx, 0
        arr = np.array(remap, np.uint8)[np.array(pal)]
        p2 = list(p)
        for c in range(3):
            p2[c], p2[kidx * 3 + c] = p[kidx * 3 + c], p[c]
        pal = Image.fromarray(arr, "P")
        pal.putpalette(p2 + [0] * (768 - 48))
        p = p2
    return pal, p


def main():
    os.makedirs(PREVIEW, exist_ok=True)
    logo = make_logo()
    out = os.path.join(REPO, "title", "nitrofiles", "graphics", "logo_nerdmod.png")
    logo.save(out, optimize=True)
    logo.resize((768, 576), Image.NEAREST).save(os.path.join(PREVIEW, "logo_nerdmod.png"))
    pal, p = make_launcher_icon()
    for rel in ("booter/icon.bmp", "booter_fc/icon.bmp"):
        ga.write_bmp_4bpp(pal, p, os.path.join(REPO, rel))
    Image.open(os.path.join(REPO, "booter", "icon.bmp")).convert("RGBA").resize((256, 256), Image.NEAREST).save(os.path.join(PREVIEW, "launcher_icon.png"))
    print("done")


if __name__ == "__main__":
    main()
