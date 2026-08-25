#!/usr/bin/env bash
# Run the destructive-gate sabotages, positive scenarios, and reachability witnesses.
# Sabotages run first so each green safety result is backed by a named red control.
set -uo pipefail
cd "$(dirname "$0")" || exit 2

JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }

# shellcheck disable=SC1091
source ./tlc_temporal_gate.sh
check_tlc_temporal_gate "$JAR" || exit 4
echo "TEMPORAL SMOKE PASS"

MODULE=CaGcDestructiveGateCore

# name                                      expectation  expected-property
CONFIGS=(
  "sab_gate_omits_anomalies                  violation   PhysicalDeleteOnlyWhenGateOpen"
  "sab_gate_omits_holds                      violation   PhysicalDeleteOnlyWhenGateOpen"
  "sab_gate_omits_frontier                   violation   PhysicalDeleteOnlyWhenGateOpen"
  "sab_gate_accepts_empty_universe           violation   PhysicalDeleteOnlyWhenGateOpen"
  "sab_lifecycle_uses_global_suppression     violation   ProvedRemovalEraseIsNotPhysicalSuppression"
  "healthy                                   green       -"
  "empty_universe                            green       -"
  "anomaly                                   green       -"
  "carried_hold                              green       -"
  "budget_exhausted                          green       -"
  "witness_healthy_physical_delete           violation   WITNESS_HEALTHY_PHYSICAL_DELETE"
  "witness_suppressed_removal_erase          violation   WITNESS_SUPPRESSED_REMOVAL_ERASE"
  "witness_empty_universe_suppressed         violation   WITNESS_EMPTY_UNIVERSE_SUPPRESSED"
)

run_id="${MODULE}-$$-$(date +%s%N)"
overall=0
printf '%-43s %-10s %-48s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
    read -r name expect want <<<"$row"
    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc-${run_id}-${name}.log"
    meta="../../../tmp/tlc-meta-${run_id}-${name}"
    rm -rf "$meta"

    start=$SECONDS
    # `TLC_JAVA_OPTS` intentionally expands to zero or more JVM arguments.
    # shellcheck disable=SC2086
    timeout 3600 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    rc=$?
    elapsed=$((SECONDS - start))

    result=error
    if [[ $rc -eq 124 ]]; then
        result=timeout
    elif [[ $rc -eq 0 ]] && grep -q "No error has been found" "$log"; then
        result=green
    elif grep -q "Temporal properties were violated" "$log"; then
        result=temporal-error
    elif grep -qE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log"; then
        violated="$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log" | sed -n '1{s/^[^ ]* \([^ ]*\).*/\1/p;}')"
        result="violation:${violated}"
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1

    printf '%-43s %-10s %-48s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then
    echo "ALL EXPECTATIONS MET"
else
    echo "SOME EXPECTATIONS UNMET"
fi
exit "$overall"
