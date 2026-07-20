# Umbrella-Review Fixes (no new features) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the maximum number of findings from `docs/superpowers/reports/2026-07-20-umbrella-review-cas-vs-antalya-26.6.md` using the smallest number of behavior-changing ("dangerous") interventions, and no new-feature work.

**Architecture:** Seven phases ordered by risk. Phases 1–5 are fail-close guards, exception-path hardening, diagnostics fixes, mechanical refactors, tests-only and strings/docs — none of them changes behavior for a healthy configuration. Phase 7 contains exactly two medium-risk performance changes, each independently droppable. Everything genuinely dangerous (relink retention pin, startup retry, lock restructuring) is explicitly deferred with reasons.

**Tech Stack:** C++ (ClickHouse), gtest (`src/Disks/tests/`, `src/IO/S3/tests/`), stateless SQL tests, integration tests (praktika), docs.

## Global Constraints

- Allman braces everywhere (CI style check).
- Never rebase/amend; one commit per task; never push without explicit authorization.
- Say "exception", not "crash", for logical errors; `chassert` is a no-op in release — release-critical invariants need real throws.
- Fail-close: no silent fallbacks; malformed persisted/wire data throws `CORRUPTED_DATA`.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>`; no `no-*` tags unless strictly necessary.
- Docs files need frontmatter + `{#kebab-anchor}` on every heading.
- Build: `ninja -C <build_dir> <target> > <build_dir>/build_<task>.log 2>&1` (no `-j`); analyze logs with a subagent.
- Unit tests binary: `<build_dir>/src/unit_tests_dbms`; run CA gate as `--gtest_filter='Cas*:CA*:Ref*'` plus the filter named in the task.
- Full report with evidence and line anchors: `docs/superpowers/reports/2026-07-20-umbrella-review-cas-vs-antalya-26.6.md` (referred to below as "report finding N").

## Explicitly deferred (do NOT implement in this plan)

| Item | Reason |
|---|---|
| Relink retention pin (report finding 1) | Planned separately as a protocol feature (fetch-handoff epoch-floor design). |
| Startup bounded retry (finding 6) | Already has its own recorded fix design on the branch (`ensureRefTableRecovered` seal-PUT retry, commit `3b9325f8029`); startup semantics change = dangerous, executed under its own plan. |
| `promote()` skip of manifest GET-back (perf minor) | Violates the project rule "no skip-read shortcuts in CA storage; re-proving is the identity primitive". Rejected, not deferred. |
| `head_first` adaptive threshold (perf minor) | Write-path tuning; needs benchmark data first. |
| Orphan-sweep per-pass protection-view cache (perf 45) | GC-internals churn touching a verified-correct area; low urgency. |
| `SingleWriterSlot::renewOnce` lock restructuring (concurrency 35) | Concurrency change on the lease heartbeat; benefit is theoretical today. A documenting comment lands in Task 14 instead. |
| `promote()` setting `alive = false` (deep-audit nit) | Hidden call-path risk: `publishStaging`/destructor legitimately call `abandon()` after some promote-adjacent flows; symmetry change could convert no-ops into throws. Document-only. |
| GC-log `srid` column, Prometheus `AsynchronousMetrics` for GC health (operability 42) | Additive schema/metric surface = feature work. |
| `GARBAGE COLLECTION`/`GC REBUILD` client result sets (ux 45) | Additive UX; harmless but not a fix. Do after this plan if desired. |
| Upstream diff split, worklog-path comment citations, `CasLayout.h` out-of-line move | Upstream-preparation work, separate effort. |

---

## Phase 1 — Fail-close guards (no behavior change for healthy configs)

### Task 1: Read-only guard for `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER`

Covers report finding 3(a). Risk: **safe** (adds a throw on a path that today silently misbehaves).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (public section)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`createTransaction` ~line 608, GC checks ~lines 372/404)
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (`CONTENT_ADDRESSED_DROP_POOL_MEMBER` case, ~line 1016)
- Test: `tests/integration/test_content_addressed_drop_pool_member/test.py`

**Interfaces:**
- Produces: `void ContentAddressedMetadataStorage::checkNotReadOnly(std::string_view what) const` — throws `ErrorCodes::READONLY` when the disk is in observe-only mode.

- [ ] **Step 1: Add the helper.** In the header, next to `isContentAddressed()`:

```cpp
    /// Fail-close gate shared by every mutating entry point (transactions, GC round, GC rebuild,
    /// pool-member decommission): an observe-only (`<readonly>`) disk must reject them all.
    void checkNotReadOnly(std::string_view what) const;
```

In the .cpp:

```cpp
void ContentAddressedMetadataStorage::checkNotReadOnly(std::string_view what) const
{
    if (read_only)
        throw Exception(ErrorCodes::READONLY,
            "Content-addressed disk is opened read-only: {} is rejected", what);
}
```

- [ ] **Step 2: Use it at all four mutating entry points.** Replace the inline `read_only` throw in `createTransaction` (~line 608) and the `read_only` half of the two GC checks (~lines 372, 404) with calls to `checkNotReadOnly("writes")` / `checkNotReadOnly("GC round")` / `checkNotReadOnly("GC rebuild")` (keep the separate `!gc_enabled` checks as-is). In `InterpreterSystemQuery.cpp`, immediately after the `if (!ca) throw ...` in the `CONTENT_ADDRESSED_DROP_POOL_MEMBER` case, add:

