# CAS Grafana dashboards v1 — design {#cas-grafana-dashboards-design}

Backlog: CAS-289.1 (board), parent CAS-289. Alert rules stay in CAS-289.2.

**Decision.** Two dashboards for Grafana 12.3.0 with the Altinity ClickHouse datasource
(`vertamedia-clickhouse-datasource`): `CAS / Overview` in plain language for end users, and
`CAS / Drilldown` for support and CAS developers, with the GC, write-path and S3 detail as
collapsible rows on one time axis. v1 uses only signals that exist in
`26.6.4.20001.altinityantalya`; every gap becomes a blocker of a new backlog task "CAS Grafana
dashboard v2". Built and verified on the otel.demo stand first; the port to `utils/ca-soak` with
fault injection is a separate task.

## 1. Audience and the placement rule {#placement-rule}

- **Overview** answers "is something wrong, and where". Its reader does not know CAS internals.
- **Drilldown** answers "why". Its reader is Altinity support or a CAS developer.
- A panel goes on the Overview only if all three hold: a reader without CAS knowledge can read
  it; it has a clear good/bad state; a change on it leads to an action. Cache usage, GC phase
  breakdown and per-class S3 counters fail the first two, so they live on the Drilldown.
- One Drilldown rather than one board per subsystem: an incident spans subsystems (S3 throttling →
  slow GC round → ref-lane wait), and reading it on one time axis beats switching boards.
  Collapsed rows run no queries until opened, so the board stays cheap.
- Every Overview block links to the Drilldown, carrying the time range and variables. Grafana has
  no URL parameter that expands a collapsed row, so the link lands on the top of the board; row
  order and the subsystem prefix in row titles (`GC:`, `Writes:`, `S3:`) make the target easy to
  find. If `?viewPanel=<id>` opens a panel inside a collapsed row (to be checked on otel.demo), each
  tile links to the key panel of its row instead.
- Moving a panel between boards is cheap; promote a Drilldown panel to the Overview once otel.demo
  usage shows it is opened on every visit.

## 2. Sources and known traps {#sources-and-traps}

| Source | Grain | Used for |
|---|---|---|
| `system.metric_log` (wide: 183 `ProfileEvent_CAS*`, 9 `CurrentMetric_CAS*`, 41 `ProfileEvent_S3*`) | 1 s per host | every counter panel; rows hold per-second deltas, so sums survive restarts |
| `system.asynchronous_metric_log` (`CASGCIsLeader_<disk>`, `CASGCWedgedNamespaces_<disk>`) | per tick | GC leader timeline, wedged lanes |
| `system.cas_gc_log` | `Start` / `Finish` / `Phase` rows per round | round outcomes, duration, stage counts, per-phase cost |
| `system.cas_log` | one row per writer/GC decision | integrity events, lease and remount timeline |
| `system.cas_mounts` | live view, one backend `listMounts` per query | "mounts now" table only; refresh 5 min |
| `system.blob_storage_log`, `system.part_log`, `system.query_log` | per op / part / query | S3 errors, per-unit S3 cost |

Rules the queries follow, from audit F25 (`docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f25`):

- `S3ReadRequestsErrors` is never used: on a CAS disk it equals `CASBlobHeadMiss` (HEAD-before-PUT misses).
- `S3WriteRequestsErrors` is shown only on the Drilldown with a description that 412/409 rows are
  benign; the error signal is `blob_storage_log.error` minus its `PreconditionFailed` (412) and
  `ConditionalRequestConflict` (409) rows (108 of 112 error rows in a quiet 6 hours on otel.demo), and
  the throttling counters.
- `CASGCPendingReclaim_*` and `CASGCLastSuccessAgeSeconds_*` are never used (CAS-1.3, CAS-1.1).
  Backlog trend comes from `CASGCRetiredCondemned − CASGCRetiredRedeleted`; round age from `cas_gc_log`.
- The `disk` variable excludes `*_cas_cache` wrapper disks, or every `CASGC*` sum doubles.
- Counters are read as sums over windows of 5 minutes or longer, never as per-second gauges
  (`CASRefQueueWaitMicroseconds` and `CASGCRetiredGraduated` spike per second by design).
- The 99 counters that never moved in a week are not given panels; the Drilldown has one
  collapsed row "All CAS counters, nonzero in range" built by a query, so a new counter shows up by itself.

## 3. `CAS / Overview` {#overview}

