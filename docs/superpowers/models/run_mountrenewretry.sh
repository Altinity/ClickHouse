#!/usr/bin/env bash
# Run the complete `CaMountRenewRetryCore` battery and accept only each exact expected result.
# With one or more config names as arguments, run only those rows with identical assertions.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaMountRenewRetryCore
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

classify_tlc_result()
{
    local log="$1"
    local rc="$2"
    local violation_count
    local behavior_count
    local error_count
    local fatal_count
    local statistics_count
    local depth_count
    local footer_count
    local no_error_count
    local green_statistics_count
    local violated

    if [[ $rc -eq 124 ]]
    then
        echo timeout
        return
    fi
    if grep -qE 'Deadlock reached|Error: Deadlock' "$log"
    then
        echo deadlock
        return
    fi
    if grep -qE 'Parse Error|Error: Parsing|Semantic errors|Parsing or semantic analysis failed' "$log"
    then
        echo parse-error
        return
    fi

    violation_count="$(grep -cE '^Error: (Invariant|Property|Action property) [A-Za-z0-9_]+ is violated\.$' "$log" || true)"
    behavior_count="$(grep -cE '^Error: The behavior up to this point is:$' "$log" || true)"
    error_count="$(grep -cE '^Error:' "$log" || true)"
    fatal_count="$(grep -cE 'Exception in thread|java\.[A-Za-z0-9_.]*(Error|Exception)|OutOfMemoryError|StackOverflowError|TLC threw an unexpected exception|^Killed$|^Aborted$' "$log" || true)"
    statistics_count="$(grep -cE '^[0-9,]+ states generated, [0-9,]+ distinct states found, [0-9,]+ states left on queue\.$' "$log" || true)"
    green_statistics_count="$(grep -cE '^[0-9,]+ states generated, [0-9,]+ distinct states found, 0 states left on queue\.$' "$log" || true)"
    depth_count="$(grep -cE '^The depth of the complete state graph search is [0-9]+\.$' "$log" || true)"
    footer_count="$(grep -cE '^Finished in .* at \(.*\)$' "$log" || true)"
    no_error_count="$(grep -cE '^Model checking completed\. No error has been found\.$' "$log" || true)"

    if [[ $violation_count -gt 0 ]]
    then
        if [[ $rc -ne 12 ]]
        then
            echo unallowed-exit
            return
        fi
        if [[ $violation_count -ne 1 ]]
        then
            echo multiple-violations
            return
        fi
        if [[ $fatal_count -ne 0 \
              || $behavior_count -gt 1 \
              || $error_count -ne $((violation_count + behavior_count)) \
              || $no_error_count -ne 0 ]]
        then
            echo additional-error
            return
        fi
        if [[ $behavior_count -ne 1 \
              || $statistics_count -ne 1 \
              || $depth_count -ne 1 \
              || $footer_count -ne 1 ]]
        then
            echo incomplete
            return
        fi
        violated="$(sed -nE 's/^Error: (Invariant|Property|Action property) ([A-Za-z0-9_]+) is violated\.$/\2/p' "$log")"
        echo "violation:${violated}"
        return
    fi

    if [[ $rc -ne 0 ]]
    then
        echo unallowed-exit
        return
    fi
    if [[ $fatal_count -ne 0 || $error_count -ne 0 ]]
    then
        echo additional-error
        return
    fi
    if [[ $no_error_count -ne 1 \
          || $statistics_count -ne 1 \
          || $green_statistics_count -ne 1 \
          || $depth_count -ne 1 \
          || $footer_count -ne 1 ]]
    then
        echo incomplete
        return
    fi
    echo green
}

write_violation_fixture()
{
    local log="$1"
    local name="$2"
    {
        printf 'Error: Invariant %s is violated.\n' "$name"
        printf 'Error: The behavior up to this point is:\n'
        printf '10 states generated, 8 distinct states found, 2 states left on queue.\n'
        printf 'The depth of the complete state graph search is 4.\n'
        printf 'Finished in 00s at (fixture)\n'
    } > "$log"
}

write_green_fixture()
{
    local log="$1"
    {
        printf 'Model checking completed. No error has been found.\n'
        printf '10 states generated, 8 distinct states found, 0 states left on queue.\n'
        printf 'The depth of the complete state graph search is 4.\n'
        printf 'Finished in 00s at (fixture)\n'
    } > "$log"
}

runner_self_test()
{
    local fixture_dir
    local failures=0
    fixture_dir="$(mktemp -d ../../../tmp/mountrenewretry-runner-selftest.XXXXXX)" || return 1
    trap "rm -rf -- '$fixture_dir'" EXIT

    local complete_violation="$fixture_dir/complete-violation.log"
    local complete_green="$fixture_dir/complete-green.log"
    local second_violation="$fixture_dir/second-violation.log"
    local additional_error="$fixture_dir/additional-error.log"
    local missing_statistics="$fixture_dir/missing-statistics.log"
    local missing_footer="$fixture_dir/missing-footer.log"

    write_violation_fixture "$complete_violation" ExpectedInvariant
    write_green_fixture "$complete_green"
    write_violation_fixture "$second_violation" ExpectedInvariant
    printf 'Error: Invariant SecondInvariant is violated.\n' >> "$second_violation"
    write_violation_fixture "$additional_error" ExpectedInvariant
    printf 'Error: synthetic checker failure\n' >> "$additional_error"
    write_violation_fixture "$missing_statistics" ExpectedInvariant
    sed -i '/states generated/d' "$missing_statistics"
    write_violation_fixture "$missing_footer" ExpectedInvariant
    sed -i '/^Finished in /d' "$missing_footer"

    check_fixture()
    {
        local label="$1"
        local want="$2"
        local log="$3"
        local rc="$4"
        local got
        got="$(classify_tlc_result "$log" "$rc")"
        if [[ "$got" == "$want" ]]
        then
            printf 'PASS %-32s %s\n' "$label" "$got"
        else
            printf 'FAIL %-32s wanted=%s got=%s\n' "$label" "$want" "$got"
            failures=$((failures + 1))
        fi
    }

    check_fixture complete-violation violation:ExpectedInvariant "$complete_violation" 12
    check_fixture complete-green green "$complete_green" 0
    check_fixture abnormal-exit unallowed-exit "$complete_violation" 137
    check_fixture second-violation multiple-violations "$second_violation" 12
    check_fixture additional-error additional-error "$additional_error" 12
    check_fixture missing-statistics incomplete "$missing_statistics" 12
    check_fixture missing-footer incomplete "$missing_footer" 12

    printf 'SELF-TESTS: %s/7 passed\n' "$((7 - failures))"
    [[ $failures -eq 0 ]]
}

if [[ ${1:-} == "--self-test" ]]
then
    [[ $# -eq 1 ]] || { echo "--self-test takes no additional arguments" >&2; exit 2; }
    runner_self_test
    exit $?
fi

source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

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

    result="$(classify_tlc_result "$log" "$rc")"

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
