"""CAS / Overview: six status tiles and four rows, in plain language."""
from . import panels as P
from .panels import SETTINGS, DRILLDOWN_LINK, guarded, c

INTEGRITY_EVENTS = "('read_missing', 'dangling_access', 'corrupt_dangle', 'corrupt_decode', 'snap_journal_incoherent', 'exception')"
METRICS_USED = set()  # filled by _m() so tests can hand the checker exactly the names this module uses


def _m(name: str) -> str:
    METRICS_USED.add(name)
    return name


def _metric_sql(select: str) -> str:
    return f"""SELECT {select}
FROM clusterAllReplicas('$cluster', system.metric_log)
WHERE hostName() IN ($host) AND $timeFilter
{SETTINGS}"""


def tile_storage() -> dict:
    a = f"""SELECT
    (SELECT count() FROM system.clusters WHERE cluster = '$cluster') - uniqExact(host) AS "Hosts not reporting",
    countIf(lifecycle != 'live') AS "Disks not live"
FROM (SELECT hostName() AS host, disk, any(lifecycle) AS lifecycle
      FROM clusterAllReplicas('$cluster', system.cas_mounts)
      WHERE disk IN ($disk)
      GROUP BY host, disk)
{SETTINGS}"""
    b = _metric_sql(f"""{guarded(c(_m('CASMountLeaseLost')))} AS "Lease lost",
    dateDiff('second', max(event_time), now()) AS "Metrics age (s)\"""")
    return P.stat(
        "Storage online",
        "Every host can read and write the shared storage right now. "
        "Hosts not reporting: hosts in the cluster that did not answer. Disks not live: storage disks that lost their "
        "connection to the pool. Lease lost: how many times a host lost its write permission in the selected period. "
        "Metrics age: seconds since the last metrics row; when the storage is down the server stops writing its own logs, "
        "so a growing age is itself a warning. What to do: check the host and its object storage endpoint; "
        "see Drilldown → Writes: lease and Writes: mounts now.",
        [P.target(a, fmt="table", table="cas_mounts", ref="A"), P.target(b, fmt="table", ref="B")],
        {"Hosts not reporting": {"steps": [("green", None), ("red", 1)]},
         "Disks not live": {"steps": [("green", None), ("red", 1)]},
         "Lease lost": {"steps": [("green", None), ("red", 1)]},
         "Metrics age (s)": {"steps": [("green", None), ("yellow", 120), ("red", 600)], "unit": "s"}},
        links=[DRILLDOWN_LINK], interval="5m", h=7)


def tile_writes() -> dict:
    sql = _metric_sql(f"""{guarded(c(_m('CASRequestGaveUp')))} AS "Gave up",
    if(sum(ProfileEvent_{_m('CASConditionalWriteAttempts')}) = 0, NULL,
       round(100 * sum(ProfileEvent_{_m('CASConditionalWriteUnresolved')}) / sum(ProfileEvent_CASConditionalWriteAttempts), 3)) AS "Unresolved %\"""")
    return P.stat(
        "Writes safe",
        "Writes to the shared storage are confirmed. Gave up: writes abandoned after all retries (any value needs a look). "
        "Unresolved %: share of writes whose answer from the storage was lost and had to be re-checked by reading back; "
        "0.03% is normal on a healthy backend, a rise means storage timeouts. What to do: check object-storage latency and "
        "errors; see Drilldown → Writes: conditional writes and S3: errors and throttling.",
        [P.target(sql, fmt="table")],
        {"Gave up": {"steps": [("green", None), ("red", 1)]},
         "Unresolved %": {"steps": [("green", None), ("yellow", 0.1)], "unit": "percent"}},
        links=[DRILLDOWN_LINK], h=7)


