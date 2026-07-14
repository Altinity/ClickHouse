#!/usr/bin/env bash
# Run every CaRefTableSnapshotLogCore config and print a one-line PASS/FAIL verdict per config.
#   safe                         -> expect GREEN (no error)
#   sab_deletebeforesnapshot     -> expect VIOLATION (INV_RECOVERY)
#   sab_vanishiscorruption       -> expect VIOLATION (INV_NOFAIL)
#   sab_recreatebeforecompleted  -> expect VIOLATION (INV_RECREATE)
#   sab_remountkeepsoldepoch     -> expect VIOLATION (INV_RECOVERY; C1 self-remount stamping fresh appends
#                                   at the old below-durable epoch — the epoch-route fix makes it GREEN)
#   latepred                     -> expect VIOLATION (INV_RECOVERY; documented Phase-1 late-predecessor
#                                   limitation, spec §late-predecessor-put — "violation found" is PASS)
#   rev6_safe                    -> expect GREEN (coverage-at-birth seal, no late delivery possible)
#   rev6_latedelivery            -> expect GREEN (NoDivergentFold; amended 2026-07-14 -- the
#                                   in-flight transient is inexpressible under this model's
#                                   reader-freeze abstraction once WriterPublishSnapshot's
#                                   CoveredFold excludes droppedEver; see the Task 1 amendment note
#                                   in docs/superpowers/plans/2026-07-13-cas-ref-lease-exclusivity-rev6.md)
#   rev6_freshreader             -> expect GREEN (INV_FRESH_READER + INV_SNAP_DETERMINISTIC; the
#                                   regression guard for the CoveredFold fix -- was RED on
#                                   INV_SNAP_DETERMINISTIC before that fix)
# Exits nonzero if any expectation is unmet.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefTableSnapshotLogCore

# name  expectation(green|violation)  expected-invariant(for the log line)
CONFIGS=(
  "safe                        green      -"
  "sab_deletebeforesnapshot    violation  INV_RECOVERY"
  "sab_vanishiscorruption      violation  INV_NOFAIL"
  "sab_recreatebeforecompleted violation  INV_RECREATE"
  "sab_remountkeepsoldepoch    violation  INV_RECOVERY"
  "latepred                    violation  INV_RECOVERY"
  "rev6_safe                   green      -"
  "rev6_latedelivery           green      -"
  "rev6_freshreader            green      -"
)

overall=0
printf '%-30s %-10s %-14s %s\n' "CONFIG" "EXPECT" "RESULT" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-refsnap-${name}"
  rm -rf "$meta"
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers auto -config "$cfg" "$MODULE.tla" >"$log" 2>&1

  if grep -q "No error has been found" "$log"; then
    result="green"
  elif grep -q "is violated" "$log"; then
    result="violation:$(grep -oE 'Invariant [A-Za-z_]+ is violated' "$log" | head -1 | awk '{print $2}')"
  else
    result="error"
  fi

  verdict="FAIL"
  case "$expect" in
    green)     [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation) [[ "$result" == violation:* ]] && verdict="PASS" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1

  printf '%-30s %-10s %-14s %s\n' "$name" "$expect" "$result" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
