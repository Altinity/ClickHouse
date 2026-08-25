#!/usr/bin/env bash
# Run every CaRefCatalogCore config and print a one-line PASS/FAIL verdict per config.
# Model: the v9 namespace catalog (spec 2026-07-27-cas-ref-chain-complete-cut-design.md,
# §2 INV-3 catalog + opaque life identities + the churn bound, and the `_ckpt` creation readiness
# required by §2 INV-4, §3 Lifecycles). Results and traces: CaRefCatalogCore_RESULTS.md.
#
# Sabotages run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_janitoreatsnewborn     -> INV_NEWBORN_SAFE     the janitor must stay incarnation-scoped; the
#                                                      damage is the `Live` entry its victim's
#                                                      creator publishes afterwards
#   sab_zombiegolive           -> INV_NEWBORN_SAFE     the same invariant from the opposite end: a
#                                                      fenced-out / token-stale creator's install
#                                                      must be refused by BOTH credentials (§3 fence
#                                                      generation, INV-3 catalog token-CAS)
#   sab_reconcilelivecreator   -> INV_RECONCILE_SAFE   reconcile only after the creator's fence is
#                                                      terminal (spec §3)
#   sab_reconcilestaletoken    -> INV_RECONCILE_SAFE   reconcile only by TOKEN-EXACT CAS: on a stale
#                                                      sample the victim is whatever the catalog
#                                                      holds now, up to a `Live` successor
#   sab_deletewithoutevidence  -> INV_REMOVAL_DELETE_PROVED  adopted matching evidence required
#   sab_deletewithforeignevidence -> INV_REMOVAL_DELETE_PROVED evidence must name this life
#   sab_deleteunderhold        -> INV_REMOVAL_DELETE_PROVED  a held life is not drainable
#   sab_deletewithoutexactobservation -> INV_REMOVAL_DELETE_PROVED exact catalog row required
#   sab_sameincarnationrebirth -> INV_NO_ALIAS         THE inertness proof: reuse the incarnation and
#                                                      removal's missing physical-empty proof turns
#                                                      into a new life reading a dead one's bytes
#   sab_floorretainsdeadname   -> INV_BOUNDED_CATALOG  §10's rejected `seq_floor`, executable: dead
#                                                      names' records never retire
#   finding_briefreconcileinv  -> INV_RECONCILE_SAFE_BRIEF
#                                       FINDING, violation = evidence: the task brief's proposed
#                                       reconciliation invariant is red on a legitimate transient
#                                       state (fence terminal before the `_ckpt` create) in the
#                                       FULLY HONEST model, so it cannot be the safety property
#   safe                       -> GREEN  THE GATE: honest lifecycles, every adversary on
#   churn                      -> GREEN  the user's create/drop-per-second scenario: three full
#                                        create -> drop -> recreate cycles stay O(C+L+R)
#
# The three witnesses are all negated — a VIOLATION is the evidence — and all exist because BFS
# reports the SHORTEST counterexample, so a route the near one does not travel would silently rot:
#
#   witness_churn3             -> WITNESS_CHURN         the three cycles actually COMPLETE, and
#                                                       complete with debris outstanding
#   witness_aliasremnant       -> WITNESS_ALIAS_REMNANT rebirth aliases onto a COMPLETED removal's
#                                                       remnant, not only onto a reconciled
#                                                       creator's `_ckpt` (the shorter route)
#   witness_orphaneaten        -> WITNESS_ORPHAN_EATEN  the janitor actually DELETES a running life's
#                                                       bytes, the severe arm of INV_RECONCILE_SAFE
#                                                       (its own red stops at `OrphanWrite`)
# Exits nonzero if any expectation is unmet.
#
# `-workers 1`, NOT `-workers auto`, and that is deliberate. Parallel BFS visits states in a
# nondeterministic order, so with `auto` the reported depth, the abort runs' state counts and — the
# part that matters — WHICH shortest counterexample TLC prints all vary between identical runs (three
# consecutive `auto` runs of `_safe` reported depth 19, 19, 20). Every trace narrated in the cfg
# headers and in RESULTS is a specific action sequence, so the run that produces them has to be
# reproducible. The whole suite is seconds either way. Override with TLC_WORKERS=auto if you only
# want a verdict and not the numbers.
#
# BOUND=<n> re-runs the whole suite with MaxInc overridden to <n>, to show the bound is not doing the
# work. Every expectation above holds unchanged at BOUND=5, so MaxInc = 3 is a convenience, not a
# load-bearing constant. Note that `witness_churn3` scales with the bound by construction
# (`lives = MaxInc`), and that at BOUND=5 `safe` and `churn` become the same run.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefCatalogCore

# name  expectation(green|violation)  expected-invariant(asserted, not just logged)
CONFIGS=(
  "sab_janitoreatsnewborn     violation  INV_NEWBORN_SAFE"
  "sab_zombiegolive           violation  INV_NEWBORN_SAFE"
  "sab_reconcilelivecreator   violation  INV_RECONCILE_SAFE"
  "sab_reconcilestaletoken    violation  INV_RECONCILE_SAFE"
  "sab_deletewithoutevidence  violation  INV_REMOVAL_DELETE_PROVED"
  "sab_deletewithforeignevidence violation INV_REMOVAL_DELETE_PROVED"
  "sab_deleteunderhold        violation  INV_REMOVAL_DELETE_PROVED"
  "sab_deletewithoutexactobservation violation INV_REMOVAL_DELETE_PROVED"
  "sab_sameincarnationrebirth violation  INV_NO_ALIAS"
  "sab_floorretainsdeadname   violation  INV_BOUNDED_CATALOG"
  "finding_briefreconcileinv  violation  INV_RECONCILE_SAFE_BRIEF"
  "safe                       green      -"
  "churn                      green      -"
  "witness_churn3             violation  WITNESS_CHURN"
  "witness_aliasremnant       violation  WITNESS_ALIAS_REMNANT"
  "witness_orphaneaten        violation  WITNESS_ORPHAN_EATEN"
)

overall=0
printf '%-28s %-10s %-34s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-refcatalog-${name}"
  if [[ -n "${BOUND:-}" ]]; then
    # Written to the scratch tree, never beside the module: `-config` takes a path, so a BOUND run
    # leaves nothing behind in the tracked directory even if it is interrupted.
    cfg="../../../tmp/${MODULE}_bound${BOUND}_${name}.cfg"
    sed -E "s/MaxInc = [0-9]+/MaxInc = ${BOUND}/" "${MODULE}_${name}.cfg" > "$cfg"
    log="../../../tmp/tlc_${MODULE}_bound${BOUND}_${name}.log"
    meta="../../../tmp/tlc-meta-refcatalog-bound${BOUND}-${name}"
  fi
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

  verdict="FAIL"
  case "$expect" in
    green)     [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation) [[ "$result" == "violation:${want}" ]] && verdict="PASS" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1

  printf '%-28s %-10s %-34s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
