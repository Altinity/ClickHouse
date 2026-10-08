## Verdict — REVISE

The captured-fence algorithm fixes the round-6 counterexample and is correct for ordinary registration, terminal preparation failure, `killMutation`, and transactional rollback with the stated prerequisite.

One uncovered transition remains: `StorageMergeTree::alter` can temporarily remove an already-registered mutation while attempting durable metadata rollback, then re-register the same version if that rollback fails. During that interval the version is neither registered nor pending, so a barrier can pass its fence. This also contradicts rev.9’s central selector-coherence invariant.

Apart from that transition and the missing failed-fence regression test, every round-6 item is closed.

## MAJOR findings

### 1. Post-registration `ALTER` rollback creates a hole in the fence

The current non-replicated `alter` recovery path:

```cpp
held_entry.emplace(std::move(it->second));
current_mutations_by_version.erase(it);
...
background_lock.unlock();

database->alterTable(... old_metadata ...);

if (!durable_rolled_back)
{
    std::lock_guard relock(currently_processing_in_background_mutex);
    current_mutations_by_version.try_emplace(held_version, std::move(*held_entry));
}
```

It removes the mutation at [StorageMergeTree.cpp:677](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:677), releases `currently_processing_in_background_mutex` at [StorageMergeTree.cpp:696](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:696), attempts durable rollback, and may re-register the same version at [StorageMergeTree.cpp:738](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:738).

Counterexample:

1. Rename mutation `P` is pending.
2. An alias barrier captures fence `P`.
3. `P` registers, erasing its pending entry.
4. `mt_alter_throw_after_mutation_registered` or a real in-memory commit failure enters recovery.
5. Recovery removes registered `P`, unlocks the background mutex, and starts durable rollback.
6. The barrier wakes, observes no pending mutation `<= P` and no registered mutation `<= P`, and proceeds.
7. Durable rollback fails; recovery re-registers `P` and publishes the new metadata.

The barrier has overtaken a mutation exactly at its fence. This is reachable through `StorageAlias`, which forwards the alias’s lock rather than acquiring the target alter lock at [StorageAlias.cpp:219](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219). An alias-issued failing `ALTER` can also expose the gap to merge selection.

It directly falsifies the spec’s claim:

> “a selection sees either the pending version or the already-visible state … never neither”

[rev.9 §1](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:128)

Exact spec text that closes it, to append to the `startMutation` writer bullet:

> The post-registration recovery path in `StorageMergeTree::alter` preserves the same invariant. If recovery temporarily removes a registered mutation into `held_entry` before attempting durable metadata rollback, it atomically changes that version from registered back to `pending_versions[version] = Mutation` while still holding `currently_processing_in_background_mutex`. The re-armed pending guard remains live across the durable rollback attempt. If durable rollback succeeds, recovery performs the mutation-file cleanup and then erases the pending version as terminal failure. If durable rollback fails, recovery atomically re-registers `held_entry`, republishes `new_metadata`, and erases the pending version under the background mutex. Both terminal transitions notify `currently_processing_in_background_condition`. A version that may still be re-registered is never absent from both `current_mutations_by_version` and `pending_versions`.

Also replace the final sentence of the first §4 bullet with:

> Therefore, at every selection the version is pending, visible, or terminally abandoned. Temporary unregistration during metadata rollback remains represented as pending until durable rollback either succeeds or the mutation is re-registered.

That preserves the one-mutex design; no epoch or snapshot is needed.

## MINOR findings

- Round 6 explicitly requested a regression with unfinished registered `N`, pending fence `P > N`, and failed `P`. Rev.9 fixes the algorithm but S4 still contains only the original single-pending case at [the spec:222](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:222).

  Append this exact text to S4:

  > Additional fence arms cover: unfinished registered `N` plus pending `P > N` whose preparation fails, proving the barrier still waits for `N`; two pending mutations where the lower one fails; failure of the pending mutation exactly at the fence; and a mutation allocated after fence capture, proving it is not waited for. A post-registration metadata-rollback arm pauses after registered-to-pending conversion and exercises both durable-rollback success and failure/re-registration. A focused helper/unit test covers these fence-state transitions if arranging multiple alias writers in a stateless test is impractical.

