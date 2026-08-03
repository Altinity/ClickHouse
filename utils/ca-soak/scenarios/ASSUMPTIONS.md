# Scenario suite — assumptions and operating model

This suite was built and first run unattended. Every non-obvious choice is recorded here so a reader
can tell a deliberate decision from an accident, and a green dev run from spec-scale validation.

## Infrastructure

- **Cluster.** Reuses the `utils/ca-soak` docker compose: two ClickHouse replicas (`ch1`/`ch2`) +
  RustFS object store + a single-node Keeper. There is also a `gc_shards2` variant
  (`docker-compose-gc_shards2.yml`, `storage_conf_gc_shards2.xml`) with `gc_shards=2`.
- **Binary.** The host-built CA binary at `../../build/programs/clickhouse` is bind-mounted over the
  stock image. Runs use whatever is currently built (recorded per run as `version()` + the repo git
  sha in `config.json`).
- **RustFS, not MinIO.** The CA capability probe requires a store that enforces conditional ops
  (wrong-token DELETE → 412). RustFS `1.0.0-beta.8` provides this; MinIO's If-Match semantics do not,
  so the suite targets RustFS (the soak's settled choice).

## Fresh pool per run

The README asks for a fresh pool prefix per run (`<scenario>/<seed>/<run_id>`). The compose endpoint
is a **fixed** prefix (`test/soak_pool/`), so a per-run prefix would require reconfiguring the disk
endpoint and recreating the servers for every run. Instead we realize a **fresh pool by hard reset**:
`docker compose down -v` (the RustFS container is ephemeral — no named volume — so teardown wipes the
pool) followed by `up -d`, before each scenario (unless `--no-reset`). Server logs are
host-bind-mounted under `logs/ch1` / `logs/ch2` and archived per run before the reset. This gives each
run a virgin pool + fresh `PoolMeta`, which is the operational equivalent of a fresh prefix.

`--no-reset` reuses the current pool (fast dev iteration); the first such run is NOT on a clean pool.

## Scale

- Default scale is **`dev`**: small/fast (seconds to ~2 min) so the whole suite is runnable
  unattended. `--scale ci` is medium; `--scale full` targets the spec numbers (e.g. S01 100 GiB, S03
  1–10M objects, S05 10000 tables). Each card records the **actual** scale it ran at in its
  observations and a verdict, so a passing dev run is never mistaken for spec-scale validation.
- The 15-minute default `--duration` is a cap; most dev cards finish well under it and ignore the
  remainder. Scale tests that prefill do the prefill outside the measured window.

## What the 2-replica + single-store compose cannot exercise (marked `needs_infra`, inconclusive)

These are recorded as `inconclusive` with a reason — never silently skipped or converted to pass:

- **S12** ten-replica shared pool — compose has 2 replicas. (S20 covers the 2-replica fetch/relink
  proxy of the same property.)
- **S22** object-store throttling/retry budget — needs a fault-injecting S3 proxy (503/429/slow/close)
  between ClickHouse and RustFS.
- **S24** small dedup-cache capacity — needs a storage_conf variant with a tiny `dedup_cache_bytes`
  (compose mounts only the default 64 MiB).
- **S27** backend list-pagination ambiguity — needs an instrumented store returning duplicate/unstable
  LIST pages.
- **S15** `gc_shards=8` point — compose provides only `gc_shards` 1 and 2; the 1-vs-2 comparison runs.
- **S23** 1-server and 10-server idle variants — compose is fixed at 2 servers; the 2-server idle case
  runs.

## Known live-build caveats folded into cards

- **Freeze / shadow (S18).** A pre-existing freeze/shadow bug ("B3") may make `FREEZE` fail in this
  build. S18 handles a freeze failure gracefully → `inconclusive` + a backlog anomaly, rather than a
  hard fail.
- **Manifest caps (S07).** The fail-closed caps (1M entries / 256 MiB encoded / 16 MiB inline / 1 MiB
  largest inline) are not reachable via dev-scale SQL. S07 makes a best-effort trigger and, if it
  cannot deterministically hit a cap, records `inconclusive` while still verifying the fail-closed
  *property* (no live ref at a rejected manifest; clean pool after GC).
- **Ordinary database (S25).** The `Ordinary` engine is deprecated/likely blocked; S25 attempts it and
  records the exact behavior, `inconclusive` if blocked.
- **Lightweight delete / patch parts (S10).** Support is probed at runtime; unsupported sub-paths are
  recorded `inconclusive`, and lightweight `DELETE` correctness is still tested.

## Correctness oracle

The primary correctness oracle is **all-replicas-agree** on a deterministic order-independent
aggregate (`count() + sum(sipHash64(*))`), plus, where the workload is deterministic (`INSERT ...
SELECT ... FROM numbers(N)`), an absolute Python-side count/aggregate prediction. Structural
correctness uses `clickhouse-disks ca-fsck` (`dangling==0`) and the `ca-gc-dryrun ⊆ unreachable` subset
oracle, asserted only at **quiesced** checkpoints (mid-write precommit-vs-promote windows are not used
as a hard verdict, per the README's structural-inspection caveat).

## GC driving

`SYSTEM CAS GC RUN ca` runs one synchronous round on the node it hits (only
the lease holder makes progress). A fixpoint drive issues rounds on both replicas until the fsck
unreachable count settles. The root-local part-manifest GC has no displaced-tree debris class, so the
expectation for a non-abandoning scenario is `unreachable == 0` after forced GC; a nonzero residual is
classified, not ignored.