Layout:

```
[ datasource | cluster | host | disk ]                       default range 6h, refresh 1m
┌───────────┬───────────┬───────────┬───────────┬───────────┬───────────┐
│ Storage   │ Writes    │ GC: last  │ Garbage   │ Integrity │ S3        │
│ online    │ safe      │ success   │ trend     │           │ throttling│
└───────────┴───────────┴───────────┴───────────┴───────────┴───────────┘
▸ Needs attention   (table of recent problem events, plain language)
▸ Storage and writes (lease, committed writes, insert delay from CAS)
▸ Garbage collection (rounds by outcome, duration, stage flow, trend)
▸ S3 requests        (by verb, throttling)
```

### 3.1 Status tiles {#tiles}

Thresholds come from the otel.demo baseline measured on 2026-09-30 (last 24 h unless noted):
rounds every 430 s p50 / 515 s p90; round duration 386 s avg, 214 s p90, 8.8 h max (one round);
`Unresolved / Attempts` = 0.028%; `GaveUp` = 0; lane wait 193 ms per mutation; 3.07 mutations
per flush; throttling 0; `S3ListObjects` 0.3/s; `CASMountLeaseLost` 0; week: 1 `gc_fence_out`,
1 `watermark_renew`, 0 integrity events. Garbage over the week, 10-round windows of
`net / condemned`: p50 0, p90 0.05, p99 0.58 (the hour after a restart); the sign of a single
round's `net` is noise (half of all windows are net-positive), so the tile uses the ratio, not a
count of growing rounds. Real S3 errors (412/409 excluded): 5–33 per day on quiet days, 2,005 on
2026-09-26 (timeouts).

| Tile | Query | Green | Yellow | Red |
|---|---|---|---|---|
| Storage online | `cas_mounts`: hosts with `lifecycle = 'live'` vs all; plus `sum(CASMountLeaseLost)` in range | all live, 0 lost | — | any host not live, or lease lost in range |
| Writes safe | `sum(CASRequestGaveUp)`, `sum(CASConditionalWriteUnresolved) / sum(CASConditionalWriteAttempts)` | gave up 0, unresolved < 0.1% | unresolved ≥ 0.1% | gave up > 0 |
| GC: last success | `now() − max(event_time)` of `cas_gc_log` `Finish` with outcome in (`Success`, `Deferred`), per disk | < 30 min | 30 min – 2 h | > 2 h, or last outcome `Error` |
| Garbage trend | per `Success` round from `cas_gc_log`: `net = entries_condemned − entries_redeleted`; the tile shows `100 · sum(net) / sum(entries_condemned)` over the last 10 rounds and `sum(net)` | growth ≤ 25% | 25–50% | > 50% |
| Integrity | count of `cas_log` rows with `event_type` in (`read_missing`, `dangling_access`, `corrupt_dangle`, `corrupt_decode`, `snap_journal_incoherent`, `exception`) + `sum(anomalies)` from `Finish` rows + max `CASGCWedgedNamespaces_*` + `sum(CASRefNeedsRecovery)` | 0 | `anomalies` > 0 only | any other > 0 |
| S3 throttling | `sum(S3ReadRequestsThrottling + S3WriteRequestsThrottling)` per minute; `blob_storage_log` rows with `error != ''` per hour, excluding `PreconditionFailed` (412) and `ConditionalRequestConflict` (409) | 0 throttling, ≤ 5 errors/h | any throttling, or 5–50 errors/h | > 10 throttling/min, or > 50 errors/h |

Rules for every tile:

- **No data is a grey state, never green.** On otel.demo the system logs sit on the CAS disk; when
  CAS is down the logs stop and the tiles must not stay green. Implemented as Grafana "no value"
  mapping to a grey "no data" text.
- Each tile has a description: what it means, what to do, a link to the Drilldown and to
  `docs/en/antalya/cas/operations/troubleshooting.md`.
- Tiles on `cas_mounts` refresh every 5 minutes (panel-level `interval`), the rest with the board.

### 3.2 Rows under the tiles {#overview-rows}

- **Needs attention** — one table: time, host, what happened, error text. `UNION ALL` of: `cas_log`
  integrity rows; `cas_gc_log` `Finish` rows with outcome in (`Error`, `Aborted`, `Stopped`) with
  `error`; `cas_log` `mount_remount` and `watermark_renew` rows with outcome `failed`; `gc_fence_out`.
  Ordered newest first, limit 100.
