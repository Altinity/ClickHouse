# CA soak-test harness

A deterministic 24-hour soak test for content-addressed MergeTree (`cas-mergetree-poc`). The harness
hammers a two-replica ClickHouse cluster with inserts, merges, mutations, and DROP/ATTACH round-trips
while continuously verifying that both replicas agree with an in-process integer-aggregate oracle.

## Determinism contract

A single `--seed` integer drives everything. From it a `splitmix64`-based stream produces a
deterministic op ledger that feeds both the SQL workload and the in-memory model oracle. Checkpoints
quiesce all writes, then assert exact integer aggregates match on both replicas and match the oracle,
followed by a `clickhouse-disks cas-fsck` on the content-addressed disk. Re-running with the same seed
and the same ClickHouse binary must reproduce the same sequence of operations and the same assertions.

## Usage

```bash
# Start the cluster (two replicas + MinIO + ZooKeeper)
docker compose up -d

# Run Phase 1 (short smoke, ~5 min). Inserts run SYNC by default (async_insert=0): B138 showed
# the sync ABORTED-retry is idempotent, while async retries lose rows via the dedup-token-vs-part
# hazard (B139). Pass `--insert-mode async` only for a deliberate async-specific experiment.
python3 -m soak.run --seed 1 --phase 1

# Full 24-hour soak
python3 -m soak.run --seed 1 --phase full

# Run unit tests only
cd utils/ca-soak && python3 -m pytest tests/ -q
```
