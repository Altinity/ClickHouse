#!/usr/bin/env bash
# Emit ONE compact health summary line for the CA soak (used by the hourly Monitor + manual pulses).
# Covers: driver liveness, parts responsiveness, correctness (no-ref/broken), #1 HEAD:write ratio,
# GC round outcomes (B160), 412/503, disk (roots/blobs) + reaper effect, replication lag.
set -uo pipefail
cd "$(dirname "$0")/.."

q() { curl -s --max-time 20 "http://localhost:$1/" --data-binary "$2" 2>/dev/null; }

# teardown detection: both nodes unreachable AND no driver => soak ended
if ! curl -sf --max-time 5 http://localhost:8123/ping >/dev/null 2>&1 \
   && ! curl -sf --max-time 5 http://localhost:8124/ping >/dev/null 2>&1; then
  if ! pgrep -f "python3 -m soak.run" >/dev/null 2>&1; then
    echo "SOAK ENDED (both nodes unreachable, driver gone) @ $(date +%H:%M:%S)"
    exit 0
  fi
fi

drv=$(pgrep -f "python3 -m soak.run" >/dev/null 2>&1 && echo up || echo DOWN)
up=$(q 8123 "SELECT toUInt64(uptime()) FORMAT TabSeparated")

# per-node: parts (responsiveness via query time), genuine no-ref exceptions, broken detached parts
sig() {
  local hp=$1
  local t0 t1 parts
  t0=$(date +%s.%N)
  parts=$(q "$hp" "SELECT count() FROM system.parts WHERE active FORMAT TabSeparated")
  t1=$(date +%s.%N)
  local pq; pq=$(awk "BEGIN{printf \"%.2f\", $t1-$t0}")
  local noref broken
  noref=$(q "$hp" "SELECT count() FROM system.text_log WHERE message LIKE '%no ref for%' AND logger_name NOT IN ('executeQuery') AND source_file NOT LIKE '%executeQuery%' AND source_file NOT LIKE '%SelectExecutor%' FORMAT TabSeparated")
  broken=$(q "$hp" "SELECT count() FROM system.detached_parts WHERE reason LIKE '%broken%' FORMAT TabSeparated")
  echo "${parts:-NA}/${pq}s noref=${noref:-NA} broken=${broken:-NA}"
}

heads=$(q 8123 "SELECT concat(toString((SELECT value FROM system.events WHERE event='S3HeadObject')),'/',toString((SELECT value FROM system.events WHERE event='S3PutObject'))) FORMAT TabSeparated")
gc=$(q 8123 "SELECT concat(toString(countIf(message LIKE '%CA GC round failed%')),'f/',toString(countIf(message LIKE '%CA GC round %candidates=%')),'ok/',toString(countIf(message LIKE '%moved during retire%')),'rc') FROM system.text_log WHERE message LIKE '%CA GC round%' FORMAT TabSeparated")
err=$(q 8123 "SELECT concat(toString(countIf(message LIKE '%recondition%' OR message LIKE '% 412%')),'x412/',toString(countIf(message LIKE '%ervice%navailable%' OR message LIKE '% 503%')),'x503/',toString(countIf(message LIKE '%roken pipe%')),'xbp') FROM system.text_log WHERE level<='Error' FORMAT TabSeparated")
repl=$(q 8123 "SELECT max(absolute_delay) FROM system.replicas FORMAT TabSeparated")
du=$(docker exec ca-soak-rustfs1-1 sh -c 'echo "roots=$(du -sh /data/test/soak_pool/roots 2>/dev/null|cut -f1) blobs=$(du -sh /data/test/soak_pool/blobs 2>/dev/null|cut -f1)"' 2>/dev/null)
reap=$(tail -1 "$(ls -t logs/*_reaper.log 2>/dev/null | head -1)" 2>/dev/null | sed 's/.*orphan_reaper/reaper/')

echo "SOAK t=${up:-?}s drv=$drv | ch1 $(sig 8123) | ch2 $(sig 8124) | HEAD/PUT=${heads:-NA} | GC=${gc:-NA} | err=${err:-NA} | replLag=${repl:-NA}s | du[$du] | ${reap:-noreap}"
