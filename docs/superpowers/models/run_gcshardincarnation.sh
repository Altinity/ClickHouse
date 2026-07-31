#!/usr/bin/env bash
# Run the `CaGcShardIncarnationCore` design and sabotage configurations.
# Exits nonzero if an asserted expectation is unmet.
set -uo pipefail
cd "$(dirname "$0")"

JAR=../../../tmp/tla2tools.jar
MODULE=CaGcShardIncarnationCore
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }

# Sabotages run first; each must break the invariant that its removed coordinate protects.
# name                     expectation  expected-invariant
CONFIGS=(
  "sab_deletebeforefold    violation    INV_NO_ORPHAN_EDGE"
  "sab_incarnationreuse    violation    INV_NO_DANGLING"
  "sab_newbornnofloor      violation    INV_NO_DANGLING"
  "sab_pathkeyedcursor     violation    INV_NO_DANGLING"
  "design                  green        -"
)

overall=0
printf '%-28s %-11s %-40s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
    read -r name expect want <<<"$row"
    cfg="${MODULE}_${name}.cfg"
    log="../../../tmp/tlc_${MODULE}_${name}.log"
    meta="../../../tmp/tlc-meta-${MODULE}-${name}"
    rm -rf "$meta"
    start=$SECONDS
    timeout 3600 /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
    rc=$?
    elapsed=$((SECONDS - start))

    if grep -q "No error has been found" "$log"; then
        result=green
    elif grep -q "is violated" "$log"; then
        result="violation:$(grep -oE '(Invariant|Property) [A-Za-z0-9_]+ is violated' "$log" | head -1 | awk '{print $2}')"
    else
        result=error
    fi

    verdict=FAIL
    case "$expect" in
        green) [[ "$result" == green ]] && verdict=PASS ;;
        violation) [[ "$result" == "violation:${want}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-28s %-11s %-40s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo
if [[ $overall -eq 0 ]]; then echo "ALL EXPECTATIONS MET"; else echo "SOME EXPECTATIONS UNMET"; fi
exit "$overall"
