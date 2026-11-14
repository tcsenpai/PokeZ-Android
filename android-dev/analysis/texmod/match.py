#!/usr/bin/env python3
# match.py <frame png> x0 y0 x1 y1 [dump dir]: which dumped textures (default ./dump, device/pull-texdump.sh) look like
# that screen patch (grey, multi-scale, tiled for wrap). Names the texture hash to put in a pack.
import glob, os, sys
from multiprocessing import Pool
import cv2
import numpy as np

fr, x0, y0, x1, y1 = sys.argv[1], *map(int, sys.argv[2:6])
scr = cv2.imread(fr, cv2.IMREAD_GRAYSCALE)[y0:y1, x0:x1]
scr = cv2.resize(scr, (scr.shape[1] // 2, scr.shape[0] // 2), interpolation=cv2.INTER_AREA).astype(np.float32)
SCALES = [0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.6, 0.75, 0.9, 1.1]


def one(f):
    t = cv2.imread(f, cv2.IMREAD_UNCHANGED)
    if t is None or min(t.shape[:2]) < 128: return (0, f, 0)
    g = cv2.cvtColor(t[:, :, :3], cv2.COLOR_BGR2GRAY) if t.ndim == 3 else t
    g = np.tile(g, (2, 2)).astype(np.float32)
    best, bk = -1, 0
    for k in SCALES:
        tk = cv2.resize(g, None, fx=k, fy=k, interpolation=cv2.INTER_AREA)
        if tk.shape[0] < scr.shape[0] + 2 or tk.shape[1] < scr.shape[1] + 2: continue
        res = cv2.matchTemplate(tk, scr, cv2.TM_CCOEFF_NORMED)
        h, w = scr.shape
        m1 = cv2.boxFilter(tk, -1, (w, h), anchor=(0, 0), normalize=True, borderType=cv2.BORDER_CONSTANT)
        m2 = cv2.boxFilter(tk * tk, -1, (w, h), anchor=(0, 0), normalize=True, borderType=cv2.BORDER_CONSTANT)
        sd = np.sqrt(np.maximum(m2 - m1 * m1, 0))[:res.shape[0], :res.shape[1]]
        res[sd < 8] = -1
        v = float(res.max())
        if v > best: best, bk = v, k
    return (best, f, bk)


if __name__ == '__main__':
    fs = sorted(glob.glob(os.path.join(sys.argv[6] if len(sys.argv) > 6 else 'dump', '*.png')))
    with Pool(16) as p:
        r = p.map(one, fs, chunksize=8)
    r.sort(reverse=True)
    for v, f, k in r[:15]: print(f'{v:.3f} {os.path.basename(f)} scale {k}')
