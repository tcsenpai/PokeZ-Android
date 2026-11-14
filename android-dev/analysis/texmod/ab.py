#!/usr/bin/env python3
# ab.py <run dir> <out dir>: label each in-app frame of a LARECOMP_TEX_HD_FLIP run as HD / original (log order),
# pair neighbours of opposite state whose pictures otherwise match (still camera), save side-by-side crops
import os, re, sys
import numpy as np
from PIL import Image

run, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
state = 'HD'  # (the ini has tex_hd = 1: HD until the first flip)
flips = []  # (frame, new state): it shows from the next frame on (a shot at the flip's own frame has the old state)
shots = []
for line in open(os.path.join(run, 'lasrecomp.log'), errors='replace'):
    m = re.search(r'\[tex\] textures: (HD|original) .* at frame (\d+)', line)
    if m: flips.append((int(m.group(2)), m.group(1)))
    m = re.search(r'saved .*/frame_(\d+)\.png', line)
    if m: shots.append(int(m.group(1)))
frames = []
for f in shots:
    st = 'HD'  # (the ini has tex_hd = 1: HD until the first flip)
    for ff, s in flips:
        if ff < f: st = s
    frames.append((f, st))
print(len(frames), 'frames:', ' '.join(f'{f}{s[0]}' for f, s in frames))
pairs = []
for (f1, s1), (f2, s2) in zip(frames, frames[1:]):
    if s1 == s2: continue
    a = np.asarray(Image.open(f'{run}/frames/frame_{f1:05d}.png').convert('L'), dtype=np.float32)
    b = np.asarray(Image.open(f'{run}/frames/frame_{f2:05d}.png').convert('L'), dtype=np.float32)
    d = np.abs(a - b)
    pairs.append((d.mean(), f1, s1, f2, s2))
for d, f1, s1, f2, s2 in sorted(pairs)[:6]:
    print(f'pair {f1}({s1}) {f2}({s2}) mean |diff| {d:.2f}')
best = sorted(pairs)[:3]
for k, (d, f1, s1, f2, s2) in enumerate(best):
    o, h = (f1, f2) if s1 == 'original' else (f2, f1)
    A = Image.open(f'{run}/frames/frame_{o:05d}.png').convert('RGB')
    B = Image.open(f'{run}/frames/frame_{h:05d}.png').convert('RGB')
    s = Image.new('RGB', (A.width * 2, A.height))
    s.paste(A, (0, 0)); s.paste(B, (A.width, 0))
    s.save(f'{out}/ab{k}_full_orig{o}_hd{h}.jpg', quality=88)
    # the region that changed most, 360x360, x2 nearest
    diff = np.abs(np.asarray(A, dtype=np.float32) - np.asarray(B, dtype=np.float32)).sum(2)
    step = 60
    best_box, best_v = (0, 0), -1
    for y in range(0, A.height - 360, step):
        for x in range(0, A.width - 360, step):
            v = diff[y:y + 360, x:x + 360].mean()
            if v > best_v: best_v, best_box = v, (x, y)
    x, y = best_box
    ca = A.crop((x, y, x + 360, y + 360)).resize((720, 720), Image.NEAREST)
    cb = B.crop((x, y, x + 360, y + 360)).resize((720, 720), Image.NEAREST)
    c = Image.new('RGB', (1440, 720)); c.paste(ca, (0, 0)); c.paste(cb, (720, 0))
    c.save(f'{out}/ab{k}_crop_orig{o}_hd{h}.jpg', quality=90)
    print('saved', k, o, h, 'crop at', best_box, f'{best_v:.2f}')
