#!/usr/bin/env python3
"""Drive spirv-compile-bench over a set of dumped shaders and a matrix of SPIR-V variants.

  manifest   pick the N slowest compute pipelines of a --shader-timing-log run and find their SPIR-V
  matrix     build every variant (python rewrites, spirv-opt), validate it, time it on the GPU
  parallel   compile K shaders at once and report the scaling
  selfcheck  run tests/guards.spvasm and each rewritten variant on the GPU and compare the results
  graphics   count unique graphics modules in a timing log, optionally time VS/PS pairs

Hold the shared bench lock while timing: the harness compiles on the GPU.
"""
import argparse
import csv
import glob
import json
import os
import re
import resource
import statistics
import subprocess
import sys
import tempfile

import spvtool

HERE = os.path.dirname(os.path.abspath(__file__))

OPT_LITE = ["--merge-blocks", "--eliminate-dead-branches", "--simplify-instructions",
            "--eliminate-dead-code-aggressive"]

# name -> (python rewrite or None, spirv-opt args or None, harness robustness)
VARIANTS = {
    "asis":             (None, None, "emu"),
    "opt":              (None, ["-O"], "emu"),
    "optlite":          (None, OPT_LITE, "emu"),
    "nobounds":         ("nobounds", None, "emu"),
    "nobounds+optlite": ("nobounds", OPT_LITE, "emu"),
    "robust":           ("robust", None, "emu"),
    "ldsclamp":         ("ldsclamp", None, "emu"),
    "robust+ldsclamp":  ("robust+ldsclamp", None, "emu"),
    "robust+ldsclamp+optlite": ("robust+ldsclamp", OPT_LITE, "emu"),
    "asis@norobust":    (None, None, "none"),   # device robustness off (diagnostic)
}
DEFAULT_MATRIX = ["asis", "opt", "optlite", "nobounds", "nobounds+optlite", "robust", "ldsclamp",
                  "robust+ldsclamp", "robust+ldsclamp+optlite", "asis@norobust"]


def find_bin(arg):
    for c in [arg, os.environ.get("SPIRV_COMPILE_BENCH"),
              os.path.join(HERE, "..", "..", "..", "build", "compile-harness", "spirv-compile-bench"),
              "/home/claudio/Documents/ps5-emulator/kyty/build/compile-harness/spirv-compile-bench"]:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    raise SystemExit("spirv-compile-bench binary not found; build it or pass --bin")


def children_cpu():
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


def spirv_val(path):
    p = subprocess.run(["spirv-val", "--target-env", "vulkan1.3", path], capture_output=True, text=True)
    return p.returncode == 0, p.stderr.strip()[:300]


def make_variant(name, src, dst, grammar):
    """Build variant `name` of `src` at `dst`.  Returns dict(stats, opt_cpu_ms, valid, rewrite)."""
    rewrite, opt_args, _ = VARIANTS[name]
    info = {"rewrite": {}}
    cur = src
    if rewrite:
        m = spvtool.read_module(src)
        info["rewrite"] = dict(spvtool.apply_variant(m, grammar, rewrite))
        cur = dst + ".rw.spv" if opt_args else dst
        with open(cur, "wb") as f:
            f.write(m.to_bytes())
    if opt_args:
        t0 = children_cpu()
        p = subprocess.run(["spirv-opt", "--target-env=vulkan1.3"] + opt_args + [cur, "-o", dst],
                           capture_output=True, text=True)
        info["opt_cpu_ms"] = round((children_cpu() - t0) * 1000, 1)
        if p.returncode != 0:
            info["valid"], info["error"] = False, p.stderr[:300]
            return info
        if cur != src and cur != dst:
            os.unlink(cur)
    elif not rewrite:
        if os.path.abspath(src) != os.path.abspath(dst):
            if os.path.lexists(dst):
                os.unlink(dst)
            os.symlink(os.path.abspath(src), dst)
    info["valid"], info["error"] = spirv_val(dst)
    info["stats"] = spvtool.read_module(dst).stats()
    return info


