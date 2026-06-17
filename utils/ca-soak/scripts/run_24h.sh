#!/usr/bin/env bash
# CA soak Phase-3: the real, operator-invoked 24h productionization run.
#
# Time-driven stage timeline (soak.schedule) over --duration (default 24h): warmup -> steady ->
# +mutations -> +TTL pressure -> GC checkpoint -> +chaos -> truncate/drop cliff -> final
# converge+restart. A per-minute MetricsTicker records the referenced-vs-physical pool curve into
# the --metrics sqlite and enforces the --max-pool-gb budget by THROTTLING (never dropping) inserts.
#
# Docker logs are preserved to a unique timestamped file BEFORE teardown so a 24h run's evidence
# survives the `docker compose down -v`. `soak.run` prints its OWN authoritative "PHASE3 OK" and
# exits non-zero on any checkpoint/workload/transport failure; we gate on that exit code.
set -uo pipefail
cd "$(dirname "$0")/.."

SEED="${SEED:-20260613}"
DURATION="${DURATION:-24h}"
WORKERS="${WORKERS:-6}"
METRICS="${METRICS:-soak.db}"
MAX_POOL_GB="${MAX_POOL_GB:-40}"

LOGDIR="$(pwd)/logs"
mkdir -p "$LOGDIR"
RUN_TS="$(date +%Y%m%dT%H%M%S)"
COMPOSE_LOG="$LOGDIR/phase3_${RUN_TS}_server.log"

# B165: per-node ClickHouse log dirs bind-mounted into the containers so the server's own logs
# survive `docker compose down -v` (the soak #7 OOM left no in-container logs to diagnose). The
# server runs as uid 101 inside the container, so the host dirs must be writable by it. Start each
# run from a clean dir so a post-mortem reads only THIS run's logs.
rm -rf "$LOGDIR/ch1" "$LOGDIR/ch2"
mkdir -p "$LOGDIR/ch1" "$LOGDIR/ch2"
chmod 777 "$LOGDIR/ch1" "$LOGDIR/ch2"

docker compose down -v >/dev/null 2>&1; docker compose up -d
trap 'docker compose logs --no-color > "$COMPOSE_LOG" 2>&1 || true; echo "preserved docker logs -> $COMPOSE_LOG"; docker compose down -v' EXIT

# Wait for both replicas HTTP-healthy.
for url in http://localhost:8123 http://localhost:8124; do
  for i in $(seq 1 90); do curl -sf "$url/ping" >/dev/null 2>&1 && break; sleep 1; done
done

PYTHONPATH="$(pwd)" python3 -m soak.run \
  --seed "$SEED" --phase 3 --duration "$DURATION" --workers "$WORKERS" \
  --metrics "$METRICS" --max-pool-gb "$MAX_POOL_GB" ${NO_CHAOS:+--no-chaos}
rc=$?
if [ "$rc" -ne 0 ]; then echo "PHASE3 FAILED (rc=$rc)"; exit "$rc"; fi

# Render the metrics curve (PNG if matplotlib is present, else a TSV).
PYTHONPATH="$(pwd)" python3 scripts/plot.py "$METRICS" "${METRICS%.db}_curve.png" || true
echo "PHASE3 OK (run.py exit 0)"
