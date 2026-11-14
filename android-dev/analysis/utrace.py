#!/usr/bin/env python3
# utrace.py <utrace.txt> [skip fraction]: Turnip's u_trace (MESA_GPU_TRACES=print) render passes, aggregated by shape:
# count, GPU ms (start..end of the pass), sysmem/GMEM and the reason, load/clear/store bytes per pixel, draws
import re, sys, collections
L = open(sys.argv[1], errors="replace").read().split("\n")
skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
L = L[int(len(L) * skip):]
kv = lambda s: dict(re.findall(r"(\w+)=([^,]*)", s))
agg = collections.defaultdict(lambda: [0, 0.0, 0]); cur = None; t0 = None; tot = 0.0; cb = 0
for l in L:
    m = re.match(r"(\d+)\s+[-+]\d+: (\w+) ?(.*)", l)
    if not m: continue
    t, ev, rest = int(m.group(1)), m.group(2), m.group(3)
    if ev == "start_render_pass": cur = kv(rest); t0 = t
    elif ev == "end_render_pass" and cur:
        e = kv(rest); ms = (t - t0) / 1e6
        k = (f"{cur['width']}x{cur['height']} att {cur['attachment_count']} clr/ld/st {cur['clearCPP']}/{cur['loadCPP']}/{cur['storeCPP']} "
             f"depth {cur['hasDepth']} bins {cur['numberOfBins']} | {'GMEM' if e['tiledRender']=='true' else 'sysmem'} {e.get('forceRenderModeReason','')[:40]} lrz {e['lrz']} {e.get('lrzDisableReason','')[:30]}")
        a = agg[k]; a[0] += 1; a[1] += ms; a[2] += int(e["drawCount"]); tot += ms; cur = None
    elif ev == "start_cmd_buffer": cb += 1
print(f"{cb} command buffers, render passes {tot:.1f} ms in all")
for k, (n, ms, d) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:30]:
    print(f"{ms:8.1f} ms {n:6d}x {ms/n:6.3f} ms/pass {d/n:6.1f} dr  {k}")