def tile_gc() -> dict:
    a = f"""SELECT if(count() = 0, NULL, dateDiff('minute', max(event_time), now())) AS "Minutes since last success" /* now */
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND event_type = 'Finish' AND outcome IN ('Success', 'Deferred') AND event_date >= today() - 7
{SETTINGS}"""
    b = f"""SELECT countIf(outcome IN ('Error', 'Aborted', 'Stopped')) AS "Failed rounds"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND event_type = 'Finish' AND $timeFilter
{SETTINGS}"""
    return P.stat(
        "GC: last success",
        "Garbage collection frees storage that deleted or merged data no longer needs. Rounds normally finish every 5–10 "
        "minutes. Minutes since last success: time since the last completed round anywhere in the cluster; yellow after "
        "30 minutes, red after 2 hours. Failed rounds: rounds in the selected period that ended with an error or were "
        "aborted (an event count: no rows means none). What to do: open Drilldown → GC: rounds for the error text; a long-running round is normal after a large "
        "deletion, a stopped one is not.",
        [P.target(a, fmt="table", table="cas_gc_log", ref="A"), P.target(b, fmt="table", table="cas_gc_log", ref="B")],
        {"Minutes since last success": {"steps": [("green", None), ("yellow", 30), ("red", 120)], "unit": "m"},
         "Failed rounds": {"steps": [("green", None), ("red", 1)]}},
        links=[DRILLDOWN_LINK], h=7)


def tile_garbage() -> dict:
    a = f"""SELECT if(count() = 0, NULL, round(100 * sum(net) / greatest(sum(cond), 1), 1)) AS "Backlog growth %, last 10 rounds",
       if(count() = 0, NULL, sum(net)) AS "Net objects, last 10 rounds" /* now */
FROM (SELECT toInt64(entries_condemned) - toInt64(entries_redeleted) AS net, toInt64(entries_condemned) AS cond
      FROM clusterAllReplicas('$cluster', system.cas_gc_log)
      WHERE disk_name IN ($disk) AND event_type = 'Finish' AND outcome = 'Success' AND event_date >= today() - 7
      ORDER BY event_time DESC LIMIT 10)
{SETTINGS}"""
    return P.stat(
        "Garbage trend",
        "Is garbage collection keeping up with deletions? Each round marks objects for deletion and deletes objects marked "
        "earlier. Backlog growth: over the last 10 rounds, the share of newly marked objects that were not yet deleted; "
        "near zero is steady state, it rises for about an hour after a server restart and recovers by itself. Yellow above "
        "25%, red above 50%. Net objects: marked minus deleted over those rounds. What to do: if red for more than two "
        "hours, open Drilldown → GC: stage flow and GC: round cost.",
        [P.target(a, fmt="table", table="cas_gc_log")],
        {"Backlog growth %, last 10 rounds": {"steps": [("green", None), ("yellow", 25), ("red", 50)], "unit": "percent"},
         "Net objects, last 10 rounds": {"fixed": "blue"}},
        links=[DRILLDOWN_LINK], h=7)


def tile_integrity() -> dict:
    a = f"""SELECT count() AS "Integrity events"
FROM clusterAllReplicas('$cluster', system.cas_log)
WHERE disk_name IN ($disk) AND event_type IN {INTEGRITY_EVENTS} AND $timeFilter
{SETTINGS}"""
    b = f"""SELECT if(count() = 0, NULL, sum(anomalies)) AS "GC anomalies"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND event_type = 'Finish' AND $timeFilter
{SETTINGS}"""
    cc = _metric_sql(f"""{guarded(c(_m('CASRefNeedsRecovery')))} AS "Lanes needing recovery\"""")
    d = f"""SELECT if(count() = 0, NULL, max(v)) AS "Wedged lanes"
FROM (SELECT hostName() AS host, argMax(value, event_time) AS v
      FROM clusterAllReplicas('$cluster', system.asynchronous_metric_log)
      WHERE hostName() IN ($host) AND metric LIKE 'CASGCWedgedNamespaces_%'
        AND replaceOne(metric, 'CASGCWedgedNamespaces_', '') IN ($disk) AND $timeFilter
      GROUP BY host)
{SETTINGS}"""
    return P.stat(
        "Integrity",
        "Signals that must stay at zero. Integrity events: the storage found data that should exist but does not, or "
        "could not be decoded (an event log: no rows means no events, so this field cannot show 'no data'; the "
        "Storage online tile's Metrics age tells whether logging itself stopped). GC anomalies: garbage collection hit an inconsistency and worked around it; a value that "
        "stays above zero across rounds deserves a look. Wedged lanes and Lanes needing recovery: a table's write queue to "
        "the shared storage is stuck. What to do: any red value is a support case; keep the selected time range and open "
        "Drilldown → Needs attention and GC: anomalies.",
        [P.target(a, fmt="table", table="cas_log", ref="A"), P.target(b, fmt="table", table="cas_gc_log", ref="B"),
         P.target(cc, fmt="table", ref="C"), P.target(d, fmt="table", table="asynchronous_metric_log", ref="D")],
        {"Integrity events": {"steps": [("green", None), ("red", 1)]},
         "GC anomalies": {"steps": [("green", None), ("yellow", 1)]},
         "Lanes needing recovery": {"steps": [("green", None), ("red", 1)]},
         "Wedged lanes": {"steps": [("green", None), ("red", 1)]}},
        links=[DRILLDOWN_LINK], h=7)


