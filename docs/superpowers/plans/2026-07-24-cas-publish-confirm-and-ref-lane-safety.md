# CAS publish-confirm relink + ref-lane exception safety — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close two defects on `cas-gc-rebuild`: (A) the ref lane can lose a durable transaction from
its in-memory view and then permanently hide it behind a snapshot — a data-loss class; (B) the
fetch-by-relink handoff commits the receiver's ref without proving the sender's blobs are still
protected (codex-6).

**Architecture:** Part A makes the region between "the ref-log object is durable" and "the runtime
records it" allocation-free at all three post-durable install sites, enforced by
`DENY_ALLOCATIONS_IN_SCOPE`, with an apply-pending poison marker as defense-in-depth. Part B splits
the relink receiver into publish → confirm → promote: the receiver's `+1` becomes durable first,
then one read-only interserver query proves the sender's exact manifest is still the committed
binding, and only then does the receiver promote. No GC-side changes anywhere.

**Tech Stack:** C++ (ClickHouse fork), gtest (`unit_tests_dbms`), TLA+/TLC
(`docs/superpowers/models`, jar at `tmp/tla2tools.jar`), pytest integration tests
(`tests/integration`), the ca-soak scenario suite (`utils/ca-soak/scenarios`).

**Spec:** `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` (rev.5).

## Global Constraints

- **Branch discipline:** work on `cas-gc-rebuild`. Never rebase, never amend — add new commits.
- **Shared checkout:** another session commits to this same working tree. **Always commit with a
  pathspec** — `git commit -F <msgfile> -- <paths>` — and check `git diff --cached --stat` for
  foreign staged content first. Verify `git show --stat HEAD` after every commit.
- **Allman braces**, and CAS naming conventions as in the surrounding code.
- **No `sleep` to fix races** in C++ code.
- **Pre-release, zero compat scaffolding**: no migration shims for on-disk formats.
- **Do not change** the GC fold, journal codec, snapshot format, sweeps, orphan protection, or
  anomaly/suppression semantics. Part A changes HOW a committed transaction is installed, never
  WHAT is committed.
- **gtest gate** after every task that touches C++:
  `build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'`
  — must stay at zero failures. Build first:
  `ninja -C build unit_tests_dbms > build/build_<task>.log 2>&1; echo NINJA_EXIT=$?` (never pass
  `-j`, never use `nproc`), and have a subagent summarize the log.
- **Errors:** `LOGICAL_ERROR` only for genuine programming invariants (it aborts under
  sanitizers); use `CORRUPTED_DATA` / `NETWORK_ERROR` / `ABORTED` per the existing CAS conventions.

## File Structure

**Part A — ref lane (all under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`):**
- `Pool/CasRefProtocol.h/.cpp` — add `RefTableState::swap` (noexcept member-wise swap). Owner of
  the state type; no behavioral change.
- `Pool/CasRefLedger.h` — add the poison enum + per-table field, the `PostDurableInstall` test
  hook phase, and the prepared-wedge plumbing.
- `Pool/CasRefLedger.cpp` — the three install sites: `commitRefChunk` (candidate + noexcept
  install + prepared wedge), the wedge-resolution arm, and the shared deny-region helper.
- `Pool/CasPartWriteTxn.cpp` — `precommitAdd` mint-tightening only.

**Part B — confirm protocol:**
- `Pool/CasRefLedger.h/.cpp` — `confirmExactRef` (the lane snapshot).
- `ContentAddressedMetadataStorage.h/.cpp` — storage-level confirm, prepared-relink surface.
- `ContentAddressedExchange.h` — two new interface methods (`ownsNamespace`, `confirmExactRef`)
  plus the prepared-relink handle type.
- `Parts/PartFolderAccess.h/.cpp` — split `publishEntries` into prepare / promote / abort.
- `src/Storages/MergeTree/DataPartsExchange.h/.cpp` — token minting (sender), confirm action,
  receiver flow split, `allow_ca_relink` capability + recursion brake, `to_detached` relink.

**Tests:** `src/Disks/tests/gtest_cas_ref_install_safety.cpp` (new, Part A),
`src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` (new, Part B),
`docs/superpowers/models/CaRelinkConfirmCore.tla` (+ cfgs),
`tests/integration/test_cas_replicated_relink/` (extended),
`utils/ca-soak/scenarios/cards/s42_alloc_faults.py` (new).

---

## Task 1: Pin the source-edge set property (`[UNMATCHED-MINUS-ONE]`)

Independent, zero risk. The whole design history leans on "in-degree is a SET, last-wins per key",
so an unmatched removal delta must be provably a per-key no-op. If someone ever changes the model to
a counter, this test must go red.

**Files:**
- Modify: `src/Disks/tests/gtest_cas_blob_indegree.cpp` (the in-degree fold test file — extend it;
  do NOT create a new file, all its fixtures live here)

**Interfaces:**
- Consumes (all already in that file's anonymous namespace, verified present):
  `bh(uint64_t) -> BlobRef`, `s(uint64_t) -> UInt128`, `headPresent(tok, size)`,
  `writeSourceEdgeRun(backend, layout, gen, attempt, shard, condemned, edges)`,
  `decodeRun(backend, run) -> DecodedRun{condemned, zero_markers, edges}`; and from
  `Gc/CasBlobInDegree.h`: `struct BlobDelta { BlobRef ref; UInt128 source_id; bool remove; }`,
  `foldDeltasIntoGeneration(...)`, `zeroInDegree(backend, runs)`.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Read the two existing tests this one is modelled on**

`TEST(CasBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)` (`gtest_cas_blob_indegree.cpp:56-74`)
already folds an unmatched removal; `TEST(CasThreeCursorMerge, SnapshotEdgesUnperturbedByRetired)`
(`:452-484`) shows the explicit "surviving edge rows are byte-identical" assertion via `decodeRun`.
The new test combines the two: an unmatched removal for blob H must not disturb H's OTHER edge.

- [ ] **Step 2: Write the test**

Append to `src/Disks/tests/gtest_cas_blob_indegree.cpp`, following the fold-call shape of the
`PlusMinusCancelToZeroDetectsCandidate` test verbatim (same `InMemoryBackend`/`Layout` setup, same
`foldDeltasIntoGeneration` argument order):

```cpp
/// [UNMATCHED-MINUS-ONE] pin. In-degree is a SET of source edges applied last-wins per
/// (ref, ManifestId, path) key -- NOT a counter. A removal delta whose matching activation was
/// never folded (reachable today via a false-404 at the activation fold plus a dead-build skip)
/// must therefore be a per-key NO-OP: it marks an already-absent edge absent and cannot strip a
/// sibling manifest's edge for the SAME blob. The whole "that interleaving is harmless" argument in
/// the publish-confirm design rests on this; if the model ever regresses to counter arithmetic this
/// test goes red and premature deletion becomes reachable again.
TEST(CasBlobInDegree, UnmatchedRemovalIsAPerKeyNoOpAndSparesSiblingEdges)
{
    InMemoryBackend backend;
    Layout layout{"p"};

    /// Generation 1: blob b1 is referenced by TWO distinct sources (two manifests).
    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, /*prior_runs=*/{}, /*new_generation=*/1, /*attempt=*/1,
                             /*shard=*/0,
                             {BlobDelta{bh(1), s(1), false}, BlobDelta{bh(1), s(2), false}},
                             runs1);

    /// Generation 2: fold a removal for a THIRD source that never had an activation folded.
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, runs1, /*new_generation=*/2, /*attempt=*/1, /*shard=*/0,
                             {BlobDelta{bh(1), s(99), true}},
                             runs2);

    /// Both original edges survive: the unmatched removal touched only its own (absent) key.
    const auto out = decodeRun(backend, runs2.at(0));
    ASSERT_EQ(out.edges.size(), 2u) << "an unmatched removal must not strip sibling edges";
    /// And the blob is NOT a deletion candidate.
    const auto zero = zeroInDegree(backend, runs2);
    EXPECT_TRUE(zero.empty()) << "b1 still has two live source edges";
}
```

Adjust `decodeRun`'s field access and the `foldDeltasIntoGeneration` defaulted trailing arguments to
match the file's current signature (read `CasBlobInDegree.h:244-254` before writing).

- [ ] **Step 3: Run it — it must PASS immediately**

Run: `build/src/unit_tests_dbms --gtest_filter='CasBlobInDegree.UnmatchedRemovalIsAPerKeyNoOpAndSparesSiblingEdges'`
Expected: PASS. This is a characterization test of existing correct behavior, not red-then-green.
If it FAILS, STOP the whole plan and report — the design history's premise would be wrong.

- [ ] **Step 4: Commit**

```bash
git commit -F <msgfile> -- src/Disks/tests/gtest_cas_blob_indegree.cpp
```
Message subject: `ca: pin the source-edge set model — an unmatched removal is a per-key no-op`

---

## Task 2: Benchmark baseline for the install restructure (measurement gate, part 1)

The current COW shape was justified by numbers; capture them BEFORE touching the lane.

**Files:**
- Run only: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp`
  (exists; `ENABLE_BENCHMARKS=ON` in `build`, OFF in `build_asan` — so use `build`)

