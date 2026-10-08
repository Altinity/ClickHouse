## Verdict — REVISE

The provisional-boundary rule is sound for the three in-process data-version races, provided every mutation/merge/patch decision uses one consistent snapshot. It is a good unifying invariant.

The design is not ready as written because §4 overclaims transaction, cleanup, and restart safety; the metadata-ordering fix is bypassable through `StorageAlias`; and the test plan neither exercises rule 3 nor correctly implements the claimed “mirror” merge case.

## MAJOR findings

### 1. Transaction rollback remains unsafe on the 26.6 branch

`prepareMutationEntry` associates the mutation with the transaction before registration ([StorageMergeTree.cpp:897](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:897), [StorageMergeTree.cpp:920](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:920)). If rollback runs while the mutation is paused, `killMutation` cannot find it in `current_mutations_by_version`. Registration then succeeds after the transaction is gone, and `selectPartsToMutate` reaches the `UnknownCSN` exception path.

Current upstream rechecks the transaction immediately after registration while holding `currently_processing_in_background_mutex`, erases a concurrently rolled-back entry, and removes its file. That logic exists in upstream `startMutation` but is absent from this branch. [Upstream implementation](https://github.com/ClickHouse/ClickHouse/blob/c9cb86303821b670b6b25ff44dabb7d39555312a/src/Storages/StorageMergeTree.cpp#L1159-L1215).

It is acceptable for the upstream PR to rely on that existing code. It is not acceptable to call transaction rollback out of scope for the Antalya backport: the corresponding upstream fix must be included as a prerequisite or backported in the same series.

### 2. The restart/orphan proof is false

`PreparedMutationEntry` declares `entry` before `block_holder`, so destruction runs in the opposite order: the holder releases the reservation first, then the entry attempts orphan-file deletion ([StorageMergeTree.h:224](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.h:224)). The same ordering exists among the locals in `prepareMutationEntry`: `entry` is constructed before `block_holder` ([StorageMergeTree.cpp:904](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:904)).

Deletion is best-effort; the destructor catches and logs failure ([MergeTreeMutationEntry.cpp:182](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeMutationEntry.cpp:182)). Therefore:

1. Nontransactional mutation `N` writes `mutation_N.txt`.
2. Registration fails.
3. Its reservation is released.
4. File removal fails, or the process terminates before removal.
5. On restart, `loadMutations` treats every final `mutation_*.txt` as registered and loads `N` ([StorageMergeTree.cpp:1475](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1475)).
6. A mutation whose client saw failure may unexpectedly execute; if parts advanced above `N` after reservation release, it may instead be silently skipped for those parts.

Reordering cleanup before reservation release narrows the same-process gap, but a durable distinction between “final file” and “registered mutation” is required to eliminate the restart ambiguity completely. At minimum, §4 must stop claiming orphaned files are fully handled.

### 3. The lock fix does not cover metadata `ALTER` through `StorageAlias`

Direct `ALTER` acquires the table’s alter lock in `InterpreterAlterQuery` ([InterpreterAlterQuery.cpp:313](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterAlterQuery.cpp:313)), and the proposed `mutate` change correctly retains the same target lock through registration.

But `StorageAlias::alter` forwards the lock holder acquired for the alias directly to the target without acquiring the target’s alter lock ([StorageAlias.cpp:219](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219)). Conversely, `StorageAlias::mutate` calls the target’s `mutate`, which does acquire the target lock ([StorageAlias.cpp:296](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:296)).

Thus a direct mutation paused during registration can still be overtaken by a metadata `ALTER` issued through an alias. The version-boundary rule prevents unsafe physical selection, but it does not stop the metadata from changing before the older mutation registers. Either fix alias locking or narrow the lock-fix claim and add a related issue.

### 4. The test plan does not cover the full proposed rule

The second S3 arm is not a mirror counterexample. A merge of two unmutated parts sets:

- `min_block` from the first part;
- `mutation` to the maximum source mutation, which is zero.

See [FutureMergedMutatedPart.cpp:56](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/FutureMergedMutatedPart.cpp:56) and `getDataVersion = mutation ? mutation : min_block` in [MergeTreePartInfo.h:79](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreePartInfo.h:79). Therefore the merged part remains below `N`; `DELETE WHERE id = 0` subsequently applies correctly. It does not demonstrate that `N` was skipped.

The actual “advance above `N`” case requires applying a later patch or mutation to the merge. There is also no direct test for:

- rule 3, patch selection bounded by a reservation;
- rule 1’s `Mutation` arm;
- rule 4 when a reservation is released by failure/cancellation rather than success;
- transaction rollback;
- orphan-file restart behavior.

## MINOR findings

- §4 incorrectly says every holder is destroyed under `currently_processing_in_background_mutex`. That is true for successful mutation registration, but not for an `Update`: its holder belongs to `MergeTreeSinkPatch` and outlives patch commit ([MergeTreeSinkPatch.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.cpp:28)). The rule remains safe because release occurs after patch commit, not because it uses the background mutex.
- Calling an `Update` or `Unknown` block a “current mutation version” weakens the meaning of established accessors. The existing declarations explicitly describe registered mutations ([StorageMergeTree.h:270](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.h:270)).
- A helper that scans the unfiltered `CommittingBlocksSet` for every part can become `O(parts × in-flight inserts)`, because most entries may be `NewPart`. Pre-filter version reservations once into a sorted local vector.
- `Op::Unknown` is not currently produced by plain `StorageMergeTree`; its allocations always pass a concrete operation ([StorageMergeTree.cpp:3617](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3617)). Treating it conservatively is reasonable future-proofing.
- S1 needs to say explicitly that mutation B is retried after A is released. On the fixed build, the first attempt at B times out, so otherwise there are not “both” mutations to complete.

## Answers

### 1. Soundness

| Case | Result |
|---|---|
| Two mutations | Sound in-process. The lock fix serializes ordinary creation. Independently, bounding selection by the first reservation above the part prevents a registered higher mutation from crossing a lower unregistered one. |
| Update vs mutation | Sound. A registered mutation above an `Update` reservation is excluded until the patch has committed. `updateLightweight` keeps the holder in the sink through patch publication ([StorageMergeTree.cpp:1145](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1145)). |
| Merge vs `Mutation`/`Update`/`Unknown` | Sound if merge comparison includes the same reservation snapshot. A boundary in `(left data version, right data version]` makes the parts non-mergeable. |
| Merge vs `NewPart` | No new boundary is needed. Insert allocation, part indexing, and commit occur while holding `lockParts` ([MergeTreeSink.cpp:370](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:370)); the part is inserted into `data_parts_indexes` before the lock is released ([MergeTreeData.cpp:5575](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:5575)). A selector therefore sees either no allocation yet or the intervening part. `getMaxLevelInBetween` additionally guards indexed gaps ([StorageMergeTree.cpp:1987](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1987)). |
| Reservation released after handled failure | Safe in-process: no state will register, a decision with an older snapshot remains conservative, and rule 4 wakes selection. |
| Mutation in a transaction | Unsafe on this branch without upstream’s registration-time rollback recheck. |
| Restart with orphaned final mutation file | Unsafe/ambiguous, as described in Major finding 2. |

The map-only status and cleanup paths remain valid once crossing is impossible: `getIncompleteMutationsStatus` tests parts against the registered version ([StorageMergeTree.cpp:1248](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1248)), while `clearOldMutations` uses the minimum part data version ([StorageMergeTree.cpp:2153](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2153)). They do not themselves need reservation awareness.

### 2. Elegance and minimality

“Reservations are provisional version boundaries” is the right invariant. Making allocation and registration atomic is less attractive:

- the temporary file can be written before allocation, but final rename would then occur while holding the hot background-selection mutex;
- mutation-producing metadata `ALTER` deliberately needs the durable file before durable metadata commit, so allocation and registration cannot simply be collapsed without restructuring that transaction;
- it would not solve lightweight updates.

I would not overload `getCurrentMutationVersion` and `getNextMutationVersion` so that they sometimes return an update or unknown reservation. A maintainer is more likely to prefer:

```cpp
using VersionReservations = std::vector<CommittingBlock>;

VersionReservations getVersionReservationsSnapshot() const;
const CommittingBlock * firstVersionReservationAfter(
    Int64 data_version,
    const VersionReservations & reservations);
```

Then:

- `selectPartsToMutate` uses the returned number as its map end bound;
- `canMergeParts` keeps the existing registered-mutation comparison and additionally rejects a reservation in the interval;
- patch selection takes `min(getNextMutationVersion(...), reservation number)`.

That gives one abstraction without three copied loops or semantic overloading. Pre-filtering once also avoids repeatedly scanning `NewPart` reservations.

### 3. The five open questions

1. **Use an explicit reservation helper.** Keep the existing mutation accessors map-only. An `Update` reservation is a boundary, not a current mutation.

2. **`min_update_block` is redundant after rules 2 and 3 are implemented correctly.** Rule 2 prevents a regular merge from crossing the pending update; rule 3 prevents it from materializing a later patch across that update. Patch-to-patch merges do not materialize patches into regular data, and `SourcePartsSetForPatch::merge` preserves each source part’s version interval ([SourcePartsSetForPatch.cpp:132](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/SourcePartsSetForPatch.cpp:132), [MergeTask.cpp:900](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTask.cpp:900)). No missed correctness case is visible in this code. Removing the old refusal requires dedicated below/above-boundary tests.

3. **Do not treat `NewPart` as a version boundary.** Allocation and insertion into the part index are atomic under the data-parts lock, as cited above. The spec should name that lock protocol as the primary reason; “block ranges and `getMaxLevelInBetween`” alone is incomplete.

4. **Upstream: already handled. Antalya: in scope as a prerequisite.** Retain upstream’s rollback recheck and backport it to 26.6. Do not duplicate it in the upstream PR.

5. **Keep distinct postpone strings.** `parts_postpone_reasons` is exposed through `system.mutations`; changing the string is observable. Preserve `PENDING_LIGHTWEIGHT_UPDATE` for `Update`, and introduce a separate reason for `Mutation`/`Unknown`. One internal rule does not require one user-facing diagnosis.

### 4. Test plan

- **S1:** Deterministic with `PAUSEABLE_ONCE`; red before the lock fix for the expected timeout. Explicitly retry B after releasing A.
- **S2:** Deterministic and correctly red before PR #119155.
- **S3 first arm:** Valid. Without merge-boundary handling, the merged result remains below `N`, so late registration deletes the post-`N` row.
- **S3 second arm:** Invalid; it describes correct behavior, not a skipped mutation.
- **S4:** Deterministic for direct-table `ALTER`, but add an alias variant or narrow the claim.

Add:

- Two concurrent-update patch tests: one with source parts below pending update `U`, proving later patch `V` is not materialized across `U`; one with all source parts above `U`, proving removal of `min_update_block` does not unnecessarily refuse that merge.
- A cancellation arm for S2: cancel the paused lightweight update, verify no patch is committed, and verify the postponed mutation wakes and completes.
- A focused unit test for classification and “first reservation strictly above data version,” including multiple `NewPart`s and reservations below the part.
- The existing upstream transaction rollback test in the backport.
- A restart/failure test only if the PR claims orphan-file safety.

### 5. Superseding PR #119155

Replacing its explicit `Op::Update` loop with the general helper is technically appropriate. Deleting or weakening its regression test is not.

Keep the test and its existing expected reason unchanged. The PR’s commits explicitly introduced `PENDING_LIGHTWEIGHT_UPDATE` as user-visible diagnosis and assert it in the reference output ([pr119155.diff:13](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/pr119155.diff:13), [pr119155.diff:92](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/pr119155.diff:92)).

The likely author objections are:

- unnecessary churn to a user-visible string;
- loss of a narrowly attributable regression test;
- broadening a reviewed bug fix into mutation, merge, patch, alter-lock, transaction, and cleanup behavior;
- the general bound being less conservative for parts inserted above an older pending update.

Layer the broader rule in subsequent commits, preserve the original test contract, and add new tests for the newly permitted above-boundary behavior.

### 6. Risks

- **Visible behavior:** more mutations postponed behind `Mutation` reservations; merges and `OPTIMIZE` may be temporarily refused. If `min_update_block` is removed, merges wholly above a pending update become newly permitted.
- **Observability:** changing `system.mutations.parts_postpone_reasons` strings may break monitoring or tests.
- **Performance:** `MergeTreeMergePredicate` already copies `committing_blocks` once ([MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)). Mutation selection adds another snapshot only while mutations exist. The main risk is repeated linear scans through `NewPart` reservations; pre-filter once.
- **Locking:** the safe order is background mutex → brief committing-block snapshot, releasing `committing_blocks_mutex` before taking the parts lock. Registration already follows background → committing when the holder is destroyed. `removeCommittingBlock` must release `committing_blocks_mutex` before calling `background_operations_assignee.trigger`, as PR #119155 does. I found no new reverse edge under that protocol.
- **Failure cleanup:** holder/entry destruction order remains a restart-safety hazard.

## Recommended changes to the design

Make these concrete edits:

1. Replace the accessor proposal in §1:

> Snapshot and pre-filter `Mutation`, `Update`, and `Unknown` reservations into a sorted local vector. Keep `getCurrentMutationVersion` and `getNextMutationVersion` limited to registered mutations. Use `firstVersionReservationAfter(data_version)` to bound mutation selection, reject merges spanning a reservation, and lower the patch-selection upper bound.

2. Replace the first two bullets of §4:

> Successful mutation registration transfers the entry into `current_mutations_by_version` and destroys its reservation while holding `currently_processing_in_background_mutex`. Lightweight-update reservations are not released under that mutex; instead, their lifetime extends beyond patch-part commit. A decision made before release retains the reservation in its snapshot, while a decision made after release can observe the committed patch part. Failed operations release the reservation without publishing state and wake the background assignee.

3. Add to §4:

> This rule does not by itself solve concurrent transaction rollback or durable orphan mutation files. Upstream’s registration-time transaction-state recheck is required and must be backported to the 26.6 branch. A nontransactional final `mutation_*.txt` left after failed registration can still be replayed after restart; the PR must either address that lifecycle or state the limitation explicitly.

4. Replace the S3 second arm with patch-boundary tests. State that a plain merge of unmutated parts remains below `N` and cannot demonstrate the mirror.

5. Add an S2 cancellation/failure arm and a unit test for the `Mutation` reservation bound.

6. Preserve PR #119155’s `PENDING_LIGHTWEIGHT_UPDATE` string and test output. Add a distinct mutation-registration postpone reason.

7. Add a scope note or fix for `StorageAlias::alter`:

> The direct-table alter-lock proof does not cover metadata `ALTER` forwarded through `StorageAlias`, which currently passes the alias lock to the target. Either acquire the target’s alter lock in `StorageAlias::alter` or track this as a remaining ordering exception.
198,981
## Verdict — REVISE

The provisional-boundary rule is sound for the three in-process data-version races, provided every mutation/merge/patch decision uses one consistent snapshot. It is a good unifying invariant.

The design is not ready as written because §4 overclaims transaction, cleanup, and restart safety; the metadata-ordering fix is bypassable through `StorageAlias`; and the test plan neither exercises rule 3 nor correctly implements the claimed “mirror” merge case.

## MAJOR findings

### 1. Transaction rollback remains unsafe on the 26.6 branch

`prepareMutationEntry` associates the mutation with the transaction before registration ([StorageMergeTree.cpp:897](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:897), [StorageMergeTree.cpp:920](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:920)). If rollback runs while the mutation is paused, `killMutation` cannot find it in `current_mutations_by_version`. Registration then succeeds after the transaction is gone, and `selectPartsToMutate` reaches the `UnknownCSN` exception path.

Current upstream rechecks the transaction immediately after registration while holding `currently_processing_in_background_mutex`, erases a concurrently rolled-back entry, and removes its file. That logic exists in upstream `startMutation` but is absent from this branch. [Upstream implementation](https://github.com/ClickHouse/ClickHouse/blob/c9cb86303821b670b6b25ff44dabb7d39555312a/src/Storages/StorageMergeTree.cpp#L1159-L1215).

It is acceptable for the upstream PR to rely on that existing code. It is not acceptable to call transaction rollback out of scope for the Antalya backport: the corresponding upstream fix must be included as a prerequisite or backported in the same series.

### 2. The restart/orphan proof is false

`PreparedMutationEntry` declares `entry` before `block_holder`, so destruction runs in the opposite order: the holder releases the reservation first, then the entry attempts orphan-file deletion ([StorageMergeTree.h:224](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.h:224)). The same ordering exists among the locals in `prepareMutationEntry`: `entry` is constructed before `block_holder` ([StorageMergeTree.cpp:904](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:904)).

Deletion is best-effort; the destructor catches and logs failure ([MergeTreeMutationEntry.cpp:182](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeMutationEntry.cpp:182)). Therefore:

1. Nontransactional mutation `N` writes `mutation_N.txt`.
2. Registration fails.
3. Its reservation is released.
4. File removal fails, or the process terminates before removal.
5. On restart, `loadMutations` treats every final `mutation_*.txt` as registered and loads `N` ([StorageMergeTree.cpp:1475](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1475)).
6. A mutation whose client saw failure may unexpectedly execute; if parts advanced above `N` after reservation release, it may instead be silently skipped for those parts.

Reordering cleanup before reservation release narrows the same-process gap, but a durable distinction between “final file” and “registered mutation” is required to eliminate the restart ambiguity completely. At minimum, §4 must stop claiming orphaned files are fully handled.

### 3. The lock fix does not cover metadata `ALTER` through `StorageAlias`

Direct `ALTER` acquires the table’s alter lock in `InterpreterAlterQuery` ([InterpreterAlterQuery.cpp:313](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/InterpreterAlterQuery.cpp:313)), and the proposed `mutate` change correctly retains the same target lock through registration.

But `StorageAlias::alter` forwards the lock holder acquired for the alias directly to the target without acquiring the target’s alter lock ([StorageAlias.cpp:219](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219)). Conversely, `StorageAlias::mutate` calls the target’s `mutate`, which does acquire the target lock ([StorageAlias.cpp:296](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:296)).

Thus a direct mutation paused during registration can still be overtaken by a metadata `ALTER` issued through an alias. The version-boundary rule prevents unsafe physical selection, but it does not stop the metadata from changing before the older mutation registers. Either fix alias locking or narrow the lock-fix claim and add a related issue.

### 4. The test plan does not cover the full proposed rule

The second S3 arm is not a mirror counterexample. A merge of two unmutated parts sets:

- `min_block` from the first part;
- `mutation` to the maximum source mutation, which is zero.

See [FutureMergedMutatedPart.cpp:56](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/FutureMergedMutatedPart.cpp:56) and `getDataVersion = mutation ? mutation : min_block` in [MergeTreePartInfo.h:79](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreePartInfo.h:79). Therefore the merged part remains below `N`; `DELETE WHERE id = 0` subsequently applies correctly. It does not demonstrate that `N` was skipped.

The actual “advance above `N`” case requires applying a later patch or mutation to the merge. There is also no direct test for:

- rule 3, patch selection bounded by a reservation;
- rule 1’s `Mutation` arm;
- rule 4 when a reservation is released by failure/cancellation rather than success;
- transaction rollback;
- orphan-file restart behavior.

## MINOR findings

- §4 incorrectly says every holder is destroyed under `currently_processing_in_background_mutex`. That is true for successful mutation registration, but not for an `Update`: its holder belongs to `MergeTreeSinkPatch` and outlives patch commit ([MergeTreeSinkPatch.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.cpp:28)). The rule remains safe because release occurs after patch commit, not because it uses the background mutex.
- Calling an `Update` or `Unknown` block a “current mutation version” weakens the meaning of established accessors. The existing declarations explicitly describe registered mutations ([StorageMergeTree.h:270](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.h:270)).
- A helper that scans the unfiltered `CommittingBlocksSet` for every part can become `O(parts × in-flight inserts)`, because most entries may be `NewPart`. Pre-filter version reservations once into a sorted local vector.
- `Op::Unknown` is not currently produced by plain `StorageMergeTree`; its allocations always pass a concrete operation ([StorageMergeTree.cpp:3617](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3617)). Treating it conservatively is reasonable future-proofing.
- S1 needs to say explicitly that mutation B is retried after A is released. On the fixed build, the first attempt at B times out, so otherwise there are not “both” mutations to complete.

## Answers

### 1. Soundness

| Case | Result |
|---|---|
| Two mutations | Sound in-process. The lock fix serializes ordinary creation. Independently, bounding selection by the first reservation above the part prevents a registered higher mutation from crossing a lower unregistered one. |
| Update vs mutation | Sound. A registered mutation above an `Update` reservation is excluded until the patch has committed. `updateLightweight` keeps the holder in the sink through patch publication ([StorageMergeTree.cpp:1145](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1145)). |
| Merge vs `Mutation`/`Update`/`Unknown` | Sound if merge comparison includes the same reservation snapshot. A boundary in `(left data version, right data version]` makes the parts non-mergeable. |
| Merge vs `NewPart` | No new boundary is needed. Insert allocation, part indexing, and commit occur while holding `lockParts` ([MergeTreeSink.cpp:370](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:370)); the part is inserted into `data_parts_indexes` before the lock is released ([MergeTreeData.cpp:5575](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:5575)). A selector therefore sees either no allocation yet or the intervening part. `getMaxLevelInBetween` additionally guards indexed gaps ([StorageMergeTree.cpp:1987](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1987)). |
| Reservation released after handled failure | Safe in-process: no state will register, a decision with an older snapshot remains conservative, and rule 4 wakes selection. |
| Mutation in a transaction | Unsafe on this branch without upstream’s registration-time rollback recheck. |
| Restart with orphaned final mutation file | Unsafe/ambiguous, as described in Major finding 2. |

The map-only status and cleanup paths remain valid once crossing is impossible: `getIncompleteMutationsStatus` tests parts against the registered version ([StorageMergeTree.cpp:1248](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1248)), while `clearOldMutations` uses the minimum part data version ([StorageMergeTree.cpp:2153](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2153)). They do not themselves need reservation awareness.

### 2. Elegance and minimality

“Reservations are provisional version boundaries” is the right invariant. Making allocation and registration atomic is less attractive:

- the temporary file can be written before allocation, but final rename would then occur while holding the hot background-selection mutex;
- mutation-producing metadata `ALTER` deliberately needs the durable file before durable metadata commit, so allocation and registration cannot simply be collapsed without restructuring that transaction;
- it would not solve lightweight updates.

I would not overload `getCurrentMutationVersion` and `getNextMutationVersion` so that they sometimes return an update or unknown reservation. A maintainer is more likely to prefer:

```cpp
using VersionReservations = std::vector<CommittingBlock>;

VersionReservations getVersionReservationsSnapshot() const;
const CommittingBlock * firstVersionReservationAfter(
    Int64 data_version,
    const VersionReservations & reservations);
```

Then:

- `selectPartsToMutate` uses the returned number as its map end bound;
- `canMergeParts` keeps the existing registered-mutation comparison and additionally rejects a reservation in the interval;
- patch selection takes `min(getNextMutationVersion(...), reservation number)`.

That gives one abstraction without three copied loops or semantic overloading. Pre-filtering once also avoids repeatedly scanning `NewPart` reservations.

### 3. The five open questions

1. **Use an explicit reservation helper.** Keep the existing mutation accessors map-only. An `Update` reservation is a boundary, not a current mutation.

2. **`min_update_block` is redundant after rules 2 and 3 are implemented correctly.** Rule 2 prevents a regular merge from crossing the pending update; rule 3 prevents it from materializing a later patch across that update. Patch-to-patch merges do not materialize patches into regular data, and `SourcePartsSetForPatch::merge` preserves each source part’s version interval ([SourcePartsSetForPatch.cpp:132](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/SourcePartsSetForPatch.cpp:132), [MergeTask.cpp:900](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTask.cpp:900)). No missed correctness case is visible in this code. Removing the old refusal requires dedicated below/above-boundary tests.

3. **Do not treat `NewPart` as a version boundary.** Allocation and insertion into the part index are atomic under the data-parts lock, as cited above. The spec should name that lock protocol as the primary reason; “block ranges and `getMaxLevelInBetween`” alone is incomplete.

4. **Upstream: already handled. Antalya: in scope as a prerequisite.** Retain upstream’s rollback recheck and backport it to 26.6. Do not duplicate it in the upstream PR.

5. **Keep distinct postpone strings.** `parts_postpone_reasons` is exposed through `system.mutations`; changing the string is observable. Preserve `PENDING_LIGHTWEIGHT_UPDATE` for `Update`, and introduce a separate reason for `Mutation`/`Unknown`. One internal rule does not require one user-facing diagnosis.

### 4. Test plan

- **S1:** Deterministic with `PAUSEABLE_ONCE`; red before the lock fix for the expected timeout. Explicitly retry B after releasing A.
- **S2:** Deterministic and correctly red before PR #119155.
- **S3 first arm:** Valid. Without merge-boundary handling, the merged result remains below `N`, so late registration deletes the post-`N` row.
- **S3 second arm:** Invalid; it describes correct behavior, not a skipped mutation.
- **S4:** Deterministic for direct-table `ALTER`, but add an alias variant or narrow the claim.

Add:

- Two concurrent-update patch tests: one with source parts below pending update `U`, proving later patch `V` is not materialized across `U`; one with all source parts above `U`, proving removal of `min_update_block` does not unnecessarily refuse that merge.
- A cancellation arm for S2: cancel the paused lightweight update, verify no patch is committed, and verify the postponed mutation wakes and completes.
- A focused unit test for classification and “first reservation strictly above data version,” including multiple `NewPart`s and reservations below the part.
- The existing upstream transaction rollback test in the backport.
- A restart/failure test only if the PR claims orphan-file safety.

### 5. Superseding PR #119155

Replacing its explicit `Op::Update` loop with the general helper is technically appropriate. Deleting or weakening its regression test is not.

Keep the test and its existing expected reason unchanged. The PR’s commits explicitly introduced `PENDING_LIGHTWEIGHT_UPDATE` as user-visible diagnosis and assert it in the reference output ([pr119155.diff:13](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/pr119155.diff:13), [pr119155.diff:92](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/pr119155.diff:92)).

The likely author objections are:

- unnecessary churn to a user-visible string;
- loss of a narrowly attributable regression test;
- broadening a reviewed bug fix into mutation, merge, patch, alter-lock, transaction, and cleanup behavior;
- the general bound being less conservative for parts inserted above an older pending update.

Layer the broader rule in subsequent commits, preserve the original test contract, and add new tests for the newly permitted above-boundary behavior.

### 6. Risks

- **Visible behavior:** more mutations postponed behind `Mutation` reservations; merges and `OPTIMIZE` may be temporarily refused. If `min_update_block` is removed, merges wholly above a pending update become newly permitted.
- **Observability:** changing `system.mutations.parts_postpone_reasons` strings may break monitoring or tests.
- **Performance:** `MergeTreeMergePredicate` already copies `committing_blocks` once ([MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)). Mutation selection adds another snapshot only while mutations exist. The main risk is repeated linear scans through `NewPart` reservations; pre-filter once.
- **Locking:** the safe order is background mutex → brief committing-block snapshot, releasing `committing_blocks_mutex` before taking the parts lock. Registration already follows background → committing when the holder is destroyed. `removeCommittingBlock` must release `committing_blocks_mutex` before calling `background_operations_assignee.trigger`, as PR #119155 does. I found no new reverse edge under that protocol.
- **Failure cleanup:** holder/entry destruction order remains a restart-safety hazard.

## Recommended changes to the design

Make these concrete edits:

1. Replace the accessor proposal in §1:

> Snapshot and pre-filter `Mutation`, `Update`, and `Unknown` reservations into a sorted local vector. Keep `getCurrentMutationVersion` and `getNextMutationVersion` limited to registered mutations. Use `firstVersionReservationAfter(data_version)` to bound mutation selection, reject merges spanning a reservation, and lower the patch-selection upper bound.

2. Replace the first two bullets of §4:

> Successful mutation registration transfers the entry into `current_mutations_by_version` and destroys its reservation while holding `currently_processing_in_background_mutex`. Lightweight-update reservations are not released under that mutex; instead, their lifetime extends beyond patch-part commit. A decision made before release retains the reservation in its snapshot, while a decision made after release can observe the committed patch part. Failed operations release the reservation without publishing state and wake the background assignee.

3. Add to §4:

> This rule does not by itself solve concurrent transaction rollback or durable orphan mutation files. Upstream’s registration-time transaction-state recheck is required and must be backported to the 26.6 branch. A nontransactional final `mutation_*.txt` left after failed registration can still be replayed after restart; the PR must either address that lifecycle or state the limitation explicitly.

4. Replace the S3 second arm with patch-boundary tests. State that a plain merge of unmutated parts remains below `N` and cannot demonstrate the mirror.

5. Add an S2 cancellation/failure arm and a unit test for the `Mutation` reservation bound.

6. Preserve PR #119155’s `PENDING_LIGHTWEIGHT_UPDATE` string and test output. Add a distinct mutation-registration postpone reason.

7. Add a scope note or fix for `StorageAlias::alter`:

> The direct-table alter-lock proof does not cover metadata `ALTER` forwarded through `StorageAlias`, which currently passes the alias lock to the target. Either acquire the target’s alter lock in `StorageAlias::alter` or track this as a remaining ordering exception.
