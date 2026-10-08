## Verdict — REVISE

The production fix is correct and minimal for the reported direct-table race: keeping the per-table alter lock through `startMutation` prevents concurrent `DELETE`/`ALTER ... UPDATE`/`ALTER ... DELETE` mutations from registering out of block-number order.

The test must be revised. Because `mt_pause_before_register_mutation` is `PAUSEABLE`, the unfixed second `DELETE` reaches the same failpoint and blocks forever instead of registering first. The test therefore hangs before the fix and does not reproduce the claimed interleaving.

## MAJOR findings

### 1. The regression test hangs on the unfixed code

The failpoint added at [fix_v2.diff:9](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/fix_v2.diff:9) is `PAUSEABLE`, whose documented semantics are to block every invocation until disabled ([FailPoint.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Common/FailPoint.cpp:28)).

After the first mutation pauses, the synchronous second `DELETE` at [fix_v2.diff:143](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/fix_v2.diff:143) behaves as follows before the fix:

1. It acquires and immediately releases the alter lock.
2. It allocates the higher block number.
3. It reaches the same still-enabled failpoint.
4. It pauses indefinitely.

`lock_acquire_timeout=1` does not limit failpoint waiting. Consequently, the second mutation neither succeeds nor prints `TIMEOUT_EXCEEDED`; the script hangs before reaching the statement that disables the failpoint.

Use a separate `PAUSEABLE_ONCE` failpoint for this test, or run the second client asynchronously with an explicit bounded check and change the claimed pre-fix behavior.

## MINOR findings

- The test needs `no-replicated-database`. In a `Replicated` database, `DELETE` can be enqueued through replicated DDL rather than execute with the direct local timing assumed by this test.
- `05054` was not allocated by `tests/queries/0_stateless/add-test` on this base—the helper would allocate `05053`. Current upstream is already beyond `05290`, so the test must be renumbered during rebase.
- The `startMutation` comment says “Every caller … see `mutate` and `alter`,” but `alter` does not call `startMutation`; it calls `prepareMutationEntry` and `addPreparedMutationEntry` directly.
- The commit message’s statement that “before the fix the second `DELETE` succeeds” is false with the present `PAUSEABLE` test.
- There is a pre-existing proxy exception: [StorageAlias::alter](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219) forwards the alias’s alter-lock holder to the target at [line 245](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:245), rather than acquiring the target table’s alter lock. A direct target mutation and a metadata `ALTER` through an alias can therefore still overlap. This does not affect two `UPDATE`/`DELETE` mutation commands, because [StorageAlias::mutate](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:296) enters the target’s `StorageMergeTree::mutate`, which acquires the target lock.

## Answers (1–7)

### 1. Sufficiency and all mutation-entry creation paths

For the reported direct `StorageMergeTree` race: **yes, v2 is sufficient**.

The paths are:

1. `StorageMergeTree::startMutation`

   Its only caller is [StorageMergeTree::mutate](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1073). After v2, the named `alter_lock` lives through `startMutation`, including:

   - file construction and sync in [prepareMutationEntry](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:889);
   - block-number allocation;
   - the pause point;
   - registration through [addPreparedMutationEntry](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:927).

   This covers:

   - direct `ALTER TABLE ... UPDATE/DELETE`, dispatched to `mutate` at [InterpreterAlterQuery.cpp:327](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterAlterQuery.cpp:327);
   - lightweight `DELETE` in `alter_update` mode, which constructs such an `ALTER` at [InterpreterDeleteQuery.cpp:194](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterDeleteQuery.cpp:194);
   - direct `DELETE` paths calling `mutate`;
   - mutation forwarding through `StorageAlias`, materialized views, or storage proxies, because the target’s `mutate` acquires its own alter lock.

2. `StorageMergeTree::alter` with generated mutation commands

   [StorageMergeTree::alter](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:457) calls `prepareMutationEntry` at [line 574](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:574) and registers at [line 613](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:613).

   For a direct table, [InterpreterAlterQuery](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterAlterQuery.cpp:315) acquires `lockForAlter` before calling `alter`, so this whole path is serialized with v2 `mutate`.

   The durable-rollback recovery re-registration at [StorageMergeTree.cpp:743](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:743) also remains inside that same outer alter call.

   Exception: an `ALTER` forwarded through `StorageAlias` holds the alias lock, not the target lock. A direct mutation paused with target block `N` can therefore be overtaken by an alias-forwarded metadata `ALTER` allocating and registering `N+1`. That is a separate proxy-locking defect.

