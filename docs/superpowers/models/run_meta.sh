#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
CFG="$1"; shift || true
LOG="../../../tmp/tlc_$(basename "$CFG" .cfg).log"
/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta -workers auto -deadlock -config "$CFG" "$@" CaMetaDescriptor.tla >"$LOG" 2>&1
RC=$?
grep -E "Model checking completed|Error:|violated|states generated" "$LOG" | tail -3
echo "exit=$RC log=$LOG"
exit $RC
