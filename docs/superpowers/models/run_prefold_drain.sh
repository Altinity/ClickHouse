#!/usr/bin/env bash
# Run the focused two-leader pre-fold catalog-drain gate. Sabotages run before the green and witness.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefPreFoldDrainCore

# name                              expectation       expected invariant
CONFIGS=(
  "sab_fold_bypasses_drain             violation DrainBeforeDecision"
  "sab_rebuild_bypasses_drain          violation DrainBeforeDecision"
  "sab_defer_bypasses_drain            violation DrainBeforeDecision"
  "sab_continue_after_unknown          violation DrainBeforeDecision"
  "sab_stale_delete_after_successor_hold violation DeleteUsesCurrentAdoptedProof"
  "sab_rebuild_from_unadopted_seal      violation DeleteUsesCurrentAdoptedProof"
  "sab_intake_uses_predrain_cut          violation IntakeConsumesFreshPostDrainCut"
  "sab_intake_uses_stale_token           violation IntakeConsumesFreshPostDrainCut"
  "sab_cut_before_list                    violation FreshCutFollowsCompletedHotList"
  "sab_absent_listed_defers               violation DeadListedPredecessorIsInert"
  "safe                                green     -"
  "witness_takeover_converges          violation WITNESS_TAKEOVER_CONVERGES"
  "witness_drained_row_absent_from_intake violation WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE"
  "witness_rebirth_with_retained_debris_adopts violation WITNESS_REBIRTH_WITH_RETAINED_DEBRIS_ADOPTS"
)

# The same drain also has a two-row serial-rescan core: a full-catalog token changed by the first
# deletion must not let the decision skip the second eligible row.
ALL_ROWS_MODULE=CaRefPreFoldDrainAllRowsCore
ALL_ROWS_CONFIGS=(
  "sab_skiprescan     violation AllEligibleRowsResolvedBeforeDecision"
  "sab_nonexactdelete violation ExactCatalogCAS"
  "safe               green     -"
)

overall=0
printf '%-40s %-10s %-38s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-prefold-${name}"
  rm -rf "$meta"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
  elapsed=$((SECONDS - start))

  if grep -q "No error has been found" "$log"; then
    result="green"
  elif grep -q "is violated" "$log"; then
    result="violation:$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log" | head -1 | awk '{print $2}')"
  else
    result="error"
  fi

  verdict=FAIL
  case "$expect" in
    green)     [[ "$result" == green ]] && verdict=PASS ;;
    violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
  esac
  [[ "$verdict" == FAIL ]] && overall=1
  printf '%-40s %-10s %-38s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

for row in "${ALL_ROWS_CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${ALL_ROWS_MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${ALL_ROWS_MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-prefold-allrows-${name}"
  rm -rf "$meta"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$ALL_ROWS_MODULE.tla" >"$log" 2>&1
  elapsed=$((SECONDS - start))

  if grep -q "No error has been found" "$log"; then
    result="green"
  elif grep -q "is violated" "$log"; then
    result="violation:$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log" | head -1 | awk '{print $2}')"
  else
    result="error"
  fi

  verdict=FAIL
  case "$expect" in
    green)     [[ "$result" == green ]] && verdict=PASS ;;
    violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
  esac
  [[ "$verdict" == FAIL ]] && overall=1
  printf '%-40s %-10s %-38s %-8s %s\n' "allrows_${name}" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
