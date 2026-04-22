Feature Design: `ALTER TABLE EXPORT PART` and `ALTER TABLE EXPORT PARTITION`
============================================================================

**Status:** draft
**Author(s):** Arthur Passos
**Related issues/PRs:**
https://github.com/Altinity/ClickHouse/pull/1618

**Last updated:** 2026-04-21

---

## 1. Requirements

### Motivation

Cost of storing data is a growing problem for large analytic systems
that use open source ClickHouse and replicated block storage. The core
problem is that block storage is (a) expensive and (b) replication makes
multiple copies.  Project Antalya solves the storage cost problem using
the Hybrid Table Engine.  Hybrid tables allow users to split tables
into segments, placing hot data on replicated block storage and
cold data on shared Iceberg tables using Parquet data files.

The hybrid table approach requires a robust mechanism to export table
data from MergeTree tables to Iceberg. The mechanism must be fast,
use machine resources efficiently, handle failures automatically, and
be easy to monitor. This design covers two new ClickHouse commands to 
export data.

* `ALTER TABLE EXPORT PART` -- Exports a single part to Iceberg

* `ALTER TABLE EXPORT PARTITION` -- Exports one or more partitions to Iceberg

These commands replace `INSERT INTO ... SELECT FROM` pipelines that select
rows and write them out to one or more Parquet files. This approach
costs an extra decode/sort pass per export, does not coordinate across
replicas, and does not take advantage of existing partitioning and
sorting in MergeTree. `EXPORT PART` / `EXPORT PARTITION` write parts
directly to object storage from the source replica(s), preserving the
source sort order and cutting out the `SELECT` pipeline.

### Requirements

1. **SQL only.** All operations related to export are available in SQL.
   There should be no need to use non-SQL tools or directly access
   storage to run exports or clean up problems.

2. **Efficient, order-preserving writes.** Write a specified `MergeTree`
   part (or every part of a specified partition) to an object-storage
   destination in Parquet, preserving the source part's sort order,
   without using a `SELECT` pass and also minimizing the RAM required
   to hold data during transfer.

3. **Output file management.** Allow users to break exported parts
   into smaller Parquet files, which helps ensure good performance
   when scanning Iceberg data.

4. **Data type equivalence.** Map ClickHouse types to Iceberg types
   that cast back without data loss to the original ClickHouse types
   when selecting data. Applications that access exported data through
   a Hybrid table should be able to read the data back from Iceberg
   without requiring changes.

5. **Atomic transfer.** Readers should never see a partial export in
   the target Iceberg table. Exported part(s) and partition(s) should
   be visible in their entirety in Iceberg or not at all.

6. **Distributed operation.**
   - `EXPORT PART` always runs locally on the ClickHouse host where
     it is invoked.
   - `EXPORT PARTITION` from non-replicated `MergeTree` tables runs
     locally on the ClickHouse host where it is invoked.
   - `EXPORT PARTITION` is cluster-coordinated on `Replicated*MergeTree`
     tables: any replica that has a given part contributes to the
     export; the task is persistent and resumes after restarts.

7. **Observability.** 
   It must be possible for users to track the following from system tables: 
   - Export part request status.
   - Export partition request status.
   - History of exported parts including whether export succeeded or failed. 
   - Relevant profile events related to export. 

8. **Error recovery.**
   - **Idempotence.** Re-issuing the same export to the same
     destination is a no-op while the export is running. (There should
     be a way to track 'recent' exports so that they are idempotent as
     well.)
   - **Clean-up.** There must be a procedure to clean up a failed
     export using only SQL commands.
   - **Automatic restart.** `EXPORT PARTITION` task is persistent and
     resumes after restarts.

9. **Killable.** It must be possible to terminate any `ALTER TABLE EXPORT` command. 
     The command should be idempotent and must throw a clear exception on failure
     rather than hanging. 

### Non-requirements

