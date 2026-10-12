#!/usr/bin/env python3
"""Steam library artwork for Harry's Revenge, from make_title.py's outputs.

Usage: make_steam_art.py <TITLE_E_preview.png> <TITLE_E_bg.png> <TITLE_E_logo.png> <outdir>
Writes cover.png (600x900), header.png (920x430), hero.png (1920x620), logo.png.
"""
import sys
from PIL import Image, ImageFilter

preview, bg, logo, out = sys.argv[1:5]
pv = Image.open(preview).convert("RGB")
bgi = Image.open(bg).convert("RGB")
# Drop the copyright line along the bottom.
bgi = bgi.crop((0, 0, bgi.width, int(bgi.height * 0.85)))
lg = Image.open(logo).convert("RGBA")


def cover_crop(img, w, h, focus_y=0.5):
    """Scale to cover w x h, crop around a vertical focus point."""
    s = max(w / img.width, h / img.height)
    im = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x = (im.width - w) // 2
    y = min(max(int(im.height * focus_y - h / 2), 0), im.height - h)
    return im.crop((x, y, x + w, y + h))


def with_logo(base, logo_w, cy):
    lw = logo_w
    l = lg.resize((lw, round(lg.height * lw / lg.width)), Image.LANCZOS)
    base = base.copy()
    base.paste(l, ((base.width - l.width) // 2, cy - l.height // 2), l)
    return base


cover = with_logo(cover_crop(bgi, 600, 900, 0.5), 540, 300)
cover.save(f"{out}/cover.png")
header = with_logo(cover_crop(bgi, 920, 430, 0.45), 620, 215)
header.save(f"{out}/header.png")
cover_crop(bgi, 1920, 620, 0.55).filter(ImageFilter.GaussianBlur(1)).save(f"{out}/hero.png")
lg.save(f"{out}/logo.png")
