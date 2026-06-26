#!/usr/bin/env bash
# Usage: ./run_gc_partmanifest.sh <Cfg-basename-without-.cfg>
set -u
CFG="${1:?usage: run_gc_partmanifest.sh <cfg-basename>}"
LOG="../../../tmp/tlc_${CFG}.log"
shift || true
java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
     -metadir ../../../tmp/tlc-meta -workers auto -config "${CFG}.cfg" "$@" \
     CaGcRootLocalPartManifestCore.tla 2>&1 | tee "$LOG" | \
     grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in"
RC=${PIPESTATUS[0]}
echo "exit=$RC log=$LOG"
exit "$RC"
