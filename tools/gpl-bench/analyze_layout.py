#!/usr/bin/env python3
"""D2 Step 0: size the shared superset pipeline layout from a --dump-pipelines capture.

usage: analyze_layout.py <dump_dir> [maxPushDescriptors]
Binding numbers are NativeBinding(stage, kind) = kind + 50 * group
(group 0 = VS/LS/ES/mesh, 1 = PS, 2 = TCS, 3 = TES); see ShaderIR.h.
"""
import glob, json, sys, collections, statistics as st

KIND_NAMES = {0: 'Buffers', 44: 'Samplers', 45: 'Gds', 46: 'BdaPagetable', 47: 'FaultBuffer', 48: 'FlattenedSrt', 49: 'ShaderData'}
DIMS = ['1D', '1DArray', '2D', '2DArray', '2DMsaa', '2DMsaaArray', '3D']
for k in range(1, 22):
    KIND_NAMES[k] = ['SampledFloat', 'SampledUint', 'SampledSint'][(k - 1) // 7] + DIMS[(k - 1) % 7]
for k in range(22, 29):
    KIND_NAMES[k] = 'SampledDepthCmp' + DIMS[k - 22]
for k in range(29, 34):
    KIND_NAMES[k] = 'StorageFloat' + ['1D', '1DArray', '2D', '2DArray', '3D'][k - 29]
for k in range(34, 39):
    KIND_NAMES[k] = 'StorageUint' + ['1D', '1DArray', '2D', '2DArray', '3D'][k - 34]
for k in range(39, 44):
    KIND_NAMES[k] = 'AtomicUint' + ['1D', '1DArray', '2D', '2DArray', '3D'][k - 39]
GROUPS = ['VS', 'PS', 'TCS', 'TES']
TYPE_NAMES = {0: 'SAMPLER', 2: 'SAMPLED_IMAGE', 3: 'STORAGE_IMAGE', 7: 'STORAGE_BUFFER'}

d = sys.argv[1]
max_push = int(sys.argv[2]) if len(sys.argv) > 2 else 32
pipes = [json.load(open(f)) for f in sorted(glob.glob(f'{d}/pipelines/*.json'))]
comps = [json.load(open(f)) for f in sorted(glob.glob(f'{d}/compute/*.json'))]


def analyze(name, items):
    print(f'\n### {name}: {len(items)} pipelines')
    per_binding = collections.defaultdict(list)  # binding -> counts (over pipelines that use it)
    totals, nbind, push_flags = [], [], []
    for p in items:
        b = p['layout']['bindings']
        totals.append(sum(x['count'] for x in b))
        nbind.append(len(b))
        push_flags.append(p['layout']['push_descriptors'])
        for x in b:
            per_binding[x['binding']].append((x['count'], x['type']))
    if not items:
        return
    print(f'descriptors per pipeline layout: min={min(totals)} p50={st.median(totals)} p90={sorted(totals)[int(0.9*(len(totals)-1))]} max={max(totals)}; bindings per layout: p50={st.median(nbind)} max={max(nbind)}')
    over = sum(1 for t in totals if t > max_push)
    print(f'layouts over maxPushDescriptors({max_push}) => today already use the DescriptorHeap path: {over}/{len(items)} ({100*over/len(items):.0f}%); captured uses_push_descriptors=true: {sum(push_flags)}')
    print(f'\n| group | kind | type | pipelines using | max count | p50 count |')
    print('|---|---|---|---|---|---|')
    sup_total, sup_types = 0, collections.Counter()
    for bnd in sorted(per_binding):
        kind, grp = bnd % 50, bnd // 50
        cs = [c for c, _ in per_binding[bnd]]
        t = per_binding[bnd][0][1]
        sup_total += max(cs)
        sup_types[TYPE_NAMES.get(t, t)] += max(cs)
        print(f'| {GROUPS[grp]} | {KIND_NAMES.get(kind, kind)} (binding {bnd}) | {TYPE_NAMES.get(t, t)} | {len(cs)} | {max(cs)} | {st.median(cs):g} |')
    print(f'\nsuperset (max per binding): {len(per_binding)} bindings, {sup_total} descriptors total: ' + ', '.join(f'{k}={v}' for k, v in sup_types.items()))
    print(f'push-descriptor budget {max_push}: superset needs {sup_total} -> {"FITS" if sup_total <= max_push else "DOES NOT FIT"}')
    # what fraction of per-draw set contents the superset would carry unused
    mean_used = sum(totals) / len(totals)
    print(f'mean descriptors actually written per pipeline {mean_used:.1f} vs superset {sup_total} ({100*mean_used/sup_total:.1f}% used)')
    return sup_total, sup_types


g = analyze('graphics', pipes)
c = analyze('compute', comps)
cls = collections.Counter(('mesh' if p['mesh'] else 'rectlist' if p['rect_list'] else 'tess' if p['tess'] else 'plain') for p in pipes)
print('\ngraphics pipeline classes:', dict(cls))
# DescriptorHeap pool cost (descriptor_heap.cpp: 1024 sets / 8192 storage buffers / 8192 sampled / 1024 storage image / 1024 sampler per pool)
if g:
    sup_total, sup_types = g
    print('\n### DescriptorHeap pool cost of the superset layout (one set allocation consumes all descriptorCount of the layout)')
    pools = {'STORAGE_BUFFER': 8192, 'SAMPLED_IMAGE': 8192, 'STORAGE_IMAGE': 1024, 'SAMPLER': 1024}
    sets_per_pool = min(1024, *[pools[k] // v for k, v in sup_types.items() if v and k in pools])
    print(f'sets per pool before exhaustion: {sets_per_pool} (maxSets 1024); superset per-set: {dict(sup_types)}')
    # same figure for today's per-pipeline layouts, weighted by average layout
    avg = collections.Counter()
    for p in pipes:
        for x in p['layout']['bindings']:
            avg[TYPE_NAMES.get(x['type'], x['type'])] += x['count']
    avg = {k: v / len(pipes) for k, v in avg.items()}
    spp = min(1024, *[pools[k] / v for k, v in avg.items() if v and k in pools])
    print(f'today (mean per-pipeline layout {{{", ".join(f"{k}={v:.1f}" for k, v in avg.items())}}}): ~{spp:.0f} sets per pool')