- **Storage and writes** — lease renewals: attempts / retries / recovered / lost; conditional
  writes: committed vs unresolved (two series, log scale off); insert delay from CAS:
  `sum(CASRefQueueWaitMicroseconds) / sum(CASRefBatchedMutations)` in 5-minute windows, in ms.
- **Garbage collection** — rounds by outcome (stacked bars per 30 min, one color per outcome);
  round duration (`Success` rounds as points, `Deferred` separately); stage flow per round
  (`entries_condemned`, `entries_graduated`, `entries_redeleted`); net backlog change: running
  sum of `condemned − redeleted` over the range.
- **S3 requests** — requests per second by verb (`S3PutObject`, `S3GetObject`, `S3HeadObject`,
  `S3ListObjects`, `S3DeleteObjects`) summed over hosts; throttling events.

## 4. `CAS / Drilldown` {#drilldown}

One board, rows collapsed by default, in the order of the Overview blocks. Row titles carry the
subsystem prefix. The last row, "All CAS counters, nonzero in range", is one table over every
`ProfileEvent_CAS*` and `CurrentMetric_CAS*` column of `metric_log`.

### 4.1 `GC:` rows {#rows-gc}

| Row | Panels |
|---|---|
| Rounds | state timeline of rounds by outcome per host; GC leader by time (`CASGCIsLeader_<disk>`); round duration; table of last 50 rounds with `error` and `error_code` |
| Stage flow | `entries_condemned` → `entries_graduated` → `entries_redeleted` per round; `objects_spared`, `objects_replaced`, `objects_absent`; `manifests_deleted`; cumulative differences between stages |
| Phases | phase durations stacked per round (`Phase` rows); p50 / p99 per phase table; S3 requests per phase by verb from `Phase.ProfileEvents`; `phase_metrics` |
| Round cost | S3 requests per round by verb (`Finish.ProfileEvents`); requests per deleted object; `CASGCEnumerationPages`, `CASBulkDeleteRequests`; read-ahead `Hit` / `Miss` / `Wasted`; `CurrentMetric_MemoryTracking` on the leader |
| Ref-log cleanup | `CASRootPut` vs `CASRefCleanupObjectsDeleted`; `CASRefSweepDeferred`, `CASRefSweepRearmed`; `CASRefStalePrecommitsReclaimed` |
| Anomalies | `anomalies` per round; `CASGCUnmatchedRemoveDeltas`, `CASGCCondemnMarkerUnconfirmedCarry`, `CASGCMetaWriteAnomaly`, `CASGCHeartbeatFenceOuts`, `CASGCStuckRemovals`, `CASGCNamespaceCleanupLeaks`; table of `gc_fold_clamp` / `gc_anomaly` / `gc_fence_out` from `cas_log` |

Known gap, stated in the Phases row description: S3 requests issued on GC worker pools are not
attributed to the phase that scheduled them; LIST in particular is missing from `Phase` rows (CAS-135).

### 4.2 `Writes:` rows {#rows-writes}

| Row | Panels |
|---|---|
| Mounts now | table from `cas_mounts`: host, disk, `state`, `lifecycle`, time to `expires_at`, `writer_epoch`, `is_leader`, `gc_fenced`; refresh 5 min |
| Lease | renewals: `CASMountRenewalAttempts` / `Retries` / `Resolved` / `Recovered` / `DeadlineExceeded`; `CASMountLeaseLost`; `CASRemountAttempts` / `Succeeded` / `Failed`; table of `watermark_renew` and `mount_remount` rows with `classification`, `attempts_sent`, `elapsed_ms` from `detail` |
| Conditional writes | `CASConditionalWriteAttempts` / `Committed` / `Unresolved` / `DefiniteFailure` / `FenceLostPostWrite`; `CASRequestReissue`, `ConflictPause`, `ResolveRead`, `GaveUp`, `Refused`; `CAS*CompareSwapConflict` by object class |
| Ref lane | wait per mutation; mutations per flush; flushes per second; `CASRefBatchScopeCuts`; `CASRefAppendWedged`, `CASRefNeedsRecovery`, `CASRefAppendSealRejected`, `CASRefAppendUnwedged`; checkpoint `Published` / `IdenticalSkip` / `NotAdvanced`; snapshot `PublishDispatched` / `PublishBackoff`; hot-key lane wait and `CASHotKeyCacheStarts` |
| Blobs and dedup | `CASBlobHeadMiss` (new blobs), `CASBlobPutDeduplicated`, `CASBlobBodyPutAvoided`, `CASBlobAdoptTrusted`; dedup ratio = `PutDeduplicated / (PutDeduplicated + HeadMiss)`; upload pool `CASBlobUploadPoolThreadsActive` / `Scheduled`; fan-out batches and tasks |
| Caches | part-folder view: `Hits` / `Misses` / `Invalidations` / `ValidationMismatches` / `OversizedBypasses`, hit ratio, `CASPartFolderCacheBytes` / `Entries`; manifest decode cache bytes vs the 128 MiB default cap, entries; hot-key cache bytes and entries; `CASRefTableEvictions` |
| Replication | `CASRelinkConfirmRefused*` by reason |

