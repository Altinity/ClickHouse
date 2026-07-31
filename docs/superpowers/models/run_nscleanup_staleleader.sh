#!/usr/bin/env bash
# Run every CaRefNsCleanupStaleLeaderCore config and print a one-line PASS/FAIL verdict per config.
# Model: v9's stale-leader straggler safety after the `_cleanup`-marker/round-recheck gate is deleted
# for the ref layer (spec 2026-07-27-cas-ref-chain-complete-cut-design.md, §2 INV-3 ref-layer-scoped
# incarnations + structural inertness, §3 the deposit-time-capture rule). Results and traces:
# CaRefNsCleanupStaleLeaderCore_RESULTS.md.
#
# Sabotages run FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_noincarnation -> NoLiveDataDeleted   recreation reuses the incarnation instead of minting
#                                            fresh: the SAME unconditional, unguarded stale-pass
#                                            delete then reaches the reborn life's own data
#   sab_rederive       -> NoLiveDataDeleted   the resumed pass resolves its target from the CURRENT
#                                            catalog entry instead of the incarnation captured at
#                                            deposition (spec §3's deposit-time-capture rule): once
#                                            reborn, "current" simply IS the live incarnation
#   safe               -> GREEN               honest rebirth AND honest deposit-time capture: the
#                                            straggler's delete -- scoped only to what it captured
#                                            before deposition, never re-checked or re-derived --
#                                            structurally cannot reach live data
#
# Both sabotages hit the same invariant by independent, isolated routes (see the cfg headers) --
# together they are the model-level proof that incarnation freshness AND deposit-time capture are
# each independently load-bearing, not that either alone would do.
#
# Exits nonzero if any expectation is unmet.
#
# `-workers 1`, NOT `-workers auto` (see run_refcatalog.sh for the full rationale: parallel BFS visits
# states in a nondeterministic order, so the reported depth and which trace TLC prints are not
# reproducible run to run with `auto`). Override with TLC_WORKERS=auto if you only want a verdict and
# not the numbers.
#
# COVERAGE=1 additionally re-runs `safe` under `-coverage 1`, which is where the RESULTS
# per-action invocation table comes from -- the machine-checked non-vacuity witness (M2). Off by
# default because it changes no verdict and only adds noise to the normal PASS/FAIL run.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefNsCleanupStaleLeaderCore

# name               expectation(green|violation)  expected-invariant(asserted, not just logged)
CONFIGS=(
  "sab_noincarnation  violation  NoLiveDataDeleted"
  "sab_rederive       violation  NoLiveDataDeleted"
  "safe               green      -"
)

overall=0
printf '%-20s %-10s %-28s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-nscleanup-${name}"
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

  printf '%-20s %-10s %-28s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

if [[ "${COVERAGE:-0}" == "1" ]]; then
  echo
  echo "COVERAGE=1: re-running the non-vacuity green with -coverage 1"
  log="../../../tmp/tlc_cov_${MODULE}_safe.log"
  meta="../../../tmp/tlc-meta-cov-nscleanup-safe"
  rm -rf "$meta"
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -coverage 1 \
    -config "${MODULE}_safe.cfg" "$MODULE.tla" >"$log" 2>&1
  echo "  coverage safe: $log"
fi

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
