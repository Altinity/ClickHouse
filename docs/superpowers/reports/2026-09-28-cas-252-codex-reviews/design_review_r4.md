## Verdict — REVISE

The `Mutation`/`Update` provisional-boundary rule is sound for the three motivating races once the rev.5 cut is implemented at the real publication point. The typed snapshot and map-only mutation accessors are the right API shape.

Rev.5 is not ready because its `NewPart` exception is false for `MOVE PARTITION`, and its universal publication epoch can starve merge selection indefinitely on continuously inserted tables. The former admits a concrete data-loss interleaving.

## MAJOR findings

### 1. `MOVE PARTITION` disproves both the `NewPart` exception and the claimed publication site

Rev.5 says:

> `Op::NewPart` reservations are not boundaries: insert allocation, indexing into `data_parts_indexes` and commit happen under `lockParts`.

That is true for ordinary inserts: `MergeTreeSink::commitPart` holds the parts lock across `fillNewPartName`, `renameTempPartAndAdd`, and `Transaction::commit` ([MergeTreeSink.cpp:370](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSink.cpp:370)).

It is false for `MOVE PARTITION TO TABLE`. The destination allocates `NewPart` reservations and inserts the parts as `PreActive`, then releases the destination parts lock at [StorageMergeTree.cpp:3261](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3261). It does disk renames and only later reacquires the lock in `dest_transaction.commit` at [StorageMergeTree.cpp:3282](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3282).

A concrete loss:

1. Destination has active level-zero part `P`.
2. `MOVE` allocates block `q`, inserts moved level-zero part `Q` as `PreActive`, and releases the destination parts lock while retaining its `NewPart(q)` holder.
3. A normal destination insert commits active part `R`, with block `r > q`.
4. Merge selection captures only active `P,R`. It filters out `NewPart(q)`.
5. `getMaxLevelInBetween(P,R)` sees `Q`, but rejects only when the intervening level is strictly greater than both endpoint levels ([StorageMergeTree.cpp:1987](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1987)). Level zero therefore does not prevent the merge.
6. `P,R` merge and publish a covering part spanning `q`.
7. `MOVE` commits `Q`. `Transaction::commit` finds the covering part and marks `Q` `Outdated` instead of `Active` ([MergeTreeData.cpp:9497](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:9497)).
8. `MOVE` then commits the source-side empty covering parts. The moved rows are active in neither table.

The proposed epoch does not detect this. `renameTempPartAndReplaceImpl` only calls `preparePartForCommit`, which sets `PreActive` and inserts into the indexes ([MergeTreeData.cpp:5573](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:5573), [MergeTreeData.cpp:5638](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:5638)). Actual publication is the later `PreActive → Active` transition in `Transaction::commit` at [MergeTreeData.cpp:9539](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:9539). An epoch increment at rename has already happened before the selector’s first read.

`restoreAndActivatePart` and `swapActivePart` also publish active objects without going through any rename wrapper ([MergeTreeData.cpp:6006](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:6006), [MergeTreeData.cpp:6429](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:6429)). The former currently has no caller in this tree; the latter is used by the parts mover. They still disprove the claimed one-site inventory.

Exact spec text that closes this MAJOR:

> `Op::NewPart` is also a provisional boundary unless its allocation, insertion and `PreActive → Active` publication occur in one uninterrupted data-parts write-lock critical section. Ordinary `INSERT`, `ATTACH`, `REPLACE PARTITION` and restored-part attachment satisfy that condition; `MOVE PARTITION TO TABLE` currently does not. `VersionReservations` therefore retains a split-phase `NewPart` reservation, or `MOVE` must first be changed so destination allocation and publication cannot be crossed by merge or mutation selection.
>
> The epoch is advanced at the transition that makes a part visible to the captured active-part set, not at temporary-part rename or `PreActive` insertion. The implementation must account for `MergeTreeData::Transaction::commit`, `restoreAndActivatePart`, and `swapActivePart`; wrapper names such as `renameTempPartAndReplace` are not publication points. A regression test pauses `MOVE` after destination preactivation, commits a later destination insert, attempts a merge across the pending moved part, and verifies that the moved rows remain active.

