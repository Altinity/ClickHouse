#!/usr/bin/env bash
# Tier A: codex evidence-gathering pass, one batch of ~30 clusters at a time.
# Usage: run_tierA.sh [-P concurrency] [batch-NNN ...]   (no batch args = all pending)
set -uo pipefail
WD="$(cd "$(dirname "$0")/.." && pwd)"
BATCH_DIR="$WD/verdicts/tierA-batches"
EVID_DIR="$WD/verdicts/evidence"
TMP_DIR="$WD/tmp/tierA"
mkdir -p "$EVID_DIR" "$TMP_DIR"

concurrency=8
if [ "${1:-}" = "-P" ]; then
    concurrency="$2"
    shift 2
fi

batches=("$@")
if [ ${#batches[@]} -eq 0 ]; then
    batches=($(cd "$BATCH_DIR" && ls batch-*.jsonl | sed 's/\.jsonl$//'))
fi

run_one() {
    b="$1"
    batch_file="$BATCH_DIR/$b.jsonl"
    out="$EVID_DIR/$b.jsonl"
    marker="$EVID_DIR/$b.done"
    if [ -f "$marker" ]; then
        echo "$b: already done, skip"
        return 0
    fi
    : > "$out"
    python3 "$WD/tools/build_tierA_prompt.py" "$batch_file" "$out" > "$TMP_DIR/$b.prompt"
    codex exec -m gpt-5.6-luna -s workspace-write - < "$TMP_DIR/$b.prompt" > "$TMP_DIR/$b.log" 2>&1
    rc=$?
    n_expect=$(wc -l < "$batch_file")
    n_got=$(wc -l < "$out")
    if [ "$rc" -eq 0 ] && [ "$n_got" -ge "$n_expect" ]; then
        echo "TIERA_BATCH_DONE $b exit=$rc expect=$n_expect got=$n_got" > "$marker"
    else
        echo "TIERA_BATCH_SHORT $b exit=$rc expect=$n_expect got=$n_got"
    fi
}
export -f run_one
export WD BATCH_DIR EVID_DIR TMP_DIR

printf '%s\n' "${batches[@]}" | xargs -P "$concurrency" -I{} bash -c 'run_one "$@"' _ {}
echo "TIERA_RUN_COMPLETE"
