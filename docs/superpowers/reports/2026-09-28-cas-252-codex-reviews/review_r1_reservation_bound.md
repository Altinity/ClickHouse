## Verdict — REVISE

The `selectPartsToMutate` change correctly prevents a higher registered mutation from being selected while a lower `CommittingBlock::Op::Mutation` reservation exists. It fixes the reported two-`DELETE` interleaving.

It is not sufficient as a global fix. Plain merge selection can still cross the unregistered mutation boundary, and a concurrent barrier `ALTER` can still miss the mutation. Both can violate mutation ordering. Transaction rollback and orphan-file exception paths also need explicit handling before claiming coverage of every interleaving.

## MAJOR findings

### 1. Merge selection can still cross an in-flight mutation

`MergeTreeMergePredicate` snapshots `committing_blocks`, but only uses them to find the minimum in-flight `Update`. Its mutation-version comparison consults only `current_mutations_by_version`, where the paused mutation is absent:

- [MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)
- [MergeTreeMergePredicate.cpp:50](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:50)
- [MergeTreeMergePredicate.cpp:87](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:87)

Concrete counterexample:

1. Part `P` exists below block `N`.
2. Mutation `N`, for example `DELETE WHERE id = 1`, allocates its block and pauses before registration.
3. An insert creates part `Q` above `N`, containing `id = 1`.
4. `P` and `Q` have the same current mutation version according to the incomplete map, so a merge may combine them.
5. The result has the older part’s effective data version.
6. When mutation `N` registers, it applies to the merged result and incorrectly deletes the row inserted after `N`.

The inverse shape can advance a merged part above `N`, permanently skipping `N`. A merge may also apply a committed patch above `N`, because `getNextMutationVersion` cannot see the in-flight mutation.

This is especially important because `scheduleDataProcessingJob` attempts merge selection before mutation selection at [StorageMergeTree.cpp:2051](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2051). The new bound in `selectPartsToMutate` therefore does not protect the merge path.

### 2. Barrier `ALTER` can miss the in-flight mutation

Barrier `ALTER`s determine the preceding mutation by taking `current_mutations_by_version.rbegin()`:

- [StorageMergeTree.cpp:535](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:535)

That has exactly the same missing-prefix assumption.

Moreover, the intended alter lock in `mutate` is accidentally released before `startMutation`: the `alter_lock` declared in the `if` initializer goes out of scope at the end of the `if` statement, before line 1094:

- [StorageMergeTree.cpp:1082](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1082)

Therefore:

1. A lightweight `DELETE` allocates `N` and pauses before registration.
2. It no longer owns `alter_lock`.
3. A concurrent `RENAME COLUMN` acquires the lock, sees no mutation in the map, and does not wait.
4. It can publish new metadata before `N` is registered.

The later selection bound preserves physical mutation order, but it does not restore the intended metadata ordering. The earlier mutation may then be interpreted against metadata it was not created for.

### 3. Transaction rollback is unsafe in the same window

For a transactional mutation, `prepareMutationEntry` calls `txn->addMutation` before map registration:

- [StorageMergeTree.cpp:897](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:897)
- [StorageMergeTree.cpp:920](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:920)

If the transaction rolls back while paused, `killMutation` finds no map entry. The mutation can then register after its transaction has disappeared. `selectPartsToMutate` eventually encounters an unknown CSN and raises an exception at [StorageMergeTree.cpp:1859](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1859).

The mutation-selection bound prevents a higher mutation from overtaking it while paused, but does not make the eventual registration valid. Upstream already has a separate registration-time transaction-state recheck for this race; this patch needs to be rebased onto or adapted to that logic.

## MINOR findings

- The new test prefix conflicts with existing `05027_read_wkb_deep_nesting.{sql,reference}`. The repository’s `add-test` helper would currently allocate `05053`. Both new files must be renamed.
- The test cleanup disables failpoints and waits, but does not drop `t_mut_order` on failure. Add an unconditional `DROP TABLE IF EXISTS ... SYNC`.
- The test otherwise uses condition-based polling; its `sleep 0.1` is polling backoff, not timing-based synchronization.
- The long code comment says `N-1`, but block numbers can have gaps caused by inserts and updates. Say “a lower mutation version”.
- `getCommittingBlocks` copies the entire set just to find one mutation reservation. A small locked helper returning the minimum matching block would avoid allocations in background selection.
- In the local current `upstream/master`, the same injection point already has `mt_pause_before_register_mutation`, declared as `PAUSEABLE`. The new once-only failpoint needs a clearly distinct but consistent name, such as `mt_pause_once_before_register_mutation`, or the patch must otherwise be reconciled when rebased.