3. `loadMutations`

   [loadMutations](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462) constructs entries from durable files without the alter lock. This is safe because it runs during construction before the storage is published; the code explicitly documents that isolation.

No other site creates a new live mutation entry in `current_mutations_by_version`.

### 2. Minimality and lock safety

The runtime fix is minimal. The essential change is moving `alter_lock` out of the `if` initializer so its lifetime covers `startMutation`.

Compared with v1:

- v1 permitted concurrent preparation and attempted to prevent higher registered mutations from being selected.
- v2 prevents the registration inversion itself.
- v2 also restores the intended ordering with barrier metadata `ALTER`s.
- It implements round-1 recommendation 1 directly.

Holding the alter lock across mutation-file I/O is acceptable:

- It is a per-table alter mutex, not `currently_processing_in_background_mutex`.
- Background merge/mutation selection remains available during `writeFile`, `sync`, and rename.
- Competing user mutations and metadata `ALTER`s wait with their configured timeout.
- Metadata `ALTER` already holds this same lock across substantially heavier metadata work.

The resulting relevant ordering is alter lock → `currently_processing_in_background_mutex` → `committing_blocks_mutex` during registration. Existing merge selection already takes background mutex → committing-blocks mutex. No reverse committing-blocks → alter/background path was found.

There is no self-deadlock with `InterpreterAlterQuery`:

- `AlterCommands` acquire the alter lock and call `alter`, which does not reacquire it.
- `MutationCommands` call `mutate` without first acquiring the lock, so `mutate` acquires it once.
- Mixed segments release each branch-local lock before processing the next segment.

For upstream, the failpoint definition, declaration, and injection can be dropped because upstream already has them. The verbose comment and changed error text are optional; the lock-lifetime change is the complete runtime fix.

### 3. Merge selection hole

The round-1 merge hole is real and independent of v2.

[MergeTreeMergePredicate](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22) snapshots `committing_blocks`, but [canMergeParts](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:32) only compares registered mutation versions.

Concrete interleaving:

1. Existing part `P` is below pending mutation block `N`.
2. Mutation `N` allocates its block and pauses before registration.
3. An insert creates part `Q` above `N`.
4. With `N` absent from the mutation map, `P` and `Q` both appear to have the same current mutation version and may be merged.
5. [FutureMergedMutatedPart::assign](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/FutureMergedMutatedPart.cpp:39) retains the source mutation value and the first part’s minimum block.
6. When `N` registers, it can apply to the merged part, including rows inserted after `N`.

This exists with a single in-flight mutation and is neither introduced nor fixed by v2. It should be filed separately rather than expanding this narrowly described two-user-mutation fix.

The minimal fix belongs in `MergeTreeMergePredicate::canMergeParts`: reject merging two regular parts when an in-flight `CommittingBlock::Op::Mutation` lies strictly above the lower data version and at or below the upper data version.

### 4. Transaction rollback and orphan files

Both issues are independent of v2 because transaction rollback does not acquire the table’s alter lock.

Current upstream already rechecks transaction state under `currently_processing_in_background_mutex` immediately after registration and removes a concurrently rolled-back entry. That logic must simply be retained when rebasing v2.

The orphan-file lifetime issue is also unchanged. `PreparedMutationEntry` destroys its block holder before its `MergeTreeMutationEntry`, so the reservation is released before best-effort orphan-file removal. V2’s alter lock prevents another ordinary user mutation from entering during normal unwinding, but it does not change restart behavior after abrupt termination or failed removal.

Nothing from v1’s mutation-selection bound or postpone reason addresses either rollback or orphan cleanup. They are not worth retaining for these issues. Reordering the two `PreparedMutationEntry` members could narrow the cleanup interval, but it would not make orphan deletion durable and should be handled separately if desired.

### 5. Test assessment

After the fix, synchronization is deterministic:

- `SYSTEM WAIT FAILPOINT ... PAUSE` proves the first mutation has reached the window while holding the alter lock.
- The second mutation times out on that lock.
- Disabling the failpoint releases the first mutation.
- Condition-based polling waits for completion.

Before the fix, it is not correct: the second mutation pauses at the same `PAUSEABLE` failpoint instead of registering first.

The expected IDs are correct on the fixed code:

- first insert: block `1`;
- paused first mutation: `mutation_2.txt`;
- timed-out mutation: no allocation;
- final successful mutation: `mutation_3.txt`.

The `CREATE` does not consume a block number, and a single inserted part cannot merge with another part.