- All other round-6 edits are closed: atomic announcement, fresh `OPTIMIZE FINAL` state, accessor names, explicit erase-and-disarm, reason kinds, update wake-up ownership, Debug logging, patch equality ownership, S1 retry wording, deterministic `MOVE` failure, eventual-versus-immediate wake-up wording, and corrected publication wording.

## Answers

### 1. Soundness against rounds 1–4

- **Two mutations:** Sound after the lock fix and pending end bound. The current `if` initializer releases `alter_lock` before `startMutation` at [StorageMergeTree.cpp:1086](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1086). Keeping it alive prevents ordinary inversion; pending versions additionally stop `selectPartsToMutate` at the first lower unregistered version. Current selection otherwise iterates to `current_mutations_by_version.end()` at [StorageMergeTree.cpp:1804](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1804).

- **Update versus mutation, including the settings guard:** Sound. `updateLightweight` currently allocates `Op::Update` at [StorageMergeTree.cpp:1140](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1140) and transfers its holder into `MergeTreeSinkPatch`. An unconditional pending insertion removes PR #119155’s mutable `supportsLightweightUpdate` guard. Pending erase must precede block-holder release.

- **Merge versus mutation:** Sound. `canMergeParts` compares the two current boundaries at [MergeTreeMergePredicate.cpp:50](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:50); including pending versions prevents a merge across an unregistered mutation.

- **Merge versus update:** Sound, including publication-and-release between captures. While the update is pending, a selector holding the background mutex sees its boundary and the live `min_update_block`. If erase wins before selection, patch activation has already completed under the parts lock and the freshly constructed predicate sees it. `OPTIMIZE FINAL` must rebuild after its condition-variable wait at [StorageMergeTree.cpp:1662](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1662).

- **`MOVE` split phase:** Sound. Current destination allocation and preactivation occur before the later `dest_transaction.commit` at [StorageMergeTree.cpp:3260](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3260). Announcing before `lockParts`, retaining the guard until commit, and destroying `dest_transaction` before failure cleanup covers this window. Activation occurs under the parts lock at [MergeTreeData.cpp:9539](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:9539).

- **Failure release:** Mutation preparation, update partial publication, and `MOVE` rollback are sound with the specified RAII order. Post-registration metadata rollback is the sole uncovered failure transition identified above.

- **Transactional rollback while pending:** Sound only with the stated prerequisite. `prepareMutationEntry` records the mutation in its transaction at [StorageMergeTree.cpp:921](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:921), while rollback calls map-only `killMutation` at [MergeTreeTransaction.cpp:340](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/MergeTreeTransaction.cpp:340). On this raw 26.6 tree, rollback can miss the pending mutation. The prerequisite registration-time state recheck must reject registration; its guard then erases and notifies.

- **Orphan final file:** Still outside the claimed guarantee and accurately documented. Reordering destruction narrows the live-process window but cannot make failed file deletion restart-safe.

- **Selector coverage:** Confirmed. Background selection owns the mutex at [StorageMergeTree.cpp:2038](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2038), explicit `OPTIMIZE` at [StorageMergeTree.cpp:1720](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1720), and mutation selection receives the same lock. The collector reads parts synchronously at [MergeTreePartsCollector.cpp:126](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp:126). The only unlock is the `OPTIMIZE FINAL` wait.

### Barrier fence cases

Assuming the registered-to-pending correction above:

- With pending `P < Q = fence`, failure of `P` does not release the barrier because `Q <= fence` remains pending. Once all such entries resolve, the highest registered version `<= fence` is selected.
- A mutation newly announced after capture receives a version greater than the fence because allocation and announcement occur under the mutex. It is excluded from both the wait predicate and final `upper_bound(fence)` lookup.
- If the mutation exactly at the fence fails preparation, the final lookup falls back to the highest earlier registered mutation.
- If it registers and later fails execution, `waitForMutation` reports that failure.
- If it is killed before the final lookup, the barrier falls back to an earlier registered mutation. If killed after selection, existing `waitForMutation` semantics report “killed”; missing entries are treated that way at [StorageMergeTree.cpp:1201](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1201).
- Direct target paths are primarily serialized by the corrected alter-lock lifetime. Alias paths require the fence because `StorageAlias::alter` does not acquire the target alter lock.
- A pending transactional mutation that rolls back remains pending until the registration-time recheck rejects it and guard cleanup notifies the barrier.