- Non-Parquet output formats. Only `Parquet` is targeted in this iteration.
- `partition_strategy = 'wildcard'` destinations — only `'hive'` is supported; others throw
  `NOT_IMPLEMENTED`.
- Exporting to arbitrary table functions. Only those backed by an object-storage engine that
  supports exports (e.g. `s3`, `azure`) are valid; others throw `NOT_IMPLEMENTED`.
- Schema evolution between source and destination. Schemas must match (`INCOMPATIBLE_COLUMNS`
  otherwise); destination cannot have a column that matches a source `EPHEMERAL`.
- Any read/query path over exported files — consumption happens via normal `S3` / `s3` /
  external-engine reads.
- Synchronous exports. Both commands return immediately; completion is polled via system tables.
- Importing parts back from object storage (that is tracked separately).

### Constraints

- Experimental gate: `allow_experimental_export_merge_tree_part` (query-level) for `EXPORT PART`;
  `enable_experimental_export_merge_tree_partition_feature` (server-level) for `EXPORT PARTITION`.
- `EXPORT PARTITION` requires a ZooKeeper / `clickhouse-keeper` ensemble with the `multi_read`
  feature flag.
- Destination table must use `partition_strategy = 'hive'`.
- No change to `MergeTree` on-disk part format; only the Keeper schema under the table's
  replication path is extended.

### References

- `docs/en/engines/table-engines/mergetree-family/part_export.md`
- `docs/en/engines/table-engines/mergetree-family/partition_export.md`
- `tests/queries/0_stateless/03572_export_merge_tree_part_basic.sh`
- `tests/queries/0_stateless/03572_export_merge_tree_part_to_object_storage_simple.sql`
- `tests/queries/0_stateless/03572_export_merge_tree_part_limits_and_table_functions.sh`
- `tests/queries/0_stateless/03572_export_merge_tree_part_special_columns.sh`
- `tests/queries/0_stateless/03572_export_replicated_merge_tree_part_to_object_storage.sh`
- `tests/queries/0_stateless/03572_export_replicated_merge_tree_part_to_object_storage_simple.sql`
- `tests/queries/0_stateless/03604_export_merge_tree_partition.sh`
- `tests/queries/0_stateless/03608_export_merge_tree_part_filename_pattern.sh`
- `tests/integration/test_export_merge_tree_part_to_object_storage/test.py`
- `tests/integration/test_export_replicated_mt_partition_to_object_storage/test.py`

---

## 2. Functional specification

### User-facing behavior
A user points `ALTER TABLE` at a `MergeTree` source and an object-storage destination (table or
table function). The command returns immediately with no rows. The export runs in the background;
progress lives in `system.exports` and, for partition exports, `system.replicated_partition_exports`.
Successful exports append to `system.part_log` with `event_type = 'ExportPart'`. Writers to
object storage drop one Parquet data file per part (or per chunk when split by size/rows) plus
one commit file per transaction.

### SQL syntax / API

```sql
-- Export a single part to a destination table
ALTER TABLE [db.]table
    EXPORT PART 'part_name'
    TO TABLE [dest_db.]dest_table
    [SETTINGS ...];

-- Export a single part to a destination table function
ALTER TABLE [db.]table
    EXPORT PART 'part_name'
    TO TABLE FUNCTION s3(...) PARTITION BY <expr>
    [SETTINGS ...];

-- Export every active part of a partition (Replicated*MergeTree only)
ALTER TABLE [db.]table
    EXPORT PARTITION ID 'partition_id'
    TO TABLE [dest_db.]dest_table
    [SETTINGS ...];

-- Cancel one or more partition exports
KILL EXPORT PARTITION WHERE <predicate on system.replicated_partition_exports>;
```

### Individual command examples
These are derived from `tests/queries/0_stateless/03572_*` and `03604_export_merge_tree_partition.sh`.

