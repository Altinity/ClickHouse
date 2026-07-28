#!/usr/bin/env bash
# Run every CaRefTableSnapshotLogCore config and print a one-line PASS/FAIL verdict per config.
# Model: v9 contiguous ref stream (spec 2026-07-27-cas-ref-chain-complete-cut-design.md,
# §2 INV-1/INV-2/INV-4, §4 Recovery). Results and traces: CaRefTableSnapshotLogCore_RESULTS.md.
#
#   v9_safe                  -> GREEN     the honest protocol, no straggler
#   v9_flip_latepred         -> GREEN     THE FLIP: rev.4's expected-fail is now a proof
#   *_deep                   -> GREEN     the two greens again at MaxSeq = 5 (~41 s each)
#   sab_reuseafterambiguous  -> INV_NO_PHANTOM   every-attempt rule (INV-1)
#   sab_gaponfail            -> INV_DENSE        contiguity (INV-1); rev.4's "safe id gap"
#   sab_noseal               -> INV_RECOVERY     slot occupancy (INV-2); the flip's control
#   sab_blindput             -> INV_NO_GHOST     conditional create is the fence (INV-2)
#   sab_scanistruth          -> INV_RECOVERY     LIST is a zero-trust hint (§4/§5)
#   sab_cleanupaboveckpt     -> INV_RECOVERY     log deletion gated at _ckpt.base (INV-4)
#   sab_staleckptcorruption  -> INV_NOFAIL       snapshot deletion STRICTLY below base (INV-4)
#   witness_hintlie          -> W_NO_HINT_HOLE   reachability witness: a complete-LOOKING
#                                                enumeration that omits a PRESENT log
#                                                (the observed 0x1430c/0x1430d shape).
#                                                A violation is the EVIDENCE, so it is a PASS.
# Exits nonzero if any expectation is unmet.
set -uo pipefail
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
MODULE=CaRefTableSnapshotLogCore

# name  expectation(green|violation)  expected-invariant(for the log line)
CONFIGS=(
  "v9_safe                 green      -"
  "v9_flip_latepred        green      -"
  "v9_safe_deep            green      -"
  "v9_flip_latepred_deep   green      -"
  "sab_reuseafterambiguous violation  INV_NO_PHANTOM"
  "sab_gaponfail           violation  INV_DENSE"
  "sab_noseal              violation  INV_RECOVERY"
  "sab_blindput            violation  INV_NO_GHOST"
  "sab_scanistruth         violation  INV_RECOVERY"
  "sab_cleanupaboveckpt    violation  INV_RECOVERY"
  "sab_staleckptcorruption violation  INV_NOFAIL"
  "witness_hintlie         violation  W_NO_HINT_HOLE"
)

overall=0
printf '%-26s %-10s %-30s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-refsnap-${name}"
  rm -rf "$meta"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers auto -config "$cfg" "$MODULE.tla" >"$log" 2>&1
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

  printf '%-26s %-10s %-30s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit $overall
