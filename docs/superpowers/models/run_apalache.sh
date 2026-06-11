#!/usr/bin/env bash
# Run apalache-mc with logging. Usage: ./run_apalache.sh <label> <apalache args...>
set -uo pipefail
if [[ $# -lt 2 ]]; then echo "usage: $0 <label> <apalache args...>" >&2; exit 2; fi
cd "$(dirname "$0")"
APA=../../../tmp/apalache/bin/apalache-mc
[[ -x "$APA" ]] || { echo "apalache not found: $APA" >&2; exit 3; }
LABEL="$1"; shift
LOG="../../../tmp/apa_${LABEL}.log"
"$APA" "$@" >"$LOG" 2>&1
RC=$?
grep -E "Checker reports|The outcome is|Error|error|Counterexample|PASS|FAIL|It took me" "$LOG" | tail -8
echo "exit=$RC log=$LOG"
exit $RC
