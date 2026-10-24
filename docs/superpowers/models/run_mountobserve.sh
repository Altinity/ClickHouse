#!/usr/bin/env bash
# Run the `CaMountObserveCore` battery and accept only each exact expected result.
# With config names as arguments, run only those rows with identical assertions.
#
#   SWEEP=1          also measure the fence margin on a grid of constants (see MARGIN_POINTS)
#   SWEEP=only       run the grid alone
#   TLC_WORKERS      workers for rows that must print one specific trace (default 1)
#   TLC_GREEN_WORKERS  workers for exhaustive green rows (default 4); their state counts do not
#                    depend on it, the reported depth can
#   TLC_JAVA_OPTS    e.g. -Xmx16g
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaMountObserveCore
mkdir -p ../../../tmp

SAFETY_INVARIANTS=(
    TypeOK
    NoFenceInsideAuthority
    NoWriteAfterFence
    FenceMarginAtLeast
)

# Reds first: a green is evidence only once the property it rests on has been seen red.
# name                                  expectation  exact invariant/witness
CONFIGS=(
    "sab_sighting_before_read              violation  NoFenceInsideAuthority"
    "sab_redecision_stale_sample           violation  NoFenceInsideAuthority"
    "sab_wrapping_stability                violation  NoFenceInsideAuthority"
    "stale_stamps_long_walk                violation  NoFenceInsideAuthority"
    "stale_stamps_nodrift_long_walk        violation  NoFenceInsideAuthority"
    "env_drift_exceeds_allowance           violation  NoFenceInsideAuthority"
    "margin_gc_tight                       violation  FenceMarginAtLeast"
    "margin_remount_tight                  violation  FenceMarginAtLeast"
    "margin_earlyfence_tight               violation  FenceMarginAtLeast"
    "gc                                    green      -"
    "remount                               green      -"
    "earlyfence                            green      -"
    "stale_stamps_short_walk               green      -"
    "stale_stamps_nodrift_short_walk       green      -"
    "healthy_gc                            green      -"
    "healthy_remount                       green      -"
    "witness_observer_fences               witness    W_ObserverFences"
    "witness_failing_holder_keeps_lease    witness    W_FailingHolderKeepsLeaseToItsEnd"
    "witness_refused_fence_redecided       witness    W_RefusedFenceIsRedecided"
    "witness_healthy_outlives_threshold    witness    W_HealthyHolderOutlivesTheThreshold"
)

# SWEEP=1. Each point asserts that the fence margin is EXACTLY `margin` holder ticks: green with
# `MinMargin = margin`, `FenceMarginAtLeast` violated with `MinMargin = margin + 1`. The horizon is
# `Threshold + 3`. Every sabotage is off.
# name            TTL Period Allowance Cadence AuthoritySpan GainEvery  margin
MARGIN_POINTS=(
    "nodrift_gc        4  2  1  2  4  0   3"
    "nodrift_remount   4  2  1  1  4  0   2"
    "noallowance       4  2  0  2  4  0   2"
    "drift1_gc         4  2  1  2  4  4   1"
    "drift1_remount    4  2  1  1  4  4   0"
    "drift2_gc         4  2  1  2  4  2   0"
    "allowance2        4  2  2  2  4  4   2"
    "span2             4  2  1  2  2  4   3"
    "ttl6_gc           6  2  1  2  6  6   1"
    "ttl6_cadence3     6  3  1  3  6  6   2"
    "ttl6_drift2       6  3  1  3  6  3   1"
    "ttl10_gc         10  3  1  3 10 10   2"
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
    local violation_count behavior_count error_count fatal_count statistics_count
    local green_statistics_count depth_count footer_count no_error_count violated

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

source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

overall=0
executed=0
run_id="${MODULE}-$$-$(date +%s%N)"

# run_row <label> <cfg path> <expect> <want>
run_row()
{
    local label="$1" cfg="$2" expect="$3" want="$4"
    local log="../../../tmp/tlc-${run_id}-${label}.log"
    local meta="../../../tmp/tlc-meta-${run_id}-${label}"
    local workers="${TLC_WORKERS:-1}"
    [[ "$expect" == green ]] && workers="${TLC_GREEN_WORKERS:-4}"

    local start=$SECONDS
    timeout "${TLC_TIMEOUT:-3600}" nice -n 10 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} \
        -cp "$JAR" tlc2.TLC -metadir "$meta" -workers "$workers" \
        -noGenerateSpecTE -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    local rc=$?
    local elapsed=$((SECONDS - start))
    rm -rf "$meta"

    local result stats generated distinct depth
    result="$(classify_tlc_result "$log" "$rc")"
    stats="$(grep -E '^[0-9,]+ states generated, [0-9,]+ distinct states found' "$log" | tail -n 1)"
    generated="$(sed -nE 's/^([0-9,]+) states generated,.*/\1/p' <<<"$stats")"
    distinct="$(sed -nE 's/^[0-9,]+ states generated, ([0-9,]+) distinct states found,.*/\1/p' <<<"$stats")"
    depth="$(sed -nE 's/^The depth of the complete state graph search is ([0-9]+).*/\1/p' "$log" | tail -n 1)"

    local verdict=FAIL
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
    printf '%-36s %-10s %-46s %12s %11s %6s %8s %s\n' \
        "$label" "$expect" "$result" "${generated:--}" "${distinct:--}" "${depth:--}" "$elapsed" "$verdict"
    printf '  log: %s\n' "$log"
}

