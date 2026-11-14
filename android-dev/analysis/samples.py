#!/usr/bin/env python3
# samples.py <run dir> [window ...]: CPU per thread (by name; % of one core), whole app, whole system (% of 8 cores),
# temperatures, GPU busy/clock, battery current over time windows of the run (seconds since the scenario's start, from
# ts.txt written by scenarios/lib.sh tsampler). Windows "a-b"; default: menus 35-55, intro (ready+10 .. ready+90), last 60 s.
import re, sys, os, collections
d = sys.argv[1]
ev = {}
for l in open(os.path.join(d, "events.txt")):
    t, *rest = l.split(maxsplit=1)
    if rest: ev.setdefault(rest[0].strip().split()[0] + (" " + rest[0].split()[1] if rest[0].startswith("shot") else ""), int(t))
samples = []  # (t, wall, info dict, {tid: (name, ticks)}, sys (busy, total), rss)
cur = None
for l in open(os.path.join(d, "ts.txt"), errors="replace"):
    if l.startswith("== "):
        p = l.split()
        info = dict(kv.split("=", 1) for kv in p[5:] if "=" in kv)
        cur = [int(p[1]), float(p[2]), info, {}, None, 0]
        samples.append(cur)
    elif cur is None: continue
    elif l.startswith("cpu "):
        v = list(map(int, l.split()[1:]))
        idle = v[3] + v[4]
        cur[4] = (sum(v) - idle, sum(v))
    elif l.startswith("VmRSS"): cur[5] = int(l.split()[1]) // 1024
    elif l.startswith("T "):
        s = l[2:]
        a, b = s.index("("), s.rindex(")")
        tid, name, f = int(s[:a]), s[a + 1:b], s[b + 2:].split()
        cur[3][tid] = (name, int(f[11]) + int(f[12]))
end = samples[-1][0] if samples else 0
ready = ev.get("shot ready", 60)
wins = sys.argv[2:] or [f"35-{ready - 2}", f"{ready + 10}-{ready + 90}", f"{end - 60}-{end}"]
def window(a, b):
    s = [x for x in samples if a <= x[0] <= b]
    if len(s) < 2: return None
    s0, s1 = s[0], s[-1]
    dt = s1[1] - s0[1]
    per = collections.Counter()
    for tid, (name, t1) in s1[3].items():
        t0 = s0[3].get(tid, (name, None))[1]
        if t0 is None:  # (a thread born inside the window: all its ticks)
            t0 = 0
        per[name] += (t1 - t0) / 100 / dt * 100
    sysb = (s1[4][0] - s0[4][0]) / max(1, s1[4][1] - s0[4][1]) * 100 if s0[4] and s1[4] else -1
    num = lambda k: [float(x[2].get(k, "nan")) for x in s if x[2].get(k, "").lstrip("-").isdigit()]
    mean = lambda v: sum(v) / len(v) if v else float("nan")
    return dict(dt=dt, per=per, app=sum(per.values()), sys=sysb, cpu=mean(num("cpu")), cpumax=max(num("cpu") or [0]),
                gpu=mean(num("gpu")), gpumax=max(num("gpu") or [0]), xo=mean(num("xo")), gbusy=mean(num("gpubusy")),
                gclk=mean(num("gpuclk")) / 1e6, cur=mean(num("cur")) / 1000, rss=max(x[5] for x in s))
print(f"run {d}: ready at {ready}s, end {end}s")
for w in wins:
    a, b = map(int, w.split("-"))
    r = window(a, b)
    if not r: print(f"  {w}: no samples"); continue
    top = ", ".join(f"{n} {v:.0f}" for n, v in r["per"].most_common(8))
    print(f"  {w:>9}s: app {r['app']:.0f}% of a core, system {r['sys']:.0f}% of 8 | cpu {r['cpu']:.0f} (max {r['cpumax']:.0f}) "
          f"gpu {r['gpu']:.0f} (max {r['gpumax']:.0f}) xo {r['xo']:.0f} C | gpu busy {r['gbusy']:.0f}% @ {r['gclk']:.0f} MHz | "
          f"batt {r['cur']:.0f} mA | rss {r['rss']} MB")
    print(f"             threads: {top}")