### 2. Three optimistic retries can starve merges forever under insert load

The interval being validated is:

1. capture parts and epoch under the parts read lock;
2. release that lock;
3. copy reservations under `committing_blocks_mutex`;
4. reread the epoch.

If every ordinary insert publication increments the epoch, a queued insert writer can run in this interval on every attempt. There is no scheduling or memory-model property that guarantees one of three attempts will find a quiet interval.

After three failures, rev.5 returns “nothing to merge.” Repeating this on each scheduler invocation can starve merges indefinitely. Worse, the scheduler increases its no-work backoff in `BackgroundJobsAssignee::postpone` ([BackgroundJobsAssignee.cpp:52](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/BackgroundJobsAssignee.cpp:52)), making recovery slower as insert pressure rises. No finite retry count or randomized backoff provides a liveness guarantee against a continually ready writer.

For explicit `OPTIMIZE`, “nothing to merge” is also the wrong result: validation failed; the selector did not establish that nothing was mergeable.

Exact spec text that closes this MAJOR without extending the parts-lock hold:

> Validation uses a `boundary_publication_epoch`, not an epoch advanced by every ordinary part publication. It is advanced only by publication events that can retire a reservation which may predate the captured part set: activation of a patch for an `Update`, and activation of any split-phase `NewPart` retained as a boundary. An ordinary insert whose allocation and activation occur in one parts-lock critical section does not advance this epoch; if it starts after the capture, its version is above every captured input and cannot invalidate the decision.
>
> Validation failure is not reported as `NOTHING_TO_MERGE`. Background selection records the retry and requests an immediate later attempt without increasing the normal no-work backoff. Explicit `OPTIMIZE` returns `CANNOT_SELECT` with a transient “part set changed while capturing merge inputs” explanation. Three in-call attempts bound CPU work; they are not claimed to guarantee progress under an adversarial stream of relevant boundary publications.

## MINOR findings

### Rule 3 needs an explicit exclusive bound

The spec says `getPatchPartsByPartition` excludes patches “at or above” the boundary, but the existing `getPatchesToApplyOnMerge` includes a patch whose data version equals its third argument: it skips only `max_patch_version > next_mutation_version` ([PatchPartsUtils.cpp:291](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:291)).

For registered mutations equality cannot normally occur because block numbers are unique. It can occur when the argument is an in-flight update’s own reservation and one partition has already published its patch. The design must decide whether equality is permitted. The stated “may not cross/reach the boundary” rule and existing `min_update_block` filtering imply an exclusive upper bound.

### The bounded-retry test is not deterministic

S3e’s first arm is deterministic if it waits for update `U` to finish publication and release before resuming selection.

The second arm is not. A background insert loop does not guarantee one publication in each of the three narrow validation windows. It may publish many times during the initial pause and then miss a retry window entirely. Use a per-attempt failpoint/counter immediately between the first epoch read and the reservation snapshot, and acknowledge each of the three injected publications.

### The release-trigger tests still need one named hook

The repository has no existing assignee-backoff failpoint; only the pre-selection pause exists. Disabling that pause itself allows progress and therefore cannot prove `trigger`. Rev.5 recognizes this at §3 lines 295–296, but S2 still says “or the existing backoff failpoint.” Name and specify the new test-only hook exactly.

### S2c is only partially specified

Section 5 says that holding `K` pending is “carried into the implementation plan,” but S2c does not state the failpoint or handshake that keeps `K` unapplied while `U` and `M` are registered. Without it, `K < U < M` can collapse before the observation.

### S1’s claimed loss proof is not in the supplied patch

The final fixed test is deterministically red before the lock fix because B succeeds rather than returning `TIMEOUT_EXCEEDED`. The supplied [fix_v3.diff](/home/mfilimonov/workspace/ClickHouse/master/tmp/cas252/fix_v3.diff) does not wait for B to complete while A remains paused and inspect the rows before releasing A, so it does not implement the stronger loss proof claimed by rev.5.

