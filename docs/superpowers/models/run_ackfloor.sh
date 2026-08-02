#!/usr/bin/env bash
# Run the complete `CaGcAckFloorCore` battery and assert every result by exact invariant name.
# With one or more cfg names as arguments, run only those rows while retaining the same assertions.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaGcAckFloorCore
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

# Sabotages first, then honest gates, then negated-reachability witnesses.
# name                    expectation(green|violation)  expected-invariant
CONFIGS=(
    "sab_ackbeforedrain      violation  INV_NO_DANGLE"
    "sab_ackwithoutread      violation  INV_NO_DANGLE"
    "sab_adopttoken          violation  INV_NO_RETURN"
    "sab_clampnosuppress     violation  INV_NO_DANGLE"
    "sab_ignorefloor         violation  INV_NO_DANGLE"
    "sab_openbeforeload      violation  INV_NO_DANGLE"
    "sab_rebuilddropedge     violation  INV_NO_DANGLE"
    "sab_rebuildkeepretired  violation  INV_NO_DANGLE"
    "sab_rebuildlowround     violation  INV_NO_DANGLE"
    "sab_skipshard           violation  INV_NO_DANGLE"
    "sab_sleeperrearm        violation  INV_NO_DANGLE"
    "stage1                  green      -"
    "empty_blobs             green      -"
    "witness_clamp           violation  W_ClampHappens"
    "witness_copyforward     violation  W_CopyForwardHappens"
    "witness_delete          violation  W_DeleteHappens"
    "witness_rebuild         violation  W_RebuildHappens"
    "witness_recreate        violation  W_RecreateHappens"
    "witness_spare           violation  W_SpareHappens"
)

selected()
{
    local name="$1"
    shift
    [[ $# -eq 0 ]] && return 0

    local requested
    for requested in "$@"
    do
        requested="${requested##*/}"
        requested="${requested%.cfg}"
        requested="${requested#${MODULE}_}"
        [[ "$requested" == "$name" ]] && return 0
    done
    return 1
}

overall=0
run_id="${MODULE}-$$-$(date +%s%N)"
printf '%-25s %-11s %-32s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue

    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc-${run_id}-${name}.log"
    meta="../../../tmp/tlc-meta-${run_id}-${name}"
    start=$SECONDS
    timeout "${TLC_TIMEOUT:-3600}" /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    rc=$?
    elapsed=$((SECONDS - start))

    result=error
    if [[ $rc -eq 0 ]] && grep -q "No error has been found" "$log"
    then
        result=green
    elif grep -qE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log"
    then
        violated="$(grep -oE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log" \
            | sed -n '1{s/.* \([A-Za-z0-9_]*\) is violated/\1/p;}')"
        result="violation:${violated}"
    elif [[ $rc -eq 124 ]]
    then
        result=timeout
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-25s %-11s %-32s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
    printf '  log: %s\n' "$log"
done

if [[ $# -gt 0 ]]
then
    for requested in "$@"
    do
        requested="${requested##*/}"
        requested="${requested%.cfg}"
        requested="${requested#${MODULE}_}"
        if ! printf '%s\n' "${CONFIGS[@]}" | awk '{print $1}' | grep -qx -- "$requested"
        then
            echo "unknown ${MODULE} config: $requested" >&2
            overall=1
        fi
    done
fi

echo
if [[ $overall -eq 0 ]]
then
    echo "ALL EXPECTATIONS MET"
else
    echo "SOME EXPECTATIONS UNMET"
fi
exit "$overall"
