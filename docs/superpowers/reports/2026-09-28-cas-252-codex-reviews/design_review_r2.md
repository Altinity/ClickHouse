## Verdict (REVISE)

The boundary invariant is good, but rev.2’s proposed implementation is not sound yet. A reservation snapshot is taken before the merge’s regular-part and patch-part snapshots. A reservation allocated afterward can therefore be missed while the same merge sees later parts or patches and crosses it.

The design needs a coherent merge-input snapshot: capture the regular and patch parts first, then snapshot reservations, and make both the collector and predicate use those captured parts. With that correction, the rule is sound for the three in-process races.

## MAJOR findings

### 1. “One snapshot per decision” is not a coherent snapshot

`MergeTreeMergePredicate` currently snapshots `committing_blocks` in its initializer, then reads patch parts in its body ([MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)). Regular parts are collected even later, when `grabAllPossibleRanges` calls `collectInitial` ([MergeTreePartsCollector.cpp:126](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp:126)). Rev.2 preserves this ordering.

Counterexample with a `Mutation` reservation:

1. Part `P` exists below version `N`.
2. Merge selection constructs the predicate and snapshots no reservations.
3. Mutation `N` allocates `Op::Mutation` and pauses before registration.
4. Insert `Q > N` commits.
5. The parts collector now sees both `P` and `Q`.
6. The predicate’s stale snapshot does not contain `N`; the mutation map cannot contain it because selection holds `currently_processing_in_background_mutex`.
7. `P` and `Q` merge. `FutureMergedMutatedPart::assign` keeps the first part’s `min_block` when neither source has a mutation ([FutureMergedMutatedPart.cpp:56](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/FutureMergedMutatedPart.cpp:56), [MergeTreePartInfo.h:79](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreePartInfo.h:79)).
8. After `N` registers, it applies to the merged part and incorrectly affects rows from `Q`.

`getMaxLevelInBetween` does not help: `Q` is an active selected part, not an outdated part hidden in a gap ([StorageMergeTree.cpp:1987](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1987)).

There is an analogous lost-update counterexample:

1. Predicate snapshots no reservations.
2. Update `U` reserves a block and pauses.
3. Later, disjoint update `V > U` commits. This is permitted because `waitForCommittingInsertsAndMutations` skips lower `Update` reservations ([StorageMergeTree.cpp:3629](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3629)).
4. The later patch snapshot sees `V`, but the earlier reservation snapshot missed `U`.
5. A merge materializes patch `V`, advancing the result to version `V`.
6. Patch `U` later commits below that data version and cannot apply.

The fix is to capture all regular and patch candidate parts first, then capture version reservations. The collector and predicate must consume the same captured parts. If a reservation is allocated after that second snapshot, all data contributing to the decision predates it.

### 2. S3b is not red before rule 3

The existing predicate already computes `min_update_block` and excludes patch parts at or above it ([MergeTreeMergePredicate.cpp:25](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:25), [PatchPartsUtils.cpp:368](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:368)). Therefore the proposed test with pending update `U` present before selection and committed update `V > U` is already green.

Rule 3 is either:

- a refactoring of this existing protection into a per-range bound, in which case it needs equivalence and above-boundary tests; or
- intended to close the late-reservation snapshot race above, in which case S3b must pause selection after the reservation snapshot, then allocate `U` and commit `V`.

The spec also needs to handle the `0` sentinel explicitly. `getNextMutationVersion` returns `0` for “infinity” ([StorageMergeTree.cpp:2136](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2136)), and `getPatchesToApplyOnMerge` converts `0` to `Int64::max` ([PatchPartsUtils.cpp:291](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:291)). A literal:

```cpp
std::min(getNextMutationVersion(dv), reservation->number)
```

produces `0`, which is then interpreted as unbounded and defeats rule 3.

### 3. Round-1 findings are not all closed