- [ ] **Step 1: Build the benchmark**

```bash
ninja -C build benchmark_cas_ref_protocol > build/build_bench_baseline.log 2>&1; echo NINJA_EXIT=$?
```
Binary: `build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol`

- [ ] **Step 2: Record the baseline**

```bash
build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  > tmp/unattended/bench_baseline.txt 2>&1; echo EXIT=$?
```

**Which numbers are the gate** (this matters — the two flush-install benchmarks measure opposite
shapes):
- **`BM_FlushInstallUniqueOwner` is the PRIMARY gate.** It models what production actually does —
  a uniquely-owned base, so `materializeCommitted` takes the fast in-place O(overlay) path. The
  whole point of this plan's design refinement is to KEEP that shape; a regression here means the
  candidate accidentally kept the base shared at fold time.
- `BM_FlushInstall` (shared fixture, `use_count()==2`, deliberately the worst case) is informational.
- `BM_ScratchCopy` is the isolation floor for the one extra COW copy the restructure adds.
- `BM_ApplyRefLogTxn` covers the apply itself, which merely MOVES to before the PUT.

- [ ] **Step 3: Record a memory baseline**

Note the RSS of the benchmark process at the largest N (`/usr/bin/time -v` on the run above), so
Task 6 can compare. The restructure holds one extra COW copy (base shared, overlay only) across the
PUT — the expectation is "no material change".

- [ ] **Step 4: Record the numbers in the worklog and commit it**

`tmp/` is scratch; paste the medians into
`docs/superpowers/worklogs/2026-07-24-unattended-publish-confirm.md` and
`git commit -F <msgfile> -- docs/superpowers/worklogs/2026-07-24-unattended-publish-confirm.md`.

---

## Task 3: A1 site 1 — no-throw install in `commitRefChunk`

The core fix. Today: PUT (`CasRefLedger.cpp:1676`) → `applyRefLogTxn(rt->state, chunk_txn)`
(`:1694`) which can throw on allocation, leaving the transaction durable but unrecorded. After:
the candidate is built BEFORE the PUT and installed by a noexcept swap.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h`
  (add `swap`), `.cpp` (define it)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`
  (add `PostDurableInstall` to `CarvePhaseForTest`, line 207)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
  (`commitRefChunk`, lines 1592-1802)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

**Interfaces:**
- Produces: `void RefTableState::swap(RefTableState & other) noexcept;`
- Produces: `CarvePhaseForTest::PostDurableInstall` — fired inside `commitRefChunk` immediately
  after `putIfAbsentControlled` returns `Committed` and BEFORE the install; tests inject a throw
  there to simulate the post-durable failure.

- [ ] **Step 1: Add the noexcept swap to `RefTableState`**

In `CasRefProtocol.h`, in the public section of `class RefTableState` (after the getters around
line 167):

```cpp
    /// Member-wise swap, guaranteed non-throwing: every member's own swap is noexcept
    /// (`shared_ptr::swap`, `std::map::swap`, `std::set::swap`, `std::optional::swap` over trivially
    /// swappable payloads, plus PODs). This is the ONLY sanctioned way to install a prepared
    /// candidate after its transaction is durable -- see `CasRefLedger::commitRefChunk`'s
    /// post-durable install region, which runs under `DENY_ALLOCATIONS_IN_SCOPE`. Move-assignment
    /// would ALSO be noexcept today, but a swap keeps the old state's destruction (and its
    /// deallocation) outside the caller's install region by construction.
    void swap(RefTableState & other) noexcept;
```

In `CasRefProtocol.cpp`, next to the other `RefTableState` members:

```cpp
void RefTableState::swap(RefTableState & other) noexcept
{
    using std::swap;
    swap(lifecycle, other.lifecycle);
    swap(remove_txn_id, other.remove_txn_id);
    swap(greatest_applied, other.greatest_applied);
    committed.swap(other.committed);
    precommits.swap(other.precommits);
    owned_manifests.swap(other.owned_manifests);
    swap(snapshot_body_bytes, other.snapshot_body_bytes);
    swap(removal_body_bytes, other.removal_body_bytes);
}
```

If `RefCowMap`/`RefCowManifestSet` lack a `swap` member, add one to each (member-wise swap of
`base` and `overlay`, both noexcept) in `CasRefCowMap.h` / `CasRefCowManifestSet.h`. **Verify the
member list against the header before writing this** — `RefTableState`'s private members are at
`CasRefProtocol.h:176-195`; if a member was added since, swap it too (a missed member is a silent
state-corruption bug).

- [ ] **Step 2: Add the test hook phase**

`CasRefLedger.h:207` — extend the enum:

```cpp
    enum class CarvePhaseForTest { PlanSeenRefs, PlanBatchGrow, PlanReserveOwned, PublishPop, ValidateFinalOps, ChunkReseed, PostDurableInstall };
```

Document it next to `ChunkReseed`'s comment (around `:202-206`): `PostDurableInstall` fires inside
`commitRefChunk` after the chunk's `PUT` returned `Committed` and BEFORE the candidate is installed
— the seam a test uses to prove that a throw there can no longer strand a durable transaction.

- [ ] **Step 3: Write the failing tests**

In `src/Disks/tests/gtest_cas_ref_install_safety.cpp` (reuse `openPool`, `publishEmptyPart` and the
`Caller` pattern from `src/Disks/tests/gtest_cas_ref_chunked_flush.cpp:69-95`):

```cpp
/// A throw at the post-durable install point must NOT be able to strand a durable transaction:
/// after the fix the install region is allocation-free and cannot throw, so the hook's throw is
/// the only way to simulate the OLD failure -- and the test asserts the state is coherent either
/// way: the cached `greatest_applied` must equal the durable transaction's id.
TEST(CasRefInstallSafety, PostDurableInstallIsAllocationFree)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"p/test/ns1"};

    bool hook_fired = false;
    store->setCarveHookForTest([&](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase == CasRefLedger::CarvePhaseForTest::PostDurableInstall)
            hook_fired = true;
    });

    publishEmptyPart(store, ns, "part_a");
    store->setCarveHookForTest(nullptr);

    EXPECT_TRUE(hook_fired) << "the post-durable install seam must be reached by an ordinary commit";
    /// The transaction is recorded: the tail counter advanced and the ref resolves.
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), 1u);
    EXPECT_TRUE(store->resolveRef(ns, "part_a", /*allow_stale=*/false).has_value());
}

/// The install region must contain no allocation. Under a debug build
/// `DENY_ALLOCATIONS_IN_SCOPE` turns any allocation there into a LOGICAL_ERROR (which aborts), so
/// this negative control proves the guard is ARMED and the region is actually entered -- otherwise
/// a future edit could add an allocating statement and nothing would notice.
TEST(CasRefInstallSafetyDeathTest, AllocationInsideTheInstallRegionIsCaught)
{
#if defined(MEMORY_TRACKER_DEBUG_CHECKS)
    EXPECT_DEATH(
        {
            auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
            auto store = openPool(backend);
            const RootNamespace ns{"p/test/ns1"};
            store->setInstallRegionProbeForTest([] { volatile auto * p = new char[64]; (void)p; });
            publishEmptyPart(store, ns, "part_a");
        },
        "");
#else
    GTEST_SKIP() << "DENY_ALLOCATIONS_IN_SCOPE is a no-op in this build";
#endif
}
```

