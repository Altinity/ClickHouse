## Verdict (REVISE)

Rev.3 closes most round-2 findings, but its “parts first, reservations second” ordering is still not a coherent cut for lightweight updates. Patch publication and reservation release can both occur between those two snapshots, so merge selection can see neither the pending boundary nor its patch.

A second issue concerns `OPTIMIZE FINAL`: every retry must build a fresh cut. Reusing either captured parts or a reservation snapshot across retries is incorrect.

## MAJOR findings

### 1. Lightweight-update publication can fall between the two snapshots

Counterexample:

1. Regular part `P` has `dv(P) < U`.
2. Lightweight update `U` holds an `Op::Update` reservation and is paused before patch commit.
3. A disjoint update `V > U` commits. This is allowed because `waitForCommittingInsertsAndMutations` deliberately ignores earlier `Update` reservations ([StorageMergeTree.cpp:3629](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3629)).
4. Merge selection captures `P` and patch `V`, but not uncommitted patch `U`, then releases the data-parts lock.
5. `U` commits its patch and its sink destroys the holder. Patch publication happens through `commitPart` under the parts lock ([MergeTreeSinkPatch.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.cpp:28), [MergeTreeSink.cpp:370](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:370)).
6. Selection now snapshots reservations and misses `U`.
7. Rule 3 admits captured patch `V`; the result advances to `V`.
8. Patch `U` is now below the result data version and no longer applies.

This directly disproves the proof in [the spec:251](</home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:251>). Publication preceding release does not make a patch snapshot taken before publication atomic with a reservation snapshot taken after release.

Exact text that closes this MAJOR:

> For every merge-selection attempt, acquire `readLockParts`, capture the complete candidate regular-part and patch-part state, including transaction-required active and outdated inputs, and, while still holding that read lock, snapshot `VersionReservations` under `committing_blocks_mutex`. Release the data-parts lock only after all three snapshots are complete. The collector and predicate consume only those snapshots. Thus a successful lightweight update either publishes before the parts snapshot and its patch is captured, or publication is blocked by the data-parts lock and its reservation is captured. If overlapping these locks is rejected, use a monotonic generation and retry the entire capture when it changes; two independent snapshots or their union are insufficient.

This replaces the “no overlap” lock protocol at [the spec:150](</home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:150>).

The required order is not novel: insert commit already holds `lockParts` and calls `fillNewPartName`, which acquires `committing_blocks_mutex` ([MergeTreeSink.cpp:376](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:376), [StorageMergeTree.cpp:3617](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3617)).

### 2. Each `OPTIMIZE FINAL` retry must get a new coherent cut

Currently, `selectPartsToMerge` constructs one predicate and collector ([StorageMergeTree.cpp:1540](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1540)), but the partition path can wait for active merges and retry ([StorageMergeTree.cpp:1654](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1654)). The collector currently rereads parts on every call ([MergeTreePartsCollector.cpp:139](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp:139)).

With the proposed captured-input implementation:

- Reusing captured parts makes the retry stale and can break `OPTIMIZE` progress.
- Recollecting parts while reusing the old reservation snapshot recreates the rev.2 counterexample.
- Lazy predicate initialization once per `selectPartsToMerge` call has the same problem.

Exact text that closes this MAJOR:

> Each collector invocation is a separate selection attempt. This includes every retry after the `OPTIMIZE FINAL` wait. For each attempt, capture a fresh regular/patch candidate state and its reservation snapshot, and construct or reset the collector and predicate from that bundle. Never reuse a reservation snapshot with a later `collectInitial` read.

This is a localized `StorageMergeTree`/collector construction change, not a redesign of the compaction framework.

## MINOR findings