def run_bench(binary, files, extra, reps, warmup=0):
    cmd = [binary, "--reps", str(reps), "--warmup", str(warmup)] + extra + list(files)
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("harness failed: %s\n%s" % (" ".join(cmd), p.stderr[-800:]))
    out = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    return out, p.stderr


# ---- manifest ----------------------------------------------------------------------------------------
def parse_detail(s):
    return dict(x.split("=", 1) for x in s.split(";") if "=" in x)


def index_spv_dirs(dirs):
    """hash -> spv path, from d2-style dumps (shaders.jsonl + spv/) and --graphics-debug-dump folders."""
    found = {}
    for d in dirs:
        jl = os.path.join(d, "shaders.jsonl")
        if os.path.exists(jl):
            for line in open(jl):
                j = json.loads(line)
                p = os.path.join(d, "spv", "%d.spv" % j["shader_id"])
                if os.path.exists(p):
                    found.setdefault(j["hash"], p)
        for p in glob.glob(os.path.join(d, "**", "*_new_shader_*_*.spv"), recursive=True):
            m = re.search(r"_new_shader_(\w+)_([0-9a-f]{16})\.spv$", p)
            if m:
                found.setdefault(m.group(2), p)
    return found


def cmd_manifest(a):
    rows = list(csv.DictReader(open(a.timing)))
    tr = {}
    for r in rows:
        if r["kind"] == "program_translate":
            d = parse_detail(r["detail"])
            tr[(d["stage"], int(d["shader_id"]))] = d
    cs = [r for r in rows if r["kind"] == "pipeline_create_cs"]
    cs.sort(key=lambda r: -int(r["dur_ns"]))
    spv = index_spv_dirs(a.spv_dir)
    out = []
    for r in cs[:a.top]:
        d = parse_detail(r["detail"])
        cid = int(d["cs_id"])
        t = tr.get(("cs", cid), {})
        h = t.get("shader_hash")
        out.append({"name": "cs%d" % cid, "cs_id": cid, "hash": h, "gcn_dwords": t.get("code_dwords"),
                    "driver_ms": round(int(d["driver_ns"]) / 1e6, 1), "spv": spv.get(h)})
    json.dump(out, open(a.out, "w"), indent=1)
    missing = [o["name"] for o in out if not o["spv"]]
    print("wrote %s (%d shaders), missing SPIR-V: %s" % (a.out, len(out), missing or "none"))