`setInstallRegionProbeForTest` does not exist yet — add it in Step 5 alongside the implementation,
**following the established seam style**: the `...ForTest` members live on `CasRefLedger`
(`CasRefLedger.h:162-282`) and are reached from tests through the `Pool` (`store->`), exactly like
the existing `setCarveHookForTest` / `tailSinceSnapshotCountForTest` / `refLaneWedgedForTest`. Add
the `Pool` forwarder next to those. Do NOT introduce a second accessor style
(no `refLedgerForTest()`).

- [ ] **Step 4: Run the tests to verify they fail**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRefInstallSafety*'`
Expected: compile error (the new hook phase / accessors do not exist yet) — that is the red state.

- [ ] **Step 5: Implement the restructure in `commitRefChunk`**

In `CasRefLedger.cpp`, inside `commitRefChunk` (currently lines 1592-1802):

1. After `const RefTxnId id = allocateRefTxnId();` (`:1659`) and the `chunk_txn` construction
   (`:1660`), and BEFORE the `sealObject`/PUT, build the candidate:

```cpp
    /// Build the candidate BEFORE the PUT so the post-durable install cannot throw (spec §A1).
    /// A COW copy shares the base with the live state, so this is cheap; the apply creates only the
    /// overlay. A throw HERE is a clean PRE-durability failure -- the same class as an ordinary
    /// validation reject: no object exists yet, the cache is untouched, the id is a safe gap.
    /// Deliberately NOT materialized: a candidate that shares its base cannot fold in place, so
    /// materializing here would rebuild the whole base (O(n) per chunk). The install below restores
    /// unique base ownership, and the existing post-install fold stays O(overlay).
    RefTableState candidate;
    {
        std::lock_guard lock(rt->state_mutex);
        candidate = rt->state;
    }
    try
    {
        applyRefLogTxn(candidate, chunk_txn);
    }
    catch (...)
    {
        complete_error(chunk_survivors, std::current_exception());
        return false;
    }
```

2. Replace the `case CasWriteOutcome::Committed:` body's apply (`:1691-1755`) with the install:

```cpp
        case CasWriteOutcome::Committed:
        {
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::PostDurableInstall);
            {
                std::lock_guard lock(rt->state_mutex);
                /// The install: allocation-free by construction, so it cannot throw. Only this
                /// leader mutates `rt->state` (single leader per table; the wedge-resolution apply
                /// ran earlier in this same flush; publishers only read), so the candidate's base
                /// snapshot is still current -- asserted below with a non-allocating comparison.
                {
                    if (install_region_probe_for_test)
                        install_region_probe_for_test();
                    DENY_ALLOCATIONS_IN_SCOPE;
                    chassert(rt->state.getGreatestApplied() < chunk_txn.txn_id);
                    rt->state.swap(candidate);
                    rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                    rt->tail_bytes_since_snapshot.fetch_add(bytes.size(), std::memory_order_relaxed);
                }
                /// `candidate` now holds the OLD state; it is destroyed at the end of this function,
                /// outside the deny region. With the old state gone, `rt->state` uniquely owns its
                /// base again, so the fold below is the in-place O(overlay) one, exactly as before.
                /// Unchanged semantics: the fold is an OPTIMIZATION, not part of the commit -- a
                /// failure here leaves `rt->state` coherent with a non-empty overlay and is swallowed.
                try
                {
                    rt->state.materializeCommitted();
                }
                catch (...)
                {
                    tryLogCurrentException(getLogger("CasPool"), fmt::format(
                        "CAS ref-log append for namespace '{}': committed txn {}-{} was applied durably, but "
                        "the post-commit overlay fold failed and was retained coherently for the next flush",
                        ns.string(), id.writer_epoch, id.ref_sequence));
                }
            }
            ProfileEvents::increment(ProfileEvents::CasRefBatchFlushes);
            /// ... rest of the existing Committed arm unchanged (survivor completion, snapshot schedule) ...
```

   The old outer `catch (...)` around the apply (`:1730-1755`) — the one that rethrew
   `LOGICAL_ERROR` — is **deleted**: the install cannot throw, so there is nothing to catch.

3. Add the test seam member next to `carve_hook_for_test` (`CasRefLedger.h:482`):

```cpp
    /// Test-only probe fired INSIDE the post-durable install region (under `DENY_ALLOCATIONS_IN_SCOPE`).
    /// The negative control for the deny guard: a probe that allocates must abort a debug build,
    /// proving the region is armed and actually entered. Null in production.
    std::function<void()> install_region_probe_for_test;
```
with the setter `void setInstallRegionProbeForTest(std::function<void()> p) { install_region_probe_for_test = std::move(p); }`
next to `setCarveHookForTest` (`:208`), and add `#include <Common/MemoryTracker.h>` to
`CasRefLedger.cpp` if not already present.

4. Add the `Pool` forwarder for `setInstallRegionProbeForTest` beside the existing
   `setCarveHookForTest` forwarder. The other assertions reuse seams that already exist
   (`tailSinceSnapshotCountForTest`, `resolveRef`).

- [ ] **Step 6: Run the tests to verify they pass**

Run: `ninja -C build unit_tests_dbms > build/build_task3.log 2>&1; echo NINJA_EXIT=$?`
then `build/src/unit_tests_dbms --gtest_filter='CasRefInstallSafety*'`
Expected: PASS (the death test runs only in debug builds; under RelWithDebInfo it SKIPs).

- [ ] **Step 7: Run the full CAS gate**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*' > build/gate_task3.log 2>&1; echo EXIT=$?`
Expected: zero failures. Have a subagent summarize the log.

- [ ] **Step 8: Commit**

```bash
git commit -F <msgfile> -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
  src/Disks/tests/gtest_cas_ref_install_safety.cpp
```
Subject: `ca: ref lane — no-throw post-durable install in commitRefChunk (A1 site 1)`

---

## Task 4: A1 site 3 — preconstruct the wedge before the PUT

Today `rt->wedge = RefAppendWedge{id, key, bytes}` (`CasRefLedger.cpp:1788`) copies two `String`s
(`CasRefLedger.h:308-310`) inside the post-durable path. If that allocation fails, the object may be
durable while the runtime records NEITHER the transaction NOR the wedge — worse than a wedge,
because the next append allocates a fresh id and proceeds against a state missing a landed txn.

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` (`commitRefChunk`)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
/// An `Unresolved` PUT must always leave the lane wedged with the EXACT {id, key, bytes} of the
/// in-doubt object -- the wedge is the only record that can later resolve it. Constructing the
/// wedge after the PUT could fail on allocation and record nothing at all, so it is preconstructed.
TEST(CasRefInstallSafety, UnresolvedAlwaysRecordsTheWedge)
{
    auto backend = std::make_shared<ChunkFaultBackend>();   // reuse from gtest_cas_ref_chunked_flush.cpp
    backend->fault_substr = "_log/";
    backend->mode = ChunkFaultBackend::Mode::Unresolved;
    auto store = openPool(backend);
    const RootNamespace ns{"p/test/ns1"};

    EXPECT_ANY_THROW(publishEmptyPart(store, ns, "part_a"));

    /// Both seams already exist (`CasRefLedger.h:171,173`).
    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "an Unresolved PUT must always leave a wedge";
    EXPECT_FALSE(store->wedgedKeyForTest(ns).empty());
}
```

`ChunkFaultBackend` lives in `gtest_cas_ref_chunked_flush.cpp`'s anonymous namespace. Prefer
promoting it into `src/Disks/tests/cas_test_helpers.h` (where `CountingBackend` and
`MetaWriteFaultBackend` already live) over duplicating it; if promoting, move it verbatim and
update the chunked-flush file's use — no behavior change, and run that file's suite to prove it.

- [ ] **Step 2: Run to verify it fails or is vacuous**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRefInstallSafety.UnresolvedAlwaysRecordsTheWedge'`
Expected: compile error (missing accessor) or PASS-by-accident. Note which — if it passes, it is a
characterization test that must keep passing after Step 3.

