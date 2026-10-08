## Verdict — REVISE

Rev.8’s core pending-version design is sound and substantially preferable to rev.6. Atomic allocation/announcement, serialized erasure, and fresh `OPTIMIZE FINAL` predicates close the rounds 1–4 data-version races.

One correctness defect remains in the barrier algorithm: if the highest pending mutation fails, the barrier skips earlier registered or pending mutations. Apart from that, only short test-plan and wording edits remain.

## MAJOR findings

### 1. Atomic allocation and announcement — CLOSED

Rev.8 now requires:

> “A helper acquires `currently_processing_in_background_mutex`, allocates the block number while holding it, inserts `{version, kind}` into `pending_versions`, and only then releases the mutex.”

It also specifies `MOVE` announcement before taking the destination parts lock and fixes the order to `currently_processing_in_background_mutex → committing_blocks_mutex`. [rev.8 §1](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:62)

That closes the round-5 allocation-to-announcement counterexample. Today `allocateBlockNumber` only serializes on `committing_blocks_mutex`, so the new helper must wrap that call while holding the background mutex. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3617)

For `MOVE`, the current allocation occurs inside the destination parts-lock scope. Rev.8 correctly requires moving allocation before that scope. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3260)

### 2. Stale predicate after `OPTIMIZE FINAL` wait — CLOSED

Rev.8 says:

> “After the wait releases and reacquires the mutex, `selectPartsToMerge` constructs a fresh `MergeTreeMergePredicate` and `MergeTreePartsCollector` before rereading parts. No predicate is reused across an unlock.”

[rev.8 §1](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:121)

This is required because the present predicate is built once before the loop, captures `committing_blocks`, `min_update_block`, and patch information, and is reused after `wait_for` releases the mutex. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1540), [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1662), [MergeTreeMergePredicate.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:22)

### 3. Barrier pending-to-registered wait — PARTIALLY CLOSED

Rev.8 correctly recognizes that `waitForMutation` cannot accept a pending version:

> “The barrier first waits … for the pending version to leave `pending_versions` … then … checks whether the version was registered: if so … calls `waitForMutation`; if not, preparation failed or rolled back and there is nothing to wait for.”

[rev.8 §1](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:139)

The first half is correct: current `waitForMutation` treats a missing map entry as terminal and later reports it as killed. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1019), [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1198)

The “nothing to wait for” conclusion is wrong when an earlier mutation exists:

1. Registered mutation `N` is unfinished.
2. Later mutation `P > N` is announced pending.
3. A barrier through `StorageAlias` observes `P` as the highest mutation; alias forwarding does not hold the target alter lock. [StorageAlias.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageAlias.cpp:219)
4. `P` fails preparation and is removed from `pending_versions`.
5. Rev.8 finds that `P` was not registered and proceeds without waiting for `N`.

Multiple pending mutations have the same problem: failure of the highest one can hide a lower pending mutation.

The barrier needs a captured fence, not a single selected pending version:

> Under `currently_processing_in_background_mutex`, capture `barrier_version` as the highest registered or pending mutation version. Wait until no pending mutation with version `<= barrier_version` remains. Then, under the same mutex, find the highest registered mutation with version `<= barrier_version`. If one exists, release the mutex and call `waitForMutation`; otherwise there was no successfully registered mutation before the barrier. Mutations allocated after the fence have higher versions and are not waited for.

This is the only remaining soundness blocker.

## MINOR findings

- **Boundary accessor names — CLOSED.** Rev.8 uses `getCurrentVersionBoundary` and `getNextVersionBoundary`. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:83)

- **Erase-while-locked guard — CLOSED at design level.** Rev.8 requires an explicit erase-and-disarm operation for `addPreparedMutationEntry`, avoiding destructor relocking under the same mutex. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:104)

- **`PendingVersionKind` — CLOSED.** It is retained for the three distinct postpone diagnoses. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:95)

- **Redundant PR #119155 wake-up — CLOSED.** Rev.8 drops the `removeCommittingBlock` trigger and assigns wake-up ownership to pending erasure. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:165)

- **Observability — CLOSED.** Debug insert/erase logging includes version and kind; `system.mutations` remains map-only. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:155)