Cleanup is present and drops the table. It would be clearer to make each cleanup command best-effort with `|| true` and wait for the tracked PID, but the material cleanup problem is the unbounded pre-fix second client.

Convention changes needed:

- add `no-replicated-database`;
- allocate a fresh test number with `add-test`;
- keep `no-parallel` and `no-shared-merge-tree`; both are justified.

### 6. Backend specificity

Nothing ties the change to CAS, object storage, or any particular disk:

- the code changes are in generic non-replicated `StorageMergeTree`;
- the test creates an ordinary `MergeTree`;
- the comments describe mutation files and in-memory registration generically;
- the commit message discusses generic lightweight deletes and mutation ordering.

The `tmp/cas252` directory is outside the patch. The commit’s Altinity and upstream issue links do not make the implementation backend-specific.

### 7. Upstream readiness

Not ready as submitted.

A maintainer is likely to object to:

- the regression test hanging on the unfixed code;
- the incorrect commit-message claim that the second mutation succeeds before the fix;
- re-adding a failpoint already present on upstream master;
- the stale test number;
- the missing `no-replicated-database` tag;
- the inaccurate “`startMutation` callers include `alter`” comment.

After those mechanical/test corrections, the core lock-lifetime fix is suitable for upstream. The merge-boundary and `StorageAlias` locking holes should be separate issues rather than blockers for the specifically reported direct-mutation race.

## Recommended patch changes

### 1. Use a one-shot test gate

Keep upstream’s existing `PAUSEABLE` failpoint for the transaction-rollback test, and add a separate gate for this ordering test:

```diff
diff --git a/src/Common/FailPoint.cpp b/src/Common/FailPoint.cpp
@@
     PAUSEABLE(mt_pause_before_register_mutation) \
+    PAUSEABLE_ONCE(mt_pause_once_before_register_mutation) \
```

```diff
diff --git a/src/Storages/StorageMergeTree.cpp b/src/Storages/StorageMergeTree.cpp
@@
     extern const char mt_pause_before_register_mutation[];
+    extern const char mt_pause_once_before_register_mutation[];
@@
     FailPointInjection::pauseFailPoint(FailPoints::mt_pause_before_register_mutation);
+    FailPointInjection::pauseFailPoint(FailPoints::mt_pause_once_before_register_mutation);
```

Use `mt_pause_once_before_register_mutation` throughout the new test. Then, before the fix, the first mutation consumes the one-shot pause and the second mutation can register first as claimed.

### 2. Correct the test tags

```diff
-# Tags: no-parallel, no-shared-merge-tree
+# Tags: no-parallel, no-replicated-database, no-shared-merge-tree
```

Renumber the files using `tests/queries/0_stateless/add-test mutations_register_in_order.sh` after rebasing.

### 3. Clarify the `startMutation` comment

```diff
-    /// registered yet. Every caller holds the alter lock here (see `mutate` and `alter`), so a
-    /// second mutation cannot allocate a higher block number and register it before this one.
+    /// registered yet. `mutate` holds the alter lock across this function, while mutation-producing
+    /// metadata alters are registered separately under the same lock, so another mutation cannot
+    /// allocate a higher block number and register it first.
```

### 4. File the merge issue separately

Minimal follow-up direction:

```diff
diff --git a/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp b/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp
@@
     {
         uint64_t left_mutation_version = storage.getCurrentMutationVersion(left.info.getDataVersion(), merge_mutate_lock);
         uint64_t right_mutation_version = storage.getCurrentMutationVersion(right.info.getDataVersion(), merge_mutate_lock);

         if (left_mutation_version != right_mutation_version)
             return std::unexpected(PreformattedMessage::create("Parts {} and {} have different mutation version", left.name, right.name));
     }
+
+    const Int64 lower_data_version = std::min(left.info.getDataVersion(), right.info.getDataVersion());
+    const Int64 upper_data_version = std::max(left.info.getDataVersion(), right.info.getDataVersion());
+    for (const auto & block : committing_blocks)
+    {
+        if (block.number > upper_data_version)
+            break;
+
+        if (block.op == CommittingBlock::Op::Mutation && block.number > lower_data_version)
+        {
+            return std::unexpected(PreformattedMessage::create(
+                "Parts {} and {} span mutation block {} that is still being registered",
+                left.name,
+                right.name,
+                block.number));
+        }
+    }
```

Do not fold this separate merge/insert race into v2 unless the PR’s stated scope is expanded accordingly.
2026-09-28T09:12:30.108505Z ERROR codex_core::session: failed to record rollout items: thread 01a0e741-7d47-7643-b83e-cb0e24c84efe not found
184,794
## Verdict — REVISE