- [ ] **Step 3: Implement preconstruction**

In `commitRefChunk`, before the PUT call (`:1673-1676`):

```cpp
    /// Preconstruct the wedge (spec §A1 site 3): both `String`s are copied HERE, before the PUT, so
    /// the `Unresolved` arm below only has to MOVE it into the runtime -- an allocation failure can
    /// no longer leave a possibly-durable object with neither a transaction nor a wedge recorded.
    /// The PUT reads the key/body straight out of the prepared wedge, so nothing is copied twice.
    RefAppendWedge prepared_wedge{id, key, bytes};

    CasWriteOutcome outcome{};
    try
    {
        outcome = ref_request_controller->putIfAbsentControlled(prepared_wedge.key, prepared_wedge.bytes, fence_ok);
    }
```

and in the `Unresolved` arm (`:1784-1789`):

```cpp
        case CasWriteOutcome::Unresolved:
        {
            {
                std::lock_guard lock(rt->state_mutex);
                DENY_ALLOCATIONS_IN_SCOPE;
                rt->wedge = std::move(prepared_wedge);
            }
```

`std::optional<RefAppendWedge>::operator=(RefAppendWedge&&)` moves two `String`s and a POD id — no
allocation. Keep `bytes` alive for the `sealObject`/PUT usage by reading it from `prepared_wedge`
everywhere below (replace the remaining `bytes.size()` uses with `prepared_wedge.bytes.size()`).

- [ ] **Step 4: Run the test + full gate**

Run the single test, then the full CAS filter as in Task 3 Step 7. Expected: all pass.

- [ ] **Step 5: Commit**

Subject: `ca: ref lane — preconstruct the wedge before the PUT (A1 site 3)`

---

## Task 5: A1 site 2 — wedge-resolution apply + swallow symmetry

The wedge-resolution arm (`CasRefLedger.cpp:1205+`) applies post-durably too, and unlike
`commitRefChunk` it has no inner swallow around `materializeCommitted` — a throw there leaves the
transaction applied but the wedge NOT reset, so the next resolution re-applies it and double-bumps
the tail counters.

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` (the wedge-resolution block inside `flushRefBatch`)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
/// Resolving a wedge is a post-durable install too: the GET proves the object landed, so the
/// transaction MUST be recorded. Build the candidate BEFORE the resolving GET (its bytes are known
/// from the wedge), then install it by noexcept swap. A fold failure afterwards must not leave the
/// wedge set -- otherwise the next resolution re-applies the same transaction.
TEST(CasRefInstallSafety, WedgeResolutionInstallsExactlyOnce)
{
    // 1. Drive an Unresolved PUT that actually LANDED (fault mode: lost response, object present).
    // 2. Drive a second append -> the wedge resolves.
    // 3. EXPECT: wedge cleared; greatest_applied == the wedged txn id; the tail counter advanced by
    //    exactly one for that transaction (not two).
}
```

Fill in with the `ChunkFaultBackend` "landed but lost response" mode (add such a mode if only
`Unresolved` exists: put the object, then report Unresolved).

- [ ] **Step 2: Run to verify it fails**

Expected: the double-count / wedge-not-cleared assertion fails, or the test compiles red.

- [ ] **Step 3: Implement**

In the wedge-resolution block: decode the wedged transaction and build the candidate BEFORE calling
`resolveByExactGet` (the bytes are already in the wedge, so no extra I/O), then on `Committed`
install with the same deny-region swap + counter bumps used in Task 3, clear the wedge inside that
region, and move `materializeCommitted()` outside it into a try/catch that swallows (mirroring
`commitRefChunk`). Do NOT retain the candidate in the wedge across attempts — recompute it per
resolution attempt (the wedge can live until remount; a retained full state copy would be a
long-lived memory cost for a rare path).

- [ ] **Step 4: Run test + full gate; Step 5: Commit**

Subject: `ca: ref lane — no-throw install + swallow symmetry in wedge resolution (A1 site 2)`

---

## Task 6: Measurement gate (part 2) — prove no regression

- [ ] **Step 1: Rebuild and re-run**

```bash
ninja -C build benchmark_cas_ref_protocol > build/build_bench_after.log 2>&1; echo NINJA_EXIT=$?
build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  > tmp/unattended/bench_after.txt 2>&1; echo EXIT=$?
```

- [ ] **Step 2: Compare.** Acceptance: **`BM_FlushInstallUniqueOwner` median within 5% of
  baseline** (the production shape — the primary gate), `BM_ScratchCopy` unchanged, and no other
  benchmark worse than 10%. RSS not materially higher.
- [ ] **Step 3: If `BM_FlushInstallUniqueOwner` regresses**, STOP and report with the numbers. The
  overwhelmingly likely cause is that the candidate is still sharing its base at fold time (i.e.
  something kept the old state alive past the swap, or the candidate got materialized before the
  PUT after all) — re-read Task 3's comment block rather than tuning around it.
- [ ] **Step 4: Record the comparison in the worklog and commit the worklog.**

---

## Task 7: A2 — apply-pending poison state machine

Defense in depth after Task 3, and the confirm's rule 4.

**Files:**
- Modify: `.../Pool/CasRefLedger.h` (enum + per-table field + accessor), `.cpp` (transitions)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

**Interfaces:**
- Produces: `enum class RefApplyState : uint8_t { Clean, ApplyPending, Poisoned };` and
  `RefApplyState applyStateForTest(const RootNamespace &) const;` plus an internal
  `std::atomic<RefApplyState> apply_state{RefApplyState::Clean};` on `RefTableRuntime`.

- [ ] **Step 1: Write the failing tests** covering every transition:
  `Clean → ApplyPending → Clean` on a successful commit; `→ Clean` on a conclusive PUT throw
  (`CasRefLedger.cpp:1678-1686`), on `DefiniteFailure` (`:1775-1783`), and on a wedge that resolves
  to absent; `→ Poisoned` when the install probe throws post-durability (only reachable via the
  test seam after Task 3); and `Poisoned` is NEVER cleared by a later successful flush.
- [ ] **Step 2: Run — red.**
- [ ] **Step 3: Implement.** Arm (`Clean → ApplyPending`) immediately before the PUT — a plain
  `store(std::memory_order_relaxed)`, allocation-free. Clear on the paths above. Set `Poisoned` in
  the (now unreachable in production) failure path. Export a ProfileEvent
  `CasRefApplyPoisoned` on the transition to `Poisoned`.
- [ ] **Step 4: Wire rule 4** — nothing to wire yet (the confirm arrives in Task 11); just expose
  the accessor.
- [ ] **Step 5: Run tests + full gate; Step 6: Commit.**

Subject: `ca: ref lane — apply-pending poison state machine (A2)`

---

## Task 8: A3 — `precommitAdd` mint-tightening

**Files:**
- Modify: `.../Pool/CasPartWriteTxn.cpp` (`precommitAdd`, line 920) and `.h` if a member set of
  staged ids is needed
- Modify: `src/Disks/tests/gtest_cas_promote_republish.cpp` (the legality pin at line 283 is
  replaced)

- [ ] **Step 1: Write the failing test**

```cpp
/// An unowned ManifestId may enter ownership ONLY from the transaction that freshly staged it.
/// Without this, a dropped identity can be re-owned later, which would make the relink confirm's
/// exact-ManifestRef equality an ABA (the same token could name a manifest whose blobs were already
/// reclaimed). No production path performs this transition.
TEST(CasPromoteRepublish, PrecommitAddRejectsAnIdThisTxnDidNotStage)
{
    // stage id in txn1; abandon; then in txn2 call precommitAdd(ns, ref, id_from_txn1)
    // EXPECT: throws (LOGICAL_ERROR), nothing appended.
}
```
and update the existing test at `gtest_cas_promote_republish.cpp:283` that currently pins the
opposite behavior — read it first and rewrite its expectation with a comment explaining the change.