```sql
-- Part export to S3 table
ALTER TABLE mt_table EXPORT PART '2020_1_1_0' TO TABLE s3_table
SETTINGS allow_experimental_export_merge_tree_part = 1;

-- Part export to S3 table function (schema inferred from source)
ALTER TABLE mt_table EXPORT PART '2020_1_1_0'
TO TABLE FUNCTION s3(s3_conn, filename='tf', format='Parquet', partition_strategy='hive')
PARTITION BY year
SETTINGS allow_experimental_export_merge_tree_part = 1;

-- Split large part across multiple Parquet files
ALTER TABLE big EXPORT PART '2025_0_32_3' TO TABLE big_dest
SETTINGS allow_experimental_export_merge_tree_part = 1,
         export_merge_tree_part_max_bytes_per_file = 10000000,
         output_format_parquet_row_group_size_bytes = 5000000;

-- Partition export across a Replicated cluster
ALTER TABLE rmt_table EXPORT PARTITION ID '2020' TO TABLE s3_table;

-- Cancel by filter
KILL EXPORT PARTITION
WHERE partition_id = '2020'
  AND source_table = 'rmt_table'
  AND destination_table = 's3_table';
```

### End-to-end example

Create a `ReplicatedMergeTree` source, seed two partitions from
`system.numbers`, create a hive-partitioned S3 destination (the on-disk
shape an external Iceberg catalog such as Glue / REST / Nessie /
Lakekeeper registers as an Iceberg table), export one partition,
and verify:

```sql
-- 1. Source table: Replicated MergeTree partitioned by year.
CREATE TABLE events
(
    id    UInt64,
    ts    DateTime,
    year  UInt16
)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/events', 'r1')
PARTITION BY year
ORDER BY (year, id);

-- 2. Seed two partitions (2024, 2025) straight from `system.numbers`.
INSERT INTO events
SELECT
    number                                                 AS id,
    toDateTime('2024-01-01 00:00:00') + INTERVAL number SECOND AS ts,
    2024                                                   AS year
FROM system.numbers
LIMIT 1000000;

INSERT INTO events
SELECT
    number                                                 AS id,
    toDateTime('2025-01-01 00:00:00') + INTERVAL number SECOND AS ts,
    2025                                                   AS year
FROM system.numbers
LIMIT 1000000;

-- 3. Destination: S3 with hive partition layout. This is the on-disk shape that
--    an Iceberg catalog registers as a table; the `EXPORT PARTITION` command
--    itself does not touch any catalog.
CREATE TABLE events_iceberg
(
    id    UInt64,
    ts    DateTime,
    year  UInt16
)
ENGINE = S3(s3_conn, filename='warehouse/events', format = Parquet, partition_strategy = 'hive')
PARTITION BY year;

-- 4. Export the 2024 partition. Returns immediately; runs in the background.
ALTER TABLE events EXPORT PARTITION ID '2024' TO TABLE events_iceberg;

-- 5. Watch progress (Keeper round-trip — use sparingly).
SELECT status, parts_count, parts_to_do, last_exception
FROM system.replicated_partition_exports
WHERE source_table = 'events' AND partition_id = '2024';

-- 6. When status = 'COMPLETED', the destination bucket contains:
--      warehouse/events/year=2024/<part_name>_<checksum>.1.parquet    (one per part)
--      warehouse/events/commit_2024_<tx_id>                           (atomicity manifest)
--    Readers that filter by commit see either the full partition or nothing.
SELECT count() FROM events_iceberg WHERE year = 2024;

-- 7. Or inspect the layout directly.
SELECT _path
FROM s3(s3_conn, filename = 'warehouse/events/year=2024/**', format = 'One')
ORDER BY _path;
```

The same flow works for a non-replicated `MergeTree` source — coordination collapses to the
local node, but the command, destination shape, and observability surfaces are identical.

### Operational notes

The following notes expand on expected behavior of commands. 