The production fix is correct and minimal for the reported direct-table race: keeping the per-table alter lock through `startMutation` prevents concurrent `DELETE`/`ALTER ... UPDATE`/`ALTER ... DELETE` mutations from registering out of block-number order.

The test must be revised. Because `mt_pause_before_register_mutation` is `PAUSEABLE`, the unfixed second `DELETE` reaches the same failpoint and blocks forever instead of registering first. The test therefore hangs before the fix and does not reproduce the claimed interleaving.

## MAJOR findings

### 1. The regression test hangs on the unfixed code

The failpoint added at [fix_v2.diff:9](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/fix_v2.diff:9) is `PAUSEABLE`, whose documented semantics are to block every invocation until disabled ([FailPoint.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Common/FailPoint.cpp:28)).

After the first mutation pauses, the synchronous second `DELETE` at [fix_v2.diff:143](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/fix_v2.diff:143) behaves as follows before the fix:

1. It acquires and immediately releases the alter lock.
2. It allocates the higher block number.
3. It reaches the same still-enabled failpoint.
4. It pauses indefinitely.

`lock_acquire_timeout=1` does not limit failpoint waiting. Consequently, the second mutation neither succeeds nor prints `TIMEOUT_EXCEEDED`; the script hangs before reaching the statement that disables the failpoint.

Use a separate `PAUSEABLE_ONCE` failpoint for this test, or run the second client asynchronously with an explicit bounded check and change the claimed pre-fix behavior.

## MINOR findings

- The test needs `no-replicated-database`. In a `Replicated` database, `DELETE` can be enqueued through replicated DDL rather than execute with the direct local timing assumed by this test.
- `05054` was not allocated by `tests/queries/0_stateless/add-test` on this base—the helper would allocate `05053`. Current upstream is already beyond `05290`, so the test must be renumbered during rebase.
- The `startMutation` comment says “Every caller … see `mutate` and `alter`,” but `alter` does not call `startMutation`; it calls `prepareMutationEntry` and `addPreparedMutationEntry` directly.
- The commit message’s statement that “before the fix the second `DELETE` succeeds” is false with the present `PAUSEABLE` test.
- There is a pre-existing proxy exception: [StorageAlias::alter](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219) forwards the alias’s alter-lock holder to the target at [line 245](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:245), rather than acquiring the target table’s alter lock. A direct target mutation and a metadata `ALTER` through an alias can therefore still overlap. This does not affect two `UPDATE`/`DELETE` mutation commands, because [StorageAlias::mutate](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:296) enters the target’s `StorageMergeTree::mutate`, which acquires the target lock.

## Answers (1–7)

### 1. Sufficiency and all mutation-entry creation paths

For the reported direct `StorageMergeTree` race: **yes, v2 is sufficient**.

The paths are:

1. `StorageMergeTree::startMutation`

   Its only caller is [StorageMergeTree::mutate](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1073). After v2, the named `alter_lock` lives through `startMutation`, including:

   - file construction and sync in [prepareMutationEntry](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:889);
   - block-number allocation;
   - the pause point;
   - registration through [addPreparedMutationEntry](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:927).

   This covers:

   - direct `ALTER TABLE ... UPDATE/DELETE`, dispatched to `mutate` at [InterpreterAlterQuery.cpp:327](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterAlterQuery.cpp:327);
   - lightweight `DELETE` in `alter_update` mode, which constructs such an `ALTER` at [InterpreterDeleteQuery.cpp:194](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterDeleteQuery.cpp:194);
   - direct `DELETE` paths calling `mutate`;
   - mutation forwarding through `StorageAlias`, materialized views, or storage proxies, because the target’s `mutate` acquires its own alter lock.

2. `StorageMergeTree::alter` with generated mutation commands

   [StorageMergeTree::alter](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:457) calls `prepareMutationEntry` at [line 574](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:574) and registers at [line 613](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:613).

   For a direct table, [InterpreterAlterQuery](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterAlterQuery.cpp:315) acquires `lockForAlter` before calling `alter`, so this whole path is serialized with v2 `mutate`.

   The durable-rollback recovery re-registration at [StorageMergeTree.cpp:743](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:743) also remains inside that same outer alter call.

   Exception: an `ALTER` forwarded through `StorageAlias` holds the alias lock, not the target lock. A direct mutation paused with target block `N` can therefore be overtaken by an alias-forwarded metadata `ALTER` allocating and registering `N+1`. That is a separate proxy-locking defect.

