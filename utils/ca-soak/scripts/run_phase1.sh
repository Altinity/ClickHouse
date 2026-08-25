#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Preserve per-run docker logs to a UNIQUE timestamped/labeled file before teardown, so the EXIT
# trap (which runs `docker compose down -v`) never destroys the evidence. The label can be set via
# RUN_LABEL (e.g. the insert-mode of the experiment); it defaults to "phase1".
LOGDIR="$(pwd)/logs"
mkdir -p "$LOGDIR"
RUN_LABEL="${RUN_LABEL:-phase1}"
RUN_TS="$(date +%Y%m%dT%H%M%S)"
COMPOSE_LOG="$LOGDIR/compose_${RUN_LABEL}_${RUN_TS}.log"
docker compose up -d
trap 'docker compose logs --no-color > "$COMPOSE_LOG" 2>&1 || true; echo "preserved docker logs -> $COMPOSE_LOG"; docker compose down -v' EXIT
for url in http://localhost:8123 http://localhost:8124; do for i in $(seq 1 90); do curl -sf "$url/ping">/dev/null 2>&1 && break; sleep 1; done; done
PYTHONPATH="$(pwd)" python3 -m soak.run --seed 20260613 --phase 1 --ops 1500 --workers 6 --checkpoint-every 300 ${INSERT_MODE:+--insert-mode "$INSERT_MODE"}
echo "PHASE1 OK"
