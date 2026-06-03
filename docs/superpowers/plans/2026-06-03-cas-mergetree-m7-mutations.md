---
description: 'M7 — support mutations, data-ALTER, and lightweight DELETE (patch parts, B5) on a content_addressed disk by lifting the supportsHardLinks gate and verifying the existing whole-part carry-forward path end-to-end.'
sidebar_label: 'CAS MergeTree M7 mutations'
sidebar_position: 10
slug: /superpowers/plans/cas-mergetree-m7
title: 'Content-Addressed MergeTree — M7 Plan (mutations, data-ALTER, patch parts)'
doc_type: 'guide'
---

# CAS MergeTree — M7: mutations, data-ALTER, patch parts (B5) {#m7}

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development` + the
> `clickhouse-praktika-tests` skill for stateless runs. Steps use `- [ ]`. Approach **C (hybrid)**:
> gtest the new/risky invariants, then the real stateless suite is the acceptance oracle.

**Goal:** `ALTER TABLE … UPDATE/DELETE`, `MATERIALIZE …`, data-`ALTER` (`MODIFY COLUMN` type), and
lightweight `DELETE` (native patch-part model, B5) work on a `content_addressed` disk.

**Architecture:** Lift `DiskObjectStorage::supportsHardLinks` for CA (audit: exactly two pure-gate
consumers, no path-choosing branch). Mutations already build the part through one whole-part
`MutateTask` transaction with `createHardLinkFrom` for unchanged columns (CA carry-forward by
reference = same blob, no re-upload) + `writeFile` for changed ones + one `commit`. Patch parts are
normal CA parts under the existing `refs/` namespace; the GC reachability sweep catches them
automatically.

**Tech Stack:** `ContentAddressed/` units, `MergeTreeData.cpp` gates, `MutateTask`, the
`gtest_content_addressed_metadata.cpp` harness (fixture `ContentAddressedMetaTest`), praktika job
`Stateless tests (arm_binary, content_addressed storage, parallel)`.

**Source spec:** `docs/superpowers/specs/2026-06-03-cas-mergetree-mutations-design.md`.

## Build & test {#build}
- Build: `cmake --build build --target clickhouse unit_tests_dbms > build/cas_m7_build.log 2>&1`
  (redirect to log; a subagent summarizes the log). No `-j`/`nproc`.
- gtests: `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`.
- Stateless: via the `clickhouse-praktika-tests` skill, **foreground** praktika (no poll loops),
  binary symlinked at `ci/tmp/clickhouse`. **Never `clickhouse local`** (B48 hang).
- Conventions: Allman braces; `DB::Exception` + `ErrorCodes`; no `<...>` in `///`; full-path includes.

---

## Phase A — gate-lift + heavy-mutation carry-forward {#phase-a}

### Task 1: gtest the mutation carry-forward invariant (TDD, new/risky bit) {#task-1}