### 4.3 `S3:` rows {#rows-s3}

| Row | Panels |
|---|---|
| By verb | PUT / GET / HEAD / LIST / DELETE per second, per host |
| Who makes requests | object class × verb from `CASBlob*`, `CASManifest*`, `CASRoot*`, `CASGC*`, `CASMeta*`, `CASOther*`; GC share vs writer share |
| Cost estimate | requests × price per 1000 (prices are dashboard variables with AWS us-east-1 defaults), per day |
| Errors and throttling | `S3ReadRequestsThrottling`, `S3WriteRequestsThrottling`; `blob_storage_log` errors by `event_type` and error text; `S3WriteRequestsErrors` with the 412/409 caveat in the description; `CASRequestConnectFailureHint` |
| Cost per unit | S3 requests per INSERT, merge, mutation, fetch (`query_log` by `query_kind`, `part_log` by `event_type`), per GC round (`Finish.ProfileEvents`) |
| Latency | `S3ReadMicroseconds / S3ReadRequestsCount`, `S3WriteMicroseconds / S3WriteRequestsCount` |

## 5. Conventions shared by all boards {#conventions}

| Variable | Values | Source query |
|---|---|---|
| `datasource` | any `vertamedia-clickhouse-datasource` | Grafana datasource variable |
| `cluster` | cluster name | `SELECT DISTINCT cluster FROM system.clusters` |
| `host` | hostnames, multi-select, default all | `SELECT DISTINCT hostname FROM clusterAllReplicas('$cluster', system.cas_gc_log)` |
| `disk` | CAS disks without `*_cas_cache` | `SELECT DISTINCT disk_name FROM clusterAllReplicas('$cluster', system.cas_gc_log) WHERE disk_name NOT LIKE '%_cas_cache'` |

- Every query reads `clusterAllReplicas('$cluster', system.<table>)` with the plugin's `$timeFilter`
  and filters `hostname IN ($host)`; time-series queries bucket with `$timeSeries` and the plugin's
  `$interval`, with a floor of 300 s on counter panels.
- Two annotations on every board: server restart (`asynchronous_metric_log` `Uptime` drop) and
  lease lost (`metric_log` `ProfileEvent_CASMountLeaseLost > 0`).
- Dashboard uids are fixed: `cas-overview`, `cas-drilldown`. Both live in a Grafana folder `CAS`.
- Default refresh 1 min; default range 6 h; the "Mounts now" panels use a panel interval of 5 min.
- Panel titles and descriptions are English. Overview descriptions use no CAS-internal terms.

## 6. Files {#files}

```
utils/cas-grafana/
  README.md                 import steps, required grants (SELECT on system.*, REMOTE), how to run the checks
  dashboards/cas-overview.json
  dashboards/cas-drilldown.json
  check_dashboards.py       offline checks over the JSON (section 7)
  verify_on_grafana.py      runs every panel query through /api/ds/query and reports empty or failing ones
  local.env.example         GRAFANA_URL, CLICKHOUSE_* placeholders; local.env and .netrc are gitignored
docs/en/antalya/cas/operations/monitoring.md   new section "Dashboards": links, one paragraph per Overview tile
```

The JSON files are the source of truth. Boards are edited on otel.demo and exported with
`GET /api/dashboards/uid/<uid>`; the export strips `id`, `version` and `iteration` before commit.
No generator: the checks in section 7 give the guarantees a generator would.

The otel.demo Grafana is preprod: the scripts only create or update the two uids above, never
pass `overwrite` for another uid, and never touch `/api/datasources`.

