#!/usr/bin/env bash
# Run the complete `CaMountRenewRetryCore` battery and accept only each exact expected result.
# With one or more config names as arguments, run only those rows with identical assertions.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaMountRenewRetryCore
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3
mkdir -p ../../../tmp

SAFETY_INVARIANTS=(
    ExactAttemptOnly
    ForeignOrSuccessorNeverAdopted
    ConfirmedDeadlineNeverExtendedByResponse
    NoRequestAfterSafeDeadline
    TerminalNeverRearmsAuthority
    OneLogicalBodyPerExpectedToken
    AcknowledgedRenewalIsDurable
    LateDeliveryCannotOverwriteSuccessor
    PendingSurvivesLocalTerminal
    OneIncarnationPerPredecessor
    CadenceAnchoredAtAttemptStart
)

# Sabotages run first, followed by the safe graph and six negated-reachability witnesses.
# name                                  expectation  exact invariant/witness
CONFIGS=(
    "sab_ignore_attempt_id                  violation  ExactAttemptOnly"
    "sab_refresh_deadline_from_response     violation  ConfirmedDeadlineNeverExtendedByResponse"
    "sab_retry_with_new_body                violation  OneLogicalBodyPerExpectedToken"
    "sab_accept_after_terminal              violation  TerminalNeverRearmsAuthority"
    "sab_accept_successor                   violation  ForeignOrSuccessorNeverAdopted"
    "sab_drop_pending_on_terminal           violation  PendingSurvivesLocalTerminal"
    "sab_late_rearm                         violation  TerminalNeverRearmsAuthority"
    "sab_response_relative_cadence          violation  CadenceAnchoredAtAttemptStart"
    "sab_send_after_deadline                violation  NoRequestAfterSafeDeadline"
    "sab_double_conditional_landing         violation  OneIncarnationPerPredecessor"
    "safe                                  green      -"
    "witness_direct_retry                  witness    WitnessDirectRetry"
    "witness_read_adoption                 witness    WitnessReadAdoption"
    "witness_exhaustion_fences             witness    WitnessExhaustionFences"
    "witness_late_before_reclaim           witness    WitnessLateBeforeReclaim"
    "witness_late_after_successor          witness    WitnessLateAfterSuccessor"
    "witness_catchup                       witness    WitnessImmediateCatchup"
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

witness_declares_safety()
{
    local cfg="$1"
    local invariant
    for invariant in "${SAFETY_INVARIANTS[@]}"
    do
        grep -qE "^[[:space:]]*${invariant}[[:space:]]*$" "$cfg" || return 1
    done
}

overall=0
executed=0
run_id="${MODULE}-$$-$(date +%s%N)"
printf '%-39s %-10s %-48s %10s %10s %7s %8s %s\n' \
    "CONFIG" "EXPECT" "RESULT" "GENERATED" "DISTINCT" "DEPTH" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue

    cfg="${MODULE}_${name}.cfg"
    executed=$((executed + 1))
    log="../../../tmp/tlc-${run_id}-${name}.log"
    meta="../../../tmp/tlc-meta-${run_id}-${name}"
    start=$SECONDS
    timeout "${TLC_TIMEOUT:-3600}" /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} \
        -cp "$JAR" tlc2.TLC -metadir "$meta" -workers "${TLC_WORKERS:-1}" \
        -noGenerateSpecTE -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    rc=$?
    elapsed=$((SECONDS - start))

    result=error
    if [[ $rc -eq 124 ]]
    then
        result=timeout
    elif grep -qE 'Deadlock reached|Error: Deadlock' "$log"
    then
        result=deadlock
    elif grep -qE 'Parse Error|Error: Parsing|Semantic errors|Parsing or semantic analysis failed' "$log"
    then
        result=parse-error
    elif grep -qE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log"
    then
        violated="$(grep -oE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log" \
            | sed -n '1{s/.* \([A-Za-z0-9_]*\) is violated/\1/p;}')"
        result="violation:${violated}"
    elif [[ $rc -eq 0 ]] && grep -q 'No error has been found' "$log"
    then
        result=green
    fi

    stats="$(grep -E '[0-9,]+ states generated, [0-9,]+ distinct states found' "$log" | tail -n 1)"
    generated="$(sed -nE 's/^([0-9,]+) states generated,.*/\1/p' <<<"$stats")"
    distinct="$(sed -nE 's/^[0-9,]+ states generated, ([0-9,]+) distinct states found,.*/\1/p' <<<"$stats")"
    depth="$(sed -nE 's/^The depth of the complete state graph search is ([0-9]+).*/\1/p' "$log" | tail -n 1)"
    if [[ -z "$depth" ]]
    then
        depth="$(sed -nE 's/^State ([0-9]+):.*/\1/p' "$log" | tail -n 1)"
    fi
    generated=${generated:--}
    distinct=${distinct:--}
    depth=${depth:--}

    verdict=FAIL
    case "$expect" in
        green)
            [[ "$result" == green ]] && verdict=PASS
            ;;
        violation)
            [[ "$result" == "violation:${want}" ]] && verdict=PASS
            ;;
        witness)
            if [[ "$result" == "violation:${want}" ]] && witness_declares_safety "$cfg"
            then
                result="witness:${want}"
                verdict=PASS
            fi
            ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-39s %-10s %-48s %10s %10s %7s %8s %s\n' \
        "$name" "$expect" "$result" "$generated" "$distinct" "$depth" "$elapsed" "$verdict"
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

if [[ $executed -eq 0 ]]
then
    echo "no ${MODULE} rows executed" >&2
    overall=1
fi

if [[ $# -eq 0 && $executed -ne ${#CONFIGS[@]} ]]
then
    overall=1
fi

echo
echo "EXECUTED ROWS: ${executed}/${#CONFIGS[@]}"
if [[ $overall -eq 0 ]]
then
    if [[ $# -eq 0 ]]
    then
        echo "ALL EXPECTATIONS MET"
    else
        echo "SELECTED EXPECTATIONS MET"
    fi
else
    echo "SOME EXPECTATIONS UNMET"
fi
exit "$overall"
