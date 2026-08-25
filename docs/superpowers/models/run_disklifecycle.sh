#!/usr/bin/env bash
# Run every CaDiskLifecycle config and print a one-line PASS/FAIL verdict per config.
# Model: the rev.8 (FORGET-only v1) pool lifecycle state machine + the as-built
# `SYSTEM CONTENT ADDRESSED FORGET` protocol (spec 2026-07-22 "throw-when-uncertain"
# SS1/SS3/SS5/SS6), the Task-15 gate.
#
# 2026-07-28: rewritten during the v9 ref-chain TLA phase audit. It used to glob every cfg and set
# `overall=1` on any nonzero TLC exit -- so the three sabotages and the three witnesses, whose whole
# purpose is to make TLC exit nonzero, made the suite FAIL every single time it ran. A runner that
# always fails is a runner nobody runs. Expectations are now asserted BY NAME, matching
# run_mount.sh / run_refcatalog.sh.
#
# Sabotages/reds run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_nogcselfexit      -> GcExitsAfterVanished (temporal)  the pre-[C1] scheduler: it never exits
#                                                             on a natural Vanished transition, so a
#                                                             VanishedReplaced pool keeps running GC
#                                                             rounds forever
#   sab_notrip2           -> I1ForgetTerminal                 the second `tripMountLost` removed: a
#                                                             reclaim completing inside the
#                                                             remount-join window re-arms the fence
#                                                             and FORGET finishes with write authority
#                                                             still granted (the Task-10 race)
#   sab_unearnedfarewell  -> I2EarnedFarewell                 the drain gate removed: finishTeardown
#                                                             writes the clean farewell regardless of
#                                                             the drain outcome
#   main                  -> GREEN   the as-built protocol: five safety invariants + both liveness
#                                    properties (a started FORGET always completes; GC exits once
#                                    Vanished) under FairSpec
#
# Witnesses are negated reachability -- a VIOLATION is the evidence. They exist because BFS reports
# the SHORTEST counterexample, so a branch no red travels would silently rot:
#
#   witness_forgetdone          -> WForgetNeverDone        FORGET completion is reachable at all
#   witness_joinwindowreclaim   -> WNoJoinWindowReclaim    the trip#2 race is REAL: an in-flight
#                                                          attempt's reclaim can complete inside the
#                                                          join window, leaving Live/mayMutate at the
#                                                          Trip2 step
#   witness_racedreplaced       -> WNoRacedReplaced        first-terminal-wins is REAL: FORGET can
#                                                          complete on a pool a racing natural
#                                                          promotion left VanishedReplaced
#
# TLC names no property in a liveness counterexample ("Error: Temporal properties were violated."),
# so the `temporal` expectation asserts what can be asserted: a temporal violation happened, NO
# invariant violation happened, and the cfg declares exactly the one property named in the table.
# That keeps an unrelated red from passing as this one.
#
# Exits nonzero if any expectation is unmet.
#
# This is a whole-suite runner: it owns the config list and takes no arguments. To run one
# config by hand:
#   java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers 1 \
#        -config CaDiskLifecycle_<name>.cfg CaDiskLifecycle.tla
#
# `-workers 1`, NOT `-workers auto`, and that is deliberate (see run_refcatalog.sh for the full
# rationale: parallel BFS makes the reported depth, the state counts and WHICH shortest
# counterexample TLC prints vary between identical runs). Override with TLC_WORKERS=auto if you only
# want a verdict and not the numbers.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3
MODULE=CaDiskLifecycle

# name  expectation(green|violation|temporal)  expected-invariant/property(asserted, not just logged)
CONFIGS=(
  "sab_nogcselfexit          temporal   GcExitsAfterVanished"
  "sab_notrip2               violation  I1ForgetTerminal"
  "sab_unearnedfarewell      violation  I2EarnedFarewell"
  "main                      green      -"
  "witness_forgetdone        violation  WForgetNeverDone"
  "witness_joinwindowreclaim violation  WNoJoinWindowReclaim"
  "witness_racedreplaced     violation  WNoRacedReplaced"
)
check_tlc_temporal_expectations "$JAR" "${CONFIGS[@]}" || exit 4

# The PROPERTY/PROPERTIES names a cfg declares, one per line (used by the `temporal` assertion).
declared_properties() {
  awk 'BEGIN { split("SPECIFICATION SPECIFICATIONS INIT NEXT INVARIANT INVARIANTS PROPERTY \
                      PROPERTIES CONSTANT CONSTANTS CONSTRAINT CONSTRAINTS ACTION_CONSTRAINT \
                      SYMMETRY VIEW CHECK_DEADLOCK ALIAS POSTCONDITION", k, " ")
              for (i in k) kw[k[i]] = 1 }
       { sub(/\\\*.*/, ""); if (NF == 0) next
         if ($1 in kw) { inprop = ($1 == "PROPERTY" || $1 == "PROPERTIES")
                         if (inprop) for (i = 2; i <= NF; i++) print $i
                         next }
         if (inprop) for (i = 1; i <= NF; i++) print $i }' "$1"
}

overall=0
printf '%-28s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-lifecycle-${name}"
  rm -rf "$meta"
  start=$SECONDS
  timeout 3600 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
  rc=$?
  elapsed=$((SECONDS - start))

  if grep -q "No error has been found" "$log"; then
    result="green"
  elif grep -qE "(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated" "$log"; then
    result="violation:$(grep -oE '(Invariant|Property|Action property) [A-Za-z0-9_]+ is violated' "$log" \
                        | head -1 | sed -E 's/.* ([A-Za-z0-9_]+) is violated/\1/')"
  elif grep -q "Temporal properties were violated" "$log"; then
    result="temporal:$(declared_properties "$cfg" | paste -sd, -)"
  elif [[ $rc -eq 124 ]]; then
    result="incomplete"
  else
    result="error"
  fi

  verdict="FAIL"
  case "$expect" in
    green)      [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation)  [[ "$result" == "violation:${want}" ]] && verdict="PASS" ;;
    temporal)   [[ "$result" == "temporal:${want}" ]] && verdict="PASS" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1

  printf '%-28s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
