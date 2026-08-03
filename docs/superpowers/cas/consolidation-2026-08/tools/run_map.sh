#!/usr/bin/env bash
# Usage: run_map.sh [batch_id ...]              (no args = all pending batches from batches.tsv)
#        run_map.sh --file <path> <batch_id>     (single-file probe: file list comes from <path>
#                                                  instead of batches.tsv; batch_id need not be
#                                                  registered in batches.tsv)
set -uo pipefail
WD="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$WD/extracted" "$WD/tmp"

file_override=""
if [ "${1:-}" = "--file" ]; then
    file_override="$2"
    shift 2
fi

batches=("$@")
if [ -z "$file_override" ] && [ ${#batches[@]} -eq 0 ]; then
    batches=($(tail -n +2 "$WD/batches.tsv" | cut -f1 | sort -u))
fi

for b in "${batches[@]}"; do
    out="$WD/extracted/$b.jsonl"
    [ -s "$out.manifest" ] && { echo "$b: done, skip"; continue; }
    if [ -n "$file_override" ]; then
        python3 "$WD/tools/build_prompt.py" "$b" "$out" "$file_override" > "$WD/tmp/$b.prompt"
    else
        python3 "$WD/tools/build_prompt.py" "$b" "$out" > "$WD/tmp/$b.prompt"
    fi
    echo "=== $b ==="
    codex exec -m gpt-5.6-luna - < "$WD/tmp/$b.prompt" > "$WD/tmp/$b.log" 2>&1
    echo "$b exit=$?"
done
