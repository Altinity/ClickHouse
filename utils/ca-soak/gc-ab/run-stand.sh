#!/bin/bash
# Run a ca-soak scenario (default S11) against a chosen binary and collect a run directory for extract.py.
#
#   run-stand.sh run     <label> <binary> [scenario args...]   (default: --scenario S11 --seed 20260930 --duration 300s --scale dev)
#   run-stand.sh collect <label> <harness-run-dir>              (snapshot a stand that is still up)
#   GC_EXTRA_CONFIG=/abs/file.xml run-stand.sh run ...           (mount one more config.d file on both servers)
#   run-stand.sh down                                           (teardown of the ca-soak stack, down -v)
#
# Output: $GC_AB_OUT/runs/<label>/ (default GC_AB_OUT: the current directory).
# scenarios/RUN_HISTORY.md and BACKLOG.md are put back to their pre-run bytes when the runner only appended to them.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOAK=$(cd "$HERE/.." && pwd)
STAND=${GC_AB_OUT:-$PWD}
export GC_AB_OUT=$STAND
mode=${1:-}; shift || true

compose_env() {  # <binary>
    python3 $HERE/gen_override.py "$1" "${GC_EXTRA_CONFIG:-}"
    export COMPOSE_FILE=$SOAK/docker-compose.yml:$STAND/override-current.yml
}

q() { docker exec "$1" clickhouse-client -q "$2" 2>&1; }

collect() {  # <label> <harness-run-dir>
    local label=$1 hrun=$2 out=$STAND/runs/$1
    mkdir -p $out/snap $out/predown
    cp -r $hrun/raw $out/raw
    cp $hrun/report.md $hrun/run.log $out/ 2>/dev/null
    for n in ch1 ch2; do
        d=$SOAK/logs/predown/$n/$(basename $hrun | cut -d_ -f2)_end_of_batch
        mkdir -p $out/predown/$n
        cp $d/events.tsv $d/part_log.tsv $d/gc_log.tsv $out/predown/$n/ 2>/dev/null
    done
    : > $out/snap/list_under.tsv
    for c in ca-soak-ch1-1 ca-soak-ch2-1; do
        q $c "SYSTEM FLUSH LOGS" >/dev/null
        q $c "SELECT '$c', extract(message, 'prefix=([^,]+),') AS prefix, count(), sum(toUInt32OrZero(extract(message, 's3_list_requests=(\\\\d+)'))) FROM system.text_log WHERE message LIKE 'listUnder prefix=%' GROUP BY prefix ORDER BY 3 DESC FORMAT TSV" >> $out/snap/list_under.tsv
        q $c "SELECT * FROM system.mutations FORMAT TSVWithNames" > $out/snap/mutations_$c.tsv
        q $c "SELECT event_type, count() FROM system.blob_storage_log GROUP BY event_type FORMAT TSV" > $out/snap/blob_storage_log_$c.tsv
        q $c "SELECT toString(event_time), ProfileEvent_DiskS3HeadObject, ProfileEvent_DiskS3ListObjects, ProfileEvent_DiskS3GetObject, ProfileEvent_DiskS3DeleteObjects FROM system.metric_log ORDER BY event_time FORMAT TSV" > $out/snap/metric_log_$c.tsv
        q $c "SELECT event, value FROM system.events WHERE event LIKE 'CAS%' OR event LIKE 'DiskS3%' OR event LIKE 'S3%' FORMAT TSV" > $out/snap/events_$c.tsv
    done
    {
        echo "run: $label   harness dir: $(basename $hrun)"
        echo "version: $(q ca-soak-ch1-1 'SELECT version()')"
        echo "binary sha256 (host file mounted): $(sha256sum $(readlink -f $BINARY_PATH 2>/dev/null || echo /dev/null) | cut -d' ' -f1)"
    } > $out/binary.txt
}

restore_tracked() {
    for f in RUN_HISTORY.md BACKLOG.md; do
        pre=$STAND/pre-run/$f
        cur=$SOAK/scenarios/$f
        if python3 - "$pre" "$cur" <<'PY'
import sys
pre = open(sys.argv[1], 'rb').read(); cur = open(sys.argv[2], 'rb').read()
sys.exit(0 if cur.startswith(pre) and cur != pre else 1)
PY
        then cp $pre $cur; echo "restored $f (runner append removed)"; fi
    done
}

case $mode in
run)
    label=$1; BINARY_PATH=$2; shift 2
    args=("$@"); [ ${#args[@]} -eq 0 ] && args=(--scenario S11 --seed 20260930 --duration 300s --scale dev)
    if docker ps --format '{{.Names}}' | grep -q '^ca-soak-'; then
        echo "REFUSING: a ca-soak stack is already up; the runner's reset would tear it down"; exit 3
    fi
    mkdir -p $STAND/pre-run $STAND/runs
    cp $SOAK/scenarios/RUN_HISTORY.md $SOAK/scenarios/BACKLOG.md $STAND/pre-run/
    compose_env $BINARY_PATH
    cd $SOAK
    python3 -m scenarios.run "${args[@]}"; rc=$?
    hrun=$(ls -td $SOAK/scenarios/runs/*/ | head -1)
    collect $label ${hrun%/}
    restore_tracked
    echo "RUN_RC=$rc  COLLECTED=$STAND/runs/$label"
    echo "STACK LEFT UP: run '$0 down' when done"
    ;;
collect)
    BINARY_PATH=${3:-}; collect "$1" "$2" ;;
down)
    [ -n "${BINARY_PATH:-}" ] || BINARY_PATH=/dev/null
    compose_env $BINARY_PATH
    cd $SOAK && docker compose down -v --remove-orphans ;;
*) sed -n 2,8p $0 ;;
esac
