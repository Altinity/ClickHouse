---
description: 'Implementation plan for giving freezeRemote a content-addressed single-transaction branch so cross-disk ATTACH PARTITION FROM works'
sidebar_label: 'CAS freezeRemote transaction'
sidebar_position: 1
slug: /superpowers/plans/cas-freezeremote-transaction
title: 'freezeRemote single-transaction branch — implementation plan'
doc_type: 'guide'
---

# `freezeRemote` single-transaction branch — Implementation Plan {#cas-freezeremote-plan}

> **Blob-publication supersession (2026-08-23):** this historical plan predates mandatory blob
> `HEAD` and unconditional publication. Its transaction-boundary fix remains implemented, but its
> test prose/configuration about an adaptive `HEAD`-first threshold, a presence cache, and a
> conditional body PUT describes the superseded writer. Current reuse always starts with `HEAD` and
> records `CASBlobBodyPutAvoided` after a safe present observation; see the
> [current blob protocol](/antalya/cas/architecture/blob-protocol#conditional-write-sequence).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cross-disk `ALTER TABLE dst ATTACH PARTITION … FROM src` work when `dst` is on a content-addressed disk, by cloning the part through one CAS transaction instead of one autocommit per file.

**Architecture:** `freezeRemote` is the cross-disk clone path and the only one of the three without a CAS branch. It gains the same shape the fork already uses in `freeze`: when the caller supplied no transaction and the destination is content-addressed, self-create one, stream every file into it, route the post-clone removals through it, and commit once. The existing `Backup` path is untouched for every other destination.

**Tech Stack:** C++ (ClickHouse fork), stateless `.sh` test, per-query `ProfileEvents` read from `system.query_log`.

**Spec:** `docs/superpowers/cas/BACKLOG.md` `{#issue-2173-freezeremote-gap}` — CONFIRMED and REPRODUCED on HEAD. **Read the spec-delta section below before the tasks:** the spec's fix shape is right in outline and incomplete in three ways, and it cannot be corrected in place right now.

## Spec delta — what this plan adds to the spec, and why the spec was not edited {#spec-delta}

`docs/superpowers/cas/BACKLOG.md` is being actively rewritten by another session as this plan is written. Editing a file another agent is mid-write on risks losing one of the two edits, so the corrections live here and Task 3 acts on them once that file is free. Treat this section as authoritative where it disagrees with the spec.

Note what Task 3 does with the entry, because it is not what a reader would guess. That file declares itself a live backlog of **open work only**, with history left to git. The issue #2212 entry was not flipped to `FIXED` when it was fixed — it was **deleted**, and its anchor is already gone from the file. So a fixed #2173 leaves the live backlog too, and the durable record of what was done lives in this plan, in the triage document, and in the commits.

1. **The three post-clone `removeFileIfExists` calls must route through the self-created transaction.** The spec mentions only that the `metadata_version.txt` / `txn_version.txt` writes "become legal single repoints". It does not say that the removals, which today go straight to the disk, would autocommit — introducing one transaction and immediately breaking it with three separate publishes against the same ref.
2. **`freezeRemote`'s `external_transaction` branches must NOT be removed.** Verified: the whole function is byte-identical to `$(git merge-base altinity/antalya-26.6 HEAD)` — untouched upstream code. Those branches are dead *in this fork only*, because the single tree-wide assignment to `ClonePartParams::external_transaction` reaches `freeze`, not `freezeRemote`. Removing them would change shared upstream behaviour for no benefit to this fix, and the next upstream merge would meet the removal. A simplification along those lines was proposed during design and withdrawn for this reason.
3. **The helper this fix needs is declared *after* the function that must call it.** `copyDirectoryContentIntoTransaction` lives in an anonymous namespace between `freezeRemote` and `clonePart`, so `clonePart` can call it and `freezeRemote` cannot. The namespace contains that one helper and nothing else, so moving the block above `freezeRemote` is the minimal fix; a forward declaration would work too but leaves the reader hunting.

Two further facts the spec does not state, both verified, both load-bearing for the test:

- The path selector is `on_same_disk`: same disk goes to `freeze` (which already has the CAS branch), different disk goes to `freezeRemote`. It is not `copy_instead_of_hardlink`. A test whose two tables share a disk exercises the wrong function entirely.
- **CAS→CAS in one pool needs no extra production code.** The destination's publish resolves each blob through the dedup gate, so the clone is effectively a ref repoint. The ref-collision unknown tracked for `MOVE` (`[VERIFY-ca-ca-same-pool-move]`) does not transfer here: `ATTACH` writes into the *destination table's* namespace under a temporary part name, so there is no shared ref to collide on.

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`. No rebase, no amend — add new commits.
- **THE WORKTREE IS SHARED AND BUSY.** Another session is executing a different plan in this same checkout: it holds uncommitted edits under `src/Disks/tests/` and is rewriting `docs/superpowers/cas/BACKLOG.md`. Before staging anything, re-run `git status --short <the exact paths>` and stage only paths whose diff is yours. Never `git add -A`, `git add .`, or `git commit -a`. Do not stage a file while someone else's diff sits in it — that is a condition to re-check before each commit, not a permanent ban on a particular file.
- `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp` is upstream code. Change only what this fix needs; do not tidy, reorder, or delete anything else in it.
- No `LOGICAL_ERROR`. This change adds no new failure mode; the CAS branch either commits or undoes and rethrows.
- Allman braces (opening brace on its own line) — enforced by the style check.
- Comments must not cite this plan, the BACKLOG, a task number, or an issue number. Keep the reason, drop the provenance.
- The test number `05025_cas_attach_partition_cross_disk` is already reserved by `add-test`; both files exist as untracked stubs. Do not renumber, do not re-run `add-test`, do not `chmod`.

---

## Task 1: Pin the failure with a cross-disk attach test {#task-1}

**Files:**
- Modify: `tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.sh` (stub from `add-test`)
- Modify: `tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.reference` (emptied back to the `add-test` state; an earlier draft of this plan left a stale five-line version in it, so REPLACE its contents rather than appending)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: the test Task 2 must turn green.

Three legs, none of them decoration. Leg 1 is the issue's repro. Leg 2 covers the same-pool case that needs no production code and would otherwise never be exercised, and its counter assertion is the only thing separating "it worked" from "it worked by re-uploading everything". Leg 3 covers the replicated ATTACH, which reaches the same function through different parameters and then repoints an already-committed part -- a shape leg 1 does not produce, because a non-replicated attach leaves `metadata_version_to_write` unset.

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

# Every table this test creates, including leg 3's, so a run interrupted mid-script can be repeated.
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS src_plain;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS dst_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS src_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS dst_cas_same_pool;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS src_plain_repl SYNC;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS dst_cas_repl SYNC;"

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
    deduplication_head_first_min_bytes = 1,
    path = '05025_cas_shared_pool/');"

${CLICKHOUSE_CLIENT} --query "INSERT INTO src_cas SELECT number % 2, toString(number) FROM numbers(64);"

ATTACH_QUERY_ID="05025_attach_same_pool_${CLICKHOUSE_DATABASE}"
${CLICKHOUSE_CLIENT} --query_id "$ATTACH_QUERY_ID" \
    --query "ALTER TABLE dst_cas_same_pool ATTACH PARTITION 1 FROM src_cas;"

${CLICKHOUSE_CLIENT} --query "SELECT 'leg2', count(), sum(k), uniqExact(v) FROM dst_cas_same_pool;"

# The content is already in the pool, so the publish must have adopted the existing blobs rather
# than uploading their bodies again. `CASBlobBodyPutAvoided` is raised only on the HEAD-first branch,
# and that branch is taken on a dedup-cache hit or above `deduplication_head_first_min_bytes` --
# whose default is 1 MiB, far above these blobs, and the destination pool's presence cache starts
# empty because it is a different `Cas::Pool` object. Hence the destination disk lowers that
# threshold to 1: without it the publish takes the conditional-body-PUT branch, the counter stays 0,
# and this assertion fails for a reason that has nothing to do with the fix.
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS;"
${CLICKHOUSE_CLIENT} --query "
SELECT 'leg2_reused_blobs', ProfileEvents['CASBlobBodyPutAvoided'] > 0
FROM system.query_log
WHERE current_database = currentDatabase() AND query_id = '${ATTACH_QUERY_ID}' AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC LIMIT 1;"

# ------------------------------------------------ leg 3: replicated, local -> content-addressed

# The replicated ATTACH is a DIFFERENT shape and reaches the same function: of the replicated clone
# sites only the ATTACH branch passes `must_on_same_disk=false`, and its clone params set
# `metadata_version_to_write`, so after the transaction commits the caller writes
# `metadata_version.txt` separately -- a repoint of an already-published part rather than a file
# inside the clone. REPLACE on a replicated table is same-disk only and cannot reach this path.

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE src_plain_repl (k UInt32, v String)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/05025_src_repl', 'r1')
ORDER BY k PARTITION BY k
SETTINGS disk = disk(type = local, name = '05025_plain_repl', path = '05025_plain_repl/');"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE dst_cas_repl (k UInt32, v String)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/05025_dst_repl', 'r1')
ORDER BY k PARTITION BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05025_dst_repl',
    name = '05025_cas_dst_repl',
    path = '05025_cas_dst_repl_pool/');"

${CLICKHOUSE_CLIENT} --query "INSERT INTO src_plain_repl SELECT number % 2, toString(number) FROM numbers(64);"
${CLICKHOUSE_CLIENT} --query "ALTER TABLE dst_cas_repl ATTACH PARTITION 1 FROM src_plain_repl;"
${CLICKHOUSE_CLIENT} --query "SELECT 'leg3', count(), sum(k), uniqExact(v) FROM dst_cas_repl;"

# The metadata-version repoint lands on a committed part, so the part must still read after a
# detach/attach round trip -- that is what proves the repoint did not corrupt the published ref.
${CLICKHOUSE_CLIENT} --query "ALTER TABLE dst_cas_repl DETACH PARTITION 1;"
${CLICKHOUSE_CLIENT} --query "ALTER TABLE dst_cas_repl ATTACH PARTITION 1;"
${CLICKHOUSE_CLIENT} --query "SELECT 'leg3_roundtrip', count(), sum(k) FROM dst_cas_repl;"

${CLICKHOUSE_CLIENT} --query "DROP TABLE src_plain;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE dst_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE src_cas;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE dst_cas_same_pool;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE src_plain_repl SYNC;"
${CLICKHOUSE_CLIENT} --query "DROP TABLE dst_cas_repl SYNC;"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

Reference file:

```
leg1	32	32	32
leg1_roundtrip	32	32
leg2	32	32	32
leg2_reused_blobs	1
leg3	32	32	32
leg3_roundtrip	32	32
dropped_ok
```

- [ ] **Step 2: Run it and confirm WHERE it fails**

Run: `./tests/clickhouse-test 05025_cas_attach_partition_cross_disk`

Expected: leg 1's `ATTACH PARTITION 1 FROM src_plain` throws. The reported error is one of two, and which one depends on which file the pool reaches first — both are the same defect: a unique-ref refusal from the promote, or `NOT_IMPLEMENTED` about autocommit writes not being supported for content part files. The single production fact responsible is that `freezeRemote` has no content-addressed branch, so `Backup` fans the part's files onto a thread pool and each becomes its own autocommit transaction against one ref.

The script deliberately does not `set -e`. Only about an eighth of the stateless shell tests do, and here it would abort the run at leg 1 on the pre-fix tree — which is the expected state — leaving a truncated diff instead of showing which legs are missing. The instruction below is addressed to you, not to the script: the script runs to the end and the reference diff tells you what failed.

**Do not proceed to Task 2 until you have seen that.** Three other outcomes mean the test is wrong rather than the bug pinned, and each is fixed here:

- The attach *succeeds*. Then the two tables are not on different disks and the clone went through `freeze`. Check the disk names actually differ and that the destination policy reserves the CAS disk.
- Leg 2's counter row is empty rather than `0`/`1`. Then the `query_id`, the `current_database` filter, or `SYSTEM FLUSH LOGS` placement is wrong, not the dedup claim.
- Leg 2's counter row is `0`. Then check `deduplication_head_first_min_bytes = 1` really reached the destination disk, because that is what puts the publish on the HEAD-first branch the counter lives on.

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

Leave `save_metadata_callback` and the the `create` tail exactly as they are. The callback is empty at the only call site and is upstream signature shape.

- [ ] **Step 4: Correct the helper's own justification, which this change outgrows**

`copyDirectoryContentIntoTransaction`'s doc comment justifies copying sequentially rather than through the parallel thread pool by saying "MOVE is a background, latency-insensitive operation, so parallelizing this is a deferred optimization, not a correctness requirement". After this task the same helper also serves `ATTACH PARTITION FROM`, which is a foreground statement a user waits on — so the justification no longer covers all of its callers.

Keep the correctness half untouched (a content-addressed transaction batches every file into one manifest and its staging map is not mutex-guarded — that is why the copy is sequential). Replace only the latency clause: say that the callers are a background move and a user-issued cross-disk attach, and that parallelising it remains a deferred optimisation whose cost is now visible to a waiting statement rather than only to a background one. Do not parallelise anything here — that is a separate change with its own correctness argument to make about the staging map.

- [ ] **Step 5: Build and run**

Build BOTH binaries — the server for the stateless tests and the unit-test binary for the gate, since building only one leaves the other stale and a green gate on a stale binary is evidence about different code. Redirect every build and every run to its own uniquely named log under the build directory, and have a subagent summarise each log rather than reading them inline:

```bash
ninja -C build clickhouse      > build/2173_clickhouse_build.log 2>&1; echo "EXIT=$?" >> build/2173_clickhouse_build.log
ninja -C build unit_tests_dbms > build/2173_unit_build.log 2>&1;       echo "EXIT=$?" >> build/2173_unit_build.log

./tests/clickhouse-test 05025_cas_attach_partition_cross_disk > build/2173_new_test.log 2>&1;        echo "EXIT=$?" >> build/2173_new_test.log
./tests/clickhouse-test 05003_cas_freeze 05024_cas_freeze_two_roots > build/2173_freeze_tests.log 2>&1; echo "EXIT=$?" >> build/2173_freeze_tests.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/2173_cas_gate.log 2>&1;             echo "EXIT=$?" >> build/2173_cas_gate.log
```

Every command carries its own marker, and the status is read from the marker rather than from the shell: a wrapper's exit code has lied in this repository before.

Expected: the new test passes all three legs; the two freeze tests are unaffected — they exercise `freeze`, not `freezeRemote`, and this change touches neither `freeze` nor the helper's body; the gtest gate is unchanged, since no CAS-internal signature moved.

- [ ] **Step 6: Commit**

```bash
git status --short src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
git add src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
git commit -m 'Clone a part into a content-addressed disk in one transaction on the cross-disk path'
```

---

## Task 3: Retire the entry and update the triage record {#task-3}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG.md` — **delete** the `{#issue-2173-freezeremote-gap}` section, only if the file is clean
- Modify: `docs/superpowers/cas/2031-triage.md` — five CAS-058 sites and four more references to the dying anchor
- Modify: `docs/superpowers/cas/final-checks-todo.md` — item 1, which also holds a link to that anchor

**Interfaces:**
- Consumes: Tasks 1 and 2 landed.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Check whether the backlog is yours to touch**

Run `git status --short docs/superpowers/cas/BACKLOG.md`.

If it reports anything, another session is still working in it. **Stop and report that.** Do not edit it, do not stage it, and do not relocate the content elsewhere to get around it — the plan's spec-delta section stays the record until someone can land this properly. Go on to Step 3 meanwhile; the triage file is a different file and may be free.

- [ ] **Step 2: If clean, delete the entry**

Remove the whole `{#issue-2173-freezeremote-gap}` section. Do not flip its heading to `FIXED`, and do not leave a stub: the file's own header says it carries only open work, and the #2212 entry set the precedent by being deleted rather than annotated. Before removing it, grep `BACKLOG/` and `docs/superpowers/cas/` for links to that anchor; any link dies with it, in the same commit.

- [ ] **Step 3: Update the triage record — five CAS-058 sites, and every one is needed**

`grep -n "CAS-058" docs/superpowers/cas/2031-triage.md` returns five lines. Read the whole output; an earlier revision of this plan claimed four because the grep behind it had been piped through `head`, and the site it missed is the largest one.

1. The verdict row, currently `подтверждено | P1` with a link to the backlog anchor. Mark it fixed, name the commits, and repoint the link at this plan — the anchor dies in Step 2 and a dangling link is worse than none.
2. The `{#p1-list}` table. Drop the CAS-058 row.
3. The paragraph after that table, "Из четырёх исходных P1 CAS-001 закрыт коммитами …". CAS-058 joins CAS-001 there, and the sentence about which P1s the triage did not find first has to still parse with one fewer row above it.
4. The priority tally line reading `**P1 — 3**`. It becomes 2. A stale P1 count is worse than a stale sentence: it is the number someone reads to decide whether the release is ready.
5. **The full `{#cas-058}` section.** Its heading still ends `(подтверждено, P1)` and its body says the fix is only scheduled. Historicise it the way this document already historicises a closed verdict elsewhere — do not delete it: unlike the live backlog, the triage IS the history, and its own header says so.

- [ ] **Step 4: Correct the four other references to the dying anchor in the same file**

`grep -n "issue-2173-freezeremote-gap" docs/superpowers/cas/2031-triage.md` returns four more lines beyond the verdict row. Two of them assert things that stop being true, and they matter more than the links:

- one states outright that on HEAD the fix is NOT applied and the branch is absent in `freezeRemote` — after Task 2 that is false;
- another calls `freezeRemote` "незакрытый член семьи" of the three clone paths — the family has no unclosed member now;
- the remaining two are cross-references (a not-a-duplicate note and a family reference) whose links die with the anchor.

- [ ] **Step 5: Close the scheduling item, which also holds a link to the anchor**

`docs/superpowers/cas/final-checks-todo.md` item 1 is the pre-release schedule line for this fix and points at the anchor Step 2 removes. That is why it lives in this task rather than in Task 4: the anchor and every link to it have to go in one commit, and a link left behind in another task's file would dangle in between. Item 2 in the same file was closed by another session for issue #2212 and shows the format — the heading gains `DONE —` and the body names the commits.

- [ ] **Step 6: Commit**

Re-check dirtiness across all three files, stage only paths whose diff is yours, then:

```bash
git commit -m 'Retire the cross-disk clone gap from the live backlog'
```

---

## Task 4: The rest of the documentation {#task-4}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG/replication.md` (`[VERIFY-ca-ca-same-pool-move]`)
- Modify: `docs/superpowers/cas/BACKLOG/formats-and-storage.md` (the re-freeze-same-part-name item)
- Modify: `src/Storages/MergeTree/MergeTreeData.cpp` (the partition-clone support comment)
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorage.cpp` (the gated-clone-paths comment)

`final-checks-todo.md` is NOT here — it moved to Task 3, because it links the backlog anchor Task 3 deletes and the two have to travel in one commit.

**Interfaces:**
- Consumes: Tasks 1 and 2 landed.
- Produces: nothing later tasks depend on.

Mostly documentation, plus two source comments that need a build. Each file gets its own dirtiness check before staging, for the reason in the Global Constraints.

- [ ] **Step 1: Record what the new test does and does NOT answer for the same-pool question**

`BACKLOG/replication.md`'s `[VERIFY-ca-ca-same-pool-move]` asks whether a same-pool CA↔CA clone works and whether the target's ref `<part>` collides with the source's. The new test's second leg answers the first half for `ATTACH`: the publish dedup-resolves the blobs and the clone is a ref repoint, now asserted by a counter.

**Do not close the item.** Its collision question is about `MOVE`, where source and target are the SAME table — one namespace, one ref name `<part>`. `ATTACH` writes into the destination table's namespace under a temporary part name, so it cannot collide and therefore proves nothing about the case the item is actually asking about. Add that distinction to the item so the next reader does not close it on the strength of this test.

- [ ] **Step 2: Record that this fix widens another item's reachability**

`BACKLOG/formats-and-storage.md`'s re-freeze item says that re-freezing a DIFFERENT part carrying the same part name — "after `REPLACE PARTITION` / `ATTACH PARTITION FROM`, which can reuse `all_1_1_0`" — silently produces one frozen ref mixing two snapshots. Until now the cross-disk route to that state was closed by the very bug this plan fixes. Note on the item that cross-disk `ATTACH PARTITION FROM` into a content-addressed disk now works, so that route is open and the item's priority should be reconsidered rather than inherited.

This is the kind of consequence a fix does not usually announce about itself: nothing in the fix is wrong, and another item got more reachable because of it.

- [ ] **Step 3: Correct two source comments the fix falsifies**

Both sit outside the file Task 2 touches, so they need their own build. Both are load-bearing prose about what is and is not supported.

`src/Storages/MergeTree/MergeTreeData.cpp`, in the `MetadataStorageType::CAS` case that discusses partition-clone support, says the clone path "is now transactional — `DataPartStorageOnDiskBase::freeze` runs the whole clone through ONE CA transaction". After Task 2 that is one of two such paths, and the neighbouring claim that cloning is "publishing a ref (no byte copy)" holds for a same-pool clone and not for a cross-disk clone from a local disk, which streams bytes. Name both paths and say which one copies.

`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp`, in the comment explaining why advertising a capability does not open the per-file clone hazard, lists "the corrupting whole-part clone paths (partition clone, BACKUP hard-link, replication)" as "gated by their own independent checks". The partition clone is no longer gated — it works. Remove it from that list rather than rewording around it, and leave the other two entries alone.

- [ ] **Step 4: Build and run after the comment edits**

Comment-only, but they are in `.cpp` files, so both binaries get rebuilt and each run gets its own log:

```bash
ninja -C build clickhouse      > build/2173_docs_clickhouse_build.log 2>&1; echo "EXIT=$?" >> build/2173_docs_clickhouse_build.log
ninja -C build unit_tests_dbms > build/2173_docs_unit_build.log 2>&1;      echo "EXIT=$?" >> build/2173_docs_unit_build.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/2173_docs_cas_gate.log 2>&1; echo "EXIT=$?" >> build/2173_docs_cas_gate.log
```

Read each status from its marker, not from the shell. Say explicitly in the report that you changed no code; if making a comment true required changing code, stop and say so instead.

- [ ] **Step 5: Commit**

Re-check dirtiness across all four paths — two documents and two sources — stage only what is yours, then:

```bash
git commit -m 'Record the cross-disk clone fix across the tracking documents and comments'
```

## Documentation checked and deliberately NOT changed {#docs-not-changed}

Recorded so the executing agent does not re-derive it, and so a later reader knows the sweep happened. The triage document is NOT in this list — it is changed, in Task 3 Step 3.

- **`docs/en/antalya/cas/`** — nothing stale. The user-facing roadmap's `{#known-limitations}` list never mentioned cross-disk `ATTACH PARTITION FROM`, so there is no limitation to retract; and this was a bug rather than an unshipped feature, so there is nothing to announce either. Checked every page for `ATTACH PARTITION`, "not supported" and "limitation".
- **`BACKLOG/operability-and-introspection.md`** — its list of paths reaching `freeze` includes "Same-disk `ATTACH/REPLACE PARTITION FROM`". That stays true: same-disk still routes to `freeze`, and this plan changes only the cross-disk path. Left alone on purpose.
- **`BACKLOG/replication.md` `[move-part-to-ca-architecturally-unimplemented]`** — already marked CLOSED at HEAD and kept for provenance. No edit needed there; the stale sentence about the family's remaining unfixed member lives in the `BACKLOG.md` entry and is handled in Task 3.
- **`BACKLOG/replication.md` `[RPL-5 slice]`** — the replicated queue-clone relink test gap. This plan touches the local cross-disk path, not the queue path, so the gap is unchanged.
- **`clonePart`'s L2 comment** — says it mirrors `freeze`'s `owned_transaction` shape. Still true after this change; a third mirror does not make it false.

## Self-review {#self-review}

**Spec coverage.** The spec's fix shape — self-create a transaction, stream through the existing helper, commit once — is Task 2 Steps 1-3. Its "plus a stateless test from the 3-statement repro" is Task 1 leg 1. Its strategic-superset note (shared `external_transaction` through bulk clone paths) is explicitly out of scope and untouched here. The three spec gaps are carried in the spec-delta section and land in Task 3.

**Placeholders.** None. Every code step gives the exact before and after text; the test and reference are complete.

**Type consistency.** `owned_transaction` is a `DiskTransactionPtr`, matching the name and type `freeze` already uses for the same role in this file. `copyDirectoryContentIntoTransaction` is called with the signature verified from its definition, including the trailing cancellation hook. No declaration in `DataPartStorageOnDiskBase.h` changes.

**Two misses this plan corrected, both worth naming.** The CAS-058 site list said four and the real count is five, because the grep behind it was piped through `head` — the missing site being the largest one, a full section whose heading still called the issue open. Any claim in this plan of the form "N places" should be re-derived with an untruncated grep before it is trusted. And an earlier revision put the scheduling item in a different task from the anchor it links, which would have left a dangling link between two commits.

**A risk that was removed rather than deferred.** An earlier revision left leg 2's counter to "the first run will settle it". That was derivable without a run and wrong: `CASBlobBodyPutAvoided` is raised only on the HEAD-first branch, whose threshold defaults to 1 MiB, and these blobs are far smaller — so the assertion would have failed for a reason unrelated to the fix. The destination disk now lowers that threshold explicitly. What genuinely remains unverifiable by reading is only whether the counter is emitted once per blob or once per part, which changes nothing about the assertion being `> 0`. And the whole plan assumes the busy worktree — if the other session has finished by execution time, the dirtiness checks simply pass and Task 3 proceeds.