- [ ] **Step 2: Run — red.**
- [ ] **Step 3: Implement.** In `PartWriteTxn`, record every `ManifestId` returned by
  `stageManifest` in a member set; in `precommitAdd`, after the existing namespace check
  (`:926-929`), require membership unless the id is already the committed binding for that ref (the
  idempotent re-drive short-circuit at `:952-954` must keep working — it is evaluated inside the
  closure against live state, so the membership check must not reject that path; verify by running
  the existing re-drive tests).
- [ ] **Step 4: Run tests + full gate; Step 5: Commit.**

Subject: `ca: precommitAdd accepts only ids this transaction staged (A3, ABA barrier)`

---

## Task 9: TLA+ model for the confirm protocol

**Files:**
- Create: `docs/superpowers/models/CaRelinkConfirmCore.tla`
- Create: `docs/superpowers/models/CaRelinkConfirmCore_main.cfg`,
  `..._sab_nogate1.cfg`, `..._sab_stalecache.cfg`, `..._witness_confirmno.cfg`
- Create: `docs/superpowers/models/run_relinkconfirm.sh` (copy `run_ackfloor.sh`, change the module)

- [ ] **Step 1: Read `docs/superpowers/models/README.md`** for the naming/gating conventions
  (`_sab_*` must FAIL, `_witness_*` violation means reachable).
- [ ] **Step 2: Model** two journals (sender, receiver) with a monotonic id per journal, a GC round
  with a fold cursor and three-phase graduation (condemn → delete_pending → delete) with sparing on
  positive in-degree, and the confirm as an atomic predicate over the sender's state.
- [ ] **Step 3: State the property**

```
THEOREM ConfirmedRelinkNeverDangles ==
  [](\A r \in Receivers :
       (Promoted[r] /\ ConfirmYesObservedAt[r] /\ ActivationDurableBefore[r])
         => BlobsOf(Manifest[r]) \subseteq LiveBlobs)
```
- [ ] **Step 4: Sabotage configs** — `_sab_nogate1` (confirm answers yes on ref-name match only →
  must produce a violation, proving gate 1's exactness is load-bearing); `_sab_stalecache` (confirm
  reads a cache that may lag a durable removal → violation, proving the lane-quiescence rules).
- [ ] **Step 5: Run** `cd docs/superpowers/models && ./run_relinkconfirm.sh CaRelinkConfirmCore_main`
  (positive: "Model checking completed", no violation) and each `_sab_*` (must violate).
- [ ] **Step 6: Record results** in `CaRelinkConfirmCore_RESULTS.md` and **commit**.

---

## Task 10: `confirmExactRef` — the ledger-side lane snapshot (gate 1)

**Files:**
- Modify: `.../Pool/CasRefLedger.h/.cpp`
- Test: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` (create)

**Interfaces:**
- Produces:
```cpp
    enum class ConfirmAnswer : uint8_t { Yes, No, Unknown };
    /// Gate 1 of the relink confirm (spec §confirm-primitive): does `ref_name` in `ns` still name
    /// EXACTLY `manifest_ref` in this writer's committed view, read under a lane snapshot that
    /// cannot observe a stale cache? Performs ZERO object-store I/O: a cold/evicted/recovering
    /// table answers `Unknown` rather than recovering from storage.
    ConfirmAnswer confirmExactRef(const RootNamespace & ns, const String & ref_name, const ManifestRef & manifest_ref) const;
```

- [ ] **Step 1: Write the failing tests** — one per rule, from the spec §testing "Gate 1
  determinism" list: repointed live part → `No`; dropped+recreated → `No`; cold/evicted/recovering
  → `Unknown` with zero backend requests (assert via `CountingBackend`'s counters);
  pending/in-flight/wedge/mid-tenure (`CarvePhaseForTest::ChunkReseed`) → `Unknown`; poison set →
  `Unknown`; quiescent exact match → `Yes`; and a race test where an append admitted concurrently
  with the snapshot is ordered strictly after it.
- [ ] **Step 2: Run — red.**
- [ ] **Step 3: Implement** exactly the six-rule snapshot from the spec: take `ref_queue_mutex`,
  find the runtime **without creating one** (`ref_tables.find`, not `getRefTableRuntime`, which
  would create an unrecovered entry), then take `state_mutex` while still holding the queue mutex;
  evaluate warm → quiescent (`wedge`, `pending`, `leader_active`) → poison → exact row equality →
  fence last (`fence_ok_fn()` and `!superseded_by_remount`); return `Unknown` for every ambiguity.
  **Do not call** `ensureRefTableRecovered`, `sweepStalePrecommitsForRead`, or
  `maybeScheduleSnapshotPublish` — all three are in `resolveRef` and all three can do I/O.
- [ ] **Step 4: Run tests + full gate; Step 5: Commit.**

Subject: `ca: confirmExactRef — exact-token lane-linearized confirm (gate 1)`

---

## Task 11: Storage-level confirm + gate 0 + exchange interface

**Files:**
- Modify: `.../ContentAddressedExchange.h` (two methods), `.../ContentAddressedMetadataStorage.h/.cpp`
- Modify: `src/Storages/MergeTree/DataPartsExchange.cpp` (`Service` side, gate 0 lookup)

**Interfaces:**
- Produces, on `IContentAddressedExchange`:
```cpp
    /// Does this instance own `root_namespace` under `{pool_uuid, server_root_id}`? Routing
    /// predicate for the confirm action: a pool UUID is shared across server roots, so it alone
    /// cannot select the answering mount.
    virtual bool ownsNamespace(const String & server_root_id, const String & root_namespace) const = 0;

    /// Gate 1 of the relink confirm, forwarded to the ledger. `Unknown` on any ambiguity.
    virtual CasConfirmAnswer confirmExactRef(const String & root_namespace, const String & ref_name,
                                             const String & manifest_ref_text) const = 0;
```
  (`CasConfirmAnswer` = a small enum declared in `ContentAddressedExchange.h` so
  `DataPartsExchange.cpp` needs no CAS-internal headers; the `manifest_ref_text` is the canonical
  `epoch:build:ordinal` rendering already used in events — reuse the existing formatter, find it
  with `grep -n "manifestRefDebugString\|renderManifestRef" -r src/.../ContentAddressed/`.)

- [ ] **Step 1: Write the failing tests** for gate 0's filter semantics (spec §testing "Gate 0"):
  `Deleting` → `No`; absent/other-disk → `No`; the `Deleting → Outdated` rollback state must still
  be rejected by gate 1 when the ref is gone; `MOVE ... TO DISK` same-name-other-disk → `No` unless
  the matched instance is the part's current disk. These are integration-level — put them in the
  Task 16 pytest file and keep this task's unit tests to `ownsNamespace` routing.
- [ ] **Step 2: Implement `ownsNamespace` + `confirmExactRef`** on
  `ContentAddressedMetadataStorage` as thin forwards (`store()`/the ledger), with the
  storage-lifecycle gates applied (a not-started / terminal disk answers `Unknown`, never throws
  out of a confirm).
- [ ] **Step 3: Implement the `Service`-side resolution**: enumerate `data.getDisks()`, map each
  through the existing `tryGetContentAddressedExchange` (`DataPartsExchange.cpp:117-122`), keep the
  ones where `ownsNamespace(...)` is true; **zero or more than one match → `Unknown`**; then gate 0:
  look up the part by name in the parts set and require `Active`/`Outdated` **on the matched
  instance's disk** (the `MOVE ... TO DISK` same-name case). Read the parts set under its own lock
  and release it before calling into the ledger.
- [ ] **Step 4: Run gate; Step 5: Commit.**

Subject: `ca: exchange-level confirm (ownsNamespace + confirmExactRef) with the gate-0 part filter`

---

## Task 12: `PreparedRelink` handle + typed prepare boundary

**Files:**
- Modify: `.../Parts/PartFolderAccess.h/.cpp` (split `publishEntries`, currently `.cpp:338-372`)
- Modify: `.../ContentAddressedExchange.h`, `.../ContentAddressedMetadataStorage.h/.cpp`

**Interfaces:**
- Produces, on `CachedPartFolderAccess`:
```cpp
    /// Stage + precommit only: the receiver's `+1` becomes durable, the promote is deferred until
    /// the caller has proven the source still holds the manifest (spec §relink-handle). The returned
    /// handle OWNS the open transaction and must be either `promote`d or `abort`ed -- destruction
    /// alone is not cleanup (`~PartWriteTxn` only retires the build sequence).
    PreparedPartWrite prepareEntries(const PartRefKey & dst, const std::vector<Cas::ManifestEntry> & entries,
                                     Cas::ProvenanceOp op);
