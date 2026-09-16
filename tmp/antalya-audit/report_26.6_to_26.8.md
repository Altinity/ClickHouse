# Antalya 26.6 -> 26.8 functionality audit

Scope: the 70 Altinity-branch PRs merged into `antalya-26.6` (delta over `antalya-26.5`).
`antalya-26.8` was not built by continuing `antalya-26.6`'s history (it forked
independently from the same `antalya-26.5` base onto a newer upstream ClickHouse
version - `antalya-26.6` is not an ancestor of `antalya-26.8`), so verification was done
by comparing actual code/functionality, not commit ancestry or merged-PR search.

Method: a text-diff heuristic (`tmp/antalya-audit/verify_in_268.py`) flagged 48/70 PRs
as low-overlap; each flagged PR was then individually re-verified by reading its full
diff and searching `antalya-26.8`'s current source for the same functionality (possibly
under a different name/location), to separate real gaps from refactor-driven false
positives. 22/70 were high-confidence present from the heuristic alone (>=90% of added
lines still found verbatim) and were not individually re-verified.

## Fully missing functionality (confirmed)

### EXPORT PARTITION / EXPORT PART feature - entirely absent
The whole feature (SQL `EXPORT PARTITION`/`EXPORT PART`, `ExportPartTask`,
`ExportPartitionTaskScheduler`, `ExportReplicatedMergeTreePartitionManifest`,
`system.exports` / `system.replicated_partition_exports`, ~15 settings, docs) does not
exist anywhere in `antalya-26.8` - confirmed by exhaustive `git grep`/`git ls-tree`
searches turning up zero hits. Every PR that built or extended it is missing:

