#!/usr/bin/env bash
# Run every CaRefDeltaIntakeCore config and print a one-line PASS/FAIL verdict per config.
# Model: v9 pool-wide GC ref intake (spec 2026-07-27-cas-ref-chain-complete-cut-design.md,
# §1 pool-wide in-degree, §5 fold + destructive-round frontier proof + durable hold, §7 REBUILD).
# Results and traces: CaRefDeltaIntakeCore_RESULTS.md.
#
# Sabotages run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_skip_catalog_target  -> EveryCatalogTargetAttempted  LIST omission may not skip a cut-named life
#   sab_skip_held_retry      -> EveryCarriedHoldRetried      a carried hold exact-retries every round
#   sab_cleanupignorescursor -> GREEN   exact CTE turns premature cleanup into a durable hold
#   sab_adoptbeforecommit    -> NoMissedFold    (LosingCommitAdoptsNothing falls in the same behaviour)
#                                               cursor adoption must be atomic with the fold commit
#   sab_destroyunderhold     -> HoldSuppresses  a carried hold must suppress every destructive site
#   sab_rebuilddropshold     -> HoldReleaseRequiresFold  THE r8 blocker: REBUILD must carry holds verbatim
#   sab_clearholdonabsent    -> HoldReleaseRequiresFold  a hold clears only by folding through its position
#   ctl_holdsuppresses       -> GREEN   the hold sabotages' control (same settings, no toggle)
#   sab_deleteignoresindeg   -> NoAckedLoss     the delete site must RE-READ in-degree: a +1 that
#                                               lands after the probe but before condemnation is
#                                               folded only by the next round
#   v9_safe                  -> GREEN   THE GATE: honest store, zero-trust hint
#   v9_hintomission          -> GREEN   the hint returns nothing, ever (replaces _sab_resumeskip)
#   v9_hold                  -> GREEN   total hint omission + committed gap: CTE re-detects every round
#   plan_safe                -> GREEN   exact catalog-built key set
#   sab_adaptermints         -> PlanKeySetExact  adapters cannot create rows
#   witness_planbuilt        -> WITNESS_PLAN_BUILT  plan construction is reachable
# Exits nonzero if any expectation is unmet.
#
# COVERAGE=1 additionally re-runs the two greens that carry the non-vacuity argument
# (`v9_safe`, `v9_hold`) under `-coverage 1`, which is where the RESULTS action-coverage table comes
# from. Off by default because it roughly doubles their runtime and changes no verdict.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefDeltaIntakeCore

# name  expectation(green|violation)  expected-invariant(for the log line)
CONFIGS=(
  "sab_skip_catalog_target  violation  EveryCatalogTargetAttempted"
  "sab_skip_held_retry      violation  EveryCarriedHoldRetried"
  "sab_adaptermints         violation  PlanKeySetExact"
  "sab_adoptbeforecommit    violation  NoMissedFold"
  "sab_destroyunderhold     violation  HoldSuppresses"
  "sab_rebuilddropshold     violation  HoldReleaseRequiresFold"
  "sab_clearholdonabsent    violation  HoldReleaseRequiresFold"
  "ctl_holdsuppresses       green      -"
  "sab_cleanupignorescursor green      -"
  "sab_deleteignoresindeg   violation  NoAckedLoss"
  "plan_safe                green      -"
  "witness_planbuilt        violation  WITNESS_PLAN_BUILT"
  "v9_safe                  green      -"
  "v9_hintomission          green      -"
  "v9_hold                  green      -"
)

overall=0
printf '%-26s %-10s %-32s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../build/task5b_delta_intake_${name}.log"
  meta="../../../build/tlc-meta-task5b-deltaintake-${name}"
  rm -rf "$meta"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers auto -config "$cfg" "$MODULE.tla" >"$log" 2>&1
  elapsed=$((SECONDS - start))

  if grep -q "No error has been found" "$log"; then
    result="green"
  elif grep -q "is violated" "$log"; then
    result="violation:$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log" | head -1 | awk '{print $2}')"
  else
    result="error"
  fi

  verdict="FAIL"
  case "$expect" in
    green)     [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation) [[ "$result" == "violation:${want}" ]] && verdict="PASS" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1

  printf '%-26s %-10s %-32s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

if [[ "${COVERAGE:-0}" == "1" ]]; then
  echo
  echo "COVERAGE=1: re-running the non-vacuity greens with -coverage 1"
  for name in v9_safe v9_hold; do
    log="../../../build/task5b_delta_intake_cov_${name}.log"
    meta="../../../build/tlc-meta-task5b-deltaintake-cov-${name}"
    rm -rf "$meta"
    /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
      -metadir "$meta" -workers auto -coverage 1 \
      -config "${MODULE}_${name}.cfg" "$MODULE.tla" >"$log" 2>&1
    echo "  coverage ${name}: $log"
  done
fi

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