### 2. The five §5 questions

1. **Fourth writer:** None. Ordinary `INSERT`, `ATTACH`, `REPLACE PARTITION`, and restored attachment allocate and activate under one parts-lock critical section. `restoreAndActivatePart` and `swapActivePart` publish existing versions rather than allocate new ones.

2. **Lock inversion:** None in the proposed orders. They are update lock → background → committing, and operation-with-parts → destination background → committing. `MOVE` must never take the background mutex under a destination parts lock.

3. **Accessor consumers:** Exactly three plain-`MergeTree` calls: two current-boundary calls in `canMergeParts` and one next-boundary call in patch selection. None subsequently indexes `current_mutations_by_version`.

4. **Barrier:** The fence arithmetic is correct. Its lifecycle assumption needs the registered-to-pending rollback handoff above.

5. **Anything simpler:** Nothing material can be dropped. `PendingVersionKind`, both boundary accessors, the mutate end bound, retained `min_update_block`, retry reconstruction, RAII cleanup, and the alias-aware barrier each serve a distinct correctness obligation.

### 3. Elegance and minimality versus rev.6

A MergeTree maintainer is much more likely to accept rev.9’s pending-map design than rev.6’s reservation snapshot and boundary-handoff epoch. It centralizes ordering in `StorageMergeTree`, uses the mutex selectors already own, and leaves `MergeTreePartsCollector`, patch utilities, and `MergeTreeData::Transaction` unchanged.

The rollback correction is one extra state transition, not a return to snapshot machinery. Nothing else should be dropped.

### 4. Test plan

S1, S2, S2c, S3, S4, S5, and S6 are deterministic and red for their stated race. S3b is intentionally green today and is correctly described as an invariant pin. S7 and the unit tests are implementation/failure-path guards rather than necessarily red on the unpatched base.

Still required:

- The round-6 failed-highest-pending barrier test.
- Fence-state coverage for two pending mutations, exact-fence failure, and post-fence allocation.
- A deterministic post-registration rollback-gap test covering both terminal durable rollback and re-registration.
- The transactional rollback test from the Antalya prerequisite.

### 5. Superseding PR #119155

No objection to replacing its `Op::Update` scan with a pending insertion in `updateLightweight`. It removes the settings-dependent TOCTOU and generalizes the boundary consistently.

The insertion must be inside the atomic allocation helper, and the pending guard must move into `MergeTreeSinkPatch` with destruction ordered as:

1. finish or unwind patch publication;
2. erase pending and notify/trigger;
3. release the committing-block holder;
4. release the lightweight-update lock.

Keep `PENDING_LIGHTWEIGHT_UPDATE` and bounded failpoint waits; drop PR #119155’s `removeCommittingBlock` trigger.

### 6. Risks

- **Lock ordering:** No cycle found. The strict prohibition on parts lock → background mutex remains essential.
- **Contention:** Updates and `MOVE` now wait for the background mutex during announcement and cleanup. `select_in_partition` can hold it during one-second memory-pressure polling at [StorageMergeTree.cpp:1625](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1625).
- **Observability:** Debug logs for every pending insert, registered-to-pending transition, erase, and kind are sufficient. Pending entries should remain outside `system.mutations`.
- **Behavior:** Merges spanning a pending boundary are temporarily refused; mutations may still apply a registered prefix below it; alias barriers wait longer and may propagate existing killed/failed-mutation errors.
- **Liveness:** A leaked pending entry blocks later work indefinitely, so RAII and deterministic exception testing are mandatory.
- **Compatibility:** No on-disk, wire, or system-table format change.

## Recommended changes

1. Add the exact registered-to-pending recovery text from MAJOR 1.
2. Amend §4’s invariant as quoted above.
3. Extend S4 with the exact fence arms described under MINOR findings.
4. Add a pause failpoint after post-registration conversion back to pending, before durable rollback, and test both rollback outcomes.
5. Keep every other rev.9 decision unchanged.

No implementation or tests were run; this was a design review against the unpatched 26.6-based tree.
