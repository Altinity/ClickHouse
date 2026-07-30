#!/usr/bin/env bash
# Run the `CaRefLaneCore` sabotage, honest, and witness configurations.
set -uo pipefail
cd "$(dirname "$0")"

JAR=../../../tmp/tla2tools.jar
MODULE=CaRefLaneCore
RUN_ID="${RUN_ID:-$(date +%Y%m%dT%H%M%S)-$$}"
LOG_ROOT="../../../build/tlc-runs/reflane/${RUN_ID}"
META_ROOT="../../../build/tlc-meta/reflane/${RUN_ID}"

[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
mkdir -p "$LOG_ROOT" "$META_ROOT"

# Sabotages precede honest and witness runs.
CONFIGS=(
  "sab_noarm              violation ReadyCaughtUp"
  "sab_dropuncertain      violation ReadyCaughtUp"
  "sab_appendblocked      violation NoAppendWhileBlocked"
  "sab_incompleterecovery violation ReadyCaughtUp"
  "sab_skipidentity       violation InstallMatchesAttempt"
  "sab_nofence            violation CertifiedViewIsCurrent"
  "sab_certifyblocked     violation CertifiedViewIsCurrent"
  "safe                   green     -"
  "witness_commit         violation W_Commit"
  "witness_unresolved     violation W_Unresolved"
  "witness_retrycreated   violation W_RetryCreated"
  "witness_recovery       violation W_Recovery"
  "witness_staleresult    violation W_StaleResult"
  "witness_closed         violation W_Closed"
  "witness_faulted        violation W_Faulted"
)

overall=0
printf '%-28s %-10s %-34s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
    read -r name expect wanted <<<"$row"
    cfg="${MODULE}_${name}.cfg"
    log="${LOG_ROOT}/tlc_${cfg%.cfg}.log"
    meta="${META_ROOT}/${name}"
    start=$SECONDS
    /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    elapsed=$((SECONDS - start))

    if grep -q "No error has been found" "$log"; then
        result=green
    elif grep -q "is violated" "$log"; then
        result="violation:$(grep -oE '(Invariant|Property) [A-Za-z_]+ is violated' "$log" | head -1 | awk '{print $2}')"
    else
        result=error
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${wanted}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-28s %-10s %-34s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo "logs=${LOG_ROOT}"
if [[ $overall -eq 0 ]]; then
    echo "ALL EXPECTATIONS MET"
else
    echo "SOME EXPECTATIONS UNMET"
fi
exit "$overall"
