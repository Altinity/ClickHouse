#!/usr/bin/env bash
# Run the relink/lane composition sabotage, honest, and witness configurations.
set -uo pipefail
cd "$(dirname "$0")"

JAR=../../../tmp/tla2tools.jar
MODULE=CaRelinkLaneComposition
RUN_ID="${RUN_ID:-$(date +%Y%m%dT%H%M%S)-$$}"
LOG_ROOT="../../../build/tlc-runs/relinklane/${RUN_ID}"
META_ROOT="../../../build/tlc-meta/relinklane/${RUN_ID}"

[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
mkdir -p "$LOG_ROOT" "$META_ROOT"

CONFIGS=(
  "sab_confirmblocked violation ConfirmationRequiresReady"
  "sab_skipidentity   violation PromotionUsesConfirmedIdentity"
  "sab_deleteunowned  violation DeletedSourceIsOwned"
  "safe               green     -"
  "witness_confirmation   violation W_Confirmation"
  "witness_blockedrefusal violation W_BlockedRefusal"
  "witness_recovery       violation W_Recovery"
  "witness_promotion      violation W_Promotion"
  "witness_delete         violation W_Delete"
)

overall=0
printf '%-28s %-10s %-34s %-8s %s\n' "CONFIG" "EXPECT" "RESULT" "SECONDS" "VERDICT"
for row in "${CONFIGS[@]}"; do
    read -r name expect wanted <<<"$row"
    cfg="${MODULE}_${name}.cfg"
    log="${LOG_ROOT}/tlc_${cfg%.cfg}.log"
    meta="${META_ROOT}/${name}"
    start=$SECONDS
    /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >"$log" 2>&1
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
        violation) [[ "$result" == "violation:${wanted}" ]] && verdict=PASS ;;
    esac
    [[ "$verdict" == FAIL ]] && overall=1
    printf '%-28s %-10s %-34s %-8s %s\n' "$name" "$expect" "$result" "$elapsed" "$verdict"
done

echo "logs=${LOG_ROOT}"
if [[ $overall -eq 0 ]]; then
    echo "ALL EXPECTATIONS MET"
else
    echo "SOME EXPECTATIONS UNMET"
fi
exit "$overall"