printf '%-36s %-10s %-46s %12s %11s %6s %8s %s\n' \
    "CONFIG" "EXPECT" "RESULT" "GENERATED" "DISTINCT" "DEPTH" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    [[ "${SWEEP:-0}" == only ]] && break
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue
    executed=$((executed + 1))
    run_row "$name" "${MODULE}_${name}.cfg" "$expect" "$want"
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

if [[ "${SWEEP:-0}" != only ]]
then
    if [[ $executed -eq 0 ]]
    then
        echo "no ${MODULE} rows executed" >&2
        overall=1
    fi
    if [[ $# -eq 0 && $executed -ne ${#CONFIGS[@]} ]]
    then
        overall=1
    fi
fi

sweep_cfg()
{
    local path="$1" ttl="$2" period="$3" allowance="$4" cadence="$5" span="$6" gain="$7" min_margin="$8"
    local horizon=$((ttl + allowance + cadence + 3))
    cat >"$path" <<CFG
SPECIFICATION Spec
CONSTANTS
    None = NoneVal
    TTL = $ttl
    Period = $period
    Allowance = $allowance
    Cadence = $cadence
    AuthoritySpan = $span
    GainEvery = $gain
    MaxClock = $horizon
    MaxWalk = $horizon
    Healthy = FALSE
    MaxLatency = 0
    MinMargin = $min_margin
    SabSightingBeforeRead = FALSE
    SabRedecisionStaleSample = FALSE
    SabWrappingStability = FALSE
CHECK_DEADLOCK FALSE
INVARIANTS
    TypeOK
    NoFenceInsideAuthority
    NoWriteAfterFence
    FenceMarginAtLeast
CFG
}

swept=0
if [[ "${SWEEP:-0}" =~ ^(1|only)$ && $# -eq 0 ]]
then
    echo
    echo "SWEEP: exact fence margin per point (green at margin, red at margin + 1)"
    for row in "${MARGIN_POINTS[@]}"
    do
        read -r name ttl period allowance cadence span gain margin <<<"$row"
        at="../../../tmp/${MODULE}_sweep_${name}_at.cfg"
        above="../../../tmp/${MODULE}_sweep_${name}_above.cfg"
        sweep_cfg "$at" "$ttl" "$period" "$allowance" "$cadence" "$span" "$gain" "$margin"
        sweep_cfg "$above" "$ttl" "$period" "$allowance" "$cadence" "$span" "$gain" "$((margin + 1))"
        run_row "sweep_${name}_margin_${margin}" "$at" green -
        run_row "sweep_${name}_margin_$((margin + 1))" "$above" violation FenceMarginAtLeast
        swept=$((swept + 1))
    done
fi

echo
[[ "${SWEEP:-0}" != only ]] && echo "EXECUTED ROWS: ${executed}/${#CONFIGS[@]}"
if [[ "${SWEEP:-0}" =~ ^(1|only)$ && $# -eq 0 ]]
then
    echo "SWEPT POINTS: ${swept}/${#MARGIN_POINTS[@]}"
    [[ $swept -ne ${#MARGIN_POINTS[@]} ]] && overall=1
fi
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