```cpp
            ca->checkNotReadOnly("SYSTEM CONTENT ADDRESSED DROP POOL MEMBER");
```

- [ ] **Step 3: Write the failing integration test.** In `tests/integration/test_content_addressed_drop_pool_member/test.py`, add a second disk to the node config that points at the same bucket with `<readonly>true</readonly>` (copy the existing disk XML block, rename to `ca_disk_ro`, add the readonly tag), and:

```python
def test_drop_pool_member_rejected_on_readonly_disk(started_cluster):
    with pytest.raises(QueryRuntimeException, match="read-only"):
        node.query("SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'whatever' FROM DISK 'ca_disk_ro'")
```

- [ ] **Step 4: Run the test, confirm it fails without the guard (revert Step 2 locally to check if unsure), passes with it.** `python -m ci.praktika run "integration" --test test_content_addressed_drop_pool_member > <build_dir>/test_task1.log 2>&1`, subagent-summarize the log.
- [ ] **Step 5: Build + run the CA gtest gate to confirm no regression.**
- [ ] **Step 6: Commit** — `cas: fail-close read-only guard for SYSTEM CONTENT ADDRESSED DROP POOL MEMBER`.

### Task 2: Renew the decommission admin lease regardless of host-disk mode

Covers report finding 3(b). Risk: **safe** (scoped to the decommission path only).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:504-511` (`Pool::openForDecommission`)
- Test: existing decommission gtest file under `src/Disks/tests/` (locate with `grep -rln openForDecommission src/Disks/tests/`)

- [ ] **Step 1: Write the failing assertion** in the decommission gtest: after `Pool::openForDecommission(...)`, assert `store->config.background_watermark == true` even when the incoming `PoolConfig` has `background_watermark = false` (construct the config that way in the test).
- [ ] **Step 2: Run it, confirm FAIL.**
- [ ] **Step 3: Fix** — in `openForDecommission`, next to the two existing force-sets:

```cpp
    config.server_root_id = victim_srid;
    config.read_only = false;
    config.skip_access_check = true;   /// the pool exists (the calling disk validated it); no probe writes
    /// The admin claim must be RENEWED like any writable mount: the host disk may be observe-only
    /// (background_watermark=false), but an unrenewed claim (TTL ~30s) aborts any long drain midway.
    config.background_watermark = true;
```

- [ ] **Step 4: Run the test — PASS; run the decommission gtest suite fully.**
- [ ] **Step 5: Commit** — `cas: decommission admin claim always renews its lease (force background_watermark)`.

### Task 3: Reject explicit `use_fake_transaction=true` on CAS/Keeper disks

Covers report finding 4. Risk: **safe** (config validation; only rejects a config that silently corrupts today).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:85-87`
- Test: new stateless test via `./tests/queries/0_stateless/add-test cas_reject_fake_transaction.sh`

- [ ] **Step 1: Implement the rejection** (fail-close, mirrors the missing-`server_root_id` handling):

```cpp
        const auto metadata_type = metadata_storage->getType();
        const bool needs_real_transaction = metadata_type == MetadataStorageType::Keeper
            || metadata_type == MetadataStorageType::ContentAddressed;
        /// An explicit `use_fake_transaction=true` on a metadata type that requires deferred
        /// transactions would silently break the atomic manifest/ref publish (per-file autocommit,
        /// no commit point). Reject it instead of honoring it.
        if (needs_real_transaction && config.getBool(config_prefix + ".use_fake_transaction", false))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Disk '{}': `use_fake_transaction` cannot be enabled for metadata type '{}'",
                name, magic_enum::enum_name(metadata_type));
        bool use_fake_transaction = config.getBool(config_prefix + ".use_fake_transaction", !needs_real_transaction);
```

