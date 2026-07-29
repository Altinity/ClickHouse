#!/bin/bash
# Run a soak AND capture its specimen, in one command.
#
# WHY A WRAPPER. `soak.run` never tears its cluster down — an operator does, later, by hand. Twice on
# 2026-07-29 that meant a cluster was destroyed before anyone queried it, and the GC performance audit
# lost its entire queryable specimen both times: the compose mounts only the binary, the configs and
# ./logs/chN, so every system table dies with the container. The scenario runner closes this for
# scenarios (a dump before every reset, plus one at end of batch). A soak has no such hook, so this
# wrapper IS the hook: the dump runs the instant the run returns, while the cluster is still standing,
# and the operator can tear down whenever they like afterwards.
#
# Usage:  scripts/run_soak.sh <label> [soak.run args ...]
#   e.g.  scripts/run_soak.sh t15_revalidation --seed 20260729 --phase 3 --duration 90m --workers 6
#
# The soak's own exit code is preserved and returned, so a wrapper failure and a soak failure stay
# distinguishable. The dump is best-effort and never masks the soak's verdict.

set -u
CA_SOAK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$CA_SOAK_DIR" || exit 90

if [ $# -lt 1 ]; then
    echo "usage: scripts/run_soak.sh <label> [soak.run args ...]" >&2
    exit 2
fi
LABEL="$1"; shift

export PYTHONPATH="${PYTHONPATH:-.}"
echo "=== soak start $(date -Iseconds)  label=${LABEL}"
python3 -m soak.run "$@"
SOAK_RC=$?
echo "=== soak finished rc=${SOAK_RC} at $(date -Iseconds); capturing the specimen BEFORE any teardown"

./scripts/predown_dump.sh "soak_${LABEL}"
DUMP_RC=$?
if [ "$DUMP_RC" -ne 0 ]; then
    echo "WARNING: predown dump reported rc=${DUMP_RC} — check logs/predown/*/soak_${LABEL}/manifest.txt"
    echo "         (the soak's own result is rc=${SOAK_RC} and is NOT affected by this)"
fi

echo "The cluster is STILL UP and its specimen is captured. Tear down when ready:"
echo "    docker compose down -v --remove-orphans"
exit "$SOAK_RC"
