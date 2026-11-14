#!/usr/bin/env python3
# sheet.py <dir> [cols] [width]: contact sheet of the in-app frames (frame_*.png, as in a run's frames/) or adb shots
# (shot_*.png) in dir -> <dir>/sheet.jpg (labels = file names). Output is a picture of the game: keep it out of the repo.
import sys, glob, os
from PIL import Image, ImageDraw
d = sys.argv[1]; cols = int(sys.argv[2]) if len(sys.argv) > 2 else 3; w = int(sys.argv[3]) if len(sys.argv) > 3 else 480
files = sorted(glob.glob(os.path.join(d, "frame_*.png"))) or sorted(glob.glob(os.path.join(d, "shot_*.png")))
ims = []
for f in files:
    im = Image.open(f).convert("RGB")
    if im.height > im.width: im = im.rotate(90, expand=True)
    im = im.resize((w, int(im.height * w / im.width)))
    ImageDraw.Draw(im).text((4, 4), os.path.basename(f).split("_", 1)[1][:-4], fill=(255, 0, 255))
    ims.append(im)
if not ims: sys.exit("no shots")
h = max(i.height for i in ims); rows = (len(ims) + cols - 1) // cols
S = Image.new("RGB", (cols * w, rows * h))
for k, im in enumerate(ims): S.paste(im, ((k % cols) * w, (k // cols) * h))
S.save(os.path.join(d, "sheet.jpg"), quality=80)
print(os.path.join(d, "sheet.jpg"), len(ims))
