import sys, random
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

src, out = sys.argv[1], sys.argv[2]
W, H = 1280, 960                       # 4:3 display space
im = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
a = np.asarray(im).astype(np.float32)

# Logo + TM band (480-line rows ~105..185 -> x2): rebuild it entirely by
# blending the rows just above and below, so no outline of the old logo survives.
y0, y1 = 196, 384
top = a[y0 - 6:y0].mean(axis=0)
bot = a[y1:y1 + 6].mean(axis=0)
t = np.linspace(0, 1, y1 - y0)[:, None, None]
fill = top[None] * (1 - t) + bot[None] * t
# soften horizontally so column noise in the source rows doesn't streak
fill_img = Image.fromarray(np.clip(fill, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(6))
fill = np.asarray(fill_img).astype(np.float32)
# feather the seams
w = np.ones((y1 - y0, 1, 1)); r = 14
w[:r, 0, 0] = np.linspace(0, 1, r); w[-r:, 0, 0] = np.linspace(1, 0, r)
a[y0:y1] = a[y0:y1] * (1 - w) + fill * w
a[y0:y1] += np.random.default_rng(7).normal(0, 3, (y1 - y0, W, 1))
im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))

im.save(out.replace(".png", "_bg.png"))


def distressed_text(text, font, fill, size_box, seed):
    layer = Image.new("L", size_box, 0)
    d = ImageDraw.Draw(layer)
    d.text((size_box[0] // 2, size_box[1] // 2), text, font=font, fill=255, anchor="mm")
    arr = np.asarray(layer).astype(np.float32) / 255
    rng = np.random.default_rng(seed)
    speck = rng.random(arr.shape) < 0.012
    speck = np.asarray(Image.fromarray((speck * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))) > 0
    arr[speck] *= rng.uniform(0.2, 0.7, speck.sum())
    alpha = Image.fromarray((arr * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))
    color = Image.new("RGB", size_box, fill)
    return color, alpha

impact = "/System/Library/Fonts/Supplemental/Impact.ttf"
small = ImageFont.truetype(impact, 70)
big = ImageFont.truetype(impact, 170)

glow = Image.new("L", (W, H), 0)
gd = ImageDraw.Draw(glow)
gd.text((W // 2, 228), "HARRY'S", font=small, fill=200, anchor="mm")
gd.text((W // 2, 322), "REVENGE", font=big, fill=255, anchor="mm")
glow = glow.filter(ImageFilter.GaussianBlur(18))
red = Image.new("RGB", (W, H), (150, 0, 10))
im = Image.composite(red, im, glow.point(lambda v: int(v * 0.55)))

c, al = distressed_text("HARRY'S", small, (235, 235, 240), (W, 120), 1)
im.paste(c, (0, 228 - 60), al)
c, al = distressed_text("REVENGE", big, (200, 14, 22), (W, 220), 2)
shadow = al.filter(ImageFilter.GaussianBlur(4))
im.paste(Image.new("RGB", (W, 220), (10, 0, 0)), (6, 322 - 110 + 6), shadow)
im.paste(c, (0, 322 - 110), al)

# Back to the TIM's 2:3 pixel grid at 2x (640x960).
im.resize((640, 960), Image.LANCZOS).save(out)
im.save(out.replace(".png", "_preview.png"))

# Transparent logo for the Steam library (its own canvas, same lettering).
LW, LH = 1280, 420
logo = Image.new("RGBA", (LW, LH), (0, 0, 0, 0))
c, al = distressed_text("HARRY'S", ImageFont.truetype(impact, 110), (235, 235, 240), (LW, 150), 3)
logo.paste(c, (0, 10), al)
c, al = distressed_text("REVENGE", ImageFont.truetype(impact, 260), (200, 14, 22), (LW, 280), 4)
sh = Image.new("RGBA", (LW, 280), (10, 0, 0, 255))
logo.paste(sh, (8, 138), al.filter(ImageFilter.GaussianBlur(5)))
logo.paste(c, (0, 130), al)
logo.save(out.replace(".png", "_logo.png"))

