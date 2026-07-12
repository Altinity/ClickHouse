#!/usr/bin/env bash
# Run every CaRefFoldClampRecoveryCore.tla configuration and check its expected verdict.
#   CaRefFoldClampRecoveryCore_safe.cfg               per-log staging  -> GREEN
#   CaRefFoldClampRecoveryCore_sab_edgegranularity.cfg edge-granularity -> RED (NoDeleteBehindClamp)
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
       CaRefFoldClampRecoveryCore.tla >"$log" 2>&1
  local outcome
  if grep -q "No error has been found" "$log"; then outcome=green
  elif grep -qE "is violated|Error:" "$log"; then outcome=violation
  else outcome=unknown; fi
  if [[ "$outcome" == "$expect" ]]; then echo "PASS ${cfg}: expect=${expect} got=${outcome}";
  else echo "FAIL ${cfg}: expect=${expect} got=${outcome} log=${log}"; FAILURES+=("$cfg"); fi
}
run_cfg CaRefFoldClampRecoveryCore_safe green
run_cfg CaRefFoldClampRecoveryCore_sab_edgegranularity violation
[[ ${#FAILURES[@]} -gt 0 ]] && { echo "FAILED: ${FAILURES[*]}"; exit 1; }
echo "ALL EXPECTATIONS MET"; exit 0
