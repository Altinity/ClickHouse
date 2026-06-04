# CAS MergeTree Multi-Part Disk Transaction (B67 layer 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build … > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Tests FOREGROUND, bounded (`timeout` ≤ 900), non-empty `--test`, never `clickhouse local`, never background a build/test. This is a **refactor confined to one class** — the regression gate (the 120 `ContentAddressed*` gtests + a CA non-transaction smoke set) is run BEFORE any Tier-2 un-gate.

**Goal:** Generalize `ContentAddressedTransaction` from single-part to per-part staging, so a transactional merge/mutation (whose one disk transaction spans the merge-output part + the covered source parts' `txn_version` rewrites, incl. the deferred `tmp_merge → final` rename) works on a content-addressed (CA) disk.

**Architecture:** Replace the single `(table_uuid, part_name)` + `recorded`/`recorded_mutable`/`recorded_mutable_removed`/`frozen_*` with a `std::map<PartKey, PartStaging>`. Every staging op routes to the entry for the path's parsed `(table_uuid, part_name)`. `commit`/`rollback` iterate all entries (new content parts → whole-part publish; mutable-only-on-committed parts → the B39 sidecar-in-place branch). Single-part is one entry and stays byte-equivalent. **Confined to `ContentAddressedTransaction.{h,cpp}`** — the `IMetadataTransaction` interface signatures and all non-CA / MVCC-engine code are unchanged.

**Spec:** `docs/superpowers/specs/2026-06-04-cas-mergetree-multipart-transaction-design.md`.
**Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified) — single-part state in `ContentAddressedTransaction.h` (~lines 385-412):**
`std::map<std::string, ContentAddressed::BlobEntry> recorded;` (content blobs → manifest), `std::map<std::string, std::string> recorded_mutable;` (mutable per-part files → sidecar), `std::set<std::string> recorded_mutable_removed;`, `std::string table_uuid; std::string part_name;`, `std::string frozen_backup_name; std::string frozen_table_dir;`. Transaction-wide (NOT per-part, leave as-is): `pinned_blob_keys`, `session_open`, `in_flight_pinned_blobs`.

**Methods that touch per-part staging (`ContentAddressedTransaction.cpp`):** `rememberTarget` (165), `recordBlob` (188), `tryGetInFlightStorageObjects` (195), `tryReadFileInFlight` (209), `tryGetInFlightFileSize` (225), `writeFile` (237), `moveFile` (946), `replaceFile` (1039), `unlinkFile` (1087), `readStagedOrCommittedBytes` (1019), `commit` (1135), `moveDirectory` (789); and the re-key/republish helpers that assume a single target: `republishCommittedPartIntoDetached` (388), `rekeyDetachedPartDir` (477), `republishDetachedStagingIntoActive` (557), `republishTableRefs` (700), `rekeyStagedProjectionDir` (747), `renameCommittedPartRef` (1391), `unlinkPartDirRefs` (1455), `removeRecursive` (1507). The detached/projection/republish helpers operate on COMMITTED refs (object storage), not the staging map — most need NO change; verify per Task 3.

---

## Phase 1 — the per-part refactor (regression-gated on single-part)

### Task 1: introduce `PartStaging` + the `parts` map + accessors; migrate the core staging methods

**Files:** Modify `…/ContentAddressed/ContentAddressedTransaction.h`, `…/ContentAddressedTransaction.cpp`

- [ ] **Step 1: define `PartStaging` + the map in the `.h`.** Replace the single-part members (`recorded`, `recorded_mutable`, `recorded_mutable_removed`, `frozen_backup_name`, `frozen_table_dir`) with:
```cpp
    /// Per-part staging. A single transaction may write more than one part — a transactional merge's
    /// one disk transaction spans the merge-output part PLUS the covered source parts' txn_version
    /// rewrites (B67). The single-part case (every INSERT / non-merge write) is exactly one entry.
    struct PartStaging
    {
        std::map<std::string, ContentAddressed::BlobEntry> recorded;     /// content blobs -> manifest
        std::map<std::string, std::string> recorded_mutable;             /// mutable per-part files -> sidecar
        std::set<std::string> recorded_mutable_removed;                  /// mutable files to delete from a committed sidecar
        std::string frozen_backup_name;                                  /// FREEZE target (per-part)
        std::string frozen_table_dir;
    };
    using PartKey = std::pair<std::string /*table_uuid*/, std::string /*part_name*/>;
    std::map<PartKey, PartStaging> parts;
```
Keep `table_uuid` and `part_name` as the **last-remembered target** (a convenience for path-routed callers that already parsed) — many methods reference them; they now mean "the most recent rememberTarget", not "the only part".