1. `ALTER TABLE t EXPORT PART 'p' TO TABLE s3_t` writes
   `<dir>/<partition>/<part>_<checksum>.1.parquet` plus
   `<dir>/commit_<part>_<checksum>`, readable end-to-end via
   `SELECT * FROM s3(...)` in tests `03572_*` and `03608_*`.

2. `ALTER TABLE rmt EXPORT PARTITION ID 'p' TO TABLE s3_t` exports
   every active part of partition `p` across all replicas that host
   it; `system.replicated_partition_exports` converges to `COMPLETED`.

3. Re-issuing the same `EXPORT PARTITION` within
   `export_merge_tree_partition_manifest_ttl` is a no-op (no
   duplicate files) unless `export_merge_tree_partition_force_export = 1`.

4. Killing an in-flight partition export via `KILL EXPORT PARTITION`
   transitions status to `KILLED` and stops all replicas' contributions.

5. Exception during part export is counted in `PartsExportFailures`;
   retry behavior honors `export_merge_tree_partition_max_retries`.

### Settings

| Setting | Scope | Default | Range / Values | Applies to | Description |
| --- | --- | --- | --- | --- | --- |
| `allow_experimental_export_merge_tree_part` | query | `false` | `Bool` | `EXPORT PART` | Experimental gate; required. |
| `enable_experimental_export_merge_tree_partition_feature` | server | `false` | `Bool` | `EXPORT PARTITION` | Experimental gate; required. |
| `export_merge_tree_part_overwrite_file_if_exists` | query | `false` | `Bool` | `EXPORT PART` | Overwrite existing destination file; otherwise throws. |
| `export_merge_tree_part_max_bytes_per_file` | query | `0` | `UInt64` (`0`=unlimited) | both | Soft cap per output file. Non-zero values can break idempotency. |
| `export_merge_tree_part_max_rows_per_file` | query | `0` | `UInt64` (`0`=unlimited) | both | Soft cap per output file. Non-zero values can break idempotency. |
| `export_merge_tree_part_throw_on_pending_mutations` | query | `true` | `Bool` | both | Refuse to export parts with pending mutations (unless mutation was `IN PARTITION`). |
| `export_merge_tree_part_throw_on_pending_patch_parts` | query | `true` | `Bool` | both | Refuse to export parts with pending patch parts. |
| `export_merge_tree_part_filename_pattern` | query | `{part_name}_{checksum}` | `String` | both | Filename template; supports `{part_name}`, `{checksum}`, `{database}`, `{table}`, server macros. |
| `export_merge_tree_partition_force_export` | query | `false` | `Bool` | `EXPORT PARTITION` | Overwrite a live Keeper manifest for the same `(source, destination, partition_id)`. |
| `export_merge_tree_partition_max_retries` | query | `3` | `UInt64` | `EXPORT PARTITION` | Per-part retry budget before the partition export fails. |
| `export_merge_tree_partition_manifest_ttl` | query | `180` (seconds) | `UInt64` | `EXPORT PARTITION` | Live-manifest TTL; acts as the idempotency window. Does not interrupt in-flight tasks. |
| `export_merge_tree_part_file_already_exists_policy` | query | `skip` | `skip` / `error` / `overwrite` | `EXPORT PARTITION` | Per-file policy during partition export. |

Default-value impact: all new settings default to "off" or to conservative
values (pending-mutation guards default to throwing). No existing query
behavior changes unless a user opts in.

### System tables / metrics / log messages / observability

- `system.exports` — rows for currently-executing part exports (source/destination tables,
  `part_name`, destination paths, `elapsed`, rows/bytes counters, memory counters). Dropped when
  the export completes.
- `system.replicated_partition_exports` — rows for `EXPORT PARTITION` tasks. Backed by Keeper;
  querying it is a Keeper round-trip and should be used sparingly. Columns include
  `source_database`, `source_table`, `destination_database`, `destination_table`, `create_time`,
  `partition_id`, `transaction_id`, `source_replica`, `parts`, `parts_count`, `parts_to_do`,
  `status`, `exception_replica`, `last_exception`, `exception_part`, `exception_count`.
