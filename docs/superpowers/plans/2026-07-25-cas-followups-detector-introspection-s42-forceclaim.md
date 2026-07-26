# CAS follow-ups: skipped-transaction detector, per-phase GC rows, S42 verdict, mount force-claim — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **ROUND STATUS 2026-07-26 — CLOSED except force-claim.** Tasks 1-9 DONE: the S42 verdict rework, the
> skipped-transaction detector (probes A/B1/B2), and the per-phase GC log rows with their documentation.
> The detector has since FIRED 14 times in a live soak and that investigation carries into the GC round.
> **Tasks 10-12 (force-claim) NOT STARTED, deliberately** — blocked on a user decision and stripped of
> their original motivation when the one-line CI fix landed.

**Goal:** Land the four decided follow-ups from BACKLOG {#user-decisions-2026-07-25} and
{#list-as-journal-decision-c} as one batch, after the Part B codex review and before the Part B soak.

**Architecture:** (1) A mechanism-agnostic detector for "the fold cursor advanced past a transaction
that was never applied", built from two probes — a set comparison between the two full `cas/refs/`
enumerations a round *already* performs, and a per-transaction apply ledger carried through the fold
into the shard reducers. (2) One row per GC phase in
`system.content_addressed_garbage_collection_log`, correlated by a new `round_id`, carrying a
microsecond duration, a per-phase `ProfileEvents` delta, and a phase-specific metrics map. (3) The S42
scenario card's verdict rests on its consistency assertions; the targeted window counters become
reported, the generic anti-vacuity guard survives. (4) An opt-in mount-time force-claim that overrides
a differing server uuid and mounts as WRITE, gated on a certificate of death for the mount slot.

**Tech Stack:** C++ (ClickHouse fork), gtest (`unit_tests_dbms`), the stateless suite
(`tests/queries/0_stateless`), Python (`utils/ca-soak/scenarios`), ClickHouse system-table docs.

**Spec:** `docs/superpowers/specs/2026-07-25-cas-gc-observability-and-mount-force-design.md`.

## EXECUTION STATUS (as of 2026-07-26) {#execution-status}

- **Tasks 1-4 — DONE**, one commit: `e01b5cd82be`, gate 1382/1382 (1373 + 2 detector + 7 ledger).
- **Tasks 5-8 — IN PROGRESS.** Dispatched at 00:09 UTC (worklog `75a03e26ffb`); no commit yet, so
  nothing in those tasks is ticked and nothing in them should be trusted as landed.
- **Task 9 — DONE**: `402a85c4a64`.
- **Tasks 10-12 — BLOCKED** on the user decision recorded at BACKLOG {#operator-uuid-recovery}: which
  reading of "force a new uuid" — overwrite the owner uuid (what these tasks implement) or adopt the
  pool's existing one (`Pool::openForDecommission` already does this, with no identity damage). That is
  also escalation item 1 in {#escalate}. Their CI motivation has already evaporated
  (BACKLOG {#ci-scrape-readonly-sed-fix}). Do not start them.

## SIZING WARNING — read before starting {#sizing-warning}

Three of the four items are contained. **Task 4 (probe B2) is not instrumentation** — it adds a field
to `BlobDelta`, the fold's hot per-edge row, and threads an output vector into
`foldDeltasIntoGeneration` / `ShardReducer::reduce`. Review it as a hot-path change. If Step 1 of Task
4 shows the struct grows, stop and report before continuing.

> **OUTCOME:** it did NOT grow — 64 bytes before and after, the ordinal landing in existing tail
> padding. That is now pinned by `kBlobDeltaSize` + a `static_assert` in `CasBlobInDegree.h`, so a
> future field that breaks it has to argue itself as a hot-path change.

**Task 1 must go RED.** It is written before any fix and its whole value is that it fails against the
shipped code. If it passes on the first run, that is a finding: the assumed mechanism does not reach
the code by the assumed route. Report it and stop — do not adjust the test until it goes red.

> **OUTCOME:** exactly as predicted in Task 1 Step 8 — the retention test GREEN (the omitted `-1` is
> skipped forever), the mirror test RED ("GC deleted a blob that manifest 1:2:1 still references"). The
> data-loss class is now executable. The mirror test SHIPS ENABLED, not pinned as an expected failure:
> a red nobody can act on is worse than no test, so Task 2 turns it green and it additionally asserts
> the detector fired, so an inert probe cannot satisfy it.

## Global Constraints

- **Branch discipline:** work on `cas-gc-rebuild`. Never rebase, never amend — add new commits.
- **Shared checkout:** other sessions commit to this same working tree. **Always commit with a
  pathspec** — `git commit -F <msgfile> -- <paths>` — and check `git diff --cached --stat` for foreign
  staged content first. Verify `git show --stat HEAD` after every commit.
- **Allman braces**; CAS naming conventions as in the surrounding code.
- **No `sleep` in C++ to fix races.**
- **Pre-release, zero compat scaffolding:** no migration shims for on-disk formats. Nothing in this
  plan changes a durable format — the apply ledger is round-local and never persisted.
- **Avoid fallback paths.** Where an operation cannot prove its precondition, surface the error; never
  substitute a default.
- **Errors:** `LOGICAL_ERROR` only for genuine programming invariants (it aborts under sanitizers).
  Use `CORRUPTED_DATA` / `ABORTED` per the existing CAS conventions.
- **Docs:** every heading under `docs/` needs an explicit `{#kebab-case-anchor}`; new files need the
  frontmatter block.
- **Build:** `ninja -C build unit_tests_dbms > build/build_<task>.log 2>&1; echo NINJA_EXIT=$?`. Never
  pass `-j`, never use `nproc`. Have a subagent summarise the log.
- **gtest gate** after every task that touches C++ — the corrected filter from BACKLOG
  {#gate-filter-gap-3-backend-contract}:

  ```
  build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*'
  ```

  Zero failures. Redirect to `build/test_<task>.log` and have a subagent summarise.

## File Structure

All CAS paths are relative to
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.

**Item 1 — detector:**
- `Gc/CasGc.h` — `RefScanSummary`, `TxnApplyLedger`, `preFoldRefScan` replacing `changedShardCount`,
  the `fold` signature gaining the pre-fold scan.
- `Gc/CasGc.cpp` — probe A comparison, probe B1 identity, probe B2 ledger wiring, the throw.
- `Gc/CasBlobInDegree.h/.cpp` — `BlobDelta::txn_ordinal`, the `out_applied_by_txn_ordinal` out-param.
- `Gc/CasGcShardPlan.h/.cpp` — forward the new out-param.
- `src/Disks/tests/gtest_cas_holey_list_detector.cpp` — NEW: the holey-`LIST` backend and both mirror
  tests.

**Item 2 — per-phase rows:**
- `Gc/CasGcScheduler.h/.cpp` — `GcPhaseRecord`, `GcPhaseSink`, `round_id` mint, `phase` on
  `GcRoundLogRecord`.
- `Gc/CasGcPhaseTimer.h` — NEW: the RAII timer (header-only).
- `Gc/CasGc.h/.cpp` — the phase sink member and the per-phase scopes.
- `src/Interpreters/ContentAddressedGarbageCollectionLog.h/.cpp` — schema.
- `ContentAddressedMetadataStorage.cpp` — the converter.
- `src/Disks/tests/gtest_cas_gc_log.cpp` — extend.
- `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` — update.
- `tests/queries/0_stateless/05007_content_addressed_gc_introspection.sh` — extend.

**Item 3 — S42:**
- `utils/ca-soak/scenarios/framework/report.py` — `Verdict.reported`.
- `utils/ca-soak/scenarios/cards/s42_alloc_faults.py` — verdict rework + docstring.

**Item 4 — force-claim:**
- `Pool/CasServerRoot.h/.cpp` — `ForeignMountDeath`, `proveForeignMountDead`.
- `Pool/CasPool.h/.cpp` — `PoolConfig::force_owner_claim`, the force path in `mountWritable`.
- `ContentAddressedSettings.cpp` — the `force_owner_claim` setting.
- `ContentAddressedMetadataStorage.h/.cpp` — thread it into `PoolConfig`.
- `src/Disks/tests/gtest_cas_force_owner_claim.cpp` — NEW.
- `docs/en/engines/table-engines/mergetree-family/content-addressed.md` (or the CA disk settings doc
  the repo already uses) — document the setting.

---

## Task 1: The mirror safety test — make the deletion path executable (EXPECT RED)

**STATUS: DONE** — `e01b5cd82be`.

**Corrections to this task's own text. Three of them would have made the sabotage a silent no-op, i.e.
a test that passes while proving nothing — read them before reusing any of the code below.**

1. **"the greatest new key = M2's own activation transaction" (Step 5) is FALSE.** One logical publish
   appends SEVERAL ref-log transactions: `precommitAdd`, then `promote`. It is the **`precommitAdd`**
   that carries the `+1` activation; the promote is an owner move at the same `manifest_ref` and emits
   **no edge at all**. Taking `added.back()` therefore omits the WRONG object and the hole falls on a
   record whose absence changes nothing. The landed test selects the key by **decoding** the candidate
   objects and finding the one that emits an edge of the wanted sign for the wanted `ManifestId`
   (`refLogKeyEmittingEdge`), and asserts there is exactly one.
2. **Do not mix the raw `dropRefTransition` / `publishCommittedTransition` helpers with the real writer
   on the same namespace — they COLLIDE.** The raw helper allocates its sequence by LISTING, while the
   ledger allocates from its own in-memory sequence, so the two disagree as soon as the same namespace
   is written through the writer again (which Step 4 does, with `part_h`). Both tests use the real
   writer API: `s->dropRef(ns, ...)`.
3. **Arm the sabotage LAST, after every seeding write.** `omitFromNthListCall` counts qualifying `list`
   calls, and the WRITER'S OWN sequence allocation lists the namespace prefix — so arming before a
   seeding write silently shifts `nth` and the hole lands on the wrong call.

Also needed and not in the text below:
- `s->renewWatermarkOnce()` after the drop, so the dropped closure is not spared as in-flight;
- an assertion that M1's manifest BODY is still present, so its `-1` edges are readable at
  removal-fold;
- the counter read is the PROCESS-GLOBAL `ProfileEvents::global_counters` with a delta, not
  `CurrentThread::getProfileEvents()` — a bare gtest thread has no attached `ThreadStatus` (Task 2
  Step 6 offers this as a fallback; it is the operative form, not the fallback).
- Step 5's closing paragraph refers to "the `appendRefLogSeed` probe trick above", which appears
  nowhere in this task — stale text from a superseded draft. The operative instruction is the
  diff-the-ref-prefix-and-decode one.
- Step 2's comment names `Gc::preFoldRefScan`; at Task 1 time that function is still
  `Gc::changedShardCount` (Task 2 renames it). Harmless, but it will not grep.
- One property worth keeping, established while writing the backend: erasing a key from a page never
  disturbs pagination, because `ListPage::next_cursor` is the last key the underlying backend returned
  and is computed BEFORE the erase.

Pins both directions of the skipped-transaction defect against the SHIPPED code, before any fix.
Converts the TLA+ `_sab_holeylist` result into an executable one.

**Files:**
- Create: `src/Disks/tests/gtest_cas_holey_list_detector.cpp`

**Interfaces:**
- Consumes, from `src/Disks/tests/cas_test_helpers.h` (verified present, exact signatures):
  `DB::Cas::BlobRef idOf(const String &)`, `DB::UInt128 u128Of(const String &)`,
  `uint64_t publishCommittedTransition(Backend &, const Layout &, const RootNamespace &, const String & ref_name, std::optional<ManifestRef> old_ref, const ManifestRef & new_ref, uint64_t shard = 0)`
  — returns the allocated `ref_sequence`, and the txn id is `RefTxnId{/*writer_epoch=*/1, ref_sequence}`;
  ~~`uint64_t dropRefTransition(Backend &, const Layout &, const RootNamespace &, const String & ref_name, const ManifestRef & old_ref, uint64_t shard = 0)`~~
  **— NOT USED. Neither raw helper may be mixed with the real writer on the same namespace: they
  allocate sequences by LISTING while the ledger allocates from its own in-memory counter, so the two
  collide (correction 2 above). Use `Pool::dropRef` and find the emitted key by decoding.**
- Consumes, from `Backend/CasInMemoryBackend.h`: `class InMemoryBackend : public Backend` with
  `ListPage list(const String &, const String &, size_t) override` (virtual, overridable).
- Produces: `class HoleyListBackend` — used by no later task, but Task 2 flips both tests to green.

- [x] **Step 1: Read the fixture this test is modelled on**

Read `src/Disks/tests/gtest_cas_gc_leak.cpp` lines 1-200. The pieces reused verbatim below are
`openTestPool`, `blobEntry`, `publishOneBlobPart`, `runGcToFixpoint`, `anyRetiredPending` and
`blobPresent`. Do not invent a new fixture shape.

- [x] **Step 2: Write the holey-`LIST` backend**

Create `src/Disks/tests/gtest_cas_holey_list_detector.cpp` with:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <atomic>
#include <string>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

/// A backend that drops ONE chosen key from ONE chosen `list` call, while leaving exact `get`/`head`
/// of that key working. This is the minimal realisation of "the store returned an incomplete answer":
/// the record is durable and readable, it is simply absent from one enumeration. The mechanism is
/// deliberately NOT modelled (no page split, no cursor games) — the detector under test must not
/// depend on how the hole was produced.
///
/// WHICH call is explicit and load-bearing. A GC round enumerates the ref prefix TWICE:
/// `Gc::preFoldRefScan` (the defer signal) and then `Gc::fold`. Only a hole in the FOLD's walk makes
/// the fold skip the record, which is the defect being reproduced; a hole in the pre-fold walk alone
/// changes nothing today. `nth` counts, from the moment `omitFromNthListCall` is called, only those
/// `list` calls that WOULD have returned the key — so unrelated prefix enumerations do not shift it.
class HoleyListBackend : public InMemoryBackend
{
public:
    /// Omit `key` from the `nth` (0-based) subsequent qualifying `list` call. Resets the counter.
    void omitFromNthListCall(const String & key, size_t nth)
    {
        std::lock_guard lock(m);
        omitted = key;
        target_call = nth;
        seen_calls = 0;
        served = false;
    }

    /// Whether the hole was actually served. Every test asserts this, so a mis-typed key or a
    /// miscounted `nth` cannot let a test pass vacuously.
    bool holeServed() const
    {
        std::lock_guard lock(m);
        return served;
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = InMemoryBackend::list(prefix, cursor, limit);
        std::lock_guard lock(m);
        if (omitted.empty())
            return page;
        auto it = std::find_if(page.keys.begin(), page.keys.end(),
                               [&](const ListedKey & k) { return k.key == omitted; });
        if (it == page.keys.end())
            return page;              /// not a qualifying call — do not count it
        if (seen_calls++ != target_call)
            return page;
        page.keys.erase(it);
        served = true;
        omitted.clear();              /// one hole only
        return page;
    }

private:
    mutable std::mutex m;
    String omitted;
    size_t target_call = 0;
    size_t seen_calls = 0;
    bool served = false;
};

}
```

- [x] **Step 3: Write the shared fixture helpers**

Append to the same file, inside the same anonymous namespace (copied from `gtest_cas_gc_leak.cpp`,
which is the established shape for this fixture — do not re-derive it):

```cpp
PoolPtr openHoleyPool(std::shared_ptr<HoleyListBackend> & out_backend)
{
    out_backend = std::make_shared<HoleyListBackend>();
    return Pool::open(out_backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

ManifestEntry blobEntry(const String & path, const String & payload)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Blob;
    e.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of(payload))};
    e.blob_size = payload.size();
    return e;
}

/// Publish one single-blob part through the REAL writer sequence and return its ManifestId.
ManifestId publishOneBlobPart(const PoolPtr & s, const RootNamespace & ns, const String & ref,
                              const String & payload)
{
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = s->beginPartWrite(info);
    const ManifestId id = build->stageManifest({blobEntry("data.bin", payload)});
    build->precommitAdd(ns, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

bool blobPresent(const std::shared_ptr<HoleyListBackend> & b, const Layout & layout, const String & payload)
{
    return b->head(layout.blobKey(BlobRef{BlobHashAlgo::CityHash128,
                                          BlobDigest::fromU128(u128Of(payload))})).exists;
}

/// UNUSED in the landed test — the raw ref-log helpers are not used at all (correction 2), so nothing
/// needs to reconstruct a txn id from a sequence.
///   RefTxnId txnIdOfSeq(uint64_t seq) { return RefTxnId{/*writer_epoch=*/1, /*ref_sequence=*/seq}; }

/// Every ref object key of one namespace. Used to identify WHICH object a publish appended, rather
/// than guessing a sequence number.
std::set<String> listRefKeys(Backend & b, const Layout & layout, const RootNamespace & ns)
{
    std::set<String> keys;
    forEachListedKey(b, layout.refsNamespacePrefix(ns), [&](const ListedKey & k) { keys.insert(k.key); });
    return keys;
}

/// The keys present in `after` and not in `before`.
std::vector<String> addedKeys(const std::set<String> & before, const std::set<String> & after)
{
    std::vector<String> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    return added;
}

/// Among `candidates`, the ONE ref-log key whose transaction emits an edge of sign `change` naming
/// `manifest_id`. THIS, not "the greatest new key", is how the sabotage target is chosen — see
/// correction 1 at the top of this task. `EXPECT_EQ(hits.size(), 1u)` inside is load-bearing: an
/// ambiguous match means the seeding changed and the hole would land somewhere unintended.
String refLogKeyEmittingEdge(Backend & b, const Layout & layout, const RootNamespace & ns,
                             const std::vector<String> & candidates, const ManifestId & manifest_id,
                             int change);   /// decode each candidate with `decodeRefLogTxn` and scan
                                            /// `manifestEdgesOfTxn`; see the landed file for the body.

void runRounds(const PoolPtr & s, Gc & gc, int rounds)
{
    for (int i = 0; i < rounds; ++i)
    {
        gc.runRegularRound();
        s->renewWatermarkOnce();
    }
}
```

- [x] **Step 4: Write the retention-direction test**

Append to the same file. It publishes a part, drops its ref, adds a later harmless record, omits the
DROP record from ~~the first~~ **the FOLD's** ref-prefix `LIST` (`nth = 1` — the round's SECOND
qualifying walk; a hole in the pre-fold walk alone changes nothing, as the code below says), and
asserts the shipped behaviour: the cursor advances past the drop, so the `+1` survives and the blob is
never reclaimed even after the listing recovers. (Task 2 Step 6 then flips this final assertion, since
probe A stops the cursor advancing.)

```cpp
/// RETENTION DIRECTION (the RCA's primary reproduction). A ref-log record omitted from ONE listing
/// sorts at or below the cursor forever, so restoring the listing cannot recover it: the blob's `-1`
/// never folds and the blob is retained permanently.
TEST(CasHoleyListDetector, OmittedRemoveRecordIsSkippedForever)
{
    std::shared_ptr<HoleyListBackend> b;
    auto s = openHoleyPool(b);
    const Layout & layout = s->layout();
    const RootNamespace ns{"test/tbl"};
    const String payload = "holey-payload";

    /// A: publish the part (its +1 edges). Folded by the first round below.
    const ManifestId part = publishOneBlobPart(s, ns, "part_a", payload);
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    runRounds(s, gc, 2);
    ASSERT_TRUE(blobPresent(b, layout, payload));

    /// R: drop the ref (the -1). H: a later, unrelated record so the cursor has a reason to advance
    /// past R even when R is not returned.
    ///
    /// WRONG — see correction 2 at the top of this task. `dropRefTransition` allocates its sequence by
    /// LISTING and collides with the ledger's own in-memory sequence the moment `part_h` below is
    /// written through the writer. Use `s->dropRef(ns, "part_a")` and find the emitted key by decoding.
    ///   const uint64_t remove_seq = DB::Cas::tests::dropRefTransition(*b, layout, ns, "part_a", part.ref);
    const std::set<String> before_drop = listRefKeys(*b, layout, ns);
    s->dropRef(ns, "part_a");
    const std::set<String> after_drop = listRefKeys(*b, layout, ns);
    const String remove_key =
        refLogKeyEmittingEdge(*b, layout, ns, addedKeys(before_drop, after_drop), part, -1);

    const ManifestId other = publishOneBlobPart(s, ns, "part_h", "harmless-payload");
    (void)other;
    s->renewWatermarkOnce();   /// advance the floor so the dropped closure is not spared as in-flight

    /// nth = 1: the round's SECOND qualifying walk is `Gc::fold`'s own enumeration (the first is
    /// `preFoldRefScan`/`changedShardCount`). Only a hole there makes the fold skip the record.
    /// ARMED LAST — see correction 3: a writer-side namespace listing would otherwise consume a
    /// qualifying call and shift `nth`.
    b->omitFromNthListCall(remove_key, /*nth=*/1);

    runRounds(s, gc, 1);
    ASSERT_TRUE(b->holeServed()) << "the sabotage never fired — the omitted key was never listed";

    /// The listing is whole again from here on. Drive to a fixpoint; the removal can never be folded.
    runRounds(s, gc, 12);

    EXPECT_TRUE(blobPresent(b, layout, payload))
        << "the blob WAS reclaimed — the omitted removal record was somehow re-folded, so the "
           "permanent-skip mechanism does not reach the shipped code by the assumed route";
}
```

- [x] **Step 5: Write the deletion-direction (mirror) test**

Append to the same file. This is the one that matters: it asserts a LIVE blob is never deleted.

```cpp
/// DELETION DIRECTION (the mirror safety test from the RCA). Two owners share ONE deduplicated blob.
/// The SECOND owner's `+1` is omitted from one listing while the FIRST owner's `-1` folds normally,
/// so GC sees zero edges for a blob a live manifest still references. THIS MUST NEVER DELETE THE BLOB.
TEST(CasHoleyListDetector, OmittedActivationNeverPermitsDeletingALiveBlob)
{
    std::shared_ptr<HoleyListBackend> b;
    auto s = openHoleyPool(b);
    const Layout & layout = s->layout();
    const RootNamespace ns{"test/tbl"};
    const String payload = "shared-payload";

    /// M1 owns the token. Fold it so its +1 is durable in the in-degree generation.
    const ManifestId m1 = publishOneBlobPart(s, ns, "part_1", payload);
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    runRounds(s, gc, 2);
    ASSERT_TRUE(blobPresent(b, layout, payload));

    /// M2 adopts the SAME deduplicated blob (`putBlob` of an identical payload dedups). Learn WHICH
    /// ref-log object carries M2's activation by diffing the namespace's ref prefix around the
    /// publish — do NOT guess a sequence number, and do not append a probe transaction (that would
    /// perturb the very stream under test).
    const std::set<String> before = listRefKeys(*b, layout, ns);
    const ManifestId m2 = publishOneBlobPart(s, ns, "part_2", payload);
    const std::set<String> after = listRefKeys(*b, layout, ns);
    std::vector<String> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    ASSERT_FALSE(added.empty()) << "M2's publish appended no ref object";

    /// WRONG — see correction 1 at the top of this task. A publish appends `precommitAdd` AND
    /// `promote`; the `precommitAdd` carries the `+1` and the promote emits NO edge, so the greatest
    /// new key is the promote and omitting it changes nothing. Select by DECODING instead.
    ///   const String m2_key = added.back();   /// the greatest new key = M2's own activation transaction
    const String m2_key = refLogKeyEmittingEdge(*b, layout, ns, added, m2, +1);
    ASSERT_FALSE(m2_key.empty());

    /// M1's removal folds normally. Through the REAL writer API — see correction 2.
    ///   DB::Cas::tests::dropRefTransition(*b, layout, ns, "part_1", m1.ref);
    s->dropRef(ns, "part_1");
    s->renewWatermarkOnce();   /// advance the floor so the removed closure is not spared as in-flight

    /// nth = 1: the fold's own walk (see the note in the retention test). ARMED LAST — see
    /// correction 3.
    b->omitFromNthListCall(m2_key, /*nth=*/1);

    runRounds(s, gc, 12);   /// condemn -> graduate -> delete needs several rounds
    ASSERT_TRUE(b->holeServed());

    EXPECT_TRUE(blobPresent(b, layout, payload))
        << "GC deleted a blob that manifest " << manifestRefDebugString(m2.ref)
        << " still references — the skipped-transaction DATA-LOSS class, reproduced";
}
```

~~If the `appendRefLogSeed` probe trick above turns out to perturb the stream (it appends an empty
transaction), replace it by listing `layout.refsNamespacePrefix(ns)` before and after
`publishOneBlobPart` and taking the newly-appeared Log-kind id.~~ **Stale — no `appendRefLogSeed` trick
appears anywhere in this task; that sentence survived from a superseded draft. And "the newly-appeared
Log-kind id" is not enough either: a publish appends TWO of them (correction 1).** The operative rule
is the one that stands in the last sentence: the omitted key must be the ref-log object carrying M2's
ACTIVATION, chosen by decoding, and `holeServed()` proves the hole was actually served.

- [x] **Step 6: Register the new test file in the build**

`src/Disks/tests/` sources are globbed by the `unit_tests_dbms` target; confirm with
`grep -rn "gtest_cas_gc_leak" src/Disks/tests/CMakeLists.txt src/CMakeLists.txt` — if that grep returns
nothing, the directory is globbed and no edit is needed. If it returns an explicit list, add the new
file there.

- [x] **Step 7: Build**

```bash
ninja -C build unit_tests_dbms > build/build_task1.log 2>&1; echo NINJA_EXIT=$?
```
Expected: `NINJA_EXIT=0`. Have a subagent summarise `build/build_task1.log`.

- [x] **Step 8: Run and REQUIRE failure**

```bash
build/src/unit_tests_dbms --gtest_filter='CasHoleyListDetector.*' > build/test_task1.log 2>&1; echo EXIT=$?
```
Expected: **both tests FAIL**.

- `OmittedRemoveRecordIsSkippedForever` asserts the blob is still present. It goes RED only if the
  omitted removal somehow *did* fold — which would refute the permanent-skip mechanism.
- `OmittedActivationNeverPermitsDeletingALiveBlob` asserts the live blob is still present. It goes RED
  when GC deletes it. **This RED is the data-loss reproduction and is the point of the task.**

So the expected first run is: test 1 GREEN (the retention direction holds — the record really is
skipped forever), test 2 RED (the deletion direction reproduces).

**If test 2 PASSES here, STOP and report.** A green means the assumed mechanism does not reach the
shipped code by the assumed route, and the detector's target must be re-derived before Task 2. Do not
weaken the test to make it red.

- [x] **Step 9: Commit**

```bash
git add src/Disks/tests/gtest_cas_holey_list_detector.cpp
cat > /tmp/msg1.txt <<'EOF'
ca: executable reproduction of the skipped-transaction defect (both directions)

A `HoleyListBackend` omits one durable ref-log key from a single `LIST` walk while exact `get` still
works. Two tests: the retention direction (the omitted removal is skipped forever, so the blob is
never reclaimed) and the MIRROR safety test (an omitted second-owner activation lets GC delete a blob
a live manifest still references). Both RED against the shipped code, which upgrades
`CaRelinkConfirmCore.tla` `_sab_holeylist` from a model result to an executable one. The detector in
the following commit turns them green.
EOF
git commit -F /tmp/msg1.txt -- src/Disks/tests/gtest_cas_holey_list_detector.cpp
git show --stat HEAD
```

---

## Task 2: Probe A — compare the two enumerations the round already performs

**STATUS: DONE** — `e01b5cd82be`.

**Corrections to this task's own text:**
- **The Interfaces block below says `fold` gains a LEADING `pre_scan` parameter; Step 1's own code
  block and the landed signature put it TRAILING.** The plan contradicts itself. Trailing is operative.
- **Step 2 says to replace the whole body of `changedShardCount` with the body given there. It was not
  used verbatim** — the landed `preFoldRefScan` keeps the existing function's structure (its
  `readFoldSeal` / cursor comparison) and only adds the id set. Read the current body before rewriting
  anything; the substitute below is a sketch, not a patch.
- **Probe A has a STATED LIMITATION this task does not mention, and it belongs in the design, not in a
  commit message.** The max-witness rule needs a witness ABOVE the missing id in the OTHER
  enumeration, so two shapes stay invisible: (a) a hole that reproduces IDENTICALLY in both walks
  (nothing cheap catches that), and (b) a namespace one enumeration dropped WHOLESALE — it has no
  `ref_tables` entry, so the loop never visits it, and the fold side offers no id to witness against.
  **Widening (b) to use the pre-scan's own maximum as the witness was considered and REJECTED**: it
  inverts the rule's justification and would fire on a namespace whose logs were legitimately all
  cleaned between the two walks, and a false positive here blocks the cursor advance for good rather
  than for one round.
- **Step 6's primary counter read is wrong in this environment.** `CurrentThread::getProfileEvents()`
  reads a container nothing writes to on a bare gtest thread. The step's own fallback —
  `ProfileEvents::global_counters[...]` snapshotted before the rounds and asserted as a delta — is what
  landed and is the operative form.
- `probe_a_holes_this_round` is RESET to 0 at the top of the block, not only assigned at the end,
  so an aborted or skipped round cannot report a stale count.
- **Two files the file list does not name were touched**, because renaming the test seam
  `changedShardCountForTest` → `preFoldRefScanForTest` reaches them: `src/Disks/tests/gtest_cas_gc_round.cpp`
  and `src/Disks/tests/gtest_cas_gc_round_defer.cpp`.
- The `RefScanSummary` doc comment as landed also names the test that pins both walks actually happen
  (`CasGcRound.EnumerationPagesCountedEvenWithSweepBudgetZeroed`), which is what stops the
  "obvious optimisation" of merging them from silently making probe A vacuous.

The round lists `cas/refs/` twice per round (`changedShardCount` at `CasGc.cpp:349`, `fold` at `:846`)
and throws the first result away. Keeping it makes the detector cost zero backend calls.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.h` (declaration of `changedShardCount` at `:342`,
  the test accessor at `:462-465`, the `fold` declaration at `:275`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.cpp` (`:349`, `:833`, `:878`, `:1910`)
- Modify: `src/Disks/tests/gtest_cas_holey_list_detector.cpp` (assertions flip)

**Interfaces:**
- Produces: `struct Cas::RefScanSummary { size_t changed_shards; std::map<String, std::set<RefTxnId>>
  logs_by_ns; std::map<String, RefTxnId> max_log_by_ns; };` and
  `RefScanSummary Gc::preFoldRefScan(const GcState &)`, consumed by Tasks 6 and 7 for the
  `defer_decision` phase metrics.
- `Gc::fold` gains a ~~leading~~ **TRAILING** `const RefScanSummary & pre_scan` parameter (see the
  corrections above — Step 1's code block already had it trailing).

- [x] **Step 1: Add `RefScanSummary` and change the declarations**

In `Gc/CasGc.h`, above the `Gc` class (next to the other GC-owned plain structs):

```cpp
/// The pre-fold enumeration of `cas/refs/`, retained rather than discarded. Two independent uses:
/// the DEFER signal (`changed_shards`, unchanged semantics), and the round's SECOND witness for the
/// skipped-transaction detector — `Gc::fold` performs its own full enumeration of the same prefix a
/// moment later, and disagreement between two enumerations of an append-only prefix is the only
/// unambiguous evidence available (see the design's "why a gap in the id sequence is not the signal").
///
/// KEEPING THE TWO WALKS SEPARATE IS LOAD-BEARING. Merging them into one enumeration — an otherwise
/// obvious optimisation, since the round lists the same prefix twice — silently makes probe A vacuous.
struct RefScanSummary
{
    size_t changed_shards = 0;                          /// tables with a log above their sealed cursor
    std::map<String, std::set<RefTxnId>> logs_by_ns;    /// Log-kind ids only, per namespace
    std::map<String, RefTxnId> max_log_by_ns;           /// greatest Log-kind id per namespace
};
```

Replace `size_t changedShardCount(const GcState & state);` (`:342`) with:

```cpp
    /// One full enumeration of `cas/refs/`, producing BOTH the defer signal and the detector's first
    /// witness. Lenient parse: a malformed key is not counted here; `groupRefKeys` in the fold does
    /// the strict validation and round-abort. `parseRefObjectKey` never throws.
    RefScanSummary preFoldRefScan(const GcState & state);
```

Replace the test accessor at `:462-465`:

```cpp
    RefScanSummary preFoldRefScanForTest(const GcState & state)
    {
        return preFoldRefScan(state);
    }
```

Change the `fold` declaration (`:275`) to:

```cpp
    FoldResult fold(GcState & state, Token & state_token, RoundReport & report, uint64_t current_round,
                    const RefScanSummary & pre_scan);
```

- [x] **Step 2: Implement `preFoldRefScan`**

Replace the whole body of `Gc::changedShardCount` (`CasGc.cpp:1910-1944`) with:

```cpp
RefScanSummary Gc::preFoldRefScan(const GcState & state)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    std::map<String, ShardCoverage> cursors;
    if (const auto seal = readFoldSeal(state.snap_generation, state.snap_attempt))
        cursors = seal->per_ns_shard;

    RefScanSummary scan;
    forEachListedKey(backend, layout.casRefsPrefix(), [&](const ListedKey & lk)
    {
        const auto parsed = layout.parseRefObjectKey(lk.key);
        if (parsed && parsed->kind == RefObjectKind::Log)
        {
            const String ns = parsed->ns.string();
            scan.logs_by_ns[ns].insert(parsed->txn_id);
            RefTxnId & g = scan.max_log_by_ns[ns];
            if (g < parsed->txn_id)
                g = parsed->txn_id;
        }
    }, 1000, onGcEnumerationPage);

    for (const auto & [ns_str, greatest] : scan.max_log_by_ns)
    {
        RefTxnId folded{};
        if (const auto it = cursors.find(cursorKey(RootNamespace{ns_str}, /*shard*/0)); it != cursors.end())
            folded = it->second.last_folded_ref_id;
        if (folded < greatest)
            ++scan.changed_shards;
    }
    return scan;
}
```

- [x] **Step 3: Update the two call sites**

At `CasGc.cpp:347-353`, replace the `changedShardCount` call:

```cpp
    const RefScanSummary pre_scan = preFoldRefScan(state);
    {
        const bool graduation_due = graduationDue(state, new_round);
        const size_t changed = pre_scan.changed_shards;
```

(the rest of the defer block is unchanged), and at `:403`:

```cpp
    FoldResult folded = fold(state, state_token, report, new_round, pre_scan);
```

Also update the definition line at `:833`:

```cpp
Gc::FoldResult Gc::fold(GcState & state, Token & /*state_token*/, RoundReport & report,
                        uint64_t current_round, const RefScanSummary & pre_scan)
```

- [x] **Step 4: Add the comparison in `fold`**

Insert immediately after the `for (const auto & [ns_str, listing] : ref_tables) result.root_shards…`
loop at `CasGc.cpp:878-879`, i.e. after `groupRefKeys` has produced `ref_tables` and before the
parent-cursor read:

```cpp
    /// PROBE A — "the store lied". Compare this round's TWO independent full enumerations of
    /// `cas/refs/` (the pre-fold scan and the one just performed above). The ref log per namespace is
    /// append-only with strictly increasing ids, so:
    ///
    ///   an id present in one enumeration, absent from the other, and STRICTLY BELOW the other
    ///   enumeration's maximum id for that same namespace CANNOT be a concurrent append.
    ///
    /// That is the only unambiguous signal available: a bare gap in the id sequence is LEGITIMATE
    /// (an append refused before any network attempt leaves a safe gap by design), and the interval
    /// between two ids is unbounded across a writer_epoch change, so the missing ids cannot be
    /// enumerated and probed. This check makes no assumption about WHY an enumeration was incomplete
    /// — a backend page, continuation behaviour, the iterator, or a mis-parse all surface identically.
    ///
    /// The one benign explanation is a DELETION between the two walks. Only `cleanupRefObjects`
    /// deletes ref logs and it runs post-CAS, so this round cannot be the deleter — but a DEPOSED
    /// leader still finishing its own post-CAS cleanup can be. That is itself a reason not to advance
    /// this round's cursors, so it takes the same path, with the alternative named in the log.
    if (!ref_folding_aborted)
    {
        uint64_t holes = 0;
        for (const auto & [ns_str, listing] : ref_tables)
        {
            std::set<RefTxnId> fold_logs(listing.logs.begin(), listing.logs.end());
            const auto pre_it = pre_scan.logs_by_ns.find(ns_str);
            const std::set<RefTxnId> empty_set;
            const std::set<RefTxnId> & pre_logs = pre_it != pre_scan.logs_by_ns.end() ? pre_it->second : empty_set;

            RefTxnId fold_max{};
            for (const RefTxnId & id : fold_logs)
                if (fold_max < id)
                    fold_max = id;
            RefTxnId pre_max{};
            if (const auto mit = pre_scan.max_log_by_ns.find(ns_str); mit != pre_scan.max_log_by_ns.end())
                pre_max = mit->second;

            const auto report_hole = [&](const RefTxnId & id, const char * which)
            {
                ++holes;
                LOG_ERROR(logger,
                    "CAS GC probe A: ref log {} of namespace {} was returned by one enumeration of {} "
                    "and NOT by the other ({}), below that enumeration's own maximum id for the "
                    "namespace — an append cannot explain this. Either the object store gave two "
                    "different answers about the same durable prefix, or a deposed leader is deleting "
                    "ref objects concurrently. Ref folding is ABORTED this round: no cursor advances "
                    "and no destructive action runs.",
                    renderRefTxnId(id), ns_str, layout.casRefsPrefix(), which);
            };

            for (const RefTxnId & id : pre_logs)
                if (id < fold_max && !fold_logs.contains(id))
                    report_hole(id, "missing from the fold's own scan");
            for (const RefTxnId & id : fold_logs)
                if (id < pre_max && !pre_logs.contains(id))
                    report_hole(id, "missing from the pre-fold scan");
        }
        if (holes > 0)
        {
            ProfileEvents::increment(ProfileEvents::CasGcRefScanDisagreements, holes);
            ref_folding_aborted = true;
            report.recordAnomaly(RootNamespace{}, 0, ManifestId{},
                                 "ref-prefix enumerations disagree: ref folding aborted this round");
        }
        probe_a_holes_this_round = holes;   /// surfaced as a phase metric in Task 7
    }
```

Add the member `uint64_t probe_a_holes_this_round = 0;` to the private section of `class Gc` in
`Gc/CasGc.h`, with the comment `/// Probe A's per-round hole count; read by the phase-row emitter.`

- [x] **Step 5: Declare the ProfileEvent**

In `src/Common/ProfileEvents.cpp`, next to `CasGcEnumerationPages` (line ~877):

```cpp
    M(CasGcRefScanDisagreements, "Number of ref-log ids on which a GC round's two independent enumerations of the ref prefix disagreed, below the other enumeration's own maximum id for that namespace. An append cannot produce this shape, so a nonzero value means either the object store answered inconsistently about a durable prefix or a deposed leader deleted ref objects concurrently. The round aborts ref folding when this is nonzero.", ValueType::Number) \
```

and add the matching `extern const Event CasGcRefScanDisagreements;` to the anonymous
`namespace ProfileEvents` block at the top of `Gc/CasGc.cpp`.

- [x] **Step 6: Flip the two tests to their post-fix expectations**

In `src/Disks/tests/gtest_cas_holey_list_detector.cpp`, both tests keep the same seeding and sabotage
but now assert the DETECTED behaviour.

**Retention test** — replace its final assertion (which asserted the blob is retained forever) with:

```cpp
    /// With probe A landed, the round that served the hole aborts ref folding, so the cursor never
    /// advances past the omitted record and the next round's complete enumeration folds it normally.
    EXPECT_FALSE(blobPresent(b, layout, payload))
        << "the removal was never folded even after the listing recovered — probe A recorded the "
           "disagreement but the cursor advanced anyway";
```

**Mirror test** — keep `EXPECT_TRUE(blobPresent(...))` (the live blob must still never be deleted) and
add a positive assertion that the DETECTOR fired, so a run that merely happened not to delete the blob
cannot pass for the wrong reason:

```cpp
    /// Assert the detector, not just the outcome.
    EXPECT_GT(CurrentThread::getProfileEvents()[ProfileEvents::CasGcRefScanDisagreements].load(), 0)
        << "the live blob survived, but probe A never fired — this test may be passing for the wrong reason";
```

Add `#include <Common/CurrentThread.h>` and `#include <Common/ProfileEvents.h>` plus the
`extern const Event CasGcRefScanDisagreements;` declaration to the test file. If the bare gtest thread
has no attached `ThreadStatus` (it does not — see the note at the top of `gtest_cas_gc_log.cpp`), read
the process-global counter instead: `ProfileEvents::global_counters[ProfileEvents::CasGcRefScanDisagreements]`,
snapshotting it before the round and asserting the delta.

- [x] **Step 7: Build and run**

```bash
ninja -C build unit_tests_dbms > build/build_task2.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasHoleyListDetector.*' > build/test_task2.log 2>&1; echo EXIT=$?
```
Expected: `NINJA_EXIT=0`, `EXIT=0`, both tests PASS.

- [x] **Step 8: Full gate**

```bash
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task2_gate.log 2>&1; echo EXIT=$?
```
Expected: `EXIT=0`, zero failures. Have a subagent summarise.

- [x] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
        src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_holey_list_detector.cpp
cat > /tmp/msg2.txt <<'EOF'
ca: probe A — detect a skipped ref transaction by comparing the round's two ref enumerations

A GC round already enumerates `cas/refs/` twice (the defer signal and the fold) and discarded the
first result. `changedShardCount` becomes `preFoldRefScan`, which keeps the id set; the fold then
compares the two. Because the per-namespace log is append-only with strictly increasing ids, an id
present in one enumeration and absent from the other BELOW the other's own maximum for that namespace
cannot be a concurrent append — that is the unambiguous signal, and it is mechanism-agnostic. A
disagreement aborts ref folding for the round (no cursor advances, no destructive action), reusing the
existing abort path. Zero additional backend calls.

Deliberately NOT a LIST-hole detector: the LIST hypothesis is unconfirmed, so the probe catches the
EFFECT. Id gaps stay legitimate (an append refused before any attempt leaves one by design).
EOF
git commit -F /tmp/msg2.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_holey_list_detector.cpp
git show --stat HEAD
```

---

## Task 3: Probe B1 — the intake-layer identity

**STATUS: DONE** — `e01b5cd82be`. No surprises; two small deviations worth knowing:
- `logs_intended_this_round` / `logs_applied_this_round` are RESET to 0 before the block as well as
  assigned inside it, so a ref-folding abort (where the identity does not apply) cannot leave the
  previous round's numbers standing on the phase row.
- Ordering: probe **B2**'s verdict throws FIRST, then B1's comparison runs — which is what Task 4
  Step 6 asks for ("immediately before the probe-B1 block added in Task 3"), stated here too because
  reading Task 3 alone gives the opposite impression.

Cheap control-flow assertion: the number of logs the round declares covered, recomputed at seal time
from the sealed cursors, must equal the running count of logs that actually folded. **This is NOT a
detector for the suspected defect** — the recomputation reads the same listing — and the code comment
must say so, or a future reader will over-trust it.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.cpp` (`:1049-1219`, and before `:1479`)

- [x] **Step 1: Add the running counter**

At `CasGc.cpp:1084`, next to `RefTxnId resolved_through = cursor;`, add to the enclosing scope (just
above the `for (auto & [ns_str, listing] : ref_tables)` loop at `:1049`):

```cpp
    /// PROBE B1 — intake-layer identity. `logs_applied` counts, at the SINGLE cursor-advance site
    /// below, every log whose whole body folded. At seal time it is compared against a recomputation
    /// from the sealed coverage and the listing. The two are derived differently (a running counter vs
    /// a recomputation), so a control-flow bug that advances a cursor without folding breaks the
    /// equality.
    ///
    /// REACH, stated so this is not over-trusted: the recomputation reads the SAME listing the intake
    /// read, so B1 is BLIND to a record missing from the listing. It is a control-flow assertion, not
    /// a detector for the skipped-transaction defect — probe A covers the listing, probe B2 covers
    /// everything below the intake.
    uint64_t logs_applied = 0;
```

and at `:1210-1211`, immediately after `resolved_through = log_id;`:

```cpp
            ++logs_applied;
```

- [x] **Step 2: Add the seal-time recomputation**

Insert just before `putDeterministicArtifact(backend, layout.foldSealKey(new_generation, attempt), …)`
at `CasGc.cpp:1479`:

```cpp
    /// PROBE B1's comparison. Skipped on a ref-folding abort: that path deliberately discards every
    /// cursor advance and carries the parent cursors, so the identity does not apply.
    if (!ref_folding_aborted)
    {
        uint64_t logs_intended = 0;
        for (const auto & [ns_str, listing] : ref_tables)
        {
            const String cursor_key = cursorKey(RootNamespace{ns_str}, /*shard*/0);
            RefTxnId cursor_prev{};
            if (const auto pit = parent_cursors.find(cursor_key); pit != parent_cursors.end())
                cursor_prev = pit->second.last_folded_ref_id;
            RefTxnId sealed{};
            if (const auto sit = result.fold_seal.per_ns_shard.find(cursor_key);
                sit != result.fold_seal.per_ns_shard.end())
                sealed = sit->second.last_folded_ref_id;
            for (const RefTxnId & id : listing.logs)
                if (cursor_prev < id && !(sealed < id))
                    ++logs_intended;
        }
        if (logs_intended != logs_applied)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS GC fold: the round sealed coverage over {} ref log(s) but only {} fully folded — "
                "a cursor advanced past a log this round never applied. GC refuses to commit the round; "
                "recover with SYSTEM CONTENT ADDRESSED GC REBUILD.",
                logs_intended, logs_applied);
        logs_intended_this_round = logs_intended;   /// surfaced as a phase metric in Task 7
        logs_applied_this_round = logs_applied;
    }
```

Add `uint64_t logs_intended_this_round = 0;` and `uint64_t logs_applied_this_round = 0;` to the private
section of `class Gc` alongside `probe_a_holes_this_round`.

- [x] **Step 3: Build, gate**

```bash
ninja -C build unit_tests_dbms > build/build_task3.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task3.log 2>&1; echo EXIT=$?
```
Expected: `NINJA_EXIT=0`, `EXIT=0`. A failure here means an existing path advances a cursor without
folding — investigate before proceeding; do not relax the check.

- [x] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.{h,cpp}
cat > /tmp/msg3.txt <<'EOF'
ca: probe B1 — the round's sealed ref coverage must equal the logs it actually folded

A running counter at the single cursor-advance site vs a seal-time recomputation from the sealed
coverage and the listing. Different derivations, so a control-flow bug that advances a cursor without
folding breaks the equality and the round refuses to commit. The comment states its reach explicitly:
the recomputation reads the same listing, so B1 is blind to a record missing from that listing — that
half is probe A's, and everything below the intake is probe B2's.
EOF
git commit -F /tmp/msg3.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
git show --stat HEAD
```

---

## Task 4: Probe B2 — per-transaction apply ledger through the reducers

**STATUS: DONE** — `e01b5cd82be`.

> ### ⚠ STEP 2 AS WRITTEN CONTAINS A LIVE BUG. DO NOT FOLLOW IT. {#task4-field-placement}
>
> The struct below declares `txn_ordinal` **before `remove`**. Several existing tests brace-initialise
> a `BlobDelta` POSITIONALLY as `{ref, source_id, remove}`. Inserting a field ahead of `remove`
> silently rebinds that third initialiser to the ordinal, so **every removal delta becomes an
> activation** — it compiles, it is type-correct, and it is wrong. Eight tests went red.
>
> **Operative rule: the ordinal is declared LAST, after `remove`**, with a comment forbidding future
> insertions ahead of it (or a conversion of every aggregate initialiser to designated form first).
> Step 2's code block below is corrected in place; the wrong ordering is kept in a comment there so the
> trap stays visible.

**Other corrections to this task's own text:**
- **The row did NOT grow** — 64 bytes before and after, the ordinal landing in existing tail padding.
  Steps 1 and 8 only ask to measure and report; the landed code goes further and PINS it
  (`kBlobDeltaSize` + `static_assert`), so the next field addition has to argue itself as a hot-path
  change instead of being discovered by a perf run.
- **Step 3's struct omits `markApplied`**, which this task's own Interfaces block lists. It is needed
  and it landed.
- **Step 6 assigns `txns_unapplied_this_round = 0` AFTER the throw block**, which throws away the only
  forensic value the member has: on the round that fails, the count is what a reader wants. Landed
  version resets it to 0 before the check and sets it to `unapplied.size()` INSIDE the failure branch.
- **The file list misses four call sites**, all on the rebuild path: three `foldManifestEdges` calls
  that must pass `/*txn_ordinal=*/0`, and the rebuild's own `foldDeltasIntoGeneration`, which passes
  `nullptr` for the out-vector. That last one needs its reason recorded, not just the argument: the
  rebuild derives edges from raw owner STATE, not from a stream of ref transactions, so there is no
  transaction whose deltas could go unapplied and no fold cursor to advance past one.
- **Step 7's three tests are not enough and only three of the seven that landed.** They exercise the
  ledger's own arithmetic, which cannot distinguish "the probe is correct" from "the probe is inert" —
  the fold-side wiring would be covered only NEGATIVELY (the gate does not throw). Two of the four
  added tests drive the REAL reducer: one pins that a routed delta marks its ordinal while an ordinal
  no delta carries stays unmarked, and one pins that a REMOVAL delta is marked too (removals are the
  direction that legitimately collapses to nothing inside the set merge, so a mark placed at run flush
  instead of at consumption would skip exactly this case and report a healthy round as lossy).

**HOT PATH.** This adds a field to `BlobDelta`, the fold's per-edge row.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/Gc/CasBlobInDegree.h` (`:140-145`, `:264-275`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasBlobInDegree.cpp` (`:587-603`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGcShardPlan.h` (`:106-115`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGcShardPlan.cpp` (`:43-61`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.h` (`TxnApplyLedger`, `foldManifestEdges` at `:280`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.cpp` (`:753`, `:802-830`, `:1137-1211`, `:1384-1444`)
- Create: `src/Disks/tests/gtest_cas_txn_apply_ledger.cpp`

**Interfaces:**
- Produces: `struct Cas::TxnApplyLedger` (in `Gc/CasGc.h`) with `open`, `markProduced`,
  `markCommitted`, `markApplied`, `unapplied`, and the public member
  `std::vector<uint8_t> applied` that the reducers write through a raw pointer.
- `BlobDelta` gains `uint32_t txn_ordinal = 0;` **as its LAST member — see {#task4-field-placement}.**
- `foldDeltasIntoGeneration` and `ShardReducer::reduce` gain a trailing
  `std::vector<uint8_t> * out_applied_by_txn_ordinal = nullptr`.

- [x] **Step 1: MEASURE the struct growth before writing any code**

```bash
.claude/tools/cppexpr.sh -i Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h \
  'OUT(sizeof(DB::Cas::BlobDelta)) OUT(alignof(DB::Cas::BlobDelta))'
```
Record the value. Then repeat after Step 2 and compare. If `sizeof` increases, say so explicitly in
the Step 9 commit message — do not let a hot-row growth land silently.

- [x] **Step 2: Add the field to `BlobDelta`** — **AS THE LAST MEMBER. See {#task4-field-placement}.**

`Gc/CasBlobInDegree.h:140-145`:

```cpp
struct BlobDelta
{
    BlobRef ref{};
    UInt128 source_id{};   /// `sourceEdgeId(ManifestId, path)` — an edge identity, not a content hash
    /// WRONG PLACEMENT — the original plan put `txn_ordinal` HERE, before `remove`. That silently
    /// rebinds every positional `BlobDelta{ref, source_id, remove}` initialiser in the tests, turning
    /// every removal delta into an activation. Compiles, type-correct, wrong; eight tests went red.
    bool remove = false;
    /// Round-local index of the ref transaction that emitted this delta, into `TxnApplyLedger::txns`.
    /// NEVER persisted and never part of any comparator: it exists only so the reducer can prove it
    /// consumed at least one delta from every transaction the round declared covered (probe B2). It
    /// lands in the struct's existing tail padding — the row does not grow.
    ///
    /// DECLARED LAST, and that is load-bearing. Any future field goes AFTER this one, or every
    /// aggregate initialiser gets converted to designated form first.
    uint32_t txn_ordinal = 0;
};

/// Pin the size rather than leaving a perf run to discover a regression.
static constexpr size_t kBlobDeltaSize = 64;
static_assert(sizeof(BlobDelta) == kBlobDeltaSize,
              "BlobDelta is the fold's hot per-edge row; growing it is a hot-path change");
```

- [x] **Step 3: Add `TxnApplyLedger`**

In `Gc/CasGc.h`, next to `RefScanSummary`:

```cpp
/// PROBE B2 — end-to-end transaction accounting for ONE round. Round-local, never persisted.
///
/// The naive "intended vs applied" counter pair is VACUOUS: in the intake both counts increment in the
/// same basic block, so they cannot differ. This ledger separates them across the whole pipeline
/// instead — a transaction is `committed` when the intake merges its staged buffers, `produced` when
/// it emitted at least one `BlobDelta`, and `applied` only when a shard reducer actually CONSUMED one
/// of its deltas. A delta lost between the intake and a reducer (routing across gc shards, a skipped
/// bucket, a future filter) leaves a committed+produced transaction unapplied.
///
/// Marking happens at reducer CONSUMPTION, not at run flush: the in-degree model is a SET, so an
/// unmatched `-1` and a duplicate `+1` legitimately vanish inside the reducer and a flush-side mark
/// would fire on healthy rounds. Loss inside the reducer's own set collapse is a different class,
/// covered by `CasGcUnmatchedRemoveDeltas` and the mirror safety test.
///
/// SINGLE-THREADED: the shard reducers run sequentially on the fold thread (`Gc::fold`), so `applied`
/// needs no synchronisation. A future parallel reducer must revisit this.
struct TxnApplyLedger
{
    std::vector<RefTxnId> txns;        /// ordinal -> the log id
    std::vector<String> namespaces;    /// ordinal -> namespace, for the failure message
    std::vector<uint8_t> produced;     /// this transaction emitted >= 1 BlobDelta
    std::vector<uint8_t> committed;    /// this transaction folded fully and merged into the round buffers
    std::vector<uint8_t> applied;      /// >= 1 of this transaction's deltas was consumed by a reducer

    uint32_t open(const RootNamespace & ns, const RefTxnId & id)
    {
        const uint32_t ordinal = static_cast<uint32_t>(txns.size());
        txns.push_back(id);
        namespaces.push_back(ns.string());
        produced.push_back(0);
        committed.push_back(0);
        applied.push_back(0);
        return ordinal;
    }
    void markProduced(uint32_t ordinal) { produced[ordinal] = 1; }
    void markCommitted(uint32_t ordinal) { committed[ordinal] = 1; }
    void markApplied(uint32_t ordinal) { applied[ordinal] = 1; }   /// omitted from the original block

    /// Ordinals that were committed AND produced deltas but whose deltas never reached a reducer.
    std::vector<uint32_t> unapplied() const
    {
        std::vector<uint32_t> out;
        for (uint32_t i = 0; i < txns.size(); ++i)
            if (committed[i] && produced[i] && !applied[i])
                out.push_back(i);
        return out;
    }
};
```

- [x] **Step 4: Thread the ordinal through the intake**

`foldManifestEdges` (`Gc/CasGc.h:280`, `Gc/CasGc.cpp:753`) gains a trailing `uint32_t txn_ordinal`
parameter, and the `deltas.push_back` at `CasGc.cpp:806-809` becomes:

```cpp
            /// Designated initialisers must follow DECLARATION order, so with the corrected field
            /// placement ({#task4-field-placement}) `.remove` comes before `.txn_ordinal`.
            deltas.push_back(BlobDelta{
                .ref = entry.ref,
                .source_id = sourceEdgeId(id, entry.path),
                .remove = (sign < 0),
                .txn_ordinal = txn_ordinal});
```

In the intake loop, immediately after the `if (!(cursor < log_id)) continue;` / `if (clamped) break;`
guards at `CasGc.cpp:1088-1091`:

```cpp
            const uint32_t txn_ordinal = ledger.open(ns, log_id);
```

pass `txn_ordinal` to the `foldManifestEdges` call at `:1142`, and at the merge point (`:1200-1211`),
right after the `log_deltas` merge loop:

```cpp
            if (!log_deltas.empty())
                ledger.markProduced(txn_ordinal);
            ledger.markCommitted(txn_ordinal);
```

Declare `TxnApplyLedger ledger;` next to `std::vector<BlobDelta> deltas;` at `CasGc.cpp:1027`. On the
`ref_folding_aborted` path (`:1224-1239`) add `ledger = TxnApplyLedger{};` alongside `deltas.clear();`.

- [x] **Step 5: Thread the out-vector into the reducers**

`Gc/CasBlobInDegree.h`, append to `foldDeltasIntoGeneration`'s parameter list (after
`bool suppress_destructive = false`):

```cpp
                              ,
                              /// PROBE B2 (see `Cas::TxnApplyLedger`): when set, one byte is stored per
                              /// CONSUMED delta at `(*out_applied_by_txn_ordinal)[d.txn_ordinal]`. Raw
                              /// vector rather than a callback: this runs once per delta over a stream
                              /// that can reach millions of rows, and a `std::function` call there is
                              /// not free. Never read by the merge; write-only.
                              std::vector<uint8_t> * out_applied_by_txn_ordinal = nullptr);
```

In `Gc/CasBlobInDegree.cpp`, inside the delta-consumption loop at `:587-603`, immediately before the
`present = …` assignment:

```cpp
                if (out_applied_by_txn_ordinal)
                    (*out_applied_by_txn_ordinal)[scattered[di].txn_ordinal] = 1;
```

Mirror the parameter on `ShardReducer::reduce` (`Gc/CasGcShardPlan.h:106-115`) with the same default
and forward it verbatim in `Gc/CasGcShardPlan.cpp:55-59`.

- [x] **Step 6: Pass the ledger at both fold call sites and add the verdict**

`CasGc.cpp:1398-1403` (single-shard) and `:1433-1439` (sharded): append `&ledger.applied` as the last
argument.

Then, immediately before the probe-B1 block added in Task 3 (i.e. before the seal write at `:1479`):

```cpp
    /// PROBE B2's verdict. A committed transaction that produced deltas but whose deltas never
    /// reached a reducer means this round LOST a durable record it had already read and decoded.
    /// Unlike a 404 during a fold (missing evidence — see feedback_ca_gc_never_throw_on_404, which
    /// must never wedge the round), this is proof of loss, so the round fails CLOSED: nothing is
    /// adopted, GC reclaims and deletes nothing, and an operator has to intervene.
    txns_unapplied_this_round = 0;
    if (const std::vector<uint32_t> unapplied = ledger.unapplied(); !unapplied.empty())
    {
        txns_unapplied_this_round = unapplied.size();   /// set HERE, not after the block — the count
                                                        /// is the forensic record of the failed round
        String detail;
        for (size_t i = 0; i < unapplied.size() && i < 8; ++i)
        {
            if (i != 0)
                detail += ", ";
            detail += ledger.namespaces[unapplied[i]] + "@" + renderRefTxnId(ledger.txns[unapplied[i]]);
        }
        ProfileEvents::increment(ProfileEvents::CasGcUnappliedFoldedTxns, unapplied.size());
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS GC fold: {} ref transaction(s) folded and merged into the round buffers but NONE of "
            "their blob deltas reached a shard reducer ({}{}). The round would have advanced its "
            "cursor past a transaction it never applied. GC refuses to commit the round; recover with "
            "SYSTEM CONTENT ADDRESSED GC REBUILD.",
            unapplied.size(), detail, unapplied.size() > 8 ? ", …" : "");
    }
    ///   txns_unapplied_this_round = 0;   <- WRONG: this discarded the count on exactly the round that
    ///   needed it. Surfaced as a phase metric in Task 7 (0 on every COMMITTED round, by construction,
    ///   because a nonzero value throws above).
```

Add `uint64_t txns_unapplied_this_round = 0;` to the private section of `class Gc`, and declare
`CasGcUnappliedFoldedTxns` in `src/Common/ProfileEvents.cpp` next to `CasGcRefScanDisagreements`:

```cpp
    M(CasGcUnappliedFoldedTxns, "Number of ref transactions a GC round folded and merged but whose blob deltas never reached a shard reducer. Always 0 on a healthy round; a nonzero value fails the round closed, because the round would otherwise advance its fold cursor past a transaction it never applied.", ValueType::Number) \
```

- [x] **Step 7: Unit-test the ledger in isolation**

Create `src/Disks/tests/gtest_cas_txn_apply_ledger.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>

using namespace DB::Cas;

/// The ledger is pure round-local bookkeeping; test it directly rather than trying to fabricate a
/// lost bucket inside a real fold. The fold-side wiring is covered by the gate: every existing GC
/// test now runs with the ledger armed and would throw if a delta went missing.
TEST(CasTxnApplyLedger, HealthyRoundReportsNothingUnapplied)
{
    TxnApplyLedger ledger;
    const uint32_t a = ledger.open(RootNamespace{"ns"}, RefTxnId{1, 1});
    const uint32_t b = ledger.open(RootNamespace{"ns"}, RefTxnId{1, 2});
    ledger.markProduced(a);
    ledger.markCommitted(a);
    ledger.applied[a] = 1;
    ledger.markCommitted(b);          /// committed but produced no blob deltas — legitimate
    EXPECT_TRUE(ledger.unapplied().empty());
}

TEST(CasTxnApplyLedger, CommittedAndProducedButNeverAppliedIsReported)
{
    TxnApplyLedger ledger;
    const uint32_t a = ledger.open(RootNamespace{"ns"}, RefTxnId{1, 1});
    ledger.markProduced(a);
    ledger.markCommitted(a);
    ASSERT_EQ(ledger.unapplied().size(), 1u);
    EXPECT_EQ(ledger.unapplied().front(), a);
}

TEST(CasTxnApplyLedger, ClampedTransactionIsNotReported)
{
    /// A clamped log emits deltas into the per-log staging buffer that is then DISCARDED; it is never
    /// committed, so it must not be reported unapplied.
    TxnApplyLedger ledger;
    const uint32_t a = ledger.open(RootNamespace{"ns"}, RefTxnId{1, 1});
    ledger.markProduced(a);
    EXPECT_TRUE(ledger.unapplied().empty());
}
```

- [x] **Step 8: Build, measure again, gate**

```bash
ninja -C build unit_tests_dbms > build/build_task4.log 2>&1; echo NINJA_EXIT=$?
.claude/tools/cppexpr.sh -i Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h 'OUT(sizeof(DB::Cas::BlobDelta))'
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task4.log 2>&1; echo EXIT=$?
```
Expected: `NINJA_EXIT=0`, `EXIT=0`. Compare the `sizeof` against Step 1.

- [x] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/ src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_txn_apply_ledger.cpp
cat > /tmp/msg4.txt <<'EOF'
ca: probe B2 — prove every folded ref transaction's deltas reached a shard reducer

A round-local `TxnApplyLedger` assigns each opened ref log an ordinal, carried on `BlobDelta` and
marked by the reducer at the point it CONSUMES the delta. A transaction that was committed by the
intake and produced deltas, but whose deltas never reached a reducer, fails the round closed before
the seal write — the round would otherwise advance its cursor past a transaction it never applied.

The naive intended-vs-applied counter pair would have been vacuous (both increment in the same basic
block); this separates the two counts across the whole pipeline instead, which is the half a
LIST-focused detector cannot see. Marking is at reducer consumption, not at run flush, because the
in-degree model is a set and legitimate collapse would otherwise read as loss.

sizeof(BlobDelta): <BEFORE> -> <AFTER>.
EOF
git commit -F /tmp/msg4.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_txn_apply_ledger.cpp
git show --stat HEAD
```

---

## Task 5: Per-phase row plumbing — schema, record, correlator

**STATUS: DONE 2026-07-26** — landed with Tasks 6-8 in `d412f85f749`.

**STATUS: IN PROGRESS** (tasks 5-8 dispatched together at 00:09 UTC 2026-07-26, worklog
`75a03e26ffb`). No commit yet — nothing below is ticked, and nothing in tasks 5-8 should be read as
landed. Two constraints were given to the executing agent on dispatch and are recorded here so they
survive the agent: the correlator must NOT be `round` (it is 0 on Start and does not exist at all on a
non-leader round — exactly the rounds worth correlating), and `meta_pool_wait`'s `ProfileEvents` delta
is empty BY CONSTRUCTION because that work runs on other threads, so it must carry explicit job counts
rather than look like it has coverage.

Tasks 5-8 are also the READER for the four values the detector deliberately left written-but-unread
(`probe_a_holes_this_round`, `logs_intended_this_round`, `logs_applied_this_round`,
`txns_unapplied_this_round`, `e01b5cd82be`). They were designed together and must be WIRED, not
duplicated.

Additive only; no phase is instrumented yet, so the table gains columns and an event type but no new
rows. Kept separate so the schema change can be reviewed on its own.

**Files:**
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.h` (`:10-49`)
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp` (`:16-93`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGcScheduler.h` (`:20-55`)
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGcScheduler.cpp` (`:110-208`)
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedMetadataStorage.cpp` (`:466-519`)
- Modify: `src/Disks/tests/gtest_cas_gc_log.cpp`

**Interfaces:**
- Produces: `GcRoundLogRecord` gains `String round_id`, `String phase`, `UInt64 phase_duration_us`,
  `std::map<String, UInt64> phase_metrics`, and `EventType::Phase`. Tasks 6 and 7 emit rows through
  `CasGcScheduler`'s new `emitPhase` sink.

- [x] **Step 1: Extend `GcRoundLogRecord`**

`Gc/CasGcScheduler.h`, inside `struct GcRoundLogRecord`:

Change the enum on the existing line and append four members after `std::map<String, UInt64>
profile_events;` (the last member today). Nothing existing is removed or reordered.

```cpp
    enum class EventType { Start, Finish, Phase };   /// was { Start, Finish }

    /// Correlator for every row of ONE round: a fresh random hex id minted per `runRoundLogged`
    /// invocation and stamped on the Start row, every Phase row, and the Finish row. NOT the round
    /// number: `round` is 0 on Start, is only known after the single `gc/state` CAS on a folding
    /// round, and does not exist at all on a NotALeader round — and the rounds that fail are exactly
    /// the ones a reader needs to correlate.
    String round_id;
    /// One of the 17 phase names (the literals passed to `Cas::GcPhaseTimer`); empty on Start/Finish.
    String phase;
    /// Wall time of this phase. MICROseconds: `meta_pool_wait` and `round_commit` are routinely
    /// sub-millisecond and the whole point of the row is seeing when they are not.
    UInt64 phase_duration_us = 0;
    /// Phase-specific semantic counts. The per-phase ProfileEvents delta rides `profile_events`.
    std::map<String, UInt64> phase_metrics;
```

- [x] **Step 2: Extend the log element**

`src/Interpreters/ContentAddressedGarbageCollectionLog.h` — change the enum at `:12` and append four
members after `std::map<String, UInt64> profile_events;` at `:43`:

```cpp
    enum EventType : int8_t { START = 1, FINISH = 2, PHASE = 3 };   /// was { START = 1, FINISH = 2 }

    String round_id;                           /// correlator for every row of one round attempt
    String phase;                              /// empty on START/FINISH
    UInt64 phase_duration_us = 0;              /// PHASE rows only
    std::map<String, UInt64> phase_metrics;    /// PHASE rows only
```

`ContentAddressedGarbageCollectionLog.cpp` — add `{"Phase", static_cast<Int8>(PHASE)}` to `type_enum`
(`:18-19`), and append four columns at the END of `getColumnsDescription` (after `ProfileEvents`, so
existing positional readers are undisturbed):

```cpp
        {"round_id", std::make_shared<DataTypeString>(),
            "Correlator for every row of one round (Start, each Phase, Finish). Minted per round attempt; unlike `round` it exists even for a round that never committed or never led. Group by this column to reconstruct one round."},
        {"phase", lc_string,
            "The GC phase this row describes (empty on Start/Finish): lease, heartbeat_floor, defer_decision, fold_ref_list, fold_seal_read, fold_ref_intake, fold_reduce, fold_ns_cleanup_scan, fold_seal_write, pending_deletes, meta_pool_wait, round_commit, handoff_reclaim, manifest_deletes, namespace_cleanup, ref_object_cleanup, orphan_sweep."},
        {"phase_duration_us", std::make_shared<DataTypeUInt64>(),
            "Wall-clock duration of this phase in microseconds (Phase rows only)."},
        {"phase_metrics", std::make_shared<DataTypeMap>(lc_string, std::make_shared<DataTypeUInt64>()),
            "Phase-specific semantic counts (Phase rows only). The per-phase S3/Cas ProfileEvents delta is in the `ProfileEvents` column of the same row."},
```

and the matching four `columns[i++]->insert(...)` at the end of `appendToBlock`, with the map built
exactly like `profile_events`.

- [x] **Step 3: Mint the correlator and add the phase sink**

`Gc/CasGcScheduler.cpp`, inside `runRoundLogged`, immediately after the `SCOPE_EXIT` at `:119`:

```cpp
    const String round_id = Cas::u128ToHex(
        (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng());
```

Set `start.round_id = round_id;` before the `emit(start)` at `:142`, and (since `Rec fin = start;` at
`:160` copies it) the Finish row inherits it. Add, right after `emit(start)`:

```cpp
    /// The phase sink handed to the round engine. Same best-effort discipline as `emit`: a throwing
    /// sink must never break GC.
    Cas::GcPhaseSink phase_sink = [&](const Cas::GcPhaseRecord & p)
    {
        Rec row = start;
        row.event_type = Rec::EventType::Phase;
        row.phase = p.phase;
        row.phase_duration_us = p.duration_us;
        row.phase_metrics = p.metrics;
        row.profile_events = p.profile_events;
        emit(row);
    };
    round_gc.setPhaseSink(phase_sink);
    SCOPE_EXIT({ round_gc.setPhaseSink({}); });
```

Add `GcPhaseRecord` / `GcPhaseSink` to `Gc/CasGcScheduler.h` above `GcRoundLogRecord`:

```cpp
/// One phase of one round. Pure data, no Interpreters dependency — the same discipline
/// `GcRoundLogRecord` follows.
struct GcPhaseRecord
{
    String phase;
    UInt64 duration_us = 0;
    std::map<String, UInt64> metrics;
    std::map<String, UInt64> profile_events;   /// this phase's ProfileEvents delta
};
using GcPhaseSink = std::function<void(const GcPhaseRecord &)>;
```

and to `class Gc` (`Gc/CasGc.h`, public): `void setPhaseSink(GcPhaseSink sink) { phase_sink = std::move(sink); }`
with the private member `GcPhaseSink phase_sink;`.

- [x] **Step 4: Extend the converter**

`ContentAddressedMetadataStorage.cpp:475-477`, replace the two-way `event_type` mapping with a
three-way switch, and add after `e.profile_events = r.profile_events;` (`:516`):

```cpp
        e.round_id = r.round_id;
        e.phase = r.phase;
        e.phase_duration_us = r.phase_duration_us;
        e.phase_metrics = r.phase_metrics;
```

- [x] **Step 5: Extend the scheduler gtest**

In `src/Disks/tests/gtest_cas_gc_log.cpp`, add:

```cpp
/// Every row of one round carries the SAME non-empty `round_id`, and two rounds carry DIFFERENT ones.
/// That is the property the column exists for: `round` is 0 on Start and absent on a round that never
/// committed, so it cannot serve as the correlator.
TEST(CasGcLog, EveryRowOfARoundSharesOneRoundId)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"srv1/tbl"};
    publishPart(store, ns.string(), "all_0_0_0", "hello-round-id");
    store->dropRef(ns, "all_0_0_0");
    store->renewWatermarkOnce();

    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(
        store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const Rec & r) { rows.push_back(r); });

    sched.runOneRoundNow(Rec::Trigger::Manual);
    const size_t after_first = rows.size();
    ASSERT_GE(after_first, 2u);
    const String first_id = rows.front().round_id;
    EXPECT_FALSE(first_id.empty());
    for (size_t i = 0; i < after_first; ++i)
        EXPECT_EQ(rows[i].round_id, first_id) << "row " << i << " of the first round has a different round_id";

    store->renewWatermarkOnce();
    sched.runOneRoundNow(Rec::Trigger::Manual);
    ASSERT_GT(rows.size(), after_first);
    const String second_id = rows[after_first].round_id;
    EXPECT_FALSE(second_id.empty());
    EXPECT_NE(second_id, first_id) << "two rounds must not share a round_id";
    for (size_t i = after_first; i < rows.size(); ++i)
        EXPECT_EQ(rows[i].round_id, second_id);
}
```

- [x] **Step 6: Build and gate**

```bash
ninja -C build unit_tests_dbms > build/build_task5.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task5.log 2>&1; echo EXIT=$?
```
Expected: `NINJA_EXIT=0`, `EXIT=0`.

- [x] **Step 7: Commit**

```bash
git add src/Interpreters/ContentAddressedGarbageCollectionLog.{h,cpp} \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.{h,cpp} \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_cas_gc_log.cpp
cat > /tmp/msg5.txt <<'EOF'
ca: per-phase GC log rows — schema, record, and the round correlator

`system.content_addressed_garbage_collection_log` gains a `Phase` event type and four columns:
`round_id` (the correlator for every row of one round attempt — unlike `round` it exists for a round
that never committed or never led), `phase`, `phase_duration_us`, and `phase_metrics`. The existing
`ProfileEvents` map carries the per-phase delta on a Phase row, so no verb columns are invented.

Plumbing only: no phase is instrumented yet, so no Phase rows are emitted.
EOF
git commit -F /tmp/msg5.txt -- src/Interpreters/ContentAddressedGarbageCollectionLog.h src/Interpreters/ContentAddressedGarbageCollectionLog.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_cas_gc_log.cpp
git show --stat HEAD
```

---

## Task 6: The phase timer and the ten round-level phases

**STATUS: DONE 2026-07-26** (`d412f85f749`), `CasGcPhaseTimer.h`.

**STATUS: IN PROGRESS** — see Task 5's status note. Not landed.

**Files:**
- Create: `src/Disks/.../ContentAddressed/Gc/CasGcPhaseTimer.h`
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.cpp` (`runRegularRound`, `:269-751`)

**Interfaces:**
- Produces: `class Cas::GcPhaseTimer`, constructed as `GcPhaseTimer t(phase_sink, "<phase>")` where
  `<phase>` is one of the 17 string literals from the spec's phase table. No enum: the names are
  literals at the single site that uses each one, and an enum plus a name function would add a second
  place to keep in sync for no reader benefit. Consumed by Task 7.

- [x] **Step 1: Write the timer**

Create `Gc/CasGcPhaseTimer.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Common/CurrentThread.h>
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>

namespace DB::Cas
{

/// Times ONE GC phase and emits a `GcPhaseRecord` on destruction.
///
/// The ProfileEvents delta is a plain snapshot difference of whatever counters container is currently
/// attached to this thread. It deliberately does NOT use a nested `ProfileEventsScope`: that
/// RE-PARENTS the thread's counters (`ProfileEventsScope.cpp`), and the round-level scope installed by
/// `CasGcScheduler::runRoundLogged` already holds that slot. A snapshot diff composes with the outer
/// scope instead of fighting it, and degrades to an empty map on a thread with no `ThreadStatus`
/// (a bare gtest thread) — the same degradation the round-level capture already accepts.
///
/// Cost per phase: one `Stopwatch` and two counters snapshots, against phases that each perform
/// network I/O. Always on; no setting, deliberately (a knob whose default nobody remembers is how
/// instrumentation degrades to silence).
class GcPhaseTimer
{
public:
    GcPhaseTimer(const GcPhaseSink & sink_, const char * phase_)
        : sink(sink_), phase(phase_), attached(CurrentThread::isInitialized())
    {
        if (attached)
            before = CurrentThread::getProfileEvents().getPartiallyAtomicSnapshot();
    }

    /// Record one phase-specific count. Overwrites a previous value for the same key.
    void metric(const String & key, UInt64 value) { metrics[key] = value; }

    ~GcPhaseTimer()
    {
        if (!sink)
            return;
        GcPhaseRecord rec;
        rec.phase = phase;
        rec.duration_us = watch.elapsedMicroseconds();
        rec.metrics = std::move(metrics);
        if (attached)
        {
            const auto after = CurrentThread::getProfileEvents().getPartiallyAtomicSnapshot();
            for (ProfileEvents::Event e = ProfileEvents::Event(0); e < ProfileEvents::Counters::num_counters; ++e)
            {
                const auto delta = after[e] - before[e];
                if (delta != 0)
                    rec.profile_events.emplace(String(ProfileEvents::getName(e)), static_cast<UInt64>(delta));
            }
        }
        /// Best-effort, exactly like the round-row sink: instrumentation must never break GC.
        try { sink(rec); } catch (...) {}   // NOLINT(bugprone-empty-catch)
    }

private:
    const GcPhaseSink & sink;
    const char * phase;
    bool attached;
    Stopwatch watch{CLOCK_MONOTONIC};
    ProfileEvents::Counters::Snapshot before;
    std::map<String, UInt64> metrics;
};

}
```

- [x] **Step 2: Instrument `lease` and `heartbeat_floor`**

In `runRegularRound`, wrap `acquireOrRenewLease` (`:274`):

```cpp
    RoundReport report;
    GcState state;
    Token state_token;
    {
        GcPhaseTimer t(phase_sink, "lease");
        report.acquired_lease = acquireOrRenewLease(state, state_token, allow_steal);
        t.metric("acquired", report.acquired_lease ? 1 : 0);
        t.metric("steal_allowed", allow_steal ? 1 : 0);
    }
    if (!report.acquired_lease)
        return report;
```

and the heartbeat floor (`:306-341`) in a `{ GcPhaseTimer t(phase_sink, "heartbeat_floor"); … }` block
recording `t.metric("live", floor.live)`, `"terminated"`, `"fenced_now"`, `"already_fenced"`.
`floor` must be hoisted out of the block (declare `HeartbeatFloor floor;` before it and assign inside)
because it is read later.

- [x] **Step 3: Instrument `defer_decision`**

Rewrite `CasGc.cpp:347-381` (the block Task 2 already touched). `pre_scan` must OUTLIVE the timer
block because the fold consumes it, and `report.deferred` must be set before the timer's destructor
runs — hence the explicit inner scope rather than wrapping the whole `if`:

```cpp
    RefScanSummary pre_scan;
    {
        GcPhaseTimer t(phase_sink, "defer_decision");
        const bool graduation_due = graduationDue(state, new_round);
        pre_scan = preFoldRefScan(state);
        const size_t changed = pre_scan.changed_shards;
        const bool defer = shouldDeferRound(changed, graduation_due, rounds_since_last_fold_,
                                            store->poolConfig().gc_fold_threshold,
                                            store->poolConfig().gc_fold_max_defer_rounds);

        UInt64 ref_keys = 0;
        for (const auto & [ns_str, ids] : pre_scan.logs_by_ns)
            ref_keys += ids.size();
        t.metric("changed_shards", changed);
        t.metric("namespaces_seen", pre_scan.max_log_by_ns.size());
        t.metric("ref_log_keys_listed", ref_keys);
        t.metric("graduation_due", graduation_due ? 1 : 0);
        t.metric("deferred", defer ? 1 : 0);
        t.metric("rounds_since_last_fold", rounds_since_last_fold_);

        if (defer)
        {
            ++rounds_since_last_fold_;
            report.deferred = true;
            report.round = state.round;   /// the honest, already-durable round (see the existing comment)
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcFence;
                e.object_kind = CasEventObjectKind::Snap;
                e.round = state.round;
                e.gen = state.snap_generation;
                e.outcome = "deferred";
                e.reason = "skip-unchanged: no changed shard reached the fold threshold and no graduation "
                           "is due; re-adopting the sealed generation (snapshot rebuild elided)";
                e.detail = {{"changed_shards", std::to_string(changed)},
                            {"rounds_since_last_fold", std::to_string(rounds_since_last_fold_)}};
            });
            return report;   /// the timer's destructor emits the defer_decision row on the way out
        }
        rounds_since_last_fold_ = 0;   /// this round folds
    }
```

Keep the existing long comment above `report.round = state.round;` verbatim — it explains why a
deferred round must not print `new_round`, and deleting it would lose that reasoning.

- [x] **Step 4: Instrument the seven post-fold phases**

Same pattern, one `{ GcPhaseTimer t(phase_sink, "<name>"); … }` block each:

| Wrap | `phase` | Metrics |
|---|---|---|
| `:424-612` (the pre-CAS delete + outcome-log loops) | `pending_deletes` | `redeleted`, `deleted`, `absent`, `replaced`, `spared` from `report` deltas |
| `:619` `meta_pool->wait()` | `meta_pool_wait` | `jobs_scheduled`, `jobs_completed` (counters incremented in `scheduleMetaJob` and its completion path) |
| `:627-651` (`pruneSupersededGenerations` + the `gc/state` CAS) | `round_commit` | `generations_pruned`, `pruned_through` |
| `:675-697` | `handoff_reclaim` | `generations_reclaimed`, `objects_reclaimed` |
| `:702-720` | `manifest_deletes` | `attempted`, `deleted` |
| `:736` `runNamespaceCleanupPasses` | `namespace_cleanup` | `items` (size of `folded.fold_seal.ns_cleanup_items`), `suppressed` |
| `:737-738` `cleanupRefObjects` | `ref_object_cleanup` | `suppressed`, `trim_enabled` |
| `:741-748` `runManifestSweepCursorPass` | `orphan_sweep` | `cursor_advanced` |

For `meta_pool_wait`, add two `std::atomic<uint64_t>` members to `Gc` (`meta_jobs_scheduled_`,
`meta_jobs_completed_`), incremented in `scheduleMetaJob` (`:198`) at submission and at the end of the
job lambda. Snapshot both at the start of the round and report the deltas.

- [x] **Step 5: Build, gate, and eyeball the rows**

```bash
ninja -C build unit_tests_dbms > build/build_task6.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task6.log 2>&1; echo EXIT=$?
```
Expected: `NINJA_EXIT=0`, `EXIT=0`.

- [x] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcPhaseTimer.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.{h,cpp}
cat > /tmp/msg6.txt <<'EOF'
ca: instrument the ten round-level GC phases

`GcPhaseTimer` emits one row per phase with a microsecond duration, the phase's ProfileEvents delta
(a plain snapshot diff, so it composes with the round-level `ProfileEventsScope` instead of
re-parenting the thread's counters), and phase-specific counts. Ten phases from `runRegularRound`:
lease, heartbeat_floor, defer_decision, pending_deletes, meta_pool_wait, round_commit,
handoff_reclaim, manifest_deletes, namespace_cleanup, ref_object_cleanup, orphan_sweep.

`meta_pool_wait` gets explicit `jobs_scheduled`/`jobs_completed` counts because the meta pool's own
work runs on other threads and cannot appear in this thread's ProfileEvents delta — that is the seam
the throughput-collapse RCA predicts the fold thread parks on, so it must not read as a blank.
EOF
git commit -F /tmp/msg6.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcPhaseTimer.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
git show --stat HEAD
```

---

## Task 7: The seven fold phases, carrying the detector's numbers

**STATUS: DONE 2026-07-26** (`d412f85f749`).

**STATUS: IN PROGRESS** — see Task 5's status note. Not landed.

**One thing this task's tables cannot know, because the detector landed after it was written:** all
four members it reads now exist and are reset per round (`e01b5cd82be`), so the metrics below are
wiring, not new computation. The `fold_ref_intake` row's `logs_intended`/`logs_applied` and the
`fold_reduce` row's `txns_unapplied` are 0-or-equal on every COMMITTED round by construction — a
nonzero `txns_unapplied` throws before the seal — which is exactly what makes them worth emitting:
"they are always equal" becomes observable rather than assumed.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/Gc/CasGc.cpp` (`fold`, `:833-1489`)

- [x] **Step 1: Instrument the fold's phases**

Same `{ GcPhaseTimer t(phase_sink, "<name>"); … }` pattern:

| Wrap | `phase` | Metrics |
|---|---|---|
| `:842-877` + the probe-A block from Task 2 | `fold_ref_list` | `ref_keys_listed` = `ref_object_keys.size()`, `namespaces_seen` = `ref_tables.size()`, `probe_a_holes` = `probe_a_holes_this_round`, `ref_folding_aborted` |
| `:890-898` and `:1036-1038` (one timer covering both reads) | `fold_seal_read` | `parent_runs` = `discover_ref_seal.blob_target_runs.size()`, `ns_cleanup_items`, `redundant_reads` = 2 |
| `:1049-1219` | `fold_ref_intake` | `logs_intended` / `logs_applied` (Task 3's counters), `deltas_emitted` = `deltas.size()`, `clamps`, `dead_precommits_skipped` |
| `:1259-1294` | `fold_ns_cleanup_scan` | `items_carried`, `items_completed`, `items_retired` |
| `:1384-1444` | `fold_reduce` | `shards_reduced`, `shards_pure_carry`, `deltas_in`, `condemned` = `report.condemned` delta, `graduated`, `spared`, `txns_unapplied` = `txns_unapplied_this_round` |
| `:1479-1480` | `fold_seal_write` | `seal_runs` = `result.fold_seal.blob_target_runs.size()` |

Note the `fold_seal_read` row's `redundant_reads` metric is deliberate: `adopted_seal` (`:890`) and
`discover_ref_seal` (`:1037`) read the same key at the same `(generation, attempt)`. Add the comment
`/// One redundant GET per round: both reads resolve the same key. Instrumented, not fixed — the
follow-up study decides.` above the second read.

- [x] **Step 2: Extend the stateless test**

Read `tests/queries/0_stateless/05007_content_addressed_gc_introspection.sh` in full, then append a
check that phase rows appear and correlate:

```sql
SELECT count() > 0
FROM system.content_addressed_garbage_collection_log
WHERE event_type = 'Phase' AND phase = 'fold_ref_list';

SELECT count(DISTINCT round_id) = 1
FROM system.content_addressed_garbage_collection_log
WHERE round_id = (SELECT round_id FROM system.content_addressed_garbage_collection_log
                  WHERE event_type = 'Finish' ORDER BY event_time_microseconds DESC LIMIT 1);
```

Update `05007_content_addressed_gc_introspection.reference` accordingly.

- [x] **Step 3: Build, gate, run the stateless test**

```bash
ninja -C build unit_tests_dbms clickhouse > build/build_task7.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task7.log 2>&1; echo EXIT=$?
python3 -m ci.praktika run "stateless" --test 05007_content_addressed_gc_introspection > build/test_task7_stateless.log 2>&1; echo EXIT=$?
```
Expected: all zero. Have a subagent summarise each log.

- [x] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
        tests/queries/0_stateless/05007_content_addressed_gc_introspection.sh \
        tests/queries/0_stateless/05007_content_addressed_gc_introspection.reference
cat > /tmp/msg7.txt <<'EOF'
ca: instrument the seven fold phases; surface the detector's numbers as phase metrics

fold_ref_list (carrying probe A's hole count), fold_seal_read, fold_ref_intake (probe B1's
logs_intended/logs_applied), fold_ns_cleanup_scan, fold_reduce (probe B2's txns_unapplied),
fold_seal_write. The probes' numbers are visible on every healthy round, so "they are always equal"
becomes an observable property rather than an assumption.

Records one finding the enumeration surfaced and does not fix: the fold reads the adopted seal twice
at the same (generation, attempt) — one redundant GET per round, now visible on the fold_seal_read row.
EOF
git commit -F /tmp/msg7.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp tests/queries/0_stateless/05007_content_addressed_gc_introspection.sh tests/queries/0_stateless/05007_content_addressed_gc_introspection.reference
git show --stat HEAD
```

---

## Task 8: Document the new columns

**STATUS: DONE 2026-07-26** — `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` carries the phase columns.

**STATUS: IN PROGRESS** — see Task 5's status note. Not landed.

**Files:**
- Modify: `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md`

- [x] **Step 1: Read the existing doc**

Read the whole file, noting its heading style and whether every heading already carries a
`{#kebab-case-anchor}` (the project rule requires it; if an existing heading lacks one, add it).

- [x] **Step 2: Add the four columns and a phase section**

Add the four column descriptions matching `getColumnsDescription` verbatim, then append the section
below (shown here inside a four-backtick fence so its own SQL fences are literal):

````markdown
## Per-phase rows {#per-phase-rows}

Besides the `Start` and `Finish` row of each round, the collector emits one `Phase` row per GC phase.
Every row of one round attempt — `Start`, each `Phase`, and `Finish` — shares a `round_id`.

Which phase dominates a round:

```sql
SELECT phase,
       count() AS rounds,
       quantile(0.5)(phase_duration_us) AS p50_us,
       quantile(0.99)(phase_duration_us) AS p99_us,
       sum(phase_duration_us) AS total_us
FROM system.content_addressed_garbage_collection_log
WHERE event_type = 'Phase' AND disk_name = 'ca'
GROUP BY phase
ORDER BY total_us DESC;
```

One round, in order:

```sql
SELECT phase, phase_duration_us, phase_metrics, ProfileEvents['S3ListObjects'] AS lists
FROM system.content_addressed_garbage_collection_log
WHERE round_id = '...' AND event_type = 'Phase'
ORDER BY event_time_microseconds;
```

Caveat: work scheduled onto the GC meta pool runs on other threads, so the `meta_pool_wait` row's
`ProfileEvents` delta is empty by construction. Read its `phase_metrics` `jobs_scheduled` /
`jobs_completed` next to its duration instead.
````

- [x] **Step 3: Commit**

```bash
git add docs/en/operations/system-tables/content_addressed_garbage_collection_log.md
cat > /tmp/msg8.txt <<'EOF'
docs: per-phase rows in system.content_addressed_garbage_collection_log
EOF
git commit -F /tmp/msg8.txt -- docs/en/operations/system-tables/content_addressed_garbage_collection_log.md
git show --stat HEAD
```

---

## Task 9: S42 — consistency assertions decide the verdict

**STATUS: DONE** — `402a85c4a64`.

**Corrections and additions from executing it:**
- **The docstring Step 6 rewrites asserted as FACT that S42 "cannot return a conclusive green".** That
  was true under the old guard and false after this task, so it had to be corrected as part of the
  change — a stale docstring contradicting the code is how a reader draws the wrong conclusion about a
  test they are relying on. Step 6's replacement text already handles this; do not reintroduce the
  old claim elsewhere in the file.
- The "two rows, not one" premise in this task's heading note was CONFIRMED and is the load-bearing
  part: fixing only the soundness guard would have left the card reading `skipped`, not green, because
  `SKIPPED` outranks `PASS`.
- Verified rather than assumed, and worth repeating on any change to this file: the harness pytest
  baseline is unchanged at 4 failed / 214 passed (the four are pre-existing); `Verdict.skipped` remains
  in use by OTHER cards, so the factory was not removed; and the five surviving `Verdict.inconclusive`
  sites are all genuinely-missing-data cases, exactly as Step 7 predicted.

Small and self-contained. **Two rows cap the run, not one** — the soundness guard at `:557` AND the
`Verdict.skipped` at `:518`, because `SKIPPED` outranks `PASS` in
`framework/report.py:21`.

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/report.py` (`:32-42`)
- Modify: `utils/ca-soak/scenarios/cards/s42_alloc_faults.py` (`:1-66`, `:517-530`, `:557-572`, `:673-684`)

- [x] **Step 1: Add the non-gating factory**

`framework/report.py`, after `skipped` (`:42`):

```python
    @staticmethod
    def reported(name: str, expected: str, observed, note: str = "") -> "Verdict":
        """A recorded observation that never gates the run status.

        Use ONLY where the metric is non-gating BY DESIGN (a characterisation number, or a signal
        that is structurally zero in the current build). Never as a way to soften an assertion that
        should fail, and never for data that is UNAVAILABLE -- that stays `inconclusive`, per the
        README rule that a missing observation must never be silently converted into a pass.
        """
        return Verdict(name, expected, str(observed), PASS, note)
```

- [x] **Step 2: Soundness guard becomes reported**

`s42_alloc_faults.py:557-572` — replace both branches:

```python
        if targeted == 0:
            result.add(Verdict.reported(
                "post-durable install window traversal (reported, not gating)",
                "> 0 targeted signals (CasRefApplyPoisoned transitions or post-PUT failpoint hits)",
                f"targeted=0 with {generic} generic allocation failures",
                f"the window was NOT proven traversed: a nonzero MEMORY_LIMIT_EXCEEDED count proves "
                f"only that SOME allocation failed, never that the few-instruction post-durable "
                f"install region was entered while armed. {failpoint_why}. With §A1 landed the region "
                f"allocates nothing, so this counter is EXPECTED to stay 0. Per the 2026-07-25 "
                f"decision this no longer gates: green means the consistency oracle held, not that "
                f"the target window was hit."))
        else:
            result.add(Verdict.reported(
                "post-durable install window traversal (reported, not gating)",
                "> 0 targeted signals (CasRefApplyPoisoned transitions or post-PUT failpoint hits)",
                f"targeted={targeted} (poison={poison_total}, failpoint={failpoint_hits})",
                "the window WAS reached — the poison verdict above is a conclusive statement about "
                "§A1 for this run, not a vacuous zero"))
```

- [x] **Step 3: The skipped verdict becomes reported**

`s42_alloc_faults.py:517-521` — this is the row the decision text does not mention and that would
otherwise cap the run at `skipped`:

```python
        if poison_total == 0:
            result.add(Verdict.reported(
                "no snapshot advanced across a poisoned transaction",
                "vacuous while poison_total == 0",
                "not exercised",
                "no transaction was poisoned this run, so nothing could advance across one. The "
                "snapshot integrity oracle above is the unconditional half of this check and DID "
                "run. Reported rather than skipped: `skipped` outranks `pass` in the harness, so a "
                "structurally-vacuous branch would otherwise cap every healthy run."))
```

The `else` branch (`:522-530`) is unchanged — a real `check`.

- [x] **Step 4: Retro-fit the two hand-built rows**

`s42_alloc_faults.py:673-684` — replace the two `Verdict(..., "pass")` constructions with
`Verdict.reported(...)`, keeping every string identical. Same behaviour, one convention.

- [x] **Step 5: Leave the generic anti-vacuity guard alone**

`:543-555` is unchanged. Add one line to its `else`-branch note so the reason it survived is on the
record:

```python
                "generic only — proves SOME allocation failed, NOT that the post-durable window was "
                "hit. This guard STAYS gating (2026-07-25 decision): a run in which no allocation "
                "fault occurred at all must never read green."
```

- [x] **Step 6: Rewrite the docstring**

`s42_alloc_faults.py:38-55` — replace the "Soundness guard (step 5, mandatory)" paragraph:

```
**What green means (2026-07-25 decision).** Green is A CONSISTENT STATE ON DISK AND IN MEMORY, not
proof that a fault landed in the post-durable install window. The verdict rests on the consistency
oracle: the post-restart (journal-rebuilt) view identical to the pre-restart view, every acked
block present, replicas agreeing, fsck `dangling`/`unaccounted`/`stale_edge` clean pre- and
post-restart, the snapshot integrity oracle clean, zero `LOGICAL_ERROR`, no wedged ref lane, GC
rounds succeeding after disarm.

**Anti-vacuity, which survives.** A run in which no allocation fault occurred at all still cannot
read green: `generic == 0` (client-visible injected failures plus the `QueryMemoryLimitExceeded`
delta) is `inconclusive`. Only the WINDOW-SPECIFIC targeting was dropped as a gate.

**The targeted signal is reported, not gating.**

  targeted = CasRefApplyPoisoned transitions + post-PUT apply failpoint hits

is structurally 0 today: the §A1 seam is the gtest-only `CasRefLedger::setInstallRegionProbeForTest`
with no `src/Common/FailPoint.cpp` registration, and `CasRefApplyPoisoned` is correctly 0 while §A1
holds. The card records it and says so. If poison ever DOES fire, the `CasRefApplyPoisoned == 0`
verdict is a real `check` and the run fails — that half is unchanged.
```

- [x] **Step 7: Verify the card parses and the verdict set is reachable**

```bash
python3 -c "import ast,sys; ast.parse(open('utils/ca-soak/scenarios/cards/s42_alloc_faults.py').read()); ast.parse(open('utils/ca-soak/scenarios/framework/report.py').read()); print('OK')"
grep -n "Verdict.skipped\|Verdict.inconclusive" utils/ca-soak/scenarios/cards/s42_alloc_faults.py
```
Expected: `OK`, and the surviving `Verdict.inconclusive` sites are only the genuinely-unavailable-data
ones (`:458` fsck field absent, `:482` oracle checked==0, `:544` generic anti-vacuity, `:605` probe
failed, `:658` no rounds seen). **No `Verdict.skipped` should remain in the file.**

- [x] **Step 8: Commit**

```bash
git add utils/ca-soak/scenarios/framework/report.py utils/ca-soak/scenarios/cards/s42_alloc_faults.py
cat > /tmp/msg9.txt <<'EOF'
ca-soak: S42's verdict rests on the consistency oracle, not on window-traversal proof

Per the 2026-07-25 decision: green is a consistent state on disk and in memory. The targeted
post-durable-install signal — structurally zero, because the seam is gtest-only — becomes REPORTED
instead of capping every healthy run at `inconclusive`. The generic anti-vacuity guard survives
unchanged, so a run with no injected allocation fault at all still cannot read green.

Two rows had to move, not one: the `Verdict.skipped` for "no snapshot advanced across a poisoned
transaction" also outranks `pass` in the harness, so fixing only the soundness guard would have left
the run reading `skipped`. New `Verdict.reported` factory, which also replaces the two hand-built
`Verdict(..., "pass")` rows the card already had.
EOF
git commit -F /tmp/msg9.txt -- utils/ca-soak/scenarios/framework/report.py utils/ca-soak/scenarios/cards/s42_alloc_faults.py
git show --stat HEAD
```

---

## Task 10: `proveForeignMountDead` — the certificate of death for a foreign mount slot

**STATUS: NOT STARTED — deliberately.** Blocked on the user's choice between overwriting the owner uuid and adopting the pool's existing one, and its CI motivation evaporated when the one-line CI fix landed. NOT part of the GC round.

> ### ⛔ TASKS 10-12 ARE BLOCKED — DO NOT START THEM {#tasks-10-12-blocked}
>
> **Blocked on a user decision recorded at BACKLOG {#operator-uuid-recovery}:** which reading of "force
> a new uuid" is wanted.
> - **Overwrite the owner uuid** — what Tasks 10-12 below implement. It works, and it PERMANENTLY locks
>   the original server out of the pool.
> - **Adopt the pool's existing owner uuid and mount AS it** — reaches the same "mount as WRITE despite
>   a differing local uuid" outcome with **no durable identity damage**, and
>   `Pool::openForDecommission` (`CasPool.cpp:720-776`) already does exactly this, so most of the work
>   exists.
>
> The BACKLOG entry says plainly that "the second reading looks strictly better for the stated need and
> the first should have to justify itself". This plan implements the first. That is the same question as
> {#escalate} item 1, and it is unmade.
>
> Two further facts that changed the ground under these tasks:
> - **Their CI motivation has evaporated.** The scrape is fixed by a one-line change to which file its
>   `sed` patches (BACKLOG {#ci-scrape-readonly-sed-fix}), landed in `d99a7df4540` + `8aea3a0dedc`, with
>   zero product change. What remains is a real OPERATOR need, not a CI one.
> - **The factual concern raised in BACKLOG {#q2-force-claim} is VOID.** It asked whether force-claiming
>   would be taking ownership from a genuinely LIVE server. The CI scrape runs against a STOPPED server
>   — stated in the code's own comment and confirmed at the call site (`ebbae78d739`). That removes the
>   objection to forcing in the CI case; it does not decide the reading above.
>
> Nothing in Tasks 10-12 has been started and nothing below is ticked.

The guard, landed before anything can use it.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/Pool/CasServerRoot.h` (next to `claimMountAwaitingExpiry`, `:414`)
- Modify: `src/Disks/.../ContentAddressed/Pool/CasServerRoot.cpp`
- Create: `src/Disks/tests/gtest_cas_force_owner_claim.cpp`

**Interfaces:**
- Produces: `struct Cas::ForeignMountDeath { enum Kind { Absent, CleanFarewell, GcFenced,
  ObservedStable, Live }; Kind kind; std::optional<Token> token; MountLease body; };` and
  `ForeignMountDeath proveForeignMountDead(...)`, consumed by Task 11.

- [ ] **Step 1: Read the observation loop being mirrored**

Read `claimMountAwaitingExpiry` in `Pool/CasServerRoot.cpp` in full, plus its contract at
`Pool/CasServerRoot.h:385-412` and `claimMount`'s at `:305-330`. The new helper must use the SAME
`mountObservationThresholdMs(ttl_ms, poll_interval_ms)` formula and the same bounded-restart
discipline; a divergent threshold here would be exactly the drift that formula exists to prevent.

- [ ] **Step 2: Declare the helper**

`Pool/CasServerRoot.h`, after `claimMountAwaitingExpiry` (`:422`):

```cpp
/// Evidence that a mount slot held under a FOREIGN `server_uuid` may be taken over. Used ONLY by the
/// opt-in force-claim path (`PoolConfig::force_owner_claim`); the ordinary claim path never consults
/// it and `claimMount`'s "different server_uuid -> ForeignOwner, do NOT write" rule is untouched.
///
/// Mirrors `claimMount`'s certificate-of-death discipline exactly: a bare `expires_at_ms <= now_ms`
/// reading is NEVER sufficient, because comparing a predecessor's stamp against our own wall clock is
/// unsafe under clock skew or a merely late-observing caller. Only an authoritative absence, the
/// predecessor's own farewell sentinel, a GC fence, or OUR OWN monotonic observation of an unchanged
/// write token across the full `mountObservationThresholdMs` window counts.
///
/// READ-ONLY: this function never writes. A `Live` result means the caller must refuse having
/// mutated nothing.
struct ForeignMountDeath
{
    enum Kind
    {
        Absent,           /// no mount object at all
        CleanFarewell,    /// `min_active == UINT64_MAX` — the predecessor's own graceful stop
        GcFenced,         /// the GC leader already, itself, threshold-gated this incarnation dead
        ObservedStable,   /// we watched this exact token hold for the full observation threshold
        Live,             /// no certificate — refuse
    };
    Kind kind = Live;
    std::optional<Token> token;   /// the observed token, for the caller's token-guarded takeover
    MountLease body;
};

ForeignMountDeath proveForeignMountDead(
    Backend & b, const Layout & l, const String & srid,
    const std::function<uint64_t()> & mono_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn);
```

- [ ] **Step 3: Implement it**

In `Pool/CasServerRoot.cpp`, alongside `claimMountAwaitingExpiry`:

```cpp
ForeignMountDeath proveForeignMountDead(
    Backend & b, const Layout & l, const String & srid,
    const std::function<uint64_t()> & mono_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn)
{
    static constexpr int kMaxObservationRestarts = 3;   /// same bound as claimMountAwaitingExpiry
    const uint64_t threshold_ms = mountObservationThresholdMs(ttl_ms, poll_interval_ms);
    const String key = l.mountKey(srid);

    for (int restart = 0; restart <= kMaxObservationRestarts; ++restart)
    {
        const auto got = b.get(key);
        if (!got)
            return ForeignMountDeath{.kind = ForeignMountDeath::Absent};

        const MountLease body = decodeMountLease(got->bytes);
        if (body.gc_fenced)
            return ForeignMountDeath{.kind = ForeignMountDeath::GcFenced, .token = got->token, .body = body};
        if (body.min_active == std::numeric_limits<uint64_t>::max())
            return ForeignMountDeath{.kind = ForeignMountDeath::CleanFarewell, .token = got->token, .body = body};

        /// No positive certificate: watch the write token on OUR OWN monotonic clock.
        const Token watched = got->token;
        const uint64_t started_mono_ms = mono_ms_fn();
        bool changed = false;
        while (mono_ms_fn() - started_mono_ms < threshold_ms)
        {
            sleep_ms_fn(poll_interval_ms);
            const auto again = b.get(key);
            if (!again)
                return ForeignMountDeath{.kind = ForeignMountDeath::Absent};
            if (!(again->token == watched))
            {
                changed = true;   /// the holder renewed (or a twin is alive) — restart the observation
                break;
            }
        }
        if (!changed)
            return ForeignMountDeath{.kind = ForeignMountDeath::ObservedStable, .token = watched, .body = body};
    }
    /// A holder whose token keeps changing across that many restarts is alive, not dead.
    return ForeignMountDeath{.kind = ForeignMountDeath::Live};
}
```

- [ ] **Step 4: Test it with a fake clock**

Create `src/Disks/tests/gtest_cas_force_owner_claim.cpp` with four tests, all against an
`InMemoryBackend` and injected clocks so nothing really sleeps:

```cpp
TEST(CasForceOwnerClaim, AbsentMountSlotIsProvenDeadImmediately)      /// no mount object
TEST(CasForceOwnerClaim, CleanFarewellIsProvenDeadImmediately)        /// min_active == UINT64_MAX
TEST(CasForceOwnerClaim, GcFencedIsProvenDeadImmediately)             /// gc_fenced = true
TEST(CasForceOwnerClaim, StableTokenAcrossTheThresholdIsProvenDead)   /// fake mono clock jumps past it
TEST(CasForceOwnerClaim, RenewingHolderIsNeverProvenDead)             /// mutate the body each poll -> Live
```

Each test asserts the `Kind` and, for the three proven kinds, that `token` is set. The renewing-holder
test must also assert the backend saw no write (`InMemoryBackend` exposes its key map; compare the
mount object's token before and after).

- [ ] **Step 5: Build, run, gate**

```bash
ninja -C build unit_tests_dbms > build/build_task10.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasForceOwnerClaim.*' > build/test_task10.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task10_gate.log 2>&1; echo EXIT=$?
```
Expected: all zero.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.{h,cpp} \
        src/Disks/tests/gtest_cas_force_owner_claim.cpp
cat > /tmp/msg10.txt <<'EOF'
ca: proveForeignMountDead — a read-only certificate of death for a foreign mount slot

The guard the force-claim path will need, landed first and on its own. Absent / clean-farewell /
gc_fenced decide with one GET; anything else enters the same token-stability observation
`claimMountAwaitingExpiry` uses, on the same `mountObservationThresholdMs` formula and the same
restart bound, so the two paths cannot drift apart. A renewing holder returns Live and the function
never writes, so a refusal costs nothing durable. `claimMount`'s ForeignOwner invariant is untouched.
EOF
git commit -F /tmp/msg10.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_force_owner_claim.cpp
git show --stat HEAD
```

---

## Task 11: The force path in `mountWritable`

**STATUS: NOT STARTED** — see Task 10.

**STATUS: BLOCKED — see {#tasks-10-12-blocked}.** This task is where the unmade choice is isolated
(`Pool::forceOwnerClaim`), which is exactly why it must not be written before the choice is made.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/Pool/CasPool.h` (`PoolConfig`, near `read_only` at `:120`)
- Modify: `src/Disks/.../ContentAddressed/Pool/CasPool.cpp` (`mountWritable`, `:466-500`)
- Modify: `src/Disks/tests/gtest_cas_force_owner_claim.cpp`

- [ ] **Step 1: Add the config flag**

`Pool/CasPool.h`, in `PoolConfig` next to `read_only`:

```cpp
    /// OPT-IN, never a default. On a writable open whose configured `server_id` differs from the
    /// pool's persisted owner uuid, override the identity refusal and mount as WRITE anyway.
    ///
    /// This PERMANENTLY REASSIGNS the pool's identity: the previous owner, on its next start, presents
    /// the old uuid and is refused by `claimOwnerOrThrow` until an operator restores the owner object
    /// or reconfigures `server_root_id`. It is therefore gated on a certificate of death for the mount
    /// slot (`proveForeignMountDead`) — a LIVE holder refuses with `ABORTED`, having written nothing —
    /// and it logs the displaced uuid at WARNING plus an audit event.
    bool force_owner_claim = false;
```

- [ ] **Step 2: Implement the force path**

`Pool/CasPool.cpp`, in `mountWritable`, replacing the step-2 comment and call at `:488-491`:

```cpp
    /// 2. Owner anchor — IDENTITY (clock-free). A foreign uuid fails closed; an absent owner over a
    ///    non-empty subtree is CORRUPTED_DATA; a fresh empty root is claimed.
    ///
    ///    FORCE-CLAIM (opt-in, `PoolConfig::force_owner_claim`): the uuid appears in TWO durable
    ///    objects and the refusal fires from both, so forcing only the owner anchor would rewrite the
    ///    pool's identity and then still be refused by `claimMount` as `ForeignOwner` — a graceful
    ///    shutdown stamps the farewell sentinel into the mount object and leaves it in place carrying
    ///    the predecessor's uuid. The force therefore covers both, in this order, which is load-bearing:
    ///
    ///      (a) prove the foreign mount slot dead — READ ONLY, so a live holder refuses with nothing
    ///          written;
    ///      (b) overwrite the owner anchor, token-conditional so a concurrent legitimate claim wins;
    ///      (c) `allocateWriterEpoch` — UNCHANGED, and it must run BEFORE (d): its absent-epoch branch
    ///          refuses to re-mint `writer_epoch 1` while a mount object exists, and deleting the mount
    ///          first would disarm that guard;
    ///      (d) exact-token delete of the stale foreign mount object, so a predecessor that renewed
    ///          between (a) and here causes a miss and we refuse;
    ///      (e) the normal `claimMount`, which now sees an absent slot. `claimMount` ITSELF IS NOT
    ///          MODIFIED — the force concept never enters that primitive.
    std::optional<Token> forced_mount_token;
    if (store->config.force_owner_claim)
        forced_mount_token = forceOwnerClaim(store, srid, our_uuid);
    claimOwnerOrThrow(*store->pool_backend, store->pool_layout, srid, our_uuid);
```

Add the private static helper implementing (a) and (b):

```cpp
std::optional<Token> Pool::forceOwnerClaim(PoolPtr & store, const String & srid, UInt128 our_uuid)
{
    Backend & b = *store->pool_backend;
    const Layout & l = store->pool_layout;

    const auto owner_obj = b.get(l.ownerKey(srid));
    if (!owner_obj)
        return std::nullopt;                       /// nothing to force: the normal claim path handles it
    const OwnerObject owner = decodeOwner(owner_obj->bytes);
    if (owner.server_uuid == our_uuid)
        return std::nullopt;                       /// already ours

    const uint64_t ttl_ms = static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count());
    const uint64_t poll_ms = static_cast<uint64_t>(store->config.mount_renew_period.count());
    const ForeignMountDeath death = proveForeignMountDead(
        b, l, srid,
        [store]() { return store->bootMsNow(); },
        ttl_ms, poll_ms,
        [](uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });

    if (death.kind == ForeignMountDeath::Live)
        throw Exception(ErrorCodes::ABORTED,
            "CAS server-root '{}': force_owner_claim refused — the mount slot is held by a LIVE "
            "incarnation of server_uuid={} (writer_epoch {}, pid {}, host {}). Nothing was written. "
            "Stop that server, or wait for the GC leader to fence its expired lease, and retry.",
            srid, u128ToHex(owner.server_uuid), death.body.writer_epoch, death.body.pid, death.body.hostname);

    LOG_WARNING(getLogger("CasPool"),
        "CAS server-root '{}': FORCE-CLAIMING ownership from server_uuid={} to server_uuid={} "
        "(mount-slot death certificate: {}). This permanently reassigns the pool's identity — the "
        "previous owner will be refused at its next start until its owner object is restored or its "
        "server_root_id is reconfigured.",
        srid, u128ToHex(owner.server_uuid), u128ToHex(our_uuid), foreignMountDeathName(death.kind));

    const CasResult res = b.casPut(l.ownerKey(srid),
        encodeOwner(OwnerObject{.server_uuid = our_uuid, .retired_at_ms = std::nullopt}),
        owner_obj->token);
    if (res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS server-root '{}': force_owner_claim lost a race — the owner object moved between the "
            "death check and the takeover. Nothing was taken over; retry.", srid);

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MountForceClaim;
        e.object_kind = CasEventObjectKind::Snap;
        e.outcome = "forced";
        e.reason = "force_owner_claim: identity reassigned after a mount-slot death certificate";
        e.detail = {{"srid", srid},
                    {"displaced_server_uuid", u128ToHex(owner.server_uuid)},
                    {"new_server_uuid", u128ToHex(our_uuid)},
                    {"certificate", foreignMountDeathName(death.kind)}};
    });
    return death.token;
}
```

Add `MountForceClaim` to the `CasEventType` enum and `foreignMountDeathName` next to
`ForeignMountDeath` in `Pool/CasServerRoot.h`.

- [ ] **Step 3: Delete the stale slot before `claimMount`**

In `mountWritable`, immediately before step 4's `claimMount` / `claimMountAwaitingExpiry` call:

```cpp
    /// (d) The stale foreign slot, removed token-conditionally now that the durable epoch has been
    ///     bumped (see the ordering note on step 2). A miss means the predecessor renewed after our
    ///     observation — refuse rather than claim over a live holder.
    if (forced_mount_token)
    {
        const DeleteOutcome del = store->pool_backend->deleteExact(
            store->pool_layout.mountKey(srid), *forced_mount_token);
        if (classifyDeleteOutcome(del) == DeleteClass::Replaced)
            throw Exception(ErrorCodes::ABORTED,
                "CAS server-root '{}': force_owner_claim refused at the mount slot — its token changed "
                "after the death observation, so the predecessor is alive. The owner anchor was already "
                "reassigned; restore it or retry once that server is stopped.", srid);
    }
```

- [ ] **Step 4: Tests**

Add to `src/Disks/tests/gtest_cas_force_owner_claim.cpp`:

```cpp
TEST(CasForceOwnerClaim, ForeignOwnerRefusedWithoutTheFlag)        /// CORRUPTED_DATA, owner unchanged
TEST(CasForceOwnerClaim, ForeignOwnerWithCleanFarewellIsTakenOver) /// owner rewritten, mount claimed, WRITE mount works
TEST(CasForceOwnerClaim, LiveForeignMountRefusesAndWritesNothing)  /// ABORTED; owner object byte-identical afterwards
TEST(CasForceOwnerClaim, WriterEpochIsNotResetByATakeover)         /// epoch after > epoch before
```

The third is the important one: assert the owner object's bytes AND token are unchanged after the
refusal, so a failed force leaves no durable trace.

- [ ] **Step 5: Build, run, gate**

```bash
ninja -C build unit_tests_dbms > build/build_task11.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasForceOwnerClaim.*' > build/test_task11.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task11_gate.log 2>&1; echo EXIT=$?
```
Expected: all zero.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.{h,cpp} \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.{h,cpp} \
        src/Disks/tests/gtest_cas_force_owner_claim.cpp
cat > /tmp/msg11.txt <<'EOF'
ca: opt-in mount-time force-claim over a differing server uuid

`PoolConfig::force_owner_claim` overrides the owner-identity refusal and mounts as WRITE. The uuid
lives in TWO durable objects, so forcing only the owner anchor would rewrite the pool's identity and
still be refused by `claimMount` as ForeignOwner — a graceful shutdown leaves the mount object in
place carrying the predecessor's uuid. The force covers both, ordered: prove the mount slot dead
(read-only, so a live holder refuses with nothing written) -> token-conditional owner overwrite ->
`allocateWriterEpoch` unchanged (BEFORE the slot delete, so its refuse-to-re-mint-epoch-1 guard stays
armed) -> exact-token delete of the stale slot -> the ordinary `claimMount`, which now sees an absent
slot. `claimMount` itself is untouched.

Loud by construction: a WARNING naming the displaced uuid and the death certificate, plus an audit
event. It permanently reassigns the pool's identity, and both the config comment and the log line say
so.
EOF
git commit -F /tmp/msg11.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/tests/gtest_cas_force_owner_claim.cpp
git show --stat HEAD
```

---

## Task 12: Expose the setting and document it

**STATUS: NOT STARTED** — see Task 10.

**STATUS: BLOCKED — see {#tasks-10-12-blocked}.** The setting's NAME and its warning text both depend on
which reading wins, so this cannot be written ahead of Task 11.

**Files:**
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedSettings.cpp` (`:76-83`)
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedMetadataStorage.h` (near `:591-592`)
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedMetadataStorage.cpp` (`:286`, `:711-713`)
- Modify: the CA disk settings documentation page

- [ ] **Step 1: Declare the setting**

`ContentAddressedSettings.cpp`, in the `DECLARE` list next to `skip_access_check` (`:76`):

```cpp
    DECLARE(Bool,   force_owner_claim, false, "Override the owner-uuid identity refusal at mount and claim this server-root as WRITE. Opt-in, never a default: it PERMANENTLY reassigns the pool's identity, and it refuses (writing nothing) unless the mount slot presents a certificate of death.", 0) \
```

- [ ] **Step 2: Thread it through**

`ContentAddressedMetadataStorage.h`, next to `const bool skip_access_check;`:

```cpp
    /// Per-disk `<force_owner_claim>` policy passed to `Cas::PoolConfig`. See its doc comment there.
    const bool force_owner_claim;
```

`ContentAddressedMetadataStorage.cpp:286`, in the initialiser list after `skip_access_check`:

```cpp
    , force_owner_claim(settings_[ContentAddressedSetting::force_owner_claim].value)
```

and in `openPoolView` after `pool_config.skip_access_check = skip_access_check;` (`:713`):

```cpp
    pool_config.force_owner_claim = force_owner_claim;
```

- [ ] **Step 3: Document it**

Find the page that documents the CA disk settings:

```bash
grep -rln "skip_access_check" docs/en/ | head
```

Add a row/section for `force_owner_claim` with the same warning the config comment carries: it
permanently reassigns identity, it refuses against a live mount slot, and the previous owner will be
locked out until its owner object is restored or its `server_root_id` reconfigured. Every heading
added must carry a `{#kebab-case-anchor}`.

- [ ] **Step 4: Build and gate**

```bash
ninja -C build unit_tests_dbms clickhouse > build/build_task12.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*' > build/test_task12.log 2>&1; echo EXIT=$?
```
Expected: both zero.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp} \
        docs/en/
cat > /tmp/msg12.txt <<'EOF'
ca: expose `force_owner_claim` as a per-disk setting and document it
EOF
git commit -F /tmp/msg12.txt -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp docs/en
git show --stat HEAD
```

---

## Decisions the executor must NOT make alone {#escalate}

These are recorded as open questions in the spec and are user decisions, not implementation details.
If any of them blocks a step, stop and ask.

1. **Item 4: overwrite or impersonate?** — **STILL OPEN, and it is what blocks Tasks 10-12**
   ({#tasks-10-12-blocked}). The plan implements the literal reading (overwrite the owner
   uuid). `Pool::openForDecommission` (`CasPool.cpp:720-776`) already implements the alternative —
   read the pool's existing owner uuid and mount AS it — which reaches the same "mount as WRITE despite
   a differing uuid" outcome without permanently reassigning identity. Task 11 isolates the choice to
   `Pool::forceOwnerClaim`, so switching is a small edit. BACKLOG {#operator-uuid-recovery} records
   both readings and states that the second looks strictly better for the stated need.
2. ~~**The CI `sed` one-liner.**~~ — **DONE, and it went further than described.**
   `d99a7df4540` patches `config.d` as well as `config.xml` AND makes a future no-op LOUD (a warning
   naming the consequence when a CA disk is declared but the marker is absent afterwards); then
   `8aea3a0dedc` stops naming a path at all — `grep` locates the files by their marker and `xargs -r`
   patches exactly those, which is a clean no-op when there are none. Both were verified by SIMULATING
   the substitution against the real config files rather than by reading the `sed` and believing it.
   Recorded at BACKLOG {#ci-scrape-readonly-sed-fix}. **Consequence for this plan: it removed the CI
   justification for item 4, which is why that item is now an operator need rather than a CI one.**
3. **Probe B2's throw wedges GC** until `SYSTEM CONTENT ADDRESSED GC REBUILD`. On the soak stand that
   costs the disk budget. Suppress-and-count on first occurrence instead? — **STILL OPEN, and now
   concrete rather than hypothetical: the throw is landed (`e01b5cd82be`).** Probe A's disagreement
   takes the softer path (abort ref folding for the round, no cursor advance, no destructive action),
   but probe B2's verdict throws `CORRUPTED_DATA` before the seal write and the whole round evaporates.
   Decide before the Part B soak, not during it.
4. **Does S42 become a soak gate** now that it can read green? — **STILL OPEN.** Task 9 is landed
   (`402a85c4a64`), so the precondition ("now that it can read green") is satisfied and the question is
   live.