(Check `ErrorCodes::BAD_ARGUMENTS` is declared in this file; add if missing. If `magic_enum` isn't already used here, print the raw `metadata_type` name via the existing to-string helper or a literal.)

- [ ] **Step 2: Write the failing test.** Shell test creating a custom disk inline and expecting the error (minio endpoint as in existing custom-disk tests):

```sql
-- expects BAD_ARGUMENTS mentioning use_fake_transaction
CREATE TABLE t (x Int) ENGINE=MergeTree ORDER BY x
SETTINGS disk = disk(type='s3', metadata_type='content_addressed', use_fake_transaction=1,
                     server_root_id='srv-test', endpoint='http://localhost:11111/test/cas_fake_txn/', ...);
```

Model the disk(...) argument list on an existing CA stateless test (grep `tests/queries/0_stateless` for `content_addressed` disk examples). Assert the query fails with the new message; `.reference` captures the error-check echo.

- [ ] **Step 3: Run failing → implement → run passing** (`python3 -m ci.praktika run` stateless job with `--test <name>`), logs to `<build_dir>/test_task3.log`.
- [ ] **Step 4: Commit** — `cas: reject explicit use_fake_transaction on content_addressed/Keeper disks`.

### Task 4: `RefCowMap::find` — mergeable iterator for overlay-only keys

Covers report finding 10. Risk: **safe** (strict improvement; identical behavior when the key is shadowed).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.cpp:79`
- Test: the existing RefCowMap gtest (locate: `grep -rln RefCowMap src/Disks/tests/`)

- [ ] **Step 1: Write the failing test:**

```cpp
TEST(CasRefCowMap, FindOverlayOnlyKeyIteratesIntoBase)
{
    RefCowMap m = /* base with rows "A" and "D" via the fixture's usual construction */;
    m.insert_or_assign("B", makeRow("mB"));   /// overlay-only key between base keys
    auto it = m.find("B");
    ASSERT_NE(it, m.end());
    EXPECT_EQ(it->first, "B");
    ++it;
    ASSERT_NE(it, m.end());                    /// FAILS today: iterator collapses to end()
    EXPECT_EQ(it->first, "D");
}
```

(Adapt `makeRow`/construction to the fixture's helpers.)

- [ ] **Step 2: Run — FAIL** (`--gtest_filter='*RefCowMap*'`).
- [ ] **Step 3: Fix — one line** at `CasRefCowMap.cpp:79`:

```cpp
        it.base_it = base->lower_bound(key);   /// first base key >= this one: keeps the iterator mergeable
```

- [ ] **Step 4: Run — PASS; run the full RefCowMap suite.**
- [ ] **Step 5: Commit** — `cas: RefCowMap::find keeps the merge iterator valid for overlay-only keys`.

### Task 5: `CasGcOutcomesFormat` digest-width check before `fromHex`

Covers report minor (correctness 32). Risk: **safe** (error-code correction on already-rejected corrupt input).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.cpp:118-119`
- Test: the formats gtest covering outcome logs (`grep -rln decodeOutcomeLog src/Disks/tests/`)

- [ ] **Step 1: Failing test:** encode a valid outcome log, corrupt the `h` hex field to a wrong length, assert decode throws with `ErrorCodes::CORRUPTED_DATA` (today: `BAD_ARGUMENTS`).
- [ ] **Step 2: Fix** — mirror `CasPartManifestFormat.cpp:225-234`:

```cpp
        const BlobHashAlgo algo = blobHashAlgoFromWord(ha, "outcome log");
        /// Validate the digest width before `fromHex`: a width mismatch must surface as the
        /// CORRUPTED_DATA required for malformed serialized input, not fromHex's BAD_ARGUMENTS.
        if (hhex.size() != blobHashLenFor(algo) * 2)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS outcome log: digest width {} does not match algo '{}'", hhex.size(), ha);
        e.ref = BlobRef{algo, codecFor(algo).fromHex(hhex)};
```

- [ ] **Step 3: Run failing→passing; commit** — `cas: outcome-log decode fails closed with CORRUPTED_DATA on digest width mismatch`.

---

## Phase 2 — `abandon()` exception-path hardening

Covers report finding 2 (all three facets). All changes touch **only exception paths**; the happy path is byte-identical.

### Task 6: Guard audit emits in `PartWriteTxn::abandon` and fix `alive` ordering

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:1128-1213`
- Test: `src/Disks/tests/` — the PartWriteTxn/pool gtest (locate: `grep -rln 'abandon' src/Disks/tests/gtest_cas_pool.cpp` and siblings)

- [ ] **Step 1: Failing test A (throwing sink must not fail abandon).** Using the in-memory backend fixture, install an event sink that throws from `emitEvent`, run a build through `stageManifest`+`precommitAdd`, call `abandon()`, assert: no exception propagates, and the precommit binding is gone (resolve of the ref reports no precommit).
- [ ] **Step 2: Failing test B (abandon retryable when append fails).** Force the first `appendRefOps` inside abandon to fail (use the existing wedge/fault test hook — `forceWedgeForTest` or the fixture's failing-backend wrapper), assert `abandon()` throws, then clear the fault and assert a **second** `abandon()` succeeds (today: `LOGICAL_ERROR "has been abandoned"`).
- [ ] **Step 3: Implement.** In `abandon()`:
  1. Move `alive = false;` from before the `if (precommitted)` block to immediately **after** it (i.e. after `appendRefOps` succeeded / when there was nothing to append). Keep the `cancelled` early-branch's own `alive = false` untouched. Add the comment:

```cpp
    /// `alive` flips only after the correctness-bearing precommit removal is durable: a caller that
    /// catches an appendRefOps failure may retry abandon() on this same object. A retry after an
    /// ambiguous already-appended failure re-validates old_binding and errors — never corrupts.
    alive = false;
```

  2. Wrap all three `EventEmitter{*store}.emit(...)` calls (cancelled-branch `BuildAbort`, `PrecommitRemoved`, final `BuildAbort`) exactly like `promote()` does:

```cpp
    try
    {
        EventEmitter{*store}.emit([&](CasEvent & e) { /* unchanged body */ });
    }
    catch (...)
    {
        tryLogCurrentException(getLogger("CasPartWriteTxn"), "CAS event emission after durable abandon");
    }
```

- [ ] **Step 4: Run both tests — PASS; run the full `Cas*:CA*:Ref*` gate.**
- [ ] **Step 5: Commit** — `cas: abandon() audit emission guarded + retryable precommit removal (review finding 2)`.

### Task 7: `publishEntries` abandons its build on exception

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp:308-322`
- Test: the PartFolderAccess gtest (`grep -rln publishEntries src/Disks/tests/`)

- [ ] **Step 1: Failing test:** with a fault injected into promote (fixture backend failure on the ref append / conditional PUT), call `publishEntries`, catch the exception, clear the fault, then assert the namespace has **no live precommit binding left** (e.g. via the ledger's precommit-introspection test hook / `committedOverlayEntriesForTest`, or by asserting a follow-up conflicting `publishEntries` for the same `dst` succeeds — today it is blocked by the leaked binding until remount).
- [ ] **Step 2: Implement:**

```cpp
void CachedPartFolderAccess::publishEntries(const PartRefKey & dst,
    const std::vector<Cas::ManifestEntry> & entries, Cas::ProvenanceOp op, bool allow_repoint)
{
    auto build = store->beginPartWrite(Cas::PartWriteInfo{.intended_ref = dst.ns.string() + "/" + dst.ref,
                                                  .intended_namespace = dst.ns, .op = op});
    try
    {
        /// Record write evidence for each non-inline entry. No pool HEAD/GET is performed before
        /// precommit; the promote path re-proves each dependency fail-closed. Inline entries need no evidence.
        for (const auto & entry : entries)
            build->adoptEvidence(entry);
        /// Stage a fresh manifest over the same entries. Blobs are content-addressed, but each part owns
        /// its manifest ID, so `dst` receives a distinct manifest before ownership moves to it.
        const Cas::ManifestId id = build->stageManifest(entries);
        build->precommitAdd(dst.ns, dst.ref, id);
        promoteBuild(*build, dst, build->buildId(), id, allow_repoint);
    }
    catch (...)
    {
        /// A failed publish must not leak a live-epoch precommit binding: only abandon() removes it
        /// (the destructor merely retires the build seq; the stale sweep is prior-epoch-scoped and GC
        /// never touches live precommits). Abandon may itself fail on the same broken backend — log,
        /// the original error stays primary.
        try
        {
            build->abandon();
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("CachedPartFolderAccess"),
                                   "abandoning the build of a failed publishEntries");
        }
        throw;
    }
}
```

- [ ] **Step 3: Run failing→passing; full gate; commit** — `cas: publishEntries abandons its build on exception (no leaked live-epoch precommit)`.

### Task 8: Transaction destructor logs swallowed `abandon` failures

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:110-118`

- [ ] **Step 1: Replace the silent catch** in `~ContentAddressedTransaction`:

```cpp
        try
        {
            st.build->abandon();
        }
        catch (...)
        {
            /// Destructor must not throw. But a failed abandon can leave a LIVE-epoch precommit
            /// binding that neither GC nor the (prior-epoch-scoped) stale sweep will reclaim until
            /// remount — that must be diagnosable, not silent.
            tryLogCurrentException(getLogger("ContentAddressedTransaction"),
                                   "abandoning a build during transaction destruction "
                                   "(a live precommit binding may persist until remount)");
        }
```

(Also fix the now-inaccurate comment above the loop claiming "lingering debris is GC-reclaimed".)

- [ ] **Step 2: Build; run the ContentAddressedTransaction gtests; commit** — `cas: transaction destructor logs failed abandon instead of silently swallowing`.

---

## Phase 3 — Diagnostics correctness

### Task 9: `system.content_addressed_mounts` — GC-health columns only on the local row

Covers report finding 5. Risk: **safe** (pre-release diagnostic table; makes wrong data absent instead of misleading).

**Files:**
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp` (columns description + row loop at 144-163)
- Test: extend the integration test that queries this table (`grep -rln content_addressed_mounts tests/integration/`)

- [ ] **Step 1: Make the four columns Nullable** in `getColumnsDescription` — `is_leader Nullable(UInt8)`, `pending_reclaim Nullable(Int64)`, `last_success_age_seconds Nullable(UInt64)`, `wedged_namespace_count Nullable(UInt64)` — and update their comments to say "NULL on rows describing OTHER servers' mounts; populated only on this server's own mount row".
- [ ] **Step 2: Scope the inserts:**

```cpp
        const auto health = ca->gcHealth();
        const String & local_srid = store->poolConfig().server_root_id;
        for (const auto & m : mounts)
        {
            ...
            /// GC health is a process-local fact about THIS server's scheduler. Stamping it onto
            /// peer rows misreads as "peer B is GC leader" during incidents — NULL there instead.
            const bool is_local_row = (m.srid == local_srid);
            if (is_local_row && health)
            {
                col_is_leader->insert(static_cast<UInt8>(health->is_leader));
                col_pending->insert(health->pending_reclaim);
                col_last_success->insert(health->last_success_age_seconds);
                col_wedged->insert(health->wedged_namespace_count);
            }
            else
            {
                col_is_leader->insertDefault();
                col_pending->insertDefault();
                col_last_success->insertDefault();
                col_wedged->insertDefault();
            }
        }
```

(`insertDefault` on a Nullable column inserts NULL. Column creation must go through the Nullable types from Step 1.)

- [ ] **Step 3: Extend the integration test:** on a 2-node pool, assert `SELECT count() FROM system.content_addressed_mounts WHERE is_leader IS NOT NULL` equals 1 per disk on each node, and the non-NULL row's `srid` equals that node's own `server_root_id`.
- [ ] **Step 4: Run; commit** — `cas: system.content_addressed_mounts scopes GC health to the local mount row`.

### Task 10: Disk-scoped logging in `CasGc`

Covers report minor (docs 45). Risk: **safe** (log text only).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h` (add `LoggerPtr log` member + constructor param, default `getLogger("CasGc")`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` (~10 sites: replace `getLogger("CasGc")` with `log`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:414` and any other `getLogger("CasGcFold")` sites the same way (thread the logger or include srid in the message)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp` — pass its already-disk-scoped logger (cf. `ContentAddressedMetadataStorage.cpp:244-245` naming) into `Gc`'s constructor; audit other `Gc` constructions (`CasFsck`, `CasInspect`, `runGcRebuildNow`) and pass a scoped name there too (e.g. `fmt::format("CasGc({})", config.server_root_id)`).

- [ ] **Step 1: Implement mechanically; grep the whole `Gc/` directory for `getLogger("` to catch every site.**
- [ ] **Step 2: Build; run GC gtests; eyeball one log line in a gtest run to confirm the scoped name.**
- [ ] **Step 3: Commit** — `cas: GC round-engine logs carry the disk/srid-scoped logger`.

### Task 11: Shape-validate `ManifestEntry.path` at decode

Covers report minor (security 15, defense-in-depth on the interserver channel). Risk: **low** (must not reject legitimate paths — projection subdirs are legal; validation is syntactic only).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp` (decode, `e.path = r.readString();` ~line 199)
- Test: the part-manifest formats gtest

- [ ] **Step 1: Failing tests:** decode manifests whose entry path is (a) `"../evil"`, (b) `"/abs"`, (c) `""`, (d) `"a//b"`, (e) `"a/./b"` — each must throw `CORRUPTED_DATA`; and a positive case `"proj.proj/data.bin"` must decode fine.
- [ ] **Step 2: Implement** right after reading `path`, before the existing ascending/no-duplicate checks:

```cpp
        /// Manifest bytes arrive over the interserver relink channel: enforce the same path hygiene
        /// as CasLayout::checkNamespace so no future consumer can inherit a traversal. Relative,
        /// no empty/'.'/'..' segments. (Syntactic only — legal projection subdirs pass.)
        if (e.path.empty() || e.path.front() == '/')
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS part manifest: invalid entry path '{}'", e.path);
        for (std::string_view rest = e.path; !rest.empty();)
        {
            const size_t slash = rest.find('/');
            const std::string_view seg = rest.substr(0, slash);
            if (seg.empty() || seg == "." || seg == "..")
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS part manifest: invalid entry path '{}'", e.path);
            rest = (slash == std::string_view::npos) ? std::string_view{} : rest.substr(slash + 1);
        }
```

- [ ] **Step 3: Run failing→passing; run the full formats suite AND one CA integration write/read test (guards against over-rejection); commit** — `cas: part-manifest decode rejects malformed entry paths (fail-close on the wire boundary)`.

### Task 12: Tighten `PartPathParser`'s Atomic-UUID anchor

Covers report minor (correctness 38). Risk: **low** (strictly narrows a heuristic; real Atomic layouts always pass the narrowed check).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.cpp:89-99`
- Test: the PartPathParser gtest (`grep -rln findTableUuidComponent src/Disks/tests/` or the CaPartPathParser suite)

- [ ] **Step 1: Failing test:** `data/abc/abcxyz/1_1_1_0/x.bin` (3-char db, table sharing the prefix) must NOT be classified via the Atomic-anchor branch (assert on the parse result the suite already checks — the non-Atomic fallback split). Positive control: `store/abc/abc12345-1234-5678-9abc-def012345678/all_1_1_0/x.bin` still anchors.
- [ ] **Step 2: Implement:**

```cpp
static bool isLowerHex(std::string_view s)
{
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

static bool looksLikeUuidDirName(std::string_view s)
{
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < 36; ++i)
    {
        const bool dash_pos = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash_pos != (s[i] == '-'))
            return false;
        if (!dash_pos && !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    }
    return true;
}

static std::optional<size_t> findTableUuidComponent(const std::vector<std::string> & p)
{
    for (size_t i = 1; i < p.size(); ++i)
    {
        const auto & prefix = p[i - 1];
        const auto & uuid = p[i];
        /// Shape-based on purpose (robust to a missing `store/`), but the shape is now the REAL
        /// Atomic one: 3 lowercase-hex chars followed by a well-formed UUID sharing that prefix —
        /// a 3-char database named like its table (`data/abc/abcxyz/...`) no longer false-anchors.
        if (prefix.size() == 3 && isLowerHex(prefix) && looksLikeUuidDirName(uuid)
            && uuid.compare(0, 3, prefix) == 0)
            return i;
    }
    return std::nullopt;
}
```

- [ ] **Step 3: Run failing→passing; run the FULL PartPathParser suite (it pins known ambiguities — none may regress); commit** — `cas: PartPathParser anchors only on real hex-prefix/UUID pairs`.

---

## Phase 4 — Mechanical refactors (zero behavior change)

### Task 13: One shared CA-disk detection helper (4 call sites)

Covers report minor (ockham 30) + the duplication in the mounts table. Risk: **safe** (pure refactor).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h/.cpp`
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (3 copies: ~1023-1039, ~2302-2318, ~2348-2364)
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp` (the equivalent inline block at ~98-118)

**Interfaces:**
- Produces: `static ContentAddressedMetadataStorage * ContentAddressedMetadataStorage::tryFromDisk(const DiskPtr & disk)` — returns nullptr for non-CA disks (treating `NOT_IMPLEMENTED` from `getMetadataStorage` as "not content-addressed"), rethrows anything else.

- [ ] **Step 1: Add the static helper** (body = the existing lambda verbatim, including the `NOT_IMPLEMENTED` comment). **Step 2: Replace all four inline copies with calls.** The mounts-table loop keeps its extra `continue`-on-null flow. **Step 3: Build; run parser+interpreter gtests + the mounts integration test; commit** — `cas: deduplicate content-addressed disk detection into tryFromDisk`.

### Task 14: Small hygiene batch

Covers report minors: dead includes (15), `renewOnce` invariant comment (35→doc), GC prune-floor nit (15). Risk: **safe**.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:3-4` — delete the two unused CAS includes (`ContentAddressedTransaction.h`, `PartPathParser.h`); full build must stay green.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:942-969` — add above `renewOnce`'s `lock_guard`:

```cpp
    /// INVARIANT: holding `state_mutex` across the PUT below is safe ONLY because (a) doTerminate
    /// joins the renewal thread before taking this mutex and (b) renewOnce has a single driver.
    /// Do NOT add new `state_mutex`-guarded accessors without revisiting this (they would stall for
    /// a full network round trip); the prepareRenew pattern above shows the lock-free alternative.
```

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` (~404-406 / 610-625): capture `const uint64_t prune_floor_generation = state.snap_generation;` **before** `fold()` advances it, and pass that to `pruneSupersededGenerations` so a lost `gc/state` CAS never prunes one generation deeper than `gc_snap_generations_to_keep` promises. Run the GC gtests.

- [ ] **Step 1: Apply all three; build; run `Cas*:CA*:Ref*` gate; commit** — `cas: hygiene — dead includes, renewOnce lock invariant note, pre-fold prune floor`.

### Task 15: Route `prepareRead` CA hooks through `IContentAddressedExchange`

Covers report finding 7. Risk: **low** (mechanical seam change; same calls, same order — only the cast target changes).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h` — add the two pure-virtual hooks with the exact signatures currently on the concrete class (`prepareInManifestRead`, `getBlobViewPlan`; copy const-ness/return types from `ContentAddressedMetadataStorage.h`).
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` — mark the two methods `override`.
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:824-833` — `dynamic_cast<const IContentAddressedExchange *>` instead of the concrete class; body otherwise unchanged.

- [ ] **Step 1: Implement; build; run the read-path gtests + one CA stateless read test; verify `git grep -n 'dynamic_cast<.*ContentAddressedMetadataStorage' src/Disks/DiskObjectStorage/DiskObjectStorage.cpp` is empty.**
- [ ] **Step 2: Commit** — `cas: prepareRead casts to the narrow IContentAddressedExchange seam, not the concrete class`.

### Task 16: `hasAnyRefWithPrefix` for pure existence probes

Covers report minor (performance 35). Risk: **low** (same lock, same recovery preamble as `listRefs`; early-exit instead of full materialization).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h/.cpp` (next to `listRefs`, ~line 168)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h/.cpp` (forwarder next to the `listRefs` forwarder)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:879,893,901` (`ShadowTable`, `ShadowIntermediate`, `TableDir` emptiness probes) and the `DetachedContainer`/`MovingContainer` cases (via prefix)
- Test: the ledger gtest

**Interfaces:**
- Produces: `bool CasRefLedger::hasAnyRefWithPrefix(const RootNamespace & ns, std::string_view prefix)` (empty prefix = "any ref at all"); `bool Pool::hasAnyRefWithPrefix(...)` forwarder.

- [ ] **Step 1: Failing test:** namespace with refs {`all_1_1_0`, `detached-x`} → `hasAnyRefWithPrefix(ns, "")` true, `(ns, "detached-")` true, `(ns, "moving-")` false; empty namespace → false; tombstoned-only namespace → false (reuse the fixture's drop helpers).
- [ ] **Step 2: Implement** (same maintenance preamble as `listRefs`, no map materialization):

```cpp
bool CasRefLedger::hasAnyRefWithPrefix(const RootNamespace & ns, std::string_view prefix)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::lock_guard lock(rt->state_mutex);
    for (const auto [ref_name, row] : rt->state.committed)
        if (prefix.empty() || std::string_view(ref_name).starts_with(prefix))
            return true;
    return false;
}
```

(Early exit makes the dominant `prefix.empty()` case O(1); prefixed cases stay a no-allocation scan. Keep `listRefs` untouched.)

- [ ] **Step 3: Swap the five call sites** (`!store()->listRefs(...).empty()` → `store()->hasAnyRefWithPrefix(..., "")`; `!detachedRefNames(...).empty()` in the `DetachedContainer` case → `hasAnyRefWithPrefix(ns, Cas::kDetachedRefPrefix)`, same for moving). Leave `detachedRefNames`/`movingRefNames` themselves (they need the names).
- [ ] **Step 4: Run failing→passing; run the existsDirectory-related gtests; commit** — `cas: existence probes use hasAnyRefWithPrefix instead of materializing listRefs`.

---

## Phase 5 — Tests-only (cover findings with zero product risk)

### Task 17: Access-control stateless tests for the new SYSTEM verbs

Covers report finding 3(c) + tests minor 35. `checkAccess` runs before disk resolution, so **no CA disk is needed** for the denial paths.

**Files:**
- Modify: `tests/queries/0_stateless/05011_cas_gc_rebuild_access.sh` — add: user with zero grants gets `ACCESS_DENIED` on plain `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION 'no_such_disk'` (denial fires before `UNKNOWN_DISK`).
- Create (via `add-test`): `tests/queries/0_stateless/<NNNNN>_cas_drop_pool_member_access.sh` — user with zero grants → `ACCESS_DENIED` on `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'x' FROM DISK 'y'`; after `GRANT SYSTEM CONTENT ADDRESSED DROP POOL MEMBER ON *.*` → the same query now fails with `UNKNOWN_DISK` (proving the grant, and only that grant, unlocks it).

- [ ] **Step 1: Write both, model the user-create/cleanup scaffolding on `05011`. Step 2: Run via praktika stateless job, logs to `<build_dir>/test_task17.log`. Step 3: Commit** — `cas: access-control negative tests for GC and DROP POOL MEMBER verbs`.

### Task 18: Mock-S3 unit tests for the conditional-write primitives

Covers report tests findings 45/50. Tests-only; extends the existing `MockS3::Client`.

**Files:**
- Modify: `src/IO/tests/gtest_writebuffer_s3.cpp` (or a new `src/IO/tests/gtest_s3_conditional_ops.cpp` reusing its mock): add `CopyObject`, `DeleteObject`, `GetBucketVersioning` handlers with injectable outcomes (success / 412 / 404 / AccessDenied).
- Tests to add:
  1. `S3ObjectStorage::removeObjectIfTokenMatches` → injected 412 ⇒ returns `TokenMismatch`; 404 ⇒ `NotFound`; success ⇒ removed.
  2. `S3ObjectStorage::copyObjectConditional` → 412 ⇒ `PreconditionFailed` result, success ⇒ ok.
  3. `copyS3File` with `if_none_match` set: injected multipart-copy `AccessDenied` ⇒ exception propagates (the fallback-to-unconditional-copy branch at `copyS3File.cpp:747-753`/`816-819` must NOT run — assert no `PutObject` upload happened); losing conditional copy surfaces `S3Exception::isPreconditionFailed()`.

- [ ] **Step 1: Write tests failing on handler-absence, wire handlers, drive through `S3ObjectStorage`/`copyS3File` the way `WBS3Test` constructs its client. Step 2: Run `--gtest_filter='*Conditional*:*WBS3*'`. Step 3: Commit** — `cas: mock-S3 unit tests for conditional remove/copy and copyS3File fallback-disable`.

### Task 19: `MergeTreeDeduplicationLog` null-writer regression test

Covers report tests minor 40 (B37 fix shipped untested).

**Files:**
- Create: a stateless test (via `add-test`) that sets `non_replicated_deduplication_window` on a table whose disk leaves `current_writer` null (reuse the disk from `03711_merge_tree_deduplication_with_disk_not_support_writing_with_append.sql` but drive an actual `addPart`-reaching INSERT), asserting the server returns the new exception and stays alive (follow-up `SELECT 1`).

- [ ] **Step 1: Reproduce which disk/scenario reaches the throw (read `MergeTreeDeduplicationLog.cpp:275-290` guards first; if no reachable non-CA scenario exists, convert to a gtest constructing the log directly with a null writer). Step 2: Test → run → commit** — `tests: dedup-log null-writer fails with an exception, not a crash (B37 regression)`.

---

## Phase 6 — Strings and docs

### Task 20: `Cas*` ProfileEvents description sweep

Covers report docs finding 4 (55). Risk: **safe** (strings only). Mechanical — good codex-delegation candidate.

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (the 129-entry `Cas*` block, ~lines 858-984)

- [ ] **Step 1: Rewrite every description to operator-facing content only** — what it counts, when it fires, what a growing rate implies. **Delete all internal-process citations**: `(closes artifact #N)`, `(Round-B §N)`, `(rev.N §...)`, `RFC cas-*` references, review-history narration. Keep genuinely actionable notes (e.g. `CasConditionalWriteSdkRetries` "must stay zero" tripwire). Target ≤ 200 chars each; move any surviving "why" narrative into a `///` comment above the `M(...)` line.
- [ ] **Step 2: Verify:** `grep -nE 'artifact #|Round-B|rev\.[0-9]+ §|closes ' src/Common/ProfileEvents.cpp` returns nothing; build; commit — `cas: ProfileEvents descriptions rewritten for operators (internal citations removed)`.

### Task 21: Docs sync for the shipped SQL/table/config surface

Covers report docs findings 1-3. Risk: **safe** (docs only). Respect frontmatter + `{#anchor}` rules.

**Files:**
- Modify: `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` — regenerate the Columns section from `ContentAddressedGarbageCollectionLogElement::getColumnsDescription()` (drop the nonexistent `children_cascaded`; add `manifests_deleted`, `entries_condemned`, `entries_graduated`, `entries_redeleted`, `fence_outs`, `anomalies` with the per-column comments from the source).
- Create: `docs/en/operations/system-tables/content_addressed_mounts.md` and `content_addressed_log.md` (columns from the two `getColumnsDescription` implementations; model frontmatter on the GC-log doc; mounts doc must state the Task-9 local-row NULL semantics).
- Modify: `docs/en/sql-reference/statements/system.md` — add `### SYSTEM CONTENT ADDRESSED GC REBUILD {#system-content-addressed-gc-rebuild}` and `### SYSTEM CONTENT ADDRESSED DROP POOL MEMBER {#system-content-addressed-drop-pool-member}` next to the existing GC section (syntax from `ParserSystemQuery.cpp:479/491`, FORCE semantics + destructive warnings from the interpreter comments).
- Modify: `docs/en/operations/storing-data.md` — add `content_addressed` to the `metadata_type` list and a subsection with a config example listing all keys parsed in `MetadataStorageFactory.cpp` (lift each knob's explanation from its parse-site comment).

- [ ] **Step 1: Write all four; cross-check every column/key name against the source (not the report). Step 2: Commit** — `docs: sync CAS system tables, SYSTEM verbs, and disk config keys with src`.

---

## Phase 7 — Opt-in medium-risk perf fixes (each independently droppable)

### Task 22: Per-transaction ForceFresh memoization in `unlinkFile`

Covers report finding 8. Risk: **medium** — skips repeat HEADs *within one disk transaction*. Justification: the transaction validated the same ref moments earlier; nothing outside the transaction can legitimately repoint it mid-removal, and the subsequent `removeDirectory` ref-drop is conditional anyway.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h` (add member `std::unordered_set<String> force_fresh_validated_refs;`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:1387-1411` (`unlinkFile`)
- Test: gtest with the instrumented backend counting HEADs

- [ ] **Step 1: Failing test:** through one `ContentAddressedTransaction`, unlink N files of one part (in-memory/instrumented backend counting `head` calls), assert exactly **one** manifest-body HEAD for the ref, not N. (Today: N.)
- [ ] **Step 2: Implement** — read `unlinkFile` first; at the `getView(r->refKey(), Cas::Freshness::ForceFresh)` call:

```cpp
        /// One mandatory body-HEAD per (transaction, ref), not per file: the MergeTree fast-removal
        /// path unlinks every file of the part through THIS transaction right before removeDirectory.
        /// The first unlink re-proves the body ForceFresh; the rest of the burst reuses that proof.
        const String memo_key = r->refKey().cacheKey();
        const bool already_proven = force_fresh_validated_refs.contains(memo_key);
        auto view = metadata_storage.partAccess()->getView(
            r->refKey(), already_proven ? Cas::Freshness::CachedForLoad : Cas::Freshness::ForceFresh);
        if (view && !already_proven)
            force_fresh_validated_refs.insert(memo_key);
```

  Clear the set in `commit()`'s epilogue alongside the other per-transaction state resets.
- [ ] **Step 3: Run failing→passing; run the FULL removal-path gtests + one CA stateless merge/drop test; commit** — `cas: one ForceFresh body proof per (transaction, ref) on the fast-removal path`.

### Task 23: Emit `RefResolve` audit events only when the resolve does real work

Covers report finding 9. Risk: **medium** — changes audit-log volume/semantics (warm-cache reads stop producing rows). Justification: every other `CasEvent` site is mutation-bounded; this is the only read-hot-path emitter, and a warm-hit row carries no new information.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:122-166` (`resolveRef`) and its declaration — add `enum class ResolveAudit : uint8_t { Emit, Deferred };` parameter, default `Emit` (all existing callers keep today's behavior).
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp` (`resolve`/`getView` at 156-200): `resolve` passes `ResolveAudit::Deferred`; `getView` emits the identical event itself (same fields, from the `Resolved` it holds) on every path EXCEPT the warm `CachedForLoad` hit-with-matching-manifest return.
- Test: gtest with a counting event sink.

- [ ] **Step 1: Failing test:** counting sink; first `getView(CachedForLoad)` (cold) → 1 `RefResolve` event; second identical call (warm hit) → still 1 total (today: 2). A `ForceFresh` call → +1.
- [ ] **Step 2: Implement; make sure `listRefs`/drop/GC callers of `resolveRef` are untouched (default arg).**
- [ ] **Step 3: Run failing→passing; run the full `Cas*:CA*:Ref*` gate; grep the audit-log integration test expectations for RefResolve counts and update if any pin exact numbers; commit** — `cas: RefResolve audit fires on real resolve work, not on warm view-cache hits`.

---

## Self-review checklist (done at plan-writing time)

- Coverage vs report: findings 2, 3, 4, 5, 7, 8, 9, 10 have tasks; finding 1 and 6 deferred by user instruction / existing plan; finding 11 is upstream-process, not code. Minors covered: interpreter lambda (T13), dead includes + renewOnce + prune floor (T14), outcomes width (T5), PartPathParser (T12), manifest-path validation (T11), listRefs emptiness (T16), logger scoping (T10), ProfileEvents (T20), docs (T21), access tests (T17), mock-S3/copyS3File tests (T18), B37 test (T19). Rejected/deferred minors listed in the deferred table.
- Type consistency: `checkNotReadOnly` (T1) used only in files T1 touches; `tryFromDisk` (T13) matches the lambda's exact semantics; `hasAnyRefWithPrefix` signature consistent between T16's ledger/pool/call-site steps.
- Every code step shows the code; steps referencing existing fixtures name the grep to locate them.