- S2 does not deterministically prove the release trigger. `mt_merge_selecting_task_pause_when_scheduled` pauses before selection ([StorageMergeTree.cpp:2019](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2019)); disabling it itself permits progress. I found no existing “force long assignee backoff” failpoint.
- S3d is not deterministic. `mt_throw_after_mutation_commit` throws immediately ([StorageMergeTree.cpp:915](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:915)), leaving no handshake in which merge selection first observes and refuses the reservation.
- Existing S3c tests allocation after an old reservation snapshot. It does not test the newly identified patch-publication/release gap. Add a pause between patch capture and reservation capture.
- S1 is red before the lock fix for ordering, but [fix_v3.diff:142](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/fix_v3.diff:142) does not implement the claimed pre-fix data-loss proof: it does not wait for B to finish while A remains paused and then inspect A’s surviving rows.
- S2c needs an explicit pause preventing K from completing before `U` and `M` are arranged; otherwise the intended `K < U < M` partial-progress state is timing-dependent.
- Rule 3 lacks focused coverage for `getNextMutationVersion(dv) == 0` and patches below, at, and above the reservation bound. S3c tests the cut, not the new per-range calculation independently.
- Reordering only `PreparedMutationEntry` does not ensure cleanup precedes release on exceptions inside `prepareMutationEntry`: local `entry` is declared before local `block_holder`, so unwinding destroys the holder first ([StorageMergeTree.cpp:904](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:904)). Reorder or aggregate the local ownership too.
- “A failed operation releases its reservation without publishing state” is too broad. `MergeTreeSinkPatch::finishDelayedChunk` commits partitions one at a time, so an update can fail after publishing a subset of its patches ([MergeTreeSinkPatch.cpp:28](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.cpp:28)).
- Triggering for every successful `Mutation` release is redundant because registration already triggers at [StorageMergeTree.cpp:940](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:940). It is harmless, but worth avoiding if a clean “failed release versus transferred registration” distinction is convenient.

Round-2 closure status:

| Round-2 item | Rev.3 status |
|---|---|
| Coherent cut | Partially closed; publication/release and retry gaps remain |
| S3b already green / `0` sentinel | Text closed; rule-3 coverage partial |
| Transaction prerequisite | Closed |
| Orphan restart scope | Closed as an explicit limitation; local destruction-order claim needs correction |
| `StorageAlias::alter` | Closed |
| Invalid mirror test | Closed |
| Failure/cancellation wake tests | Still open deterministically |
| Explicit typed helper | Closed |
| Map-only mutation accessors | Closed |
| Preserve PR #119155 reason/test | Closed |
| Drop mutable capability guard | Closed |
| Bound `SYSTEM WAIT FAILPOINT` | Closed |
| `Unknown` wake and diagnosis | Closed |

The publication/release handoff and per-retry cut are new issues exposed by rev.3’s captured-input formulation.

## Answers (1-6)

### 1. Soundness

- **Two mutations:** Sound in-process once the cut is fixed. For part version `d`, selection must iterate from `upper_bound(d)` only to `lower_bound(first reservation above d)`. A registered higher mutation therefore cannot cross a lower unregistered one. Retaining the alter lock through `startMutation` additionally prevents the ordinary registration inversion; current code drops it at the `if` initializer ([StorageMergeTree.cpp:1082](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1082)).
- **Update versus mutation:** Sound. If the update reservation remains, rule 1 bounds selection. If the patch commits and the holder disappears in the snapshot gap, mutation execution still takes a later patch snapshot: `MutateTask::prepare` requests patch parts up to the selected target version ([MutateTask.cpp:3329](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MutateTask.cpp:3329)), `getMutationsSnapshot` reads active patches ([StorageMergeTree.cpp:3505](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3505)), and `getAlterConversionsForPart` applies them ([MergeTreeData.cpp:10995](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:10995)).
- **Merge versus `Mutation`:** Sound under the fixed cut. Registration and reservation destruction are serialized with selection by `currently_processing_in_background_mutex`; the interval test rejects a reservation in `(min dv, max dv]`.
- **Merge versus `Update`:** Unsound as written, via the `U < V` counterexample above. Holding the parts read lock through reservation capture closes it.
- **Merge versus `Unknown`:** Conservatively sound under the same protocol.
- **Merge versus `NewPart`:** It should not be a boundary. Allocation, indexing, and commit are under `lockParts` ([MergeTreeSink.cpp:376](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:376), [MergeTreeData.cpp:5589](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:5589)). `getMaxLevelInBetween` protects indexed outdated gaps, not a later active part omitted from a stale collector ([StorageMergeTree.cpp:1987](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1987)).
- **Failure release:** Failed mutation registration publishes no map entry; an old boundary snapshot is only conservative, and the assignee trigger restores progress. A failed lightweight update may have published some patch parts, which the atomic cut must observe.
- **Mutation inside a transaction:** Safe upstream only because `startMutation` rechecks rollback immediately after registration while holding the background mutex. This 26.6 tree lacks that check and can reach the `UnknownCSN` exception ([StorageMergeTree.cpp:1859](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1859)). The specified Antalya prerequisite is therefore mandatory.
- **Restart with an orphaned final mutation file:** Still unsafe and correctly scoped out. `loadMutations` treats every final `mutation_*` file as registered ([StorageMergeTree.cpp:1462](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462)), while deletion is best-effort ([MergeTreeMutationEntry.cpp:182](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeMutationEntry.cpp:182)). If `N` fails registration, its file survives, and later `M > N` advances a part, restart loads `N` but selection starts above the part’s data version and skips it. Alternatively, `N` may unexpectedly execute despite its client receiving failure.

