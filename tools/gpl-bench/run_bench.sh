#!/usr/bin/env bash
# Cold-run wrapper: NVIDIA disk shader cache off and isolated, one variant per process.
# usage: run_bench.sh <gpl_bench> <dump_dir> <out_dir> [extra gpl_bench args per variant are set below]
set -euo pipefail
BIN=$1; DUMP=$2; OUT=$3
mkdir -p "$OUT"
export __GL_SHADER_DISK_CACHE=0
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
export __GL_SHADER_DISK_CACHE_PATH="$(mktemp -d)"
trap 'echo "disk cache dir contents after run: $(find "$__GL_SHADER_DISK_CACHE_PATH" -type f | wc -l) files" ; rm -rf "$__GL_SHADER_DISK_CACHE_PATH"' EXIT
run() { name=$1; shift; echo "=== $name"; "$BIN" --dump "$DUMP" --csv "$OUT/$name.csv" "$@" | tee "$OUT/$name.log" | grep -v '^seq' || true; }
run mono_own        --variant mono --layout own
run gpl_own         --variant gpl  --layout own
run gpl_super       --variant gpl  --layout super
run mono_super      --variant mono --layout super
run mono_own_rev    --variant mono --layout own --order reverse
run gpl_own_rev     --variant gpl  --layout own --order reverse
run mono_own_warm   --variant mono --layout own --warm
run gpl_own_warm    --variant gpl  --layout own --warm
run e2e_own         --variant e2e  --layout own
run e2e_super       --variant e2e  --layout super
