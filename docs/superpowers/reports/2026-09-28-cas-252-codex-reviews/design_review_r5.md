## Verdict (REVISE)

Rev.7 is substantially simpler and more maintainable than rev.6. Once a version is in `pending_versions`, serializing its removal with selection under `currently_processing_in_background_mutex` does close the publication/release-between-captures race without snapshots or epochs.

It is not yet sound as written for three reasons:

1. Version allocation and pending announcement are not atomic with respect to selection.
2. `OPTIMIZE FINAL` releases the mutex but reuses a stale predicate containing captured patch state.
3. The proposed barrier `ALTER` passes a still-pending version to `waitForMutation`, which treats an unregistered version as killed rather than waiting.

## MAJOR findings

### 1. Allocation followed by announcement still leaves the original race—and `MOVE` can invert the lock order

Rev.7 says:

> `prepareMutationEntry` allocates the block and writes the file as today; between them, under the mutex, insert `{version, Mutation}`

and:

> `updateLightweight`: after `allocateBlockNumber(Op::Update)`, insert `{U, LightweightUpdate}`

and:

> `movePartitionToTable`: after `fillNewPartName` for the moved parts, insert `{q, MovedPart}` for each

All three allocate first and announce second. `allocateBlockNumber` only holds `committing_blocks_mutex`:

```cpp
std::unique_ptr<PlainCommittingBlockHolder>
StorageMergeTree::allocateBlockNumber(CommittingBlock::Op op)
{
    std::lock_guard lock(committing_blocks_mutex);
    CommittingBlock block(op, increment.get());
    ...
}
```

([StorageMergeTree.cpp:3617](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3617))

A selector does not inspect `committing_blocks`; it will inspect only the proposed `pending_versions` and registered state. Consequently:

1. Writer A allocates `N`.
2. A is descheduled before inserting `pending_versions[N]`.
3. Writer B allocates and publishes/registers `M > N`.
4. Selection acquires `currently_processing_in_background_mutex`.
5. Selection sees `M` but neither pending nor registered `N`.
6. It crosses `N`.

That reproduces:

- two-mutation inversion;
- update `U` lost under later mutation `M`;
- merge crossing an unregistered mutation;
- merge crossing a split-phase moved part.

For `MOVE`, inserting while still inside its current destination parts-lock scope would instead create `data-parts lock → background mutex`, while selectors use `background mutex → data-parts lock`. The current selector constructs/calls the predicate and collector while holding the background mutex, and both read parts ([StorageMergeTree.cpp:1540](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1540), [MergeTreePartsCollector.cpp:126](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp:126)). That is a direct deadlock possibility.

Exact spec text that closes this MAJOR:

> Allocation and pending announcement are one atomic operation with respect to selection. A helper acquires `currently_processing_in_background_mutex`, allocates the block number while holding that mutex, inserts `{version, kind}` into `pending_versions`, and only then releases the mutex. No selector can observe an allocated version before its pending announcement.
>
> `startMutation` and `updateLightweight` use this helper before mutation-file or patch work begins. `movePartitionToTable` allocates and announces all destination versions before taking the destination data-parts lock; the announced versions are then assigned to the moved-part names under the parts lock. `movePartitionToTable` must not acquire `currently_processing_in_background_mutex` while holding a data-parts lock.
>
> The lock order is `currently_processing_in_background_mutex → committing_blocks_mutex`. This order already exists in merge-predicate construction. Pending cleanup erases under the background mutex, releases it, and only then triggers the background assignee.

### 2. `OPTIMIZE FINAL` does not hold the one mutex throughout its decision

The ordinary background and explicit merge entry points do acquire the mutex around `selectPartsToMerge`:

```cpp
std::unique_lock lock(currently_processing_in_background_mutex);
return selectPartsToMerge(..., lock, ...);
```

([StorageMergeTree.cpp:1720](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1720), [StorageMergeTree.cpp:2040](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2040))

