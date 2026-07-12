#!/usr/bin/env bash
# Run every CaRefTableSnapshotLogCore config and print a one-line PASS/FAIL verdict per config.
#   safe                         -> expect GREEN (no error)
#   sab_deletebeforesnapshot     -> expect VIOLATION (INV_RECOVERY)
#   sab_vanishiscorruption       -> expect VIOLATION (INV_NOFAIL)
#   sab_recreatebeforecompleted  -> expect VIOLATION (INV_RECREATE)
#   latepred                     -> expect VIOLATION (INV_RECOVERY; documented Phase-1 late-predecessor
#                                   limitation, spec §late-predecessor-put — "violation found" is PASS)
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
  "latepred                    violation  INV_RECOVERY"
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
