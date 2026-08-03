#!/usr/bin/env bash
# B170 soak watcher: poll the running 12h keepalive soak and RETURN (exit) on the first of:
#   (a) a checkpoint failure / dangling detected in the runner log,
#   (b) the runner process exited (RUNPY_EXIT present),
#   (c) ~1h elapsed (hourly status tick).
# On exit it prints a compact status block (stage, latest metrics tick, event-log histogram,
# any dangling) so the controlling agent can report and decide. Re-launch after each return.
set -uo pipefail
cd "$(dirname "$0")/.."

RUNNER_LOG="logs/soak_b171_12h_runner.log"
DEADLINE=$(( $(date +%s) + 1200 ))   # 20-minute tick
REASON="20min-tick"

q1(){ curl -s --max-time 10 "http://localhost:8123/" --data-binary "$1" 2>/dev/null; }
q2(){ curl -s --max-time 10 "http://localhost:8124/" --data-binary "$1" 2>/dev/null; }

while :; do
  # (a) failure signals in the runner log
  if grep -Eiq "dangling=[1-9]|PHASE3 FAILED|RUNPY_EXIT=[1-9]|AssertionError|checkpoint.*FAIL" "$RUNNER_LOG" 2>/dev/null; then
    REASON="FAILURE-DETECTED"; break
  fi
  # (b) runner finished (clean or not)
  if grep -q "RUNPY_EXIT=" "$RUNNER_LOG" 2>/dev/null; then
    REASON="RUNNER-EXITED"; break
  fi
  # (c) 20-minute tick
  if [ "$(date +%s)" -ge "$DEADLINE" ]; then
    REASON="20min-tick"; break
  fi
  sleep 30
done

echo "==== SOAK WATCH RETURN: $REASON  ($(date -u +%H:%M:%SZ)) ===="
echo "---- runner tail ----"
tail -12 "$RUNNER_LOG" 2>/dev/null
echo "---- docker ps ----"
docker compose ps --format '{{.Service}} {{.Status}}' 2>/dev/null
echo "---- HEALTHCHECK ----"
P1=$(q1 "SELECT 1 FORMAT TSV"); P2=$(q2 "SELECT 1 FORMAT TSV")
echo "ch1-alive: ${P1:-DOWN}   ch2-alive: ${P2:-DOWN}"
echo "ch1 mem-MB / max-MB: $(q1 "SELECT round(value/1048576) FROM system.metrics WHERE metric='MemoryTracking' FORMAT TSV") / $(q1 "SELECT round(max(memory_usage)/1048576) FROM system.processes FORMAT TSV")"
echo "errors last 20m (ch1): $(q1 "SELECT name, sum(value) FROM system.errors WHERE last_error_time > now()-1200 GROUP BY name ORDER BY sum(value) DESC LIMIT 6 FORMAT TSV" | tr '\n' '|')"
echo "ca_stress rows ch1/ch2: $(q1 'SELECT count() FROM ca_stress FORMAT TSV' 2>/dev/null) / $(q2 'SELECT count() FROM ca_stress FORMAT TSV' 2>/dev/null)"
echo "active parts ch1: $(q1 "SELECT count(), round(sum(bytes_on_disk)/1048576) FROM system.parts WHERE active AND table='ca_stress' FORMAT TSV")"
echo "---- event-log (ch1) ----"
q1 "SYSTEM FLUSH LOGS content_addressed_log" >/dev/null
echo "rows/types: $(q1 'SELECT count(), uniqExact(event_type) FROM system.cas_log FORMAT TSV')"
echo "reclaim-so-far (del/root_rm/strip/retire): $(q1 "SELECT countIf(event_type IN ('blob_delete','tree_delete')), countIf(event_type='root_remove'), countIf(event_type='tree_strip'), countIf(event_type='gc_retire_decision') FROM system.cas_log FORMAT TSV")"
echo "anomalies (fail_closed/incoherent/corrupt/read_missing): $(q1 "SELECT countIf(event_type='fail_closed'), countIf(event_type='snap_journal_incoherent'), countIf(event_type='corrupt_decode'), countIf(event_type='read_missing') FROM system.cas_log FORMAT TSV")"
echo "B171 precommit (precommit/removed/reclaim): $(q1 "SELECT countIf(event_type='precommit'), countIf(event_type='precommit_removed'), countIf(event_type='precommit_reclaim') FROM system.cas_log FORMAT TSV")"
echo "B171 FALSE-RECLAIM signature (reclaim w/ 'frozen' reason — should be 0): $(q1 "SELECT count() FROM system.cas_log WHERE event_type='precommit_reclaim' AND reason ILIKE '%frozen%' FORMAT TSV")"
echo "CORRUPTED_DATA / part-check (the B140-dangle symptom — should be 0): $(q1 "SELECT sum(value) FROM system.errors WHERE name='CORRUPTED_DATA' FORMAT TSV")"
echo "---- last checkpoint lines ----"
grep -Ei "checkpoint|dangling|stage |OK|PHASE3|metrics tick" "$RUNNER_LOG" 2>/dev/null | tail -8
