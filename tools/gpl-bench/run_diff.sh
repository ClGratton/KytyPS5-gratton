#!/usr/bin/env bash
# One process per pipeline so a GPU fault in one captured shader cannot take the whole sweep down.
set -u
BIN=$1; DUMP=$2; OUT=$3; LAYOUT=${4:-own}
export __GL_SHADER_DISK_CACHE=0
: > "$OUT"
n=$(ls "$DUMP/pipelines" | wc -l)
for i in $(seq 0 $((n-1))); do
  timeout 60 "$BIN" --dump "$DUMP" --variant diff --layout "$LAYOUT" --only $i >> "$OUT" 2>&1 || echo "seq $i process failed rc=$?" >> "$OUT"
done
grep -E "^(seq|DIFF)" "$OUT" | grep -v "^DIFF" | awk '{print}' | head -0