### Successful mutation registration still triggers twice

`addPreparedMutationEntry` already triggers the assignee ([StorageMergeTree.cpp:927](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:927)). Triggering again when the transferred `Mutation` holder is destroyed is harmless but unnecessary. The failure path is the load-bearing case.

### Round-3 closure audit

| Round-3 item | Rev.5 status |
|---|---|
| MAJOR: atomic part/patch/reservation cut | Partially closed. Epoch validation is a valid strategy, but the named bump site is pre-publication and the `MOVE`/`NewPart` path is missed. |
| MAJOR: fresh bundle on every `OPTIMIZE FINAL` retry | Closed by §1 lines 133–140 and 175–180. |
| S2 deterministic trigger proof | Closed in intent, but the hook still needs an exact name and semantics. |
| S3d deterministic failure-release proof | Closed in intent by the added pre-throw pause and long-backoff hook. |
| Missing publication/release test | Closed by S3e first arm. |
| S1 pre-fix loss observation | Still open in the supplied test artifact. |
| S2c explicit `K` hold | Partially closed; promised but not specified. |
| Rule-3 `0` sentinel/unit coverage | Closed by lines 290–294, subject to the equality ambiguity above. |
| Reorder both local and member destruction | Closed by §4 lines 320–330. |
| Partial patch publication on failure | Closed by §4 lines 314–319. |
| Avoid redundant successful `Mutation` wake | Still open; harmless. |
| Recommended atomic-cut text | Replaced by the owner-selected epoch strategy; correctness remains partially open for the reasons above. |
| Fresh bundle per collector invocation | Closed. |
| Correct lightweight-update proof | Closed if the epoch is advanced at the actual handoff event. |
| Correct failure statement | Closed. |
| Add S3e | Closed, except its retry-exhaustion arm is nondeterministic. |
| Deterministic S3d | Closed in design. |
| Direct rule-3 unit coverage | Closed. |
| Cleanup-order wording | Closed. |

## Answers (1–6)

### 1. Soundness

- **Two mutations:** Sound in-process after rule 1. For part version `d`, iterating registered mutations only up to `lower_bound(firstReservationAfter(d))` prevents higher registered mutation `M` crossing lower unregistered `N`. Retaining the alter lock across `startMutation` removes the ordinary inversion at its source; current `mutate` releases its `if`-initializer lock before calling `startMutation` ([StorageMergeTree.cpp:1073](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1073)).

- **Update versus mutation:** Sound with the validated handoff. If `U` remains reserved, it bounds mutation selection. If its patch publishes and the holder disappears between captures, the boundary epoch forces recapture. If publication precedes the initial capture, `MutateTask::prepare` later takes a patch snapshot up to its target version ([MutateTask.cpp:3329](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MutateTask.cpp:3329)).

- **Merge versus `Mutation`:** Sound. Selection holds `currently_processing_in_background_mutex`; registration and reservation destruction occur under that mutex, so it sees either the reservation or the map entry. The interval `(min dv, max dv]` rejects crossing.

- **Merge versus `Update`:** Sound after the actual patch-publication handoff advances the validation epoch. Rule 2 prevents regular-part crossing; rule 3 prevents a later patch from advancing the result across `U`.

- **Merge versus `Unknown`:** Conservatively sound when treated exactly like an unclassified version boundary. Plain `StorageMergeTree` currently never allocates one.

- **Merge versus `NewPart`:** Unsound as specified because `MOVE` holds `NewPart` across an unlocked `PreActive` window. Ordinary inserts, `ATTACH`, `REPLACE PARTITION`, and restored-part attachment do keep allocation and activation under one parts lock ([StorageMergeTree.cpp:3095](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3095), [StorageMergeTree.cpp:3452](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3452)); `MOVE` does not.

- **Reservation released on failure:** Safe in-process once the epoch is correct. A failed mutation publishes no map entry. A failed update can publish a subset of partition patches; the reservation protects the partial state until release, and later selections capture the published subset. Triggering after releasing `committing_blocks_mutex` restores liveness.