def tile_s3() -> dict:
    a = _metric_sql(f"""if(count() = 0, NULL,
       round(60 * (sum(ProfileEvent_{_m('S3ReadRequestsThrottling')}) + sum(ProfileEvent_{_m('S3WriteRequestsThrottling')}))
             / greatest(dateDiff('second', min(event_time), max(event_time)), 1), 2)) AS "Throttling per minute\"""")
    b = f"""SELECT round(countIf(error != '' AND error NOT LIKE '%PreconditionFailed%' AND error NOT LIKE '%ConditionalRequestConflict%')
             / greatest(($__to - $__from) / 3600000, 1), 1) AS "S3 errors per hour"
FROM clusterAllReplicas('$cluster', system.blob_storage_log)
WHERE disk_name IN ($disk) AND $timeFilter
{SETTINGS}"""
    return P.stat(
        "S3 throttling",
        "The object storage is answering at full speed. Throttling per minute: requests the storage slowed down "
        "(HTTP 429/503) per minute over the selected period; zero is normal, bursts follow a server restart. S3 errors: "
        "failed uploads or deletes per hour, not counting the storage's expected answers to concurrent writes of the same "
        "object (HTTP 412 and 409); a few per day are normal and retried, yellow above 5 per hour, red above 50. What to do: sustained throttling means the request rate is above "
        "the bucket's limit; see Drilldown → S3: by verb and S3: errors and throttling.",
        [P.target(a, fmt="table", ref="A"), P.target(b, fmt="table", table="blob_storage_log", ref="B")],
        {"Throttling per minute": {"steps": [("green", None), ("yellow", 0.01), ("red", 10)]},
         "S3 errors per hour": {"steps": [("green", None), ("yellow", 5), ("red", 50)]}},
        links=[DRILLDOWN_LINK], h=7)


def tiles() -> list:
    return [tile_storage(), tile_writes(), tile_gc(), tile_garbage(), tile_integrity(), tile_s3()]


NEEDS_ATTENTION_SQL = f"""SELECT toUInt64(event_time) * 1000 AS "Time", host AS "Host", what AS "What happened", detail AS "Details"
FROM (
    SELECT event_time, hostName() AS host, concat('Integrity: ', toString(event_type)) AS what,
           concat(namespace, ' ', ref_name, ' ', toString(reason)) AS detail
    FROM clusterAllReplicas('$cluster', system.cas_log)
    WHERE disk_name IN ($disk) AND event_type IN {INTEGRITY_EVENTS} AND $timeFilter
    UNION ALL
    SELECT event_time, hostName() AS host, concat('GC round ', toString(round), ': ', toString(outcome)) AS what, error AS detail
    FROM clusterAllReplicas('$cluster', system.cas_gc_log)
    WHERE disk_name IN ($disk) AND event_type = 'Finish' AND outcome IN ('Error', 'Aborted', 'Stopped') AND $timeFilter
    UNION ALL
    SELECT event_time, hostName() AS host, concat('Storage lease: ', toString(event_type), ' ', toString(outcome)) AS what,
           concat(toString(reason), ' ', toString(detail)) AS detail
    FROM clusterAllReplicas('$cluster', system.cas_log)
    WHERE disk_name IN ($disk) AND event_type IN ('mount_remount', 'watermark_renew') AND outcome = 'failed' AND $timeFilter
    UNION ALL
    SELECT event_time, hostName() AS host, 'GC fenced out an expired host' AS what, concat(toString(reason), ' ', toString(detail)) AS detail
    FROM clusterAllReplicas('$cluster', system.cas_log)
    WHERE disk_name IN ($disk) AND event_type = 'gc_fence_out' AND $timeFilter
)
ORDER BY "Time" DESC LIMIT 100
{SETTINGS}"""


