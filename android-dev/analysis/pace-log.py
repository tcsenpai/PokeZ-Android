#!/usr/bin/env python3
# pace-log.py <lasrecomp.log> [bucket_s]: frame pacing from LARECOMP_LOG_PACE=1 lines, bucketed by the frames' own clock
# (cumulative period since the first paced frame). Prints per-bucket fps, period p50/p95/max, CPU (game thread), render
# CPU, GPU ms, share of frames over 34 ms, gameplay flag; plus [thermal]/[scale] lines in order.
import re, sys, statistics as st
path = sys.argv[1]; B = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
rx = re.compile(r"\[pace\] period ([\d.]+) blocked ([\d.]+) busy ([\d.]+) cost ([\d.]+) ms \(queue wait ([\d.]+), cpu ([\d.]+), render cpu ([\d.]+), gpu (-?[\d.]+)")
t = 0.0; rows = []; marks = []
for line in open(path, errors="replace"):
    m = rx.search(line)
    if m:
        p, bl, busy, cost, qw, cpu, rc, gpu = map(float, m.groups())
        t += p / 1000
        rows.append((t, p, cpu, rc, gpu, "no gameplay" not in line, busy))
    elif line.startswith(("[thermal] headroom", "[scale]", "[pace] holding", "[pace] trying", "[pace] 60 fps")):
        marks.append((t, line.strip()[:120]))
def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))]
print(f"{'t0':>6} {'n':>4} {'fps':>5} {'p50':>6} {'p95':>6} {'max':>6} {'>34':>5} {'cpu':>5} {'rcpu':>5} {'gpu':>5} {'busy':>5} play")
b = 0
while True:
    seg = [r for r in rows if b <= r[0] < b + B]
    if not seg and b > (rows[-1][0] if rows else 0): break
    if seg:
        per = [r[1] for r in seg]; g = [r[4] for r in seg if r[4] >= 0]
        print(f"{b:6.0f} {len(seg):4d} {len(seg)/B:5.1f} {st.median(per):6.1f} {pct(per,.95):6.1f} {max(per):6.1f} "
              f"{sum(x > 34 for x in per)/len(per)*100:4.0f}% {st.mean(r[2] for r in seg):5.1f} {st.mean(r[3] for r in seg):5.1f} "
              f"{(st.mean(g) if g else -1):5.1f} {st.mean(r[6] for r in seg):5.1f} {sum(r[5] for r in seg)*100//len(seg):3d}%")
    b += B
print("total frames", len(rows), "clock", round(t, 1), "s")
for tm, l in marks: print(f"  @{tm:6.1f}s {l}")
