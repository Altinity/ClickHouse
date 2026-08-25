#!/usr/bin/env bash
# Usage: run_cross_merge.sh [name ...]   (no args = all clusters/merge-in/*.jsonl)
#
# For each target-group input (clusters/merge-in/<name>.jsonl), dispatches a
# codex cross-chunk merge-detection pass writing
# clusters/merge-in/<name>.merge.jsonl + .manifest. Resumable: a name whose
# output already passes gate_merge_output.py is skipped.
set -uo pipefail
WD="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$WD/clusters/merge-in" "$WD/tmp"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
    names=($(cd "$WD/clusters/merge-in" && ls *.jsonl 2>/dev/null | grep -v '\.merge\.jsonl$' | sed 's/\.jsonl$//' | sort))
fi

for n in "${names[@]}"; do
    in="$WD/clusters/merge-in/$n.jsonl"
    out="$WD/clusters/merge-in/$n.merge.jsonl"
    if [ ! -s "$in" ]; then
        echo "$n: no input file $in, skip"
        continue
    fi
    if python3 "$WD/tools/gate_merge_output.py" "$in" "$out" > "$WD/tmp/merge_$n.gate.log" 2>&1; then
        echo "$n: already valid, skip"
        continue
    fi
    target="$(cat "$WD/clusters/merge-in/$n.target" 2>/dev/null || echo "$n")"
    python3 "$WD/tools/build_merge_prompt.py" "$target" "$in" "$out" > "$WD/tmp/merge_$n.prompt"
    echo "=== $n ==="
    codex exec -m gpt-5.6-luna -s workspace-write - < "$WD/tmp/merge_$n.prompt" > "$WD/tmp/merge_$n.log" 2>&1
    rc=$?
    echo "$n exit=$rc"
    if python3 "$WD/tools/gate_merge_output.py" "$in" "$out" > "$WD/tmp/merge_$n.gate.log" 2>&1; then
        echo "$n: PASS"
    else
        echo "$n: FAIL, see $WD/tmp/merge_$n.gate.log"
    fi
done
