#!/usr/bin/env python3
import re, sys
rows, other = [], []
for l in open(sys.argv[1]):
    m = re.match(r'seq\s+(\d+) \w+\s+layout=(\w+) bytes=(\d+) covered_texels=(\d+) distinct_texels=(\d+) \| mono-vs-mono\(control\) diff=(\d+) \| mono-vs-fast diff=(\d+) max=(\d+) \| mono-vs-opt diff=(\d+) max=(\d+) \| fast-vs-opt diff=(\d+) max=(\d+)', l)
    if m:
        rows.append(tuple(int(x) if x.isdigit() else x for x in m.groups()))
    elif l.startswith('seq') or 'failed' in l:
        other.append(l.strip())
inf = [r for r in rows if r[3] > 0]
print(f'{sys.argv[1]}: rendered {len(rows)}; informative (geometry reached the target, >0 texels differ from the clear value): {len(inf)} ({sum(r[3] for r in inf)} texels); '
      f'mono-vs-fast differing: {sum(1 for r in inf if r[6])}; mono-vs-opt: {sum(1 for r in inf if r[8])}; fast-vs-opt: {sum(1 for r in inf if r[10])}; '
      f'determinism-control failures: {sum(1 for r in rows if r[5])}; other lines: {other}')
