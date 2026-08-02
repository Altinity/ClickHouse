#!/usr/bin/env bash
# Run every CaBuildRootPrecommit config and print a one-line PASS/FAIL verdict per config.
# Model: the B140/B171 adopted-blob dangle fix -- build-root structural reachability
# (`UseBuildRoot`) x fail-closed commit (`FailClosedCommit`) as a 2x2 necessity/sufficiency matrix --
# plus the B199-S2 inline-closure liveness fix (`InlineClosure`). Documented conclusions and
# counterexample traces: docs/superpowers/cas/06-tla-models.md {#cabuildRootPrecommit}.
#
# 2026-07-28: written during the v9 ref-chain TLA phase audit. This model had NO runner at all -- its
# configs were run by hand and their colours lived only in prose, which is how the parse bug fixed in
# the same commit (an unparenthesised ghost disjunction at `Commit`, reachable only under
# `FailClosedCommit = FALSE`) survived unnoticed. Expectations are asserted BY NAME here, matching
# run_mount.sh / run_refcatalog.sh: a red whose invariant nobody checks is a red that rots.
#
# Sabotages/reds run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   buggy            -> INV_NO_DANGLE_COMMITTED  neither half of the fix: adopt-without-ownership ->
#                                                owner retires -> GcDelete -> blind commit dangles
#   buildrootonly    -> INV_NO_DANGLE_COMMITTED  build-root alone is insufficient: the blob reaches
#                                                in-degree 0 BEFORE the precommit edge exists
#   lazyleak         -> INV_NO_LEAK (temporal)   the B199-S2 leak: the lazy path records an EMPTY
#                                                closure when the tree object is already gone, so the
#                                                blob is never snapped and GcDelete can never fire
#   failclosedonly   -> GREEN                    fail-closed commit alone holds for SAFETY (the
#                                                build-root arm is vacuous at UseBuildRoot = FALSE)
#   fixed            -> GREEN                    both halves: all four safety invariants
#   inlineclosure    -> GREEN                    + INV_NO_LEAK under FairSpec
#   inlineclosure_b2 -> GREEN                    two blobs: shared spared, unique reclaimed
#   b2_witness       -> W_SharedSparedUniqueReclaimed
#                                                non-vacuity of the b2 run: the "shared spared AND
#                                                unique reclaimed" state is really REACHED (a
#                                                witness is negated reachability, so the VIOLATION
#                                                is the evidence)
#
# `INV_NO_LEAK` is a liveness property and TLC names no property in a liveness counterexample
# ("Error: Temporal properties were violated."), so the `temporal` expectation asserts what can be
# asserted: a temporal violation happened, NO invariant violation happened, and the cfg declares
# exactly the one property named in the table. That keeps an unrelated red from passing as this one.
#
# Exits nonzero if any expectation is unmet.
#
# This is a whole-suite runner: it owns the config list and takes no arguments. To run one
# config by hand:
#   java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers 1 \
#        -config CaBuildRootPrecommit_<name>.cfg CaBuildRootPrecommit.tla
#
# `-workers 1`, NOT `-workers auto`, and that is deliberate (see run_refcatalog.sh for the full
# rationale: parallel BFS makes the reported depth, the state counts and WHICH shortest
# counterexample TLC prints vary between identical runs, while the traces narrated in
# 06-tla-models.md are specific action sequences). Override with TLC_WORKERS=auto if you only want a
# verdict and not the numbers.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3
MODULE=CaBuildRootPrecommit

# name  expectation(green|violation|temporal)  expected-invariant/property(asserted, not just logged)
CONFIGS=(
  "buggy             violation  INV_NO_DANGLE_COMMITTED"
  "buildrootonly     violation  INV_NO_DANGLE_COMMITTED"
  "lazyleak          temporal   INV_NO_LEAK"
  "failclosedonly    green      -"
  "fixed             green      -"
  "inlineclosure     green      -"
  "inlineclosure_b2  green      -"
  "b2_witness        violation  W_SharedSparedUniqueReclaimed"
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
printf '%-20s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-brp-${name}"
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

  printf '%-20s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
