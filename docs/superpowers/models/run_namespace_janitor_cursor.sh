#!/usr/bin/env bash
# Assert the two-page namespace-janitor cursor/suppression liveness gate.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaNamespaceJanitorCursorCore

CONFIGS=(
  "sab_advancesuppressed temporal_violation EventuallyDeadAReclaimed"
  "safe                  green              -"
  "witness_deletion      violation          WITNESS_DEAD_A_DELETED"
)

overall=0
printf '%-26s %-20s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
    read -r name expect wanted <<<"$row"
    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc_${MODULE}_${name}.log"
    meta="../../../tmp/tlc-meta-namespace-janitor-cursor-${name}"
    rm -rf "$meta"
    start=$SECONDS
    /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    elapsed=$((SECONDS - start))

    if grep -q "No error has been found" "$log"; then
        result=green
    elif grep -q "Temporal properties were violated" "$log"; then
        result="temporal_violation:${wanted}"
    elif grep -q "is violated" "$log"; then
        result="violation:$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+' "$log" | head -1 | awk '{print $2}')"
    else
        result=error
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        temporal_violation) [[ "$result" == "temporal_violation:${wanted}" ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${wanted}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-26s %-20s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit "$overall"