`getIncompleteMutationsStatus` and `clearOldMutations` can remain map-only once crossing gaps is impossible; they compare registered versions with visible part versions ([StorageMergeTree.cpp:1248](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1248), [StorageMergeTree.cpp:2153](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2153)).

### 2. Elegance and minimality

“Reservations are provisional version boundaries” is the right abstraction.

A single helper that locks and calculates “next boundary above data version” independently for each part is simpler syntactically but wrong architecturally: it loses the one-snapshot-per-decision property and repeats locking/scanning. Making mutation registration atomic with allocation would require holding the background mutex across mutation-file I/O and metadata durability; it also does nothing for lightweight updates.

A maintainer would likely prefer rev.3’s semantic split:

- `getCurrentMutationVersion`: registered mutations only.
- `getNextMutationVersion`: registered mutations only.
- `getVersionReservations`: one prefiltered, typed snapshot reused by the decision.
- `VersionReservations::firstAfter`: binary search over that snapshot.

That is clearer than PR #119155’s copied `Op::Update` loops and avoids changing the meaning of established accessors. The necessary correction is to make creation of the parts/patch/reservation bundle atomic, not to collapse the API.

### 3. Open questions

The current rev.3 §5 contains three questions, not five. The five inherited questions are:

1. **Explicit helper or overloaded mutation accessors?** Use the explicit typed helper. The existing accessors document and implement registered mutation-map semantics ([StorageMergeTree.h:270](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.h:270), [StorageMergeTree.cpp:2127](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2127)).
2. **Remove `min_update_block`?** Not in this PR. Rules 2 and 3 make its global refusal redundant for correctness, but retaining `canUsePartInMerges`’s conservative check avoids changing above-boundary merge behavior simultaneously ([MergeTreeMergePredicate.cpp:70](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:70)). Replace the global patch filtering with the per-range bound; do not keep two independently defined patch rules.
3. **Is `NewPart` a boundary?** No. Allocation and insertion are atomic with respect to the candidate-parts snapshot under the data-parts lock.
4. **Transaction rollback?** Retain upstream’s registration-time recheck; land it first and separately on Antalya, exactly as rev.3 now requires.
5. **Postpone reasons?** Keep `PENDING_LIGHTWEIGHT_UPDATE` unchanged and add a distinct mutation-registration reason. `parts_postpone_reasons` is user-visible through `system.mutations`.

The three rev.3 questions:

1. **Captured collector/predicate inputs:** Implementable with a localized constructor/API refactor. Hoist the initial collection from `MergeTreePartsCollector`, pass captured inputs in, and rebuild the bundle per retry. Lazy reservation capture is not acceptable unless the parts lock remains held through it or an epoch validation forces retry.
2. **Other wrong-order paths:** No separate bypass. Background selection and explicit `OPTIMIZE` both call `selectPartsToMerge` ([StorageMergeTree.cpp:2052](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2052), [StorageMergeTree.cpp:1734](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1734)). `waitForCommittingInsertsAndMutations` inspects only reservations. The `OPTIMIZE FINAL` retry within `selectPartsToMerge` is the exception that needs a fresh cut.
3. **Are S3b/S3c red with reservation-first ordering?** Yes. The current predicate snapshots reservations before patch infos ([MergeTreeMergePredicate.cpp:22](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)), while regular candidates are collected later ([MergeTreePartsCollector.cpp:126](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp:126)). The tests correctly expose late `N/Q` and `U/V`. They do not expose release between patch and reservation capture.

### 4. Test plan

- `05227`: deterministic after bounding `SYSTEM WAIT FAILPOINT`; keep it.
- S1: deterministic red for ordering, but the supplied test does not yet prove the claimed lost-row interleaving.
- S2: cancellation ordering is plausible, but the trigger proof is not deterministic.
- S2b: correctly red on PR #119155’s mutable capability guard.
- S2c: valid after explicitly holding K pending while arranging `K < U < M`.
- S3: valid and deterministic if the background selector stays paused while explicit `OPTIMIZE` selects.
- S3b/S3c: valid and red for reservation-first ordering.
- S3d: not deterministic with an immediate throw failpoint.
- S4: direct and alias variants cover the two lock paths.
- Unit helper test: useful, but add direct rule-3 bound and `0`-sentinel cases.