| Round-1 item | Rev.2 status |
|---|---|
| Transaction rollback on 26.6 | Partially closed. Upstream’s recheck is verified in `upstream/master:StorageMergeTree::startMutation`; the Antalya dependency remains an open packaging decision. |
| Orphan-file/restart proof | Closed as a scope correction. Reordering destruction narrows the in-process gap but deliberately does not claim durable safety. |
| `StorageAlias::alter` gap | Partially closed. It is documented but still undecided. The alias still passes its own lock to the target ([StorageAlias.cpp:219](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219)). |
| Invalid mirror merge test | Closed. |
| Rule 3 test coverage | Still open: the replacement S3b is green before the proposed change. |
| Failure/cancellation wake-up | Partially closed. The update arm is proposed, but it is timing-sensitive and the `Mutation` failure-release arm is untested. |
| Explicit reservation helper | Closed. |
| Keep mutation accessors map-only | Closed. |
| Preserve PR #119155’s reason string/test | Closed. |
| Drop mutable `supportsLightweightUpdate` guard | Closed. |
| Bound every `SYSTEM WAIT FAILPOINT` | Closed in the plan. |

New in rev.2 are the invalid S3b red claim, the ambiguous `0` upper-bound calculation, and an inconsistency where `Unknown` is treated as a boundary but is neither given an accurate diagnostic nor included in the release trigger.

## MINOR findings

- `KILL QUERY` does not release a thread blocked in `pauseFailPoint`; `disableFailPoint` or `notifyFailPoint` must follow the kill before cancellation can unwind ([FailPoint.cpp:282](/home/mfilimonov/workspace/ClickHouse/master/src/Common/FailPoint.cpp:282)).
- Testing “wakes without a scheduler tick” using a short timeout is not deterministic. The normal backoff is randomized and can be as low as fractions of a second or grow to 600 seconds ([BackgroundJobsAssignee.cpp:52](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/BackgroundJobsAssignee.cpp:52)). Add an observable trigger/failpoint or force the assignee into a known long backoff.
- S3 needs to prevent the ordinary background selector from merging its two parts before `OPTIMIZE`. The existing `mt_merge_selecting_task_pause_when_scheduled` failpoint can pause the background path while explicit `OPTIMIZE` performs selection ([StorageMergeTree.cpp:2019](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2019)).
- If `Unknown` is conservatively a boundary, `removeCommittingBlock` should wake for it as well. Otherwise a future producer can create a postponed decision without a release wake-up.
- The orphan paragraph should distinguish process termination from deletion failure. Termination while the reservation still exists cannot be followed by an in-process crossing; the durable ambiguity is that restart treats the final file as registered.

## Answers (1-6)

### 1. Soundness

- **Two mutations:** Mutation selection is sound when its part set and reservation set form a coherent cut. A lower reservation bounds the map iterator with `lower_bound(N)`, so a registered `M > N` cannot be applied first. Rev.2’s merge path remains unsound because it can miss a newly allocated `N` and later collect `Q > N`, as shown above.
- **Update versus mutation:** Sound after dropping `supportsLightweightUpdate`. The update holder lives in `MergeTreeSinkPatch` through patch commitment ([MergeTreeSinkPatch.cpp:14](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.cpp:14)). Mutation selection either sees the reservation or, after release, can read the committed patch.
- **Merge versus `Mutation`:** Unsound with the specified constructor snapshot ordering. Sound after candidate parts are captured before reservations.
- **Merge versus `Update`:** Likewise unsound for the two-update `U < V` interleaving above. If `U` was already present at snapshot time, existing `min_update_block` protection is sufficient.
- **Merge versus `NewPart`:** `NewPart` should not be a boundary. Allocation and insertion into the part index occur under `lockParts` ([MergeTreeSink.cpp:370](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:370)). This remains safe only if the merge uses captured parts predating its reservation snapshot.
- **Failure release:** Safe in-process when failure publishes no state: an older snapshot is conservative and the release wakes selection. Reorder `PreparedMutationEntry` so file cleanup precedes holder release.
- **Transactional mutation:** Safe upstream only because `startMutation` rechecks rollback after registration under the background mutex. The 26.6 code shown here lacks it: rollback cannot find an unregistered entry, and selection later reaches the `UnknownCSN` exception at [StorageMergeTree.cpp:1859](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1859).
- **Orphan final file:** Not sound across deletion failure and restart. If removal of `mutation_N.txt` fails, the reservation is eventually released; a later mutation can advance parts above `N`; `loadMutations` then loads every final mutation file after restart ([StorageMergeTree.cpp:1462](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462)). The orphan `N` can be silently skipped. Rev.2 correctly scopes this out.

