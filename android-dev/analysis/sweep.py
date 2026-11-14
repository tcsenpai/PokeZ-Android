#!/usr/bin/env python3
# sweep.py <run dir>...: one line per run over its walk: frames of the last $1 s of the frame clock (default 90):
# fps, period p50/p95/p99/max, share > 34 ms (> 26 ms in the 40 fps mode), GPU ms mean/p95, game / render CPU ms,
# then samples.py's temperatures / app CPU / battery over the walk, and SurfaceFlinger's off-cadence share.
import re, sys, os, subprocess, statistics as st
rx = re.compile(r"\[pace\] period ([\d.]+) .*?cpu ([\d.]+), render cpu ([\d.]+), gpu (-?[\d.]+)")
W = 90.0
def pct(v, q): v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))]
for d in sys.argv[1:]:
    rows = [tuple(map(float, m.groups())) for m in map(rx.search, open(f"{d}/lasrecomp.log", errors="replace")) if m]
    t, sel = 0.0, []
    for r in reversed(rows):
        t += r[0] / 1000
        if t > W: break
        sel.append(r)
    if len(sel) < 50: print(f"{d}: {len(sel)} frames"); continue
    per = [r[0] for r in sel]; g = [r[3] for r in sel if r[3] >= 0]
    f40 = "f40" in d
    lim = 26 if f40 else 34
    ev = {l.split(maxsplit=1)[1].strip(): int(l.split()[0]) for l in open(f"{d}/events.txt") if len(l.split()) > 1}
    w0 = next((v for k, v in ev.items() if k == "walk"), 0); end = next((v for k, v in ev.items() if k.startswith("end")), 0)
    smp = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "samples.py"), d, f"{w0 + 10}-{end}"],
                         capture_output=True, text=True).stdout.split("\n")
    s1 = smp[1].split(":", 1)[1].strip() if len(smp) > 1 else ""
    sf = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "sf.py")] +
                        [f"{d}/{f}" for f in sorted(os.listdir(d)) if f.startswith("sf_w") and not f.endswith("_list.txt")],
                        capture_output=True, text=True).stdout
    offs = re.findall(r"off the median ([\d.]+)%", sf); vs = re.findall(r"in vsyncs (\{[^}]*\})", sf)
    print(f"{os.path.basename(d)}: {len(per)/W:.1f} fps, period p50 {st.median(per):.1f} p95 {pct(per,.95):.1f} "
          f"p99 {pct(per,.99):.1f} max {max(per):.0f} ms, >{lim} ms {sum(x > lim for x in per)/len(per)*100:.1f}% | "
          f"GPU {st.mean(g):.1f} (p95 {pct(g,.95):.1f}) ms, game {st.mean(r[1] for r in sel):.1f}, render "
          f"{st.mean(r[2] for r in sel):.1f} ms | SF off-cadence {'/'.join(offs)}% {' '.join(vs)}\n    {s1}")