Missing scenarios:

- Update publication/release between patch capture and reservation capture.
- Deterministic `Mutation` failure-release wake-up.
- Direct per-range patch-bound coverage.

### 5. Superseding PR #119155

Replacing its explicit `Op::Update` loop with the general bound is appropriate; retaining both loops would create duplicate policies. Do it in a later commit on top of its preserved commits so attribution and bisectability remain clear.

Do not change:

- `PENDING_LIGHTWEIGHT_UPDATE`.
- The focused `05227_mutation_waits_for_lightweight_update.sh` contract, apart from the timeout around `SYSTEM WAIT FAILPOINT`.

The author’s likely objections would be:

- Expanding a focused mutation-selection fix into merge, patch, alter-lock, alias, cleanup, and transaction work.
- Weakening bisectability.
- Changing `K < U < M` from “postpone the whole part” to safe partial progress.
- Churn to an already user-visible diagnostic or deletion of their focused regression test.

Rev.3’s proposed layering—preserve their commits, replace only the redundant loop later, retain the reason and test—is the least contentious approach.

### 6. Risks

- **Visible behavior:** More mutations are postponed behind unregistered mutation reservations; background merges and `OPTIMIZE FINAL` may temporarily refuse ranges.
- **Observability:** The new mutation-registration string appears in `system.mutations.parts_postpone_reasons`. The existing update string must remain stable.
- **Partial progress:** `K < U < M` now permits K to run before postponing M. This is safe but observable in part versions and mutation progress.
- **Performance:** Merge selection already copies all committing blocks once. A filtered `VersionReservations` vector reduces per-part work to binary search, but constructing it still scans under `committing_blocks_mutex`. Holding the parts read lock through that scan adds short commit-side contention; keep the critical section to pointer/info capture and one compact reservation copy.
- **Snapshot retention:** Captured part vectors hold shared pointers for the decision’s lifetime. This is bounded by one selection attempt.
- **Locking:** The corrected order is `currently_processing_in_background_mutex → data-parts lock → committing_blocks_mutex`. The inner `data-parts → committing-blocks` edge already exists in insert commit. No reverse edge was found in the reviewed paths. `removeCommittingBlock` must continue releasing `committing_blocks_mutex` before calling `background_operations_assignee.trigger`.
- **Liveness:** Failure-release triggers are important because assignee backoff can grow to 600 seconds ([BackgroundJobsAssignee.cpp:52](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/BackgroundJobsAssignee.cpp:52)).
- **Restart:** Durable orphan files remain an acknowledged limitation, not solved by the provisional-boundary rule.

## Recommended changes to the design

1. Replace the coherent-cut and lock-order paragraphs with the atomic capture text from MAJOR 1.
2. Add:

> Every collector invocation, including each retry after an `OPTIMIZE FINAL` wait, creates a new captured-input/reservation bundle. A predicate snapshot is never reused with a later parts read.

3. Replace the lightweight-update proof with:

> Patch publication requires the data-parts write lock and the update reservation outlives publication. Merge selection holds the data-parts read lock through reservation capture. Therefore it observes either the committed patch or the reservation. Ordering the two snapshots without overlapping the lock is insufficient.

4. Replace the failure statement with:

> A failed mutation publishes no registered mutation entry. A failed lightweight update may have published a subset of patch parts. Until release, its reservation protects missing or partially published state; after release, committed patches are observed through the atomic cut.

5. Add test S3e:

> Arrange pending update `U` and committed disjoint patch `V > U`. Pause merge selection after capturing patch infos but before reservation capture. Let `U` attempt to commit. With the fix, publication blocks on the held data-parts read lock, selection captures the `U` reservation, then releases the lock; the merge must not materialize `V` across `U`. Without the fix, `U` publishes and releases before reservation capture and the test loses `U`.

6. Replace S3d’s immediate-throw setup with:

> Add a `PAUSEABLE_ONCE` immediately before the failure injection. Wait until the merge has refused the reservation, release the mutation into the injected failure, and use a test-only deterministic long-backoff hook so completion is attributable to `removeCommittingBlock` calling `trigger`.

7. Add direct unit coverage for the rule-3 upper bound, including `getNextMutationVersion(dv) == 0` and patches below, equal to, and above the reservation.

8. Either reorder both `PreparedMutationEntry` members and the locals in `prepareMutationEntry`, or narrow the cleanup-order claim to the paths actually covered.
