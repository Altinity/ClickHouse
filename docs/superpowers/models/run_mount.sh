#!/usr/bin/env bash
# Run every CaCasMountCore config and print a one-line PASS/FAIL verdict per config.
# Model: mount ownership + server-root identity -- sticky owner, monotone durable writer epoch,
# observation-based lease reclaim (rev.6), the fence-not-rescue epoch-wipe gate (2026-07-24), and
# the v9 recovery-generation layer (2026-07-28, spec
# 2026-07-27-cas-ref-chain-complete-cut-design.md §3 "Recovery ownership" + §9's r9-5).
# Results, state counts and traces: CaCasMountCore_RESULTS.md.
#
# 2026-07-28: this script used to take a single cfg basename as its argument and run just that one.
# It is now a whole-suite runner with asserted expectations, matching run_refcatalog.sh /
# run_nscleanup_staleleader.sh -- a per-cfg colour that nothing checks is a colour that rots. To run
# one config by hand:
#   java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers 1 \
#        -config CaCasMountCore_<name>.cfg CaCasMountCore.tla
#
# Sabotages run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_epochreset        -> WriterEpochMonotoneUnique   the durable epoch counter is zeroed when the
#                                                        mount is cleared: monotonicity gone
#   sab_foreigntakeover   -> ForeignUuidNeverAutoTakesOver  a foreign server uuid takes over an
#                                                        expired mount
#   sab_adoptwedge        -> NoPermanentWedge            the OLD adopt: a token mismatch at
#                                                        AdoptWrite fails closed PERMANENTLY (the
#                                                        pre-fix LOGICAL_ERROR out of Store::open,
#                                                        exit 49 -- the S13 wedge)
#   sab_fenceresurrect    -> FenceCostsEpoch             the OLD adopt read that skips the gc_fenced
#                                                        check: same-epoch resurrection of a fenced
#                                                        incarnation
#   sab_wallclockreclaim  -> GlobalSupersededWriterMakesNoMutation
#                                                        a reclaimer trusts the holder's own stamp
#                                                        instead of waiting out TTL + Drift on its
#                                                        OWN clock (the rev.6 bug)
#   sab_epochwipelive     -> SupersededWriterMakesNoMutation
#                                                        the pre-fix allocateWriterEpoch: re-mint
#                                                        over a lost epoch object while a mount is
#                                                        still LIVE
#   sab_decomblindbypass  -> FenceCostsEpoch             the rejected decommission blind bypass:
#                                                        mint epoch 1 regardless of mount liveness
#                                                        (round-3 finding-1)
#
#   --- 2026-07-28 v9 recovery-generation layer ---
#   sab_staleinstall      -> GlobalSupersededWriterMakesNoMutation
#                                                        §9 r9-5: `Install` drops the post-I/O
#                                                        generation recheck, so an old recovery's
#                                                        result publishes after a self-remount
#   sab_wedgeretryoldgen  -> GlobalSupersededWriterMakesNoMutation
#                                                        INV-1: a wedged lane's conditional create
#                                                        fires under the NEW generation unchecked,
#                                                        injecting a dead incarnation's bytes into
#                                                        the successor's live stream
#   sab_slotnocompare     -> AckedOpsAreDurable          INV-2's slot-occupy resolution: the byte
#                                                        comparison is skipped, so the lane acks its
#                                                        own operation while someone ELSE's bytes
#                                                        are at the key (acked-then-lost)
#
# The first two v9 sabotages target the SAME pre-existing invariant, deliberately: an old-generation
# publication IS a mutation by a superseded incarnation, which is what that invariant already says,
# and inventing a parallel property per sabotage is how a suite ends up with six near-duplicates.
# The routes are independent -- a returning recovery RESULT vs a dead lane's own CREATE -- and each
# cfg header names its own. Only the third needed a new invariant, because no existing one records
# what a caller was TOLD.
#
#   stage1                -> GREEN   the legacy positive gate (recovery-generation layer inert)
#   v9_recoverygen        -> GREEN   THE v9 gate: the layer on, every sabotage off
#
# Witnesses are negated reachability -- a VIOLATION is the evidence. They exist because BFS reports
# the SHORTEST counterexample, so a branch no red travels would silently rot:
#
#   witness_reclaim                     -> W_SameUuidReclaimsExpired
#   witness_remountafterfence           -> W_RemountAfterFence
#   witness_observedreclaim             -> W_ObservedReclaim
#   witness_recoveryafterobservedreclaim-> W_RecoveryAfterObservedReclaim  (the slow one, ~2 min:
#                                          honest-path safety is not "safe by permanent dead-end")
#   witness_genrefused                  -> W_GenerationRefused   the honest post-I/O recheck really
#                                          refuses a returning old-generation result
#   witness_sealrejected                -> W_SealRejectedRetry   an old generation's lane really
#                                          meets a SUCCESSOR's EpochSeal and is conclusively
#                                          rejected -- plan task 1's concurrent-recoverer hand-off
#
# NOT in the default suite: CaCasMountCore_rev6_observe.cfg. It does not complete in an interactive
# budget and that is PRE-EXISTING, confirmed against the pre-2026-07-24 committed model (Drift = 2
# triples the nondeterministic `\E d \in 0..Drift` branch at every ClaimMount/Renew/AdoptWrite,
# compounded across MaxClock = 10); see CaCasMountCore_RESULTS.md's "not completed" section. Run it
# with SLOW=1, which appends it with a 3-hour timeout and reports `incomplete` without failing the
# suite -- a config with no verdict must not be able to masquerade as a green one.
#
# Exits nonzero if any expectation is unmet.
#
# `-workers 1`, NOT `-workers auto`, and that is deliberate (see run_refcatalog.sh for the full
# rationale: parallel BFS visits states in a nondeterministic order, so the reported depth, the
# abort runs' state counts and WHICH shortest counterexample TLC prints all vary between identical
# runs, while every trace narrated in the cfg headers and in RESULTS is a specific action sequence).
# Override with TLC_WORKERS=auto if you only want a verdict and not the numbers.
#
# ADMISSIONS=<n> re-runs the whole suite with `MaxAdmissions` overridden to <n> in every config that
# has the v9 layer on, to show that bound is not doing the work. `MaxAdmissions` is a state-space
# bound in the same declared spirit as `epochWiped`'s one-shot `WipeEpoch` guard (see its comment in
# the module): without it the v9 green gate has no verdict at all. Overridden cfgs are written to the
# scratch tree, never beside the module, so an interrupted ADMISSIONS run leaves the tracked
# directory clean. Note that the legacy configs are unaffected by construction -- their
# `RecoveryGenOn = FALSE` disables both admission actions regardless of the bound.
#
# COVERAGE=1 additionally re-runs `v9_recoverygen` under `-coverage 1`, which is where the RESULTS
# per-action invocation table comes from -- the machine-checked non-vacuity witness that each of the
# nine new actions actually fires, and in particular that BOTH generation-recheck refusal sites
# (RecoveryRefused and WedgeAbandonStale) do, since one shared flag drives W_GenerationRefused. Off
# by default because it changes no verdict.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaCasMountCore

# name  expectation(green|violation|incomplete)  expected-invariant(asserted, not just logged)
CONFIGS=(
  "sab_epochreset                        violation  WriterEpochMonotoneUnique"
  "sab_foreigntakeover                   violation  ForeignUuidNeverAutoTakesOver"
  "sab_adoptwedge                        violation  NoPermanentWedge"
  "sab_fenceresurrect                    violation  FenceCostsEpoch"
  "sab_wallclockreclaim                  violation  GlobalSupersededWriterMakesNoMutation"
  "sab_epochwipelive                     violation  SupersededWriterMakesNoMutation"
  "sab_decomblindbypass                  violation  FenceCostsEpoch"
  "sab_staleinstall                      violation  GlobalSupersededWriterMakesNoMutation"
  "sab_wedgeretryoldgen                  violation  GlobalSupersededWriterMakesNoMutation"
  "sab_slotnocompare                     violation  AckedOpsAreDurable"
  "stage1                                green      -"
  "v9_recoverygen                        green      -"
  "witness_reclaim                       violation  W_SameUuidReclaimsExpired"
  "witness_remountafterfence             violation  W_RemountAfterFence"
  "witness_observedreclaim               violation  W_ObservedReclaim"
  "witness_recoveryafterobservedreclaim  violation  W_RecoveryAfterObservedReclaim"
  "witness_genrefused                    violation  W_GenerationRefused"
  "witness_sealrejected                  violation  W_SealRejectedRetry"
)
[[ "${SLOW:-0}" == "1" ]] && CONFIGS+=("rev6_observe  incomplete  -")

overall=0
printf '%-38s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-mount-${name}"
  if [[ -n "${ADMISSIONS:-}" ]]; then
    cfg="../../../tmp/${MODULE}_adm${ADMISSIONS}_${name}.cfg"
    sed -E "s/MaxAdmissions = [1-9][0-9]*/MaxAdmissions = ${ADMISSIONS}/" "${MODULE}_${name}.cfg" > "$cfg"
    log="../../../tmp/tlc_${MODULE}_adm${ADMISSIONS}_${name}.log"
    meta="../../../tmp/tlc-meta-mount-adm${ADMISSIONS}-${name}"
  fi
  # rev6_observe is the only config allowed to run out of time; every other one must finish, and a
  # timeout there has to read as a FAILURE rather than as a quiet "error".
  tmo=3600; [[ "$expect" == "incomplete" ]] && tmo=10800
  rm -rf "$meta"
  start=$SECONDS
  timeout "$tmo" /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
  rc=$?
  elapsed=$((SECONDS - start))

  if grep -q "No error has been found" "$log"; then
    result="green"
  elif grep -q "is violated" "$log"; then
    result="violation:$(grep -oE '(Invariant|Property) [A-Za-z_]+ is violated' "$log" | head -1 | awk '{print $2}')"
  elif [[ $rc -eq 124 ]]; then
    result="incomplete"
  else
    result="error"
  fi

  verdict="FAIL"
  case "$expect" in
    green)      [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation)  [[ "$result" == "violation:${want}" ]] && verdict="PASS" ;;
    incomplete) [[ "$result" == "incomplete" ]] && verdict="KNOWN" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1

  printf '%-38s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

if [[ "${COVERAGE:-0}" == "1" ]]; then
  echo
  echo "COVERAGE=1: re-running the v9 green gate with -coverage 1"
  log="../../../tmp/tlc_cov_${MODULE}_v9_recoverygen.log"
  meta="../../../tmp/tlc-meta-cov-mount-v9_recoverygen"
  rm -rf "$meta"
  timeout 3600 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -coverage 1 \
    -config "${MODULE}_v9_recoverygen.cfg" "$MODULE.tla" >"$log" 2>&1
  echo "  coverage v9_recoverygen: $log"
fi

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