- `system.part_log` — each completed part export appends one row with `event_type = 'ExportPart'`,
  filled `remote_file_paths`, `merged_from = [part_name]`, plus standard timing / size / error
  fields.
- `ProfileEvents`:
  - `PartsExports` — successful part-export completions.
  - `PartsExportFailures` — failures.
  - `PartsExportDuplicated` — skipped because destination file already existed.
  - `PartsExportTotalMilliseconds` — cumulative wall time.
- Status enum on `system.replicated_partition_exports`: `PENDING`, `COMPLETED`, `FAILED`, `KILLED`.

### Error behavior

- Missing experimental flag: `SUPPORT_IS_DISABLED` (exact code TBD — confirm against
  `src/Common/ErrorCodes.cpp`).
- Destination schema mismatch (columns / types / order; source `EPHEMERAL` column present in
  destination): `INCOMPATIBLE_COLUMNS`.
- Destination engine doesn't support exports (e.g. `url`, non-hive `partition_strategy`, unknown
  engine): `NOT_IMPLEMENTED`.
- Destination is an unknown table function: `UNKNOWN_FUNCTION`.
- Pending mutations or patch parts when the guard is enabled: `BAD_ARGUMENTS` (TBD — confirm).
- Part not found on any replica: `NO_SUCH_DATA_PART` (TBD — confirm).
- Destination file already exists and policy is `error` / `export_merge_tree_part_overwrite_file_if_exists = 0`:
  `FILE_ALREADY_EXISTS` (TBD — confirm).
- Duplicate live manifest without `..._force_export`: `DUPLICATE_EXPORT_TASK` or equivalent
  (TBD — confirm).

All of the above **throw exceptions**; they do not crash the server.

### Backward compatibility
- **Older client → newer server:** harmless — the client issues the new `ALTER` text; the server
  parses it. No wire-protocol change.
- **Newer client → older server:** older server fails parse on `EXPORT PART` / `EXPORT PARTITION`
  with `SYNTAX_ERROR`. Acceptable.
- **Mixed-version cluster (replication):** `EXPORT PART` is local to one replica; no cross-replica
  effect. `EXPORT PARTITION` stores its manifest under the table's Keeper path in a new
  subtree; replicas on older versions ignore unknown nodes but will NOT contribute parts to the
  export — the initiating replica (which must be on the new version) completes alone if it holds
  all the parts; otherwise the task stalls on `parts_to_do > 0`. An upgrade-ordering note for
  operators is required (section 4 / Rollout).
- **On-disk format:** unchanged. Parts are read as-is; Parquet is produced on the fly.
- **Default-value changes:** none that affect existing workloads (see settings table).

---

## 3. Implementation

### Architecture overview
Parser adds two new `ASTAlterCommand` variants (`EXPORT_PART`, `EXPORT_PARTITION`) plus
`KILL EXPORT PARTITION`. The interpreter side routes `EXPORT PART` into a new per-part export
task scheduled on the background export pool; `EXPORT PARTITION` is routed through Keeper for
coordination and expands into N per-part tasks across the replicas that host each part.

Part-level pipeline: reuse the existing Parquet output stack (`StorageS3Sink` / Parquet writer)
fed by a source that streams a single part in primary-key order, with no post-read sort or merge
pass.

Partition coordination: Keeper nodes under `<table_zk>/partition_exports/<tx_id>/` hold the
manifest (parts list, policy), per-part assignment, per-replica progress, and the kill flag.
Each replica watches `partition_exports` and picks up parts it holds locally.

### Key design decisions

1. **Separate AST nodes for each command.** `EXPORT PART` and `EXPORT PARTITION` get distinct
   `ASTAlterCommand::Type` variants; `KILL EXPORT PARTITION` is its own `ASTKillExportPartitionQuery`.
   The part primitive and the cluster-coordinated partition command do materially different work
   and should not share a single code path.

