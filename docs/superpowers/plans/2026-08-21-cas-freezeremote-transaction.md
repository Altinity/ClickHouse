---
description: 'Implementation plan for giving freezeRemote a content-addressed single-transaction branch so cross-disk ATTACH PARTITION FROM works'
sidebar_label: 'CAS freezeRemote transaction'
sidebar_position: 1
slug: /superpowers/plans/cas-freezeremote-transaction
title: 'freezeRemote single-transaction branch — implementation plan'
doc_type: 'guide'
---

# `freezeRemote` single-transaction branch — Implementation Plan {#cas-freezeremote-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cross-disk `ALTER TABLE dst ATTACH PARTITION … FROM src` work when `dst` is on a content-addressed disk, by cloning the part through one CAS transaction instead of one autocommit per file.

**Architecture:** `freezeRemote` is the cross-disk clone path and the only one of the three without a CAS branch. It gains the same shape the fork already uses in `freeze`: when the caller supplied no transaction and the destination is content-addressed, self-create one, stream every file into it, route the post-clone removals through it, and commit once. The existing `Backup` path is untouched for every other destination.

**Tech Stack:** C++ (ClickHouse fork), stateless `.sh` test, per-query `ProfileEvents` read from `system.query_log`.

**Spec:** `docs/superpowers/cas/BACKLOG.md` `{#issue-2173-freezeremote-gap}` — CONFIRMED and REPRODUCED on HEAD. **Read the spec-delta section below before the tasks:** the spec's fix shape is right in outline and incomplete in three ways, and it cannot be corrected in place right now.

## Spec delta — what this plan adds to the spec, and why the spec was not edited {#spec-delta}

`docs/superpowers/cas/BACKLOG.md` is being actively rewritten by another session as this plan is written. Editing a file another agent is mid-write on risks losing one of the two edits, so the corrections live here and Task 3 moves them once that file is free. Treat this section as authoritative where it disagrees with the spec.

1. **The three post-clone `removeFileIfExists` calls must route through the self-created transaction.** The spec mentions only that the `metadata_version.txt` / `txn_version.txt` writes "become legal single repoints". It does not say that the removals, which today go straight to the disk, would autocommit — introducing one transaction and immediately breaking it with three separate publishes against the same ref.
2. **`freezeRemote`'s `external_transaction` branches must NOT be removed.** Verified: the whole function is byte-identical to `$(git merge-base altinity/antalya-26.6 HEAD)` — untouched upstream code. Those branches are dead *in this fork only*, because the single tree-wide assignment to `ClonePartParams::external_transaction` reaches `freeze`, not `freezeRemote`. Removing them would change shared upstream behaviour for no benefit to this fix, and the next upstream merge would meet the removal. A simplification along those lines was proposed during design and withdrawn for this reason.
3. **The helper this fix needs is declared *after* the function that must call it.** `copyDirectoryContentIntoTransaction` lives in an anonymous namespace between `freezeRemote` and `clonePart`, so `clonePart` can call it and `freezeRemote` cannot. The namespace contains that one helper and nothing else, so moving the block above `freezeRemote` is the minimal fix; a forward declaration would work too but leaves the reader hunting.

Two further facts the spec does not state, both verified, both load-bearing for the test:

