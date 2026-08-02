#!/usr/bin/env bash
# Run the `CaIncarnationCore` battery and assert every result by exact outcome kind and name.
# With cfg names as arguments, run only those rows. `SLOW=1` adds the three intentionally unbounded
# BFS probes; the finite random-simulation probe remains in the default battery but is never a proof.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaIncarnationCore
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

# Sabotages first, then staged proof configs, the known liveness lasso, and the finite simulation.
# name                  expectation                         expected-name
CONFIGS=(
    "sab_cascade          violation                           INV_NO_LOSS"
    "sab_cutoverclaim     violation                           INV_NO_DANGLE"
    "sab_foldtimeuniverse violation                           INV_NO_DANGLE"
    "sab_noevreobserve    violation                           INV_NO_LOSS"
    "sab_nofence          violation                           INV_NO_DANGLE"
    "sab_norecheckfold    violation                           INV_NO_DANGLE"
    "sab_noregistry       violation                           INV_NO_DANGLE"
    "sab_noreobserve      violation                           INV_NO_DANGLE"
    "sab_noretireview     violation                           INV_NO_DANGLE"
    "sab_reusedtag        violation                           INV_NO_RETURN"
    "sab_unconddelete     violation                           INV_NO_DANGLE"
    "stage1               green                               -"
    "stage2               green                               -"
    "reval_stage2         green                               -"
    "stage3               green                               -"
    "stage4_journaltree   green                               -"
    "stage4_small         green                               -"
    "stage5_small         green                               -"
    "stage6_cross_smoke   green                               -"
    "stage6_evstale       green                               -"
    "stage6_registry      green                               -"
    "empty_hashes          green                               -"
    "stage2_live          temporal                            NoLeakForever"
    "hunt_sim             simulation                         -"
)

SLOW_CONFIGS=(
    "hunt_cross           incomplete                         -"
    "stage4               incomplete                         -"
    "stage5               incomplete                         -"
)
[[ "${SLOW:-0}" == 1 ]] && CONFIGS+=("${SLOW_CONFIGS[@]}")
check_tlc_temporal_expectations "$JAR" "${CONFIGS[@]}" || exit 3

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

known_name()
{
    local requested="$1"
    printf '%s\n' "${CONFIGS[@]}" "${SLOW_CONFIGS[@]}" | awk '{print $1}' | grep -qx -- "$requested"
}

overall=0
run_id="${MODULE}-$$-$(date +%s%N)"
printf '%-22s %-12s %-36s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue

    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc-${run_id}-${name}.log"
    meta="../../../tmp/tlc-meta-${run_id}-${name}"
    timeout_seconds="${TLC_TIMEOUT:-3600}"
    extra=()
    if [[ "$expect" == simulation ]]
    then
        timeout_seconds="${TLC_SIM_TIMEOUT:-600}"
        extra=(-simulate "num=${TLC_SIMULATIONS:-1000}" -depth "${TLC_SIM_DEPTH:-200}")
    elif [[ "$expect" == incomplete ]]
    then
        timeout_seconds="${TLC_SLOW_TIMEOUT:-3600}"
    fi

    start=$SECONDS
    timeout "$timeout_seconds" /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" "${extra[@]}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    rc=$?
    elapsed=$((SECONDS - start))

    result=error
    if [[ "$expect" == simulation ]] && [[ $rc -eq 0 ]] \
        && grep -q "Running Random Simulation" "$log" \
        && grep -q "Simulation using seed" "$log" \
        && ! grep -q '^Error:' "$log"
    then
        result=simulation
    elif [[ $rc -eq 0 ]] && grep -q "No error has been found" "$log"
    then
        result=green
    elif grep -qE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log"
    then
        violated="$(grep -oE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log" \
            | sed -n '1{s/.* \([A-Za-z0-9_]*\) is violated/\1/p;}')"
        result="violation:${violated}"
    elif grep -qF "Error: Temporal property ${want} was violated." "$log" \
        && grep -Eq "^PROPERTY[[:space:]]+${want}$" "$cfg"
    then
        result="temporal:${want}"
    elif [[ $rc -eq 124 ]]
    then
        result=incomplete
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
        temporal) [[ "$result" == "temporal:${want}" ]] && verdict=PASS ;;
        simulation) [[ "$result" == simulation ]] && verdict=PROBE ;;
        incomplete)
            [[ "$result" == incomplete ]] && verdict=KNOWN
            [[ "$result" == green ]] && { verdict=KNOWN; result="green (tighten expectation)"; }
            ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-22s %-12s %-36s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
    printf '  log: %s\n' "$log"
done

if [[ $# -gt 0 ]]
then
    for requested in "$@"
    do
        requested="${requested##*/}"
        requested="${requested%.cfg}"
        requested="${requested#${MODULE}_}"
        if ! known_name "$requested"
        then
            echo "unknown ${MODULE} config: $requested" >&2
            overall=1
        elif ! printf '%s\n' "${CONFIGS[@]}" | awk '{print $1}' | grep -qx -- "$requested"
        then
            echo "${MODULE}_${requested}.cfg requires SLOW=1" >&2
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
