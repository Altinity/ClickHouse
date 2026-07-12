#!/usr/bin/env bash
# Run TLC against CaRefWriterCleanupCore.tla and check PASS/FAIL against each config's expectation.
#
# Usage:
#   ./run_refwcleanup.sh                    # run every known config, print one PASS/FAIL line each
#   ./run_refwcleanup.sh <cfg-basename>      # run just one config (no .cfg suffix), print its line
#
# Expectation table (by config name):
#   *_safe                        -> expect GREEN: no invariant/property violation (includes the
#                                     liveness property, checked under weak fairness).
#   *_sab_*                       -> expect a VIOLATION (the named sabotage toggle must break
#                                     exactly the one invariant listed in that config).
#
# Exit status is nonzero if any config's outcome does not match its expectation.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }

MODEL=CaRefWriterCleanupCore.tla
ALL_CFGS=(
  CaRefWriterCleanupCore_safe
  CaRefWriterCleanupCore_sab_retirebeforeremoval
  CaRefWriterCleanupCore_sab_successorcurrentepoch
  CaRefWriterCleanupCore_sab_cancelbeforedurable
)

run_one() {
  local cfg="$1"
  local log="../../../tmp/tlc_${cfg}.log"
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config "${cfg}.cfg" "$MODEL" \
       >"$log" 2>&1
  local rc=$?

  local violated=0
  grep -qE "is violated|Error: |is not enabled" "$log" && violated=1

  local expect_violation=0
  [[ "$cfg" == *_sab_* ]] && expect_violation=1

  local verdict
  if [[ $expect_violation -eq 1 ]]; then
    [[ $violated -eq 1 ]] && verdict=PASS || verdict=FAIL
  else
    [[ $violated -eq 0 && $rc -eq 0 ]] && verdict=PASS || verdict=FAIL
  fi

  echo "${verdict}: ${cfg} (rc=${rc} violated=${violated} expect_violation=${expect_violation}) log=${log}"
  grep -E "Model checking completed|states generated|distinct states|Finished in" "$log" | tail -3 | sed 's/^/    /'

  [[ "$verdict" == "PASS" ]]
}

overall=0
if [[ $# -ge 1 ]]; then
  run_one "$1" || overall=1
else
  for cfg in "${ALL_CFGS[@]}"; do
    run_one "$cfg" || overall=1
  done
fi
exit $overall
