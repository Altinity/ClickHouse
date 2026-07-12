#!/usr/bin/env bash
# Run every CaRefNsCleanupStaleLeaderCore.tla configuration and check its expected verdict.
#   CaRefNsCleanupStaleLeaderCore_safe.cfg          straggler guards -> GREEN
#   CaRefNsCleanupStaleLeaderCore_sab_noguard.cfg   no guards        -> RED (NoRecreatedDataDeleted)
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
FAILURES=()
run_cfg() {
  local cfg="$1" expect="$2"
  local log="../../../tmp/tlc_${cfg}.log"
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
       -metadir ../../../tmp/tlc-meta-${cfg} -workers auto -config "${cfg}.cfg" \
       CaRefNsCleanupStaleLeaderCore.tla >"$log" 2>&1
  local outcome
  if grep -q "No error has been found" "$log"; then outcome=green
  elif grep -qE "is violated|Error:" "$log"; then outcome=violation
  else outcome=unknown; fi
  if [[ "$outcome" == "$expect" ]]; then echo "PASS ${cfg}: expect=${expect} got=${outcome}";
  else echo "FAIL ${cfg}: expect=${expect} got=${outcome} log=${log}"; FAILURES+=("$cfg"); fi
}
run_cfg CaRefNsCleanupStaleLeaderCore_safe green
run_cfg CaRefNsCleanupStaleLeaderCore_sab_noguard violation
[[ ${#FAILURES[@]} -gt 0 ]] && { echo "FAILED: ${FAILURES[*]}"; exit 1; }
echo "ALL EXPECTATIONS MET"; exit 0