## Answers

### 1. Correctness

No, not in every interleaving.

For two or more ordinary concurrent mutations, the core selection argument is sound:

- `CommittingBlocksSet` is ordered by block number.
- Every allocated mutation retains its holder until registration.
- `selectPartsToMutate` runs under `currently_processing_in_background_mutex`.
- Registration and destruction of the holder happen while that same mutex is held.
- Therefore selection cannot observe the mutation in the map without the reservation having been removed atomically with respect to selection.

With several in-flight mutations, using the lowest mutation reservation correctly limits the usable prefix of `current_mutations_by_version`.

Cases:

- Mutation fails before registration: normally safe; once its holder disappears, the gap is known never to register and higher mutations may proceed.
- Transaction without concurrent rollback: the range bounding works.
- Transaction with rollback in the prepare/register window: unsafe for the separate reason described above.
- Process restart while the holder still exists: the holder disappears, but final `mutation_*.txt` files are loaded into an ordered map by `loadMutations`, so an ordinary crash before any part crossed the boundary is safe.
- Failed mutation leaving an orphaned final file: not fully safe. `MergeTreeMutationEntry` cleanup is best-effort, and the holder is destroyed before the entry with the current local/member ordering. A removal failure, or termination after reservation removal but before file removal, can leave a mutation that is replayed on restart.
- `ALTER` versus lightweight `DELETE`: not fully safe because the barrier wait scans only the map and `mutate` drops its alter lock too early.
- Merge selection: unsafe, as shown in the counterexample above.
- Lightweight `DELETE` versus heavy `ALTER` mutation selection: both use `CommittingBlock::Op::Mutation`, so the new mutation-selector bound itself is command-type agnostic.

### 2. Minimality

The runtime correctness hunk is only the `selectPartsToMutate` range bound. The postpone reason is observability; the failpoint and test are regression coverage.

Alternatives:

- Fix the lifetime of `alter_lock`: this is the smallest fix for two concurrent user-submitted mutations and restores the behavior promised by the existing comment. It holds only the per-table alter mutex across mutation file I/O, not the background-processing mutex. It serializes mutation preparation, reducing concurrency, but exception behavior is straightforward RAII. It does not by itself protect merges crossing a single in-flight mutation.
- Allocate under `currently_processing_in_background_mutex`, then unlock: insufficient. A higher mutation can still register while the lower thread performs I/O.
- Write the temporary file first, then allocate, rename, and register under the mutex: safe for ordinary `startMutation` and holds the mutex only across final rename plus registration. It does not fit the current metadata-`ALTER` path, where the final mutation file must precede the durable metadata commit.
- Register a placeholder before file I/O: sound if every map consumer understands placeholders, but invasive and exception-heavy. `committing_blocks` is already effectively that placeholder mechanism.
- Hold `currently_processing_in_background_mutex` across prepare and registration: simplest safety model for ordinary mutations and also blocks merges, but it holds a hot background-selection lock across `writeFile`, `sync`, and rename. It cannot simply be extended across metadata `ALTER` preparation because the code already documents a `DatabaseAtomic::mutex` lock-order inversion.
- Use the current patch’s reservation model: best for concurrency, but only if mutation selection, merge selection, patch selection, barrier ordering, transaction rollback, and cleanup all respect the reservation.

### 3. Sufficiency

Missed sites:

- `MergeTreeMergePredicate::canMergeParts`: uses only registered mutations.
- `MergeTreeMergePredicate::getPatchesToApplyOnMerge`: `getNextMutationVersion` can skip an in-flight mutation and allow later patches.
- Barrier handling in `StorageMergeTree::alter`: `rbegin` can miss a lower in-flight mutation.
- Transaction registration: needs a rollback-state recheck after insertion under the background mutex.

Sites that do not independently require the same bound:

- `getIncompleteMutationsStatus` and `getMutationsStatus` remain correct if no operation can advance a part across the missing version. They become wrong downstream of the missed merge case.
- `clearOldMutations` has the same dependency: it is safe only if part data versions cannot cross the gap.
- `MutationsSnapshot` can legitimately omit an unregistered, unacknowledged mutation for a query snapshot. Its merge-task use is indirectly affected through patch/merge selection.
- There is no `StorageMergeTree::mutationsFinalizingTask` in this non-replicated implementation. That task belongs to `StorageReplicatedMergeTree`; non-replicated completion is handled through status checks and `clearOldMutations`.

### 4. Postpone reason and failpoint

The postpone reason is placed appropriately in `PostponeReasons`, and assigning it per part is consistent with `system.mutations`.

The failpoint is at the exact desired window: after allocation and file commit, before acquiring the registration mutex. `PAUSEABLE_ONCE` is correct because the first mutation must remain paused while the second passes through the same call site. `SYSTEM WAIT FAILPOINT ... PAUSE` observes persistent pause state, so the handshake is race-free.

The only naming concern is upstream compatibility: current upstream already has `mt_pause_before_register_mutation` at that point. The new name should explicitly distinguish its once-only behavior.

### 5. Test

The synchronization is deterministic:

- The test waits for the server-side pause rather than sleeping.
- Polling checks explicit database state.
- `timeout 60` prevents an unreachable failpoint from hanging the suite.
- Disabling the failpoint releases the paused mutation.
- `PAUSEABLE_ONCE` lets the higher mutation register.

It fails before the mutation-selector fix for the intended reason: the higher mutation completes instead of becoming postponed. It passes after the proposed hunk for that scenario.

Problems:

- It does not cover merge selection, the largest remaining correctness gap.
- It does not cover transaction rollback or a barrier `ALTER`.
- It reuses test number `05027`.
- Cleanup should drop the table and should preserve the original test exit status.
- A final `wait` without a PID is acceptable here but less precise than tracking and clearing `paused_pid`.

The tags are justified:

- `no-parallel` is necessary because both failpoints are server-global.
- `no-shared-merge-tree` is necessary because the code under test is specifically `StorageMergeTree`.
- `$CLICKHOUSE_CLIENT` usage follows repository convention.

### 6. CAS dependence

Nothing in the code or test depends on a content-addressed disk:

- No CAS setting or disk engine is configured.
- The logic uses generic `DiskPtr`, `committing_blocks`, and `StorageMergeTree`.
- The test creates an ordinary `MergeTree`.
- `no-shared-merge-tree` refers to the engine implementation, not content-addressed storage.

The `tmp/cas252` directory name is external to the patch and has no product effect. The patch is conceptually a generic MergeTree fix.

### 7. Upstream readiness

Not acceptable as-is.

A ClickHouse maintainer is likely to object to:

- the unhandled merge/patch-selection correctness hole;
- the missed barrier-`ALTER` ordering;
- failure to account for transaction rollback in the newly exposed window;
- the duplicate stateless-test number;
- the stale/conflicting failpoint naming relative to current upstream;
- the long `N`/`N-1` explanatory comment despite block-number gaps;
- lack of a regression case proving merges cannot cross the reservation.

## Recommended patch changes

### 1. Retain the alter lock through mutation creation

```diff
diff --git a/src/Storages/StorageMergeTree.cpp b/src/Storages/StorageMergeTree.cpp
@@
-        if (auto alter_lock = tryLockForAlter(query_context->getSettingsRef()[Setting::lock_acquire_timeout]); alter_lock == std::nullopt)
+        auto alter_lock = tryLockForAlter(query_context->getSettingsRef()[Setting::lock_acquire_timeout]);
+        if (!alter_lock)
         {
             throw Exception(
                 ErrorCodes::TIMEOUT_EXCEEDED,
@@
         }
         version = startMutation(commands, query_context);
```

This fixes the documented mutation/metadata serialization contract and prevents a barrier `ALTER` from overtaking a mutation already preparing its entry.

### 2. Make merge and patch selection honor in-flight mutation blocks

Add to `MergeTreeMergePredicate::canMergeParts`, after the registered-mutation comparison:

