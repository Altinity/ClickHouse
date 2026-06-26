#!/usr/bin/env bash
# Resource + disk-evidence monitor for a running soak. Samples every INTERVAL until the soak driver
# (`python3 -m soak.run`) exits. Captures WHERE disk goes: per-pool-dir `du` inside the rustfs
# container, host `df`, docker stats (CPU/mem), and `docker system df` (overlay/volume/log growth).
set -uo pipefail
cd "$(dirname "$0")/.."
RES="${RES:-logs/soak_resmon.log}"
INTERVAL="${INTERVAL:-300}"
RUSTFS="${RUSTFS:-ca-soak-rustfs1-1}"
POOL="${POOL:-/data/test/soak_pool}"
: > "$RES"
# Gate on the soak LOG's terminal marker (robust — does NOT race soak.run startup like pgrep did).
SOAKLOG="${SOAKLOG:?set SOAKLOG to the run_24h.sh stdout log}"
while ! grep -qE "PHASE3 OK|PHASE3 FAILED|TRANSPORT FAILURE|Traceback|CheckpointFailure|WORKLOAD FAILURE" "$SOAKLOG" 2>/dev/null; do
  {
    echo "===== $(date -u +%H:%M:%SZ) ====="
    df -h / | awk 'NR==2{print "host_/ avail="$4" use="$5}'
    docker stats --no-stream --format "stat {{.Name}} cpu={{.CPUPerc}} mem={{.MemUsage}}" 2>/dev/null | grep ca-soak || true
    echo "pool_du:"
    docker exec "$RUSTFS" sh -c "du -sh $POOL $POOL/blobs $POOL/trees $POOL/roots $POOL/gc 2>/dev/null" 2>/dev/null | sed 's/^/  /' || echo "  (du failed)"
    docker system df --format "dockerdf {{.Type}} {{.Size}} reclaimable={{.Reclaimable}}" 2>/dev/null | grep -iE "Images|Containers|Local Volumes|Build" || true
  } >> "$RES" 2>&1
  sleep "$INTERVAL"
done
echo "===== monitor stopped (soak.run gone) $(date -u +%H:%M:%SZ) =====" >> "$RES"