2. **Reuse the existing object-storage sink.** Export rides on the destination engine's existing
   Parquet writer (`StorageS3` / `StorageAzureBlob` sink). No new encoder. The sink is extended
   to accept an already-ordered stream and a target filename derived from
   `export_merge_tree_part_filename_pattern`.

3. **Stream parts in primary-key order, no re-`SELECT`.** The per-part reader walks the part in
   its on-disk order and feeds the Parquet writer directly, skipping the analyzer / planner /
   executor decode and sort path that `INSERT INTO ... SELECT` would take. This is the central
   performance claim.

4. **Coordinate partition exports via a dedicated Keeper subtree.** New path
   `<table_zk>/partition_exports/<tx_id>/` holds the manifest, per-part assignment, per-replica
   progress, and the kill flag — separate from the replication log. The replication log is for
   data mutations that must apply on every replica; partition exports are a distributed
   side-effect whose assignment depends on which replica holds which part, so they warrant their
   own subtree.

5. **Atomicity via commit files.** Each transaction emits one `commit_<part>_<checksum>` (part
   level) or `commit_<partition_id>_<tx_id>` (partition level) file that lists every data file
   written. Readers wanting atomicity filter by commit; this avoids on-target renames or
   multipart-transaction protocols on object storage.

6. **Async model with three observability surfaces.** Commands return immediately. In-flight
   progress lives in `system.exports` (local, dropped on completion);
   `system.replicated_partition_exports` (Keeper-backed — querying is a Keeper round-trip, use
   sparingly); and `system.part_log` gains an `ExportPart` `event_type` for completed per-part
   exports. Four `ProfileEvents` (`PartsExports`, `PartsExportFailures`, `PartsExportDuplicated`,
   `PartsExportTotalMilliseconds`) expose aggregate counters.

7. **Idempotency enforced in Keeper.** Duplicate `(source, destination, partition_id)` submissions
   are refused while the manifest is live. The manifest TTL
   (`export_merge_tree_partition_manifest_ttl`, default 180s) defines the idempotency window; it
   does NOT terminate in-flight tasks. `export_merge_tree_partition_force_export = 1` overrides.

8. **Two experimental gates, asymmetric scope.** `EXPORT PART` is gated query-level
   (`allow_experimental_export_merge_tree_part`) — individual users can try it. `EXPORT PARTITION`
   is gated server-level (`enable_experimental_export_merge_tree_partition_feature`) because it
   writes to Keeper and engages cluster coordination — rollout is an operator decision, not a
   per-query one.

### Concurrency / locking

- Per-part export holds the part's `DataPartStorage` lock for the duration of the read (same
  guarantees as a merge/mutation read).
- Partition-export coordinator uses Keeper multi-transactions to (a) claim a part, (b) record
  progress, (c) decrement `parts_to_do`, (d) transition status.
- No server-wide lock. Background export pool size is bounded (reuse the existing
  `background_pool_size` knob or add a dedicated one — TBD).
- Idempotency against duplicate submission is enforced at the Keeper manifest level (unique
  `(source, destination, partition_id)` while manifest is live).

### Storage format changes

- **On-disk parts:** unchanged.
- **Keeper:** adds `<table_zk>/partition_exports/` subtree. Older servers ignore unknown
  Keeper children; no schema version bump required but coordinator code MUST be tolerant of
  concurrent removal by another version (TBD — verify).
- **Object-storage layout:** `<dest_path>/<hive_partition>/<filename>.<N>.<format>` plus
  `<dest_path>/commit_<filename>` (part-level) or
  `<dest_path>/commit_<partition_id>_<tx_id>` (partition-level). Readers that don't understand
  commit files will see the data files directly — this is acceptable for non-atomic readers but
  callers wanting atomicity MUST filter by commit.

