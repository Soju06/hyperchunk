#!/usr/bin/env python3
"""hyperchunk brand banner generator (v17 spec, owner-approved 2026-08-19).

Renders the README banner from the real demo tile (B-6/VIZ-4 measured r.0.0
top view) — the artwork IS generator output, no stock graphics.

Usage:
  python3 make_banner.py --theme ivory --out banner.png
  python3 make_banner.py --theme dark  --out banner-dark.png

Requires: Pillow, numpy; fonts in ~/.local/share/fonts/brand/
(SpaceGroteskVar.ttf, Inter-Regular.ttf). Tile: demo/tiles/hyperchunk-free.png.
"""
import argparse, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
FB = os.path.expanduser("~/.local/share/fonts/brand/")
W, H = 1600, 410
# v17 crop (owner pick): zoom7, x0=0, y0=160, art_w=790 — water 37%, coastal
# detail band (lagoon/sandbar/headland/diagonal channel) fully opaque.
ZOOM, X0, Y0, ART_W = 7, 0, 160, 790
FSTART, FLEN = 60, 200  # smoothstep alpha ramp, linear-light composite

THEMES = {
    "ivory": {"bg": (246, 242, 234), "ink": (40, 36, 32), "sub": (104, 98, 90), "art_blend": 0.05},
    "dark":  {"bg": (14, 12, 11),   "ink": (238, 235, 228), "sub": (160, 154, 146), "art_blend": 0.12},
}

def sg(size, wght=640):
    f = ImageFont.truetype(FB + "SpaceGroteskVar.ttf", size)
    try: f.set_variation_by_axes([wght])
    except Exception: pass
    return f

def inter(size):
    return ImageFont.truetype(FB + "Inter-Regular.ttf", size)

def water_mask(img):
    a = np.asarray(img).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return (b > r + 12) & (b > g + 8)

def make_mark(d, x, y, s, color):
    """Brand mark: 2x2 chunk grid, last cell snapping in half-a-cell offset (= bit-exact moment)."""
    s = int(s); g = max(4, round(s * 0.12)); c = (s - g) // 2; g = s - 2 * c
    r = max(3, round(c * 0.16))
    for (cx, cy) in [(0, 0), (0, 1), (1, 1)]:
        x0 = int(x + cx * (c + g)); y0 = int(y + cy * (c + g))
        d.rounded_rectangle([x0, y0, x0 + c, y0 + c], radius=r, fill=color)
    off = round(c * 0.5)
    x0 = int(x + (c + g) + off); y0 = int(y - off)
    d.rounded_rectangle([x0, y0, x0 + c, y0 + c], radius=r, fill=color)

def to_lin(x): return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)
def to_srgb(x): return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theme", choices=THEMES, default="ivory")
    ap.add_argument("--tile", default=os.path.join(HERE, "..", "demo", "tiles", "hyperchunk-free.png"))
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    th = THEMES[args.theme]

    tile = Image.open(args.tile).convert("RGB")
    big = tile.resize((352 * ZOOM, 352 * ZOOM), Image.NEAREST)
    tc = big.crop((X0, Y0, X0 + ART_W, Y0 + H))
    tc = ImageEnhance.Color(tc).enhance(1.12)              # saturate greens
    a = np.asarray(tc).astype(float); m = water_mask(tc)
    a[m] = np.clip(a[m] * 1.10 + 8, 0, 255)                # lift ocean darkness
    tc = Image.fromarray(a.astype(np.uint8))
    tc = Image.blend(tc, Image.new("RGB", tc.size, th["bg"]), th["art_blend"])

    # linear-light lerp to exact bg (kills gray haze), smoothstep ramp, uniform all rows
    arr = to_lin(np.asarray(tc).astype(float) / 255.0)
    bg = to_lin(np.array(th["bg"], dtype=float)[None, None, :] / 255.0)
    xs = np.arange(ART_W, dtype=float)
    t = np.clip((xs - FSTART) / FLEN, 0, 1)
    alpha = (t * t * (3 - 2 * t))[None, :, None]
    comp = to_srgb(arr * alpha + bg * (1 - alpha)) * 255.0

    im = Image.new("RGB", (W, H), th["bg"])
    im.paste(Image.fromarray(comp.astype(np.uint8)), (W - ART_W, 0))
    d = ImageDraw.Draw(im)
    LX, mark_s, my = 92, 60, 142
    make_mark(d, LX, my, mark_s, th["ink"])
    d.text((LX + mark_s + round(mark_s * 0.25) + 34, my + mark_s / 2),
           "hyperchunk", font=sg(70), fill=th["ink"], anchor="lm")
    d.text((LX, my + mark_s + 50),
           "Bit-exact vanilla Minecraft worldgen in pure C",
           font=inter(24), fill=th["sub"])
    im.save(args.out)
    print("wrote", args.out)

if __name__ == "__main__":
    main()
