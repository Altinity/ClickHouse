#!/usr/bin/env bash
# Run every CaDiskLifecycle config and print a one-line PASS/FAIL verdict per config.
# Model: the pool lifecycle of a writable CAS mount, its one lease thread (renewal and reclaim in
# sequence), the GC scheduler, and the `SYSTEM CONTENT ADDRESSED FORGET` protocol.
#
# Expectations are asserted BY NAME. Sabotages run first: a green is only evidence once the property
# it rests on has been seen red.
#
#   sab_nogcselfexit            -> GcExitsAfterVanished (temporal)  the GC scheduler never exits on a
#                                                                   natural Vanished transition
#   sab_armafterintent          -> NoJoinWindowReclaim     arming ignores a stop and a terminal
#                                                          lifecycle: a reclaim arms inside the join window
#   sab_notrip2                 -> I1ForgetTerminal        the same, and no re-trip after the join: FORGET
#                                                          completes with the fence armed
#   sab_unearnedfarewell        -> I2EarnedFarewell        the farewell regardless of the drain outcome
#   sab_renewafterintent        -> RenewNotAfterIntent     a renewal starts after the FORGET intent
#   sab_liveunready             -> LiveOnlyWhenAdmissible  a reclaim with a too-old claim arms the
#                                                          fence and reports Live without the renewal
#   sab_livependingrequest      -> NoArmWhileRequestPending  a renewal arms while a request is pending
#   sab_reclaimarmpending       -> NoArmWhileRequestPending  a reclaim arms although a request was
#                                                          raised after its snapshot
#   sab_ackrequested            -> I7RequestNotLost        a reclaim acknowledges the current request,
#                                                          not its snapshot
#   sab_reclaimnotrip           -> I4aTransientImpliesLost a reclaim's step 0 leaves the fence armed
#   sab_reclaimnotrip_vanished  -> I4bVanishedImpliesLost  the same, ending Replaced on an armed fence
#   main                        -> GREEN   every invariant, the four action properties and the three
#                                          liveness properties under FairSpec
#
# Witnesses are negated reachability -- a VIOLATION is the evidence:
#
#   witness_forgetdone            -> WForgetNeverDone         FORGET completes
#   witness_racedreplaced         -> WNoRacedReplaced         FORGET completes on a pool a racing
#                                                             natural promotion left VanishedReplaced
#   witness_livebyrenewal         -> WNoLiveByRenewal         a renewal arms and reports Live for a
#                                                             reclaim whose claim was too old
#   witness_requestduringreclaim  -> WNoRequestDuringReclaim  a remount request is raised during a reclaim
#   witness_forgetfarewell        -> WNoForgetFarewell        FORGET still writes the farewell
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
  "sab_nogcselfexit             temporal   GcExitsAfterVanished"
  "sab_armafterintent           violation  NoJoinWindowReclaim"
  "sab_notrip2                  violation  I1ForgetTerminal"
  "sab_unearnedfarewell         violation  I2EarnedFarewell"
  "sab_renewafterintent         violation  RenewNotAfterIntent"
  "sab_liveunready              violation  LiveOnlyWhenAdmissible"
  "sab_livependingrequest       violation  NoArmWhileRequestPending"
  "sab_reclaimarmpending        violation  NoArmWhileRequestPending"
  "sab_ackrequested             violation  I7RequestNotLost"
  "sab_reclaimnotrip            violation  I4aTransientImpliesLost"
  "sab_reclaimnotrip_vanished   violation  I4bVanishedImpliesLost"
  "main                         green      -"
  "witness_forgetdone           violation  WForgetNeverDone"
  "witness_racedreplaced        violation  WNoRacedReplaced"
  "witness_livebyrenewal        violation  WNoLiveByRenewal"
  "witness_requestduringreclaim violation  WNoRequestDuringReclaim"
  "witness_forgetfarewell       violation  WNoForgetFarewell"
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
printf '%-30s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
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
  elif grep -qE "Temporal propert(ies were|y [A-Za-z0-9_]+ was) violated" "$log"; then
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

  printf '%-30s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