- **Mutation inside a transaction:** Not safe on this 26.6 base without the registration-time rollback recheck. Rollback cannot remove an entry not yet registered, and later selection reaches the `UnknownCSN` exception at [StorageMergeTree.cpp:1859](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1859). The separate Antalya prerequisite is mandatory. Upstream must retain its existing recheck.

- **Restart with an orphaned mutation file:** Still unsafe and correctly scoped out. `loadMutations` treats every final `mutation_*.txt` as registered ([StorageMergeTree.cpp:1462](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462)); destructor deletion is best-effort ([MergeTreeMutationEntry.cpp:182](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeMutationEntry.cpp:182)). Reordering destruction narrows the same-process window but does not durably distinguish prepared from registered files.

`getIncompleteMutationsStatus` and `clearOldMutations` can remain map-only once crossings are impossible; both reason from visible part versions and registered mutation versions ([StorageMergeTree.cpp:1191](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1191), [StorageMergeTree.cpp:2144](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2144)).

### 2. Elegance and minimality

“Reservations are provisional versions” is the right abstraction. In fact, the `MOVE` counterexample argues for applying it consistently to split-phase `NewPart` reservations.

Making mutation allocation and registration atomic is not simpler:

- the mutation file needs its allocated version;
- doing final file I/O while holding `currently_processing_in_background_mutex` would block merge/mutation selection;
- metadata `ALTER` has a separate durability protocol;
- it would not solve lightweight updates.

A single “next boundary” accessor that locks internally per part would hide snapshot ownership and risk mixing snapshots. A maintainer should prefer:

- `getCurrentMutationVersion`: registered-map semantics;
- `getNextMutationVersion`: registered-map semantics;
- one `VersionReservations` snapshot owned by the selection bundle;
- `firstAfter` as a binary search over that snapshot.

That is clearer than PR #119155’s explicit loops and preserves the established meaning of the mutation accessors. The design should name the epoch a boundary-handoff epoch rather than claim every physical part publication is relevant.

### 3. Five inherited open questions and rev.5 §6

Rev.5 §5 now contains four settled bullets. The five inherited questions have these answers:

1. **Explicit helper versus overloaded accessors:** Use the explicit typed snapshot; keep mutation accessors map-only.
2. **Remove `min_update_block`:** Keep `canUsePartInMerges`’s conservative refusal in this PR. Make rule 3 the canonical patch bound, then remove the global refusal only in a separately tested follow-up.
3. **Is `NewPart` a boundary?** Ordinary lock-nested insert reservations need not be observable boundaries, but split-phase `MOVE` reservations do. The simplest robust representation is to retain `NewPart` in `VersionReservations`; an optimized exclusion requires encoding and proving atomic allocation/publication per holder.
4. **Transaction rollback:** Retain upstream’s recheck; land it first and independently on Antalya.
5. **Postpone reasons:** Preserve `PENDING_LIGHTWEIGHT_UPDATE`; add the distinct mutation-registration reason. If `NewPart` can postpone mutation selection, add a precise part-publication reason rather than misreporting it as a mutation.

The three rev.5 §6 questions:

1. **Publication inventory:** One rename site does not cover publication. Ordinary insert, patch insert, mutation commit, `ATTACH`, `REPLACE PARTITION`, and restored clone use rename plus `Transaction::commit`; merge and `MOVE` separate preactivation from commit; `restoreAndActivatePart` and `swapActivePart` bypass rename. The common visible transition is activation in `Transaction::commit`, with the two direct activation functions as exceptions.

2. **Three retries/starvation:** A hot insert stream can starve forever if all inserts bump the epoch. No finite bound or backoff proves progress. Narrow the epoch to relevant reservation handoffs, or use the rejected lock-overlap fallback. Given the owner constraint, the narrowed epoch is preferable.

3. **Mutation selection:** Yes, the corrected epoch protocol is sufficient even though mutation selection captures no patches. A pending update either remains a reservation and bounds the target, publishes during the gap and invalidates the capture, or published before capture and is subsequently included by `MutateTask`’s patch snapshot. Updates allocated after validation have versions above every selected mutation/result.