- The path selector is `on_same_disk`: same disk goes to `freeze` (which already has the CAS branch), different disk goes to `freezeRemote`. It is not `copy_instead_of_hardlink`. A test whose two tables share a disk exercises the wrong function entirely.
- **CAS→CAS in one pool needs no extra production code.** The destination's publish resolves each blob through the dedup gate, so the clone is effectively a ref repoint. The ref-collision unknown tracked for `MOVE` (`[VERIFY-ca-ca-same-pool-move]`) does not transfer here: `ATTACH` writes into the *destination table's* namespace under a temporary part name, so there is no shared ref to collide on.

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`. No rebase, no amend — add new commits.
- **THE WORKTREE IS SHARED AND BUSY.** Another session is executing a different plan in this same checkout: it holds uncommitted edits under `src/Disks/tests/` and is rewriting `docs/superpowers/cas/BACKLOG.md`. Before staging anything, re-run `git status --short <the exact paths>` and stage only paths whose diff is yours. Never `git add -A`, `git add .`, or `git commit -a`. **Never stage `docs/superpowers/cas/BACKLOG.md`.**
- `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp` is upstream code. Change only what this fix needs; do not tidy, reorder, or delete anything else in it.
- No `LOGICAL_ERROR`. This change adds no new failure mode; the CAS branch either commits or undoes and rethrows.
- Allman braces (opening brace on its own line) — enforced by the style check.
- Comments must not cite this plan, the BACKLOG, a task number, or an issue number. Keep the reason, drop the provenance.
- The test number `05025_cas_attach_partition_cross_disk` is already reserved by `add-test`; both files exist as untracked stubs. Do not renumber, do not re-run `add-test`, do not `chmod`.

---

## Task 1: Pin the failure with a cross-disk attach test {#task-1}

**Files:**
- Modify: `tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.sh` (stub from `add-test`)
- Modify: `tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.reference` (empty from `add-test`)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: the test Task 2 must turn green.

Three legs. The second and third are not decoration: leg 2 covers the case that needs no code and would otherwise never be exercised, and its counter assertion is the only thing that distinguishes "it worked" from "it worked by re-uploading everything".

- [ ] **Step 1: Write the failing test**

```bash
#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# `ATTACH PARTITION FROM` across disks clones the part through `freezeRemote`, and a
# content-addressed destination models a part as ONE atomic unit: N files, one manifest, one ref.
# Without a single transaction every file autocommits as its own one-file manifest against the same
# ref, so two of them resolve the ref as absent and the loser hits the unique-ref guard -- the very
# first attach fails.
#
# The two tables must be on DIFFERENT disks: the clone path is chosen by `on_same_disk`, and a
# same-disk attach goes through `freeze`, which already has the transaction branch.
#
# Leg 2 is the same-pool content-addressed case. It needs no extra production code -- the
# destination's publish resolves each blob through the dedup gate, so the clone is effectively a ref
# repoint -- and that claim is asserted rather than assumed. The counter is read PER QUERY from
# `system.query_log`: `system.events` is process-wide and stateless tests run in parallel, so a
# global read would be measuring someone else's traffic.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS src_plain;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS dst_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS src_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS dst_cas_same_pool;"

# ---------------------------------------------------------------- leg 1: local -> content-addressed

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE src_plain (k UInt32, v String)
ENGINE = MergeTree ORDER BY k PARTITION BY k
SETTINGS disk = disk(type = local, name = '05025_plain', path = '05025_plain/');"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE dst_cas (k UInt32, v String)
ENGINE = MergeTree ORDER BY k PARTITION BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05025_dst',
    name = '05025_cas_dst',
    path = '05025_cas_dst_pool/');"

${CLICKHOUSE_CLIENT} --query "INSERT INTO src_plain SELECT number % 2, toString(number) FROM numbers(64);"

${CLICKHOUSE_CLIENT} --query "ALTER TABLE dst_cas ATTACH PARTITION 1 FROM src_plain;"

# Completeness, not mere presence: a partially published part would satisfy a bare count().
${CLICKHOUSE_CLIENT} --query "SELECT 'leg1', count(), sum(k), uniqExact(v) FROM dst_cas;"

# A detach/attach round trip reads the part back from its manifest rather than from whatever the
# writing session still had warm.
${CLICKHOUSE_CLIENT} --query "ALTER TABLE dst_cas DETACH PARTITION 1;"
${CLICKHOUSE_CLIENT} --query "ALTER TABLE dst_cas ATTACH PARTITION 1;"
${CLICKHOUSE_CLIENT} --query "SELECT 'leg1_roundtrip', count(), sum(k) FROM dst_cas;"

# ------------------------------------------- leg 2: content-addressed -> content-addressed, one pool

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE src_cas (k UInt32, v String)
ENGINE = MergeTree ORDER BY k PARTITION BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05025_shared_a',
    name = '05025_cas_shared_a',
    path = '05025_cas_shared_pool/');"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE dst_cas_same_pool (k UInt32, v String)
ENGINE = MergeTree ORDER BY k PARTITION BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05025_shared_b',
    name = '05025_cas_shared_b',
    path = '05025_cas_shared_pool/');"

${CLICKHOUSE_CLIENT} --query "INSERT INTO src_cas SELECT number % 2, toString(number) FROM numbers(64);"

ATTACH_QUERY_ID="05025_attach_same_pool_${CLICKHOUSE_DATABASE}"
${CLICKHOUSE_CLIENT} --query_id "$ATTACH_QUERY_ID" \
    --query "ALTER TABLE dst_cas_same_pool ATTACH PARTITION 1 FROM src_cas;"