### 2. Elegance and minimality

“Reservations are provisional version boundaries” is the right abstraction. Atomic allocation plus registration is not generally simpler:

- the mutation file is prepared before registration;
- mutation-producing metadata `ALTER` needs its final file before durable metadata commit ([StorageMergeTree.cpp:555](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:555));
- lightweight updates still require a long-lived boundary through patch commit.

A maintainer should prefer the rev.2 accessor direction over three explicit loops, but package it as one typed snapshot abstraction:

```cpp
struct VersionReservations
{
    std::vector<CommittingBlock> blocks; // sorted, only Mutation/Update/Unknown

    const CommittingBlock * firstAfter(Int64 data_version) const;
};

VersionReservations getVersionReservations() const;
```

Keep `getCurrentMutationVersion` and `getNextMutationVersion` map-only. The key missing requirement is not another accessor—it is snapshot ordering and ownership of the captured part set.

Compared with PR #119155’s loop, the general bound also permits useful progress: if registered `K < U < M`, it may apply `K` while postponing `M`; #119155 postpones the whole part after computing target `M`. That is safe but deserves a test.

### 3. Open questions in §5

Rev.2 contains four questions, not five.

1. **Dropping `supportsLightweightUpdate` is sufficient.** Do not block settings `ALTER`. Eligibility to begin another update is irrelevant once an `Op::Update` reservation exists. The capability check reads mutable table settings ([MergeTreeData.cpp:11046](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:11046)); the reservation is the authoritative state.

2. **Keep `PENDING_MUTATION_REGISTRATION` for `Mutation`.** Use the established style: “Mutation with a lower block number is not registered yet.” Do not use that diagnosis for `Unknown`; either add a generic uncommitted-operation reason or document `Unknown` as conservative but currently unreachable.

3. **Include the `StorageAlias::alter` fix.** The PR already claims to restore mutation/metadata ordering and tests barrier `ALTER`. Leaving a known forwarding path that passes the alias lock to the target contradicts that claim. Acquire the target’s alter lock and pass that holder to `target_storage->alter`; add the alias S4 variant.

4. **Land the Antalya transaction recheck as its own PR first.** It is an independent correctness fix with an independent test. Merge it into the 26.6 branch, then base the boundary PR on that result; do not create a stacked PR.

The two former rev.1 questions are also settled correctly: keep `min_update_block` in this PR, and do not treat `NewPart` as a version boundary.

### 4. Test plan

- **`05227`:** Deterministic and red for the original update/mutation race. Preserve it and add the failpoint-wait timeout.
- **S1:** Deterministic for lock acquisition. To deterministically prove the data-loss consequence before the fix, wait for B to finish while A remains paused before releasing A.
- **S2 cancellation:** Needs `KILL QUERY`, then failpoint release, then client reap. It does not deterministically prove immediate wake-up without additional scheduling instrumentation.
- **S2b:** Correctly targets the audit finding. Keep the setting disabled until the postpone reason is observed. Using `mt_mutate_task_pause_in_prepare` can make the defective branch explicit.
- **S3:** Correct semantics, but prevent the background selector from consuming the parts before explicit `OPTIMIZE`.
- **S3b:** Not red before rule 3. Replace it with the late-reservation test: pause merge selection after its reservation snapshot, allocate pending `U`, commit later `V`, then resume.
- **S4:** Deterministic for a direct table. Add the alias variant if the alias fix is included.
- **Unit test:** Good for classification and strictness, but it does not exercise rule 1’s `Mutation` map bound.
- **Missing:** a `Mutation` reservation released by failure that wakes a previously refused merge, the coherent-snapshot race, and the transaction rollback test in the Antalya prerequisite.

