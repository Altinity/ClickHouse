#!/usr/bin/env bash
# Run every CaRefDeltaIntakeCore.tla configuration and check its expected verdict.
# Usage: ./run_refintake.sh
#
#   CaRefDeltaIntakeCore_safe.cfg                       honest                      -> GREEN
#   CaRefDeltaIntakeCore_sab_resumeskip.cfg             S1 resume-skip pagination   -> RED (NoMissedFold)
#   CaRefDeltaIntakeCore_sab_adoptbeforecommit.cfg      S2 cursor before commit     -> RED (NoMissedFold/LosingCommitAdoptsNothing)
#   CaRefDeltaIntakeCore_sab_cleanupignorescursor.cfg   S3 cleanup ignores cursor   -> RED (NoMissedFold)
#   CaRefDeltaIntakeCore_latepred.cfg                   late predecessor PUT        -> RED (expected; treated as PASS)
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }

FAILURES=()

# run_cfg <cfg-basename-without-.cfg> <green|violation>
run_cfg() {
  local cfg="$1" expect="$2"
  local log="../../../tmp/tlc_${cfg}.log"
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config "${cfg}.cfg" \
       CaRefDeltaIntakeCore.tla >"$log" 2>&1

  local outcome
  if grep -q "No error has been found" "$log"; then
    outcome=green
  elif grep -qE "Invariant .* is violated|Error:" "$log"; then
    outcome=violation
  else
    outcome=unknown
  fi

  local verdict
  if [[ "$outcome" == "$expect" ]]; then
    verdict=PASS
  else
    verdict=FAIL
    FAILURES+=("$cfg")
  fi
  echo "${verdict} ${cfg}: expect=${expect} got=${outcome} log=${log}"
}

run_cfg CaRefDeltaIntakeCore_safe green
run_cfg CaRefDeltaIntakeCore_sab_resumeskip violation
run_cfg CaRefDeltaIntakeCore_sab_adoptbeforecommit violation
run_cfg CaRefDeltaIntakeCore_sab_cleanupignorescursor violation
run_cfg CaRefDeltaIntakeCore_latepred violation

if [[ ${#FAILURES[@]} -gt 0 ]]; then
  echo "FAILED configs: ${FAILURES[*]}"
  exit 1
fi
echo "ALL EXPECTATIONS MET"
exit 0
