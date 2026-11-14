#!/usr/bin/env python3
# audraw.py <audout.raw>: the game's own audout output (LARECOMP_AUDOUT_WAV, s16 stereo 48 kHz, headerless)
# per second: peak / RMS dBFS, clipped samples (|s| >= 32767), digital-silence runs >= 5 ms inside sound (the game
# producing zeros), and the longest such run.
import sys, numpy as np
d = np.fromfile(sys.argv[1], dtype='<i2')
d = d[: len(d) // 2 * 2].reshape(-1, 2).astype(np.int32)
R = 48000
print(f"{len(d) / R:.1f} s")
for s in range(0, len(d) // R):
    x = d[s * R:(s + 1) * R]
    m = np.abs(x).max()
    rms = np.sqrt((x.astype(np.float64) ** 2).mean())
    clip = int((np.abs(x) >= 32767).sum())
    z = (np.abs(x).max(axis=1) == 0).astype(np.int8)
    runs, cur, longest = 0, 0, 0
    for v in np.concatenate([z, [0]]):
        if v: cur += 1
        else:
            if cur >= 240: runs += 1
            longest = max(longest, cur); cur = 0
    print(f"{s:4d}s peak {20 * np.log10(max(m, 1) / 32768):6.1f} dBFS rms {20 * np.log10(max(rms, 1e-9) / 32768):6.1f} "
          f"clip {clip:6d}  zero-runs>=5ms {runs:3d} longest {longest / 48:6.1f} ms")