# ---- matrix --------------------------------------------------------------------------------------------
def cmd_matrix(a):
    grammar = spvtool.Grammar(a.grammar or spvtool.default_grammar_path())
    binary = find_bin(a.bin)
    manifest = [m for m in json.load(open(a.manifest)) if m.get("spv")]
    if a.only:
        manifest = [m for m in manifest if m["name"] in a.only.split(",")]
    variants = a.variants.split(",") if a.variants else DEFAULT_MATRIX
    os.makedirs(a.work, exist_ok=True)
    results = {"meta": {"reps": a.reps, "rounds": a.rounds, "variants": variants}, "shaders": {}}
    # warm the driver with the smallest shader (not reported)
    smallest = min(manifest, key=lambda m: os.path.getsize(m["spv"]))
    run_bench(binary, [smallest["spv"]], [], 2)
    for m in manifest:
        entry = {"hash": m["hash"], "gcn_dwords": m["gcn_dwords"], "stage_driver_ms": m["driver_ms"], "variants": {}}
        results["shaders"][m["name"]] = entry
        files = {}
        for v in variants:
            d = os.path.join(a.work, v)
            os.makedirs(d, exist_ok=True)
            dst = os.path.join(d, m["name"] + ".spv")
            info = make_variant(v, m["spv"], dst, grammar)
            entry["variants"][v] = info
            files[v] = dst
            print("[%s] %-26s %s" % (m["name"], v, json.dumps({k: info[k] for k in ("stats", "valid", "opt_cpu_ms") if k in info})), flush=True)
        rounds = a.rounds_big if m["name"] in (a.big or "").split(",") else a.rounds
        for rnd in range(rounds):
            for v in variants:
                if not entry["variants"][v].get("valid"):
                    continue
                extra = ["--robustness", VARIANTS[v][2]] + a.extra.split()
                out, _ = run_bench(binary, [files[v]], extra, a.reps)
                entry["variants"][v].setdefault("samples", []).extend(out[0]["ms"])
        for v in variants:
            s = entry["variants"][v].get("samples")
            if s:
                entry["variants"][v]["median_ms"] = round(statistics.median(s), 1)
                entry["variants"][v]["min_ms"] = round(min(s), 1)
        # shader object timing of the as-is module
        if a.shader_object:
            samples = []
            for rnd in range(rounds):
                out, _ = run_bench(binary, [files["asis"]], ["--mode", "shader-object"] + a.extra.split(), a.reps)
                samples.extend(out[0]["ms"])
            entry["shader_object"] = {"median_ms": round(statistics.median(samples), 1), "min_ms": round(min(samples), 1),
                                      "samples": samples}
        json.dump(results, open(a.out, "w"), indent=1)
    print("wrote", a.out)


def table_md(results, ratio_base="asis"):
    variants = results["meta"]["variants"]
    so = any("shader_object" in s for s in results["shaders"].values())
    head = ["shader", "GCN dw"] + variants + (["shader-object"] if so else [])
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for name, e in results["shaders"].items():
        row = [name, str(e["gcn_dwords"])]
        for v in variants:
            x = e["variants"].get(v, {})
            if "median_ms" not in x:
                row.append("n/a")
                continue
            st = x.get("stats", {})
            cell = "%d ms" % x["median_ms"]
            base = e["variants"].get(ratio_base, {}).get("median_ms")
            if base and v != ratio_base:
                cell += " (%+d%%)" % round((x["median_ms"] / base - 1) * 100)
            cell += " [%dk w, %d b]" % (round(st.get("words", 0) / 1000), st.get("blocks", 0))
            if "opt_cpu_ms" in x:
                cell += " +opt %d ms" % x["opt_cpu_ms"]
            row.append(cell)
        if so:
            s = e.get("shader_object")
            base = e["variants"].get("asis", {}).get("median_ms")
            row.append("%d ms (%+d%%)" % (s["median_ms"], round((s["median_ms"] / base - 1) * 100)) if s and base else "n/a")
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def cmd_table(a):
    print(table_md(json.load(open(a.results))))


# ---- parallel ----------------------------------------------------------------------------------------------
def cmd_parallel(a):
    binary = find_bin(a.bin)
    manifest = [m for m in json.load(open(a.manifest)) if m.get("spv")]
    res = []
    run_bench(binary, [min((m["spv"] for m in manifest), key=os.path.getsize)], [], 2)
    names = a.names.split(",") if a.names else [manifest[0]["name"]]
    for k in [int(x) for x in a.threads.split(",")]:
        for mode in a.modes.split(","):
            # same shader K times
            files = [next(m["spv"] for m in manifest if m["name"] == names[0])]
            out, _ = run_bench(binary, files, ["--parallel", str(k), "--mode", mode, "--robustness", "emu"], a.reps, warmup=1)
            res.append(dict(out[0], scenario="same:%s" % names[0]))
            print(json.dumps(res[-1]), flush=True)
            if k > 1:
                dfiles = [m["spv"] for m in manifest if m["name"] in (a.distinct_names or "").split(",")] if a.distinct_names else [m["spv"] for m in manifest[:k]]
                out, _ = run_bench(binary, dfiles, ["--parallel", str(k), "--distinct", "--mode", mode, "--robustness", "emu"], a.reps, warmup=1)
                res.append(dict(out[0], scenario="distinct"))
                print(json.dumps(res[-1]), flush=True)
    json.dump(res, open(a.out, "w"), indent=1)