- [#2146](https://github.com/Altinity/ClickHouse/pull/2146) - Partition export + cluster functions (the ~300-file PR that introduced the whole feature)
- [#2200](https://github.com/Altinity/ClickHouse/pull/2200) - Fix `rescheduleTasksFromReplica`
- [#2201](https://github.com/Altinity/ClickHouse/pull/2201) - Fix file identifier in `rescheduleTasksFromReplica`
- [#2209](https://github.com/Altinity/ClickHouse/pull/2209) - Export task timeout 1h -> 24h
- [#2210](https://github.com/Altinity/ClickHouse/pull/2210) - Commit info in partition exports table
- [#2218](https://github.com/Altinity/ClickHouse/pull/2218) - Check name/position of partition-key columns
- [#2229](https://github.com/Altinity/ClickHouse/pull/2229) - Position matching + extra source columns
- [#2253](https://github.com/Altinity/ClickHouse/pull/2253) - Non-matching partition expressions when destination doesn't split data
- [#2284](https://github.com/Altinity/ClickHouse/pull/2284) - Non-matching schema, export by name
- [#2293](https://github.com/Altinity/ClickHouse/pull/2293) - Fix `SettingsChangesHistory.cpp` version tags for export settings (moot - the settings it retags don't exist in 26.8)
- [#2126](https://github.com/Altinity/ClickHouse/pull/2126) *(qa)* - regression test case for related S3 encoding fix, also absent

### Iceberg deletion vectors / puffin files - entirely absent
- [#2183](https://github.com/Altinity/ClickHouse/pull/2183) - Puffin-based Iceberg deletion vector reader (`IcebergDeletionVector`, `PuffinDeletionVectorReader`) - none of it exists; `PositionDeleteTransform.cpp` still hard-throws for non-parquet delete files
- [#2271](https://github.com/Altinity/ClickHouse/pull/2271) - Databricks delta_bin deletion vectors (extends #2183's infra, which is absent)

### Other entirely-absent Iceberg/DataLake features
- [#2154](https://github.com/Altinity/ClickHouse/pull/2154) - Iceberg data files on a secondary/external object storage
- [#2125](https://github.com/Altinity/ClickHouse/pull/2125) - `TRUNCATE TABLE` support for Iceberg (REST catalog) - 26.8 still unconditionally throws `NOT_IMPLEMENTED`
- [#2144](https://github.com/Altinity/ClickHouse/pull/2144) - Hybrid tables (`HybridSegmentPruner`, `system.hybrid_watermarks`)
- [#2145](https://github.com/Altinity/ClickHouse/pull/2145) - Cluster request improvements + Iceberg/Parquet read fixes (task-reschedule-on-lost-replica, related settings/profile events)
- [#2235](https://github.com/Altinity/ClickHouse/pull/2235) - Parquet v3 read concurrency (separate prefetch/decode thread budgeting)
- [#2141](https://github.com/Altinity/ClickHouse/pull/2141) - Expose Iceberg `partition_key`/`sorting_key` in `system.tables`
- [#2157](https://github.com/Altinity/ClickHouse/pull/2157) - Idempotency/retry-safety fixes for Iceberg REST-catalog schema/column ALTERs
- [#2156](https://github.com/Altinity/ClickHouse/pull/2156) - Cache vended credentials for REST catalogs (still re-fetched on every use in 26.8)
- [#2124](https://github.com/Altinity/ClickHouse/pull/2124) - Guard against empty-name subnamespaces from catalog
- [#2040](https://github.com/Altinity/ClickHouse/pull/2040) - Enable experimental datalake catalogs (`allow_experimental_database_{iceberg,unity_catalog,glue_catalog}`) by default - 26.8 still defaults them to `false`

### Auth / cluster - entirely absent
- [#2140](https://github.com/Altinity/ClickHouse/pull/2140) - Server-side token authentication/authorization + OAuth client login (`TokenAccessStorage`, JWKS/JWT validation, `clickhouse-client` OAuth flow)
- [#2197](https://github.com/Altinity/ClickHouse/pull/2197) - Reload cluster-discovery settings from `remote_servers` without restart
- [#2249](https://github.com/Altinity/ClickHouse/pull/2249) - Fix JOIN filter pushdown through rename (`canPrefilterJoinSide`, cluster listing-filter merging)

### Experimental CAS feature - entirely absent
- [#2159](https://github.com/Altinity/ClickHouse/pull/2159) - Content-addressed storage over shared object storage (~614 files / ~171k lines). Only two dead, unreferenced job-definition stubs remain in `ci/defs/altinity_jobs.py`; the actual implementation lives on separate branches that were never merged into `antalya-26.8`.

### Test-only gaps (no functional/user-facing risk, but coverage is thinner)
- [#2126](https://github.com/Altinity/ClickHouse/pull/2126) - S3-encoding-with-slash Iceberg partition-value test case not present (already listed above)
- [#2268](https://github.com/Altinity/ClickHouse/pull/2268) - `test_num_rows_cache_no_collision_across_buckets` hardening - the whole test file/feature area was reorganized away in 26.8; no equivalent found

## Partially missing functionality (confirmed)

- [#2129](https://github.com/Altinity/ClickHouse/pull/2129) *(dev)* - Iceberg `time` type: write-path mapping for `Time` exists, but `Time64` write support and the entire read path (`getSimpleType` still returns `Int64`, no Avro `TIME_MILLIS/TIME_MICROS` handling) are missing.
- [#2184](https://github.com/Altinity/ClickHouse/pull/2184) *(dev)* - S3 Tables Iceberg catalog: core catalog support (`S3TablesCatalog`, AWS SigV4) is present, but the catalog profile events and dedicated credential-refresh path, plus the `allow_experimental_database_s3_tables` gating setting, are missing.
- [#2222](https://github.com/Altinity/ClickHouse/pull/2222) *(dev)* - Datalake catalog auth: underlying token-cache/401-retry logic it hardens is present, but the actual deliverable (4 new profile events + `used_cached_oauth_token` tracking) is absent.
- [#2038](https://github.com/Altinity/ClickHouse/pull/2038) *(dev)* - The substantive PostgreSQL-protocol fix (`VERSION_STRING_WITHOUT_FLAVOUR` so `server_version` isn't corrupted by the Antalya build suffix) is present, but the `ClickHouseVersion` suffix-parsing support and the "tag Antalya settings under a suffixed version key" convention it introduced were not carried forward - 26.8 files Antalya settings under the plain mainline version instead.
- [#2160](https://github.com/Altinity/ClickHouse/pull/2160) *(cicd)* - Several stability fixes carried over, but specific ones weren't: `max_estimated_execution_time` stayed at 600 (not bumped to 900), the TPC-DS `no-parallel` tag and `00093_prewhere_array_join.sql`'s `no-random-settings` tag are missing - these specific flakiness regressions could resurface.
- [#2196](https://github.com/Altinity/ClickHouse/pull/2196) *(qa)* - Bundle of ~15 flaky-test fixes; all but one are present (verbatim or via an equivalent fix). Missing: the `SYSTEM CANCEL VIEW` exception test case in `04105_system_pause_view.sh`.

## Confirmed present despite low text-overlap (heuristic false positives)

`#2320` (Iceberg decimal write support), `#2013`, `#2022`, `#2135`, `#2142`, `#2223`,
`#2234`, `#2277` (all CI/CD fixes - mechanism changed, intent preserved), `#2256`
(sqllogic memory fix), `#2246` (alter-with-custom-disk fix).

## Not applicable / moot

- [#2255](https://github.com/Altinity/ClickHouse/pull/2255) - fixes flaky races in EXPORT PART tests; moot, since the whole EXPORT PART feature is absent (see above).
- [#2261](https://github.com/Altinity/ClickHouse/pull/2261) - fixes a `test_s3_cluster` scenario (`hidden_clusters.xml`, second cluster instance) that itself was never added to `antalya-26.8`; nothing to regress.
- [#2039](https://github.com/Altinity/ClickHouse/pull/2039) - design/skill markdown docs only, no code; not ported but has no runtime impact.
- [#2148](https://github.com/Altinity/ClickHouse/pull/2148) - Parallel Parquet-file reads in `StorageFile`; absent from 26.8, but this PR was itself reverted within `antalya-26.6` by `#2217`, so its absence is consistent, not a regression.

## Not individually re-verified (high heuristic confidence, >=90% coverage)

`#2014, #2056, #2061, #2071, #2098, #2107(n/a, version bump), #2155, #2179, #2189(n/a,
version bump), #2198, #2199, #2208, #2217, #2248, #2252, #2257, #2260, #2263, #2269,
#2276, #2280` - trusted present based on the text-diff heuristic alone.
