#!/usr/bin/env python3
# audstat.py <lasrecomp.log> [-v]: totals of the LARECOMP_AUDIO_STATS lines - seconds, game frames/s, longest
# append gap, min game queue, starved ms, device underruns / missing ms, min stream ms, clipped samples, silent buffers
# while sound was playing (a silent second next to sounding ones), peak dBFS. -v: every second with a problem.
import re, sys
pat = re.compile(r"\[audstat\] \S+ ([\d.]+)s: game (\d+) bufs (\d+) frames \(gap max (\d+) ms, queue (\d+)-(\d+), tid (-?\d+) "
                 r"cpu (-?\d+)\) \| out: starved (\d+) ms, stream min (-?\d+) ms \| device: (\d+) underruns, (\d+) ms missing \| "
                 r"level: peak (-?[\d.]+) dBFS, (\d+) clipped, rms (-?[\d.]+) dBFS, (\d+) silent bufs")
rows = [m.groups() for m in map(pat.search, open(sys.argv[1], errors='replace')) if m]
verbose = '-v' in sys.argv
n = len(rows); dur = sum(float(r[0]) for r in rows); fr = gap = st = un = miss = clip = 0; qmin = smin = 1 << 30; pk = -999.0; sil = 0
for i, r in enumerate(rows):
    f = [float(x) for x in r]
    fr += f[2]; gap = max(gap, f[3]); st += f[8]; un += f[10]; miss += f[11]; clip += f[13]; pk = max(pk, f[12])
    if f[4] or f[1]: qmin = min(qmin, f[4])
    if f[9] >= 0: smin = min(smin, f[9])
    loud = f[12] > -60
    if loud and f[15]: sil += f[15]
    if verbose and (f[8] or f[10] or f[13] or (loud and f[15]) or f[3] > 60):
        print(f"#{i}: " + ' '.join(r))
print(f"{n} s, {fr / max(dur, 1):.0f} frames/s, gap max {gap:.0f} ms, game queue min {qmin}, starved {st:.0f} ms, "
      f"underruns {un:.0f} ({miss:.0f} ms), stream min {smin} ms, clipped {clip:.0f}, silent bufs in sounding s {sil:.0f}, "
      f"peak {pk:.1f} dBFS")
