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
SELECT_WORKERS="${SELECT_WORKERS:-4}"
SELECT_INTERVAL_S="${SELECT_INTERVAL_S:-2.0}"

LOGDIR="$(pwd)/logs"
mkdir -p "$LOGDIR"
RUN_TS="$(date +%Y%m%dT%H%M%S)"
COMPOSE_LOG="$LOGDIR/phase3_${RUN_TS}_server.log"

# B165: per-node ClickHouse log dirs bind-mounted into the containers so the server's own logs
# survive `docker compose down -v` (the soak #7 OOM left no in-container logs to diagnose). The
# server runs as uid 101 inside the container, so the host dirs must be writable by it. Start each
# run from a clean dir so a post-mortem reads only THIS run's logs.
# Archive-before-restart (2026-07-13): never delete the previous run's logs — a restart used to
# destroy the evidence of a failed run before its investigation finished. Move them aside instead.
if [ -d "$LOGDIR/ch1" ] || [ -d "$LOGDIR/ch2" ]; then
  PREV_DIR="$LOGDIR/prev_${RUN_TS}"
  mkdir -p "$PREV_DIR"
  mv "$LOGDIR/ch1" "$PREV_DIR/" 2>/dev/null || true
  mv "$LOGDIR/ch2" "$PREV_DIR/" 2>/dev/null || true
  echo "previous run's log dirs archived -> $PREV_DIR"
fi
mkdir -p "$LOGDIR/ch1" "$LOGDIR/ch2"
chmod 777 "$LOGDIR/ch1" "$LOGDIR/ch2"

docker compose down -v >/dev/null 2>&1; docker compose up -d
WATCHDOG_PID=""
SOAK_OK=0   # set to 1 only after a clean PHASE3 OK; the trap tears down ONLY on a happy finish.
# EXIT trap: ALWAYS preserve evidence (compose logs + per-container inspect, captured while the
# containers still exist) and stop the watchdog. Tear down (`down -v`) ONLY on a happy finish — on
# ANY failure we leave the FULL stack (containers + volumes) UP so the crash can be diagnosed
# (who/how/where: disk, OOM, CA exception). Manual teardown: `cd utils/ca-soak && docker compose down -v`.
trap '
  docker compose logs --no-color > "$COMPOSE_LOG" 2>&1 || true
  docker compose ps -a >> "$COMPOSE_LOG" 2>&1 || true
  for c in $(docker compose ps -aq 2>/dev/null); do docker inspect --format "{{.Name}} State={{.State.Status}} OOMKilled={{.State.OOMKilled}} ExitCode={{.State.ExitCode}}" "$c" >> "$COMPOSE_LOG" 2>&1 || true; done
  # The B165 host bind-mount stopped capturing server logs (dirs come out empty), and chaos
  # restarts recreate containers so `docker compose logs` loses earlier instances. Copy the
  # server log FILES out of the live containers while they still exist — this is the only
  # reliable evidence path (2026-07-13, v2/v3 postmortems).
  for node in ch1 ch2; do
    docker cp "$(docker compose ps -q $node 2>/dev/null | head -1)":/var/log/clickhouse-server/. "$LOGDIR/$node/" 2>/dev/null || true
  done
  echo "preserved docker logs+state -> $COMPOSE_LOG (+ server log files -> $LOGDIR/ch1,ch2)"
  kill $WATCHDOG_PID 2>/dev/null || true
  if [ "$SOAK_OK" = 1 ]; then
    echo "PHASE3 OK — tearing down (down -v)"; docker compose down -v
  else
    echo "SOAK DID NOT FINISH OK — leaving stack UP for inspection (containers + volumes preserved)."
    echo "Inspect: docker compose -f utils/ca-soak/docker-compose.yml logs ch1 ; then: cd utils/ca-soak && docker compose down -v"
  fi
' EXIT

# Wait for both replicas HTTP-healthy.
for url in http://localhost:8123 http://localhost:8124; do
  for i in $(seq 1 90); do curl -sf "$url/ping" >/dev/null 2>&1 && break; sleep 1; done
done

# Start the host-disk safety watchdog (B167g/B204). It exits on its own when the driver finishes,
# but the EXIT trap kills it explicitly so no stray process lingers after teardown.
bash scripts/disk_watchdog.sh &
WATCHDOG_PID=$!

PYTHONPATH="$(pwd)" python3 -m soak.run \
  --seed "$SEED" --phase 3 --duration "$DURATION" --workers "$WORKERS" \
  --metrics "$METRICS" --max-pool-gb "$MAX_POOL_GB" \
  --select-workers "$SELECT_WORKERS" --select-interval-s "$SELECT_INTERVAL_S" \
  ${NO_CHAOS:+--no-chaos}
rc=$?
if [ "$rc" -ne 0 ]; then echo "PHASE3 FAILED (rc=$rc) — stack left UP for inspection (see trap)"; exit "$rc"; fi
SOAK_OK=1   # happy finish — the EXIT trap will now tear down

# Render the metrics curve (PNG if matplotlib is present, else a TSV).
PYTHONPATH="$(pwd)" python3 scripts/plot.py "$METRICS" "${METRICS%.db}_curve.png" || true
echo "PHASE3 OK (run.py exit 0)"