### Performance

- Hot path: Parquet encoding of a single part. No extra `SELECT` / sort / merge pass vs.
  `INSERT ... SELECT` baseline — that is the expected win.
- Memory: one Parquet writer per concurrent export; row-group buffering bounded by
  `output_format_parquet_row_group_size_bytes`.
- I/O: one network write stream per output file; chunked when
  `export_merge_tree_part_max_bytes_per_file` / `_max_rows_per_file` set.
- Benchmark coverage: TBD — propose a `tests/performance/export_merge_tree_part.xml` comparing
  `EXPORT PART` vs. `INSERT INTO s3_t SELECT FROM mt_t WHERE _part = ...` over a ~1 GB part.

### Alternatives considered

1. `INSERT INTO s3_t SELECT FROM mt_t WHERE _part = 'p'` — today's workaround. Rejected
   because it runs the full `SELECT` pipeline (decode, potential re-sort, distribute) per export,
   has no cross-replica coordination, and no native commit-file atomicity.
2. **Synchronous `ALTER ... EXPORT PART` that blocks the client** — rejected; partition exports
   can run for hours and the HTTP / native session would time out. Async + system tables mirrors
   `ALTER ... MUTATE` and is already familiar.
3. **Non-replicated `EXPORT PARTITION` (per-replica, uncoordinated)** — rejected because
   duplicates and split-brain are the default outcome when every replica independently exports
   the parts it holds.
4. **Queue the partition export in the existing replication log** — rejected; the replication
   log is for *data* mutations that must apply on every replica, whereas partition exports are
   a distributed *side-effect* whose assignment depends on which replica holds which part.
   Separate Keeper subtree is cleaner.
5. **Non-Keeper coordination (leader replica drives everything)** — rejected; would require a
   new leader-election path and wouldn't survive leader restart without a Keeper-backed manifest
   anyway.

### Open questions

- Exact error codes for each failure class above — confirm names in
  `src/Common/ErrorCodes.cpp` during prototype.
- Whether `EXPORT PART` should refuse to run against `Replicated*MergeTree` (forcing users to
  `EXPORT PARTITION`) or remain allowed as the primitive it clearly is. Current tests allow
  both; this should be documented explicitly.
- Dedicated background pool for exports vs. reuse of existing `background_move_pool_size` /
  similar — TBD.
- Behaviour of `EXPORT PARTITION` when initiating replica dies mid-task: the manifest persists
  in Keeper, but does a surviving replica take over as "source replica"? Current docs say "task
  is persistent" — clarify recovery semantics.
- Is the manifest TTL enforced by the initiator or by a cluster-wide cleanup job? Affects what
  happens when the initiator is offline.

---

## 4. Test plan

### Functional tests — `tests/queries/0_stateless`

Existing coverage to retain:

- `03572_export_merge_tree_part_basic.sh` — golden path, idempotent re-export, wildcard +
  hive partition strategies.
- `03572_export_merge_tree_part_to_object_storage_simple.sql` — error cases
  (`INCOMPATIBLE_COLUMNS`, `NOT_IMPLEMENTED`, `UNKNOWN_FUNCTION`, `EPHEMERAL` collision).
- `03572_export_merge_tree_part_limits_and_table_functions.sh` — `max_bytes_per_file`,
  `max_rows_per_file`, table-function destination with schema inheritance / explicit structure.
- `03572_export_merge_tree_part_special_columns.sh` — `ALIAS`, `MATERIALIZED`, `EPHEMERAL`,
  mixed / complex expressions.
- `03572_export_replicated_merge_tree_part_to_object_storage.sh` +
  `03572_export_replicated_merge_tree_part_to_object_storage_simple.sql` — part-level export
  from `ReplicatedMergeTree`.
- `03604_export_merge_tree_partition.sh` — basic `EXPORT PARTITION ID`.
- `03608_export_merge_tree_part_filename_pattern.sh` — default and custom
  `export_merge_tree_part_filename_pattern` including `{database}` / `{table}` macros.