The collector also reads parts synchronously while that lock is held.

But `OPTIMIZE FINAL` releases it here:

```cpp
currently_processing_in_background_condition.wait_for(lock, timeout)
```

([StorageMergeTree.cpp:1654](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1654))

The predicate and collector were constructed once, before the retry loop:

```cpp
auto merge_predicate = std::make_shared<MergeTreeMergePredicate>(*this, lock);
auto parts_collector = std::make_shared<MergeTreePartsCollector>(
    *this, txn, merge_predicate);
```

([StorageMergeTree.cpp:1540](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1540))

The collector rereads active parts on every retry, but the predicate retains the old:

- `committing_blocks`;
- `min_update_block`;
- `patches_by_partition`.

([MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22))

Counterexample:

1. `OPTIMIZE FINAL` constructs its predicate without update `U`.
2. It cannot select because another merge is active and waits, releasing the background mutex.
3. `U` allocates, publishes its patch, erases its pending version, and completes.
4. The active merge finishes and wakes `OPTIMIZE`.
5. `OPTIMIZE` reacquires the mutex and recollects parts, but reuses the predicate that has no patch `U` and no `min_update_block = U`.
6. It can select a merge without materializing the now-visible patch.

Thus the “pending or visible, never neither” handoff is true at mutex acquisition, but the reused predicate does not reread the visible state.

Exact spec text that closes this MAJOR:

> A merge-selection attempt may use a predicate and collector only while the `currently_processing_in_background_mutex` acquisition under which that predicate was constructed remains uninterrupted. After `currently_processing_in_background_condition.wait_for` releases and reacquires the mutex, `OPTIMIZE FINAL` constructs a fresh `MergeTreeMergePredicate` and `MergeTreePartsCollector` before rereading parts. No predicate containing captured patch or committing-block state is reused across an unlock.
>
> This is a localized construction change in `StorageMergeTree::selectPartsToMerge`; the collector, predicate, patch utilities, and compaction interfaces remain unchanged.

### 3. The barrier `ALTER` does not wait for a pending mutation

Rev.7 says:

> A barrier `ALTER` ... now also considers the highest pending mutation version

But the proposed next step—calling the existing `waitForMutation`—does not work. `getIncompleteMutationsStatusUnlocked` starts with:

```cpp
auto current_mutation_it = current_mutations_by_version.find(mutation_version);
if (current_mutation_it == current_mutations_by_version.end())
    return {};
```

([StorageMergeTree.cpp:1198](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1198))

`waitForMutation` interprets that empty result as terminal:

```cpp
return !mutation_status
    || mutation_status->is_done
    || !mutation_status->latest_fail_reason.empty();
```

and its final check reports:

```cpp
throw Exception(ErrorCodes::UNFINISHED, "Mutation {} was killed", ...);
```

([StorageMergeTree.cpp:1019](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1019), [MergeTreeMutationStatus.cpp:16](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeMutationStatus.cpp:16))

Therefore an alias barrier either overtakes the pending mutation or reports that it was killed; it does not wait for registration. This matters because `StorageAlias::alter` forwards the alias’s lock rather than acquiring the target’s lock ([StorageAlias.cpp:219](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219)).

Exact spec text that closes this MAJOR:

> A barrier that selects a pending mutation version first waits for that version to leave `pending_versions`. The wait predicate and transition are guarded by `currently_processing_in_background_mutex`; every pending mutation registration, failure removal, and rollback removal notifies `currently_processing_in_background_condition`.
>
> Once the pending version disappears, the barrier checks under the same mutex whether the version was registered. If registered, it releases the mutex and calls `waitForMutation`; if absent, preparation failed or rolled back and there is no mutation to wait for. `waitForMutation` itself remains map-only and is never called for a still-pending version.

## MINOR findings

