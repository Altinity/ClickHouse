# MergeTree committing-block boundaries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the mutation-ordering races of plain `StorageMergeTree` by treating `committing_blocks` as the registry of allocated-but-invisible versions, in one upstream pull request on top of PR #119155, replacing the `PendingVersions` branch (`fix/mergetree-pending-versions`, kept as reference: its tests and three of its commits are reused).

**Architecture:** Selectors (mutation selection, the merge predicate, the merged-part version helper) take a snapshot of the `Mutation`/`Update` committing blocks under the background mutex they already hold and treat the first such block above a part's data version as a boundary; patch materialization on merge is additionally capped by a watermark read together with the snapshot (§3 of the spec). Plus the independent fixes: `mutate` keeps the alter lock through `startMutation`; `OPTIMIZE FINAL` rebuilds predicate, collector and metadata on retry; the alter recovery path does not re-register a moved-from entry.

**Tech Stack:** C++23 (ClickHouse), stateless bash tests (`tests/clickhouse-test`), failpoints, ninja in the worktree `~/workspace/ClickHouse/mt-pending-versions` (branch `fix/mergetree-committing-block-boundaries`, base b8749fd2dfd = upstream/master 13d023155c7 + #119155's two commits + a test timeout wrapper).

**Spec:** `docs/superpowers/specs/2026-09-29-mergetree-committing-block-boundaries-design.md` (rev.3). Reference branch with reusable commits/tests: `fix/mergetree-pending-versions` (head 627c5c816dd) in the same worktree.

## Global Constraints

- Only plain `StorageMergeTree`, its merge predicate and its parts collector change (the collector only drops above-watermark parts from the initial set). `ReplicatedMergeTree`, `MergeTreeData::Transaction` untouched. No new header, no gtest, no new registry, no lock held longer than today; lock order stays `currently_processing_in_background_mutex → committing_blocks_mutex`; nothing takes the background mutex on an allocation path.
- `NewPart` blocks are not boundaries (spec §1). `PostponeReasons::PENDING_LIGHTWEIGHT_UPDATE` and test `05227_mutation_waits_for_lightweight_update` stay verbatim from #119155 (timeout wrapper already applied).
- The predicate reads reservations and the watermark BEFORE the patch parts (spec §3); that order is stated in a comment.
- Nothing in code, comments, tests or commit messages mentions CAS, content-addressed disks, Altinity or Claude; `git commit -s` only (no other trailers); Allman braces; comments state the reason (at most ~6 lines each); SQL/function names in inline code marks in commit messages.
- Tests: reused files are copied from the reference branch by `git show fix/mergetree-pending-versions:<path> > <path>` and adapted only where this plan says; tags with one comment line each; every `SYSTEM WAIT FAILPOINT` through a `timeout 60` helper; traps disable every failpoint used (stderr visible), `wait`, drop tables; bounded polls; `KILL … ASYNC` for parked queries; `$MUTATIONS` macro contains `WHERE` (append with `AND`); active-part counts exclude `partition_id` starting with `patch-`; full test names on the command line.
- Build: `build/` is release with `-DWERROR=OFF` (local clang 21.1.8 `-Wpsabi` in untouched `ColumnsCommon.cpp`); `ninja -C build clickhouse > build/build_<task>.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_<task>.log`, foreground, re-run on timeout, never two ninja at once; `grep -n "warning:" build/build_<task>.log | grep -v ColumnsCommon` must print nothing.
- Test server: `bash tmp/start_server_ci_configs.sh` from the worktree root (CI configs installed under `tmp/srv/etc`, embedded Keeper with ACLs; 04516 needs it); port 19681; stop only with `SYSTEM SHUTDOWN`; one owner at a time (the current implementer). Binary must be newer than the last source edit before every run (`ls -la --time-style=+%T build/programs/clickhouse`).
- Never `pkill -f`/`pgrep -f`/`ps | grep` with a pattern in your own command line; `grep` is ugrep; temp files under the worktree's `tmp/` or `build/`; commit exact files with `git add <path>`; leave untracked `access/`, `preprocessed_configs/`, `tmp/` alone; do not push, rebase or amend.

## Review Focus

1. A mutation allocated after the predicate's reservation copy and an insert activated before the parts are read (spec §1, late candidate): the new part must not be a merge candidate of that selection. Pinned by 05298 (Task 4). And the rename recovery interleaving (spec §2): a merge during the durable-rollback interval must not record a later update's version. Pinned by 05299 (Task 4).
2. A mutation that throws after allocation (`mt_throw_after_mutation_commit`): its block is released by the holder's destructor with no lock held; a merge refused by that reservation proceeds afterwards. Pinned by 05295 (Task 5).
3. `ALTER TABLE … MODIFY SETTING enable_block_offset_column = 0` while an update is parked: the mutation must still be postponed (no `supportsLightweightUpdate()` guard). Pinned by 05292 section 1 (Task 2).
4. A `RENAME COLUMN` registered above a parked update: the merge that would materialize it is refused, not silently recorded. Pinned by 05296 arm 2 (Task 3).
5. `OPTIMIZE FINAL` retry after a held merge sees a `RENAME` published during the wait with fresh metadata. Pinned by 05297 (Task 6).

---

### Task 1: Alter lock through `startMutation` (R1) — cherry-picks

**Files:** `src/Common/FailPoint.cpp`, `src/Storages/StorageMergeTree.cpp`, `tests/queries/0_stateless/05291_mutations_register_in_order.{sh,reference}`

- [ ] `git cherry-pick 61cba63f8bb` (failpoint `mt_pause_once_before_register_mutation` + test 05291, RED on base) then `git cherry-pick 6109419a064` (lock scope). Both apply cleanly (same base). Do not edit their messages.
- [ ] Build, start the server, run `05291_mutations_register_in_order` → `[ OK ]`. RED evidence: the reference branch's ledger records the pre-fix failure; optionally re-verify by running 05291 before the second cherry-pick.

---

### Task 2: Mutation selection ends at the first reservation (R2)

**Files:**
- Modify: `src/Storages/StorageMergeTree.h` (snapshot type + accessor), `src/Storages/StorageMergeTree.cpp` (`getCommittingBlocksSnapshot`, `selectPartsToMutate`)
- Test: `tests/queries/0_stateless/05292_mutation_waits_for_pending_versions.{sh,reference}` copied from the reference branch (three sections: settings-independent postponement; cancelled update wakes the mutation; truncation with `STOP/START MERGES`). Section 1 is RED on the current code (the guard skips the scan). Keep the file name.

**Interfaces (produced, used by Tasks 3-5):**

```cpp
/// StorageMergeTree.h, next to getCommittingBlocks()
struct CommittingBlocksSnapshot
{
    CommittingBlocksSet blocks;      /// every committing block at the time of the copy
    Int64 watermark = 0;             /// value of `increment` at the same instant: every block allocated later is above it
    /// Lowest Mutation/Update block strictly above data_version, or nullopt. NewPart blocks are not boundaries.
    std::optional<Int64> firstReservationAfter(Int64 data_version) const;
};
CommittingBlocksSnapshot getCommittingBlocksSnapshot() const;   /// one lock_guard on committing_blocks_mutex for both fields
```

- [ ] **Step 1: copy the test, run it RED** (`build/test_05292_before.log`; expected: section 1 times out waiting for the postpone reason).
- [ ] **Step 2: implement.** `getCommittingBlocksSnapshot`: under `committing_blocks_mutex`, copy `committing_blocks` and read `increment.value` (`SimpleIncrement` has `std::atomic<UInt64> value`; read it after the copy under the same lock — allocation increments and inserts under this mutex, so the two agree). `firstReservationAfter`: linear scan of the ordered set from `upper_bound({Op::Unknown, data_version})`-equivalent (use `std::ranges::find_if` over blocks with `number > data_version && (op == Mutation || op == Update)`; the set is ordered by number). In `selectPartsToMutate`: replace the guarded `committing_blocks_snapshot` with `const auto reservations = getCommittingBlocksSnapshot();` taken where #119155 took it (before the parts are read); replace the `pending_update_block` loop with:

```cpp
        auto mutations_begin_it = current_mutations_by_version.upper_bound(part->info.getDataVersion());
        auto mutations_end_it = current_mutations_by_version.end();
        /// A version that is allocated but not visible yet (a lightweight update before its patch parts are
        /// committed, a mutation before its registration) is a boundary: only the registered mutations below it
        /// may be applied to this part now, or the part would be rewritten at a data version the pending
        /// update's patch no longer applies to.
        if (auto reservation = reservations.firstReservationAfter(part->info.getDataVersion()))
            mutations_end_it = current_mutations_by_version.lower_bound(static_cast<UInt64>(*reservation));
        if (mutations_begin_it == mutations_end_it)
        {
            if (auto reservation = reservations.firstReservationAfter(part->info.getDataVersion());
                reservation && reservations.blocks.contains(CommittingBlock(CommittingBlock::Op::Update, *reservation)))
            {
                LOG_DEBUG(log, "Will not mutate part {} yet because the lightweight update with block number {} is not committed", part->name, *reservation);
                current_parts_postpone_reasons[part->name] = PostponeReasons::PENDING_LIGHTWEIGHT_UPDATE;
            }
            continue;
        }
```

  (Shape the `op` lookup as the implementer sees fit: e.g. make `firstReservationAfter` return the `CommittingBlock` itself. Keep the existing `mutations_end_it`-based loop below unchanged otherwise; delete the old `mutations_end_it = end()` above the loop and the `supportsLightweightUpdate()` guard.)
- [ ] **Step 3: build, restart, GREEN** for 05292, 05291, 05227. Commit: "Postpone a mutation at the first allocated version above the part, regardless of the table settings".

---

### Task 3: Merge predicate and merged-part version respect reservations (R3)

**Files:**
- Modify: `src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.{h,cpp}`, `src/Storages/StorageMergeTree.{h,cpp}` (`getMutationVersionForMergedPart` bound parameter; call site in `selectPartsToMerge`)
- Tests: `05294_merge_does_not_cross_pending_version.{sh,reference}` and `05296_merge_does_not_record_metadata_mutation_past_pending_update.{sh,reference}` copied from the reference branch (05294 arm 1 and 05296 both arms are RED on the current code; 05294 arm 2 is a pin).

**Interfaces:** the predicate stores `CommittingBlocksSnapshot reservations` (replacing the bare `committing_blocks` member; `min_update_block` computed from `reservations.blocks` as before) and exposes `std::optional<Int64> firstReservationAfter(Int64) const`; `getMutationVersionForMergedPart(Int64 sources_data_version, const String & partition_id, std::optional<Int64> first_reservation_above, lock)`.

- [ ] **Step 1: copy both tests, run RED** (`build/test_task3_before.log`).
- [ ] **Step 2: implement.**
  - Candidate watermark: add `bool MergeTreeMergePredicate::isPartAfterSnapshot(const MergeTreeDataPartPtr &) const` (`info.min_block > reservations.watermark` — NOT the data version: restored/attached parts keep a source-table mutation version that can exceed this table's counter and must stay mergeable) and in `MergeTreePartsCollector::collectInitial` (`src/Storages/MergeTree/Compaction/PartsCollectors/MergeTreePartsCollector.cpp`, before `checkAllParts` and before the ranges are built) erase such parts from the initial vector with a four-line reason: created after the reservation snapshot, a version between the snapshot and the part may be in no snapshot; dropped rather than refused so `OPTIMIZE FINAL` still merges the parts the snapshot covers. Do NOT put it in `canUsePartInMerges` (that would make `FINAL` fail under a concurrent insert stream). The RED test is 05298 in Task 4; Task 3 may implement this already but must say so.
  - `canMergeParts`, after the mutation-version comparison: `auto lo = min(left dv, right dv), hi = max(...)`; if `reservations.firstReservationAfter(lo)` is set and `≤ hi` → refuse with "Parts {} and {} have an allocated version {} between them that is not visible yet" (a mutation before registration or a lightweight update before its patch commit).
  - `getPatchesToApplyOnMerge`: `next_version = min over {storage.getNextMutationVersion(dv), *firstReservationAfter(dv) if set}` (0 means none for the first; treat as infinity when combining), and drop patches with `getDataVersion() > reservations.watermark` before calling `DB::getPatchesToApplyOnMerge` (comment: spec §3 reason, four lines).
  - Constructor: `reservations(storage.getCommittingBlocksSnapshot())` in the initializer list, comment that it must precede the body's patch read and why.
  - `getMutationVersionForMergedPart`: new parameter; the recording loop stops at `lower_bound(*first_reservation_above)` when set; pre-check and the `RENAME` refusal loop unchanged. Call site passes `merge_predicate->firstReservationAfter(future_part->part_info.getDataVersion())`.
- [ ] **Step 3: build, restart, GREEN** for 05291, 05292, 05294, 05296, 05227. Commit: "Refuse merges across an allocated version and never record a metadata mutation past it".

---

### Task 4: The two rev.2 counterexamples — failing first, then the retained holder

**Files:**
- Modify: `src/Common/FailPoint.cpp` (`PAUSEABLE_ONCE(mt_merge_predicate_pause_after_reservation_snapshot)`, `PAUSEABLE_ONCE(mt_alter_pause_before_durable_rollback)`), `MergeTreeMergePredicate.cpp` (pause as the first statement of the constructor body, `extern` declaration), `src/Storages/StorageMergeTree.cpp` (`alter`: move the block holder out of `prepared` before `addPreparedMutationEntry`, keep it in a local declared before the `try` that publishes, so it dies after publication on success and after the recovery block on failure; pause failpoint at the recovery interval right after `background_lock.unlock()`)
- Create: `tests/queries/0_stateless/05298_merge_ignores_parts_created_after_the_snapshot.{sh,reference}` and `05299_alter_rollback_keeps_rename_reserved.{sh,reference}` via `add-test` (numbers may differ)

- [ ] **Step 1: 05298 (late candidate), RED if Task 3 did NOT implement the candidate watermark.** `SYSTEM STOP MERGES`; `CREATE TABLE t (id UInt64) ENGINE = MergeTree ORDER BY id`; `INSERT (1)` (part P, block 1); enable the predicate pause; `OPTIMIZE TABLE t FINAL` in the background; `wait_failpoint`; in the background `DELETE FROM t WHERE id = 2` with `lightweight_delete_mode = alter_update`, `lightweight_deletes_sync = 0` (allocates version 2, blocks on the background mutex at registration — verify with a bounded poll on `system.processes` that it is running); `INSERT (2)` (part Q, block 3, foreground; needs no background mutex); disable the predicate pause; `wait` OPTIMIZE and the DELETE; wait until the mutation is done; assert `SELECT count() FROM t WHERE id = 2` → 1 and list active parts. If the DELETE does not stay blocked (registration path changed), report. RED evidence: `count() = 0` without the candidate rule.
- [ ] **Step 2: 05299 (rename recovery), RED before the retained holder.** Failpoints `mt_alter_throw_after_mutation_registered`, `mt_alter_pause_before_durable_rollback`, `mt_alter_throw_in_durable_rollback`; table `(id UInt64, x String, y UInt64)` with `enable_block_number_column = 1, enable_block_offset_column = 1, min_bytes_for_wide_part = 0`, two parts with `x = 'a'`/`'b'`, `y = 1`; `ALTER TABLE t RENAME COLUMN x TO z SETTINGS alter_sync = 2` in the background (fails with FAULT_INJECTED at the end); `wait_failpoint` on the pause; while paused: start `UPDATE t SET y = 2 WHERE 1` (`enable_lightweight_update = 1`) in the BACKGROUND — with the retained holder it blocks in `waitForCommittingInsertsAndMutations` behind the rename's reservation (poll `system.processes` until the query is running; it must not finish while paused), without it it commits its patch; then `OPTIMIZE TABLE t FINAL` (foreground; with the fix nothing is materialized and the reservation refuses the merge or leaves the parts, without it the merge records version U); disable the pause; `wait` the update and the alter; wait for mutations to finish; assert `SELECT z, y, count() FROM t GROUP BY z, y ORDER BY z` → `a 2 N` / `b 2 N`. RED evidence: `z` reads `''` for the merged rows. The retained holder is declared beside `prepared` (before `background_lock`, in the scope that ends before `waitForMutation`) so every exit releases it without the mutex held.
- [ ] **Step 3: implement** the candidate watermark if not already in Task 3, and the retained holder in `alter`. Build, restart, GREEN for 05298, 05299 and the previous set (05291, 05292, 05294, 05296, 05227, 04309, 04312). Commits: "Ignore parts created after the reservation snapshot when selecting a merge" (if Task 3 did not) and "Keep the rename mutation's block reserved until the alter is published or rolled back".

### Task 5: The failure/kill pins

**Files:** tests `05295_pending_version_failure_cleanup.{sh,reference}` copied from the reference branch (arm 1: a mutation that throws after allocation releases its block, `OPTIMIZE FINAL` then succeeds; arm 2: `KILL MUTATION` of a registered mutation leaves a paused one intact).

- [ ] Copy 05295, run (expected GREEN as a pin on this design — no code change; if arm 1 shows `CANNOT_ASSIGN_OPTIMIZE`, a reservation leaked: report BLOCKED). Commit: "Test that a failed mutation releases its block number and that `KILL MUTATION` ignores an unregistered one".

### Task 6: `OPTIMIZE FINAL` retry (R4) and the recovery guard

**Files:** `src/Storages/StorageMergeTree.cpp`; tests `05297_optimize_final_retry_refreshes_metadata.{sh,reference}` (copied), `05293_barrier_alter_waits_for_pending_mutation.{sh,reference}` (copied, pin), 04309, 04312, 04516 (existing).

- [ ] Cherry-pick the code part of b9fbb41381a (predicate/collector rebuild per attempt: `git cherry-pick -n b9fbb41381a`, then `git checkout HEAD -- tests/` to drop its test files, keep only `StorageMergeTree.cpp`), then `git cherry-pick b637b72b894` (metadata re-read; resolve conflicts if any), copy 05297, run RED-then-GREEN evidence as on the reference branch (05297 is RED without b637b72b894: run it after the first cherry-pick, before the second).
- [ ] Recovery guard: in `alter`'s `!durable_rolled_back` branch change `if (held_entry)` to `if (held_entry && held_entry->commands)` with the two-line reason (moved-from entry has no commands; pre-existing null dereference). Copy 05293 (pin). Run 05293, 05297, 04309, 04312, 04516 + previous set. Commits: two (retry; guard).

---

### Task 7: Whole-branch gates and the PR text

- [ ] Grep gates (source and our commit messages; the two #119155 commits exempt), warning grep over all build logs, `ci/jobs/scripts/check_style/check_cpp.sh`, wider suites on the CI-config server (`mutation lightweight_update lightweight_delete optimize_final patch_part` + explicit branch tests), classify every red with quoted evidence, PR body from `.github/PULL_REQUEST_TEMPLATE.md` (built on #119155, supersedes it: keeps its scan idea, removes the settings guard, extends the boundary to mutations, the merge predicate, the merged-part version and patch materialization; Changelog: Bug Fix; entry; `Closes` #122083, #116686; `Related` #119155). Then the minors-closing task, the dual final review (Opus + codex gpt-6-astra high), one fix wave with failing-first tests for any Critical, one scoped re-review, the finishing menu.