New tests to add:

- `NNNN_export_merge_tree_part_pending_mutations.sh` — with/without the
  `..._throw_on_pending_mutations` / `..._throw_on_pending_patch_parts` guards and `IN PARTITION`
  mutations.
- `NNNN_export_merge_tree_part_commit_file.sh` — verify a `commit_<part>_<checksum>` file exists
  alongside every successful export and references every written data file; verify that a
  partial run (simulated by killing before commit) does not produce a commit file.
- `NNNN_export_merge_tree_part_overwrite_policy.sh` — all three values of
  `export_merge_tree_part_file_already_exists_policy` (`skip`, `error`, `overwrite`) plus
  `export_merge_tree_part_overwrite_file_if_exists`.
- `NNNN_export_merge_tree_part_profile_events.sh` — assert `PartsExports`,
  `PartsExportFailures`, `PartsExportDuplicated`, `PartsExportTotalMilliseconds` move as
  expected.

Do not add `no-parallel` to any new test unless explicitly required by shared S3 bucket paths;
`03604` currently has the tag and should be re-examined to see whether unique per-run paths
remove the need.

### Integration tests — `tests/integration`

Keep:

- `test_export_merge_tree_part_to_object_storage/` — part export in a multi-node setup.
- `test_export_replicated_mt_partition_to_object_storage/` — partition export across replicas,
  including `wait_for_export_status`, retry counting, and replica failure scenarios.

Add:

- A case where the initiating replica dies mid-partition-export and a surviving replica must
  complete the task (covers the open question above).
- A case where the experimental feature is disabled on one replica
  (`disable_experimental_export_partition.xml` config) and enabled on the rest — confirm the
  task still completes via the enabled replicas.
- `KILL EXPORT PARTITION` transitions status to `KILLED` and leaves no orphan in-flight writer.
- Mixed-version cluster: upgrade scenario where only some replicas know about `EXPORT PARTITION`.

Invocation: `python -m ci.praktika run "integration" --test test_export_merge_tree_part_to_object_storage,test_export_replicated_mt_partition_to_object_storage`.

### Performance tests — `tests/performance`

Add `export_merge_tree_part.xml`: compare `ALTER TABLE ... EXPORT PART` vs.
`INSERT INTO s3_t SELECT * FROM mt WHERE _part = ...` on a ~1 GB Wide part; track wall time and
peak memory. Hot path is the Parquet encoder, which warrants a guard against regressions.

### Manual verification

- Roundtrip: export partition → read via `SELECT * FROM s3(...)` → create new
  `ReplicatedMergeTree` from the S3 data → row counts / checksums match the source.
- `system.replicated_partition_exports` behaviour under a crashed initiator (cluster restart).
- Object-storage layout inspection via `s3(..., format=One)` listing: exactly N data files + 1
  commit file per transaction.

### Rollout / risk

- **Risk:** Keeper schema extension is write-once; a partially-rolled-out cluster where only
  some replicas understand the `partition_exports` subtree will stall partition exports
  (`parts_to_do > 0`) rather than corrupt data. Acceptable but must be documented in the upgrade
  notes.
- **Risk:** object-storage cost / accidental large exports. Mitigated by the experimental gate
  (default off) and the manifest idempotency window.
- **Flag strategy:** ship with `allow_experimental_export_merge_tree_part` (query, default
  `false`) and `enable_experimental_export_merge_tree_partition_feature` (server, default
  `false`). Flip defaults to `true` only after: (a) the open questions above are resolved, (b)
  the new functional / integration tests land, (c) one release cycle of customer feedback.
- **Watch in production:** `PartsExportFailures`, `exception_count` on
  `system.replicated_partition_exports`, Keeper watch counts under the `partition_exports`
  subtree, and object-storage request-error rates.
