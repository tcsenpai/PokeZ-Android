#!/usr/bin/env python3
# hitch.py <run dir>... [--all]: every [slow] frame (> 50 ms; LARECOMP_SLOW_LINES=1000 for all of them) in the scenario's
# measured window ("perf" .. "end", by epoch: the [slow] line's "at" vs events.txt's "start ... epoch T0") classified by its
# main cause; a pipeline-cache save logged just before counts as the cause when nothing else is.
import re, sys, os, collections
def num(rx, s, d=0.0):
    m = re.search(rx, s); return float(m.group(1)) if m else d
for d in [a for a in sys.argv[1:] if not a.startswith("--")]:
    ev = {}; t0 = None
    for l in open(os.path.join(d, "events.txt")):
        p = l.split()
        if len(p) > 1: ev.setdefault(p[1], int(p[0]))
        m = re.search(r"epoch (\d+)", l)
        if m: t0 = int(m.group(1))
    lo = t0 + ev.get("perf", 0) if t0 and "--all" not in sys.argv else 0
    hi = t0 + ev.get("end", 10**9) if t0 and "--all" not in sys.argv else 1e18
    causes = collections.Counter(); ms = collections.defaultdict(list); n = 0; save_pending = False; ex = {}
    for l in open(os.path.join(d, "lasrecomp.log"), errors="replace"):
        if l.startswith("[vk] pipeline cache: saved"): save_pending = True; continue
        if not l.startswith("[slow] frame"): continue
        at = num(r"\(at ([\d.]+)\)", l)
        if not (lo <= at <= hi): save_pending = False; continue
        f = num(r"frame \d+ (\d+) ms", l); n += 1
        build = num(r"pipelines? built \((\d+) ms", l) + num(r"programs? compiled \((\d+) ms", l) + num(r"shaders? translated \((\d+) ms", l)
        up = num(r"texture uploads? ([\d.]+) MB", l) + num(r"texture uploads? ([\d.]+) KB", l) / 1024
        hashes = num(r"(\d+) full texture hash", l)
        gw = num(r"gpu wait (\d+) ms", l); qw = num(r"queue wait (\d+) ms", l); vs = num(r"vsync/pace wait (\d+) ms", l)
        rc = num(r"render cpu (\d+) ms", l); gc = num(r"game cpu (\d+) ms", l)
        lw = re.search(r"render thread longest: (.*?) (\d+) ms", l)
        if vs > f / 2: c = "pacing / vsync wait"
        elif gw > f / 2: c = "GPU wait (game thread)"
        elif up >= 2 or hashes >= 10: c = "texture uploads / change hashes (render thread)"
        elif build >= 20: c = "shader / pipeline builds in the frame"
        elif lw and int(lw.group(2)) >= 20 and "recorder" in lw.group(1): c = "render thread waited for the recorder (submission)"
        elif lw and int(lw.group(2)) >= 20 and ("vkWaitSemaphores" in lw.group(1) or "GPU wait (" in lw.group(1)): c = "render thread waited for the GPU"
        elif save_pending: c = "pipeline-cache save just before"
        elif qw > f / 2: c = "translation (render thread CPU / latency)"
        elif gc > f / 2: c = "game thread CPU"
        else: c = "other"
        causes[c] += 1; ms[c].append(f); ex.setdefault(c, l.strip()[:260]); save_pending = False
    print(f"{os.path.basename(d)}: {n} frames over 50 ms in the window ({(hi - lo) if t0 else '?'} s)")
    for c, k in causes.most_common():
        v = sorted(ms[c]); print(f"  {k:4d}  {c:52s} median {v[len(v)//2]:.0f} max {v[-1]:.0f} ms")
    if "--ex" in sys.argv:
        for c, l in ex.items(): print(f"    e.g. {c}: {l}")
