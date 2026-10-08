#!/usr/bin/env bash
# B167g host-safety watchdog for the 12h CA soak. Polls free disk on / every 60s; if it drops below
# THRESHOLD_KB, STOP the soak (kill driver + run_24h.sh + docker compose down -v) before / fills and
# endangers the repo/builds on the same volume. Also exits if the soak driver is already gone.
# Self-contained (no LLM polling). Writes a marker so the hourly cron can report the trip.
set -uo pipefail
cd "$(dirname "$0")/.."                       # -> utils/ca-soak
THRESHOLD_KB=$((60 * 1024 * 1024))            # 60 GiB free floor
MARKER="logs/WATCHDOG_TRIPPED"
LOG="logs/disk_watchdog.log"
rm -f "$MARKER"
echo "$(date +%H:%M:%S) watchdog start, floor=60G; waiting for driver" >> "$LOG"
# Wait up to 10 min for the soak driver to appear (bring-up + warmup) before monitoring, so we don't
# mistake "not started yet" for "ended".
for _ in $(seq 1 120); do
  pgrep -f "python3 -m soak.run" >/dev/null 2>&1 && break
  sleep 5
done
echo "$(date +%H:%M:%S) driver detected (or wait elapsed) — monitoring" >> "$LOG"
while true; do
  avail=$(df -P / | awk 'NR==2{print $4}')
  roots=$(docker exec ca-soak-rustfs1-1 sh -c 'du -sm /data/test/soak_pool/roots 2>/dev/null | cut -f1' 2>/dev/null)
  echo "$(date +%H:%M:%S) avail_kb=$avail roots_mb=${roots:-NA}" >> "$LOG"
  # soak ended on its own?
  if ! pgrep -f "python3 -m soak.run" >/dev/null 2>&1; then
    echo "$(date +%H:%M:%S) driver gone — watchdog exiting (soak ended)" >> "$LOG"
    exit 0
  fi
  if [ -n "$avail" ] && [ "$avail" -lt "$THRESHOLD_KB" ]; then
    echo "$(date +%H:%M:%S) TRIPPED avail_kb=$avail < $THRESHOLD_KB — stopping soak" >> "$LOG"
    echo "tripped at $(date) avail_kb=$avail roots_mb=${roots:-NA}" > "$MARKER"
    for pid in $(ps -eo pid,args | grep -E '[s]oak\.run|[r]un_24h\.sh' | awk '{print $1}'); do kill "$pid" 2>/dev/null; done
    sleep 3
    docker compose down -v >> "$LOG" 2>&1
    echo "$(date +%H:%M:%S) soak stopped, volumes reclaimed" >> "$LOG"
    exit 0
  fi
  sleep 60
done
