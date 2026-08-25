#!/usr/bin/env bash
# Runs merge_tierB.py + make_tierB_batches.py every 3 minutes, independent of
# any interactive polling, so Tier B output never sits unmerged just because
# nobody happened to look. Logs progress lines to tmp/auto_merge.log.
set -uo pipefail
WD="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$WD/tmp/auto_merge.log"
mkdir -p "$WD/tmp"

while true; do
  {
    echo "=== $(date -Iseconds) ==="
    python3 "$WD/tools/merge_tierB.py"
    python3 "$WD/tools/make_tierB_batches.py"
    echo "verdicts.jsonl: $(wc -l < "$WD/verdicts/verdicts.jsonl")"
  } >> "$LOG" 2>&1
  sleep 180
done