- **Patch equality and holder order — CLOSED.** Rev.8 explicitly preserves `min_update_block`, requires the update holder to outlive pending erasure, and adds an equality test. This matches the current inclusive `max_patch_version > next_mutation_version` check and strict `< min_update_block` patch snapshot. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:87), [PatchPartsUtils.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:291), [PatchPartsUtils.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/PatchParts/PatchPartsUtils.cpp:368)

- **S6 — CLOSED.** It directly exercises publication and pending removal while `OPTIMIZE FINAL` is inside its mutex-releasing wait. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:221)

- **S7 — PARTIALLY CLOSED.** The mutation failure is injectable, but the plan names no deterministic failure mechanism after `MOVE` preactivation. The pause failpoint alone resumes toward success. Add a one-shot throw after the pause and before destination commit, or specify another deterministic failure injection. Also, polling proves eventual progress, not that the assignee wake-up bypassed backoff.

- **S1 retry wording — OPEN.** It still says B times out and then “both applied.” Explicitly say that B is retried after releasing A. Rev.8 acknowledges this only as an implementation-plan carry-over. [rev.8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md:191)

- **Coherence wording — inaccurate but not a design defect.** Section 4 says patch and move publication become visible “under the same mutex.” They become visible under the data-parts lock; the background mutex serializes the later pending erasure. `Transaction::commit` takes the parts lock and activates parts there. [MergeTreeData.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:9416)

## Answers

### 1. Soundness against every rounds 1–4 race

- **Two mutations:** No counterexample remains once allocation and announcement are atomic. For part version `d`, stopping the registered range at the first pending version above `d` prevents a higher registered mutation from crossing a lower pending one. The separate `mutate` lock-lifetime fix remains necessary because the current `if` initializer releases `alter_lock` before `startMutation`. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1073)

- **Update versus mutation, including settings `ALTER`:** Sound. The pending insertion is unconditional and therefore cannot disappear when `supportsLightweightUpdate` changes. The update keeps its block holder in `MergeTreeSinkPatch`; rev.8 requires pending erase before holder release. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1140), [MergeTreeSinkPatch.h](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSinkPatch.h:22)

- **Merge versus mutation:** Sound. A pending mutation between two data versions changes their current version boundaries, so `canMergeParts` refuses the pair. Both accessor calls occur while the supplied background lock remains held. [MergeTreeMergePredicate.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:50)

- **Merge versus update, including publication/release between captures:** Sound after the retry reconstruction. If a selector owns the background mutex before pending erase, it still sees the boundary even if patch publication happens concurrently. If erase wins first, patch publication has completed and the newly constructed predicate captures it. Equality remains conservatively excluded by the still-live holder and `min_update_block`.

- **`MOVE` split phase:** Sound provided the implementation follows the specified scopes. Allocate and announce before destination `lockParts`; activate using `Transaction::commit`; destroy or explicitly complete the pending guards only after commit or rollback has released the parts lock. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:3260), [MergeTreeData.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:9294)

- **Failure release:** Sound with the stated guard ordering. Mutation cleanup can erase outside the background lock on preparation failure; update cleanup happens after sink unwinding; `MOVE` cleanup must be outside the destination parts-lock scope and after `Transaction` rollback.

- **Transaction rollback:** Pending versions do not solve it. The Antalya branch still requires the upstream registration-time transaction recheck, exactly as rev.8 states. Current rollback calls map-only `killMutation`, so it cannot kill an unregistered prepared mutation. [MergeTreeTransaction.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/MergeTreeTransaction.cpp:315)

- **Orphan final file:** Not solved, and rev.8 correctly scopes it. `loadMutations` loads every final `mutation_*` file; best-effort deletion cannot prove restart safety. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1462)

- **Selector mutex coverage:** Confirmed. Background scheduling holds the mutex across merge and mutation selection. Explicit `OPTIMIZE` does the same. Collector part reads are synchronous and acquire their parts read lock while the caller still owns the background mutex. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1720), [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2038), [MergeTreePartsCollector.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp:126)

The only unlock is the `OPTIMIZE FINAL` wait, now covered by rebuilding both objects.

### 2. The five §5 questions

1. **Fourth writer:** None found. Ordinary insert, attach, replace, and restored attachment allocate and publish under one data-parts lock. `restoreAndActivatePart` and `swapActivePart` publish existing versions rather than allocate new ones. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:2871), [MergeTreeData.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:6006)