```diff
+    const Int64 lower_data_version = std::min(left.info.getDataVersion(), right.info.getDataVersion());
+    const Int64 upper_data_version = std::max(left.info.getDataVersion(), right.info.getDataVersion());
+    for (const auto & block : committing_blocks)
+    {
+        if (block.op == CommittingBlock::Op::Mutation
+            && block.number > lower_data_version
+            && block.number <= upper_data_version)
+        {
+            return std::unexpected(PreformattedMessage::create(
+                "Mutation block {} is still being registered between parts {} and {}",
+                block.number,
+                left.name,
+                right.name));
+        }
+    }
```

Bound patch application in `getPatchesToApplyOnMerge`:

```diff
     Int64 next_version = storage.getNextMutationVersion(first_part.getDataVersion(), merge_mutate_lock);
+    for (const auto & block : committing_blocks)
+    {
+        if (block.op != CommittingBlock::Op::Mutation
+            || block.number <= first_part.getDataVersion())
+            continue;
+
+        if (next_version == 0 || block.number < next_version)
+            next_version = block.number;
+        break;
+    }
     return DB::getPatchesToApplyOnMerge(it->second, range, next_version);
```

### 3. Recheck transactional state while registration is locked

Immediately after `addPreparedMutationEntry`, while still holding `currently_processing_in_background_mutex`, re-read the transaction state. If it is already rolled back, erase the just-registered entry, undo counters, remove its file outside the mutex, and throw `INVALID_TRANSACTION`. This is the same lock protocol needed so a rollback either:

- completes before the check and is detected there; or
- starts afterward and finds the registered map entry in `killMutation`.

Current upstream’s `StorageMergeTree::startMutation` already contains this pattern and should be used when rebasing.

### 4. Preserve the reservation through orphan cleanup

Reorder construction and ownership so the mutation entry is destroyed before the block holder:

```diff
diff --git a/src/Storages/StorageMergeTree.h b/src/Storages/StorageMergeTree.h
@@
     struct PreparedMutationEntry
     {
+        std::unique_ptr<PlainCommittingBlockHolder> block_holder;
         MergeTreeMutationEntry entry;
         Int64 version;
         String mutation_id;
         String additional_info;
-        std::unique_ptr<PlainCommittingBlockHolder> block_holder;
     };
```

And in `prepareMutationEntry`:

```diff
-    MergeTreeMutationEntry entry(commands, disk, relative_data_path, insert_increment.get(), current_tid, getContext()->getWriteSettings());
     auto block_holder = allocateBlockNumber(CommittingBlock::Op::Mutation);
+    MergeTreeMutationEntry entry(commands, disk, relative_data_path, insert_increment.get(), current_tid, getContext()->getWriteSettings());
@@
-    return {std::move(entry), version, std::move(mutation_id), std::move(additional_info), std::move(block_holder)};
+    return {
+        std::move(block_holder),
+        std::move(entry),
+        version,
+        std::move(mutation_id),
+        std::move(additional_info)};
```

This ensures cleanup attempts to remove the unregistered final file before making the block available to later operations.

### 5. Extend the regression test with a merge-crossing case

Add a same-partition scenario:

```bash
echo "--- merge must not cross an in-flight mutation"
$CLICKHOUSE_CLIENT --query "
    DROP TABLE t_mut_order;
    CREATE TABLE t_mut_order (id UInt64) ENGINE = MergeTree ORDER BY id;
    INSERT INTO t_mut_order VALUES (0);
"
start_paused_delete t_mut_order "id = 1"

# This row was inserted after the mutation block and must not be affected by it.
$CLICKHOUSE_CLIENT --query "INSERT INTO t_mut_order VALUES (1)"
$CLICKHOUSE_CLIENT --query "
    OPTIMIZE TABLE t_mut_order FINAL
    SETTINGS optimize_throw_if_noop = 0
"

$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT mt_pause_before_mutation_registration"
wait "$paused_pid"
paused_pid=""
wait_for_query_result "SELECT countIf(is_done) $MUTATIONS" "1"
$CLICKHOUSE_CLIENT --query "SELECT groupArray(id) FROM t_mut_order"
```

Expected result: `[0,1]`. Without the merge-predicate fix, the two parts can merge below `N`, and the late row is incorrectly deleted.

Finally:

- Rename the test using `add-test`; in this checkout the next prefix is `05053`.
- Add `DROP TABLE IF EXISTS t_mut_order SYNC` to the trap.
- Change the production comment from “`N-1`” to “a lower mutation version”.
- Rebase and reconcile the failpoint with upstream’s existing `mt_pause_before_register_mutation`.
213,718
## Verdict — REVISE

