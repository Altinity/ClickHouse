#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.."
# Preserve per-run docker logs to a UNIQUE timestamped file BEFORE teardown, so the EXIT trap (which
# runs `docker compose down -v`) never destroys the evidence of a chaos/recovery failure.
docker compose down -v >/dev/null 2>&1; docker compose up -d
trap 'mkdir -p logs; docker compose logs --no-color > "logs/phase2_$(date +%s)_server.log" 2>&1 || true; docker compose down -v' EXIT
for url in http://localhost:8123 http://localhost:8124; do for i in $(seq 1 90); do curl -sf "$url/ping">/dev/null 2>&1 && break; sleep 1; done; done
PYTHONPATH="$(pwd)" python3 -m soak.run --seed 20260613 --phase 2 --ops 1500 --workers 6 --checkpoint-every 400 --chaos-seed 20260613 --chaos-interval 90
echo "PHASE2 OK"