# ---- selfcheck ----------------------------------------------------------------------------------------------
def exec_dump(binary, spv, robustness="emu"):
    out, _ = run_bench(binary, [spv], ["--exec", "--robustness", robustness, "--range", "0=40", "--range", "2=40"], 1)
    return {j["binding"]: j["data"].split() for j in out if j.get("kind") == "exec"}


def cmd_selfcheck(a):
    grammar = spvtool.Grammar(a.grammar or spvtool.default_grammar_path())
    binary = find_bin(a.bin)
    src = os.path.join(tempfile.mkdtemp(prefix="guards-"), "guards.spv")
    subprocess.check_call(["spirv-as", "--target-env", "spv1.3", os.path.join(HERE, "tests", "guards.spvasm"), "-o", src])
    ref = exec_dump(binary, src)
    ok = True
    for v in ["robust", "ldsclamp", "robust+ldsclamp", "nobounds", "robust+ldsclamp+optlite"]:
        dst = src.replace("guards.spv", "guards_%s.spv" % v)
        info = make_variant(v, src, dst, grammar)
        got = exec_dump(binary, dst)
        same = got == ref
        ok &= same
        diff = [(b, i, ref[b][i], got[b][i]) for b in ref for i in range(len(ref[b])) if ref[b][i] != got[b][i]][:6]
        print("%-26s valid=%s identical_results=%s %s" % (v, info["valid"], same, diff or ""))
    # sanity: the reference result shows the GCN semantics (OOB loads 0, OOB stores dropped)
    print("reference out[0..3]=%s in-range lane out[5]=%s st[10..12] (must stay untouched)=%s" % (ref[1][0:3], ref[1][5], ref[2][10:13]))
    raise SystemExit(0 if ok else 1)


# ---- graphics ------------------------------------------------------------------------------------------------
def cmd_graphics(a):
    rows = list(csv.DictReader(open(a.timing)))
    gfx = [r for r in rows if r["kind"] == "pipeline_create_gfx"]
    pairs = {}
    vs_set, ps_set, tess = set(), set(), 0
    for r in gfx:
        d = parse_detail(r["detail"])
        vs, ps = int(d["vs_id"]), int(d["ps_id"])
        vs_set.add(vs)
        ps_set.add(ps)
        pairs.setdefault((vs, ps), []).append(int(r["dur_ns"]))
        tess += d.get("tess") == "1"
    tr = {}
    for r in rows:
        if r["kind"] == "program_translate":
            d = parse_detail(r["detail"])
            tr[(d["stage"], int(d["shader_id"]))] = d["shader_hash"]
    vs_hash = {tr.get(("vs", i)) for i in vs_set}
    ps_hash = {tr.get(("ps", i)) for i in ps_set}
    total = sum(int(r["dur_ns"]) for r in gfx) / 1e6
    print(json.dumps({
        "graphics_pipelines": len(gfx), "unique_vs_ids": len(vs_set), "unique_ps_ids": len(ps_set),
        "unique_modules_by_id": len(vs_set) + len(ps_set), "unique_vs_hashes": len(vs_hash - {None}),
        "unique_ps_hashes": len(ps_hash - {None}), "unique_vs_ps_pairs": len(pairs),
        "with_tess": tess, "total_driver_ms": round(total, 1),
        "pipelines_per_vs_max": max(sum(1 for (v, _p) in pairs if v == x) for x in vs_set),
        "pipelines_per_ps_max": max(sum(1 for (_v, p) in pairs if p == x) for x in ps_set)}, indent=1))