The `selectPartsToMutate` change correctly prevents a higher registered mutation from being selected while a lower `CommittingBlock::Op::Mutation` reservation exists. It fixes the reported two-`DELETE` interleaving.

It is not sufficient as a global fix. Plain merge selection can still cross the unregistered mutation boundary, and a concurrent barrier `ALTER` can still miss the mutation. Both can violate mutation ordering. Transaction rollback and orphan-file exception paths also need explicit handling before claiming coverage of every interleaving.

## MAJOR findings

### 1. Merge selection can still cross an in-flight mutation

`MergeTreeMergePredicate` snapshots `committing_blocks`, but only uses them to find the minimum in-flight `Update`. Its mutation-version comparison consults only `current_mutations_by_version`, where the paused mutation is absent:

- [MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)
- [MergeTreeMergePredicate.cpp:50](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:50)
- [MergeTreeMergePredicate.cpp:87](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:87)

Concrete counterexample:

1. Part `P` exists below block `N`.
2. Mutation `N`, for example `DELETE WHERE id = 1`, allocates its block and pauses before registration.
3. An insert creates part `Q` above `N`, containing `id = 1`.
4. `P` and `Q` have the same current mutation version according to the incomplete map, so a merge may combine them.
5. The result has the older part’s effective data version.
6. When mutation `N` registers, it applies to the merged result and incorrectly deletes the row inserted after `N`.

The inverse shape can advance a merged part above `N`, permanently skipping `N`. A merge may also apply a committed patch above `N`, because `getNextMutationVersion` cannot see the in-flight mutation.

This is especially important because `scheduleDataProcessingJob` attempts merge selection before mutation selection at [StorageMergeTree.cpp:2051](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2051). The new bound in `selectPartsToMutate` therefore does not protect the merge path.

### 2. Barrier `ALTER` can miss the in-flight mutation

Barrier `ALTER`s determine the preceding mutation by taking `current_mutations_by_version.rbegin()`:

- [StorageMergeTree.cpp:535](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:535)

That has exactly the same missing-prefix assumption.

Moreover, the intended alter lock in `mutate` is accidentally released before `startMutation`: the `alter_lock` declared in the `if` initializer goes out of scope at the end of the `if` statement, before line 1094:

- [StorageMergeTree.cpp:1082](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1082)

Therefore:

1. A lightweight `DELETE` allocates `N` and pauses before registration.
2. It no longer owns `alter_lock`.
3. A concurrent `RENAME COLUMN` acquires the lock, sees no mutation in the map, and does not wait.
4. It can publish new metadata before `N` is registered.

The later selection bound preserves physical mutation order, but it does not restore the intended metadata ordering. The earlier mutation may then be interpreted against metadata it was not created for.

### 3. Transaction rollback is unsafe in the same window

For a transactional mutation, `prepareMutationEntry` calls `txn->addMutation` before map registration:

- [StorageMergeTree.cpp:897](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:897)
- [StorageMergeTree.cpp:920](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:920)

If the transaction rolls back while paused, `killMutation` finds no map entry. The mutation can then register after its transaction has disappeared. `selectPartsToMutate` eventually encounters an unknown CSN and raises an exception at [StorageMergeTree.cpp:1859](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1859).

The mutation-selection bound prevents a higher mutation from overtaking it while paused, but does not make the eventual registration valid. Upstream already has a separate registration-time transaction-state recheck for this race; this patch needs to be rebased onto or adapted to that logic.

## MINOR findings

- The new test prefix conflicts with existing `05027_read_wkb_deep_nesting.{sql,reference}`. The repository’s `add-test` helper would currently allocate `05053`. Both new files must be renamed.
- The test cleanup disables failpoints and waits, but does not drop `t_mut_order` on failure. Add an unconditional `DROP TABLE IF EXISTS ... SYNC`.
- The test otherwise uses condition-based polling; its `sleep 0.1` is polling backoff, not timing-based synchronization.
- The long code comment says `N-1`, but block numbers can have gaps caused by inserts and updates. Say “a lower mutation version”.
- `getCommittingBlocks` copies the entire set just to find one mutation reservation. A small locked helper returning the minimum matching block would avoid allocations in background selection.
- In the local current `upstream/master`, the same injection point already has `mt_pause_before_register_mutation`, declared as `PAUSEABLE`. The new once-only failpoint needs a clearly distinct but consistent name, such as `mt_pause_once_before_register_mutation`, or the patch must otherwise be reconciled when rebased.

