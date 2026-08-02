#!/usr/bin/env bash
# Run every CaRefFoldClampRecoveryCore config and print a one-line PASS/FAIL verdict per config.
# Model: fold-clamp recoverability (spec 2026-07-11-cas-ref-table-snapshot-log-design.md
# SS gc-step-produce-manifest-edge-delta transaction atomicity / SS gc-retire) -- per-log staging
# means a clamp is always recoverable and the post-CAS delete never reclaims an unfolded log's body.
#
# 2026-07-28: expectations upgraded from bare colours to NAME assertions during the v9 ref-chain TLA
# phase audit. The old classifier accepted `is violated|Error:` as "violation", so a parse error, a
# deadlock or a violation of an entirely different invariant all read as the expected red.
#
# Sabotage runs FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_edgegranularity -> NoDeleteBehindClamp   commit a `-1` body token at EDGE granularity (the
#                                                pre-fix bug) instead of on full log fold: the
#                                                post-CAS delete reclaims A's body while its log is
#                                                clamped, and every re-fold clamps forever
#   safe                -> GREEN                 per-log staging: NoDeleteBehindClamp + EventuallyFolded
#
# The sabotage breaks BOTH declared properties -- the safety invariant `NoDeleteBehindClamp` and the
# liveness `EventuallyFolded` (a permanent freeze) -- and the expectation names the invariant because
# that is what BFS reports first. The `temporal` classification arm below exists for the case where
# it does not: without it a liveness-first report would land in `error`, which fails closed but
# reads as a harness problem during triage rather than as "the same sabotage, the other property".
#
# Exits nonzero if any expectation is unmet.
#
# This is a whole-suite runner: it owns the config list and takes no arguments. To run one
# config by hand:
#   java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers 1 \
#        -config CaRefFoldClampRecoveryCore_<name>.cfg CaRefFoldClampRecoveryCore.tla
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
MODULE=CaRefFoldClampRecoveryCore

# name  expectation(green|violation|temporal)  expected-invariant/property(asserted, not just logged)
CONFIGS=(
  "sab_edgegranularity  violation  NoDeleteBehindClamp"
  "safe                 green      -"
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
printf '%-22s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-foldclamp-${name}"
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

  printf '%-22s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
