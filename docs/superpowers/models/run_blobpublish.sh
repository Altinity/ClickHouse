#!/usr/bin/env bash
# Run the complete `CaBlobPublishCore` battery and accept only each exact expected result.
# With one or more config names as arguments, run only those rows with identical assertions.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaBlobPublishCore
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3
mkdir -p ../../../tmp

SAFETY_INVARIANTS=(
    CommittedRefHasContent
    ReadyRequiresObservedMaterialization
    CondemnedNeedsFreshPublication
    FreshAfterCondemned
    PublicationAttemptIsMonotonic
    VerbatimCopyOnlyFirstAbsent
    ExactDeleteCannotRemoveFreshIncarnation
    PublicationRequiresDurablePrecommit
    ReadyRequiresCleanMeta
    FencedWriterCannotCommit
    KeyNamesPayload
)

# Sabotages run before the safe state space and the three negated-reachability witnesses.
# name                            expectation  exact invariant/witness
CONFIGS=(
    "sab_adopt_condemned              violation  CondemnedNeedsFreshPublication"
    "sab_reuse_condemned_envelope     violation  FreshAfterCondemned"
    "sab_recopy_after_condemned       violation  ExactDeleteCannotRemoveFreshIncarnation"
    "sab_recopy_after_absent          violation  ExactDeleteCannotRemoveFreshIncarnation"
    "sab_first_condemned_then_copy    violation  VerbatimCopyOnlyFirstAbsent"
    "sab_unconditional_delete         violation  ExactDeleteCannotRemoveFreshIncarnation"
    "sab_ready_without_reobserve      violation  ReadyRequiresObservedMaterialization"
    "sab_publish_before_precommit     violation  PublicationRequiresDurablePrecommit"
    "sab_skip_meta_clean              violation  ReadyRequiresCleanMeta"
    "sab_commit_after_fence           violation  FencedWriterCannotCommit"
    "sab_wrong_payload                violation  KeyNamesPayload"
    "safe                             green      -"
    "witness_racing_publishers        witness    WitnessRacingPublishers"
    "witness_staged_retag             witness    WitnessStagedRetag"
    "witness_late_landing             witness    WitnessLateLanding"
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
run_id="${MODULE}-$$-$(date +%s%N)"
printf '%-34s %-10s %-42s %10s %10s %8s %s\n' \
    "CONFIG" "EXPECT" "RESULT" "GENERATED" "DISTINCT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue

    cfg="${MODULE}_${name}.cfg"
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
    generated=${generated:--}
    distinct=${distinct:--}

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
    printf '%-34s %-10s %-42s %10s %10s %8s %s\n' \
        "$name" "$expect" "$result" "$generated" "$distinct" "$elapsed" "$verdict"
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