- [ ] **Step 2: add private accessors** (in the `.h`, near the helpers):
```cpp
    /// Get-or-create the staging entry for (table_uuid, part_name).
    PartStaging & stagingFor(const std::string & table_uuid_, const std::string & part_name_)
    {
        return parts[PartKey{table_uuid_, part_name_}];
    }
    /// The staging entry for a parsed part-file path (must be a part file).
    PartStaging & stagingForPath(const ContentAddressed::PartFilePath & p)
    {
        return stagingFor(p.table_uuid, p.part_name);
    }
    /// Lookup without creating (for read helpers); nullptr if absent.
    const PartStaging * findStaging(const std::string & table_uuid_, const std::string & part_name_) const
    {
        auto it = parts.find(PartKey{table_uuid_, part_name_});
        return it == parts.end() ? nullptr : &it->second;
    }
```

- [ ] **Step 3: `rememberTarget` — remove the single-part assertion; create the entry.** Replace the body so it sets the last-remembered target AND captures the frozen fields INTO that part's entry (no cross-part conflict throw):
```cpp
void ContentAddressedTransaction::rememberTarget(const std::string & path)
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: not a part file path: {}", path);
    table_uuid = p->table_uuid;
    part_name = p->part_name;
    auto & st = stagingForPath(*p);
    /// FREEZE target fields are per-part; set on first touch, then must stay consistent for that part.
    if (st.frozen_backup_name.empty() && st.frozen_table_dir.empty())
    {
        st.frozen_backup_name = p->backup_name;
        st.frozen_table_dir = p->shadow_table_dir;
    }
    else if (st.frozen_backup_name != p->backup_name)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: inconsistent FREEZE target for {}/{}: '{}' vs '{}'",
            p->table_uuid, p->part_name, st.frozen_backup_name, p->backup_name);
}
```

- [ ] **Step 4: route `recordBlob`, `writeFile`, `moveFile`, `replaceFile`, `unlinkFile`, `readStagedOrCommittedBytes`, and the in-flight read helpers (`tryGetInFlightStorageObjects`/`tryReadFileInFlight`/`tryGetInFlightFileSize`) to the per-part entry.** Each currently parses the path (or uses the member `recorded`/`recorded_mutable`). Mechanical rule: after `auto p = parsePartFilePath(path)` (or the existing parse), replace bare `recorded`/`recorded_mutable`/`recorded_mutable_removed` with `stagingForPath(*p).recorded` (etc.) for WRITES, and `findStaging(p->table_uuid, p->part_name)` (null-check) for READS (in-flight helpers return nullopt/empty when absent). For `moveFile`/`replaceFile`/`rekeyStagedProjectionDir` (which move within/between parts): operate on the SOURCE part's entry and the DEST part's entry — for a same-part rename (the common case) both are the same entry; for a cross-part move, move the file between the two entries. Read each method, apply the rule, preserve all surrounding logic (the mutable vs content distinction, the frozen-key selection — now `st.frozen_backup_name`).

- [ ] **Step 5: build** `ninja -C build clickhouse > build/mpt_t1_build.log 2>&1; echo "exit=$?"; grep -cE "error:|FAILED:" build/mpt_t1_build.log` → 0 errors. (Do not migrate `commit`/`moveDirectory` yet if it compiles with a temporary single-entry shim; otherwise migrate `commit` in Task 2 — but the build must be GREEN at each commit. If `commit` references the removed members, do Task 2 in the same build.)

- [ ] **Step 6: commit** `CAS txn mpt: per-part staging map + accessors; route core staging methods (single-part unchanged)`.

### Task 2: multi-part `commit` / `moveDirectory` re-key

**Files:** Modify `…/ContentAddressedTransaction.cpp`