```
  with `PreparedPartWrite` holding `Cas::PartWriteTxnPtr build; PartRefKey key; Cas::ManifestId id;`
  and exposing `Cas::CommitOutcome promote(bool allow_repoint = false)` and `void abort()`, move-only,
  with an explicit terminal-state flag so a scope guard can abort exactly once.

- [ ] **Step 1: Write the failing tests** — prepare-then-promote equals today's `publishEntries`
  outcome; prepare-then-abort leaves no committed ref AND appends the precommit removal (assert via
  the ledger's precommit view — no same-epoch leak); double-terminal is rejected.
- [ ] **Step 2: Run — red.**
- [ ] **Step 3: Implement** by splitting `publishEntries` at `PartFolderAccess.cpp:353`: everything
  up to and including `precommitAdd` moves into `prepareEntries`; `promoteBuild` becomes the
  handle's `promote`; the existing `catch` → `abandon` → rethrow discipline (`.cpp:361-371`) moves
  into the handle's `abort` and into `prepareEntries`'s own failure path. **Keep `publishEntries`**
  implemented in terms of the new pair, so every existing caller is untouched.
- [ ] **Step 4: Run gate; Step 5: Commit.**

Subject: `ca: split publishEntries into prepare/promote/abort (PreparedRelink handle)`

---

## Task 13: Wire protocol — the confirm action and the source token

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.h/.cpp`

- [ ] **Step 1: Add the protocol version and token constants** next to the existing ones
  (`.cpp:76-111`):
```cpp
constexpr auto REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM = 11;
constexpr auto CA_CONFIRM_ACTION_PARAM = "content_addressed_confirm";     /// request: "1" selects the confirm action
constexpr auto CA_CONFIRM_TOKEN_COOKIE = "content_addressed_source_token"; /// response cookie on the relink offer
```
  and bump the version the server advertises (`.cpp:185`) and the client sends (`.cpp:523-529`).
- [ ] **Step 2: Mint the token on the sender** inside the existing relink branch
  (`.cpp:249-280`), as a compact text encoding of
  `{pool_uuid, server_root_id, root_namespace, ref_name, part_name, manifest_ref}` — the sender
  already has all of these (the manifest bytes carry `ref`/`root_namespace_id`, see
  `CasPartManifestFormat.h:76-78`). Add it as a response cookie next to `CA_RELINK_COOKIE`
  (`.cpp:268`).
- [ ] **Step 3: Handle the confirm action** at the TOP of `Service::processQuery`
  (before the body reset and before `part` is required/parsed, `.cpp:170-205`): parse the token,
  resolve the instance via Task 11's routing, answer `yes`/`no`/`unknown` as a response cookie plus
  an empty body, and return.
- [ ] **Step 4: Test** with an integration test in Task 16 (a unit test cannot exercise the HTTP
  endpoint); here, add a round-trip test for the token encoder/decoder (a pure function — put it in
  `gtest_cas_confirm_exact_ref.cpp`), including rejection of malformed and over-long fields.
- [ ] **Step 5: Run gate; Step 6: Commit.**

Subject: `ca: interserver confirm action + source token (relink protocol v11)`

---

## Task 14: Receiver flow — prepare, confirm, promote, and the typed failure taxonomy

**Files:**
- Modify: `.../ContentAddressedMetadataStorage.h/.cpp` (`adoptPartFromManifest` → typed prepare),
  `.../ContentAddressedExchange.h`, `src/Storages/MergeTree/DataPartsExchange.cpp`
  (`Fetcher::relinkPartToDisk` `.cpp:1107-1169` and the receiver branch `.cpp:728-771`)

- [ ] **Step 1: Replace the boolean adoption** with a typed prepare on the exchange interface:
```cpp
    enum class CaRelinkPrepare : uint8_t { Prepared, MechanismFallbackAllowed };
    /// Stage + precommit the receiver-local manifest and RETURN a handle; the caller must confirm
    /// against the source and then promote or abort. `MechanismFallbackAllowed` means relink cannot
    /// work here but the sender still has the part -- the caller may byte-refetch from the SAME
    /// sender. A confirm failure is NOT this class: it must abort and throw (spec §failure-taxonomy).
    virtual CaRelinkPrepare prepareAdoptFromManifest(const String & table_uuid, const String & part_name,
                                                     const String & manifest_bytes,
                                                     std::unique_ptr<ICaPreparedRelink> & out) = 0;
```
  (`ICaPreparedRelink` = a tiny abstract handle with `promote()`/`abort()` so
  `DataPartsExchange.cpp` stays free of CAS-internal types.)
- [ ] **Step 2: Rewrite the receiver branch** (`.cpp:728-771`): parse the relink cookie AND the
  token cookie → `prepareAdoptFromManifest` → on `MechanismFallbackAllowed` keep today's
  `fall_back_to_byte_fetch()` → otherwise issue the confirm request → on `yes` `promote()` and
  build the part exactly as `relinkPartToDisk` does today → on anything else `abort()` and
  **throw** a locally generated retry-later `NETWORK_ERROR` with a message naming the source and
  part.
- [ ] **Step 3: Scope guard** — the handle must be aborted on every non-promote exit including
  exceptions; use `SCOPE_EXIT` around the confirm+promote region.
- [ ] **Step 4: Verify the throw is not swallowed** — grep the call chain
  (`fetchSelectedPart` → `fetchPart` → `executeFetch`) for `catch` blocks that would convert it
  back into a byte re-request; the typed boundary exists precisely because
  `adoptPartFromManifest`'s old catch-all (`ContentAddressedMetadataStorage.cpp:2002`) did that.
- [ ] **Step 5: Run gate; Step 6: Commit.**

Subject: `ca: relink receiver — prepare/confirm/promote with a typed failure taxonomy`

---

## Task 15: B66b — `allow_ca_relink` capability, recursion brake, detached target

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.h/.cpp`,
  `src/Storages/StorageReplicatedMergeTree.cpp` (the two manual detached callers, `:8125`, `:8281`)

- [ ] **Step 1: Add the capability parameter.** Replace the `try_zero_copy && !to_detached` gate
  (`.cpp:545`) with a dedicated `allow_ca_relink` parameter on `fetchSelectedPart`
  (default `true`), leaving `try_zero_copy` untouched for real zero-copy (`.cpp:566`).
- [ ] **Step 2: Wire the recursion brake.** Every `fall_back_to_byte_fetch()` re-request
  (`.cpp:733-739`) must pass `allow_ca_relink=false` — this replaces the brake that
  `try_zero_copy=false` provided implicitly. Without it a persistent mechanism failure loops.
- [ ] **Step 3: Pass `to_detached` into `relinkPartToDisk`** and construct the temporary storage
  under the detached parent (today the active parent is hardcoded, `.cpp:1128`); the CA router
  already folds any `detached/<name>` ref (`ContentAddressedMetadataStorage.cpp:1241`).
- [ ] **Step 4: Update the two manual detached callers** to pass `allow_ca_relink=true`
  independently of `try_fetch_shared=false`.
- [ ] **Step 5: Do NOT change detached finalization** — it stays `renameTo(detached/<part>, true)`
  (`StorageReplicatedMergeTree.cpp:5719`) with its existing collision behavior.
- [ ] **Step 6: Run gate; Step 7: Commit.**

Subject: `ca: B66b — relink into detached behind allow_ca_relink, with a recursion brake`

---

## Task 16: Integration test battery

**Files:**
- Modify/create under `tests/integration/test_cas_replicated_relink/`

- [ ] **Step 1: Race test** — failpoint between `precommitAdd` and the confirm; sender drops the
  part in the window → confirm `no` → abort → retryable failure → assert the queue re-selects
  (covering-part / other replica) and that **no** byte re-request went to the original sender.
- [ ] **Step 2: Happy path** — relink proof: `CasBlobPut == 0` on the receiver.
- [ ] **Step 3: codex-6 regression** — stall the receiver's publish across ≥ 3 GC rounds with a
  small `old_parts_lifetime`, merge the sender's part away, GC to fixpoint → the stalled attempt
  must NOT produce a committed ref; fsck clean, `dangling=0`.
- [ ] **Step 4: Recursion brake** — force a persistent mechanism failure → exactly one relink
  attempt then bytes, no loop.
- [ ] **Step 5: B66b** — `FETCH PART` into `detached/` on both manual callers → relink proof +
  `ATTACH` reads correctly; cross-pool → bytes.
- [ ] **Step 6: RPL-5 slice** — `REPLACE PARTITION` / `ATTACH PARTITION ... FROM` on the 2-replica
  fixture: assert the queue-cloned `REPLACE_RANGE` fetch relinks (blob-count proof).
- [ ] **Step 7: Version mix** — confirm-capable receiver × legacy sender cookie → clean byte
  fallback.
- [ ] **Step 8: Run and commit**

```bash
ninja -C build clickhouse > build/build_srv_t16.log 2>&1; echo NINJA_EXIT=$?
python -m ci.praktika run "integration" --test test_cas_replicated_relink > build/test_t16_relink.log 2>&1; echo EXIT=$?
```
The fixture is 2-replica with `with_rustfs=True` (RustFS, not MinIO — the CAS probe needs real
conditional-PUT semantics) over the shared-pool disk in
`tests/integration/test_cas_replicated_relink/configs/storage_conf.xml`; the existing relink proof
is `count_blobs()` staying flat across a fetch (`test.py:47-51`). Reuse that helper for every new
relink proof rather than inventing a second one.

---

## Task 17: ca-soak scenario S42 — allocation-fault soak

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s42_alloc_faults.py`
- Modify: `utils/ca-soak/scenarios/README.md` (register S42 in the card list)

