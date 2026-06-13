#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose up -d
trap 'docker compose logs --no-color > /tmp/ca_soak_phase1_compose.log 2>&1 || true; docker compose down -v' EXIT
for url in http://localhost:8123 http://localhost:8124; do for i in $(seq 1 90); do curl -sf "$url/ping">/dev/null 2>&1 && break; sleep 1; done; done
PYTHONPATH="$(pwd)" python3 -m soak.run --seed 20260613 --phase 1 --ops 1500 --workers 6 --checkpoint-every 300
echo "PHASE1 OK"