- [ ] **Step 1: `commit` iterates `parts`.** The current `commit` (1135) builds one manifest from `recorded`, has the mutable-only branch (`recorded.empty()`), the whole-part publish, the sidecar writes. Restructure: take the per-pool `gc_lock` ONCE; `persistSession()` once (covers all parts' blobs); then `for (auto & [key, st] : parts)` apply the per-part publish — extract the existing single-part body into a `commitOnePart(const PartKey & key, PartStaging & st)` helper that does exactly what commit does today for one part (the `recorded.empty()` → mutable-only sidecar branch; else the whole-part manifest+sidecar+ref publish with B49 re-validation; using `st.recorded`/`st.recorded_mutable`/`st.recorded_mutable_removed`/`st.frozen_backup_name`/`st.frozen_table_dir` and `key.first`/`key.second` for uuid/part). The early-out `if (parts.empty()) return;` replaces `if (recorded.empty() && recorded_mutable.empty() && recorded_mutable_removed.empty()) return;` (a part entry with all-empty staging is a no-op — skip it inside the loop). Release the session pins after all parts (as today). Keep the `gc_lock` held across the whole loop so the multi-part publish is consistent w.r.t. a sweep.

- [ ] **Step 2: `moveDirectory` re-key across parts.** The deferred merge rename `moveDirectory(tmp_merge_X → X)` (and the existing detached/active re-key cases) must move the staging entry from the source part key to the dest key, MERGING into any existing dest entry. Read `moveDirectory` (789); for the part-dir → part-dir case, after computing src/dst parsed paths, if `parts` has a `{src.table_uuid, src.part_name}` entry, merge it into `{dst.table_uuid, dst.part_name}` (move `recorded`/`recorded_mutable` keys, preferring the existing dest entry's bytes on a key collision — a `txn_version.txt` already staged under the final name wins, since it is the newer MVCC state; recorded content blobs from the source carry over) and erase the source entry. Preserve the existing committed-ref move logic (the object-storage re-key it already does). (Detached/projection re-key helpers operate on committed refs, not staging — Task 3 verifies they need no staging change.)

- [ ] **Step 3: build** `ninja -C build clickhouse > build/mpt_t2_build.log 2>&1; echo "exit=$?"; grep -cE "error:|FAILED:" build/mpt_t2_build.log` → 0 errors.

- [ ] **Step 4: run the single-part regression gate (gtests).** `ninja -C build unit_tests_dbms > build/mpt_t2_gtest_build.log 2>&1; echo $?; build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/mpt_t2_gtest_run.log 2>&1; echo $?; tail -25 build/mpt_t2_gtest_run.log` → all 120 pass (single-part behavior unchanged). Fix until green.

- [ ] **Step 5: commit** `CAS txn mpt: multi-part commit (per-part publish) + moveDirectory staging re-key`.

### Task 3: verify the detached / projection / republish helpers + add the multi-part gtest

**Files:** Modify `…/ContentAddressedTransaction.cpp` (only if a helper regressed); Modify `src/Disks/tests/gtest_content_addressed_metadata.cpp`

- [ ] **Step 1: audit the republish/re-key helpers** (`republishCommittedPartIntoDetached`, `rekeyDetachedPartDir`, `republishDetachedStagingIntoActive`, `republishTableRefs`, `renameCommittedPartRef`, `unlinkPartDirRefs`, `removeRecursive`). These operate on COMMITTED refs/objects (via `metadata_storage`/`object_storage`), not the staging map, so most need no change. For any that DID read the bare `recorded`/`recorded_mutable` (grep them), route to the right part entry or to the new accessor. Confirm by reading each.

- [ ] **Step 2: add the multi-part gtest** in `gtest_content_addressed_metadata.cpp`: one `ContentAddressedTransaction` stages TWO content parts (`partA`, `partB` — distinct part files) AND a mutable-only update to a THIRD already-committed part (`partC`'s `txn_version.txt`), then `commit`; assert all three resolve correctly (A & B publish new manifests/refs; C's manifest/`part_id`/ref unchanged, its sidecar updated). A second test: stage content under `tmp_merge_X`, then a `txn_version.txt` write under final `X`, then `moveDirectory(tmp_merge_X → X)`, then `commit`; assert part `X` resolves with both the content and the mutable file (the re-key merged them). Build + run `--gtest_filter='ContentAddressed*'` → all green (now 122).

- [ ] **Step 3: CA non-transaction smoke (regression gate before any Tier-2).** Refresh symlink. Run a representative non-txn CA set on the CA-default job — `04278_content_addressed_disk`, `04279_content_addressed_gc`, `04280_content_addressed_clone_partition_works`, `04292_content_addressed_mutations`, `04287_content_addressed_detach_partition_listing`, `04299_content_addressed_projection_inline_disk`, `05002_content_addressed_fetch_partition`, `05003_content_addressed_freeze` — and the Tier-1 txn set `01172_transaction_counters 01173_transaction_control_queries 05004_content_addressed_transactions`. All must pass (the per-part refactor preserved single-part behavior across insert/GC/clone/mutation/detach/projection/fetch/freeze/txn). Command per the FREEZE plan's run pattern (non-empty `--test`, `timeout 900`, foreground). If anything regresses → STOP, fix, do not proceed.

- [ ] **Step 4: commit** `CAS txn mpt: multi-part gtest + verify republish helpers; single-part + CA-smoke regression clean`.

---

## Phase 2 — Tier-2 wave (reproduction-driven)

### Task 4: un-gate the mutation/isolation tests + iterate

**Files:** Modify the B67-gated Tier-2 tests.

- [ ] **Step 1: un-tag** (remove `no-content-addressed-storage`, preserve other tags + drop the B67 reason comment) and run in batches on the CA-default job (foreground, `timeout 900`, non-empty `--test`):
  - Batch A: `01168_mutations_isolation 01168_mutations_isolation_2 01174_select_insert_isolation 01170_alter_partition_isolation 03657_merge_tree_disk_support_transaction`
  - Batch B: `01167_isolation_hermitage 01169_alter_partition_isolation_stress 01169_old_alter_partition_isolation_stress 01171_mv_select_insert_isolation_long`
  - Batch C: `02421_truncate_isolation_no_merges 02421_truncate_isolation_with_mutations 02435_rollback_cancelled_queries 03803_transaction_mutation_race`
  - Batch D (likely orthogonal): `04036_backup_partition_transaction_visibility 03752_attach_as_replicated_transaction_metadata 03916_attach_as_replicated_implicit_transaction`

- [ ] **Step 2: reproduction-driven fixes.** Each failure is expected to be a further per-part touchpoint (rollback ordering of multiple precommitted parts, concurrent snapshot reads of a multi-part transaction's `txn_version.txt`, removal-TID unlock on rollback). Read the server err log; make the minimal CA-side fix within the per-part mechanism; rebuild foreground; re-run the 120+ gtests (no regression) after any CA change; re-run the batch. Commit per batch that stabilizes.

- [ ] **Step 3: honest re-gating.** Re-gate any test failing for a genuinely orthogonal reason (Batch D's attach-as-replicated / backup-visibility are the prime suspects — replicated/backup interactions, not layer 2) with a PRECISE reason; do not force, do not leave failing un-gated. Re-check `01170_alter_partition_isolation` (the spec flagged it as possibly merge-flaky — confirm it is stably green now).

- [ ] **Step 4: commit** the un-gates + fixes + documented re-gates.

---

## Phase 3 — regression, finalize, push

### Task 5: full regression + backlog + push

- [ ] **Step 1: non-CA (plain) regression** — `01172_transaction_counters 01173_transaction_control_queries 01168_mutations_isolation 01174_select_insert_isolation` on `"Stateless tests (arm_binary, parallel)"` → all pass (the refactor is CA-only; plain must be unchanged).
- [ ] **Step 2: CA full smoke** — re-run the Task-3 Step-3 set + the now-un-gated Tier-2 tests on the CA-default job → green.
- [ ] **Step 3: backlog** — `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: B67 layer 2 → DONE (per-part `ContentAddressedTransaction`); list the un-gated tests + any re-gated with reasons; note B67 fully closed (both layers) or any residual.
- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- A single `ContentAddressedTransaction` commits multiple parts (gtest: 2 content parts + 1 mutable-only co-part; the `tmp_merge → final` re-key merge); transactional merges/mutations work on CA.
- The B67 mutation/isolation Tier-2 tests pass on the CA-default job (or are re-gated for a documented orthogonal reason — Batch D).
- **No regression:** the 120 single-part `ContentAddressed*` gtests, the CA non-transaction smoke set, the Tier-1 txn tests + `05004`, and plain (non-CA) transactions all unchanged.
- Backlog B67 (both layers) closed; the change is confined to `ContentAddressedTransaction.{h,cpp}`.
