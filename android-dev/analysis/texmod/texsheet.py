#!/usr/bin/env python3
# texsheet.py <dump dir> <out prefix> [min side]: contact sheets of the colour textures in a dump (index labels), + index.tsv
import glob, os, sys
from PIL import Image, ImageDraw, ImageStat

src, out = sys.argv[1], sys.argv[2]
mn = int(sys.argv[3]) if len(sys.argv) > 3 else 64


def kind(im):
    small = im.convert('RGBA').resize((64, 64))
    r, g, b, a = (ImageStat.Stat(small.getchannel(c)) for c in 'RGBA')
    if a.mean[0] < 8: return 'mask'
    if b.mean[0] < 24 and 70 < r.mean[0] < 190 and 70 < g.mean[0] < 190: return 'normal'
    if abs(r.mean[0] - 128) < 30 and abs(g.mean[0] - 128) < 30 and b.mean[0] > 200: return 'normal'
    hsv = small.convert('RGB').convert('HSV')
    if ImageStat.Stat(hsv.getchannel('S')).mean[0] < 12 and r.stddev[0] < 6: return 'mask'
    if g.stddev[0] < 2 and b.stddev[0] < 2 and r.stddev[0] > 8: return 'mask'
    return 'colour'


rows = []
for f in sorted(glob.glob(os.path.join(src, '*.png'))):
    im = Image.open(f)
    if min(im.size) < mn: continue
    k = kind(im)
    rows.append((os.path.basename(f)[:-4], im.size, k))
with open(out + 'index.tsv', 'w') as fo:
    for i, (h, s, k) in enumerate(rows): fo.write(f'{i}\t{h}\t{s[0]}x{s[1]}\t{k}\n')
col = [r for r in rows if r[2] == 'colour']
T, C, R = 160, 10, 6
for p in range(0, len(col), C * R):
    sh = Image.new('RGB', (C * T, R * (T + 14)), (40, 40, 40))
    d = ImageDraw.Draw(sh)
    for j, (h, s, k) in enumerate(col[p:p + C * R]):
        im = Image.open(os.path.join(src, h + '.png')).convert('RGB')
        im.thumbnail((T, T))
        x, y = (j % C) * T, (j // C) * (T + 14)
        sh.paste(im, (x, y))
        d.text((x + 2, y + T), f'{p + j} {h[:6]} {s[0]}', fill=(255, 255, 0))
    sh.save(f'{out}{p // (C * R):03d}.jpg', quality=80)
print(len(rows), 'textures,', len(col), 'colour;', (len(col) + C * R - 1) // (C * R), 'sheets')
