#!/usr/bin/env bash
# Run all CaDiskLifecycle TLC configs (or one, if given as $1).
# Logs: ../../../tmp/tlc_rev7_lifecycle_<cfg>.log
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
CFGS=("${1:-}")
[[ -n "${1:-}" ]] || CFGS=(CaDiskLifecycle_*.cfg)
overall=0
for CFG in "${CFGS[@]}"; do
  LOG="../../../tmp/tlc_rev7_lifecycle_$(basename "$CFG" .cfg).log"
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir ../../../tmp/tlc-meta -workers auto -config "$CFG" \
    CaDiskLifecycle.tla >"$LOG" 2>&1
  RC=$?
  echo "=== $CFG (exit=$RC, log=$LOG)"
  grep -E "Model checking completed|Error:|violated|states generated|distinct states" "$LOG" | head -6
  [[ $RC -ne 0 ]] && overall=1
done
exit $overall