def cmd_gfxtime(a):
    """Time the N slowest graphics pipelines' VS/PS pairs (minimal fixed state, see main.cpp)."""
    binary = find_bin(a.bin)
    rows = list(csv.DictReader(open(a.timing)))
    tr = {}
    for r in rows:
        if r["kind"] == "program_translate":
            d = parse_detail(r["detail"])
            tr[(d["stage"], int(d["shader_id"]))] = d["shader_hash"]
    by_name = {}
    for p in glob.glob(os.path.join(a.spv_dir, "*_new_shader_*_*.spv")):
        m = re.search(r"/(\d+)_new_shader_(\w+)_([0-9a-f]{16})\.spv$", p)
        by_name[(m.group(2), int(m.group(1)))] = (m.group(3), p)

    def resolve(stage, sid):
        h = tr.get((stage, sid))
        for (st, idx), (hh, p) in by_name.items():
            if st == stage and hh == h and idx == sid - 1:
                return p, h
        cands = [p for (st, idx), (hh, p) in sorted(by_name.items()) if st == stage and hh == h]
        return (cands[0] if cands else None), h
    gfx = [r for r in rows if r["kind"] == "pipeline_create_gfx"]
    gfx.sort(key=lambda r: -int(r["dur_ns"]))
    seen, pairs = set(), []
    for r in gfx:
        d = parse_detail(r["detail"])
        key = (int(d["vs_id"]), int(d["ps_id"]))
        if key in seen:
            continue
        seen.add(key)
        vs, ps = resolve("vs", key[0]), resolve("ps", key[1])
        if vs[0] and ps[0]:
            pairs.append({"vs_id": key[0], "ps_id": key[1], "game_ms": round(int(r["dur_ns"]) / 1e6, 1), "vs": vs[0], "ps": ps[0]})
        if len(pairs) >= a.top:
            break
    run_bench(binary, [pairs[0]["vs"], pairs[0]["ps"]], ["--gfx"], 1)  # warm up
    for p in pairs:
        out, _ = run_bench(binary, [p["vs"], p["ps"]], ["--gfx", "--robustness", "emu"], a.reps)
        p.update({"bench_median_ms": out[0]["median_ms"], "words": out[0]["words"], "blocks": out[0]["blocks"]})
        print(json.dumps({k: p[k] for k in p if k not in ("vs", "ps")}), flush=True)
    json.dump(pairs, open(a.out, "w"), indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--grammar")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("manifest")
    p.add_argument("--timing", required=True)
    p.add_argument("--spv-dir", action="append", default=[])
    p.add_argument("--top", type=int, default=12)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_manifest)
    p = sub.add_parser("matrix")
    p.add_argument("--manifest", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--work", required=True, help="directory for the generated variant .spv files (not committed)")
    p.add_argument("--variants")
    p.add_argument("--only")
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--rounds", type=int, default=1)
    p.add_argument("--big", default="cs98", help="shaders that get --rounds-big rounds")
    p.add_argument("--rounds-big", type=int, default=3)
    p.add_argument("--shader-object", action="store_true")
    p.add_argument("--extra", default="")
    p.set_defaults(fn=cmd_matrix)
    p = sub.add_parser("table")
    p.add_argument("results")
    p.set_defaults(fn=cmd_table)
    p = sub.add_parser("parallel")
    p.add_argument("--manifest", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--threads", default="1,2,4")
    p.add_argument("--modes", default="pipeline")
    p.add_argument("--names")
    p.add_argument("--distinct-names")
    p.add_argument("--reps", type=int, default=2)
    p.set_defaults(fn=cmd_parallel)
    p = sub.add_parser("selfcheck")
    p.set_defaults(fn=cmd_selfcheck)
    p = sub.add_parser("graphics")
    p.add_argument("--timing", required=True)
    p.set_defaults(fn=cmd_graphics)
    p = sub.add_parser("gfxtime")
    p.add_argument("--timing", required=True)
    p.add_argument("--spv-dir", required=True)
    p.add_argument("--top", type=int, default=8)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_gfxtime)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