3. `loadMutations`

   [loadMutations](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462) constructs entries from durable files without the alter lock. This is safe because it runs during construction before the storage is published; the code explicitly documents that isolation.

No other site creates a new live mutation entry in `current_mutations_by_version`.

### 2. Minimality and lock safety

The runtime fix is minimal. The essential change is moving `alter_lock` out of the `if` initializer so its lifetime covers `startMutation`.

Compared with v1:

- v1 permitted concurrent preparation and attempted to prevent higher registered mutations from being selected.
- v2 prevents the registration inversion itself.
- v2 also restores the intended ordering with barrier metadata `ALTER`s.
- It implements round-1 recommendation 1 directly.

Holding the alter lock across mutation-file I/O is acceptable:

- It is a per-table alter mutex, not `currently_processing_in_background_mutex`.
- Background merge/mutation selection remains available during `writeFile`, `sync`, and rename.
- Competing user mutations and metadata `ALTER`s wait with their configured timeout.
- Metadata `ALTER` already holds this same lock across substantially heavier metadata work.

The resulting relevant ordering is alter lock → `currently_processing_in_background_mutex` → `committing_blocks_mutex` during registration. Existing merge selection already takes background mutex → committing-blocks mutex. No reverse committing-blocks → alter/background path was found.

There is no self-deadlock with `InterpreterAlterQuery`:

- `AlterCommands` acquire the alter lock and call `alter`, which does not reacquire it.
- `MutationCommands` call `mutate` without first acquiring the lock, so `mutate` acquires it once.
- Mixed segments release each branch-local lock before processing the next segment.

For upstream, the failpoint definition, declaration, and injection can be dropped because upstream already has them. The verbose comment and changed error text are optional; the lock-lifetime change is the complete runtime fix.

### 3. Merge selection hole

The round-1 merge hole is real and independent of v2.

[MergeTreeMergePredicate](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22) snapshots `committing_blocks`, but [canMergeParts](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:32) only compares registered mutation versions.

Concrete interleaving:

1. Existing part `P` is below pending mutation block `N`.
2. Mutation `N` allocates its block and pauses before registration.
3. An insert creates part `Q` above `N`.
4. With `N` absent from the mutation map, `P` and `Q` both appear to have the same current mutation version and may be merged.
5. [FutureMergedMutatedPart::assign](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/FutureMergedMutatedPart.cpp:39) retains the source mutation value and the first part’s minimum block.
6. When `N` registers, it can apply to the merged part, including rows inserted after `N`.

This exists with a single in-flight mutation and is neither introduced nor fixed by v2. It should be filed separately rather than expanding this narrowly described two-user-mutation fix.

The minimal fix belongs in `MergeTreeMergePredicate::canMergeParts`: reject merging two regular parts when an in-flight `CommittingBlock::Op::Mutation` lies strictly above the lower data version and at or below the upper data version.

### 4. Transaction rollback and orphan files

Both issues are independent of v2 because transaction rollback does not acquire the table’s alter lock.

Current upstream already rechecks transaction state under `currently_processing_in_background_mutex` immediately after registration and removes a concurrently rolled-back entry. That logic must simply be retained when rebasing v2.

The orphan-file lifetime issue is also unchanged. `PreparedMutationEntry` destroys its block holder before its `MergeTreeMutationEntry`, so the reservation is released before best-effort orphan-file removal. V2’s alter lock prevents another ordinary user mutation from entering during normal unwinding, but it does not change restart behavior after abrupt termination or failed removal.

Nothing from v1’s mutation-selection bound or postpone reason addresses either rollback or orphan cleanup. They are not worth retaining for these issues. Reordering the two `PreparedMutationEntry` members could narrow the cleanup interval, but it would not make orphan deletion durable and should be handled separately if desired.

### 5. Test assessment

After the fix, synchronization is deterministic:

- `SYSTEM WAIT FAILPOINT ... PAUSE` proves the first mutation has reached the window while holding the alter lock.
- The second mutation times out on that lock.
- Disabling the failpoint releases the first mutation.
- Condition-based polling waits for completion.

Before the fix, it is not correct: the second mutation pauses at the same `PAUSEABLE` failpoint instead of registering first.

The expected IDs are correct on the fixed code:

- first insert: block `1`;
- paused first mutation: `mutation_2.txt`;
- timed-out mutation: no allocation;
- final successful mutation: `mutation_3.txt`.

