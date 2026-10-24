# MergeTree: committing blocks as version boundaries — design (rev.6) {#design}

Target: upstream `master` (13d023155c7) on top of the two commits of PR #119155, one pull request.
Supersedes the mechanism of `2026-09-28-mergetree-uncommitted-version-boundary-design.md` (rev.10, the
`PendingVersions` registry): same races, same tests where they still apply, no new registry. The reason for the
restart is recorded there and in the SDD ledger: rev.1–rev.6 of that document were this design and were abandoned
after design review round 3 found the counterexample in §3 below; the fix chosen then (an extra registry erased under
the selection mutex) closed it at the cost of ~460 lines, a lock coupling on every mutation and update, and an alter
recovery re-arm that protects nothing. This document closes the same counterexample with one rule (§3) instead.

## 0. The races {#races}

Plain `StorageMergeTree` decides what a mutation, a merge or `OPTIMIZE FINAL` may touch from the registered
mutations (`current_mutations_by_version`) and the parts, while a *version* exists earlier than either:

- a mutation has its block number from `allocateBlockNumber(Op::Mutation)` in `prepareMutationEntry` until
  `addPreparedMutationEntry` registers it (the file is written in between);
- a lightweight update has its block number from `allocateBlockNumber(Op::Update)` in `updateLightweight` until its
  patch parts are committed (the holder dies with `MergeTreeSinkPatch`).

R1. Two mutations register out of order: `mutate` released the alter lock before `startMutation` (issue #122083).
R2. `selectPartsToMutate` applies a mutation to a part while a lightweight update of that part is between allocation
    and patch commit (issue #116686; PR #119155 postpones it, but behind `supportsLightweightUpdate()`, which a
    concurrent `MODIFY SETTING` flips, and only for `Op::Update`).
R3. A merge spans a version that is allocated but not visible, or records a metadata mutation's version past it in
    `getMutationVersionForMergedPart`, so the update's patch is skipped for the merged part.
R4. `OPTIMIZE FINAL` retries after waiting for running merges with the predicate and the metadata snapshot it built
    before the wait (a pre-existing bug, found by test 05297 of the previous branch).

## 1. The rule {#rule}

**`committing_blocks` is the registry of allocated-but-invisible versions.** Every block number is inserted there by
`allocateBlockNumber` under `committing_blocks_mutex` and removed by the holder's destructor, which `startMutation` and
`alter` run right after the final insert into `current_mutations_by_version`, under the background mutex, and which for
an update runs with the sink after the last patch part is committed. A registered version is never seen as pending by a selection, with one benign exception: `startMutation`
throwing after the insert leaves the block reserved until the holder dies, which postpones selection until the next
scheduler run and loses nothing. This is what `ReplicatedMergeTree` relies on already (`ReplicatedMergeTreeMergePredicate` refuses merges
across an uncommitted block; the queue postpones a mutation while a lower update is uncommitted).

A **version reservation** is a committing block with `op ∈ {Mutation, Update}`. `NewPart` blocks are not boundaries on
this target: an insert allocates, inserts and activates its part in one data-parts write-lock section, and
`MOVE PARTITION TO TABLE` commits its destination under the parts lock since PR #108922. (Their inclusion, as in
`ReplicatedMergeTree`, is a separate question and is out of scope.)

Selectors take a **snapshot** of the reservations under `currently_processing_in_background_mutex` (which they already
hold) and read it as follows. `firstReservationAfter(dv)` = the lowest reservation strictly above `dv`, or none.