- The exclusive patch-bound claim is not provided by `getNextMutationVersion` alone. `getPatchesToApplyOnMerge` includes equality because it skips only `max_patch_version > next_mutation_version` ([PatchPartsUtils.cpp:291](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:291)). Equality is currently excluded by the retained `min_update_block` filtering, provided the update’s committing-block holder outlives pending erasure. Either specify that ownership order explicitly or change the utility comparison to `>=`.

- A generic RAII destructor that locks the background mutex can self-deadlock if destroyed while `addPreparedMutationEntry` is unwinding under that mutex. The guard needs an explicit “erase while already locked and disarm” operation. Its fallback destructor may acquire the mutex only when the caller does not hold it.

- The accessor names and comments become misleading: a `MovedPart` version is not a mutation. There are only three plain-MergeTree call sites, so `getCurrentVersionBoundary` and `getNextVersionBoundary` would be clearer than expanding the documented meaning of “mutation version.”

- Pending versions have no direct observability. `system.mutations` should remain map-only, but debug logging of pending insert/erase, including version and kind, is warranted. A leaked pending entry otherwise manifests only as postponed work or `OPTIMIZE` refusal.

- `select_in_partition` can sleep in one-second intervals while holding the background mutex during memory-pressure polling ([StorageMergeTree.cpp:1625](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1625)). That already delays mutation registration; rev.7 extends the delay to update and `MOVE` announcement/cleanup.

## Answers (1–6)

### 1. Soundness against rounds 1–4

- **Two mutations:** Unsound as written because of the allocation-to-announcement gap. After atomic allocation plus announcement, the range bound is sound: for part version `d`, iterate registered mutations from `upper_bound(d)` only to `lower_bound(first pending version > d)`.

- **Update versus mutation, including settings `ALTER`:** The pending-map design correctly removes PR #119155’s mutable `supportsLightweightUpdate` guard. A settings `ALTER` cannot hide an already-announced update. The allocation gap must first be closed.

- **Merge versus mutation:** Sound after atomic announcement. While selection holds the mutex, registration cannot replace pending state; it sees either pending `N` or registered `N`. The current `canMergeParts` calls both accessors under the supplied lock ([MergeTreeMergePredicate.cpp:50](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:50)).

- **Merge versus update:** For a normal uninterrupted selection, rev.7 closes the round-3 publication/release gap. Patch publication can occur while selection holds the background mutex, but pending erasure cannot, so the boundary remains visible. Publication before mutex acquisition means the newly constructed predicate captures the patch. The `OPTIMIZE FINAL` retry remains unsafe because it unlocks and reuses captured predicate state.

- **`MOVE` split phase:** A pending moved-part version is the correct boundary. After destination commit, `Transaction::commit` makes the part active under the parts lock ([MergeTreeData.cpp:9416](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:9416)); erasure can then occur under the background mutex after releasing the parts lock. Allocation/announcement ordering and the parts-lock inversion must be fixed.

- **Failure release:** Semantically sound if the guard remains until no invisible state can later publish. Mutation failure publishes no map entry. An update may publish a subset of partition patches before failing because `finishDelayedChunk` commits partitions individually ([MergeTreeSinkPatch.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.cpp:28)); after failure, those patches are visible and later selectors can capture them. A failed `MOVE` must erase only after destination transaction unwinding has completed. All three erasures can be placed under the background mutex, but the guard API and destruction order must be explicit.

- **Transaction rollback:** Not solved by pending versions. `prepareMutationEntry` adds the mutation to the transaction before registration ([StorageMergeTree.cpp:919](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:919)); rollback calls map-only `killMutation` ([MergeTreeTransaction.cpp:329](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/MergeTreeTransaction.cpp:329)). This 26.6 tree therefore requires the specified upstream registration-time rollback recheck as a prerequisite.

- **Orphan final mutation file:** Not solved and correctly scoped as a restart limitation. `loadMutations` treats every final `mutation_*` file as registered ([StorageMergeTree.cpp:1462](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462)), while destructor deletion is best-effort ([MergeTreeMutationEntry.cpp:182](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeMutationEntry.cpp:182)). File removal should precede pending/block release, but deletion failure still permits replay after restart.