The `CREATE` does not consume a block number, and a single inserted part cannot merge with another part.

Cleanup is present and drops the table. It would be clearer to make each cleanup command best-effort with `|| true` and wait for the tracked PID, but the material cleanup problem is the unbounded pre-fix second client.

Convention changes needed:

- add `no-replicated-database`;
- allocate a fresh test number with `add-test`;
- keep `no-parallel` and `no-shared-merge-tree`; both are justified.

### 6. Backend specificity

Nothing ties the change to CAS, object storage, or any particular disk:

- the code changes are in generic non-replicated `StorageMergeTree`;
- the test creates an ordinary `MergeTree`;
- the comments describe mutation files and in-memory registration generically;
- the commit message discusses generic lightweight deletes and mutation ordering.

The `tmp/cas252` directory is outside the patch. The commit’s Altinity and upstream issue links do not make the implementation backend-specific.

### 7. Upstream readiness

Not ready as submitted.

A maintainer is likely to object to:

- the regression test hanging on the unfixed code;
- the incorrect commit-message claim that the second mutation succeeds before the fix;
- re-adding a failpoint already present on upstream master;
- the stale test number;
- the missing `no-replicated-database` tag;
- the inaccurate “`startMutation` callers include `alter`” comment.

After those mechanical/test corrections, the core lock-lifetime fix is suitable for upstream. The merge-boundary and `StorageAlias` locking holes should be separate issues rather than blockers for the specifically reported direct-mutation race.

## Recommended patch changes

### 1. Use a one-shot test gate

Keep upstream’s existing `PAUSEABLE` failpoint for the transaction-rollback test, and add a separate gate for this ordering test:

```diff
diff --git a/src/Common/FailPoint.cpp b/src/Common/FailPoint.cpp
@@
     PAUSEABLE(mt_pause_before_register_mutation) \
+    PAUSEABLE_ONCE(mt_pause_once_before_register_mutation) \
```

```diff
diff --git a/src/Storages/StorageMergeTree.cpp b/src/Storages/StorageMergeTree.cpp
@@
     extern const char mt_pause_before_register_mutation[];
+    extern const char mt_pause_once_before_register_mutation[];
@@
     FailPointInjection::pauseFailPoint(FailPoints::mt_pause_before_register_mutation);
+    FailPointInjection::pauseFailPoint(FailPoints::mt_pause_once_before_register_mutation);
```

Use `mt_pause_once_before_register_mutation` throughout the new test. Then, before the fix, the first mutation consumes the one-shot pause and the second mutation can register first as claimed.

### 2. Correct the test tags

```diff
-# Tags: no-parallel, no-shared-merge-tree
+# Tags: no-parallel, no-replicated-database, no-shared-merge-tree
```

Renumber the files using `tests/queries/0_stateless/add-test mutations_register_in_order.sh` after rebasing.

### 3. Clarify the `startMutation` comment

```diff
-    /// registered yet. Every caller holds the alter lock here (see `mutate` and `alter`), so a
-    /// second mutation cannot allocate a higher block number and register it before this one.
+    /// registered yet. `mutate` holds the alter lock across this function, while mutation-producing
+    /// metadata alters are registered separately under the same lock, so another mutation cannot
+    /// allocate a higher block number and register it first.
```

### 4. File the merge issue separately

Minimal follow-up direction:

```diff
diff --git a/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp b/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp
@@
     {
         uint64_t left_mutation_version = storage.getCurrentMutationVersion(left.info.getDataVersion(), merge_mutate_lock);
         uint64_t right_mutation_version = storage.getCurrentMutationVersion(right.info.getDataVersion(), merge_mutate_lock);

         if (left_mutation_version != right_mutation_version)
             return std::unexpected(PreformattedMessage::create("Parts {} and {} have different mutation version", left.name, right.name));
     }
+
+    const Int64 lower_data_version = std::min(left.info.getDataVersion(), right.info.getDataVersion());
+    const Int64 upper_data_version = std::max(left.info.getDataVersion(), right.info.getDataVersion());
+    for (const auto & block : committing_blocks)
+    {
+        if (block.number > upper_data_version)
+            break;
+
+        if (block.op == CommittingBlock::Op::Mutation && block.number > lower_data_version)
+        {
+            return std::unexpected(PreformattedMessage::create(
+                "Parts {} and {} span mutation block {} that is still being registered",
+                left.name,
+                right.name,
+                block.number));
+        }
+    }
```

Do not fold this separate merge/insert race into v2 unless the PR’s stated scope is expanded accordingly.