Design: `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` §testing,
plus `utils/ca-soak/scenarios/BACKLOG.md` §s42-allocation-fault-soak (read BOTH before writing).

- [ ] **Step 1: Copy the card skeleton** from `cards/s39_lease_fault_tolerance.py` (`@register`,
  `name`/`title`/`priority`/`param_table`/`run`), keeping its soundness-guard style.
- [ ] **Step 2: Leg A** — arm `memory_tracker_fault_probability` per query through the driver's URL
  parameters (`utils/ca-soak/soak/cluster.py:247`) over a soak-shaped insert/select workload.
- [ ] **Step 3: (MOVED to Task 20 / scenario S43.)** Thread-allocation faults were originally leg B
  here. They are a different fault CLASS with a different blast radius — they cannot reach the CAS
  commit path at all, because the ref append lane runs on the caller's thread — and mixing them into
  one card destroys attribution when something breaks. S42 is therefore query-thread allocation
  faults only.
- [ ] **Step 4: Leg C** — disarm, quiesce, GC to fixpoint, fsck, restart, and compare; ALSO replay
  from the last pre-fault snapshot plus the raw tail logs and compare against the live cache, and
  assert no snapshot advanced across a poisoned transaction (`CasRefApplyPoisoned` from Task 7).
- [ ] **Step 5: Soundness guard** — require a nonzero targeted signal (the poison-transition
  counter, or a post-PUT failpoint hit), NOT merely a nonzero `MEMORY_LIMIT_EXCEEDED` count; report
  `inconclusive` otherwise.
- [ ] **Step 6: Oracle** — queries may fail; invariants may not: zero `LOGICAL_ERROR`/abort in
  `err.log` (only expected injected errors during the armed window), acked-vs-lost = 0, replicas
  agree, fsck `dangling=0`/`unaccounted=0`, GC recovers after disarm, no permanently wedged lane,
  no query hung past a bound.
- [ ] **Step 7: Run at dev scale**
  `cd utils/ca-soak && python3 -m scenarios.run --scenario S42 --seed 1 --scale dev`
  and iterate until it is GREEN or produces a genuine, triaged finding.
- [ ] **Step 8: Commit** the card plus its `RUN_HISTORY.md` entry.

---

## Task 18: do not wedge a ref lane when no attempt was ever sent (finding #37 defect 3, behavioural half)

The diagnostic half landed separately: `putIfAbsentControlled` now reports WHY it returned
`Unresolved` via `CasUnresolvedReason`, and `CasUnresolvedReason::NoAttemptSent` marks the case where
both pre-attempt gates (mount fence, operation deadline) rejected on the FIRST iteration — the key is
provably unwritten. This task acts on that fact; it is deliberately separate because it changes
protocol behaviour, not a message.

