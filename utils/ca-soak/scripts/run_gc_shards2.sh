#!/usr/bin/env bash
# CA soak Phase-4 gc_shards=2 chaos soak.
#
# Uses docker-compose-gc_shards2.yml (mounts storage_conf_gc_shards2.xml on both replicas so
# gc_shards=2 is active from pool creation). Runs a scoped ~35-45 minute Phase-3 soak so blobs
# scatter across 2 target shards, two replicas reduce disjoint shards, and the single coordinator
# owns the global fence+seal.
#
# Default duration 40m; override via DURATION env var.
# Default seed 20260627; override via SEED env var.
set -uo pipefail
cd "$(dirname "$0")/.."

SEED="${SEED:-20260627}"
DURATION="${DURATION:-40m}"
WORKERS="${WORKERS:-6}"
METRICS="${METRICS:-soak_gc_shards2.db}"
MAX_POOL_GB="${MAX_POOL_GB:-40}"
COMPOSE_FILE="docker-compose-gc_shards2.yml"

LOGDIR="$(pwd)/logs"
mkdir -p "$LOGDIR"
RUN_TS="$(date +%Y%m%dT%H%M%S)"
DRIVER_LOG="$(pwd)/tmp/gc_shards2_driver_${RUN_TS}.log"
COMPOSE_LOG="$LOGDIR/gc_shards2_${RUN_TS}_server.log"

# B165: per-node ClickHouse log dirs bind-mounted into the containers so the server's own logs
# survive `docker compose down -v`. The server runs as uid 101 inside the container, so the host
# dirs must be writable by it. Start each run from a clean dir.
rm -rf "$LOGDIR/ch1" "$LOGDIR/ch2"
mkdir -p "$LOGDIR/ch1" "$LOGDIR/ch2"
chmod 777 "$LOGDIR/ch1" "$LOGDIR/ch2"

echo "=== CA soak Phase-4 gc_shards=2 run: seed=$SEED duration=$DURATION compose=$COMPOSE_FILE ===" | tee "$DRIVER_LOG"
echo "Metrics DB: $METRICS  Driver log: $DRIVER_LOG" | tee -a "$DRIVER_LOG"

# Teardown any prior run (clean data — mandatory for a fresh gc_shards=2 pool).
docker compose -f "$COMPOSE_FILE" down -v >>"$DRIVER_LOG" 2>&1 || true

# Bring up the gc_shards=2 cluster.
docker compose -f "$COMPOSE_FILE" up -d >>"$DRIVER_LOG" 2>&1

SOAK_OK=0
trap '
  docker compose -f "$COMPOSE_FILE" logs --no-color > "$COMPOSE_LOG" 2>&1 || true
  docker compose -f "$COMPOSE_FILE" ps -a >> "$COMPOSE_LOG" 2>&1 || true
  for c in $(docker compose -f "$COMPOSE_FILE" ps -aq 2>/dev/null); do
    docker inspect --format "{{.Name}} State={{.State.Status}} OOMKilled={{.State.OOMKilled}} ExitCode={{.State.ExitCode}}" "$c" >> "$COMPOSE_LOG" 2>&1 || true
  done
  echo "preserved docker logs+state -> $COMPOSE_LOG" | tee -a "$DRIVER_LOG"
  if [ "$SOAK_OK" = 1 ]; then
    echo "PHASE3/gc_shards=2 OK — tearing down (down -v)" | tee -a "$DRIVER_LOG"
    docker compose -f "$COMPOSE_FILE" down -v
  else
    echo "SOAK DID NOT FINISH OK — leaving stack UP for inspection." | tee -a "$DRIVER_LOG"
    echo "Inspect: docker compose -f utils/ca-soak/$COMPOSE_FILE logs ch1" | tee -a "$DRIVER_LOG"
    echo "Teardown: cd utils/ca-soak && docker compose -f $COMPOSE_FILE down -v" | tee -a "$DRIVER_LOG"
  fi
' EXIT

# Wait for both replicas HTTP-healthy.
echo "Waiting for ch1 (8123) and ch2 (8124)..." | tee -a "$DRIVER_LOG"
for url in http://localhost:8123 http://localhost:8124; do
  for i in $(seq 1 90); do
    curl -sf "$url/ping" >/dev/null 2>&1 && break
    sleep 1
  done
  curl -sf "$url/ping" >/dev/null 2>&1 && echo "$url healthy" | tee -a "$DRIVER_LOG" || { echo "FATAL: $url not healthy after 90s" | tee -a "$DRIVER_LOG"; exit 1; }
done

PYTHONPATH="$(pwd)" python3 -m soak.run \
  --seed "$SEED" --phase 3 --duration "$DURATION" --workers "$WORKERS" \
  --metrics "$METRICS" --max-pool-gb "$MAX_POOL_GB" \
  ${NO_CHAOS:+--no-chaos} \
  2>&1 | tee -a "$DRIVER_LOG"
rc=${PIPESTATUS[0]}

if [ "$rc" -ne 0 ]; then
  echo "PHASE3/gc_shards=2 FAILED (rc=$rc) — stack left UP for inspection" | tee -a "$DRIVER_LOG"
  exit "$rc"
fi
SOAK_OK=1

# Render the metrics curve if matplotlib is available.
PYTHONPATH="$(pwd)" python3 scripts/plot.py "$METRICS" "${METRICS%.db}_curve.png" >> "$DRIVER_LOG" 2>&1 || true
echo "PHASE3/gc_shards=2 OK (run.py exit 0)" | tee -a "$DRIVER_LOG"
echo "Driver log: $DRIVER_LOG  Server logs: $COMPOSE_LOG" | tee -a "$DRIVER_LOG"