def rows() -> list:
    attention = P.table(
        "Needs attention",
        "Recent events that need a person: integrity findings, garbage-collection rounds that failed or were aborted, "
        "failed storage-lease renewals or reconnects, and hosts fenced out of the pool. Empty is good. "
        "What to do: read the Details column; the same events appear with full context on the Drilldown.",
        NEEDS_ATTENTION_SQL, h=8)
    lease = P.metric_ts(
        "Storage lease renewals per second",
        "Each host renews its write permission every 10 s. Retries and recoveries mean the storage answered slowly; "
        "Lost means a host was cut off and had to reconnect.",
        {"Attempts": P.r(_m("CASMountRenewalAttempts")), "Retries": P.r(_m("CASMountRenewalRetries")),
         "Recovered": P.r(_m("CASMountRenewalRecovered")), "Lost": P.r(_m("CASMountLeaseLost"))}, unit="ops")
    writes = P.metric_ts(
        "Writes: committed vs unresolved per second",
        "Committed: writes the storage confirmed. Unresolved: writes whose confirmation was lost and had to be re-checked "
        "by reading back; normally a tiny fraction of committed.",
        {"Committed": P.r(_m("CASConditionalWriteCommitted")), "Unresolved": P.r(_m("CASConditionalWriteUnresolved"))}, unit="ops")
    delay = P.metric_ts(
        "Insert delay caused by storage (ms per operation)",
        "Average time an INSERT, merge or delete waited for the shared storage's write queue. About 200 ms is normal on "
        "otel.demo; a sustained rise slows every INSERT.",
        {"Wait per operation": f"sum(ProfileEvent_{_m('CASRefQueueWaitMicroseconds')}) / greatest(sum(ProfileEvent_{_m('CASRefBatchedMutations')}), 1) / 1000"},
        unit="ms")
    rounds = P.raw_ts(
        "GC rounds per 30 minutes, by outcome",
        "How many garbage-collection rounds finished, colored by outcome. Success and Deferred are healthy (Deferred = "
        "nothing to do). Error, Aborted or Stopped bars need a look at Needs attention.",
        f"""SELECT $timeSeries AS t, toString(outcome) AS outcome, count()
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND $timeFilter
GROUP BY t, outcome ORDER BY t
{SETTINGS}""", unit="short", stack=True, draw="bars", interval="30m", w=6)
    rounds["fieldConfig"]["overrides"] = P.outcome_color_overrides()
    duration = P.raw_ts(
        "GC round duration",
        "Wall-clock time of each finished round. Rounds take a few minutes on otel.demo; a long round after a large "
        "deletion is normal, a round that never finishes is not.",
        f"""SELECT toUInt64(event_time) * 1000 AS t, toString(outcome) AS outcome, duration_ms
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND $timeFilter
ORDER BY t
{SETTINGS}""", unit="ms", draw="points", w=6)
    stages = P.raw_ts(
        "GC per round: marked vs deleted objects",
        "Marked: objects a round found to be garbage. Deleted: objects it physically removed. Over time the two lines "
        "should track each other; deleted staying below marked for many rounds means the backlog grows.",
        f"""SELECT toUInt64(event_time) * 1000 AS t, entries_condemned AS "Marked", entries_redeleted AS "Deleted"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND outcome = 'Success' AND $timeFilter
ORDER BY t
{SETTINGS}""", unit="short", w=6)
    backlog = P.raw_ts(
        "GC backlog change over the selected period",
        "Running total of marked minus deleted objects since the start of the selected period. Flat or falling is healthy; "
        "a line that keeps rising means garbage is being found faster than it is removed.",
        f"""SELECT t, sum(net) OVER (ORDER BY t) AS "Net backlog change"
FROM (SELECT toUInt64(event_time) * 1000 AS t, toInt64(entries_condemned) - toInt64(entries_redeleted) AS net
      FROM clusterAllReplicas('$cluster', system.cas_gc_log)
      WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND outcome = 'Success' AND $timeFilter)
ORDER BY t
{SETTINGS}""", unit="short", w=6)
    from .drilldown import pm_latest, PHASE_WHERE, UPLOADS_FROM  # noqa: E402 - the Overview reuses the Drilldown's inventory expressions
    inventory = P.stat(
        "Tables and write logs",
        "What the shared storage holds right now, as counted by the last garbage-collection round. Tables: tables with "
        "data in the storage. Write-log entries: records of inserts and deletes not yet compacted away. "
        "The total counts of data objects, parts and part descriptions are not available yet.",
        [P.target(f"""SELECT {pm_latest('namespaces_seen')} AS "Tables", {pm_latest('ref_log_keys_listed')} AS "Write-log entries" /* now */
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE {PHASE_WHERE} AND event_date >= today() - 7
{SETTINGS}""", fmt="table", table="cas_gc_log")],
        {"Tables": {"fixed": "blue"}, "Write-log entries": {"fixed": "blue"}},
        links=[DRILLDOWN_LINK], w=6, h=8)
    data_objects = P.metric_ts(
        "Data objects: new vs deleted per second",
        "Data objects are the pieces of table data in the shared storage. New: objects uploaded; Deleted: objects removed by "
        "garbage collection. Over days the two should balance; deleted staying at zero while new grows means garbage "
        "collection is not removing anything.",
        {"New": P.r(_m("CASBlobHeadMiss")), "Deleted": P.r(_m("CASBlobDelete"))}, unit="ops", w=6)
    part_descriptions = P.metric_ts(
        "Part descriptions: new vs deleted per second",
        "Every data part has one description object in the shared storage. New ones appear on insert and merge, deleted "
        "ones follow part removal.",
        {"New": P.r(_m("CASManifestPut")), "Deleted": P.r(_m("CASManifestDelete"))}, unit="ops", w=6)
    write_log = P.metric_ts(
        "Write-log entries: written vs cleaned per second",
        "Each insert or delete appends an entry to the table's write log in the shared storage; garbage collection compacts "
        "old entries away. Written staying above cleaned for a long time means the logs keep growing.",
        {"Written": P.r(_m("CASRootPut")), "Cleaned": P.r(_m("CASRootDelete"))}, unit="ops", w=6)
    uploaded = P.raw_ts(
        "Data uploaded per second, by kind",
        "Bytes written to the shared storage: table data, part descriptions, write logs and GC state. This is "
        "traffic, not the amount stored; stored totals are not available yet.",
        f"""SELECT $timeSeries AS t, multiIf(p2 = 'blobs', 'Table data', p2 = 'cas' AND p3 = 'manifests', 'Part descriptions',
                                     p2 = 'cas' AND p3 = 'ns', 'Write logs', p2 = 'gc', 'GC state', 'Other') AS kind, sum(data_size) / $interval
FROM {UPLOADS_FROM}
GROUP BY t, kind ORDER BY t
{SETTINGS}""", table="blob_storage_log", unit="Bps", stack=True, interval="5m", w=24)
    verbs = P.metric_ts(
        "S3 requests per second, by type",
        "All requests the cluster sends to the object storage. Cost drivers are PUT and LIST; GET and HEAD are cheap. "
        "Spikes of LIST follow a server restart.",
        {"PUT": P.r(_m("S3PutObject")), "GET": P.r(_m("S3GetObject")), "HEAD": P.r(_m("S3HeadObject")),
         "LIST": P.r(_m("S3ListObjects")), "DELETE": P.r(_m("S3DeleteObjects"))}, unit="reqps", stack=True, w=12)
    throttling = P.metric_ts(
        "S3 throttling events per second",
        "Requests the object storage slowed down (HTTP 429/503). Zero is normal; bursts follow a restart; a steady rate "
        "means the request rate is above the bucket's limit.",
        {"Read throttling": P.r(_m("S3ReadRequestsThrottling")), "Write throttling": P.r(_m("S3WriteRequestsThrottling"))},
        unit="reqps", w=12)
    return [
        P.row("Needs attention", [attention], collapsed=False),
        P.row("Storage and writes", [lease, writes, delay], collapsed=False),
        P.row("Garbage collection", [rounds, duration, stages, backlog], collapsed=False),
        P.row("Objects", [inventory, data_objects, part_descriptions, write_log, uploaded], collapsed=False),
        P.row("S3 requests", [verbs, throttling], collapsed=False),
    ]


def build() -> dict:
    return P.dashboard(
        "cas-overview", "CAS / Overview",
        "Health of content-addressed (CAS) storage for people who run the cluster: is storage online, are writes safe, "
        "is garbage collection keeping up, is anything wrong. Details for support engineers are on CAS / Drilldown.",
        tiles() + rows(), tags=["cas", "clickhouse"],
        links=[{"title": "CAS / Drilldown", "type": "link", "url": DRILLDOWN_LINK["url"], "keepTime": True, "includeVars": True, "icon": "dashboard"},
               {"title": "Operations: monitoring", "type": "link", "url": "https://docs.altinity.com/antalya/cas/operations/monitoring", "targetBlank": True, "icon": "doc"}])