### 4. Test plan

- `05227`: deterministic after bounding `SYSTEM WAIT FAILPOINT`; keep its expected reason unchanged.
- S1: deterministic red for serialization. The stronger pre-fix lost-row proof still needs implementation.
- S2: deterministic only after adding the named long-backoff hook.
- S2b: deterministic and correctly red on PR #119155’s mutable capability guard.
- S2c: sound scenario, but explicitly pause `K` before arranging `U` and `M`.
- S3: deterministic if the background selector remains paused while explicit `OPTIMIZE` selects.
- S3b/S3c: correctly red against current reservation-first construction.
- S3d: deterministic with the new pre-throw pause plus a real long-backoff hook.
- S3e first arm: correct and necessary.
- S3e retry-exhaustion arm: nondeterministic as written.
- S4: direct and alias variants are correct.
- Rule-3 unit coverage: necessary; specify exclusive/equality semantics.
- Missing: the split-phase `MOVE`/`NewPart` regression described in Major 1, and a focused test that validation failure is not surfaced as a successful `NOTHING_TO_MERGE` for explicit `OPTIMIZE`.

### 5. Superseding PR #119155

Replacing its `Op::Update` loop with the general reservation bound is appropriate. Keeping both would create two policies and two opportunities for snapshot-order mistakes.

Do not change:

- the exact `PENDING_LIGHTWEIGHT_UPDATE` string;
- the focused `05227_mutation_waits_for_lightweight_update.sh` contract, except for bounding its wait;
- the original commits/authorship.

The author is likely to object to:

- turning a focused mutation/update fix into a merge, patch, alter-lock, alias, transaction, cleanup, epoch, and `MOVE` change;
- weakening bisectability;
- changing their user-visible `system.mutations` reason;
- deleting their focused test;
- allowing partial `K < U < M` progress without a direct proof.

The general rule should therefore be layered in later commits, while replacing only the now-redundant loop. Rev.5’s stated layering is acceptable; changing the existing reason or test is not.

### 6. Risks

- **User-visible scheduling:** More mutations are postponed; merges and `OPTIMIZE FINAL` may temporarily be refused.
- **Command semantics:** Epoch-validation failure must not masquerade as `NOTHING_TO_MERGE`.
- **Observability:** The new mutation/part reason strings appear in `system.mutations.parts_postpone_reasons`. Preserve the existing lightweight-update string.
- **Partial progress:** `K < U < M` can visibly advance a part to `K` before waiting for `U`.
- **Performance:** Merge selection already snapshots all committing blocks. Filtering is likely cheaper afterward, but recapturing regular and patch vectors up to three times is potentially expensive on tables with many parts. Mutation selection gains a new `O(committing_blocks)` snapshot.
- **Starvation:** A universal publication epoch is especially dangerous on the high-insert tables that most need background merges.
- **Locking:** The epoch approach adds no lock-order edge. The order remains background mutex, then a non-overlapping parts snapshot, then reservation mutex. `removeCommittingBlock` must trigger only after dropping its mutex.
- **Lifetime:** Captured part vectors retain shared pointers for one selection attempt; that is bounded and acceptable.
- **Restart:** Orphan final mutation files remain an acknowledged durable-format limitation.

## Recommended changes to the design

1. Replace §1 lines 99–116 with the boundary-handoff epoch text from Major 2.
2. Replace consequence 5 with the split-phase `NewPart` text from Major 1.
3. Define publication as `PreActive → Active`, not rename/preactivation, and list `Transaction::commit`, `restoreAndActivatePart`, and `swapActivePart`.
4. Add the deterministic `MOVE` regression.
5. Return `CANNOT_SELECT`, not `NOTHING_TO_MERGE`, after validation exhaustion.
6. Define the rule-3 bound as exclusive and test a patch exactly at the reservation.
7. Name the new assignee-backoff failpoint and specify its handshake.
8. State exactly how S2c holds `K` pending.
9. Update the S1 artifact so the claimed pre-fix lost-row arm actually runs.
