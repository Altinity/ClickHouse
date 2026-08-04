---
description: 'The offline clickhouse-disks CAS tools (cas-fsck, cas-gc-dryrun, cas-inspect, cas-gc-rebuild, cas-drop-member), SYSTEM CAS FSCK, reading a GC round summary, and what to collect before filing a bug.'
sidebar_label: 'Debugging'
sidebar_position: 4
slug: /antalya/cas/operations/debugging
title: 'CAS Operations — Debugging'
doc_type: 'guide'
---

# Operations — debugging {#debugging}

Five `clickhouse-disks` subcommands cover content-addressed (`CAS`) incident diagnosis; all but
`cas-gc-rebuild` and `cas-drop-member` are read-only, and all five require the disk to be opened
with `<readonly>true</readonly>` in the `clickhouse-disks` config — they must never claim a live
server's mount.

## cas-fsck {#cas-fsck}

Independently verifies pool reachability. Exits nonzero if any reachable object is missing.

```bash
clickhouse-disks -C config.xml --disk cas cas-fsck [--detail] [--timeout N] [--namespace PREFIX] [--partial]
```

- `--detail` — list every object as `<class>\t<key>\t<size>[\t<reachable_from>...]`, one row per
  object, classes `reachable`, `dangling`, `unreachable`, `pending-gc`, `awaiting-gc`,
  `unaccounted`, `stale-edge`, `corrupted-run`, `chain-broken`, `unchecked`, `lifeless-key`,
  `janitor-pending`.
- `--timeout N` — abort the scan after `N` seconds with a clear error instead of hanging (default
  `600`; `0` is unbounded).
- `--namespace PREFIX` — scope the scan to namespaces with this prefix; skips the pool-wide
  physical/pipeline classification but still reports the scoped namespaces' dangling refs and
  orphan-manifest debris.
- `--partial` — on `--timeout`, print the counts accumulated so far, flagged `partial=1`, instead of
  aborting empty-handed.

`SYSTEM CAS FSCK '<disk>'` runs the same scan over SQL, summary-only (no `--detail` equivalent yet)
— see [reading a GC round summary](#gc-round-summary) below for how its columns map to the CLI
output. `dangling` is the one class that means data loss (`INV-NO-LOSS`); `unreachable`,
`pending-gc`, and `awaiting-gc` are objects still moving through the normal condemn/graduate/delete
pipeline, not a problem on their own.

## cas-gc-dryrun {#cas-gc-dryrun}

Previews the next `GC` round's deletes without acquiring the lease or writing anything:

```bash
clickhouse-disks -C config.xml --disk cas cas-gc-dryrun
```

Prints `preview_deletes=<count>` followed by one `<reason>\t<key>\t<size>` line per candidate. It
does not fold new owner events, so away from quiescence it can **over-report** — the subset
guarantee against `cas-fsck`'s `unreachable` set holds only at quiescence, and its output must never
feed a real delete directly.

## cas-inspect {#cas-inspect}

Decodes one raw object-storage key — as printed by `cas-fsck` or `cas-gc-dryrun` — to JSON:

```bash
clickhouse-disks -C config.xml --disk cas cas-inspect '<raw-object-key>'
```

The key is positional and mandatory. This reaches straight into the pool's backend and decodes with
the same free function the unit tests exercise directly, so its output reflects the on-disk encoding
exactly, including a resolved namespace-life id when the key is a ref object or checkpoint.

## cas-gc-rebuild {#cas-gc-rebuild}

Disaster-recovery: rebuilds a `gc/state` baseline from raw owner state after the GC guard has
refused every regular round. See [`SYSTEM CAS GC REBUILD`](/sql-reference/statements/system#system-cas-gc-rebuild)
for the destructive-tool caveats — the CLI form carries the same semantics offline:

```bash
clickhouse-disks -C config.xml --disk cas cas-gc-rebuild [--force]
```

`--force` bypasses only the "healthy state" refusal (rebuild even though `gc/state` and every
referenced artifact look fine); it does not bypass a competing leader or a failed `gc/state`
compare-and-swap. Prints `performed=`, `round=`, `generation=`, `namespaces=`, `shards=`,
`committed_refs=`, `live_precommits=`, `unowned_alive_manifests=`, `edges=`, `clamped_shards=`, and
on refusal a `refusal=` line before exiting nonzero.

## cas-drop-member {#cas-drop-member}

Offline twin of [`SYSTEM CAS DROP POOL MEMBER`](/antalya/cas/operations/migration#decommission) —
see that page for preconditions and verification:

```bash
clickhouse-disks -C config.xml --disk cas cas-drop-member '<server_root_id>'
```

`<server_root_id>` is positional and mandatory. Refuses internally if the member is still alive.

## Reading a GC round summary {#gc-round-summary}

Every round writes a `Start` and a `Finish` row to
[`system.cas_gc_log`](/operations/system-tables/cas_gc_log), correlated by `round_id` (not `round`,
which is `0` on `Start` and absent on a round that never led). Read the `Finish` row's `outcome`
first — `Success`, `NotALeader`, `Deferred`, or `Error` — then, for a folding round, the counts:
`candidates_marked`, `objects_deleted`, `objects_absent`, `objects_replaced`, `objects_spared`,
`manifests_deleted`, `entries_condemned`, `entries_graduated`, `entries_redeleted`, `fence_outs`,
and `anomalies`. Between `Start` and `Finish`, one `Phase` row per phase reached carries that
phase's own `phase_duration_microseconds`, `ProfileEvents` delta, and `phase_metrics` — group by
`round_id` to reconstruct one round in order:

```sql
SELECT event_type, outcome, phase, phase_duration_microseconds, duration_ms
FROM system.cas_gc_log
WHERE round_id = '<round_id>'
ORDER BY event_time_microseconds;
```

`SYSTEM CAS GC RUN '<disk>'` runs one round synchronously and returns the same shape as one
`Finish` row, useful for driving a round on demand while watching its outcome interactively.

## The CLICKHOUSE_USER_FILES gotcha when reproducing a test manually {#user-files-gotcha}

Running a `CAS` stateless test directly with `tests/clickhouse-test` against a manually started
`clickhouse-server` (outside a configured praktika lane) requires exporting `CLICKHOUSE_USER_FILES`
to match the server's actual data path. The harness's default,
`/var/lib/clickhouse/user_files`, will not match a custom data path, which makes the pool directory
invisible to the server — the symptom is an `Unknown disk` error together with a diagnostic that
reads like an empty pool (e.g. `baseline=0 after_insert=0`) even though the server is otherwise
healthy.

## What to collect before filing a bug {#filing-a-bug}

- `clickhouse-disks cas-fsck --detail` output (or, if it times out, a `--partial` run) — the
  authoritative reachability snapshot at the time of the incident.
- The `system.cas_gc_log` rows for the relevant `round_id`(s): `Start`, every `Phase`, and `Finish`.
- The `system.cas_log` rows for the specific ref name, blob hash, or object key involved, filtered
  by `event_time` around the incident.
- `system.cas_mounts` output from every node sharing the pool, to capture lease/epoch state at
  incident time — it is a live view and will not reflect a state that has since changed.
- The server version and, if the incident is reproducible, the exact `CREATE TABLE` / `INSERT` /
  `ALTER` sequence that triggers it.
