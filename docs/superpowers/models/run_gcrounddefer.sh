#!/usr/bin/env bash
# Run the `CaGcRoundDeferCore` sabotage, positive, and reachability-witness configurations.
# Exits nonzero if an asserted expectation is unmet.
set -uo pipefail
cd "$(dirname "$0")"

JAR=../../../tmp/tla2tools.jar
MODULE=CaGcRoundDeferCore
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

# `sab_unbounded_defer` is liveness: TLC does not name the violated temporal property.
# name                     expectation  expected-invariant-or-property
CONFIGS=(
  "sab_graduate_on_stale   violation    NoOverDelete"
  "sab_unbounded_defer     temporal     EventuallyFolded"
  "stage1                  green        -"
  "witness_deferthenfold   violation    W_DeferThenFold"
)
check_tlc_temporal_expectations "$JAR" "${CONFIGS[@]}" || exit 4

overall=0
printf '%-28s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
    read -r name expect want <<<"$row"
    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc_${MODULE}_${name}.log"
    meta="../../../tmp/tlc-meta-${MODULE}-${name}"
    rm -rf "$meta"
    start=$SECONDS
    timeout 3600 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    rc=$?
    elapsed=$((SECONDS - start))

    if grep -q "No error has been found" "$log"; then
        result=green
    elif grep -qE 'Temporal propert(y|ies)( [A-Za-z0-9_]+)? (was|were) violated' "$log"; then
        result="temporal:${want}"
    elif grep -q "is violated" "$log"; then
        result="violation:$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log" | head -1 | awk '{print $2}')"
    else
        result=error
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
        temporal) [[ "$result" == "temporal:${want}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-28s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit "$overall"
