#!/usr/bin/env bash
# Run every CaRefNsCleanupStaleLeaderCore config and print a one-line PASS/FAIL verdict per config.
# Model: v9's stale-leader straggler safety after the `_cleanup`-marker/round-recheck gate is deleted
# for the ref layer (spec 2026-07-27-cas-ref-chain-complete-cut-design.md, §2 INV-3 ref-layer-scoped
# incarnations + structural inertness, §3 Lifecycles). Results and traces:
# CaRefNsCleanupStaleLeaderCore_RESULTS.md.
#
# Sabotage runs FIRST: a green is only evidence once the property it rests on has been seen red.
#
#   sab_noincarnation -> NoLiveDataDeleted   recreation reuses the incarnation instead of minting
#                                            fresh: the SAME unconditional, unguarded stale-pass
#                                            delete then reaches the reborn life's own data -- the
#                                            model-level proof that incarnation freshness, not a
#                                            live guard, carries rebirth safety
#   safe              -> GREEN               honest rebirth: recreation always mints fresh, so the
#                                            straggler's delete -- scoped only to what it captured
#                                            before deposition, never re-checked -- structurally
#                                            cannot reach it
#
# Exits nonzero if any expectation is unmet.
#
# `-workers 1`, NOT `-workers auto` (see run_refcatalog.sh for the full rationale: parallel BFS visits
# states in a nondeterministic order, so the reported depth and which trace TLC prints are not
# reproducible run to run with `auto`). Override with TLC_WORKERS=auto if you only want a verdict and
# not the numbers.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefNsCleanupStaleLeaderCore

# name               expectation(green|violation)  expected-invariant(asserted, not just logged)
CONFIGS=(
  "sab_noincarnation  violation  NoLiveDataDeleted"
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
    result="violation:$(grep -oE '(Invariant|Property) [A-Za-z_]+ is violated' "$log" | head -1 | awk '{print $2}')"
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

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