${CLICKHOUSE_CLIENT} --query "SELECT 'leg2', count(), sum(k), uniqExact(v) FROM dst_cas_same_pool;"

# The content is already in the pool, so the publish must have adopted the existing blobs rather
# than uploading their bodies again.
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS;"
${CLICKHOUSE_CLIENT} --query "
SELECT 'leg2_reused_blobs', ProfileEvents['CASBlobBodyPutAvoided'] > 0
FROM system.query_log
WHERE current_database = currentDatabase() AND query_id = '${ATTACH_QUERY_ID}' AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC LIMIT 1;"

${CLICKHOUSE_CLIENT} --query "DROP TABLE src_plain;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE dst_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE src_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE dst_cas_same_pool;"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

Reference file:

```
leg1	32	32	32
leg1_roundtrip	32	32
leg2	32	32	32
leg2_reused_blobs	1
dropped_ok
```

- [ ] **Step 2: Run it and confirm WHERE it fails**

Run: `./tests/clickhouse-test 05025_cas_attach_partition_cross_disk`

Expected: leg 1's `ATTACH PARTITION 1 FROM src_plain` throws. The reported error is one of two, and which one depends on which file the pool reaches first — both are the same defect: a unique-ref refusal from the promote, or `NOT_IMPLEMENTED` about autocommit writes not being supported for content part files. The single production fact responsible is that `freezeRemote` has no content-addressed branch, so `Backup` fans the part's files onto a thread pool and each becomes its own autocommit transaction against one ref.

**Do not proceed to Task 2 until you have seen that.** Two other outcomes mean the test is wrong rather than the bug pinned, and each is fixed here:

- The attach *succeeds*. Then the two tables are not on different disks and the clone went through `freeze`. Check the disk names actually differ and that the destination policy reserves the CAS disk.
- Leg 2's counter row is empty rather than `0`/`1`. Then the `query_id`, the `current_database` filter, or `SYSTEM FLUSH LOGS` placement is wrong, not the dedup claim.

- [ ] **Step 3: Commit**

Re-check dirtiness first — this checkout is shared:

```bash
git status --short tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.sh \
                   tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.reference
git add tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.sh \
        tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.reference
git commit -m 'Pin the cross-disk attach failure into a content-addressed disk'
```

---

## Task 2: Give `freezeRemote` the content-addressed branch {#task-2}

