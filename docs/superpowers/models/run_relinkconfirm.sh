#!/usr/bin/env bash
# Usage: ./run_relinkconfirm.sh <Cfg-basename-without-.cfg>
set -u
cd "$(dirname "$0")"
CFG="${1:?usage: run_relinkconfirm.sh <cfg-basename>}"
LOG="../../../tmp/tlc_${CFG}.log"
shift || true
java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
     -metadir ../../../tmp/tlc-meta -workers auto -config "${CFG}.cfg" "$@" \
     CaRelinkConfirmCore.tla 2>&1 | tee "$LOG" | \
     grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in"
RC=${PIPESTATUS[0]}
echo "exit=$RC log=$LOG"
exit "$RC"
