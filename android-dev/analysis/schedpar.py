#!/usr/bin/env python3
# schedpar.py <sched.txt> [tid ...]: per 2 s sample and thread, CPU run ms, runqueue-wait ms (runnable but
# not running), CPU last run on, nice. Without tids: the top threads by runqueue wait over the run.
import sys, collections
samples, cur = [], None
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('== '):
        cur = {'t': float(line.split()[1]), 'th': {}}; samples.append(cur)
    elif line.startswith('S ') and cur is not None:
        head, _, stat = line.partition(' | ')
        p = head.split()
        tid, comm = p[1], p[2]
        try: run, wait, slices = int(p[3]), int(p[4]), int(p[5])
        except (IndexError, ValueError): continue
        f = stat[stat.rfind(')') + 2:].split()
        cpu, nice = int(f[36]), int(f[16])
        cur['th'][tid] = (comm, run, wait, slices, cpu, nice)
tids = sys.argv[2:]
if not tids:
    tot = collections.Counter()
    for a, b in zip(samples, samples[1:]):
        for t, v in b['th'].items():
            if t in a['th']: tot[t] += v[2] - a['th'][t][2]
    for t, w in tot.most_common(15):
        print(f"{t} {samples[-1]['th'].get(t, ('?',))[0]:16s} rq-wait {w / 1e6:8.0f} ms")
    sys.exit()
for a, b in zip(samples, samples[1:]):
    dt = (b['t'] - a['t'])
    row = [f"{b['t']:4.0f}"]
    for t in tids:
        if t in a['th'] and t in b['th']:
            x, y = a['th'][t], b['th'][t]
            row.append(f"{t}: run {(y[1] - x[1]) / 1e6:6.0f} wait {(y[2] - x[2]) / 1e6:5.0f} ms sl {y[3] - x[3]:5d} cpu{y[4]} n{y[5]}")
    print('  '.join(row))