## Answers

### 1. Correctness

No, not in every interleaving.

For two or more ordinary concurrent mutations, the core selection argument is sound:

- `CommittingBlocksSet` is ordered by block number.
- Every allocated mutation retains its holder until registration.
- `selectPartsToMutate` runs under `currently_processing_in_background_mutex`.
- Registration and destruction of the holder happen while that same mutex is held.
- Therefore selection cannot observe the mutation in the map without the reservation having been removed atomically with respect to selection.

With several in-flight mutations, using the lowest mutation reservation correctly limits the usable prefix of `current_mutations_by_version`.

Cases:

- Mutation fails before registration: normally safe; once its holder disappears, the gap is known never to register and higher mutations may proceed.
- Transaction without concurrent rollback: the range bounding works.
- Transaction with rollback in the prepare/register window: unsafe for the separate reason described above.
- Process restart while the holder still exists: the holder disappears, but final `mutation_*.txt` files are loaded into an ordered map by `loadMutations`, so an ordinary crash before any part crossed the boundary is safe.
- Failed mutation leaving an orphaned final file: not fully safe. `MergeTreeMutationEntry` cleanup is best-effort, and the holder is destroyed before the entry with the current local/member ordering. A removal failure, or termination after reservation removal but before file removal, can leave a mutation that is replayed on restart.
- `ALTER` versus lightweight `DELETE`: not fully safe because the barrier wait scans only the map and `mutate` drops its alter lock too early.
- Merge selection: unsafe, as shown in the counterexample above.
- Lightweight `DELETE` versus heavy `ALTER` mutation selection: both use `CommittingBlock::Op::Mutation`, so the new mutation-selector bound itself is command-type agnostic.

### 2. Minimality

The runtime correctness hunk is only the `selectPartsToMutate` range bound. The postpone reason is observability; the failpoint and test are regression coverage.

Alternatives:

- Fix the lifetime of `alter_lock`: this is the smallest fix for two concurrent user-submitted mutations and restores the behavior promised by the existing comment. It holds only the per-table alter mutex across mutation file I/O, not the background-processing mutex. It serializes mutation preparation, reducing concurrency, but exception behavior is straightforward RAII. It does not by itself protect merges crossing a single in-flight mutation.
- Allocate under `currently_processing_in_background_mutex`, then unlock: insufficient. A higher mutation can still register while the lower thread performs I/O.
- Write the temporary file first, then allocate, rename, and register under the mutex: safe for ordinary `startMutation` and holds the mutex only across final rename plus registration. It does not fit the current metadata-`ALTER` path, where the final mutation file must precede the durable metadata commit.
- Register a placeholder before file I/O: sound if every map consumer understands placeholders, but invasive and exception-heavy. `committing_blocks` is already effectively that placeholder mechanism.
- Hold `currently_processing_in_background_mutex` across prepare and registration: simplest safety model for ordinary mutations and also blocks merges, but it holds a hot background-selection lock across `writeFile`, `sync`, and rename. It cannot simply be extended across metadata `ALTER` preparation because the code already documents a `DatabaseAtomic::mutex` lock-order inversion.
- Use the current patch’s reservation model: best for concurrency, but only if mutation selection, merge selection, patch selection, barrier ordering, transaction rollback, and cleanup all respect the reservation.

### 3. Sufficiency

Missed sites:

- `MergeTreeMergePredicate::canMergeParts`: uses only registered mutations.
- `MergeTreeMergePredicate::getPatchesToApplyOnMerge`: `getNextMutationVersion` can skip an in-flight mutation and allow later patches.
- Barrier handling in `StorageMergeTree::alter`: `rbegin` can miss a lower in-flight mutation.
- Transaction registration: needs a rollback-state recheck after insertion under the background mutex.

Sites that do not independently require the same bound:

