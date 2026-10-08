#!/usr/bin/env bash
# Run every CaErasureProof config and print a one-line PASS/FAIL verdict per config.
# Model: the rev.7 `Vanished(erased)` erasure-proof soundness (spec 2026-07-22
# "throw-when-uncertain" SS2 [C2][C3][D1]). Crown property `TruthEmpty`: when the observer promotes a
# pool to "verified: pool prefix empty", the prefix IS empty and STAYS empty.
#
# 2026-07-28: rewritten during the v9 ref-chain TLA phase audit. It used to glob every cfg and set
# `overall=1` on any nonzero TLC exit -- so the sabotage, the two FINDING configs and the witness,
# whose whole purpose is to make TLC exit nonzero, made the suite FAIL every single time it ran. A
# runner that always fails is a runner nobody runs. Expectations are now asserted BY NAME, matching
# run_mount.sh / run_refcatalog.sh.
#
# Reds run FIRST: a green is only evidence once the property it rests on has been seen red. TWO of
# these reds are FINDINGS, not sabotages -- they are the as-built GC, and they are why the pool
# lifecycle v1 EXCISED natural erasure promotion (see CaDiskLifecycle.tla's header):
#
#   sab_nograce        -> TruthEmpty   grace removed: a zombie (guard released, request still in
#                                      flight) lands after the second sample -- [D1] grace IS
#                                      load-bearing
#   gc_promptliteral   -> TruthEmpty   FINDING: GC exactly as the Task-8 prose argues it (hb only
#                                      inside rounds, no new round after terminal intent) still
#                                      breaks the proof -- a fresh scheduler's round CREATES
#                                      `gc/state` between the observer's final LIST and its
#                                      round_in_flight read, and COMPLETES before that read
#   gc_asbuilt         -> TruthEmpty   FINDING: the real out-of-round heartbeat pulses and no
#                                      scheduler exit on the terminal transition -- more paths, incl.
#                                      post-promotion writes recreating control keys under a pool
#                                      already promoted
#   nogc_grace         -> GREEN        writers only, grace ON: the writer-side proof machinery
#                                      (op-gate Live admission + guard counter + LIST-reset + grace)
#                                      holds
#   fix_gclivegate     -> GREEN        the candidate fix: scheduler rounds AND heartbeat pulses
#                                      refuse unless the pool lifecycle is Live
#
# Witnesses are negated reachability -- a VIOLATION is the evidence:
#
#   witness_promote    -> NeverPromoted   promotion to VanishedErased is reachable at all, so the
#                                         greens above are not green by permanent dead-end
#
# Exits nonzero if any expectation is unmet.
#
# This is a whole-suite runner: it owns the config list and takes no arguments. To run one
# config by hand:
#   java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers 1 \
#        -config CaErasureProof_<name>.cfg CaErasureProof.tla
#
# `-workers 1`, NOT `-workers auto`, and that is deliberate (see run_refcatalog.sh for the full
# rationale: parallel BFS makes the reported depth, the state counts and WHICH shortest
# counterexample TLC prints vary between identical runs, while the traces narrated in the module
# header are specific action sequences). Override with TLC_WORKERS=auto if you only want a verdict
# and not the numbers.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaErasureProof

# name  expectation(green|violation)  expected-invariant(asserted, not just logged)
CONFIGS=(
  "sab_nograce       violation  TruthEmpty"
  "gc_promptliteral  violation  TruthEmpty"
  "gc_asbuilt        violation  TruthEmpty"
  "nogc_grace        green      -"
  "fix_gclivegate    green      -"
  "witness_promote   violation  NeverPromoted"
)

overall=0
printf '%-20s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-erasure-${name}"
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

  printf '%-20s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
