#!/usr/bin/env python3
"""Aggregate gpl_bench CSVs (run_bench.sh output dir) into the numbers used by docs/d2-spike-results.md.

usage: analyze_bench.py <out_dir> [<timing_run_dir>]
"""
import csv, sys, math, statistics as st, os, collections


def load(path):
    if not os.path.exists(path):
        return []
    rows = list(csv.DictReader(open(path)))
    for r in rows:
        for k, v in r.items():
            if k not in ('class', 'variant', 'layout', 'status'):
                r[k] = float(v)
    return rows


def pct(a, p):
    a = sorted(a)
    if not a:
        return float('nan')
    k = (len(a) - 1) * p / 100
    f = math.floor(k)
    c = min(f + 1, len(a) - 1)
    return a[f] + (a[c] - a[f]) * (k - f)


def summ(name, a):
    a = [x for x in a if x >= 0]
    if not a:
        print(f'{name:36s} n=0')
        return
    print(f'{name:36s} n={len(a):3d} sum={sum(a):9.1f} mean={sum(a)/len(a):8.2f} p50={pct(a,50):8.2f} p90={pct(a,90):8.2f} p95={pct(a,95):8.2f} p99={pct(a,99):8.2f} max={max(a):8.2f} ms')


d = sys.argv[1]
mono = {r['seq']: r for r in load(f'{d}/mono_own.csv')}
mono_rev = {r['seq']: r for r in load(f'{d}/mono_own_rev.csv')}
gown = {r['seq']: r for r in load(f'{d}/gpl_own.csv')}
gsup = {r['seq']: r for r in load(f'{d}/gpl_super.csv')}
gown_rev = {r['seq']: r for r in load(f'{d}/gpl_own_rev.csv')}
msup = {r['seq']: r for r in load(f'{d}/mono_super.csv')}
e2e_own = {r['seq']: r for r in load(f'{d}/e2e_own.csv')}
e2e_sup = {r['seq']: r for r in load(f'{d}/e2e_super.csv')}
mwarm = {r['seq']: r for r in load(f'{d}/mono_own_warm.csv')}
gwarm = {r['seq']: r for r in load(f'{d}/gpl_own_warm.csv')}

ok = lambda r: r['status'] == 'ok'
seqs = [s for s in mono if ok(mono[s]) and s in gown and ok(gown[s])]
print(f'pipelines with mono+gpl ok: {len(seqs)} (classes: {dict(collections.Counter(mono[s]["class"] for s in seqs))})')

print('\n== monolithic (own layout) vs GPL pieces, cold, ms ==')
summ('mono', [mono[s]['mono'] for s in seqs])
summ('mono (reverse order process)', [mono_rev[s]['mono'] for s in seqs if s in mono_rev])
summ('mono (super layout)', [msup[s]['mono'] for s in seqs if s in msup and ok(msup[s])])
for part in ('vi', 'pre', 'fs', 'fo', 'fast', 'opt'):
    summ(f'gpl {part} (own layout)', [gown[s][part] for s in seqs])
for part in ('vi', 'pre', 'fs', 'fo', 'fast', 'opt'):
    summ(f'gpl {part} (super layout)', [gsup[s][part] for s in seqs if s in gsup and ok(gsup[s])])
summ('gpl libs sum vi+pre+fs+fo (own)', [gown[s]['vi'] + gown[s]['pre'] + gown[s]['fs'] + gown[s]['fo'] for s in seqs])
summ('gpl libs+fast (own)', [gown[s]['vi'] + gown[s]['pre'] + gown[s]['fs'] + gown[s]['fo'] + gown[s]['fast'] for s in seqs])
if gown_rev:
    summ('gpl fast (reverse order)', [gown_rev[s]['fast'] for s in seqs if s in gown_rev and ok(gown_rev[s])])

print('\n== gate: fast link < 1/10 of full compile ==')
ratio = [gown[s]['fast'] / mono[s]['mono'] for s in seqs if mono[s]['mono'] > 0]
print(f'per-pipeline ratio fast/mono: p50={pct(ratio,50):.3f} p90={pct(ratio,90):.3f} p95={pct(ratio,95):.3f} p99={pct(ratio,99):.3f} max={max(ratio):.3f}; fraction of pipelines below 0.10: {sum(1 for r in ratio if r<0.1)/len(ratio):.2f}')
tot_fast = sum(gown[s]['fast'] for s in seqs)
tot_mono = sum(mono[s]['mono'] for s in seqs)
print(f'aggregate: sum fast {tot_fast:.1f} ms / sum mono {tot_mono:.1f} ms = {tot_fast/tot_mono:.4f}')
tot_libs = sum(gown[s]['vi'] + gown[s]['pre'] + gown[s]['fs'] + gown[s]['fo'] for s in seqs)
print(f'libraries + fast link vs mono (the new-shader miss, serial on one worker): {(tot_libs+tot_fast)/tot_mono:.3f}x of mono')
tot_opt = sum(gown[s]['opt'] for s in seqs)
print(f'optimized link alone vs mono: {tot_opt/tot_mono:.3f}; libs+opt link vs mono: {(tot_libs+tot_opt)/tot_mono:.3f}')