**The problem.** `commitRefChunk`'s `Unresolved` arm wedges the table's append lane unconditionally.
A wedge is the right response to genuine ambiguity — an object that may or may not be durable, which
only `resolveByExactGet` can settle. But on `NoAttemptSent` nothing was ever sent, so there is nothing
to resolve, and the wedge is pure cost: it blocks EVERY ref append for that table on this replica
(inserts included) until the key resolves or the mount is remounted, and a GET-based resolution of a
key that was never written can never resolve — it stays `Unresolved` forever. So a transient fence
blip during the pre-attempt gate currently costs a table its write availability until remount.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
  (`commitRefChunk`'s `Unresolved` arm, and the same decision in the wedge-resolution path)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

**Interfaces:** consumes `CasUnresolvedReason` from
`Backend/CasRequestControl.h`; produces no new public surface.

- [ ] **Step 1: Write the failing test.** Drive a `NoAttemptSent` rejection (a fence that is already
  false when `commitRefChunk` is entered — `ChunkFaultBackend` is not even needed, the fence hook is
  enough) and assert: the caller gets a retry-later error, `refLaneWedgedForTest(ns)` is FALSE, and a
  subsequent append on the same table succeeds without a remount. Then the contrast case: a genuinely
  ambiguous PUT (`Mode::Unresolved`) must STILL wedge — that assertion already exists as
  `UnresolvedAlwaysRecordsTheWedge`, so extend rather than duplicate it.

- [ ] **Step 2: Run — the no-wedge assertion fails** (today the lane wedges in both cases).

- [ ] **Step 3: Implement.** In the `Unresolved` arm, skip the wedge install when the reason is
  `NoAttemptSent`: complete the survivors with the same retry-later error (unchanged), leave
  `rt->wedge` disengaged, and leave the allocated txn id as a safe gap (it already is one — ids are
  not required to be contiguous, `CasRefProtocol.cpp` only enforces strict increase). Keep the
  prepared wedge construction where it is: it is allocation-free to discard, and building it before
  the PUT is what makes the ambiguous path safe.

- [ ] **Step 4: Prove the safety argument in the comment, not just in the commit message.** The claim
  is "no attempt was sent, therefore the key is unwritten, therefore there is nothing a wedge could
  resolve". It rests on both pre-attempt gates returning before `backend->putIfAbsent` — state that,
  and state the counterexample it excludes (a fence lost AFTER an attempt is `FenceLostMidWay`, which
  keeps wedging).

- [ ] **Step 5: TLA.** The wedge is part of the append-lane's at-most-one-unresolved-PUT contract, so
  extend the ref-lane model with a `NoAttemptSent` transition and re-check that
  "every durable object is either applied or wedged" still holds. If the existing model has no
  pre-attempt gate, adding one is the work.

- [ ] **Step 6: Run the full CAS gate; Step 7: Commit.**

Subject: `ca: a pre-attempt fence reject no longer wedges the lane (finding #37 defect 3)`

---

## Task 19: a diagnostic tool must not claim ownership of a live pool (CI "Scraping system tables")

Known since the 2026-07-23 PR-2073 triage and seen again in the 2026-07-24 run, so it is not a flake.
The CI step that scrapes system tables runs `clickhouse-local` against the same data directory as the
LIVE server; on a CA disk that goes `Pool::mountWritable` -> `claimOwnerOrThrow` and fails, because
the running server legitimately owns that server root. The scrape step then reports a failure that has
nothing to do with the change under test, and — worse — it is indistinguishable at a glance from a
real mount-ownership bug, which is exactly what a diagnostic path must never look like.

**Why this is a product task, not a CI-config tweak.** The same shape bites any read-only consumer of
a live pool: `clickhouse-disks`/`ca-fsck` against a running server, a post-mortem scrape, an operator
inspecting a pool from a second process. The stand already carries a bespoke workaround for exactly
this (`utils/ca-soak/configs/fsck_only_ca.xml`, a separate fsck-only config), and BACKLOG's
`[F1-prod]` entry records the same class from the other direction (a read-only shadow disk breaking
part discovery). One decision should serve all of them.

**Design question to settle FIRST (do not skip to code):** which of these is the contract?
  (a) a CA disk opened by a tool declares itself read-only and NEVER claims the mount — the pool is
      readable without ownership, and every write-class op fails closed;
  (b) tools keep claiming, but a claim by a NON-server process against a live root is a clean,
      typed refusal the caller can recognise and downgrade to read-only itself;
  (c) the scrape simply must not see CA disks (a CI-config carve-out) — cheapest, but leaves the
      general problem and the operator case unsolved.
`Store::open`'s existing `read_only` path is the natural home for (a) — note the BACKLOG item
"[refactor: Store::open modes] split into create / open-rw / open-ro", which flags that read-only
`Store::open` can still write `_pool_meta`; that bug must be fixed as part of (a) or it undermines it.

**Files (once the contract is chosen):**
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.{h,cpp}` (open modes)
- `.../Pool/CasServerRoot.cpp` (`claimOwnerOrThrow` refusal typing, for (b))
- the CI scrape invocation, wherever it builds its `clickhouse-local` config

**Prior attempt that did NOT fix it (know this before starting):** `ee15c8ade23` ("open CA disks
read-only for the post-run scrape") is already in the SHA that still failed on 2026-07-24, with
`clickhouse-local` claiming `stateless-ca-s3` under `server_uuid=0000…`. So "make the scrape
read-only" has been tried at the call site and was not sufficient — find out why before repeating it;
that is evidence for contract (a) or (b) over (c).

- [ ] **Step 1: Reproduce locally** — start the soak stand, then run the same `clickhouse-local`
  scrape against its data dir and capture the exact failure. Without a local repro this task is
  guesswork.
- [ ] **Step 2: Pick the contract** (a)/(b)/(c) and write it into
  `docs/superpowers/cas/` where the disk-lifecycle rules live, with the operator case named.
- [ ] **Step 3: Implement, with a test that a read-only open of a LIVE pool succeeds and takes no
  ownership** — assert the live server's mount object is untouched (same holder, same epoch) after
  the tool exits.
- [ ] **Step 4: Verify the CI scrape step passes against a running CA server; Step 5: Commit.**

Subject: `ca: read-only pool access for tools — stop the system-table scrape claiming a live mount`

---

## Task 20: ca-soak scenario S43 — thread-allocation fault injection

Split out of S42 (Task 17): `cannot_allocate_thread_fault_injection_probability` is a different fault
class from `memory_tracker_fault_probability`, and running both in one card would make any finding
unattributable. Query-thread allocation faults hit the CAS commit path; THREAD-creation faults cannot
— the ref append lane runs on the caller's thread (`CasRefLedger.cpp`, verified in the round-4 review)
— so what S43 actually stresses is everything CAS runs in the BACKGROUND, which no other card covers.

**Mechanism.** `cannot_allocate_thread_fault_injection_probability` is a `ServerSetting`
(`src/Core/ServerSettings.cpp:233`) applied on config reload in `programs/server/Server.cpp:2763-2764`
(NOT via `InterpreterSystemQuery` — that site is `SYSTEM START THREAD FUZZER`), which installs it into
`CannotAllocateThreadFaultInjector`. Arm and disarm with a reversible generated config overlay plus
`SYSTEM RELOAD CONFIG`, and verify both transitions by reading the value back from
`system.server_settings` — an unverified arm is how a scenario silently becomes vacuous.

**What is actually at risk (this is the interesting part, and the oracle follows from it):**
- the **GC scheduler**'s worker and heartbeat threads — a round that cannot start must be retried, not
  lost, and the scheduler must not spin;
- the **mount-lease keeper**'s renewal — if renewal cannot run, the lease lapses and the mount fences.
  That is CORRECT fail-closed behaviour, so the card must not treat a fence as a failure; what it must
  prove is that the fence is followed by **recovery** once injection stops (self-remount rearms and
  writes resume) rather than a permanent write outage;
- the **self-remount thread** itself — the recovery path failing to spawn is the nastiest case,
  because it is the thing that repairs the previous bullet;
- the background **snapshot-publish dispatcher** (`CasRefLedger.cpp:1839`) — a dropped publish must
  cost a redundant snapshot later, never a lost transaction.

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s43_thread_alloc_faults.py`
- Modify: `utils/ca-soak/scenarios/README.md` (register S43), `utils/ca-soak/scenarios/BACKLOG.md`
  (cross-reference from the S42 entry)

- [ ] **Step 1: Copy the card skeleton** from `cards/s39_lease_fault_tolerance.py` — it is the closest
  analogue, since it already reasons about lease TTL versus a fault window.
- [ ] **Step 2: Leg A — armed window.** Arm the injector at a probability low enough that the server
  still functions (start at 0.001 and calibrate on a dev run; too high and nothing starts, which
  proves nothing), run a soak-shaped insert/merge workload for the window, and record which background
  subsystems reported failures.
- [ ] **Step 3: Leg B — recovery.** Disarm, then assert RECOVERY explicitly: GC rounds succeed again,
  the mount is live (self-remount rearmed it if it fenced), inserts succeed, and
  `system.content_addressed_mounts` shows a healthy lease. A scenario that only checks "nothing
  corrupted" would pass while leaving the node permanently unable to write.
- [ ] **Step 4: Oracle.** Queries and background tasks MAY fail during the armed window; invariants may
  not: zero `LOGICAL_ERROR`/abort in `err.log`, every ACKED insert present, replicas agree, fsck
  `dangling=0`/`unaccounted=0`, `CasRefApplyPoisoned == 0`, no permanently wedged ref lane, and the
  recovery assertions of Step 3.
- [ ] **Step 5: Soundness guard.** Require positive evidence that threads actually failed to spawn
  (the injector's own counter, or the log lines the failure path emits) — a nonzero count of *some*
  error is not enough. Otherwise report `inconclusive`, never a vacuous pass.
- [ ] **Step 6: Run at dev scale** (`python3 -m scenarios.run --scenario S43 --seed 1 --scale dev`),
  iterate to GREEN or to a triaged finding, then **commit** the card with its `RUN_HISTORY.md` entry.

Subject: `ca: ca-soak scenario S43 — thread-allocation fault injection with an explicit recovery oracle`

---

## Self-Review

**Spec coverage:** §A1 → Tasks 3-6; §A2 → Task 7; §A3 → Task 8; §confirm-primitive gate 1 → Task
10; gate 0 → Task 11; §relink-handle → Task 12; §wire-protocol → Tasks 11+13; §failure-taxonomy →
Task 14; §b66b → Task 15; §testing integration list → Task 16; S42 → Task 17; TLA → Task 9;
`[UNMATCHED-MINUS-ONE]` → Task 1. The execution order matches spec §execution-order with the
benchmark gate split across Tasks 2 and 6.

**Known deliberate deviation from the spec (recorded in the spec's §A1 refinement):** the candidate
is NOT materialized before the PUT.

**Type consistency:** `ConfirmAnswer` (ledger, Task 10) is forwarded as `CasConfirmAnswer` (exchange
header, Task 11) — deliberately two types so `DataPartsExchange.cpp` needs no CAS-internal headers;
Task 11's implementation does the one mapping. `PreparedPartWrite` (Task 12, CAS-internal) is
exposed through `ICaPreparedRelink` (Task 14, abstract) for the same reason.

**Residual risk to watch during execution:** Part A edits `commitRefChunk`, which a parallel session
rewrote hours ago (stage1). Rebase-free discipline means implementing from the landed head and
re-reading the function before each edit.