- **Selector lock coverage:** Background selection, explicit `OPTIMIZE`, mutation selection, accessor calls, and synchronous collector reads are covered by the background mutex. The one exception is the `OPTIMIZE FINAL` condition-variable wait and its stale predicate.

### 2. The five §5 questions

1. **Fourth writer:** No fourth allocator/publication gap was found. All plain-MergeTree block allocation sites go through `prepareMutationEntry`, `updateLightweight`, the `NewPart` helpers, and the replace drop-range reservation. Ordinary `INSERT`, `ATTACH`, `REPLACE PARTITION`, and restored attachment allocate and activate while holding one data-parts write lock ([MergeTreeSink.cpp:370](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:370), [StorageMergeTree.cpp:2871](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2871), [StorageMergeTree.cpp:3095](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3095), [StorageMergeTree.cpp:3452](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3452)). `restoreAndActivatePart` and `swapActivePart` publish existing versions under the parts lock; they do not allocate a new version ([MergeTreeData.cpp:6006](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:6006), [MergeTreeData.cpp:6429](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:6429)).

2. **Lock inversions:** Update insertion/erasure adds `lightweight-update lock → background mutex`; no reverse edge was found. Corrected `MOVE` adds `operation_with_data_parts_mutex → destination background mutex`; no reverse edge was found in the reviewed paths. Taking the background mutex from inside the destination parts lock is not safe and must be forbidden. `alter` already uses table lock → background mutex and deliberately avoids overlapping `DatabaseAtomic::mutex` with it ([StorageMergeTree.cpp:524](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:524)).

3. **Accessor consumers:** There are exactly three plain-MergeTree uses: the left and right calls in `MergeTreeMergePredicate::canMergeParts`, and the next-version call in `getPatchesToApplyOnMerge` ([MergeTreeMergePredicate.cpp:51](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:51), [MergeTreeMergePredicate.cpp:102](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:102)). None looks the returned version up in `current_mutations_by_version`; all treat it numerically.

4. **Barrier correctness:** Not correct as specified. `waitForMutation` treats pending-but-unregistered as killed. Add a pending-to-registered/failure wait before calling it.

5. **Anything simpler:** Keep rev.7’s core model, but centralize it in one `allocatePendingVersion(kind, op)` helper and one explicit completion/cleanup guard. Rename the two accessors to boundary terminology. For alias ordering, either implement the pending-registration wait above or acquire the target’s alter lock in `StorageAlias`; do not merely pass pending versions to `waitForMutation`.

### 3. Elegance and minimality versus rev.6

A MergeTree maintainer is much more likely to accept corrected rev.7. It localizes the state to `StorageMergeTree`, uses a mutex already defining selection serialization, removes the epoch and captured-snapshot API, and leaves the predicate/collector internals unchanged.

What can still be dropped:

- Drop PR #119155’s new `removeCommittingBlock` wake-up. Pending erasure should own the wake-up; retaining both creates duplicate triggers.
- Drop the claim that no merge-selection construction change is needed: rebuilding the existing predicate and collector after the `OPTIMIZE FINAL` wait is mandatory.
- Optionally drop the barrier-pending addition if alias ordering is explicitly left out of scope. If retained, it needs the real pending-registration wait.
- Do not drop `PendingVersionKind`; it is needed for accurate `system.mutations.parts_postpone_reasons`.
- Keep `min_update_block` in this PR. Removing it simultaneously would broaden behavior and expose the utility’s inclusive equality semantics.

### 4. Test plan

The principal tests are deterministic with their stated failpoint handshakes:

- `05227`: red for the original update/mutation loss; bound `SYSTEM WAIT FAILPOINT`.
- S1: red before the alter-lock lifetime fix. Explicitly retry B after A is released if the final assertion says both mutations applied.
- S2: red on PR #119155 because the settings change hides the reservation loop; independent of that setting under rev.7.
- S2c: valid if `K` is held at `mt_mutate_task_pause_in_prepare` before arranging `U` and `M`.
- S3: deterministic and red for the merge/mutation crossing.
- S4: direct path is red for the lock lifetime; alias path is red for the missing target serialization and will also expose the broken pending wait.
- S5: deterministic and red for the split-phase `MOVE` loss.

S3b is intentionally green today because `min_update_block` is a conservative superset. Keep it as an integration pin, but do not describe it as red-before-fix. The accessor/range unit test supplies the red test for the new behavior.

Missing coverage:

- Allocation-to-announcement atomicity for each kind.
- A retry test where `OPTIMIZE FINAL` releases the mutex, an update publishes and removes its pending version, and the retry must reconstruct the predicate.
- Mutation preparation failure after announcement, proving cleanup and wake-up.
- `MOVE` failure after destination preactivation, proving no pending entry leaks.
- The transaction-rollback test in the Antalya prerequisite.
- Patch equality at a pending update boundary, including ownership/destruction order.
- A deterministic long-backoff hook if the tests are intended to prove immediate assignee wake-up rather than eventual polling progress.

### 5. Superseding PR #119155

Replacing its guarded `Op::Update` scan with a pending insertion in `updateLightweight` is the correct shape. It removes the mutable-capability TOCTOU and unifies update, mutation, and moved-part ordering.

The insertion cannot literally be “after allocation”; allocation and announcement must be performed by the atomic helper. Preserve:

- `PENDING_LIGHTWEIGHT_UPDATE`;
- `05227`, apart from its timeout;
- the original commits/authorship where practical.

Drop the `removeCommittingBlock` trigger once pending erasure reliably triggers scheduling. The likely upstream objection remains scope expansion, not the abstraction itself; layered commits for core pending state, `MOVE`, lock/barrier behavior, and tests are appropriate.

### 6. Risks

- **Lock ordering:** Safe with `background → committing` and no `parts → background`. The corrected `MOVE` helper must announce before taking the destination parts lock.
- **Contention:** Update and `MOVE` now wait for the background mutex. Memory-pressure polling currently holds it across sleeps, potentially delaying updates and moves for the full selection timeout.
- **Observability:** Pending versions are absent from `system.mutations`; add debug insert/erase logs with kind and version. Preserve existing user-visible postpone strings.
- **Behavior:** Merges and `OPTIMIZE FINAL` can temporarily refuse ranges spanning a pending version. Mutations may make safe partial progress through `K` before stopping at pending `U`.
- **Failure sensitivity:** A leaked pending entry indefinitely blocks later work. RAII plus deterministic failure tests are mandatory.
- **Restart:** Best-effort mutation-file deletion remains a documented durable orphan limitation.
- **Patch-bound coupling:** Leaving `PatchPartsUtils` unchanged makes exclusive equality depend on `min_update_block` and destruction order; document and test that dependency.

## Recommended changes

1. Make block allocation plus `pending_versions` insertion atomic under `currently_processing_in_background_mutex`.
2. Ensure `MOVE` never acquires the background mutex while holding a data-parts lock.
3. Reconstruct the predicate and collector after every `OPTIMIZE FINAL` condition-variable wait.
4. Add an explicit wait for pending mutation registration/failure before barrier `ALTER` calls `waitForMutation`.
5. Define a pending guard with separate “complete while mutex already held” and noexcept fallback-cleanup paths.
6. Specify destruction order: visible-state cleanup/file cleanup, pending erase, then committing-block release where the exclusive patch bound depends on it.
7. Either make `getPatchesToApplyOnMerge`’s bound genuinely exclusive or explicitly retain and test the `min_update_block` dependency.
8. Add the missing atomic-announcement, retry, mutation-failure, and `MOVE`-failure tests.
9. Remove the redundant `removeCommittingBlock` wake-up and rename the accessors to version-boundary terminology.
