#!/usr/bin/env python3
# sf.py <sf_*.txt ...>: the compositor's present-to-present intervals of the game's surface (dumpsys SurfaceFlinger
# --latency: refresh period, then "desired actual-present frame-ready" ns per frame): how many intervals were 1, 2, 3...
# refresh periods, mean / p95 / max interval, and the share off the median (judder).
import sys, statistics as st
for path in sys.argv[1:]:
    lines = open(path, errors="replace").read().split("\n")
    layer = lines[0][6:] if lines and lines[0].startswith("layer") else "?"
    try: period = int(lines[1]) / 1e6
    except (ValueError, IndexError): print(f"{path}: no data ({layer})"); continue
    act = []
    for l in lines[2:]:
        p = l.split()
        if len(p) == 3 and p[1].isdigit():
            a = int(p[1])
            if 0 < a < (1 << 62): act.append(a)
    mode = next((l.strip() for l in lines if "mActiveSfDisplayMode" in l), "")
    iv = [(b - a) / 1e6 for a, b in zip(act, act[1:])]
    if len(iv) < 10: print(f"{path}: {len(iv)} intervals"); continue
    med = st.median(iv)
    vs = {}
    for x in iv: k = round(x / period); vs[k] = vs.get(k, 0) + 1
    off = sum(abs(x - med) > period / 2 for x in iv)
    s = sorted(iv)
    rr = mode.split("refreshRate=")[1].split(",")[0] if "refreshRate=" in mode else "?"
    print(f"{path.split('/')[-2]}/{path.split('/')[-1]}: refresh {period:.2f} ms (mode {rr} Hz), {len(iv)} intervals: median {med:.2f}, "
          f"mean {st.mean(iv):.2f}, p95 {s[int(.95 * len(s))]:.2f}, max {s[-1]:.2f} ms; in vsyncs {dict(sorted(vs.items()))}; "
          f"off the median {off / len(iv) * 100:.1f}%")