- `getIncompleteMutationsStatus` and `getMutationsStatus` remain correct if no operation can advance a part across the missing version. They become wrong downstream of the missed merge case.
- `clearOldMutations` has the same dependency: it is safe only if part data versions cannot cross the gap.
- `MutationsSnapshot` can legitimately omit an unregistered, unacknowledged mutation for a query snapshot. Its merge-task use is indirectly affected through patch/merge selection.
- There is no `StorageMergeTree::mutationsFinalizingTask` in this non-replicated implementation. That task belongs to `StorageReplicatedMergeTree`; non-replicated completion is handled through status checks and `clearOldMutations`.

### 4. Postpone reason and failpoint

The postpone reason is placed appropriately in `PostponeReasons`, and assigning it per part is consistent with `system.mutations`.

The failpoint is at the exact desired window: after allocation and file commit, before acquiring the registration mutex. `PAUSEABLE_ONCE` is correct because the first mutation must remain paused while the second passes through the same call site. `SYSTEM WAIT FAILPOINT ... PAUSE` observes persistent pause state, so the handshake is race-free.

The only naming concern is upstream compatibility: current upstream already has `mt_pause_before_register_mutation` at that point. The new name should explicitly distinguish its once-only behavior.

### 5. Test

The synchronization is deterministic:

- The test waits for the server-side pause rather than sleeping.
- Polling checks explicit database state.
- `timeout 60` prevents an unreachable failpoint from hanging the suite.
- Disabling the failpoint releases the paused mutation.
- `PAUSEABLE_ONCE` lets the higher mutation register.

It fails before the mutation-selector fix for the intended reason: the higher mutation completes instead of becoming postponed. It passes after the proposed hunk for that scenario.

Problems:

- It does not cover merge selection, the largest remaining correctness gap.
- It does not cover transaction rollback or a barrier `ALTER`.
- It reuses test number `05027`.
- Cleanup should drop the table and should preserve the original test exit status.
- A final `wait` without a PID is acceptable here but less precise than tracking and clearing `paused_pid`.

The tags are justified:

- `no-parallel` is necessary because both failpoints are server-global.
- `no-shared-merge-tree` is necessary because the code under test is specifically `StorageMergeTree`.
- `$CLICKHOUSE_CLIENT` usage follows repository convention.

### 6. CAS dependence

Nothing in the code or test depends on a content-addressed disk:

- No CAS setting or disk engine is configured.
- The logic uses generic `DiskPtr`, `committing_blocks`, and `StorageMergeTree`.
- The test creates an ordinary `MergeTree`.
- `no-shared-merge-tree` refers to the engine implementation, not content-addressed storage.

The `tmp/cas252` directory name is external to the patch and has no product effect. The patch is conceptually a generic MergeTree fix.

### 7. Upstream readiness

Not acceptable as-is.

A ClickHouse maintainer is likely to object to:

- the unhandled merge/patch-selection correctness hole;
- the missed barrier-`ALTER` ordering;
- failure to account for transaction rollback in the newly exposed window;
- the duplicate stateless-test number;
- the stale/conflicting failpoint naming relative to current upstream;
- the long `N`/`N-1` explanatory comment despite block-number gaps;
- lack of a regression case proving merges cannot cross the reservation.

## Recommended patch changes

### 1. Retain the alter lock through mutation creation

```diff
diff --git a/src/Storages/StorageMergeTree.cpp b/src/Storages/StorageMergeTree.cpp
@@
-        if (auto alter_lock = tryLockForAlter(query_context->getSettingsRef()[Setting::lock_acquire_timeout]); alter_lock == std::nullopt)
+        auto alter_lock = tryLockForAlter(query_context->getSettingsRef()[Setting::lock_acquire_timeout]);
+        if (!alter_lock)
         {
             throw Exception(
                 ErrorCodes::TIMEOUT_EXCEEDED,
@@
         }
         version = startMutation(commands, query_context);
```

This fixes the documented mutation/metadata serialization contract and prevents a barrier `ALTER` from overtaking a mutation already preparing its entry.

### 2. Make merge and patch selection honor in-flight mutation blocks

Add to `MergeTreeMergePredicate::canMergeParts`, after the registered-mutation comparison:

```diff
+    const Int64 lower_data_version = std::min(left.info.getDataVersion(), right.info.getDataVersion());
+    const Int64 upper_data_version = std::max(left.info.getDataVersion(), right.info.getDataVersion());
+    for (const auto & block : committing_blocks)
+    {
+        if (block.op == CommittingBlock::Op::Mutation
+            && block.number > lower_data_version
+            && block.number <= upper_data_version)
+        {
+            return std::unexpected(PreformattedMessage::create(
+                "Mutation block {} is still being registered between parts {} and {}",
+                block.number,
+                left.name,
+                right.name));
+        }
+    }
```

