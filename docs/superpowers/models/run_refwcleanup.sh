#!/usr/bin/env bash
# Run every CaRefWriterCleanupCore config and print a one-line PASS/FAIL verdict per config.
# Model: ref-writer cleanup -- failed-precommit removal ordering, successor-epoch reclamation and
# namespace-removal completeness, with liveness (`StalePrecommitEventuallyGone`) under weak fairness
# of `SuccessorCleanupStep` and `CleanupFailedProgress` (both are inside `Spec` itself).
#
# 2026-07-28: expectations upgraded from bare colours to NAME assertions during the v9 ref-chain TLA
# phase audit. The old classifier derived the expectation from the config NAME (`*_sab_*` => expect
# any red) and accepted `is violated|Error: |is not enabled` as that red -- so a parse error, a
# deadlock report or a violation of an entirely different invariant all passed as the sabotage's own
# counterexample. Each sabotage declares exactly ONE invariant precisely so it can be asserted by
# name; now it is.
#
# Sabotages run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_retirebeforeremoval    -> INV_RETIRE_AFTER_REMOVAL         S1: retire a Failed build before
#                                                                  its exact precommit removal is
#                                                                  durable -- the precommit is then
#                                                                  stuck forever, because
#                                                                  RemoveFailedPrecommit is gated on
#                                                                  buildState = "Failed"
#   sab_successorcurrentepoch  -> INV_NO_WRONGFUL_RECLAIM          S2: successor cleanup reclaims
#                                                                  epoch == current as well as
#                                                                  epoch < current -- a still-
#                                                                  promotable Active build's
#                                                                  precommit is taken from under it
#   sab_cancelbeforedurable    -> INV_NAMESPACE_REMOVAL_COMPLETE   S3: cancel a local build before the
#                                                                  namespace-removal transaction is
#                                                                  durable -- the cancelled build is
#                                                                  excluded from the removal's owner
#                                                                  enumeration, so the namespace
#                                                                  reaches "Removed" with a durable
#                                                                  never-cleaned owner behind it
#   safe                       -> GREEN                            all four safety invariants plus the
#                                                                  liveness property
#
# Exits nonzero if any expectation is unmet.
#
# This is a whole-suite runner: it owns the config list and takes no arguments. To run one
# config by hand:
#   java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers 1 \
#        -config CaRefWriterCleanupCore_<name>.cfg CaRefWriterCleanupCore.tla
#
# `-workers 1`, NOT `-workers auto`, and that is deliberate (see run_refcatalog.sh for the full
# rationale: parallel BFS makes the reported depth, the state counts and WHICH shortest
# counterexample TLC prints vary between identical runs). Override with TLC_WORKERS=auto if you only
# want a verdict and not the numbers.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefWriterCleanupCore

# name  expectation(green|violation)  expected-invariant(asserted, not just logged)
CONFIGS=(
  "sab_retirebeforeremoval    violation  INV_RETIRE_AFTER_REMOVAL"
  "sab_successorcurrentepoch  violation  INV_NO_WRONGFUL_RECLAIM"
  "sab_cancelbeforedurable    violation  INV_NAMESPACE_REMOVAL_COMPLETE"
  "safe                       green      -"
)

overall=0
printf '%-28s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-refwcleanup-${name}"
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
    result="temporal:StalePrecommitEventuallyGone"   # the only property any of these cfgs declares
  elif [[ $rc -eq 124 ]]; then
    result="incomplete"
  else
    result="error"
  fi

  verdict="FAIL"
  case "$expect" in
    green)      [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation)  [[ "$result" == "violation:${want}" ]] && verdict="PASS" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1

  printf '%-28s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
