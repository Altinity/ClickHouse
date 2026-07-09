#!/usr/bin/env bash
# Run one TLC config against CaEdgeBeforeObserve.tla. Usage: ./run_ebo.sh <cfg-file>
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
CFG="$1"; shift || true
LOG="../../../tmp/tlc_$(basename "$CFG" .cfg).log"
/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta -workers auto -deadlock -config "$CFG" "$@" CaEdgeBeforeObserve.tla >"$LOG" 2>&1
RC=$?
grep -E "Model checking completed|Error:|violated|states generated|distinct states" "$LOG" | tail -4
echo "exit=$RC log=$LOG"
exit $RC