Bound patch application in `getPatchesToApplyOnMerge`:

```diff
     Int64 next_version = storage.getNextMutationVersion(first_part.getDataVersion(), merge_mutate_lock);
+    for (const auto & block : committing_blocks)
+    {
+        if (block.op != CommittingBlock::Op::Mutation
+            || block.number <= first_part.getDataVersion())
+            continue;
+
+        if (next_version == 0 || block.number < next_version)
+            next_version = block.number;
+        break;
+    }
     return DB::getPatchesToApplyOnMerge(it->second, range, next_version);
```

### 3. Recheck transactional state while registration is locked

Immediately after `addPreparedMutationEntry`, while still holding `currently_processing_in_background_mutex`, re-read the transaction state. If it is already rolled back, erase the just-registered entry, undo counters, remove its file outside the mutex, and throw `INVALID_TRANSACTION`. This is the same lock protocol needed so a rollback either:

- completes before the check and is detected there; or
- starts afterward and finds the registered map entry in `killMutation`.

Current upstream’s `StorageMergeTree::startMutation` already contains this pattern and should be used when rebasing.

### 4. Preserve the reservation through orphan cleanup

Reorder construction and ownership so the mutation entry is destroyed before the block holder:

```diff
diff --git a/src/Storages/StorageMergeTree.h b/src/Storages/StorageMergeTree.h
@@
     struct PreparedMutationEntry
     {
+        std::unique_ptr<PlainCommittingBlockHolder> block_holder;
         MergeTreeMutationEntry entry;
         Int64 version;
         String mutation_id;
         String additional_info;
-        std::unique_ptr<PlainCommittingBlockHolder> block_holder;
     };
```

And in `prepareMutationEntry`:

```diff
-    MergeTreeMutationEntry entry(commands, disk, relative_data_path, insert_increment.get(), current_tid, getContext()->getWriteSettings());
     auto block_holder = allocateBlockNumber(CommittingBlock::Op::Mutation);
+    MergeTreeMutationEntry entry(commands, disk, relative_data_path, insert_increment.get(), current_tid, getContext()->getWriteSettings());
@@
-    return {std::move(entry), version, std::move(mutation_id), std::move(additional_info), std::move(block_holder)};
+    return {
+        std::move(block_holder),
+        std::move(entry),
+        version,
+        std::move(mutation_id),
+        std::move(additional_info)};
```

This ensures cleanup attempts to remove the unregistered final file before making the block available to later operations.

### 5. Extend the regression test with a merge-crossing case

Add a same-partition scenario:

```bash
echo "--- merge must not cross an in-flight mutation"
$CLICKHOUSE_CLIENT --query "
    DROP TABLE t_mut_order;
    CREATE TABLE t_mut_order (id UInt64) ENGINE = MergeTree ORDER BY id;
    INSERT INTO t_mut_order VALUES (0);
"
start_paused_delete t_mut_order "id = 1"

# This row was inserted after the mutation block and must not be affected by it.
$CLICKHOUSE_CLIENT --query "INSERT INTO t_mut_order VALUES (1)"
$CLICKHOUSE_CLIENT --query "
    OPTIMIZE TABLE t_mut_order FINAL
    SETTINGS optimize_throw_if_noop = 0
"

$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT mt_pause_before_mutation_registration"
wait "$paused_pid"
paused_pid=""
wait_for_query_result "SELECT countIf(is_done) $MUTATIONS" "1"
$CLICKHOUSE_CLIENT --query "SELECT groupArray(id) FROM t_mut_order"
```

Expected result: `[0,1]`. Without the merge-predicate fix, the two parts can merge below `N`, and the late row is incorrectly deleted.

Finally:

- Rename the test using `add-test`; in this checkout the next prefix is `05053`.
- Add `DROP TABLE IF EXISTS t_mut_order SYNC` to the trap.
- Change the production comment from “`N-1`” to “a lower mutation version”.
- Rebase and reconcile the failpoint with upstream’s existing `mt_pause_before_register_mutation`.
