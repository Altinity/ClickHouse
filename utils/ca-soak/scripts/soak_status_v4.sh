#!/usr/bin/env bash
# Hourly status + regression watch for the 24h CA soak v4.
# Regression watch (must all stay clean): zero raw Code 499, zero fatal "cannot reuse"/
# FILE_DOESNT_EXIST workload failures, B174 gc/snap bounded, replicas converged, dangling=0.
set -uo pipefail
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak

LOG=logs/soak_24h_v4.log
DB=soak_24h_v4.db

echo "==================== SOAK v4 STATUS @ $(date '+%F %T %Z') ===================="

# 1. liveness — is the driver still advancing?
echo "--- driver ---"
if pgrep -f "soak.run .*soak_24h_v4" >/dev/null; then echo "driver: RUNNING"; else echo "driver: NOT RUNNING (exited)"; fi
tail -4 "$LOG" 2>/dev/null | sed 's/^/  /'

# 2. metrics db: latest tick per node (pool growth = B174 gc/snap bound; convergence; dangling)
echo "--- metrics (latest tick per node) ---"
sqlite3 "$DB" "SELECT 'last_tick='||datetime(max(ts),'unixepoch')||'  ticks='||count(DISTINCT ts) FROM metrics;" 2>/dev/null
sqlite3 -column -header "$DB" "
  WITH last AS (SELECT max(ts) m FROM metrics)
  SELECT node, table_rows, parts_active, pool_objects,
         round(pool_bytes/1048576.0,1) AS pool_MB,
         repl_queue, mutations_pending, restarts,
         fsck_unreachable AS unreach, fsck_dangling AS dangling
  FROM metrics WHERE ts=(SELECT m FROM last) ORDER BY node;" 2>/dev/null

# 3. REGRESSION WATCH — server logs (read via docker exec; syslog-owned on host)
echo "--- regression watch (server logs) ---"
for c in ca-soak-ch1-1 ca-soak-ch2-1; do
  c499=$(docker exec "$c" sh -c "cat /var/log/clickhouse-server/*.log 2>/dev/null | grep -c 'Code: 499'" 2>/dev/null)
  reuse=$(docker exec "$c" sh -c "cat /var/log/clickhouse-server/*.log 2>/dev/null | grep -c 'cannot reuse'" 2>/dev/null)
  fdne=$(docker exec "$c" sh -c "cat /var/log/clickhouse-server/*.log 2>/dev/null | grep -c 'FILE_DOESNT_EXIST'" 2>/dev/null)
  echo "  $c: Code499=${c499:-?}  cannot-reuse=${reuse:-?}  FILE_DOESNT_EXIST=${fdne:-?}"
done

# 4. workload failures recorded by the driver
echo "--- workload failures (driver log) ---"
fails=$(grep -c "WORKLOAD FAILURE" "$LOG" 2>/dev/null)
echo "  WORKLOAD FAILURE lines: ${fails:-0}"
[ "${fails:-0}" -gt 0 ] && grep "WORKLOAD FAILURE" "$LOG" | tail -5 | sed 's/^/    /'

# 5. replica convergence + dangling (checker output in driver log)
echo "--- convergence / dangling (latest checker lines) ---"
grep -iE "converg|dangl|INV-NO-DANGLE|mismatch|diverg" "$LOG" 2>/dev/null | tail -6 | sed 's/^/  /'

echo "==================== END ===================="
