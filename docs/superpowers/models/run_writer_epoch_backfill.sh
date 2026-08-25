#!/usr/bin/env bash
# Mandatory numeric writer-epoch backfill.  Every expected red names the invariant it must break.
set -uo pipefail
cd "$(dirname "$0")"

JAR="${TLC_JAR:-../../../tmp/tla2tools-official.jar}"
source ./tlc_temporal_gate.sh
check_tlc_pin "$JAR" || exit 3

MODULE=CaWriterEpochBackfillCore
CONFIGS=(
  "sab_direct_skip         violation INV_NO_EPOCH_SKIP"
  "sab_frontier_after_terminal violation INV_NO_SAME_EPOCH_FRONTIER_AUTHORIZATION"
  "sab_snapshot_base_is_seal violation INV_OWNER_SET_BASE_IS_NOT_EPOCH_SEAL"
  "witness_authorizations  violation W_ALL_AUTHORIZATION_KINDS_REACHABLE"
  "safe                    green     -"
)

overall=0
printf '%-24s %-10s %-42s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
  read -r name expect want <<<"$row"
  cfg="${MODULE}_${name}.cfg"
  log="../../../tmp/tlc_${MODULE}_${name}.log"
  meta="../../../tmp/tlc-meta-writer-epoch-${name}"
  rm -rf "$meta"
  start=$SECONDS
  timeout 300 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "${MODULE}.tla" >"$log" 2>&1
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
    green)     [[ "$result" == "green" ]] && verdict="PASS" ;;
    violation) [[ "$result" == "violation:${want}" ]] && verdict="PASS" ;;
  esac
  [[ "$verdict" == "FAIL" ]] && overall=1
  printf '%-24s %-10s %-42s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then
  echo "ALL EXPECTATIONS MET"
else
  echo "SOME EXPECTATIONS UNMET"
fi
exit $overall