2. **Lock inversion:** None found under rev.8’s prohibition on `parts → background`. The added orders are update lock → background → committing, and operation-with-parts mutex → destination background → committing. No reviewed path takes either update or operation mutex while holding the background mutex.

3. **Accessor consumers:** Exactly three plain-`MergeTree` calls: two current-boundary calls in `canMergeParts` and one next-boundary call in patch selection. None dereferences the result in `current_mutations_by_version`. [MergeTreeMergePredicate.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp:51)

4. **Barrier:** Pending-to-registered handling is conceptually correct, but the failure fallback is incomplete. Use the fixed fence algorithm above.

5. **Anything simpler:** No further core mechanism should be dropped. Rev.8 has already removed the redundant wake-up and snapshot/epoch machinery. `PendingVersionKind`, `min_update_block`, retry reconstruction, and failure guards are all justified. The barrier addition could only be dropped by explicitly abandoning alias ordering, which would contradict the stated scope.

### 3. Elegance and minimality versus rev.6

A MergeTree maintainer is much more likely to accept rev.8. It adds one small map to `StorageMergeTree`, reuses the selector mutex, and leaves collector, predicate internals, patch utilities, and transaction machinery unchanged.

After correcting the barrier loop, nothing material remains to drop from the core design.

### 4. Test plan

The primary race tests are deterministic and red for the intended reason:

- `05227`, S1, S2, S3, S4, S5, and S6 target concrete pre-fix races.
- S2c correctly checks safe partial progress through `K`.
- S3b is intentionally green today and is properly described as a pin, not a red test.
- S7 is a failure-path regression test; it is not expected to be red on the old code because the old code has no pending map.

Required additions/clarifications:

- Add a barrier test with unfinished registered `N`, pending `P > N`, and failed `P`; the barrier must still wait for `N`.
- State explicitly that S1 retries B after A is released.
- Add a deterministic throw after `MOVE` destination preactivation.
- If S7 claims to prove immediate wake-up, add a deterministic long-backoff arrangement; otherwise change the claim to eventual progress.

### 5. Superseding PR #119155

No objection to this shape. Replacing its `Op::Update` scan with `allocatePendingVersion(LightweightUpdate)` is cleaner and removes the settings-dependent TOCTOU.

Preserve:

- `PENDING_LIGHTWEIGHT_UPDATE`;
- `05227` with bounded failpoint waits;
- original commits/authorship.

Drop its `removeCommittingBlock` scheduling trigger, as rev.8 specifies. The update insertion must be part of the atomic helper, not a separate insert after allocation.

### 6. Risks

- **Lock ordering:** No cycle found. `MOVE` must never clean up pending state while a destination parts lock is alive.
- **Contention:** Updates and `MOVE` now wait on the background mutex for both announcement and cleanup. `select_in_partition` can hold it across one-second memory-pressure sleeps. [StorageMergeTree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/StorageMergeTree.cpp:1618)
- **Observability:** Debug insert/erase logging is sufficient for this PR; keeping pending entries out of `system.mutations` is correct.
- **Behavior:** Merges spanning pending versions are temporarily refused; mutations can safely apply registered prefixes below the first pending boundary; alias barrier alters wait longer.
- **Failure sensitivity:** A leaked pending entry can indefinitely block later work, making RAII and deterministic failure tests mandatory.
- **Compatibility:** No on-disk, wire-format, or system-table schema change.
- **C++ safety:** No new ownership, integer, lifetime, or undefined-behavior concern is inherent in the design, provided guard and `Transaction` destruction order is made explicit.

## Recommended changes

1. Replace the barrier paragraph with the captured-fence algorithm above.
2. Add the failed-highest-pending barrier regression test.
3. In S1, explicitly say “release A, retry B, then wait for both.”
4. Give S7 a deterministic post-preactivation `MOVE` throw; either add a backoff hook or stop claiming that polling proves immediate wake-up.
5. Replace the §4 phrase “patch commit or move commit made it visible under the same mutex” with:

> Mutation registration occurs under `currently_processing_in_background_mutex`. Patch and moved-part publication occur under the data-parts lock; after publication finishes, pending erasure under the background mutex forms the handoff to the next selector.

No implementation or tests were run because this was a review of the design against the unpatched base tree.
