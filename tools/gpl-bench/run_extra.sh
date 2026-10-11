#!/usr/bin/env bash
# Extra cold runs: DISABLE_OPTIMIZATION hint, worker-pool scaling (makespan on N concurrent workers), descriptor microbench.
set -euo pipefail
BIN=$1; DUMP=$2; OUT=$3
mkdir -p "$OUT"
export __GL_SHADER_DISK_CACHE=0 __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
export __GL_SHADER_DISK_CACHE_PATH="$(mktemp -d)"
trap 'echo "disk cache dir contents after run: $(find "$__GL_SHADER_DISK_CACHE_PATH" -type f | wc -l) files"; rm -rf "$__GL_SHADER_DISK_CACHE_PATH"' EXIT
run() { name=$1; shift; echo "=== $name"; "$BIN" --dump "$DUMP" "$@" | tee "$OUT/$name.log" | grep -E "^(PAR|DIFF|seq|desc|[0-9])" | tail -n 3; }
run mono_noopt   --variant mono --layout own --disable-opt --csv "$OUT/mono_noopt.csv"
run gpl_noopt    --variant gpl  --layout own --disable-opt --csv "$OUT/gpl_noopt.csv"
for n in 1 2 4 8; do
  run par_mono_$n --variant par-mono --par $n
  run par_gpl_$n  --variant par-gpl  --par $n
done
