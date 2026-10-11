#!/usr/bin/env python3
"""D2 Step 0: stall distribution from a --shader-timing-log + --bench-frames-csv run."""
import csv, sys, collections, math
d = sys.argv[1]
FR = 1e9/60
rows = list(csv.DictReader(open(f'{d}/shader_timing.csv')))
def kv(s): return dict(x.split('=',1) for x in s.split(';') if '=' in x)
for r in rows:
    r['t']=int(r['t_ns']); r['dur']=int(r['dur_ns']); r['d']=kv(r['detail'])
frames=[int(r['t_ns']) for r in csv.DictReader(open(f'{d}/frames.csv'))]
def pct(a,p):
    a=sorted(a); k=(len(a)-1)*p/100; f=math.floor(k); c=min(f+1,len(a)-1)
    return a[f]+(a[c]-a[f])*(k-f)
def stats(name,a,unit=1e6):
    if not a: print(name,'n=0'); return
    print(f'{name:38s} n={len(a):3d} sum={sum(a)/1e9:7.3f}s mean={sum(a)/len(a)/unit:8.2f}ms p50={pct(a,50)/unit:8.2f} p90={pct(a,90)/unit:8.2f} p95={pct(a,95)/unit:8.2f} p99={pct(a,99)/unit:8.2f} max={max(a)/unit:8.2f}ms  (frames@60: p95={pct(a,95)/FR:.2f} p99={pct(a,99)/FR:.2f} max={max(a)/FR:.1f})')
span=(frames[-1]-frames[0])/1e9
print(f'run: frames={len(frames)} span={span:.1f}s')
gfx=[r for r in rows if r['kind']=='pipeline_create_gfx']
cs=[r for r in rows if r['kind']=='pipeline_create_cs']
tr=[r for r in rows if r['kind']=='program_translate']
mat=[r for r in rows if r['kind']=='materialize' and r['d'].get('miss')=='1']
print('\n== pipeline_create dur (per-miss creation stall) ==')
stats('gfx create (CreatePipelineInternal)',[r['dur'] for r in gfx])
stats('gfx driver call only',[int(r['d']['driver_ns']) for r in gfx])
stats('cs create',[r['dur'] for r in cs])
stats('all create (gfx+cs)',[r['dur'] for r in gfx+cs])
stats('translate (program_translate)',[r['dur'] for r in tr])
# group into miss events on the gpu thread: a "miss" = pipeline_create row; attribute translate rows
# that precede it (same thread) after the previous pipeline_create row.
pc = sorted(gfx+cs, key=lambda r:r['t'])
tl = sorted(tr, key=lambda r:r['t'])
ml = sorted(mat, key=lambda r:r['t'])
ev=[]; prev_t=0; ti=0
seen_ids=set()
for p in pc:
    stg=[t for t in tl if prev_t < t['t'] <= p['t'] and t['thread']==p['thread']]
    mm=[m for m in ml if prev_t < m['t'] <= p['t'] and m['thread']==p['thread']]
    trd=sum(t['dur'] for t in stg); md=sum(m['dur'] for m in mm)
    # new-shader: any translate in this group with new_source=1 (program first seen) -> new shader
    new_src=any(t['d'].get('new_source')=='1' for t in stg)
    new_perm=any(t['d'].get('new_source')=='0' for t in stg)
    ev.append(dict(kind=p['kind'],t=p['t'],create=p['dur'],translate=trd,mat=md,ntr=len(stg),new_src=new_src,new_perm=new_perm,drv=int(p['d']['driver_ns'])))
    prev_t=p['t']+p['dur']
def tot(e): return e['create']+e['translate']
print('\n== per-miss event (translate rows since previous creation + creation) ==')
for k in ('pipeline_create_gfx','pipeline_create_cs'):
    sub=[e for e in ev if e['kind']==k]
    stats(f'{k} total miss stall',[tot(e) for e in sub])
stats('ALL miss total stall',[tot(e) for e in ev])
print('\n== split: new-shader vs known-shader(new state/permutation) ==')
for k in ('pipeline_create_gfx','pipeline_create_cs'):
    for lab,f in (('new-shader (>=1 new program translated)',lambda e:e['new_src']),('known-programs, new state/perm',lambda e:not e['new_src'])):
        sub=[e for e in ev if e['kind']==k and f(e)]
        stats(f'{k[16:]} {lab}',[tot(e) for e in sub])
print()
for lab,f in (('new-shader',lambda e:e['new_src']),('known/new-state',lambda e:not e['new_src'])):
    sub=[e for e in ev if f(e)]
    n=len(sub); print(f'{lab}: {n} misses ({100*n/len(ev):.1f}%), total {sum(tot(e) for e in sub)/1e9:.2f}s')
print('\n== translation share of miss time ==')
T=sum(e['translate'] for e in ev); C=sum(e['create'] for e in ev)
print(f'translate {T/1e9:.3f}s create {C/1e9:.3f}s -> translate share {100*T/(T+C):.2f}% (per-miss: median {pct([e["translate"]/tot(e) for e in ev],50)*100:.1f}%, max {max(e["translate"]/tot(e) for e in ev)*100:.1f}%)')
Tg=sum(e['translate'] for e in ev if e['kind']=='pipeline_create_gfx'); Cg=sum(e['create'] for e in ev if e['kind']=='pipeline_create_gfx')
print(f'gfx only: translate {Tg/1e9:.3f}s create {Cg/1e9:.3f}s share {100*Tg/(Tg+Cg):.2f}%')
# translate-with-no-pipeline (cs or program translations without a creation?)
print('translate rows:',len(tr),'by stage',collections.Counter(r['d']['stage'] for r in tr),'new_source',collections.Counter(r['d']['new_source'] for r in tr))
# warm cache split by driver_cache init
print('\n== frame-time context ==')
ft=[(frames[i]-frames[i-1]) for i in range(1,len(frames))]
print('frames>1 frame-time(16.7ms)*2:',sum(1 for x in ft if x>2*FR),'>50ms:',sum(1 for x in ft if x>50e6),'>100ms:',sum(1 for x in ft if x>100e6),'median ms',pct(ft,50)/1e6,'p99 ms',pct(ft,99)/1e6)
# misses exceeding thresholds
for th in (1,2,4,10):
    n=sum(1 for e in ev if tot(e)>th*FR); print(f'misses with stall > {th} frame(s): {n}/{len(ev)}')
print('misses/min by 30s bin:')
t0=frames[0]; b=collections.Counter(int((e['t']-t0)/30e9) for e in ev); print(sorted(b.items()))
print('\nTop 12 slowest misses:')
for e in sorted(ev,key=lambda e:-tot(e))[:12]:
    print(f"  {e['kind'][16:]} t={(e['t']-t0)/1e9:6.1f}s total={tot(e)/1e6:8.1f}ms create={e['create']/1e6:8.1f} translate={e['translate']/1e6:6.2f} newsrc={e['new_src']}")
