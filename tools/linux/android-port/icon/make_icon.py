#!/usr/bin/env python3
# make_icon.py: draws the launcher icon (a "ZA" monogram in a ring over a night-city gradient) at 1024 px and writes
# the five mipmap sizes next to this script (res/mipmap-*/ic_launcher.png). make_project.sh copies res/ over SDL's.
# Needs Pillow and a bold font (FONT=<path>, default Montserrat Bold).
import math, os, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024
HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.environ.get("FONT", "/usr/share/fonts/julietaula-montserrat-fonts/Montserrat-Bold.otf")


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def background():
    img = Image.new("RGBA", (S, S))
    px = img.load()
    top, bottom = (12, 18, 48), (10, 92, 120)
    for y in range(S):
        for x in range(S):
            d = math.hypot(x - S * 0.5, y - S * 0.42) / (S * 0.75)
            c = lerp(lerp(top, bottom, y / S), (4, 6, 20), min(1.0, d * 0.9))
            px[x, y] = c + (255,)
    return img


def skyline(img):
    # a low row of building silhouettes and a tower, for the city at night
    d = ImageDraw.Draw(img)
    rnd = random.Random(7)
    x = 0
    while x < S:
        w = rnd.randint(40, 90)
        h = rnd.randint(90, 200)
        d.rectangle([x, S - h, x + w, S], fill=(6, 10, 28, 255))
        for wy in range(S - h + 18, S - 20, 34):
            for wx in range(x + 10, x + w - 12, 22):
                if rnd.random() < 0.35:
                    d.rectangle([wx, wy, wx + 8, wy + 12], fill=(255, 214, 120, 200))
        x += w + rnd.randint(2, 10)
    cx = S // 2
    d.polygon([(cx - 46, S), (cx - 14, S - 330), (cx + 14, S - 330), (cx + 46, S)], fill=(6, 10, 28, 255))
    d.rectangle([cx - 4, S - 380, cx + 4, S - 330], fill=(6, 10, 28, 255))


def ring(img):
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    c, r = (S // 2, int(S * 0.44)), int(S * 0.34)
    box = [c[0] - r, c[1] - r, c[0] + r, c[1] + r]
    cols = [(255, 92, 168), (255, 190, 60), (80, 230, 255), (150, 110, 255)]
    for i, col in enumerate(cols):  # four arcs with gaps, like a keystone ring
        start = i * 90 + 8
        d.arc(box, start, start + 74, fill=col + (255,), width=34)
    blur = glow.filter(ImageFilter.GaussianBlur(22))
    img.alpha_composite(blur)
    img.alpha_composite(glow)
    d = ImageDraw.Draw(img)
    for i in range(4):  # small gems in the gaps
        a = math.radians(i * 90 + 4)
        gx, gy = c[0] + r * math.cos(a), c[1] + r * math.sin(a)
        d.regular_polygon((gx, gy, 22), 4, rotation=45, fill=(255, 255, 255, 255))


def monogram(img):
    font = ImageFont.truetype(FONT, 400)
    c = (S // 2, int(S * 0.44))
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.text(c, "ZA", font=font, anchor="mm", fill=(255, 255, 255, 255), stroke_width=18, stroke_fill=(255, 190, 60, 255))
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((c[0] + 10, c[1] + 16), "ZA", font=font, anchor="mm", fill=(0, 0, 0, 170),
                                stroke_width=18, stroke_fill=(0, 0, 0, 170))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    img.alpha_composite(layer)
    sub = ImageFont.truetype(FONT, 86)
    ImageDraw.Draw(img).text((S // 2, int(S * 0.86)), "RECOMP", font=sub, anchor="mm", fill=(220, 240, 255, 255))


def sparkles(img):
    d = ImageDraw.Draw(img)
    for x, y, s in [(190, 170, 26), (830, 220, 20), (870, 560, 14), (150, 520, 16), (700, 120, 12)]:
        d.polygon([(x, y - s), (x + s / 4, y - s / 4), (x + s, y), (x + s / 4, y + s / 4), (x, y + s),
                   (x - s / 4, y + s / 4), (x - s, y), (x - s / 4, y - s / 4)], fill=(255, 255, 255, 230))


def rounded(img):
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


img = background()
skyline(img)
ring(img)
monogram(img)
sparkles(img)
img = rounded(img)
img.save(os.path.join(HERE, "icon-1024.png"))
for name, px in [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]:
    d = os.path.join(HERE, "res", "mipmap-" + name)
    os.makedirs(d, exist_ok=True)
    img.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
print("ok")
