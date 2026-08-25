# DRAFT upstream issue (NOT filed — pending user decision)

Title: **Silent INSERT data loss on object-storage disks: block_id dedup znode is committed to Keeper before the part's local metadata is durable**

## Description

`ReplicatedMergeTreeSink::commitPart` commits in this order (line numbers from master @ `83b3f837cc8`):

1. `transaction.renameParts()` (:946);
2. the ZooKeeper multi that durably creates the `block_id` dedup znode and the replica part-znode (:953, `tryMultiNoThrow`);
3. `transaction.commit()` (:959), whose loop (`MergeTreeData.cpp:7452-7453`) is the FIRST and only place each part's deferred disk transaction is committed.

On a **local disk** this is safe: every operation, including the rename in step 1, executes immediately (`FakeDiskTransaction`), so the part's data is durable before step 2 — the protocol's implicit invariant "a registered block_id always points at recoverable data" holds.

On an **object-storage disk** it does not hold: blob data streams to S3 during write, but ALL local metadata operations — including the tmp→final directory rename — are queued (`PureMetadataObjectStorageOperation`, `DiskObjectStorageTransaction.cpp:653-660`) and execute only in step 3, AFTER the ZooKeeper commit (`IDataPartStorage::precommitTransaction` is a no-op for `DataPartStorageOnDiskFull`, so nothing materializes earlier). A crash (or any failure) between step 2 and step 3 leaves:

- ZooKeeper: `block_id` dedup znode + part-znode durably present;
- disk: no part metadata anywhere (blobs without metadata are not a part);
- every replica: unable to fetch the part (`NO_REPLICA_HAS_PART`).

The part-check path then "resolves" the phantom via `createEmptyPartInsteadOfLost` — but the `block_id` znode lives out its independent `replicated_deduplication_window` lifetime. A client retrying the byte-identical `INSERT` (the documented idempotent-retry contract, `insert_deduplicate=1`) hits cross-replica dedup — `"Block with ID ... already exists on other replicas as part ...; ignoring it"` — and receives **success with zero rows written**. Acked data loss, invisible to any storage-level consistency check.

## Reproduction (deterministic, single node, failpoint)

The existing `disk_object_storage_fail_commit_metadata_transaction` failpoint cannot express this (it also fires on the autocommit one-shot transactions wrapping ordinary disk ops — the first hit is the temp-part `createDirectories`, which kills the insert before any Keeper state exists). The reproduction adds a targeted failpoint at the close of the part's deferred disk transaction:

```cpp
// DataPartStorageOnDiskFull::commitTransaction, before transaction->commit():
fiu_do_on(FailPoints::part_storage_fail_commit_transaction,
{
    throw Exception(ErrorCodes::FAULT_INJECTED, "part_storage_fail_commit_transaction");
});
```

```sql
-- Tags: zookeeper, no-fasttest, no-parallel
DROP TABLE IF EXISTS t_dedup_disk_commit SYNC;

CREATE TABLE t_dedup_disk_commit (k UInt64, v String)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_dedup_disk_commit', 'r1')
ORDER BY k
SETTINGS storage_policy = 's3_cache';

SYSTEM ENABLE FAILPOINT part_storage_fail_commit_transaction;

INSERT INTO t_dedup_disk_commit SETTINGS insert_deduplicate = 1, insert_keeper_fault_injection_probability = 0 VALUES (1, 'x'); -- { serverError FAULT_INJECTED }

SYSTEM DISABLE FAILPOINT part_storage_fail_commit_transaction;

-- Byte-identical retry of the failed INSERT: must really insert.
INSERT INTO t_dedup_disk_commit SETTINGS insert_deduplicate = 1, insert_keeper_fault_injection_probability = 0 VALUES (1, 'x');

SELECT count() FROM t_dedup_disk_commit;
-- Actual on master: 0 (the retry silently deduplicated against the phantom block_id).
-- Expected: 1.

DROP TABLE t_dedup_disk_commit SYNC;
```

The same window is entered without a failpoint by killing the server between the ZooKeeper multi and the disk commit.

## Proposed fix (one function)

Close each part's disk transaction in `MergeTreeData::Transaction::renameParts` — the slot every call site already invokes off-lock and BEFORE its external ZooKeeper decision (that is the documented reason `renameParts` exists: "rename is IO bound, don't hold the data parts lock"; the disk commit is even more IO-bound):

```cpp
void MergeTreeData::Transaction::renameParts()
{
    for (const auto & part_need_rename : precommitted_parts_need_rename)
    {
        LOG_TEST(data.log, "Renaming part to {}", part_need_rename->name);
        part_need_rename->renameTo(part_need_rename->name, true);
    }
    precommitted_parts_need_rename.clear();

    for (const auto & part : precommitted_parts)
        if (part->getDataPartStorage().hasActiveTransaction())
            part->getDataPartStorage().commitTransaction();
}
```

Safety notes:

- `commitTransaction` resets the storage's transaction pointer, and the existing loop in `Transaction::commit` is already guarded by `hasActiveTransaction`, so it degrades to a no-op (it stays as the safety net for paths that do not call `renameParts`). Early closure has precedent: fetch already commits the loading storage transaction at download end (`DataPartsExchange`).
- A failure in `renameParts` now surfaces BEFORE the ZooKeeper multi: no `block_id` znode is created, the client error leads to a retry that genuinely inserts. Rollback needs no new machinery — by `renameParts` time every part is already `PreActive` in `data_parts_indexes`, so the ordinary `Transaction::rollback` → outdated-part cleanup reclaims the on-disk state.
- The `ZNODEEXISTS` dedup-race branch (`rollbackPartsToTemporaryState` + rename back to tmp) now runs over committed disk state via the ordinary autocommit route; the cost is one wasted upload per lost race — bounded, against silent data loss.
- The hardware-error UNKNOWN branch keeps its semantics with local-disk-equivalent durability: data is already durable when the branch decides keep-or-drop.
- Side benefit: on object-storage disks the metadata commit no longer runs under the `data_parts` lock (today `Transaction::commit` holds it through the disk commit).

With the fix, the failpoint test above returns 1: the failpoint fires in `renameParts`, before the multi, and the retry inserts.

## Scope notes

- Verified on a fork with an additional object-storage metadata backend as well — the same one-function change fixes both; nothing in the fix is specific to any backend.
- ClickHouse Cloud / SharedMergeTree may be unaffected (different commit path) — scope is the open-source object-storage disks.