1. `selectPartsToMutate`: one snapshot per selection pass, before the parts are read (as #119155 does), taken
   unconditionally (the `supportsLightweightUpdate()` guard is removed). For each part, the applicable range of
   registered mutations is `[upper_bound(dv), lower_bound(firstReservationAfter(dv)))`. If the range is empty because of
   an `Update` reservation, the part is postponed with `PostponeReasons::PENDING_LIGHTWEIGHT_UPDATE` (kept verbatim from
   #119155). A registered mutation is never above an unregistered `Mutation` block: registrations are ordered by the
   alter lock (§4) and every registration releases its block under the background mutex before the mutex is dropped, so
   the `Update` reason is the only one.
2. `MergeTreeMergePredicate::canMergeParts`: refuse if a reservation lies in `(min(dv_l, dv_r), max(dv_l, dv_r)]`
   (`ReplicatedMergeTree`'s rule for uncommitted blocks, restricted to reservations). `canUsePartInMerges`'s
   `min_update_block` refusal stays.
3. `getMutationVersionForMergedPart`: the walk that records a materialized metadata mutation's version ends at
   `lower_bound(firstReservationAfter(sources_dv))`; the pre-check and the `RENAME` refusal loop stay unbounded (a bounded
   pre-check would skip the refusal, pinned by test 05296). The caller passes the predicate's `CommittingBlocksSnapshot`; the bound is looked up inside.
4. `getPatchesToApplyOnMerge`: `next_version = min(getNextMutationVersion(dv), firstReservationAfter(dv))`, plus the
   watermark rule of §3.
5. No wake-up on release. `alter` resets its mutation's block holder right after the final registration (success path,
   replicated-database branch, re-registration after a failed durable rollback) while it still holds the background
   mutex, exactly as `startMutation` does, so the scheduler run that the registration triggers never sees the block as
   pending; #119155's wake-up on update commit covers `Update` blocks. Only the rollback-success path releases the block
   later, at scope end: merges refused below it during the rollback resume on the next scheduler run.

Why a snapshot is enough (the monotonicity argument): block numbers come from one `SimpleIncrement`, so a version
allocated after the snapshot is above every part, every registered mutation and every reservation that existed at the
snapshot. A later mutation cannot register while the selector holds the background mutex. But the *parts* are read
after the snapshot (`MergeTreePartsCollector` runs after the predicate is constructed), and an insert activates its part
under the parts lock without the background mutex, so a part created after the snapshot can enter the candidate set
while a version allocated between the snapshot and that part is in no snapshot (design review of rev.1: mutation `2`
allocated after the snapshot, insert `3` activated, merge of `1` and `3` selected, `2` later deletes rows of block `3`).
Hence the **watermark** (§3) also bounds the candidates: a part whose `min_block` is above the watermark is not a
candidate of this selection. (`min_block`, not the data version: a restored or attached part keeps a mutation version
from its source table that can be above this table's block counter, and it must stay mergeable; a part whose blocks
were all allocated after the snapshot is exactly the late candidate.) Reachability (settled in Task 4's review): mutation and update planning take
`currently_processing_in_background_mutex` and release it before the block is allocated in `prepareMutationEntry`, so a
mutation that finished planning just before a selection took the mutex allocates its version during that selection —
the interleaving above is reachable, and the candidate rule is what closes it. Its test pauses the mutation right
before `allocateBlockNumber` and the `OPTIMIZE` right after its snapshot (a pause reached only by the explicit
`OPTIMIZE` path, so a background selection cannot consume it). It is *filtered out* before the partition-wide checks (`MergeTreePartsCollector`
drops it from the initial set), not refused: refusing it would make `OPTIMIZE FINAL` fail whenever an insert lands
between the snapshot and the collection, because `FINAL` requires every part of the partition to be usable
(confirmation round of rev.2). The filtered parts are the newest ones; a `FINAL` under a concurrent insert stream never
guaranteed a single part anyway. Everything that participates in one selection — reservations, registered mutations,
parts and patches — is then at or below one point of the block sequence, and nothing allocated later can be between
two things the selector compares.

## 2. The lock fix and the two small fixes, separately {#separate}

- `mutate` keeps the alter lock through `startMutation` (R1; commit 6109419a064 of the previous branch, test 05291).
- `OPTIMIZE FINAL` rebuilds the predicate and the parts collector and re-reads `getInMemoryMetadataPtr()` on every retry
  after the wait released the mutex (R4; commits b9fbb41381a and b637b72b894 of the previous branch, test 05297).
- `alter`'s recovery path finds the rename mutation by version in `current_mutations_by_version`, registered or
  not, and rolls it back or re-registers it from there; `addPreparedMutationEntry` takes the entry by reference and
  moves it only into the map, so no path sees a moved-from entry (`chassert(held_entry->commands)`; failpoint
  `mt_throw_after_mutation_entry_inserted`, test 05302: an orphaned `mutation_*.txt` replayed after `DETACH`/`ATTACH`
  emptied the renamed column).
- **`alter` retains the rename mutation's block holder** (it stays inside `prepared`, whose scope ends after the
  recovery block) until the new metadata is published or the recovery interval has ended. Design review of
  rev.1: with the holder released at registration, a failed in-memory commit un-registers the rename `R` for the durable
  rollback while nothing reserves `R`; an update `U > R` commits its patch (updates do not take the alter lock), a merge
  below `R` materializes `U` and records version `U`, the rollback fails, `R` is re-registered and is skipped for the
  merged part: pre-existing rows lose the renamed column. With the holder retained, `R` is a reservation throughout,
  so the merge neither materializes `U` (rule 4) nor crosses `R` (rule 2). No lock is held longer; the holder just
  dies a few statements later. `prepared` is declared before `background_lock`, in the scope that ends before
  the synchronous `waitForMutation`, so it is released on every exit: under the background mutex right after the final registration (success,
  replicated-database branch, re-registration after a failed durable rollback), and at scope end after a successful
  rollback, before any wait that could depend on it. Consequence
  (confirmation round): a lightweight update issued while a failing `ALTER` is in its recovery interval waits in
  `waitForCommittingInsertsAndMutations` for the retained reservation, bounded by `lock_acquire_timeout`, as it already
  waits for any registering mutation.

## 3. The counterexample and the watermark {#watermark}

The monotonicity argument covers parts and mutations. It does not cover **patch materialization**, because a merge that
applies a patch raises the merged part's version to the patch's version (`FutureMergedMutatedPart::assign`,
`FutureMergedMutatedPart.cpp:68-77`). Design review round 3 of the previous document:

1. part `P` with `dv < U`; update `U` allocated, paused before its patch commit;
2. update `V > U` (disjoint rows) commits its patch — updates do not wait for lower updates;
3. the merge predicate copies the reservations at `t1` and the patch parts at `t2 > t1` (the caller takes the
   snapshot under the background mutex and passes it to `MergeTreeMergePredicate`'s constructor, which reads the
   patch parts afterwards); `U` was allocated after `t1` and `V` committed before `t2`;
4. `U` is in no snapshot, patch `V` is; the merge materializes `V` and the result's version becomes `V`;
5. `U` commits its patch below the result's version: it never applies to the merged part. Data loss.

The window is the constructor's own duration. Through `updateLightweight` it is empty: an update waits in
`waitForCommittingInsertsAndMutations` for every lower non-`Update` block, and it plans its read under the background
mutex (`getMutationsSnapshot`), which the selector holds from the reservation copy to the patch read (verified with a
live stack trace). `ATTACH PART` of a detached patch part reaches it: attach takes a fresh block number without waiting
and without the mutex, so the patch can land above a mutation that is not registered yet or above the snapshot
(test 05304, failpoint `mt_optimize_pause_before_reading_patches`; both bounds below stay for that route). Rule: **the predicate reads the reservations and a
watermark together, before it reads the patch parts, and never materializes a patch above the watermark.** The
watermark is the value of `increment` at the moment of the reservation copy, read under `committing_blocks_mutex`, the
same mutex `allocateBlockNumber` increments and inserts under, so "allocated at or before the watermark" and "present in
the snapshot or already released" are the same statement. Then, for the merge's patch selection:

- `U` allocated before the copy: `U` is in the snapshot (still uncommitted at `t1`) → `min_update_block ≤ U` → every
  patch with version `≥ U`, including `V`, is left for read time (existing rule);
- `U` allocated after the copy: `U > watermark` and `V > U > watermark` → `V` is left for read time (new rule).

A patch left for read time is applied by block number and offset, idempotently, exactly as it is applied to the source
parts today; only materialization on merge raises the version. `getPatchesToApplyOnMerge` therefore skips any patch with
`max data version > watermark`. The order "reservations and watermark, then patches" is load-bearing and is stated in the
constructor; the reverse order has the symmetric hole (`U` committing between the two copies).

Reachability note (design review of rev.1): on the current code the patch interleaving above cannot complete, because an
update's planning (`MutationsInterpreter` → `getMutationsSnapshot`) takes the background mutex the selector holds, so `V`
cannot commit during the selection. The rule is kept because it is the same watermark as the candidate rule (one
statement: nothing above the watermark participates in a selection) and because it does not depend on that ordering
of the update pipeline. The candidate side of the rule is reachable today and has a red test (§5).

## 4. Why it is sound, and what it does not claim {#argument}

- I1 (no gap): a version is a committing block from allocation until visibility (§1). The alter recovery interval
  un-registers the rename mutation, but `alter` keeps its block holder (§2), so the version stays a reservation until
  the interval ends; no mutation can register above it meanwhile (the failing `ALTER` holds the alter lock). A merge
  that started before the failure with the new metadata recorded the rename's version and wrote the new names, which is
  consistent with re-registration. (The value loss for rows *inserted* in that window is a pre-existing bug,
  `tmp/cas252/issue_alter_rollback_window_loses_renamed_column.md`, not changed by this design.)
- Known limits, pre-existing and unchanged: an orphaned `mutation_*.txt` that `loadMutations` registers after a restart
  is outside any in-process snapshot; inside a transaction, a committed patch that the merge's transaction cannot see is
  filtered by `getPatchPartInfos`, and a later visible patch can still be materialized above it (§4's transaction
  exclusion). Intended behaviour changes: a lower registered mutation is applied while a higher one waits behind a
  pending update (partial progress, one extra rewrite); a part already above a pending update proceeds with later
  mutations; merges refused by a reservation are retried by the scheduler, and an explicit `OPTIMIZE FINAL` refused by one
  returns that refusal for the attempt (a no-op, or `CANNOT_ASSIGN_OPTIMIZE` with `optimize_throw_if_noop = 1`) like
  any other refused merge; patches above the watermark are left for read time.
- I2 (exact snapshot): the monotonicity argument of §1 for parts and mutations; §3 for patches.
- I3 (alter lock): every path that registers a mutation on plain `MergeTree` holds the alter lock: `mutate` (after the
  R1 fix) and `alter` (its own rename mutation, from the interpreter's `lockForAlter`). Hence no registered mutation
  above a `Mutation` reservation, and no barrier-`ALTER` fence.
- Lock order: unchanged. `currently_processing_in_background_mutex → committing_blocks_mutex` (predicate constructor,
  `selectPartsToMutate`, the block release in `startMutation` and `alter`); no new edge; nothing takes the
  background mutex on the allocation path (this is the contention the previous design added and this one does not).
- Not claimed: `NewPart` blocks as boundaries; `ReplicatedMergeTree`; `SharedMergeTree`; transactions beyond what 04516
  already pins.

## 5. Tests, failing first {#tests}

Implemented numbering (this branch, ten files): 05291 R1, 05292 R2, 05294 merge boundary, 05295 failure/kill pins, 05296 merged-part version cap, 05297 late candidate (real reproduction; arms for the transaction path and the restored-part guard), 05298 rename recovery with the retained holder, 05299 `OPTIMIZE FINAL` retry (rebuild + metadata), 05302 alter rollback after the entry was inserted (M2), 05304 patch bounds via `ATTACH PART`. Reused from the previous branch where their expectations hold on this design:
05291 (R1, red before the lock fix), 05292 (R2: settings-independent postponement, cancelled update wakes the mutation,
truncation: a lower registered mutation applies while a higher one waits), 05294 (merge across a paused mutation refused; merge below a pending update keeps it applicable), 05295 (failed mutation
releases its block; `KILL MUTATION` ignores an unregistered one), 05296 (merge does not record a metadata mutation past
a pending update; `RENAME` refused), 05297 (`OPTIMIZE FINAL` retry with a held merge sees the `RENAME` published during
the wait), 05227 (from #119155, timeout wrapper), 04516, 04309, 04312.

New, red without the watermark on candidates: **05298 the late-candidate interleaving.** `PAUSEABLE_ONCE` failpoint
`mt_merge_predicate_pause_after_reservation_snapshot` right after the predicate copied the reservations (it holds the
background mutex there); `SYSTEM STOP MERGES`; part `P` (id 1); `OPTIMIZE TABLE … FINAL` in the background reaches the
failpoint; while paused: `DELETE … WHERE id = 2` (lightweight_delete_mode = alter_update, sync 0) in the background —
it allocates version 2 and blocks on the background mutex at registration; `INSERT … (2)` activates part `Q` at block 3
(parts lock only); release the predicate; `wait` OPTIMIZE and the DELETE; wait for the mutation to finish; assert
`SELECT count() FROM t WHERE id = 2` = 1 (the row inserted after the mutation's version survives) and the parts.
Without the candidate watermark, `P` and `Q` merge at data version 1 and the mutation deletes the row of block 3.

New, red without the retained holder: **05299 the rename recovery interleaving.** Failpoints
`mt_alter_throw_after_mutation_registered` (ONCE), `mt_alter_pause_before_durable_rollback` (PAUSEABLE_ONCE, added at the
recovery interval as on the previous branch) and `mt_alter_throw_in_durable_rollback` (ONCE); table `(id, x String, y)`
with block number/offset columns, two parts; `ALTER … RENAME COLUMN x TO z` in the background reaches the pause; while
paused: `UPDATE t SET y = 2 WHERE 1` started in the background (with the retained holder it blocks in
`waitForCommittingInsertsAndMutations` behind `R` — verify with a bounded poll on `system.processes`; without it, it
commits its patch), then `OPTIMIZE TABLE … FINAL` (foreground; with the fix there is no patch to materialize and `R` is a
reservation; without it the merge materializes `U` and records version `U > R`); release the pause; the rollback fails,
the rename is re-registered; `wait` the update and the alter; wait for mutations; assert every row still reads its `x`
value under `z` and `y = 2`. Without the retained holder `z` reads defaults for the merged rows.

Test rules as before: tags with reasons, `timeout 60` around every `SYSTEM WAIT FAILPOINT`, traps that disable every
failpoint and `wait`, `KILL … ASYNC` for parked queries, bounded polls, full test names on the command line.

## 6. Size {#size}

Source: `StorageMergeTree.{h,cpp}` (mutate lock scope; `selectPartsToMutate` end bound; `getMutationVersionForMergedPart`
bound parameter; `alter` retains the block holder; `OPTIMIZE FINAL` retry; recovery lookup by version;
`getCommittingBlocksSnapshot`), `MergeTreeCommittingBlock.h` (`CommittingBlocksSnapshot`: sorted reservation numbers,
`minUpdateBlock`, `watermark`), `MergeTreeMergePredicate.{h,cpp}` (snapshot parameter; `canMergeParts` reservation rule;
`getPatchesToApplyOnMerge` bounds; `firstReservationAfter`), `MergeTreePartsCollector.cpp` (`collectInitial` drops parts with
`min_block > watermark`), `FailPoint.cpp` (failpoints `mt_pause_before_register_mutation` (#119155),
`mt_mutation_pause_before_block_allocation`, `mt_optimize_pause_after_reservation_snapshot`,
`mt_optimize_pause_before_reading_patches`, `mt_throw_after_mutation_entry_inserted`, `mt_alter_pause_before_durable_rollback`).
Size on the branch: +225/−85 lines in `src` against the #119155 pair (39 of them comments). No new header, no gtest, no new
object kind.
