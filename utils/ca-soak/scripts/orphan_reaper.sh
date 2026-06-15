#!/usr/bin/env bash
# CA soak orphan reaper (test-harness ONLY) — mitigates the RustFS beta overwrite-leak.
#
# RustFS 1.0.0-beta.8 does NOT reclaim the previous data dir on an un-versioned overwrite, so each
# casPut of a roots/<t>/<ns>/<shard> manifest leaks the old <uuid>/ data dir. This reaper reclaims
# those CONFIRMED-dead orphans, SCOPED TO roots/ (immutable blobs/trees are never touched).
#
# Safety (see spec 2026-06-15-ca-rustfs-overwrite-leak-mitigation-design.md):
#   * scoped to roots/ — cannot delete a blob/tree object
#   * keep xl.meta (never removed) + the single NEWEST <uuid>/ dir (the current incarnation)
#   * only remove <uuid>/ dirs OLDER than GRACE_SEC (a later write has since repointed xl.meta)
# Runs on the RustFS data volume directly (no S3 API load).
#
# Usage: orphan_reaper.sh <roots_dir> [--once]      (env: GRACE_SEC=120 REAP_INTERVAL=300)
set -uo pipefail

ROOTS_DIR="${1:?usage: orphan_reaper.sh <roots_dir> [--once]}"
ONCE="${2:-}"
GRACE_SEC="${GRACE_SEC:-120}"
REAP_INTERVAL="${REAP_INTERVAL:-300}"

reap_once() {
  local now reclaimed_dirs=0 reclaimed_kb=0
  now="$(date +%s)"
  # An object dir is one that directly contains xl.meta. For each, the <uuid>/ subdirs are versions.
  # Keep the newest; delete the others older than GRACE_SEC.
  while IFS= read -r meta; do
    local objdir; objdir="$(dirname "$meta")"
    # version dirs = immediate subdirectories (the <uuid>/ dirs), newest-first by mtime
    local newest=""; local first=1
    while IFS= read -r line; do
      local mt="${line%% *}"; local d="${line#* }"
      if [ "$first" = 1 ]; then newest="$d"; first=0; continue; fi   # newest kept (current incarnation)
      local age=$(( now - mt ))
      if [ "$age" -ge "$GRACE_SEC" ]; then
        local kb; kb="$(du -sk "$d" 2>/dev/null | cut -f1)"
        rm -rf -- "$d" 2>/dev/null && { reclaimed_dirs=$((reclaimed_dirs+1)); reclaimed_kb=$((reclaimed_kb + ${kb:-0})); }
      fi
    done < <(find "$objdir" -mindepth 1 -maxdepth 1 -type d -printf '%T@ %p\n' 2>/dev/null | sort -rn | sed 's/\..* / /')
  done < <(find "$ROOTS_DIR" -mindepth 1 -name xl.meta -type f 2>/dev/null)
  echo "$(date +%H:%M:%S) reaped dirs=$reclaimed_dirs reclaimed_MB=$(( reclaimed_kb / 1024 ))"
}

if [ "$ONCE" = "--once" ]; then
  reap_once
  exit 0
fi

echo "orphan_reaper: roots=$ROOTS_DIR grace=${GRACE_SEC}s interval=${REAP_INTERVAL}s"
while :; do
  reap_once
  sleep "$REAP_INTERVAL"
done
