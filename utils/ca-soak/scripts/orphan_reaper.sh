#!/usr/bin/env sh
# Safety (spec 2026-06-15-ca-rustfs-overwrite-leak-mitigation-design.md):
#   * scoped to roots/ — cannot delete a blob/tree object
#   * keep xl.meta (never removed) + the single NEWEST <uuid>/ dir (the current incarnation)
#   * only remove <uuid>/ dirs OLDER than GRACE_MIN minutes (a later write has repointed xl.meta)
# BUSYBOX-COMPATIBLE (runs inside the rustfs container via docker exec): uses `ls -t` + `find -mmin`
# (no GNU `find -printf`). Also works on host GNU coreutils.
#
# Usage: orphan_reaper.sh <roots_dir> [--once]   (env: GRACE_MIN=2 REAP_INTERVAL=300)
ROOTS_DIR="${1:?usage: orphan_reaper.sh <roots_dir> [--once]}"
ONCE="${2:-}"
GRACE_MIN="${GRACE_MIN:-2}"
REAP_INTERVAL="${REAP_INTERVAL:-300}"

reap_once() {
  reclaimed=0
  # Object dirs = those directly containing xl.meta. For each, the immediate <uuid>/ subdirs are
  # versions; keep the newest (current), delete the others older than GRACE_MIN.
  find "$ROOTS_DIR" -name xl.meta -type f 2>/dev/null | while IFS= read -r meta; do
    objdir=$(dirname "$meta")
    newest=$(ls -t "$objdir" 2>/dev/null | grep -v '^xl.meta$' | head -1)
    # candidate orphans: immediate subdirs older than GRACE_MIN minutes
    find "$objdir" -mindepth 1 -maxdepth 1 -type d -mmin "+$GRACE_MIN" 2>/dev/null | while IFS= read -r d; do
      [ "$(basename "$d")" = "$newest" ] && continue   # never the current incarnation
      rm -rf -- "$d" 2>/dev/null && reclaimed=$((reclaimed+1))
    done
  done
  echo "$(date +%H:%M:%S) orphan_reaper pass done (roots=$ROOTS_DIR grace=${GRACE_MIN}m)"
}

if [ "$ONCE" = "--once" ]; then
  reap_once
  exit 0
fi

echo "orphan_reaper: roots=$ROOTS_DIR grace=${GRACE_MIN}m interval=${REAP_INTERVAL}s"
while : ; do
  reap_once
  sleep "$REAP_INTERVAL"
done
