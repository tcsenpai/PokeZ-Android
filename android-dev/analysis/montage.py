#!/usr/bin/env python3
# montage.py <out.jpg> <frame png...>: the frames side by side (4 per row, 480 px wide each), labelled.
import sys
from PIL import Image, ImageDraw
fs = sys.argv[2:]
W, H = 480, 270
cols = 4
m = Image.new("RGB", (W * cols, H * ((len(fs) + cols - 1) // cols)))
for i, f in enumerate(fs):
    im = Image.open(f).convert("RGB").resize((W, H))
    ImageDraw.Draw(im).text((6, 6), f.split("/")[-2] + " " + f.split("/")[-1], fill=(255, 255, 0))
    m.paste(im, ((i % cols) * W, (i // cols) * H))
m.save(sys.argv[1], quality=85)
