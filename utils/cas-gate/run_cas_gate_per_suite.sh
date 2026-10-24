#!/bin/bash
# Per-suite CAS gate runner: one timed unit_tests_dbms invocation per suite, so an abort in one suite
# cannot hide the results of every suite after it. The suite list is regenerated (not hand-maintained)
# by generate_cas_suites.sh, which cross-checks against the built binary's own --gtest_list_tests and
# fails loud on any unclaimed suite -- see that script's header for why.
set -uo pipefail

BUILD_DIR="${1:-../../build}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

"$(dirname "$0")/generate_cas_suites.sh" "$BUILD_DIR" || exit 1

BIN="$BUILD_DIR/src/unit_tests_dbms"
SUITES_FILE="$BUILD_DIR/cas_suites.txt"
OUT="$BUILD_DIR/per_suite_results.txt"
LOG_DIR="$BUILD_DIR/suite_logs"
mkdir -p "$LOG_DIR"

# The file is truncated and rewritten by every run, so a short file is ambiguous between "stalled
# early" and "started seconds ago". Stamp the start and the population so a watcher can tell which,
# without having to correlate against `ps`.
{
    echo "# started $(date -Is) pid=$$"
    echo "# suites $(grep -cve '^[[:space:]]*$' "$SUITES_FILE")"
} > "$OUT"
total_pass=0
total_fail=0
total_abort=0
while IFS= read -r suite; do
    [ -z "$suite" ] && continue
    # Parameterized suites contain '/' (Inst/Suite): flatten it or the redirect target's directory
    # does not exist and the suite silently never executes (exit 1 read as a test failure).
    logf="$LOG_DIR/${suite//\//_}.log"
    # Sanitizer builds run slower; a fixed 60 s budget sat below two real suites' ASan runtime.
    timeout "${SUITE_TIMEOUT:-300}" "$BIN" --gtest_filter="${suite}.*" > "$logf" 2>&1
    code=$?
    if [ $code -eq 0 ]; then
        result="PASS"
        total_pass=$((total_pass+1))
    elif [ $code -eq 134 ] || [ $code -eq 139 ]; then
        result="ABORT"
        total_abort=$((total_abort+1))
    else
        result="FAIL"
        total_fail=$((total_fail+1))
    fi
    echo "$result $suite (exit=$code)" >> "$OUT"
done < "$SUITES_FILE"
echo "TOTALS: pass=$total_pass fail=$total_fail abort=$total_abort" >> "$OUT"
echo "DONE"
# A gate that records failures but exits 0 is not a gate.
if [ $((total_fail + total_abort)) -gt 0 ]; then
    exit 1
fi
