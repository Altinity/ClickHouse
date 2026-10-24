#!/usr/bin/env bash
# Usage: run_cluster.sh [chunk_id ...]   (no args = all clusters/chunk_*.jsonl inputs)
#
# For each chunk (clusters/chunk_NNN.jsonl), dispatches a codex clustering pass
# writing clusters/chunk_NNN.clusters.jsonl + .manifest. Resumable: a chunk whose
# output already passes gate_chunk_cluster.py is skipped.
set -uo pipefail
WD="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$WD/clusters" "$WD/tmp"

chunks=("$@")
if [ ${#chunks[@]} -eq 0 ]; then
    chunks=($(cd "$WD/clusters" && ls chunk_*.jsonl | sed 's/\.jsonl$//' | sort))
fi

for c in "${chunks[@]}"; do
    in="$WD/clusters/$c.jsonl"
    out="$WD/clusters/$c.clusters.jsonl"
    if [ ! -s "$in" ]; then
        echo "$c: no input file $in, skip"
        continue
    fi
    if python3 "$WD/tools/gate_chunk_cluster.py" "$in" "$out" > "$WD/tmp/$c.gate.log" 2>&1; then
        echo "$c: already valid, skip"
        continue
    fi
    python3 "$WD/tools/build_cluster_prompt.py" "$c" "$in" "$out" > "$WD/tmp/$c.prompt"
    echo "=== $c ==="
    codex exec -m gpt-5.6-luna -s workspace-write - < "$WD/tmp/$c.prompt" > "$WD/tmp/$c.log" 2>&1
    rc=$?
    echo "$c exit=$rc"
    if python3 "$WD/tools/gate_chunk_cluster.py" "$in" "$out" > "$WD/tmp/$c.gate.log" 2>&1; then
        echo "$c: PASS"
    else
        echo "$c: FAIL, see $WD/tmp/$c.gate.log"
    fi
done
