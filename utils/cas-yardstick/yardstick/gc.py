"""GC rounds that overlap a measured window, read from `system.cas_gc_log`."""
import json

# event_time_microseconds is DateTime64(6); comparing it with an integer overflows the decimal.
_SQL = ('SELECT round_id, toUnixTimestamp64Micro(min(event_time_microseconds)) AS t0, '
        'toUnixTimestamp64Micro(max(event_time_microseconds)) AS t1, '
        "anyIf(outcome, event_type = 'Finish') AS outcome, "
        "sumIf(objects_deleted, event_type = 'Finish') AS objects_deleted, "
        "sumIf(manifests_deleted, event_type = 'Finish') AS manifests_deleted, "
        "sumIf(duration_ms, event_type = 'Finish') AS duration_ms, "
        "countIf(event_type = 'Finish') AS finished "
        # A round without its Finish row is still running, so its last event does not bound it. A stop does not log
        # the Finish of a round it cuts, so only rounds this process started can still be running.
        'FROM system.cas_gc_log GROUP BY round_id '
        'HAVING ((finished = 0 AND t0 >= {process_start_us}) OR t1 >= {t_start_us}) AND t0 <= {t_end_us} '
        'ORDER BY t0 FORMAT JSONEachRow')


def collect(client, t_start_us: int, t_end_us: int, process_start_us: int) -> list[dict]:
    """Rounds overlapping [t_start_us, t_end_us] (microseconds since epoch), oldest first.
    `process_start_us` is taken before the current server process was launched."""
    client.query('SYSTEM FLUSH LOGS')
    text = client.query(_SQL.format(t_start_us=int(t_start_us), t_end_us=int(t_end_us),
                                    process_start_us=int(process_start_us)))
    rounds = []
    for line in text.splitlines():
        r = json.loads(line)
        rounds.append(dict(
            round_id=r['round_id'],
            started_in_window=int(r['t0']) >= t_start_us,
            finished_in_window=int(r['finished']) > 0 and int(r['t1']) <= t_end_us,
            outcome=r['outcome'],
            objects_deleted=r['objects_deleted'],
            manifests_deleted=r['manifests_deleted'],
            duration_ms=r['duration_ms']))
    return rounds