**Files:**
- Modify: `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp` (move the helper's anonymous namespace above `freezeRemote`; add the CAS branch inside `freezeRemote`)

**Interfaces:**
- Consumes: the test from Task 1.
- Produces: no new public signature. `freezeRemote`'s declaration in `DataPartStorageOnDiskBase.h` does not change.

- [ ] **Step 1: Move the helper above `freezeRemote`**

The anonymous namespace holding `copyDirectoryContentIntoTransaction` currently sits between `freezeRemote` and `clonePart`. It contains that one helper and nothing else. Move the whole `namespace { … }` block — its doc comment included — so it appears *before* `MutableDataPartStoragePtr DataPartStorageOnDiskBase::freezeRemote`. Change no character inside it; `clonePart` keeps calling it unchanged.

- [ ] **Step 2: Add the branch**

In `freezeRemote`, replace this:

```cpp
    auto src_disk = volume->getDisk();
    if (params.external_transaction)
        params.external_transaction->createDirectories(to);
    else
        dst_disk->createDirectories(to);

    /// freezeRemote() using copy instead of hardlinks for all files
    /// In this case, files_to_copy_intead_of_hardlinks is set by empty
    Backup(
        src_disk,
        dst_disk,
        getRelativePath(),
        fs::path(to) / dir_path,
        read_settings,
        write_settings,
        params.make_source_readonly,
        /* max_level= */ {},
        true,
        /* files_to_copy_intead_of_hardlinks= */ {},
        params.external_transaction);
```

with this:

```cpp
    auto src_disk = volume->getDisk();

    /// A content-addressed destination models a part as ONE atomic unit: N files become one manifest
    /// and one ref. The generic path below fans the files onto a thread pool, and each becomes an
    /// independent autocommit transaction against that same ref -- two of them resolve it as absent,
    /// both publish a one-file manifest, and the loser is refused. So when the caller supplied no
    /// transaction of its own, run the whole clone through ONE self-created transaction, the same
    /// shape `freeze` uses. `Backup` cannot serve this path: its transactional branch calls
    /// `copyFile` on the transaction, which is same-disk only and refuses a cross-disk
    /// content-addressed copy.
    DiskTransactionPtr owned_transaction;
    if (!params.external_transaction && dst_disk->isContentAddressed())
        owned_transaction = dst_disk->createTransaction();

    if (owned_transaction)
    {
        try
        {
            copyDirectoryContentIntoTransaction(
                *src_disk, getRelativePath(), *owned_transaction, fs::path(to) / dir_path,
                read_settings, write_settings, /* cancellation_hook= */ {});
        }
        catch (...)
        {
            owned_transaction->undo();
            throw;
        }
    }
    else
    {
        if (params.external_transaction)
            params.external_transaction->createDirectories(to);
        else
            dst_disk->createDirectories(to);

        /// freezeRemote() using copy instead of hardlinks for all files
        /// In this case, files_to_copy_intead_of_hardlinks is set by empty
        Backup(
            src_disk,
            dst_disk,
            getRelativePath(),
            fs::path(to) / dir_path,
            read_settings,
            write_settings,
            params.make_source_readonly,
            /* max_level= */ {},
            true,
            /* files_to_copy_intead_of_hardlinks= */ {},
            params.external_transaction);
    }
```

`copyDirectoryContentIntoTransaction` creates the destination directory itself, which is why the CAS arm has no `createDirectories` call. The empty cancellation hook is safe: `copyData` tests the callable before invoking it.

- [ ] **Step 3: Route the post-clone removals through the same transaction, and commit it**

Replace this:

```cpp
    if (params.external_transaction)
    {
        params.external_transaction->removeFileIfExists(fs::path(to) / dir_path / "delete-on-destroy.txt");
        params.external_transaction->removeFileIfExists(fs::path(to) / dir_path / VersionMetadata::TXN_VERSION_METADATA_FILE_NAME);
        if (!params.keep_metadata_version)
            params.external_transaction->removeFileIfExists(fs::path(to) / dir_path / IMergeTreeDataPart::METADATA_VERSION_FILE_NAME);
    }
    else
    {
        dst_disk->removeFileIfExists(fs::path(to) / dir_path / "delete-on-destroy.txt");
        dst_disk->removeFileIfExists(fs::path(to) / dir_path / VersionMetadata::TXN_VERSION_METADATA_FILE_NAME);
        if (!params.keep_metadata_version)
            dst_disk->removeFileIfExists(fs::path(to) / dir_path / IMergeTreeDataPart::METADATA_VERSION_FILE_NAME);
    }
```

with this:

```cpp
    /// These removals belong to the clone. On the content-addressed arm they MUST go through the same
    /// transaction: sent straight to the disk they would autocommit, which is exactly the
    /// one-publish-per-file behaviour the single transaction above exists to prevent.
    if (const DiskTransactionPtr & clone_transaction = owned_transaction ? owned_transaction : params.external_transaction)
    {
        try
        {
            clone_transaction->removeFileIfExists(fs::path(to) / dir_path / "delete-on-destroy.txt");
            clone_transaction->removeFileIfExists(fs::path(to) / dir_path / VersionMetadata::TXN_VERSION_METADATA_FILE_NAME);
            if (!params.keep_metadata_version)
                clone_transaction->removeFileIfExists(fs::path(to) / dir_path / IMergeTreeDataPart::METADATA_VERSION_FILE_NAME);
            if (owned_transaction)
                owned_transaction->commit();
        }
        catch (...)
        {
            if (owned_transaction)
                owned_transaction->undo();
            throw;
        }
    }
    else
    {
        dst_disk->removeFileIfExists(fs::path(to) / dir_path / "delete-on-destroy.txt");
        dst_disk->removeFileIfExists(fs::path(to) / dir_path / VersionMetadata::TXN_VERSION_METADATA_FILE_NAME);
        if (!params.keep_metadata_version)
            dst_disk->removeFileIfExists(fs::path(to) / dir_path / IMergeTreeDataPart::METADATA_VERSION_FILE_NAME);
    }
```

The external-transaction case keeps its exact previous behaviour: the removals go onto the caller's transaction and this function does not commit it. Only the self-created one is committed here, because only this function owns it.

Leave `save_metadata_callback` and the `create(...)` tail exactly as they are. The callback is empty at the only call site and is upstream signature shape.

- [ ] **Step 4: Correct the helper's own justification, which this change outgrows**

`copyDirectoryContentIntoTransaction`'s doc comment justifies copying sequentially rather than through the parallel thread pool by saying "MOVE is a background, latency-insensitive operation, so parallelizing this is a deferred optimization, not a correctness requirement". After this task the same helper also serves `ATTACH PARTITION FROM`, which is a foreground statement a user waits on — so the justification no longer covers all of its callers.

Keep the correctness half untouched (a content-addressed transaction batches every file into one manifest and its staging map is not mutex-guarded — that is why the copy is sequential). Replace only the latency clause: say that the callers are a background move and a user-issued cross-disk attach, and that parallelising it remains a deferred optimisation whose cost is now visible to a waiting statement rather than only to a background one. Do not parallelise anything here — that is a separate change with its own correctness argument to make about the staging map.

- [ ] **Step 5: Build and run**

```bash
ninja -C build clickhouse
./tests/clickhouse-test 05025_cas_attach_partition_cross_disk
./tests/clickhouse-test 05003_cas_freeze 05024_cas_freeze_two_roots
```

Expected: the new test passes all three legs; the two freeze tests are unaffected — they exercise `freeze`, not `freezeRemote`, and this change touches neither `freeze` nor the helper's contents. Also run the `CAS*:Cas*:CA*` gtest gate: it should be unchanged, since no CAS-internal signature moved.

- [ ] **Step 6: Commit**

```bash
git status --short src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
git add src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
git commit -m 'Clone a part into a content-addressed disk in one transaction on the cross-disk path'
```

---

## Task 3: Move the spec delta into the spec {#task-3}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG.md`, section `{#issue-2173-freezeremote-gap}` — **only if it is clean**

**Interfaces:**
- Consumes: Tasks 1 and 2 landed.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Check whether the file is yours to touch**

```bash
git status --short docs/superpowers/cas/BACKLOG.md
```

If it reports anything, another session is still working in it. **Stop and report that**; do not edit it, do not stage it, and do not work around it by putting the content somewhere else. The plan's spec-delta section stays authoritative until someone can land it properly.

- [ ] **Step 2: If clean, flip the entry to FIXED and fold in the corrections**

The `{#issue-2212-shadow-namespace}` entry shows the house format for a fixed issue: the heading changes from `CONFIRMED` to `FIXED`, the mechanism paragraph moves to the past tense, and `FIX (SCHEDULED: …)` becomes `FIX (IMPLEMENTED <date>: <commit hashes>)`. Do the same here. The current heading — "`freezeRemote` lacks the CAS single-transaction branch" — is itself the stalest sentence in the entry once Task 2 lands.

Fold in the four corrections, in the entry's own words rather than copied from this plan:

1. the three post-clone `removeFileIfExists` calls go through the self-created transaction — with the reason, that sending them to the disk would autocommit and reintroduce the very defect;
2. `freezeRemote`'s `external_transaction` branches are untouched upstream code, dead in this fork only, and deliberately left alone;
3. the helper had to move above `freezeRemote` because an anonymous namespace declared it after the caller;
4. the clone path is selected by `on_same_disk`, so a test whose tables share a disk exercises `freeze` instead.

Two more edits inside the same entry:

- Record that CAS→CAS same-pool needed no production code, and that the test now asserts the blobs were reused rather than assuming it.
- The entry's closing sentence reads "The unfixed member of the family is exactly this `freezeRemote` gap." That family now has no unfixed member; say so, because the sentence is the one a reader would trust to know what is left.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/cas/BACKLOG.md
git commit -m 'Record what the freezeRemote fix needed beyond the adjudicated shape'
```

---

## Task 4: The rest of the documentation {#task-4}

**Files:**
- Modify: `docs/superpowers/cas/final-checks-todo.md` (item 1)
- Modify: `docs/superpowers/cas/BACKLOG/replication.md` (`[VERIFY-ca-ca-same-pool-move]`)
- Modify: `docs/superpowers/cas/BACKLOG/formats-and-storage.md` (the re-freeze-same-part-name item)

**Interfaces:**
- Consumes: Tasks 1 and 2 landed.
- Produces: nothing later tasks depend on.

Documentation-only; no build. Each file gets its own dirtiness check before staging, for the reason in the Global Constraints.

- [ ] **Step 1: Close the scheduling entry**

`final-checks-todo.md` item 1 is the pre-release schedule line for this fix and still reads as pending. Item 2 in the same file was closed by another session for issue #2212 and shows the format: the heading gains `DONE —` and the body names the commits. Do the same for item 1.

- [ ] **Step 2: Record what the new test does and does NOT answer for the same-pool question**

`BACKLOG/replication.md`'s `[VERIFY-ca-ca-same-pool-move]` asks whether a same-pool CA↔CA clone works and whether the target's ref `<part>` collides with the source's. The new test's second leg answers the first half for `ATTACH`: the publish dedup-resolves the blobs and the clone is a ref repoint, now asserted by a counter.

**Do not close the item.** Its collision question is about `MOVE`, where source and target are the SAME table — one namespace, one ref name `<part>`. `ATTACH` writes into the destination table's namespace under a temporary part name, so it cannot collide and therefore proves nothing about the case the item is actually asking about. Add that distinction to the item so the next reader does not close it on the strength of this test.

- [ ] **Step 3: Record that this fix widens another item's reachability**

`BACKLOG/formats-and-storage.md`'s re-freeze item says that re-freezing a DIFFERENT part carrying the same part name — "after `REPLACE PARTITION` / `ATTACH PARTITION FROM`, which can reuse `all_1_1_0`" — silently produces one frozen ref mixing two snapshots. Until now the cross-disk route to that state was closed by the very bug this plan fixes. Note on the item that cross-disk `ATTACH PARTITION FROM` into a content-addressed disk now works, so that route is open and the item's priority should be reconsidered rather than inherited.

This is the kind of consequence a fix does not usually announce about itself: nothing in the fix is wrong, and another item got more reachable because of it.

- [ ] **Step 4: Commit**

```bash
git status --short docs/superpowers/cas/final-checks-todo.md                    docs/superpowers/cas/BACKLOG/replication.md                    docs/superpowers/cas/BACKLOG/formats-and-storage.md
git add <only the paths whose diff is yours>
git commit -m 'Record the cross-disk clone fix across the tracking documents'
```

## Documentation checked and deliberately NOT changed {#docs-not-changed}

Recorded so the executing agent does not re-derive it, and so a later reader knows the sweep happened:

- **`docs/en/antalya/cas/`** — nothing stale. The user-facing roadmap's `{#known-limitations}` list never mentioned cross-disk `ATTACH PARTITION FROM`, so there is no limitation to retract; and this was a bug rather than an unshipped feature, so there is nothing to announce either. Checked every page for `ATTACH PARTITION`, "not supported" and "limitation".
- **`BACKLOG/operability-and-introspection.md`** — its list of paths reaching `freeze` includes "Same-disk `ATTACH/REPLACE PARTITION FROM`". That stays true: same-disk still routes to `freeze`, and this plan changes only the cross-disk path. Left alone on purpose.
- **`BACKLOG/replication.md` `[move-part-to-ca-architecturally-unimplemented]`** — already marked CLOSED at HEAD and kept for provenance. No edit needed there; the stale sentence about the family's remaining unfixed member lives in the `BACKLOG.md` entry and is handled in Task 3.
- **`BACKLOG/replication.md` `[RPL-5 slice]`** — the replicated queue-clone relink test gap. This plan touches the local cross-disk path, not the queue path, so the gap is unchanged.
- **`clonePart`'s L2 comment** — says it mirrors `freeze`'s `owned_transaction` shape. Still true after this change; a third mirror does not make it false.

## Self-review {#self-review}

**Spec coverage.** The spec's fix shape — self-create a transaction, stream through the existing helper, commit once — is Task 2 Steps 1-3. Its "plus a stateless test from the 3-statement repro" is Task 1 leg 1. Its strategic-superset note (shared `external_transaction` through bulk clone paths) is explicitly out of scope and untouched here. The three spec gaps are carried in the spec-delta section and land in Task 3.

**Placeholders.** None. Every code step gives the exact before and after text; the test and reference are complete.

**Type consistency.** `owned_transaction` is a `DiskTransactionPtr`, matching the name and type `freeze` already uses for the same role in this file. `copyDirectoryContentIntoTransaction` is called with the signature verified from its definition, including the trailing cancellation hook. No declaration in `DataPartStorageOnDiskBase.h` changes.

**Risks this plan cannot remove.** Leg 2's counter name `CASBlobBodyPutAvoided` was read from the event registry, not from a run, so the first execution settles whether that is the counter the dedup path actually increments; if it is not, Task 1 Step 2's second failure mode covers it and the fix is in the test. And the whole plan assumes the busy worktree — if the other session has finished by execution time, the dirtiness checks simply pass and Task 3 proceeds.