**Files:**
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp` (append a `TEST_F`)

The carry-forward primitive (`ContentAddressedTransaction::createHardLink`) exists but is not yet
pinned by a mutation-shaped gtest. Mirror `WritePartThenReadBackAndDedup` (`:452`) for the build
helper and `IdenticalContentDifferentMutableFilesDoNotCollide` (`:740`) for the
`PartManifest::deserialize(readObject(os, partKey("", pid).string()))` manifest assertions.

- [ ] **Step 1: Write the failing test** — `MutationCarryForwardReusesUnchangedBlobs`.

```cpp
// A mutation rewrites ONE column and carries the rest forward by reference: the new part's
// unchanged-column blobs MUST be the SAME blob objects as the source (no re-upload); only the
// changed column is a fresh blob. This is the content-addressing sweet spot the mutation path relies
// on (MutateTask: createHardLinkFrom unchanged + writeFile changed, one commit).
TEST_F(ContentAddressedMetaTest, MutationCarryForwardReusesUnchangedBlobs)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_mut_cf");
    auto os = getObjectStorage("cas_mut_cf");
    const std::string uuid = "uuid-mut";
    const std::string src = "all_1_1_0";
    const std::string dst = "all_1_1_0_2"; // mutation version 2

    // source part: two columns
    {
        DB::ContentAddressedTransaction tx(*ms, "", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string,std::string>{
                 {"a.bin","AAA"}, {"b.bin","BBB"}, {"columns.txt","a b"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + src + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    auto src_a = ms->getStorageObjects("uui/" + uuid + "/" + src + "/a.bin")[0].remote_path;
    auto src_b = ms->getStorageObjects("uui/" + uuid + "/" + src + "/b.bin")[0].remote_path;

    // mutation: carry a.bin + columns.txt forward by reference, rewrite b.bin with new content
    {
        DB::ContentAddressedTransaction tx(*ms, "", kCasTestScratch);
        tx.createHardLinkFrom("uui/" + uuid + "/" + src + "/a.bin",       "uui/" + uuid + "/" + dst + "/a.bin");
        tx.createHardLinkFrom("uui/" + uuid + "/" + src + "/columns.txt", "uui/" + uuid + "/" + dst + "/columns.txt");
        auto buf = tx.writeFile("uui/" + uuid + "/" + dst + "/b.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string nb = "NEWB";
        buf->write(nb.data(), nb.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }

    // (a) unchanged columns carried forward -> SAME blob objects (no re-upload)
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/" + dst + "/a.bin")[0].remote_path, src_a);
    // (b) changed column -> a DIFFERENT, fresh blob; content is the new bytes
    auto dst_b = ms->getStorageObjects("uui/" + uuid + "/" + dst + "/b.bin")[0].remote_path;
    EXPECT_NE(dst_b, src_b);
    EXPECT_EQ(readObject(os, dst_b), "NEWB");
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/" + dst + "/a.bin")[0].remote_path), "AAA");
    // (c) the source part is untouched (its b.bin still resolves to BBB)
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/" + src + "/b.bin")[0].remote_path), "BBB");
}
```

- [ ] **Step 2: Run to verify it passes** (the primitive already exists; this PINS it).
  Run: `build/src/unit_tests_dbms --gtest_filter='*MutationCarryForward*'`.
  Expected: PASS. If it FAILS, the carry-forward classification in `createHardLink` is wrong for the
  `uui/<uuid>/<part>/<file>` grammar — fix `ContentAddressedTransaction::createHardLink` (it must
  resolve the source file's blob hash from the source ref/manifest and record the SAME hash for the
  destination), then re-run.
- [ ] **Step 3: Commit** `CAS M7: gtest pins mutation carry-forward (unchanged columns reuse blobs)`.

### Task 2: lift the supportsHardLinks gate for CA + pin partition-clone stays gated {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:719` (the `supportsHardLinks` body)
- Test: `tests/queries/0_stateless/` (new `.sh` via `add-test`, see Step 3)

- [ ] **Step 1: Flip the flag for CA.** In `DiskObjectStorage::supportsHardLinks`, return `true` when
  the metadata storage is content-addressed (the same `isContentAddressed()`/`MetadataStorageType`
  predicate used elsewhere in this file). Read the current body first; it currently returns `false`
  unconditionally for CA. Keep non-CA behavior unchanged. Add a comment: carry-forward hard-links are
  routed through the whole-part `ContentAddressedTransaction` (mutations/data-ALTER), not per-file
  autocommit; partition-clone/BACKUP/replication remain gated by their own independent checks.
- [ ] **Step 2: Build.** `cmake --build build --target clickhouse > build/cas_m7_build.log 2>&1`
  (subagent summarizes). Expected: clean.
- [ ] **Step 3: Regression test — partition-clone still rejects on CA.** Create via
  `./tests/queries/0_stateless/add-test 04xxx_content_addressed_partition_clone_still_gated.sh`.
  The test creates a CA-disk table (use the inline CA-disk pattern from the existing
  `04290_content_addressed_no_leftovers.sh`), inserts, then asserts `ALTER TABLE … MOVE PARTITION …
  TO TABLE …` (and `REPLACE PARTITION`) **fail** with `SUPPORT_IS_DISABLED` (grep the error), proving
  the flag flip did not open the clone path. Reference the gate at `checkAlterPartitionIsPossible`.
  Run in praktika (foreground); expect `[ OK ]`.
- [ ] **Step 4: Commit** `CAS M7: supportsHardLinks=true for CA (un-gate mutations/data-ALTER) + partition-clone regression`.

### Task 3: heavy mutation end-to-end + dedicated correctness oracle {#task-3}

**Files:**
- Test: `tests/queries/0_stateless/04xxx_content_addressed_mutations.sh` (via `add-test`)

- [ ] **Step 1: Smoke `ALTER UPDATE`/`DELETE` on a CA table** (manual, via praktika or the local
  binary): create a CA-disk MergeTree table, INSERT rows, run `ALTER TABLE … UPDATE col = …` and
  `ALTER TABLE … DELETE WHERE …` with `mutations_sync=2`, `SELECT` to confirm the effect. Confirm no
  exception. If it throws, triage (most likely a still-active gate or a sidecar path) and fix.
- [ ] **Step 2: Write the oracle test** — a CA-disk table and a plain MergeTree table get the SAME
  data + the SAME mutations; assert identical `SELECT` results (count, sums, ORDER BY dump). Cover:
  `ALTER UPDATE` one column (carry-forward of others), `ALTER DELETE WHERE`, `MODIFY COLUMN` type
  change, `MATERIALIZE COLUMN`. Use `mutations_sync=2`. Mirror the CA-disk DDL from
  `04290_content_addressed_no_leftovers.sh`.
- [ ] **Step 3: Run** in praktika (foreground), subagent summarizes the log. Expected `[ OK ]`.
- [ ] **Step 4: Commit** `CAS M7: heavy-mutation correctness oracle (CA vs plain MergeTree)`.

---

## Phase B — patch parts (B5, lightweight DELETE) {#phase-b}

### Task 4: gtest patch-part reachability + reclaim invariant (TDD) {#task-4}

**Files:**
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp` (append a `TEST_F`)

A patch part is a normal CA part. The new invariant to pin: its blobs are reachable while its ref
lives, and reclaimed after the ref unlinks — the GC sweep needs no patch-specific handling. Mirror
the existing GC/reachability gtest (search the file for `Reachab`/`listLivePartIds`/`runSweepOnce`
to find the established pattern and helpers) and `PartRemovalIsIdempotent` (`:547`) for the
remove-then-assert shape.

- [ ] **Step 1: Write the test** — `PatchPartIsReachableThenReclaimedLikeAnyPart`. Build a part whose
  name is patch-shaped (e.g. `patch-all_1_1_0` — confirm the actual patch part-name grammar by
  grepping `MergeTreePartInfo`/`getPartNameForLogs` for the patch prefix; use the real prefix) under
  the table's `refs/`. Assert: (a) after commit, the reachability set (the same call the sweep uses —
  mirror the existing GC test) contains the patch part's `part_id`, so a sweep would NOT delete its
  blobs; (b) after `removeRecursive` of the patch ref, the blobs are unreachable and a sweep reclaims
  them. If the patch part-name grammar makes the ref enumeration miss it, that is the bug to fix in
  `listLivePartIds`/the refs listing.
- [ ] **Step 2: Run** `build/src/unit_tests_dbms --gtest_filter='*PatchPart*'`. Iterate to PASS.
- [ ] **Step 3: Commit** `CAS M7: gtest patch-part reachability + reclaim (B5 GC invariant)`.

### Task 5: lightweight DELETE end-to-end + oracle {#task-5}

**Files:**
- Test: `tests/queries/0_stateless/04xxx_content_addressed_lightweight_delete.sh` (via `add-test`)

- [ ] **Step 1: Smoke lightweight `DELETE`** on a CA table: `CREATE … ENGINE=MergeTree` on the CA
  disk, INSERT, `DELETE FROM … WHERE …` (default `lightweight_delete_mode`), `SELECT` to confirm the
  rows are gone. Confirm a patch part (or row-level delete) is produced without exception. Triage any
  failure.
- [ ] **Step 2: Oracle test** — CA-disk table vs plain table, identical data + identical lightweight
  `DELETE`s (single and multiple, overlapping predicates); assert identical `SELECT` results.
  Include a subsequent merge (`OPTIMIZE TABLE … FINAL`) so the patch is applied during merge, then
  re-assert. Mirror the CA-disk DDL from `04290`.
- [ ] **Step 3: Run** in praktika (foreground); subagent summarizes. Expected `[ OK ]`.
- [ ] **Step 4: Commit** `CAS M7: lightweight DELETE (patch parts) correctness oracle`.

### Task 6: patch-part / mutated-part survive a restart {#task-6}

**Files:**
- Test: `tests/integration/test_content_addressed_s3/test.py` (append a test) OR a stateless
  restart-shaped check if integration restart is heavier than needed — prefer extending the existing
  integration test since it already has an S3-backed CA server with restart capability (grep the file
  for `restart_clickhouse`/`restart`).

- [ ] **Step 1: Write the test** — on the S3-backed CA server: create table, INSERT, lightweight
  `DELETE` (produces a patch part) + an `ALTER UPDATE` (produces a mutated part), `restart_clickhouse`,
  then `SELECT` and assert the post-mutation/post-delete results survive (the active set is
  rediscovered from `refs/`). This pins §5 (discovery) of the spec.
- [ ] **Step 2: Run** via `python -m ci.praktika run "integration" --test test_content_addressed_s3`
  (foreground; reference the `clickhouse-praktika-tests` skill / `reference_keeper_integration_tests`
  memory for the exact invocation). Expected: passed.
- [ ] **Step 3: Commit** `CAS M7: patch/mutated parts survive restart (active-set rediscovery)`.

---

## Phase C — real-suite oracle, un-tag, no-leftovers {#phase-c}

### Task 7: run the gated mutation/lightweight-DELETE stateless tests under CA-default {#task-7}

**Files:**
- Read/modify: the `no-content-addressed-storage`-tagged tests under
  `tests/queries/0_stateless/` (035*–049* range per the M6 tagging commit `3b175b49db5`)

- [ ] **Step 1: Enumerate** the stateless tests currently tagged `no-content-addressed-storage`
  *because of mutations / lightweight DELETE / data-ALTER* (grep the tag; cross-reference each test's
  body for `ALTER … UPDATE/DELETE`, `DELETE FROM`, `MODIFY COLUMN`). Produce the candidate list (these
  should now be runnable). Tests gated for OTHER reasons (projections, replication, BACKUP) stay
  tagged.
- [ ] **Step 2: Temporarily un-tag the candidates** and run them under CA-default via praktika
  (foreground, batched but in the foreground — no poll loops). Subagent summarizes each log.
- [ ] **Step 3: Triage** failures into (1) real CA bug → fix at the source (root-cause, not a test
  edit), (2) legitimately-unsupported (re-tag with a precise reason). Record the taxonomy + counts in
  the backlog (`docs/superpowers/deferred_backlog/cas-mergetree-integration.md`).
- [ ] **Step 4: Iterate** until the un-tagged candidates are green. Commit fixes individually with
  `CAS M7: fix <root cause> (test NNNNN)`.

### Task 8: no-leftovers after mutation + finalize un-tagging {#task-8}

**Files:**
- Test: `tests/queries/0_stateless/04xxx_content_addressed_mutation_no_leftovers.sh` (via `add-test`)

- [ ] **Step 1: No-leftovers oracle** — mirror `04290_content_addressed_no_leftovers.sh` exactly
  (inline CA disk over `object_storage_type=local`, absolute pool `path` under
  `CLICKHOUSE_USER_FILES_UNIQUE`, `gc_enabled=1`/`grace=2`/`interval=1`). Sequence: INSERT (pool
  rises) → `ALTER UPDATE` + lightweight `DELETE` (carry-forward; some new blobs; the superseded
  source parts become unreachable) → bounded-poll until the superseded blobs are reclaimed (the live
  mutated/patch parts' blobs remain) → `DROP TABLE … SYNC` → bounded-poll (60s cap) until `blobs/`
  and `parts/` are empty → assert `_pool_meta` survives.
- [ ] **Step 2: Run** in praktika (foreground); subagent summarizes. Expected `[ OK ]`.
- [ ] **Step 3: Finalize un-tagging** — keep the Task-7 un-taggings that now pass; ensure the
  remaining `no-content-addressed-storage` list is only genuinely-unsupported features, each with a
  one-line reason. Update the backlog (close B5 + the mutation parts of B30; note residual deferrals).
- [ ] **Step 4: Commit** `CAS M7: mutation/patch no-leftovers oracle + finalize un-tagging (B5 closed)`.

---

## Verify — HARD GATE {#verify}
- Build clean; `--gtest_filter='ContentAddressed*'` all green (incl. the two new invariant tests).
- New stateless oracles `[ OK ]`: mutation correctness, lightweight-DELETE correctness,
  partition-clone-still-gated, mutation no-leftovers.
- The un-tagged mutation/lightweight-DELETE stateless tests pass under CA-default.
- Integration restart test passed.
- Final fresh-subagent + codex external review reports no BLOCKER/CRITICAL.

## Self-review {#self-review}
- **Coverage:** spec §1 gate-lift (Task 2), §2 heavy mutations (Tasks 1,3), §3 data-ALTER (Task 3
  MODIFY COLUMN), §4 patch parts/B5 (Tasks 4,5), §5 discovery (Task 6), §6 edge cases (Task 3 all-cols
  / Task 8 superseded-reclaim / cancelled-mutation covered by no-ref-published → reclaim), §7
  acceptance (Tasks 7,8).
- **Safety:** the flag flip (Task 2) is pinned by the partition-clone regression; carry-forward
  correctness is pinned by Task 1; no on-disk format change.
- **Method:** hybrid — new invariants white-boxed (Tasks 1,4), the bulk proven by the real suite
  (Task 7), no-leftovers proven empirically (Task 8). Matches M6's data-driven acceptance.

## Deferrals likely to surface {#deferrals}
- `lightweight_mutation_projection` mode (needs projections — stays gated; note in backlog).
- If a mutation triggers a projection rebuild on a table that somehow has projections (should be
  blocked at CREATE) — verify the projection gate still fires; if not, add a gate.
- Cross-arch determinism of mutated `part_id`s — the LE formats (M5.4) already cover this; confirm a
  mutated part read on the other arch in CI.
