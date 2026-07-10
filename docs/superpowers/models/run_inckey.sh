#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")"
CFG="$1"; LOG="../../../tmp/tlc_${CFG}.log"
/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
  -metadir ../../../tmp/tlc-meta -workers auto -deadlock -config "${CFG}.cfg" CaMetaIncarnationKey.tla >"$LOG" 2>&1 || true
grep -E "No error has been found|is violated|Parse Error|Error:" "$LOG" | head -3
