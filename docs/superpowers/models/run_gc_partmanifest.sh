#!/usr/bin/env bash
# Run the `CaGcRootLocalPartManifestCore` battery and assert exact outcome kinds and names.
# `SLOW=1` adds the five historically long positive proofs; every config remains owned by this runner.
set -uo pipefail
cd "$(dirname "$0")"

JAR=${TLC_JAR:-../../../tmp/tla2tools.jar}
MODULE=CaGcRootLocalPartManifestCore
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

# Sabotages first. Three sharding rows deliberately pin the model's recorded composite-`UNCHANGED`
# regression;
# accepting an arbitrary TLC error here would turn a broken model into false red evidence.
# name                             expectation             expected-name
CONFIGS=(
    "sab_acceptnamespacemismatch     violation               INV_NO_DANGLE"
    "sab_acceptrefmismatch           violation               INV_NO_LOSS"
    "sab_advancepastmissingbody      violation               INV_NO_DANGLE"
    "sab_barenonce                   violation               INV_NO_LOSS"
    "sab_commitskipblobreval         violation               INV_NO_DANGLE"
    "sab_crosssharddisplacement      known-model-error       UnchangedCompositeVars"
    "sab_cutoverclaim                violation               INV_NO_DANGLE"
    "sab_deletebodybeforedecrements  temporal                NoLeakForever"
    "sab_deposedleaderwritesfinalgen violation               INV_ONLY_ADOPTED_VIEWABLE"
    "sab_frozenseqauthority          violation               INV_NO_DANGLE"
    "sab_keybyrefnotid               violation               INV_NO_LOSS"
    "sab_lazyfenceunsafe             violation               INV_NO_DANGLE"
    "sab_missingbodyactivated        violation               INV_NO_LOSS"
    "sab_missingcommittedempty       violation               INV_NO_LOSS"
    "sab_mutableasreachability       violation               INV_NO_LOSS"
    "sab_nofence                     violation               INV_NO_DANGLE"
    "sab_noorphansweep               temporal                OrphanManifestDebrisDrains"
    "sab_precommitlessprotect        violation               INV_NO_DANGLE"
    "sab_promoteaftermissingbody     violation               INV_NO_LOSS"
    "sab_reducerownsfence            known-model-error       UnchangedCompositeVars"
    "sab_reusedtag                   violation               INV_NO_RETURN"
    "sab_reusemanifestid             violation               INV_NO_LOSS"
    "sab_roundvisibilityearly        violation               INV_NO_DANGLE"
    "sab_splitpromote                violation               INV_NO_DANGLE"
    "sab_staletokenoverdelete        violation               INV_NO_LOSS"
    "sab_trimunincorporated          violation               INV_JOURNAL_COVERAGE"
    "sab_twoowners                   violation               INV_NO_LOSS"
    "sab_unconddelete                violation               INV_NO_DANGLE"
    "sab_wholesaleprefixdelete       violation               INV_NO_DANGLE"
    "empty_namespaces                green                   -"
    "stage0                          green                   -"
    "stage1                          green                   -"
    "stage5_retiretoken              green                   -"
    "stage5_sharding                 known-model-error       UnchangedCompositeVars"
    "stage6_attemptscoping           green                   -"
    "witness_committedoverfoldedblob violation               W_CommittedOverFoldedBlob"
    "witness_orphandeleted           violation               W_OrphanDeleted"
    "witness_precommitmissingbody    violation               W_PrecommitMissingBodyReached"
    "witness_twoleadersoneadopt      violation               W_TwoLeadersOneAdopt"
)

SLOW_CONFIGS=(
    "stage2                          green                   -"
    "stage3                          incomplete              -"
    "stage4                          green                   -"
    "stage5_lazytrim                 green                   -"
    "live                            green                   -"
)
[[ "${SLOW:-0}" == 1 ]] && CONFIGS+=("${SLOW_CONFIGS[@]}")
check_tlc_temporal_expectations "$JAR" "${CONFIGS[@]}" || exit 3

orig_vars_line="$(grep -n '^origVars == << present,' "$MODULE.tla" | cut -d: -f1)"
reduce_unchanged_line="$(awk '
    /^GReduceShard\(/ { in_reduce = 1 }
    in_reduce && /UNCHANGED vars/ { print NR; exit }
' "$MODULE.tla")"
if [[ -z "$orig_vars_line" || -z "$reduce_unchanged_line" ]]
then
    echo "cannot locate ${MODULE} composite-UNCHANGED regression provenance" >&2
    exit 3
fi

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
printf '%-34s %-17s %-38s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"
do
    read -r name expect want <<<"$row"
    selected "$name" "$@" || continue

    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc-${run_id}-${name}.log"
    meta="../../../tmp/tlc-meta-${run_id}-${name}"
    timeout_seconds="${TLC_TIMEOUT:-3600}"
    [[ "$expect" == incomplete ]] && timeout_seconds="${TLC_SLOW_TIMEOUT:-60}"
    start=$SECONDS
    timeout "$timeout_seconds" /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
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
    elif grep -qF "Error: Temporal property ${want} was violated." "$log" \
        && grep -Eq "^PROPERTY[[:space:]]+${want}$" "$cfg"
    then
        result="temporal:${want}"
    elif grep -qF "identifier present is either undefined or not an operator" "$log" \
        && grep -Eq "line ${orig_vars_line},.*of module ${MODULE}" "$log" \
        && grep -Eq "Line ${reduce_unchanged_line},.*in ${MODULE}" "$log"
    then
        result="model-error:UnchangedCompositeVars"
    elif [[ $rc -eq 124 ]]
    then
        result=timeout
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
        temporal) [[ "$result" == "temporal:${want}" ]] && verdict=PASS ;;
        known-model-error) [[ "$result" == "model-error:${want}" ]] && verdict=KNOWN ;;
        incomplete)
            [[ "$result" == timeout ]] && verdict=KNOWN
            [[ "$result" == green ]] && { verdict=KNOWN; result="green (tighten expectation)"; }
            ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-34s %-17s %-38s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
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
