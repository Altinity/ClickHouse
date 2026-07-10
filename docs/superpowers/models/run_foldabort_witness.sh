#!/usr/bin/env bash
# Run one TLC config against CaRetiredInRunFoldAbortWitness.tla. Usage: ./run_retiredinrun.sh <cfg-file> [extra TLC args]
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