### 5. Superseding PR #119155

Replace its explicit loop in a later commit; do not retain two final loops. Preserve:

- PR #119155’s commits and authorship;
- `PENDING_LIGHTWEIGHT_UPDATE` and its exact string;
- `05227` as the focused regression test.

Changing the existing reason string would be unnecessary user-visible churn through `system.mutations.parts_postpone_reasons`. The general rule should add a distinct mutation-registration diagnosis.

The likely author objection is scope: their focused fix becomes dependent on mutation registration, merge selection, patch selection, alias locking, transaction prerequisites, and cleanup ordering. They may also ask for proof of the less-conservative `K < U < M` behavior. Keeping their commits intact and layering the generalization afterward is the least contentious form.

### 6. Risks

- **User-visible behavior:** More mutations can be temporarily postponed; `OPTIMIZE FINAL` can temporarily report no assignable merge across an in-flight mutation.
- **Observability:** The new mutation reason is exposed through `system.mutations`. Preserve the existing lightweight-update string.
- **Performance:** Filtering reservations costs `O(committing_blocks)` once per selection; binary search is then `O(log reservations)` per part. Mutation selection gains this cost only while mutations exist. Merge selection already copies the complete set, so replacing it with a filtered vector should not regress that path.
- **Snapshotting:** Capturing regular and patch parts once can reduce duplicate parts snapshots relative to the current predicate-plus-collector flow.
- **Lock ordering:** Background selection already holds `currently_processing_in_background_mutex`. Snapshotting parts and then briefly snapshotting reservations without overlapping the two locks adds no cycle. `removeCommittingBlock` must release `committing_blocks_mutex` before calling `background_operations_assignee.trigger`.
- **Scheduling:** Successful mutation registration already triggers the assignee, so triggering again when the `Mutation` holder is destroyed is redundant but harmless. The trigger is necessary on pre-registration failure.
- **Orphan files:** Reordering destruction narrows exposure but does not provide durable registration state.

## Recommended changes to the design (concrete text for the spec)

1. Replace §1’s snapshot description with:

> A decision must use a coherent cut of its inputs, not merely one reservation snapshot. Capture the regular and patch parts that may contribute to the decision first, then snapshot `Mutation`, `Update`, and `Unknown` reservations. The decision must use only those captured parts. A reservation allocated after the second snapshot is therefore newer than every part and patch the decision can materialize.

2. Replace the merge-predicate construction text with:

> `selectPartsToMerge` captures active regular and patch parts once. It then captures `VersionReservations` and passes both snapshots to `MergeTreePartsCollector` and `MergeTreeMergePredicate`. The predicate must not reread patch parts, and the collector must not reread regular parts.

3. Clarify rule 3:

> Existing `min_update_block` filtering already prevents a merge from materializing a committed update above a pending update that was present in the snapshot. The new per-range boundary replaces or generalizes that filtering; it must not coexist ambiguously as a second independent rule. Treat `getNextMutationVersion(...) == 0` as infinity before combining it with a reservation bound.

4. Replace S3b with:

> Pause merge selection immediately after its reservation snapshot. Allocate and pause lightweight update `U`; commit a disjoint update `V > U`; resume selection. The merge must either retain the earlier captured patch set or observe `U` in a later reservation snapshot, and must not materialize `V` across `U`. This is red with reservation-first/parts-later snapshotting.

5. Add:

> A second merge test pauses selection after the reservation snapshot, allocates mutation `N`, commits insert `Q > N`, and resumes. `Q` must not enter that selection, or the later reservation snapshot must reject the merge.

6. Amend rule 4:

> Releasing `Mutation`, `Update`, or conservatively `Unknown` triggers the background assignee after `committing_blocks_mutex` is released. Test the `Mutation` failure-release path separately from successful registration.

7. Resolve the two remaining choices rather than leaving them open:

> The pull request fixes `StorageAlias::alter` by acquiring the target table’s alter lock and adds the alias barrier test. The Antalya transaction rollback recheck lands first as an independent pull request and prerequisite.
