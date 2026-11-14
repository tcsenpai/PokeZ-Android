#!/usr/bin/env python3
# pace.py <run dir> [...]: pacing summary of a scenarios/pace.sh run.
#  - display side: the compositor's present times (sf_*.txt, dumpsys SurfaceFlinger --latency, joined), per window of
#    events.txt (stand / walk): intervals in refreshes, percentiles, 50 ms+ count, off-cadence share, 1/3 alternations;
#  - [fg] on screen lines (VK_GOOGLE_display_timing, LARECOMP_FG_TIMING=1) summed over the same frames;
#  - game side: [pace] period lines (LARECOMP_LOG_PACE=1) from the frame FROM (default: after the last SHOT_AT shot).
import sys, os, re, glob, statistics as st

def pct(s, q): return s[min(len(s) - 1, int(q * len(s)))]

def sf_times(d):
    period, act = None, set()
    for f in sorted(glob.glob(os.path.join(d, "sf_*.txt"))):
        L = open(f, errors="replace").read().split("\n")
        try: period = int(L[1]) / 1e6
        except (ValueError, IndexError): continue
        for l in L[2:]:
            p = l.split()
            if len(p) == 3 and p[1].isdigit():
                a = int(p[1])
                if 0 < a < (1 << 62): act.add(a)
    return period, sorted(act)

def events(d):
    ev = {}
    for l in open(os.path.join(d, "events.txt"), errors="replace"):
        p = l.split()
        if len(p) >= 3 and re.match(r"^\d+\.\d+$", p[1]): ev.setdefault(p[2], float(p[1]))
    return ev

def summarize(name, iv, period):
    if len(iv) < 10: return f"{name}: {len(iv)} intervals"
    s = sorted(iv)
    vs = {}
    for x in iv: k = round(x / period); vs[k] = vs.get(k, 0) + 1
    med = st.median(iv)
    off = sum(abs(x - med) > period / 2 for x in iv)
    alt = sum(1 for a, b in zip(iv, iv[1:]) if round(a / period) != round(b / period))
    return (f"{name}: {len(iv)} intervals over {sum(iv)/1000:.1f} s = {len(iv)*1000/sum(iv):.2f} fps; median {med:.2f} mean {st.mean(iv):.2f} "
            f"p90 {pct(s,.9):.2f} p99 {pct(s,.99):.2f} max {s[-1]:.2f} ms; >=50 ms {sum(x >= 50 for x in iv)}; in refreshes "
            f"{dict(sorted(vs.items()))}; off-cadence {off/len(iv)*100:.1f}%; cadence changes {alt} ({alt*1000/sum(iv)*60:.0f}/min)")

for d in sys.argv[1:]:
    print(f"== {d}")
    ev = events(d)
    period, act = sf_times(d)
    if period and act:
        t = [a / 1e9 for a in act]
        wins = [("stand", ev.get("stand"), ev.get("walk")), ("walk", ev.get("walk"), ev.get("end")),
                ("speedtest+stand", ev.get("walktest"), ev.get("walk"))]
        for wn, a, b in wins:
            if a is None or b is None: continue
            sel = [x for x in t if a + 1 <= x <= b]
            iv = [(y - x) * 1000 for x, y in zip(sel, sel[1:])]
            # gaps in the sf coverage (samples every 4 s, 128 frames each): intervals over 1 s are coverage holes
            iv = [x for x in iv if x < 1000]
            print("  sf " + summarize(wn, iv, period))
        print(f"  sf refresh {period:.3f} ms; coverage {len(act)} frames {t[0]:.1f}..{t[-1]:.1f}")
    log = os.path.join(d, "lasrecomp.log")
    L = open(log, errors="replace").read().split("\n")
    shots = [int(m) for m in re.findall(r"frame_(\d+)\.png", " ".join(L))]
    frm = int(os.environ.get("FROM", (max(shots) + 30) if shots else 0))
    frame, per, fg = 0, [], [0] * 6
    vkl = []
    for l in L:
        m = re.match(r"\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms", l)
        if m: frame = int(m.group(2)); vkl.append((float(m.group(1)), frame, float(m.group(3)), int(m.group(4)))); continue
        if frame < frm: continue
        m = re.match(r"\[pace\] period ([\d.]+) ", l)
        if m: per.append(float(m.group(1))); continue
        m = re.match(r"\[fg\] on screen: (\d+) images, [\d.]+ ms apart on average; under 12 ms (\d+), 12-21 (\d+), 21-29 (\d+), 29-38 (\d+), more (\d+); (\d+) never", l)
        if m:
            g = [int(x) for x in m.groups()]
            fg = [a + b for a, b in zip(fg, g[1:])]
    if per: print("  game " + summarize(f"[pace] periods from frame {frm}", per, 16.667))
    if sum(fg[:5]): print(f"  fg on screen from frame {frm}: <12 {fg[0]}, 12-21 {fg[1]}, 21-29 {fg[2]}, 29-38 {fg[3]}, 38+ {fg[4]}; never shown {fg[5]}")
    late = [v for v in vkl if v[1] >= frm]
    if late: print(f"  [vk] from frame {frm}: fps {st.mean(v[2] for v in late):.2f} (min {min(v[2] for v in late)}), max frame {max(v[3] for v in late)} ms, {len(late)} lines")
    slow = [l for l in L if l.startswith("[slow]")]
    paces = [l for l in L if l.startswith("[pace]") and " period " not in l]
    print(f"  [slow] lines {len(slow)}; other [pace] lines: " + " | ".join(p[:110] for p in paces[:8]))
