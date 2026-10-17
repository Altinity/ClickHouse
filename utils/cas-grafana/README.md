# CAS Grafana dashboards

Two dashboards for Grafana 12 with the Altinity ClickHouse datasource (`vertamedia-clickhouse-datasource`, tested with 3.4.12):

| File | Dashboard | For |
|---|---|---|
| `dashboards/cas-overview.json` | `CAS / Overview` | people who run the cluster: six status tiles, plain language |
| `dashboards/cas-drilldown.json` | `CAS / Drilldown` | support and CAS developers: GC, write path and S3 detail as collapsed rows |

## Import {#import}

Grafana → Dashboards → New → Import → upload the JSON, pick the ClickHouse datasource. Both boards share the variables `datasource`, `cluster`, `host`, `disk`; `disk` lists CAS disks from `system.cas_gc_log`.

The datasource user needs:

```sql
GRANT SELECT ON system.* TO grafana;
GRANT REMOTE ON *.* TO grafana;   -- every query reads through clusterAllReplicas('<cluster>', system.<table>)
```

Retention: the boards read `metric_log`, `asynchronous_metric_log`, `cas_gc_log`, `cas_log`, `blob_storage_log`, `part_log`, `query_log`. Keep at least 7 days for the tiles' baselines.

## Editing {#editing}

The Python definitions are the source of truth (`casgrafana/overview.py`, `casgrafana/drilldown.py`); the JSON is built from them.

```bash
cd utils/cas-grafana
python3 -m unittest discover -s tests -t .      # unit tests
python3 build_dashboards.py                     # writes dashboards/*.json
python3 check_dashboards.py                     # offline checks (metric names exist, banned signals absent, descriptions, links)
python3 push_dashboards.py --message "why"      # uploads the two owned uids (cas-overview, cas-drilldown) to the Grafana in local.env
python3 verify_on_grafana.py --range 7d         # runs every panel query through /api/ds/query and reports errors and empty results
python3 export_dashboards.py                    # downloads what Grafana stores into dashboards/*.exported.json for diffing
```

`local.env` (gitignored; copy `local.env.example`) holds `GRAFANA_URL`, `GRAFANA_DS_UID`, `GRAFANA_CLUSTER` and, if the Grafana needs auth, `GRAFANA_TOKEN` or `GRAFANA_NETRC`. The scripts never print these values.

Edits made in the Grafana UI are experiments: port them into `casgrafana/overview.py` or `casgrafana/drilldown.py`, rebuild, push.

## Rules the checks enforce {#rules}

- Every `ProfileEvent_*` / `CurrentMetric_*` name exists in `src/Common/ProfileEvents.cpp` or `src/Common/CurrentMetrics.cpp`.
- Never used: `S3ReadRequestsErrors` (equals `CASBlobHeadMiss` on a CAS disk), `CASGCPendingReclaim_*` (goes negative after a restart), `CASGCLastSuccessAgeSeconds_*` (0 means both "just now" and "never").
- Every query reads through `clusterAllReplicas('$cluster', …)`, uses `$timeFilter` (except `/* now */` queries and `system.cas_mounts`) and ends with `SETTINGS skip_unavailable_shards = 1`.
- Every panel has a description; Overview descriptions use no CAS-internal terms; every Overview tile links to the Drilldown.
- Tile sums are NULL-guarded so an empty range shows grey "no data", not green.
- Table-format queries carry time as `toUInt64(event_time) * 1000` (rendered with the `dateTimeAsIso` unit): the plugin backend pivots any `DateTime` column into a time series and returns nothing for a table of strings.

## Plugin behavior worth knowing {#plugin-behavior}

- `/api/ds/query` drops rows outside the request's `from`/`to` when the result has a `DateTime` column; `verify_on_grafana.py` sets the range to its window.
- GC round and phase rows in `system.cas_gc_log` carry `CAS<Class><Verb>` counters for every verb, but their raw `S3ListObjects` / `S3PutObject` keys stay 0 (writes run on pool threads). The per-round panels sum the CAS-semantic keys.