## 7. Checks {#checks}

`check_dashboards.py` (offline, runs in CI style from the repo root):

- Every `ProfileEvent_*` / `CurrentMetric_*` name in any query exists in
  `src/Common/ProfileEvents.cpp` or `src/Common/CurrentMetrics.cpp`; every `CASGC*_` async name
  matches `ServerAsynchronousMetrics.cpp`.
- Banned names are absent: `S3ReadRequestsErrors`, `DiskS3ReadRequestsErrors`, `CASGCPendingReclaim`,
  `CASGCLastSuccessAgeSeconds`.
- Every panel has a non-empty `description`; every Overview tile has a `links` entry to the Drilldown.
- Every query contains `$timeFilter` and reads through `clusterAllReplicas('$cluster', ...)`; the
  exception list is the `cas_mounts` panels.
- Both files declare the same variable set (section 5) and use `${datasource}`.
- Every Drilldown row title starts with `GC:`, `Writes:` or `S3:`, except the counters row.
- `uid` and `title` match the file name; no `id`.

`verify_on_grafana.py` (against a live Grafana, read-only):

- Executes each panel query for the last 7 days and the last 6 hours; reports errors and empty
  results. Acceptance for v1 on otel.demo: no errors, no empty result over 7 days; empty over 6 h
  is allowed only for event tables that are legitimately empty (integrity, lease events).
- Compares three numbers with the F25 audit figures as a sanity check: `Unresolved / Attempts`,
  mutations per flush, `S3ListObjects` per second.

Red states cannot be produced on otel.demo (0 integrity events, 0 lease losses in 16 days). Their
verification is the `ca-soak` port task: each tile must show red at least once under fault
injection (S3 endpoint killed, GC stopped by `SYSTEM CAS GC STOP`, leader restart).

## 8. v2 gaps and backlog actions {#v2-gaps}

| Panel missing in v1 | Why | Blocker task |
|---|---|---|
| Mount state history (was any host not live at 03:00?) | no lifecycle gauge in `system.metrics`; only event counters exist | new: `CurrentMetrics` `CASDisksLive`, `CASDisksTransientNotLive`, `CASDisksTerminal` counting disks by `PoolLifecycle` (`Pool/CasMountRuntime.h`); cheap, exact at the transition |
| GC state: running / stopped by operator / disabled | not exposed | CAS-1.2 (note added: expose via `system.metrics`, same reasoning) |
| Absolute GC backlog | `pending_reclaim` is process-local and goes negative | CAS-1.3 |
| Round in progress: phase and elapsed | nothing is written before `Finish` | CAS-134 |
| S3 requests per GC phase, complete | worker-pool requests unattributed, LIST lost | CAS-135 |
| S3 failures after all retries | no counter separates retried-and-recovered from failed | CAS-299 |
| Pool size and reclaimed bytes | no bytes per object class anywhere | CAS-11 |

Backlog actions after this spec is approved (via the `backlog` CLI, per `docs/superpowers/cas/backlog`):

1. CAS-289.1: link this spec; its acceptance criteria stay.
2. New task "CAS Grafana dashboard v2" under CAS-289: the table above as its description;
   `dependencies` = the blocker ids.
3. Each existing blocker (CAS-1.2, CAS-1.3, CAS-134, CAS-135, CAS-299, CAS-11) gets a note
   "blocks CAS Grafana dashboard v2 (<id>)".
4. New task for the lifecycle `CurrentMetrics`; new task "Port the CAS dashboards to
   `utils/ca-soak` and make every Overview tile go red under fault injection".

## 9. Out of scope {#out-of-scope}

- Alert rules (CAS-289.2) — the tile thresholds are chosen so they can become rules later.
- Grafana 12 dynamic-dashboard features (tabs, conditional panels): experimental behind the
  `dashboardNewLayouts` toggle in 12.3 and off on the otel.demo instance; boards use the classic
  JSON model with rows and links.
- Per-table views; the `disk` grain is the finest in v1.
- Any change to what the server records, beyond the backlog tasks in section 8.

## 10. Order of work {#order}

1. `CAS / Overview` on otel.demo, complete with descriptions, links and both scripts.
2. `CAS / Drilldown`: `GC:` rows, then `Writes:` rows, then `S3:` rows, then the counters row.
3. `monitoring.md` section; backlog actions of section 8.
