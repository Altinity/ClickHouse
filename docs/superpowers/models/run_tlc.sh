#!/usr/bin/env bash
# Run one TLC config against CaIncarnationCore.tla. Usage: ./run_tlc.sh <cfg-file> [extra TLC args]
# Output goes to ../../../tmp/tlc_<cfg-basename>.log; last lines + result echoed.
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
# TLC_JAVA_OPTS: optional extra JVM flags (e.g. "-Xmx48g" for long bug-hunt runs); intentionally unquoted.
/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta -workers auto -config "$CFG" "$@" CaIncarnationCore.tla >"$LOG" 2>&1
RC=$?
grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in" "$LOG" | tail -8
echo "exit=$RC log=$LOG"
exit $RC
