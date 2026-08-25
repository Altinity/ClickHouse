#!/usr/bin/env bash
# Run the complete `CaRetiredInRun` battery and assert every result by exact invariant name.
# With one or more cfg names as arguments, run only those rows while retaining the same assertions.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaRetiredInRun
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

# Sabotages first: the honest settlement gates are evidence only after each broken rule is seen red.
# name               expectation(green|violation)  expected-invariant
CONFIGS=(
    "sab_attempt_reuse  violation  INV_ONE_PASS"
    "sab_inmem_token    violation  INV_NO_RETURN"
    "sab_no_pacing      violation  INV_NO_LOSS"
    "base               green      -"
    "empty_blobs        green      -"
)

config_file()
{
    local name="$1"
    if [[ "$name" == base ]]
    then
        printf '%s.cfg\n' "$MODULE"
    else
        printf '%s_%s.cfg\n' "$MODULE" "$name"
    fi
}

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
        [[ "$requested" == "$MODULE" ]] && requested=base
        [[ "$requested" == "$name" ]] && return 0
    done
    return 1
}

overall=0
run_id="${MODULE}-$$-$(date +%s%N)"
printf '%-18s %-11s %-32s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue

    cfg="$(config_file "$name")"
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
    printf '%-18s %-11s %-32s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
    printf '  log: %s\n' "$log"
done

if [[ $# -gt 0 ]]
then
    for requested in "$@"
    do
        requested="${requested##*/}"
        requested="${requested%.cfg}"
        requested="${requested#${MODULE}_}"
        [[ "$requested" == "$MODULE" ]] && requested=base
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
