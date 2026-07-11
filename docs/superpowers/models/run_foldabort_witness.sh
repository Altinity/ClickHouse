#!/usr/bin/env bash
# Run one TLC config against CaRetiredInRunFoldAbortWitness.tla (the add-only GC freshness-meta gate).
# Usage: ./run_foldabort_witness.sh <cfg-file> [extra TLC args]
#
# Gate configs (spec 2026-07-11-cas-deposed-leader-clearsparedmeta-fix §5):
#   CaRetiredInRunFoldAbortWitness.cfg                     honest add-only           -> GREEN
#   CaRetiredInRunFoldAbortWitness_sab_inmem_token.cfg     re-observed delete token  -> RED (INV_NO_RETURN)
#   CaRetiredInRunFoldAbortWitness_sab_attempt_reuse.cfg   reused attempt key        -> RED (INV_ONE_PASS)
#   CaRetiredInRunFoldAbortWitness_sab_no_pacing.cfg       graduate-at-birth         -> RED (INV_NO_LOSS)
#   CaRetiredInRunFoldAbortWitness_sab_gc_clear_on_spare.cfg  pre-CAS spare clear    -> RED (INV_NO_LOSS)
#   CaRetiredInRunFoldAbortWitness_sab_post_adoption_clear.cfg  Fix 1 post-CAS clear -> RED (INV_NO_LOSS)
set -uo pipefail
if [[ $# -lt 1 ]]; then
  echo "usage: $0 <cfg-file> [extra TLC args]" >&2
  exit 2
fi
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
CFG="$1"; shift || true
LOG="../../../tmp/tlc_$(basename "$CFG" .cfg).log"
/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta -workers auto -config "$CFG" "$@" CaRetiredInRunFoldAbortWitness.tla >"$LOG" 2>&1
RC=$?
grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in" "$LOG" | tail -8
echo "exit=$RC log=$LOG"
exit $RC