# warm
if mwarm and gwarm:
    print('\n== warm (second creation with the same VkPipelineCache), ms ==')
    ws = [s for s in seqs if s in mwarm and s in gwarm and ok(mwarm[s]) and ok(gwarm[s])]
    summ('mono warm', [mwarm[s]['mono_warm'] for s in ws])
    summ('gpl fast warm', [gwarm[s]['fast_w'] for s in ws])
    summ('gpl pre warm', [gwarm[s]['pre_w'] for s in ws])
    summ('gpl fs warm', [gwarm[s]['fs_w'] for s in ws])

print('\n== end-to-end (libs concurrent + fast link), wall ms, excluding translation ==')
es = [s for s in seqs if s in e2e_own and ok(e2e_own[s])]
summ('e2e 4 workers (own)', [e2e_own[s]['e2e4'] for s in es])
summ('e2e 2 workers (own)', [e2e_own[s]['e2e2'] for s in es])
es2 = [s for s in seqs if s in e2e_sup and ok(e2e_sup[s])]
summ('e2e 4 workers (super)', [e2e_sup[s]['e2e4'] for s in es2])
summ('e2e 2 workers (super)', [e2e_sup[s]['e2e2'] for s in es2])

if len(sys.argv) > 2:
    # add translation time from the game's timing log: new-shader miss = translate(vs)+translate(ps) in parallel
    import json, glob
    t = sys.argv[2]
    trows = list(csv.DictReader(open(f'{t}/shader_timing.csv')))
    tr = {}
    for r in trows:
        if r['kind'] == 'program_translate':
            kv = dict(x.split('=', 1) for x in r['detail'].split(';') if '=' in x)
            tr[int(kv['shader_id'])] = int(r['dur_ns']) / 1e6
    pipes = {}
    for f in sorted(glob.glob(f'{os.environ.get("GPLB_DUMP", d + "/../dump")}/pipelines/*.json')):
        j = json.load(open(f))
        pipes[j['seq']] = j
    print('\n== miss -> usable pipeline for a brand-new shader pair (shader ids joined from the dump to the game timing log) ==')
    out = []
    for s in es:
        j = pipes.get(int(s))
        if not j:
            continue
        tv, tp = tr.get(j['vs_id']), tr.get(j['ps_id'], 0.0)
        if tv is None:
            continue
        # critical path: VS translate -> pre lib ; PS translate -> fs lib ; vi/fo libs need no shader -> start at once
        g = gown[s]
        cp = max(tv + g['pre'], tp + g['fs'], g['vi'], g['fo']) + g['fast']
        cp_serial_translate = tv + tp + max(g['pre'], g['fs']) + g['fast']   # one translation thread
        mono_total = tv + tp + mono[s]['mono']
        out.append((s, tv, tp, g['pre'], g['fs'], g['fast'], cp, cp_serial_translate, mono_total))
    if out:
        summ('translate VS+PS (ms)', [o[1] + o[2] for o in out])
        summ('mono: translate + create (today)', [o[8] for o in out])
        summ('GPL new-shader miss, translation parallel', [o[6] for o in out])
        summ('GPL new-shader miss, translation serial', [o[7] for o in out])
        for fr, nm in ((1, '1 frame (16.7 ms)'), (2, '2 frames (33.3 ms)')):
            lim = fr * 1000 / 60
            n = sum(1 for o in out if o[7] <= lim)
            print(f'  GPL new-shader miss within {nm}: {n}/{len(out)}')
            n = sum(1 for o in out if o[8] <= lim)
            print(f'  mono (today) within {nm}: {n}/{len(out)}')
        tshare = [o[1] + o[2] for o in out]
        print(f'  translation share of GPL new-shader miss: {100*sum(tshare)/sum(o[7] for o in out):.1f}%  (share of mono: {100*sum(tshare)/sum(o[8] for o in out):.1f}%)')
