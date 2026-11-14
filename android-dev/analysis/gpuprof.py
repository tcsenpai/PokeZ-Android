#!/usr/bin/env python3
# gpuprof.py <run dir> [first frame]: mean GPU ms per frame for each kind of pass over the [gpuprof] frames (LARECOMP_GPUPROF_EVERY),
# from the frame where the walk starts (or [first frame]); key = the label without hashes / draw counts
import re, sys, collections, statistics as st
d = sys.argv[1]
rows = []; cur = None
for line in open(f"{d}/lasrecomp.log", errors="replace"):
    if not line.startswith("[gpuprof]"): continue
    m = re.match(r"\[gpuprof\] (\d+) marks, ([\d.]+) ms", line)
    if m: cur = [float(m.group(2)), []]; rows.append(cur); continue
    m = re.match(r"\[gpuprof\]\s+([\d.]+) ms\s+([\d.]+)%\s+(.*)", line)
    if m and cur is not None: cur[1].append((float(m.group(1)), m.group(3).strip()))
skip = int(sys.argv[2]) if len(sys.argv) > 2 else len(rows) // 3
rows = rows[skip:]
if not rows: sys.exit("no frames")
agg = collections.defaultdict(list); draws = collections.defaultdict(int); fs = collections.defaultdict(collections.Counter); clears = collections.defaultdict(int); clf = collections.defaultdict(int)
for tot, rs in rows:
    seen = collections.defaultdict(float)
    for ms, lab in rs:
        m = re.search(r"first fs ([0-9a-f]+)(?: (?:vs|code) ([0-9a-f]+)(?: ([0-9a-f]+))?)?", lab); h = " ".join(g for g in m.groups() if g) if m else "-"
        n = re.search(r", (\d+) draws", lab)
        k = re.sub(r", first fs [0-9a-f]+( (vs|code) [0-9a-f]+( [0-9a-f]+)?)?", "", lab); k = re.sub(r", \d+ draws", "", k); cl = re.search(r", (\d+) clears \((\d+) full-area first\)", lab)
        k = re.sub(r", \d+ clears \(\d+ full-area first\)", "", k)
        if cl: clears[k] += int(cl.group(1)); clf[k] += int(cl.group(2))
        k = re.sub(r"cs [0-9a-f]+", "cs #", k)
        seen[k] += ms; draws[k] += int(n.group(1)) if n else 0; fs[k][h] += ms
    for k, v in seen.items(): agg[k].append(v)
N = len(rows)
tots = [r[0] for r in rows]
idle = [sum(ms for ms, lab in rs if lab.startswith("(submission end")) for _, rs in rows]
print(f"{d}: {N} profiled frames, GPU first-to-last mean {st.mean(tots):.2f} p50 {st.median(tots):.2f} max {max(tots):.2f} ms; idle between submissions {st.mean(idle):.2f}, busy {st.mean(t - i for t, i in zip(tots, idle)):.2f} (p50 {st.median([t - i for t, i in zip(tots, idle)]):.2f}) ms")
for k, v in sorted(agg.items(), key=lambda kv: -sum(kv[1]))[:int(sys.argv[3]) if len(sys.argv) > 3 else 22]:
    print(f"{sum(v)/N:7.3f} ms/frame (in {len(v):3d} fr, max {max(v):6.2f}) {draws[k]/N:6.1f} dr {clears[k]/N:4.1f}/{clf[k]/N:3.1f} cl  {k[:130]}  fs {fs[k].most_common(1)[0][0]}")
