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
  ~~`build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'`~~
  **CORRECTED 2026-07-25** — that filter has a third known gap (parameterized `*/CasBackendContract`
  suites match neither `Cas*` nor `CA*`; see BACKLOG {#gate-filter-gap-3-backend-contract}). The
  operative filter, and the one the sibling follow-ups plan uses, is:

  ```
  build/src/unit_tests_dbms --gtest_filter='Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*'
  ```

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

**STATUS: DONE** — `d37609c0740`. The test passed on the first run, as Step 3 required, so the design
history's premise holds.

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

- [x] **Step 1: Read the two existing tests this one is modelled on**

`TEST(CasBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)` (`gtest_cas_blob_indegree.cpp:56-74`)
already folds an unmatched removal; `TEST(CasThreeCursorMerge, SnapshotEdgesUnperturbedByRetired)`
(`:452-484`) shows the explicit "surviving edge rows are byte-identical" assertion via `decodeRun`.
The new test combines the two: an unmatched removal for blob H must not disturb H's OTHER edge.

- [x] **Step 2: Write the test**

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

- [x] **Step 3: Run it — it must PASS immediately**

Run: `build/src/unit_tests_dbms --gtest_filter='CasBlobInDegree.UnmatchedRemovalIsAPerKeyNoOpAndSparesSiblingEdges'`
Expected: PASS. This is a characterization test of existing correct behavior, not red-then-green.
If it FAILS, STOP the whole plan and report — the design history's premise would be wrong.

- [x] **Step 4: Commit**

```bash
git commit -F <msgfile> -- src/Disks/tests/gtest_cas_blob_indegree.cpp
```
Message subject: `ca: pin the source-edge set model — an unmatched removal is a per-key no-op`

---

## Task 2: Benchmark baseline for the install restructure (measurement gate, part 1)

**STATUS: DONE** — baseline captured; the comparison and its verdict are recorded in the worklog with
`069f966c24f` ("Part A benchmark gate passed, no regression in the production fold path").

The current COW shape was justified by numbers; capture them BEFORE touching the lane.

**Files:**
- Run only: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp`
  (exists; `ENABLE_BENCHMARKS=ON` in `build`, OFF in `build_asan` — so use `build`)

- [x] **Step 1: Build the benchmark**

```bash
ninja -C build benchmark_cas_ref_protocol > build/build_bench_baseline.log 2>&1; echo NINJA_EXIT=$?
```
Binary: `build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol`

- [x] **Step 2: Record the baseline**

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

- [x] **Step 3: Record a memory baseline**

Note the RSS of the benchmark process at the largest N (`/usr/bin/time -v` on the run above), so
Task 6 can compare. The restructure holds one extra COW copy (base shared, overlay only) across the
PUT — the expectation is "no material change".

- [x] **Step 4: Record the numbers in the worklog and commit it**

`tmp/` is scratch; paste the medians into
`docs/superpowers/worklogs/2026-07-24-unattended-publish-confirm.md` and
`git commit -F <msgfile> -- docs/superpowers/worklogs/2026-07-24-unattended-publish-confirm.md`.

---

## Task 3: A1 site 1 — no-throw install in `commitRefChunk`

**STATUS: DONE** — `346046dae71`. Its gate run produced two findings that landed separately:
`028c3c865e7` (a load-sensitive hang in `RefWriterLaneExceptionSafety`, plus sizing the `phase_hits`
array off the enum) and `1498cf78304` (the two findings recorded in the worklog and BACKLOG).

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

- [x] **Step 1: Add the noexcept swap to `RefTableState`**

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

- [x] **Step 2: Add the test hook phase**

`CasRefLedger.h:207` — extend the enum:

```cpp
    enum class CarvePhaseForTest { PlanSeenRefs, PlanBatchGrow, PlanReserveOwned, PublishPop, ValidateFinalOps, ChunkReseed, PostDurableInstall };
```

Document it next to `ChunkReseed`'s comment (around `:202-206`): `PostDurableInstall` fires inside
`commitRefChunk` after the chunk's `PUT` returned `Committed` and BEFORE the candidate is installed
— the seam a test uses to prove that a throw there can no longer strand a durable transaction.

- [x] **Step 3: Write the failing tests**

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

- [x] **Step 4: Run the tests to verify they fail**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRefInstallSafety*'`
Expected: compile error (the new hook phase / accessors do not exist yet) — that is the red state.

- [x] **Step 5: Implement the restructure in `commitRefChunk`**

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

- [x] **Step 6: Run the tests to verify they pass**

Run: `ninja -C build unit_tests_dbms > build/build_task3.log 2>&1; echo NINJA_EXIT=$?`
then `build/src/unit_tests_dbms --gtest_filter='CasRefInstallSafety*'`
Expected: PASS (the death test runs only in debug builds; under RelWithDebInfo it SKIPs).

- [x] **Step 7: Run the full CAS gate**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*' > build/gate_task3.log 2>&1; echo EXIT=$?`
Expected: zero failures. Have a subagent summarize the log.

- [x] **Step 8: Commit**

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

**STATUS: DONE** — landed together with Task 5 in ONE commit, `10958ec8a28` ("no-throw install at the
two wedge sites (A1 sites 2 and 3)"), not as the two separate commits this plan prescribes. The two
sites share the same install shape, so splitting the commit would have split one argument in half.

Today `rt->wedge = RefAppendWedge{id, key, bytes}` (`CasRefLedger.cpp:1788`) copies two `String`s
(`CasRefLedger.h:308-310`) inside the post-durable path. If that allocation fails, the object may be
durable while the runtime records NEITHER the transaction NOR the wedge — worse than a wedge,
because the next append allocates a fresh id and proceeds against a state missing a landed txn.

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` (`commitRefChunk`)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

- [x] **Step 1: Write the failing test**

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

- [x] **Step 2: Run to verify it fails or is vacuous**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRefInstallSafety.UnresolvedAlwaysRecordsTheWedge'`
Expected: compile error (missing accessor) or PASS-by-accident. Note which — if it passes, it is a
characterization test that must keep passing after Step 3.

- [x] **Step 3: Implement preconstruction**

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

- [x] **Step 4: Run the test + full gate**

Run the single test, then the full CAS filter as in Task 3 Step 7. Expected: all pass.

- [x] **Step 5: Commit**

Subject: `ca: ref lane — preconstruct the wedge before the PUT (A1 site 3)`

---

## Task 5: A1 site 2 — wedge-resolution apply + swallow symmetry

**STATUS: DONE** — in `10958ec8a28`, jointly with Task 4 (see Task 4's status note).

The wedge-resolution arm (`CasRefLedger.cpp:1205+`) applies post-durably too, and unlike
`commitRefChunk` it has no inner swallow around `materializeCommitted` — a throw there leaves the
transaction applied but the wedge NOT reset, so the next resolution re-applies it and double-bumps
the tail counters.

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` (the wedge-resolution block inside `flushRefBatch`)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

- [x] **Step 1: Write the failing test**

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

- [x] **Step 2: Run to verify it fails**

Expected: the double-count / wedge-not-cleared assertion fails, or the test compiles red.

- [x] **Step 3: Implement**

In the wedge-resolution block: decode the wedged transaction and build the candidate BEFORE calling
`resolveByExactGet` (the bytes are already in the wedge, so no extra I/O), then on `Committed`
install with the same deny-region swap + counter bumps used in Task 3, clear the wedge inside that
region, and move `materializeCommitted()` outside it into a try/catch that swallows (mirroring
`commitRefChunk`). Do NOT retain the candidate in the wedge across attempts — recompute it per
resolution attempt (the wedge can live until remount; a retained full state copy would be a
long-lived memory cost for a rare path).

- [x] **Step 4: Run test + full gate; Step 5: Commit**

Subject: `ca: ref lane — no-throw install + swallow symmetry in wedge resolution (A1 site 2)`

---

## Task 6: Measurement gate (part 2) — prove no regression

**STATUS: DONE** — passed, no regression in the production fold path (`BM_FlushInstallUniqueOwner`);
recorded in the worklog with `069f966c24f`. Step 3's stop-condition was never reached.

- [x] **Step 1: Rebuild and re-run**

```bash
ninja -C build benchmark_cas_ref_protocol > build/build_bench_after.log 2>&1; echo NINJA_EXIT=$?
build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  > tmp/unattended/bench_after.txt 2>&1; echo EXIT=$?
```

- [x] **Step 2: Compare.** Acceptance: **`BM_FlushInstallUniqueOwner` median within 5% of
  baseline** (the production shape — the primary gate), `BM_ScratchCopy` unchanged, and no other
  benchmark worse than 10%. RSS not materially higher.
- [x] **Step 3: If `BM_FlushInstallUniqueOwner` regresses**, STOP and report with the numbers. The
  overwhelmingly likely cause is that the candidate is still sharing its base at fold time (i.e.
  something kept the old state alive past the swap, or the candidate got materialized before the
  PUT after all) — re-read Task 3's comment block rather than tuning around it.
- [x] **Step 4: Record the comparison in the worklog and commit the worklog.**

---

## Task 7: A2 — apply-pending poison state machine

**STATUS: DONE** — `1b5df9dc1a4`. Note for later readers: Task 18 (`252ccbdf2d4`) added a transition
this task does not list — a `NoAttemptSent` pre-attempt refusal must ALSO clear the apply-pending
marker, because nothing was sent and leaving it set makes `confirmExactRef`'s rule 4 answer `Unknown`
forever.

Defense in depth after Task 3, and the confirm's rule 4.

**Files:**
- Modify: `.../Pool/CasRefLedger.h` (enum + per-table field + accessor), `.cpp` (transitions)
- Test: `src/Disks/tests/gtest_cas_ref_install_safety.cpp`

**Interfaces:**
- Produces: `enum class RefApplyState : uint8_t { Clean, ApplyPending, Poisoned };` and
  `RefApplyState applyStateForTest(const RootNamespace &) const;` plus an internal
  `std::atomic<RefApplyState> apply_state{RefApplyState::Clean};` on `RefTableRuntime`.

- [x] **Step 1: Write the failing tests** covering every transition:
  `Clean → ApplyPending → Clean` on a successful commit; `→ Clean` on a conclusive PUT throw
  (`CasRefLedger.cpp:1678-1686`), on `DefiniteFailure` (`:1775-1783`), and on a wedge that resolves
  to absent; `→ Poisoned` when the install probe throws post-durability (only reachable via the
  test seam after Task 3); and `Poisoned` is NEVER cleared by a later successful flush.
- [x] **Step 2: Run — red.**
- [x] **Step 3: Implement.** Arm (`Clean → ApplyPending`) immediately before the PUT — a plain
  `store(std::memory_order_relaxed)`, allocation-free. Clear on the paths above. Set `Poisoned` in
  the (now unreachable in production) failure path. Export a ProfileEvent
  `CasRefApplyPoisoned` on the transition to `Poisoned`.
- [x] **Step 4: Wire rule 4** — nothing to wire yet (the confirm arrives in Task 11); just expose
  the accessor.
- [x] **Step 5: Run tests + full gate; Step 6: Commit.**

Subject: `ca: ref lane — apply-pending poison state machine (A2)`

---

## Task 8: A3 — `precommitAdd` mint-tightening

**STATUS: DONE** — `8874e7dbf1d` (subject as landed: "precommitAdd accepts only ids this transaction
staged (A3, the confirm's ABA barrier)").

**Files:**
- Modify: `.../Pool/CasPartWriteTxn.cpp` (`precommitAdd`, line 920) and `.h` if a member set of
  staged ids is needed
- Modify: `src/Disks/tests/gtest_cas_promote_republish.cpp` (the legality pin at line 283 is
  replaced)

- [x] **Step 1: Write the failing test**

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

- [x] **Step 2: Run — red.**
- [x] **Step 3: Implement.** In `PartWriteTxn`, record every `ManifestId` returned by
  `stageManifest` in a member set; in `precommitAdd`, after the existing namespace check
  (`:926-929`), require membership unless the id is already the committed binding for that ref (the
  idempotent re-drive short-circuit at `:952-954` must keep working — it is evaluated inside the
  closure against live state, so the membership check must not reject that path; verify by running
  the existing re-drive tests).
- [x] **Step 4: Run tests + full gate; Step 5: Commit.**

Subject: `ca: precommitAdd accepts only ids this transaction staged (A3, ABA barrier)`

---

## Task 9: TLA+ model for the confirm protocol

**STATUS: DONE** — `0d1e3f4cc7c`; the BACKLOG entry it upgraded from inference to counterexample is
`c531f0115c4`. Results in `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md`.

**Corrections to this task's own text, found while executing it:**
- **The cfg list below is short by seven.** What landed: `_main`, `_main2r` (two receivers, 4.8M
  states, covering the theorem's quantifier), sabotages `_sab_nogate1`, `_sab_stalecache`,
  `_sab_nopoison`, `_sab_nofence`, `_sab_publishafterconfirm`, `_sab_holeylist`, and witnesses
  `_witness_confirmyes`, `_witness_confirmno`, `_witness_confirmunknown`, `_witness_delete`.
  Poison and fence are SEPARATELY load-bearing — after a poisoned apply the lane is quiescent, so only
  the fence rule catches it — and Step 4's two-sabotage list would have hidden that by lumping them.
- **Step 3's theorem is VACUOUS in the publish-after-confirm sabotage**, because its antecedent
  includes `ActivationDurableBefore[r]`, which that sabotage is precisely about not establishing. That
  config needed a second, antecedent-free invariant (`PromotedNeverDangles`).
- **The fold cursor had to be modelled honestly** — advancing over the records a round OBSERVED, with
  observation as a parameter — rather than assuming every durable record is seen. That is what
  produced `_sab_holeylist`, and with it the load-bearing caveat this plan did not anticipate:
  `_main` runs at `MaxHoles = 0`, i.e. it ASSUMES a completeness property the shipped code does not
  establish. So `_main` passing means "the confirm protocol adds NO NEW dangle path", **not** "a
  confirmed relink cannot dangle" — see BACKLOG {#list-as-journal-dataloss-2026-07-25}. Do not cite
  it as dangle-freedom until the journal-chain fix lands.

**Files:**
- Create: `docs/superpowers/models/CaRelinkConfirmCore.tla`
- Create: `docs/superpowers/models/CaRelinkConfirmCore_main.cfg`,
  `..._sab_nogate1.cfg`, `..._sab_stalecache.cfg`, `..._witness_confirmno.cfg`
  (plus the seven listed in the corrections above)
- Create: `docs/superpowers/models/run_relinkconfirm.sh` (copy `run_ackfloor.sh`, change the module)

- [x] **Step 1: Read `docs/superpowers/models/README.md`** for the naming/gating conventions
  (`_sab_*` must FAIL, `_witness_*` violation means reachable).
- [x] **Step 2: Model** two journals (sender, receiver) with a monotonic id per journal, a GC round
  with a fold cursor and three-phase graduation (condemn → delete_pending → delete) with sparing on
  positive in-degree, and the confirm as an atomic predicate over the sender's state.
- [x] **Step 3: State the property**

```
THEOREM ConfirmedRelinkNeverDangles ==
  [](\A r \in Receivers :
       (Promoted[r] /\ ConfirmYesObservedAt[r] /\ ActivationDurableBefore[r])
         => BlobsOf(Manifest[r]) \subseteq LiveBlobs)
```
- [x] **Step 4: Sabotage configs** — `_sab_nogate1` (confirm answers yes on ref-name match only →
  must produce a violation, proving gate 1's exactness is load-bearing); `_sab_stalecache` (confirm
  reads a cache that may lag a durable removal → violation, proving the lane-quiescence rules).
- [x] **Step 5: Run** `cd docs/superpowers/models && ./run_relinkconfirm.sh CaRelinkConfirmCore_main`
  (positive: "Model checking completed", no violation) and each `_sab_*` (must violate).
- [x] **Step 6: Record results** in `CaRelinkConfirmCore_RESULTS.md` and **commit**.

---

## Task 10: `confirmExactRef` — the ledger-side lane snapshot (gate 1)

**STATUS: DONE** — `7da3586ed29`, gate 1348/1348 (1335 baseline + 13 new).

**Correction to Step 3, and it is the most valuable thing this task produced:** Step 3 says to take
`state_mutex` "while still holding the queue mutex", which reads as a BLOCKING acquire. A blocking
acquire is wrong. `ensureRefTableRecovered` holds `state_mutex` across its whole `LIST` and replay, so
a confirm would wait on someone else's recovery *while holding the pool-wide append-admission mutex* —
stalling every table's append lane for up to the full retry envelope. The zero-I/O contract is then
broken by proxy: the query issues no request but is paid for by one. The landed code **try-locks**, and
failing to take the lock is simply one more ambiguity → `Unknown`. The test pins the answer arriving in
under 5 s against a 20 s parked `LIST`, where it previously took 20 s.

Also recorded in the function's own comment, because it is the standing limit of what a `Yes` means: a
`Yes` does not prove this runtime's recovered view is a COMPLETE replay of the durable log. Rules 2-4
exclude every way this mount can lag its own durable writes; a recovery that silently observed less
than it should is a different defect in a different component (BACKLOG
{#list-as-journal-dataloss-2026-07-25}, and the downgraded review "blocker 1",
{#partb-review-blocker1-downgraded}).

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

- [x] **Step 1: Write the failing tests** — one per rule, from the spec §testing "Gate 1
  determinism" list: repointed live part → `No`; dropped+recreated → `No`; cold/evicted/recovering
  → `Unknown` with zero backend requests (assert via `CountingBackend`'s counters);
  pending/in-flight/wedge/mid-tenure (`CarvePhaseForTest::ChunkReseed`) → `Unknown`; poison set →
  `Unknown`; quiescent exact match → `Yes`; and a race test where an append admitted concurrently
  with the snapshot is ordered strictly after it.
- [x] **Step 2: Run — red.**
- [x] **Step 3: Implement** exactly the six-rule snapshot from the spec: take `ref_queue_mutex`,
  find the runtime **without creating one** (`ref_tables.find`, not `getRefTableRuntime`, which
  would create an unrecovered entry), then take `state_mutex` while still holding the queue mutex;
  evaluate warm → quiescent (`wedge`, `pending`, `leader_active`) → poison → exact row equality →
  fence last (`fence_ok_fn()` and `!superseded_by_remount`); return `Unknown` for every ambiguity.
  **Do not call** `ensureRefTableRecovered`, `sweepStalePrecommitsForRead`, or
  `maybeScheduleSnapshotPublish` — all three are in `resolveRef` and all three can do I/O.
- [x] **Step 4: Run tests + full gate; Step 5: Commit.**

Subject: `ca: confirmExactRef — exact-token lane-linearized confirm (gate 1)`

---

## UPSTREAM AUTHORIZATION AND ITS LIMITS (user decision, 2026-07-25) {#upstream-authorization}

Applies to Tasks 11, 13 and 14 — every task in this plan that touches
`src/Storages/MergeTree/DataPartsExchange.cpp` or the interserver wire.

**Authorized:** the CAS parts-exchange portion only — the code we wrote ourselves (the fetch-by-relink
path: the `content_addressed_pool_uuid` advertisement, the `content_addressed_relink` cookie, and the
relink send/receive branches). Nothing else in that file, and nothing in generic MergeTree, Replicated or
Keeper code or formats.

**Required shape:** the change must be LOCALIZED, as far as possible, to ONE function/method on the sender
side and ONE on the receiver side. From the current file that means:
- sender: `Service::processQuery` (already carries the relink branch),
- receiver: `Fetcher::relinkPartToDisk` (where the promote happens, so where a confirm must precede it).

`Fetcher::fetchSelectedPart` already advertises the pool uuid and reads the relink cookie; do NOT spread
new confirm logic into it as well unless there is no alternative, and if there is none, say so explicitly
rather than widening the footprint quietly.

Everything else lives behind `IContentAddressedExchange` and inside the CAS tree. This authorization does
NOT relax [[feedback_cas_upstream_coupling_minimization]]: generic code must not learn CAS-specific
concepts beyond what the existing relink path already carries.

### As-built footprint: two ACCEPTED deviations and one conflation (recorded 2026-07-26) {#upstream-authorization-as-built}

The Part B codex review (BACKLOG {#partb-review-findings}, "footprint objections") raised three points
about the landed footprint. They are written down here, next to the rule they are measured against, so a
future reader neither re-litigates them nor mistakes them for oversights.

1. **`DataPartsExchange.h` exposes CAS-specific `allow_ca_relink` and service helpers beyond the opaque
   enum.** True, and **knowingly accepted at the time and reported to the user** — `allow_ca_relink` is
   the capability parameter Task 15 exists to add, and it replaced the implicit `try_zero_copy &&
   !to_detached` brake that was worse coupling in the same file.
2. **The receiver work spans `fetchSelectedPart` AND `relinkPartToDisk`, and the sender work spans
   `processQuery` and two helpers**, where the "required shape" above asks for one function per side.
   Also **knowingly accepted and reported**. The rule's intent held where it mattered: Task 14 moved the
   confirm OUT of `fetchSelectedPart` and into `relinkPartToDisk` precisely because the plan's Step 2
   would have spread it further (see Task 14's corrections), and Task 11's `DataPartsExchange.cpp`
   change is purely additive with `processQuery` untouched.
3. **The reviewer also counted the `src/Common/ProfileEvents.cpp` registration as outside the approved
   failpoint scope. That is a CONFLATION and the objection does not stand:** that registration came from
   Task 18, not from the Task 16 failpoint approval, and a ProfileEvent registration is not protocol
   coupling. The failpoint approval's scope was exercised exactly as recorded — the registration lines in
   `src/Common/FailPoint.cpp` and nothing else in that file.

Points 1 and 2 are recorded as accepted, not as debt. If the boundary is to be tightened later, that is a
new decision, not a correction of these tasks.

---

## Task 11: Storage-level confirm + gate 0 + exchange interface

**STATUS: DONE** — `41fa94de18b`, gate 1356/1356.

**Corrections to this task's own text:**
- The footprint came out TIGHTER than the constraint asked: `DataPartsExchange.cpp` is purely
  additive — one self-contained `Service::resolveContentAddressedConfirm`, zero lines removed, no
  existing function modified. `processQuery` is deliberately UNTOUCHED here; its dispatch is Task 13's.
  The header is in the diff only because the build runs `-Weverything -Werror`, where a yet-uncalled
  function in an anonymous namespace is a `-Wunused-function` failure and a private member declaration
  is not.
- **Step 1's deferral of the gate-0 cases to Task 16 was never honoured.** The gtest says so in its own
  comment (`gtest_cas_confirm_exact_ref.cpp:689`: `Deleting`, absent, other-disk and the
  `MOVE ... TO DISK` same-name case "belong to the Task 16 pytest battery") and none of Task 16's
  eleven integration tests covers them. A declared handoff that nobody completed is indistinguishable
  from coverage — see **Task 22**, which exists to close it.
- Carried forward from Task 10 and made explicit at this boundary: rule 6 (fence) is evaluated LAST, so
  gate 0's `No`, gate 1's `No` and every `Unknown` reach the SAME caller outcome. Any future code that
  wants to treat `No` as authoritative must hoist rule 6 first.

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

- [x] **Step 1: Write the failing tests** for gate 0's filter semantics (spec §testing "Gate 0"):
  `Deleting` → `No`; absent/other-disk → `No`; the `Deleting → Outdated` rollback state must still
  be rejected by gate 1 when the ref is gone; `MOVE ... TO DISK` same-name-other-disk → `No` unless
  the matched instance is the part's current disk. These are integration-level — put them in the
  Task 16 pytest file and keep this task's unit tests to `ownsNamespace` routing.
- [x] **Step 2: Implement `ownsNamespace` + `confirmExactRef`** on
  `ContentAddressedMetadataStorage` as thin forwards (`store()`/the ledger), with the
  storage-lifecycle gates applied (a not-started / terminal disk answers `Unknown`, never throws
  out of a confirm).
- [x] **Step 3: Implement the `Service`-side resolution**: enumerate `data.getDisks()`, map each
  through the existing `tryGetContentAddressedExchange` (`DataPartsExchange.cpp:117-122`), keep the
  ones where `ownsNamespace(...)` is true; **zero or more than one match → `Unknown`**; then gate 0:
  look up the part by name in the parts set and require `Active`/`Outdated` **on the matched
  instance's disk** (the `MOVE ... TO DISK` same-name case). Read the parts set under its own lock
  and release it before calling into the ledger.
- [x] **Step 4: Run gate; Step 5: Commit.**

Subject: `ca: exchange-level confirm (ownsNamespace + confirmExactRef) with the gate-0 part filter`

---

## Task 12: ~~`PreparedRelink`~~ `PreparedPartWrite` handle + typed prepare boundary

**STATUS: DONE** — `41a248fd9c5`, gate 1361/1361. Nothing under `src/Storages`.

**Corrections to this task's own text:**
- **The heading named the wrong type.** It said `PreparedRelink` while this task's own Interfaces block
  says `PreparedPartWrite`, and the two are DIFFERENT LAYERS: the receiver's token and decoded entries
  belong to Task 14's exchange boundary (`ICaPreparedRelink`), not to a CAS-internal facade. The
  heading above is corrected in place; the commit subject as landed still says `PreparedRelink handle`.
- **The file list below names two files that belong to Task 14** — `ContentAddressedExchange.h` and
  `ContentAddressedMetadataStorage.h/.cpp`. They were correctly left alone here.
- Two files this task DID touch that the list does not name: `Pool/CasPool.{h,cpp}` and
  `Pool/CasRefLedger.{h,cpp}`, for a `livePrecommitsForTest` seam — Step 1 requires asserting through
  the ledger's precommit view and no accessor existed.
- **"move-only" (below) was implemented as move-construct + move-assign, and the assignment was later
  DELETED** by the Part B review fix (`8e6fe6ef0af`, major 4). It has no correct implementation:
  overwriting a handle that still owes a terminal must discharge that duty first, `abandon` appends
  through the ref lane and can FAIL, and an assignment cannot report that — so the landed version
  overwrote the destination even when `abandonBuildBestEffort` returned false, permanently dropping a
  cleanup owner. Read "move-only" below as move-CONSTRUCT-only; pinned by
  `CasPartFolderAccess.PreparedPartWriteIsNotMoveAssignable`.
- The `abort`-appends-the-precommit-removal property is MUTATION-VERIFIED, not asserted: `abandon` was
  commented out, the test failed on exactly that assertion, and the mutation was reverted.

**Files:**
- Modify: `.../Parts/PartFolderAccess.h/.cpp` (split `publishEntries`, currently `.cpp:338-372`)
- ~~Modify: `.../ContentAddressedExchange.h`, `.../ContentAddressedMetadataStorage.h/.cpp`~~
  (Task 14's, not this task's — see the corrections above)
- Modify (not in the original list): `.../Pool/CasPool.h/.cpp`, `.../Pool/CasRefLedger.h/.cpp`
  (`livePrecommitsForTest`)

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

- [x] **Step 1: Write the failing tests** — prepare-then-promote equals today's `publishEntries`
  outcome; prepare-then-abort leaves no committed ref AND appends the precommit removal (assert via
  the ledger's precommit view — no same-epoch leak); double-terminal is rejected.
- [x] **Step 2: Run — red.**
- [x] **Step 3: Implement** by splitting `publishEntries` at `PartFolderAccess.cpp:353`: everything
  up to and including `precommitAdd` moves into `prepareEntries`; `promoteBuild` becomes the
  handle's `promote`; the existing `catch` → `abandon` → rethrow discipline (`.cpp:361-371`) moves
  into the handle's `abort` and into `prepareEntries`'s own failure path. **Keep `publishEntries`**
  implemented in terms of the new pair, so every existing caller is untouched.
- [x] **Step 4: Run gate; Step 5: Commit.**

Subject: `ca: split publishEntries into prepare/promote/abort (PreparedRelink handle)`

---

## Task 13: Wire protocol — the confirm action and the source token

**STATUS: DONE** — `c74d8e6549a`, gate 1363/1363. Deliberately HALF-landed in the safe direction: the
server's advertised clamp rises to 11, but the client still advertises 10 and the relink-offer gate is
still keyed on 10, so no receiver asks for a confirm. Raising the gate alone would silently disable
relink; bumping the client alone would make the receiver claim it confirms when it does not. Those two
edits land together in Task 14.

**Corrections to this task's own text:**
- **Step 2's premise is wrong twice, and the second way is a TOCTOU.** It says the sender "already has
  all of these (the manifest bytes carry `ref`/`root_namespace_id`)". Decoding a manifest in
  `DataPartsExchange.cpp` would put CAS types in generic code; and minting the token from a SECOND
  lookup lets a repoint between the OFFER and the MINT hand the receiver a token naming a manifest
  whose entries it never adopted — a `Yes` for that protects the wrong blobs. The lookup now returns
  the bytes AND the token from ONE resolution.
- **Task 11's "three-line dispatch" estimate for Step 3 is wrong.** Within `processQuery` it is ~21
  added lines across eight places, because the confirm must run BEFORE the part-name validation (a
  confirm request has no part), and the offer rename touches its call sites. Footprint stayed within
  {#upstream-authorization}: one function on the sender side.
- **Stated deviation from the spec's literal `yes`/`no`/`unknown` wire:** the wire is BINARY. Only
  `yes` authorizes; `No`, `Unknown` and an absent cookie all read as `unproven`. Putting `no` on the
  wire as a distinct value would invite a receiver to treat it as knowledge, and with rule 6 evaluated
  last it is not — a fence-lost mount answers `No`. The distinction is logged where the gate that
  produced it can be named, never transmitted.
- Old peers, fail-closed both ways: an old receiver ignores an unknown cookie by name; an old sender
  offers relink with NO token, and that absence is the capability signal Task 14 keys on.
- Removed a re-check the agent had first added: the manifest reader already enforces it and throws, so
  the addition would only have converted a loud corruption error into a silent byte fetch.

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.h/.cpp`

- [x] **Step 1: Add the protocol version and token constants** next to the existing ones
  (`.cpp:76-111`):
```cpp
constexpr auto REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM = 11;
constexpr auto CA_CONFIRM_ACTION_PARAM = "content_addressed_confirm";     /// request: "1" selects the confirm action
constexpr auto CA_CONFIRM_TOKEN_COOKIE = "content_addressed_source_token"; /// response cookie on the relink offer
```
  and bump the version the server advertises (`.cpp:185`) and the client sends (`.cpp:523-529`).
- [x] **Step 2: Mint the token on the sender** inside the existing relink branch
  (`.cpp:249-280`), as a compact text encoding of
  `{pool_uuid, server_root_id, root_namespace, ref_name, part_name, manifest_ref}` — the sender
  already has all of these (the manifest bytes carry `ref`/`root_namespace_id`, see
  `CasPartManifestFormat.h:76-78`). Add it as a response cookie next to `CA_RELINK_COOKIE`
  (`.cpp:268`).
- [x] **Step 3: Handle the confirm action** at the TOP of `Service::processQuery`
  (before the body reset and before `part` is required/parsed, `.cpp:170-205`): parse the token,
  resolve the instance via Task 11's routing, answer `yes`/`no`/`unknown` as a response cookie plus
  an empty body, and return.
- [x] **Step 4: Test** with an integration test in Task 16 (a unit test cannot exercise the HTTP
  endpoint); here, add a round-trip test for the token encoder/decoder (a pure function — put it in
  `gtest_cas_confirm_exact_ref.cpp`), including rejection of malformed and over-long fields.
- [x] **Step 5: Run gate; Step 6: Commit.**

Subject: `ca: interserver confirm action + source token (relink protocol v11)`

---

## Task 14: Receiver flow — prepare, confirm, promote, and the typed failure taxonomy

**STATUS: DONE** — `260a6f81169`, gate 1366/1366; `test_cas_replicated_relink` verified on real RustFS
with the node logs proving the relink actually ran. Later hardened by the Part B review fix
`8e6fe6ef0af` (see the last bullet).

**Corrections to this task's own text — two deviations, both toward the spec:**
- **Step 2 puts the confirm in the receiver branch of `fetchSelectedPart` (`.cpp:728-771`). That
  contradicts {#upstream-authorization}'s one-function-per-side rule and the spec's own "Fetcher owns
  confirm, abort and the throw." The authorization won:** the confirm lives in
  `Fetcher::relinkPartToDisk`.
- **Step 1's `void promote()` cannot express the spec's "receiver-side ref conflict is mechanism
  fallback"** without leaking CAS error codes into generic code. `promote` therefore returns a TYPED
  answer, and `abort` is `noexcept` on the interface, since it runs with the row-3 error in flight.
- A catch-all was REMOVED that the spec contradicts: the old adopt path treated ANY error as grounds
  for a byte fetch — a fourth cause where the spec lists three. An unclassified local error is not
  evidence that a byte fetch would fare better.
- **A lifetime hazard the split created, not anticipated here:** `PreparedPartWrite` keeps its owner as
  a RAW pointer, and the handle now spans a network round trip, so the wrapper holds a `shared_ptr`
  snapshot of the part-folder facade — otherwise a concurrent shutdown dangles it.
- Step 4's grep was done and the throw is not swallowed: every `catch` between here and the queue was
  read.
- **Later correction (`8e6fe6ef0af`, review major 3):** `promote` mapped every `NETWORK_ERROR` to
  `MechanismFallbackAllowed`, but the promotion PUT MAY HAVE LANDED — a byte re-fetch on top of that is
  a sequential double publication the taxonomy says is impossible. The outcome is now tri-valued
  (`Committed` / `MechanismFallbackAllowed` = proven-not-committed / `Unresolved` = may have committed
  → retry the whole fetch later), and the post-commit window is closed the way Part A closed its own:
  outcome strings copied before the append, the commit recorded inside `DENY_ALLOCATIONS_IN_SCOPE`
  straight after it, and the catch rethrows rather than abandoning a committed build.

**Files:**
- Modify: `.../ContentAddressedMetadataStorage.h/.cpp` (`adoptPartFromManifest` → typed prepare),
  `.../ContentAddressedExchange.h`, `src/Storages/MergeTree/DataPartsExchange.cpp`
  (`Fetcher::relinkPartToDisk` `.cpp:1107-1169` and the receiver branch `.cpp:728-771`)

- [x] **Step 1: Replace the boolean adoption** with a typed prepare on the exchange interface:
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
- [x] **Step 2: Rewrite the receiver branch** (`.cpp:728-771`): parse the relink cookie AND the
  token cookie → `prepareAdoptFromManifest` → on `MechanismFallbackAllowed` keep today's
  `fall_back_to_byte_fetch()` → otherwise issue the confirm request → on `yes` `promote()` and
  build the part exactly as `relinkPartToDisk` does today → on anything else `abort()` and
  **throw** a locally generated retry-later `NETWORK_ERROR` with a message naming the source and
  part.
- [x] **Step 3: Scope guard** — the handle must be aborted on every non-promote exit including
  exceptions; use `SCOPE_EXIT` around the confirm+promote region.
- [x] **Step 4: Verify the throw is not swallowed** — grep the call chain
  (`fetchSelectedPart` → `fetchPart` → `executeFetch`) for `catch` blocks that would convert it
  back into a byte re-request; the typed boundary exists precisely because
  `adoptPartFromManifest`'s old catch-all (`ContentAddressedMetadataStorage.cpp:2002`) did that.
- [x] **Step 5: Run gate; Step 6: Commit.**

Subject: `ca: relink receiver — prepare/confirm/promote with a typed failure taxonomy`

---

## Task 15: B66b — `allow_ca_relink` capability, recursion brake, detached target

**STATUS: DONE** — `fac69e10dbd`, gate 1368/1368; `test_cas_replicated_relink` passes on real RustFS
with the relink still firing. The brake it wired was UNTESTED and said so; that gap was carried into
Task 16 by `69e81b12732` and closed there.

**Corrections to this task's own text:**
- **Step 4 is DEAD — it needed no code and none was written.** `src/Storages/StorageReplicatedMergeTree.cpp`
  was NOT touched, as {#upstream-authorization} requires: the parameter is appended after `dest_disk`
  with default `true`, so all four positional call sites keep compiling and the server links — which is
  the proof, not the argument. `allow_ca_relink` appears nowhere in that file.
- **The line numbers in the file list are stale, and the two sources that corrected them DISAGREE.**
  Task 16's own corrections block says the two manual detached callers are at `:8125` (`FETCH PART`)
  and `:8273` (`FETCH PARTITION`), "not `:8281`". The worklog's 20:29 UTC entry instead says the
  plan's `:8125`/`:8281` are stale "vs the real `:3386`/`:3514`/`:5632`/`:5818`". The two are probably
  counting different things (DDL detached callers vs all `fetchSelectedPart` call sites), but neither
  source says so. **Re-derive with a grep before trusting either.**
- **There is a THIRD detached caller neither this plan nor the spec mentions:**
  `executeClonePartFromShard` (`:3475`). Unlike the two DDL callers it IS a queue entry, which matters
  for the taxonomy: two of the three detached callers have no queue entry, so row 3 rests on the
  statement failing to the user rather than on queue re-execution (verified by checking that
  `NETWORK_ERROR` is not in the swallow list). It is deliberately left untested — see Task 16's
  corrections.
- Beyond the plan: the ref name is no longer composed in generic code. The receiver entry now takes a
  disk-relative part path, symmetric with the sender's, and the CAS side derives the namespace and ref
  itself; `detached/` folds for free, and a target that is not a live part directory throws instead of
  inviting a byte fallback to the same wrong place. That closes CAS-archaeology finding §9 #12, whose
  recommended fix was verbatim this.

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.h/.cpp`
- ~~Modify: `src/Storages/StorageReplicatedMergeTree.cpp` (the two manual detached callers, `:8125`, `:8281`)~~
  **NOT modified — see the corrections above. Step 4 required no edit.**

- [x] **Step 1: Add the capability parameter.** Replace the `try_zero_copy && !to_detached` gate
  (`.cpp:545`) with a dedicated `allow_ca_relink` parameter on `fetchSelectedPart`
  (default `true`), leaving `try_zero_copy` untouched for real zero-copy (`.cpp:566`).
- [x] **Step 2: Wire the recursion brake.** Every `fall_back_to_byte_fetch()` re-request
  (`.cpp:733-739`) must pass `allow_ca_relink=false` — this replaces the brake that
  `try_zero_copy=false` provided implicitly. Without it a persistent mechanism failure loops.
- [x] **Step 3: Pass `to_detached` into `relinkPartToDisk`** and construct the temporary storage
  under the detached parent (today the active parent is hardcoded, `.cpp:1128`); the CA router
  already folds any `detached/<name>` ref (`ContentAddressedMetadataStorage.cpp:1241`).
- [x] ~~**Step 4: Update the two manual detached callers** to pass `allow_ca_relink=true`
  independently of `try_fetch_shared=false`.~~
  **DEAD STEP — no edit was needed and none was made.** The parameter defaults to `true` and the call
  sites are positional, so they stop before it. Making this edit would have touched
  `StorageReplicatedMergeTree.cpp`, which {#upstream-authorization} does not cover.
- [x] **Step 5: Do NOT change detached finalization** — it stays `renameTo(detached/<part>, true)`
  (`StorageReplicatedMergeTree.cpp:5719`) with its existing collision behavior.
- [x] **Step 6: Run gate; Step 7: Commit.**

Subject: `ca: B66b — relink into detached behind allow_ca_relink, with a recursion brake`

---

## Task 16: Integration test battery

**STATUS: DONE** — `037cc6d0e41`, all eight steps; 11 integration tests passing in 55 s, CA battery
unchanged at 1368/1368. Part B closed here.

**CARRIED IN FROM TASK 15 — the recursion brake is wired but UNTESTED.** Task 15 (`fac69e10dbd`) could
not prove it and said so instead of inventing a test. Read its reasoning before writing one: the
reservation-outside-the-pool exit is self-braking regardless and must NOT be mistaken for evidence the
brake is unnecessary; the exits the brake actually bounds — the mixed-build cookie-value mismatch and
taxonomy rows 1, 2 and 5 — are properties of the sender/receiver PAIR and are unreachable from
configuration. Proving it needs a failpoint, which is two lines:

```
REGULAR(cas_relink_receiver_force_mechanism_failure)   // src/Common/FailPoint.cpp
fiu_do_on(FailPoints::cas_relink_receiver_force_mechanism_failure, { return nullptr; });  // after the token gate
```

`src/Common/FailPoint.cpp` is a SHARED UPSTREAM surface and sits outside {#upstream-authorization} — it is
a registration line in a generic registry, not the CAS parts-exchange portion. **APPROVED by the user
2026-07-25**: adding the failpoint is authorized. Scope of that approval as exercised: the registration
line(s) in that file and nothing else in it. This also unblocks Task 16 steps 1 and 3, which need their
own injection points for the same reason. With the failpoint, `SYSTEM ENABLE FAILPOINT` plus `SYSTEM SYNC REPLICA` is a real test:
unbraked it recurses to a stack overflow, braked it is exactly one relink attempt and then bytes.

Note also: the plan's command lines use `python`, which is not on PATH in this environment — use `python3`.


**Files:**
- Modify/create under `tests/integration/test_cas_replicated_relink/`

- [x] **Step 1: Race test** — failpoint between `precommitAdd` and the confirm; sender drops the
  part in the window → confirm `no` → abort → retryable failure → assert the queue re-selects
  (covering-part / other replica) and that **no** byte re-request went to the original sender.
  (`test_confirm_refuses_when_source_dropped_in_window`; the queue re-selected the covering part.)
- [x] **Step 2: Happy path** — relink proof: `CasBlobPut == 0` on the receiver.
  (`test_relink_happy_path_proof`. `CasBlobPut == 0` is corroboration only — see the note below.)
- [x] **Step 3: codex-6 regression** — stall the receiver's publish across ≥ 3 GC rounds with a
  small `old_parts_lifetime`, merge the sender's part away, GC to fixpoint → the stalled attempt
  must NOT produce a committed ref; fsck clean, `dangling=0`.
  (`test_stalled_publish_protects_source_blobs_and_commits_nothing`, with an explicit soundness guard:
  the same blobs ARE reclaimed once the attempt is abandoned.)
- [x] **Step 4: Recursion brake** — force a persistent mechanism failure → exactly one relink
  attempt then bytes, no loop. (`test_recursion_brake_bounds_relink_to_one_attempt`; the assertion is
  the COUNT of offers the sender made, which is 1.)
- [x] **Step 5: B66b** — `FETCH PART` into `detached/` on both manual callers → relink proof +
  `ATTACH` reads correctly; cross-pool → bytes. (Three tests. The third manual detached caller,
  `executeClonePartFromShard`, is deliberately not covered — see the note below.)
- [x] **Step 6: RPL-5 slice** — `REPLACE PARTITION` / `ATTACH PARTITION ... FROM` on the 2-replica
  fixture: assert the queue-cloned `REPLACE_RANGE` fetch relinks (blob-count proof).
- [x] **Step 7: Version mix** — confirm-capable receiver × legacy sender cookie → clean byte
  fallback. (`test_version_mix_legacy_peer_gets_bytes` covers the SENDER-side half of the gate, which
  is the half reachable without a second binary — see the note below.)
- [x] **Step 8: Run and commit**

**Corrections to this task's own text, found while executing it:**
- "the existing relink proof is `count_blobs()` staying flat across a fetch … reuse that helper for
  every new relink proof" is WRONG as a proof, and following it would have produced seven green and
  worthless tests. A BYTE fetch onto a content-addressed disk writes the same content, which
  deduplicates, so its blob-count delta is zero too. Every relink assertion instead keys on the
  receiver's `Relink of part <p> … finished (no bytes transferred).` line, which is reachable only
  after a confirm `yes` and a `Committed` promote; the blob counts stayed as corroboration. For the
  same reason `CasBlobPut == 0` (step 2's stated proof) is corroboration, not proof.
- Counting blobs by TOTAL is also wrong on this fixture: `gc_interval_sec` is 1, so unrelated debris
  can be reclaimed mid-test and the count goes DOWN. The tests assert on the KEY SET delta instead.
- Two tables inserting `numbers(0, N)` with the same schema produce byte-identical blobs, so the
  second table's "new blob" delta is EMPTY. Any test reasoning about a specific part's blobs must
  make its rows unique first; this cost one full run to discover.
- `SYSTEM … FORMAT TSVWithNames` is a syntax error (`ASTSystemQuery` is not an `ASTQueryWithOutput`),
  and the server container has no `clickhouse-client` — `SYSTEM CONTENT ADDRESSED FSCK` has to be run
  as `clickhouse client --format …` inside the container.
- Task 15's step 4 ("update the two manual detached callers to pass `allow_ca_relink=true`") needed no
  edit at all and none was made: the parameter defaults to `true`, and `allow_ca_relink` appears
  nowhere in `StorageReplicatedMergeTree.cpp`. The two callers are at `:8125` (`FETCH PART`) and
  `:8273` (`FETCH PARTITION`), not `:8281`.
- **`executeClonePartFromShard` (`:3475`) is a THIRD detached caller** and is deliberately left
  uncovered. It reaches `relinkPartToDisk` with exactly the same arguments as the two DDL callers
  (`to_detached=true`, default `allow_ca_relink`), and finalizes the same way
  (`renameTo(detached/<part>)`), so it adds no new behaviour to prove; the one thing that differs is
  its recovery on taxonomy row 3, which is queue-driven and is already exercised by step 1 on the
  active path. Reaching it needs `part_moves_between_shards_enable` and a two-shard fixture, which
  would buy a second copy of an already-proven path.
- **What step 7 does NOT cover.** The receiver-side row-1 branch — a genuinely old sender that offers
  a relink and attaches NO source token — is unreachable here: `getRelinkOffer` never yields an offer
  without a token, so producing one needs either another failpoint or a pre-`WITH_CA_CONFIRM` binary
  to play the sender. The test covers the sender-side half instead (a peer advertising protocol 10 is
  served bytes, with an otherwise identical protocol-11 request as the positive control), which is the
  half that decides whether a mixed cluster can ever see an unconfirmed relink. Covering the receiver
  half faithfully would need a MITM interserver proxy registered as a fake replica in ZooKeeper.

- **What the review found still uncovered AFTER this battery** (BACKLOG {#partb-review-findings}, test
  gaps): every gate-0 case (see **Task 22**); the genuinely-uncovered subset of the review's
  missing-test list (see **Task 23**); and a pre-existing test in this very file that is green for the
  wrong reason (see **Task 24**).

```bash
ninja -C build clickhouse > build/build_srv_t16.log 2>&1; echo NINJA_EXIT=$?
python3 -m ci.praktika run "integration" --test test_cas_replicated_relink > build/test_t16_relink.log 2>&1; echo EXIT=$?
```
The fixture is 2-replica with `with_rustfs=True` (RustFS, not MinIO — the CAS probe needs real
conditional-PUT semantics) over the shared-pool disk in
`tests/integration/test_cas_replicated_relink/configs/storage_conf.xml`.
~~the existing relink proof is `count_blobs()` staying flat across a fetch (`test.py:47-51`). Reuse
that helper for every new relink proof rather than inventing a second one.~~
**DO NOT DO THIS — it is the defect the corrections block above records, repeated here in the original
text.** A flat blob count is satisfied by a BYTE fetch too, because the same content deduplicates.
Key every relink assertion on the receiver's `Relink of part <p> … finished (no bytes transferred).`
log line; `count_blobs()` key-set deltas are corroboration only. Task 16 landed a second fixture,
`configs/storage_conf_other_pool.xml`, for the cross-pool and version-mix cases.

---

## Task 17: ca-soak scenario S42 — allocation-fault soak

**STATUS: DONE** — `c44cb6cbe44`. Two smoke runs at dev scale; the second read 26/28 pass, 1 skipped,
1 inconclusive (the soundness guard itself, targeted=0 against generic=70).

**Corrections to this task's own text:**
- **Step 4's ORDERING was wrong and made leg C vacuous every time.** Running the snapshot/journal
  oracle AFTER the forced GC cannot work: ref cleanup deletes exactly the logs the oracle needs to
  replay. The card takes its oracle scan at QUIESCENCE, before the forced GC.
- **Step 5's soundness guard, implemented literally, makes a conclusive green unreachable.** The
  failpoint term is zero BY CONSTRUCTION — the install-region seam is gtest-only and no CAS failpoint
  is registered for a running server — so S42 could return `inconclusive` or `fail` but never a
  conclusive green however healthy the system was. **Superseded by the user's Q4 decision** (BACKLOG
  {#q4-s42-green}) and implemented in the sibling follow-ups plan's Task 9 (`402a85c4a64`): green is a
  consistent state on disk and in memory; the window-specific targeting becomes REPORTED; only the
  GENERIC anti-vacuity guard still gates.
- **Writing the card found a harness regression that matters more than the card.**
  `observe.gc_log_rows` still selected `min_ack`, a column the GC log schema no longer has, so the
  query raised `UNKNOWN_IDENTIFIER`, a bare `except` turned that into an empty list, and **every GC
  verdict in every scenario was vacuously true**. Fixed in the same commit; the deeper `except`
  hazard and `assert_gc_no_failed`-passes-on-empty were closed after it in `017d5fa22a4` /
  `646fdac53fc`.

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s42_alloc_faults.py`
- Modify: `utils/ca-soak/scenarios/README.md` (register S42 in the card list)

Design: `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` §testing,
plus `utils/ca-soak/scenarios/BACKLOG.md` §s42-allocation-fault-soak (read BOTH before writing).

- [x] **Step 1: Copy the card skeleton** from `cards/s39_lease_fault_tolerance.py` (`@register`,
  `name`/`title`/`priority`/`param_table`/`run`), keeping its soundness-guard style.
- [x] **Step 2: Leg A** — arm `memory_tracker_fault_probability` per query through the driver's URL
  parameters (`utils/ca-soak/soak/cluster.py:247`) over a soak-shaped insert/select workload.
- [x] **Step 3: (MOVED to Task 20 / scenario S43.)** Thread-allocation faults were originally leg B
  here. They are a different fault CLASS with a different blast radius — they cannot reach the CAS
  commit path at all, because the ref append lane runs on the caller's thread — and mixing them into
  one card destroys attribution when something breaks. S42 is therefore query-thread allocation
  faults only.
- [x] **Step 4: Leg C** — disarm, quiesce, GC to fixpoint, fsck, restart, and compare; ALSO replay
  from the last pre-fault snapshot plus the raw tail logs and compare against the live cache, and
  assert no snapshot advanced across a poisoned transaction (`CasRefApplyPoisoned` from Task 7).
- [x] **Step 5: Soundness guard** — require a nonzero targeted signal (the poison-transition
  counter, or a post-PUT failpoint hit), NOT merely a nonzero `MEMORY_LIMIT_EXCEEDED` count; report
  `inconclusive` otherwise.
- [x] **Step 6: Oracle** — queries may fail; invariants may not: zero `LOGICAL_ERROR`/abort in
  `err.log` (only expected injected errors during the armed window), acked-vs-lost = 0, replicas
  agree, fsck `dangling=0`/`unaccounted=0`, GC recovers after disarm, no permanently wedged lane,
  no query hung past a bound.
- [x] **Step 7: Run at dev scale**
  `cd utils/ca-soak && python3 -m scenarios.run --scenario S42 --seed 1 --scale dev`
  and iterate until it is GREEN or produces a genuine, triaged finding.
- [x] **Step 8: Commit** the card plus its `RUN_HISTORY.md` entry.

---

## Task 18: do not wedge a ref lane when no attempt was ever sent (finding #37 defect 3, behavioural half)

**STATUS: DONE** — `252ccbdf2d4`, gate 1352/1352 (1348 baseline + 4 new), plus the follow-on
`99684c66655`.

**Corrections to this task's own text:**
- **Step 3's "and the same decision in the wedge-resolution path" cannot be done: that path has no
  pre-attempt gate to make the decision at.** Documented in place rather than worked around.
- **Step 5's TLA+ target does not exist.** The nearest model already ADMITS this transition, so this
  change widens which C++ paths reach it rather than what the model admits. That model was re-run:
  5/5 expectations met. No new model was written.
- **Step 3 understates the work:** the apply-pending marker must ALSO be cleared. Leaving it set is a
  permanent false claim that the table may be missing a durable transaction, and `confirmExactRef`'s
  rule 4 would then answer `Unknown` forever. Nothing was sent, so nothing is owed.
- Beyond the plan, and needed for the soak oracle to read the change at all: `99684c66655` adds
  `CasRefAppendPreAttemptRefused`, because removing the wedge also removed the ONLY evidence these
  refusals happen — an oracle watching the wedge count fall could not tell "the availability fix is
  working" from "nothing happened this run".
- A diagnostic bug fixed on the way, unrelated to the behaviour: the attempts-exhausted path returned
  `Unresolved` through bare returns that bypassed the reason helper, so the single most common wedge
  message in the system read "is UNCERTAIN (not unresolved)".

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

- [x] **Step 1: Write the failing test.** Drive a `NoAttemptSent` rejection (a fence that is already
  false when `commitRefChunk` is entered — `ChunkFaultBackend` is not even needed, the fence hook is
  enough) and assert: the caller gets a retry-later error, `refLaneWedgedForTest(ns)` is FALSE, and a
  subsequent append on the same table succeeds without a remount. Then the contrast case: a genuinely
  ambiguous PUT (`Mode::Unresolved`) must STILL wedge — that assertion already exists as
  `UnresolvedAlwaysRecordsTheWedge`, so extend rather than duplicate it.

- [x] **Step 2: Run — the no-wedge assertion fails** (today the lane wedges in both cases).

- [x] **Step 3: Implement.** In the `Unresolved` arm, skip the wedge install when the reason is
  `NoAttemptSent`: complete the survivors with the same retry-later error (unchanged), leave
  `rt->wedge` disengaged, and leave the allocated txn id as a safe gap (it already is one — ids are
  not required to be contiguous, `CasRefProtocol.cpp` only enforces strict increase). Keep the
  prepared wedge construction where it is: it is allocation-free to discard, and building it before
  the PUT is what makes the ambiguous path safe.

- [x] **Step 4: Prove the safety argument in the comment, not just in the commit message.** The claim
  is "no attempt was sent, therefore the key is unwritten, therefore there is nothing a wedge could
  resolve". It rests on both pre-attempt gates returning before `backend->putIfAbsent` — state that,
  and state the counterexample it excludes (a fence lost AFTER an attempt is `FenceLostMidWay`, which
  keeps wedging).

- [x] **Step 5: TLA.** The wedge is part of the append-lane's at-most-one-unresolved-PUT contract, so
  extend the ref-lane model with a `NoAttemptSent` transition and re-check that
  "every durable object is either applied or wedged" still holds. If the existing model has no
  pre-attempt gate, adding one is the work.

- [x] **Step 6: Run the full CAS gate; Step 7: Commit.**

Subject: `ca: a pre-attempt fence reject no longer wedges the lane (finding #37 defect 3)`

---

## Task 19: a diagnostic tool must not claim ownership of a live pool (CI "Scraping system tables")

**STATUS: SUPERSEDED — this task, as written, is NOT the work that will happen.** What landed:
- The DESIGN analysis only (`c6a6c909be4`), which Step 2 required before any code. It recommends
  contract (a) in a fourth shape (observe-only decided by process ROLE, not by a config flag) and
  rules out (b) on verified evidence.
- The CI symptom fixed by a one-liner instead of a product change: `d99a7df4540` (patch `config.d`,
  not just `config.xml`, and make the next no-op LOUD) and `8aea3a0dedc` (find the CA disks by their
  MARKER instead of naming any path at all).
- The remainder split to BACKLOG {#operator-uuid-recovery} and reframed by the user's Q2 decision
  (BACKLOG {#q2-force-claim}): **the problem is the differing SERVER UUID, not "may a tool read
  without claiming".** A genuine read-only mount is a separate, unimplemented task. The force-claim
  successor is the sibling follow-ups plan's Tasks 10-12, which are themselves BLOCKED on the user's
  choice between overwriting the owner uuid and adopting the pool's existing one.

**Corrections to this task's own text:**
- **Step 1 (local repro) was never done and no longer applies**: the soak stand had been torn down,
  and the question it existed to settle was settled by reading the call site instead — **the CI scrape
  runs against a STOPPED server** (stated in the code's own comment, confirmed at the call site;
  `ebbae78d739`). The preamble below, which says it runs "against the same data directory as the LIVE
  server", is therefore wrong.
- **The "Prior attempt that did NOT fix it" argument is weaker than this task claims.** `ee15c8ade23`
  never actually ran: its `sed` patches `config.xml` while the CA disk is declared in `config.d`. So
  it is not evidence that a read-only scrape is insufficient, and the case it builds against contract
  (c) does not stand on it.
- A prerequisite BACKLOG item cited here is stale and already fixed.
- **One genuine wrong-answer class was found that is mount-independent and needs a fix under ANY
  contract:** `fsck` reads `gc/state` and then streams the source-edge runs, so a snap-prune retiring
  that generation mid-scan surfaces as `CORRUPTED_DATA` on a healthy pool. Same area as BACKLOG
  {#adopted-seal-pruned-run-2026-07-25}, observed from the other side.

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

- [ ] ~~**Step 1: Reproduce locally**~~ — **NOT DONE and no longer applicable** (the stand was torn
  down, and the question was settled by reading the call site: the scrape runs against a STOPPED
  server). ~~start the soak stand, then run the same `clickhouse-local`
  scrape against its data dir and capture the exact failure. Without a local repro this task is
  guesswork.~~
- [ ] **Step 2: Pick the contract** (a)/(b)/(c) and write it into
  `docs/superpowers/cas/` where the disk-lifecycle rules live, with the operator case named.
  **PARTIAL:** the analysis exists (`c6a6c909be4`) and recommends (a) in a fourth shape, but it was
  deliberately "not committed as a decision". The user then reframed the question entirely (Q2), which
  superseded the note as a recommendation while leaving its verified facts standing. **No contract was
  chosen; this step is still open, now under BACKLOG {#operator-uuid-recovery}.**
- [ ] **Step 3: Implement, with a test that a read-only open of a LIVE pool succeeds and takes no
  ownership** — assert the live server's mount object is untouched (same holder, same epoch) after
  the tool exits. **NOT DONE.** Its successor, if the force-claim reading wins, is the follow-ups
  plan's Tasks 10-12; if the read-only reading wins, it is the "separate, unimplemented" read-only
  mount the user named. That choice is unmade.
- [ ] ~~**Step 4: Verify the CI scrape step passes against a running CA server; Step 5: Commit.**~~
  **DONE differently, and the premise was wrong:** the scrape runs against a STOPPED server. Fixed by
  `d99a7df4540` + `8aea3a0dedc` with no product change.

Subject: `ca: read-only pool access for tools — stop the system-table scrape claiming a live mount`

---

## Task 20 (DEFERRED — different scope, not this round): ca-soak scenario S43 — thread-allocation fault injection

**STATUS: DEFERRED by the user** (`6b547a5c220`). Not started, and not part of this round.

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

**This self-review predates Tasks 18-25 and is not a map of the plan as it stands.** Tasks 18-21 were
added during execution, and Tasks 22-25 ({#partb-review-created-work}) came out of the Task 20b review.
Two entries above are also superseded by what execution found: §confirm-primitive gate 0 → Task 11
records a coverage handoff that was never completed (now Task 22), and §testing's integration list →
Task 16 was written around a proof that does not prove anything (see Task 16's corrections block).

---

## Task 20b: CODEX REVIEW OF ALL OF PART B — mandatory gate before the soak (user instruction, 2026-07-25) {#partb-codex-review}

**STATUS: DONE.** Ran gpt-5.6-sol at xhigh over the combined 23-file diff, read as one protocol
(`tmp/partb-review/`, log `tmp/partb-review/codex_review.log`). Verdict: DO NOT MERGE AS-IS — two
blockers, two majors, recorded with evidence in BACKLOG {#partb-review-findings} (`ba45217f307`).

Outcome, in order:
- **Blocker 1 was DOWNGRADED, not fixed** (`006cab3bdf7`, BACKLOG {#partb-review-blocker1-downgraded}).
  The user challenged the framing and was right: `confirmExactRef` reads the same `RefTableRuntime` and
  recovered state that `resolveRef` serves ordinary reads from, so it INHERITS the mount's trust rather
  than creating a new assumption. The reviewer stated a true fact and attributed it to the wrong
  component; the controller had accepted that without checking the boundary. What survives is smaller:
  the confirm gives a stale LOCAL view a REMOTE consequence — amplification, not a new root cause. It
  is the known {#list-as-journal-dataloss-2026-07-25} finding through one more lens.
- **Blocker 2, major 3 and major 4 are FIXED** (`8e6fe6ef0af`, gate 1373/1373 with five new tests each
  seen red first; integration 11/11), and closed in BACKLOG {#partb-review-resolved} (`93eda876ddd`).
- **TWO of the review's four prescribed remedies were WRONG**, both caught by the implementer, and the
  controller had forwarded the first verbatim into the fix instructions. (1) "Queue the exact removal
  (it is idempotent)" is false — `RefTableState::applyOwnerTransition`'s `RemovePrecommit` arm throws
  `CORRUPTED_DATA` on an absent binding, so following it literally would have made every `abandon` of
  an uncertain build fail FOREVER, destructor retries included: a leak turned into a permanent wedge.
  (2) Major 3's stated chain does not hold — an allocation failure raises `MEMORY_LIMIT_EXCEEDED`,
  which is neither `ABORTED` nor `NETWORK_ERROR` and so propagates rather than becoming a fallback, and
  `abandon` appends a PRECOMMIT removal, which cannot undo a committed ref. The defect was real; its
  mechanism was not as described. **Standing lesson: a strong review is evidence, not instruction.**
- **What the review created rather than corrected is now scheduled as Tasks 22-25** below.

**Runs when Tasks 9-16 are all complete, and BEFORE Task 21's soak.** That order is deliberate: a soak is
hours of machine time and its failures are expensive to attribute, so it should run on reviewed code, not
be used as the first reader. If the review lands findings, fix them and only then soak.

**Scope: the whole of Part B as one change, not task by task.** The per-task subagents each saw only their
own task; nobody has yet read tasks 9-16 as a single protocol. That seam is exactly where the defects will
be — an invariant each task preserves locally but the composition does not.

**Prompt discipline, learned from the two codex RCAs run on 2026-07-25** (`tmp/gc-collapse-rca/`,
`tmp/leak-rca/`, both of which produced findings that survived independent verification): give FACTS and
artifacts, state no hypothesis as fact, and say plainly where the author is inferring. Both of those runs
corrected conclusions I had already written down, which is the entire value.

**Must be handed to the reviewer explicitly, because they postdate the spec and the plan:**
- BACKLOG `{#list-as-journal-dataloss-2026-07-25}` — GC's fold cursor advances over the records a round
  OBSERVED, from a paginated `LIST` with no completeness proof; blast radius is live-blob deletion.
- `docs/superpowers/models/CaRelinkConfirmCore.tla`, config `_sab_holeylist` — that finding mechanised:
  with every confirm rule intact and ONE incomplete page permitted, `ConfirmedRelinkNeverDangles` breaks.
  So `_main` passing means "the confirm protocol adds no new dangle path", NOT "a confirmed relink cannot
  dangle", and the reviewer must not be allowed to read it as the latter.
- The `No`-vs-`Unknown` ordering carried from Task 10 into Task 11: rule 6 (fence) is evaluated LAST, so a
  fence-lost mount answers `No`. Nothing may treat `No` as authoritative knowledge without hoisting rule 6.
  Ask the reviewer specifically whether anything in Tasks 13-16 broke that.

**Ask for, at minimum:** whether the composition preserves the spec's invariants; whether any path can
produce a `Yes` from a stale or partially-recovered view; whether the upstream footprint stayed within the
authorization ({#upstream-authorization}); what the receiver does on each failure-taxonomy branch and
whether any branch can lose or double-promote a part; and what is NOT covered by the tests that exist.

---

## Task 21: Part B soak gate — 20-minute shakeout first, then 4 hours

**STATUS: NOT STARTED.** Its prerequisites (Tasks 9-16, then 20b and its fixes) are all complete. Note
that the signals Step 2 collects have grown since this task was written: the sibling follow-ups plan
adds `CasGcRefScanDisagreements` and `CasGcUnappliedFoldedTxns` (`e01b5cd82be`), and both belong in the
collector's `IN (...)` list. Whether Tasks 22-25 run before this soak is NOT decided in any source —
decide it deliberately rather than by execution order.

**Why this task exists.** Task 16's pytest battery proves the confirm protocol FUNCTIONS. It does not
prove it survives hours of concurrent load with chaos, and Part B rewrites the `tmp-fetch` lifecycle
where every one of the 56 leaked blobs originated (BACKLOG `{#unmatched-minus-one-fetch-window}`). Task 18
also changed lease-loss behaviour, which is precisely what chaos generates, and it landed AFTER the last
green soak.

**Order is deliberate (user decision, 2026-07-25): a 20-minute run FIRST, to tune the harness, verify every
signal is actually observed, and fix the bugs that shakes out. Only once the 20-minute run is stable does
the 4-hour run happen.** Do not skip to the long run: three separate harness surfaces were found silently
under-reporting the product this week, and a 4-hour run that reports nothing useful costs a day.

**Files:**
- Modify: `utils/ca-soak/soak/run.py` (checkpoint asserts; metrics)
- Modify: `utils/ca-soak/soak/cluster.py` (ProfileEvents collection — see step 2)
- Test: `utils/ca-soak/tests/` (unit tests for whatever pure logic you add)

- [ ] **Step 1: Add `stale_edge` to the checkpoint's hard asserts.**

The checkpoint already raises on `dangling != 0` at `run.py:642` and already has a detail fsck available at
`run.py:606`. `stale_edge` is DETAIL-MODE ONLY (see `CasFsck.h`), so assert it off that detail read, not
off the cheap summary at `:596`. A blob whose every source edge names a manifest that no longer exists can
never be reclaimed by the incremental GC, so a nonzero count is a hard failure, not debris.

Fail CLOSED on absence: if the detail fsck result has no `stale_edge` key at all, that means the binary
predates the class — raise `CheckpointFailure` naming that, never treat a missing key as zero.

- [ ] **Step 2: Teach the driver to read ProfileEvents at all.**

`grep -n "ProfileEvents" utils/ca-soak/soak/*.py` returns NOTHING today — the soak driver cannot observe a
single counter. That is why the three signals added on 2026-07-25 would be invisible to it. Add a
collector to `cluster.py` alongside the existing metric probes:

```sql
SELECT event, value FROM system.events WHERE event IN (
    'CasGcUnmatchedRemoveDeltas', 'CasRefAppendPreAttemptRefused', 'CasRefAppendWedged',
    'CasGcCondemnMarkerUnconfirmedCarry', 'CasGcClampSuppressedPasses')
```

Record them per metrics tick and in every checkpoint row. Follow `soak/fsck.py`'s error discipline as
FIXED on 2026-07-25 (`observe.py`'s `_is_benign_probe_gap`): a node that is down during a chaos window is
legitimately unreadable; a query that FAILS is not, and must surface rather than degrade to zero.

- [ ] **Step 3: Report the counters; do NOT fail on them yet.**

Their benign rates are uncharacterised. `CasGcUnmatchedRemoveDeltas` should be zero in a healthy pool but
may be nonzero for reasons we have not enumerated; `CasRefAppendPreAttemptRefused` is EXPECTED to be
nonzero under chaos — that is the availability fix working. Characterising them is the point of the
20-minute runs. Record a threshold only once several runs agree.

- [ ] **Step 4: The 20-minute shakeout, repeated until stable.**

```bash
cd utils/ca-soak && docker compose up -d && python3 -m soak.run --seed 1 --phase 3 --duration 20m \
    --insert-mode sync --max-pool-gb 12 > tmp/unattended/soak_partb_20m_<n>.log 2>&1
```

(Phase 3 with `--duration` is the real timed soak; phase 1 `--ops` finishes ~10× faster and does NOT
exercise the stage plan — see [[reference_ca_soak_duration_phase3]].)

EXPECT the first runs to fail on harness problems rather than product problems, and fix those. The run is
"stable" when two consecutive runs are green AND every signal above was actually OBSERVED at least once —
a green run in which a counter was never read is not stable, it is blind. Record each attempt in
`RUN_HISTORY.md` with what broke and what was fixed.

- [ ] **Step 5: The 4-hour run, with chaos.**

Same invocation with `--duration 4h`. Gate: `PHASE3 OK`, `dangling == 0` at every checkpoint,
`stale_edge == 0` at every checkpoint, zero `WORKLOAD FAILURE`, and the counters recorded.

- [ ] **Step 6: Commit** the harness changes, the run history, and a short results note.

---

# Tasks 22-25: work the Part B codex review CREATED (added 2026-07-26) {#partb-review-created-work}

Task 20b's review produced two kinds of output. The first — findings against code that existed — is
closed (`8e6fe6ef0af`, BACKLOG {#partb-review-resolved}). The second is work that existed nowhere as a
task, and prose in a findings section is not a task. These four are that work, sized and placed.

**Ordering relative to Task 21 (the soak) is NOT decided in any source.** Decide it deliberately. The
argument for going first is Task 20b's own: a soak is hours of machine time whose failures are expensive
to attribute, and Task 22 and Task 24 are about the difference between coverage and the appearance of
coverage — which is the failure mode this whole round kept hitting.

**Already covered — do NOT re-create these as tasks.** The review's missing-test list is partly obsolete
because `8e6fe6ef0af` closed part of it while fixing the defects:
- `LandedThenLost` during the receiver's `precommitAdd` → `CasRefInstallSafety.UncertainPrecommitKeepsItsCleanupOwnerAndItsBody`
  and `…UncertainPrecommitThatNeverLandedStillAbandonsCleanly`.
- `LandedThenLost` during promotion → `CasPartFolderAccess.AnUnresolvedPromoteIsNotReportedAsDefinitelyNotCommitted`.
- An allocation failure after a durable promote but before the handle is terminal →
  `CasPartFolderAccess.APostCommitFailureLeavesTheHandleTerminal`.
- Move assignment with a failing abort → **VOID, not missing.** Move assignment was deleted rather than
  fixed, and its absence is pinned by `CasPartFolderAccess.PreparedPartWriteIsNotMoveAssignable`.

---

## Task 22: gate 0 has no test at all — complete the handoff the gtest declares

`gtest_cas_confirm_exact_ref.cpp:689` says, in its own comment, that gate 0's cases "belong to the
Task 16 pytest battery": `Deleting`, absent, other-disk, the `Deleting → Outdated` rollback state, and
the `MOVE ... TO DISK` same-name-other-disk case. **None of Task 16's eleven integration tests covers
any of them.** A handoff that is declared and never completed reads exactly like coverage — which is
worse than an acknowledged gap, because nothing points at it.

Gate 0 is the part filter in `Service::resolveContentAddressedConfirm` (Task 11 Step 3): the part must
be `Active`/`Outdated` **on the matched instance's disk**. It is the first thing a confirm consults, so
a wrong answer here is a wrong answer for the whole protocol.

**Files:**
- Modify: `tests/integration/test_cas_replicated_relink/test.py`

- [ ] **Step 1: Read Task 11 Step 3's implementation and Task 16's corrections block** before writing
  anything. The relink-proof rule from Task 16 applies unchanged: key every assertion on the receiver's
  `Relink of part <p> … finished (no bytes transferred).` line, never on a flat blob count.
- [ ] **Step 2: `Deleting` → the confirm must not authorize.** Drive a source-side part into `Deleting`
  inside the confirm window (the `cas_relink_receiver_pause_before_confirm` failpoint registered by
  Task 16 is the existing seam) and assert the receiver does NOT promote.
- [ ] **Step 3: absent, and other-disk.** A part name the source does not have, and a part the source
  has on a DIFFERENT disk from the matched instance.
- [ ] **Step 4: the `Deleting → Outdated` rollback state.** Task 11 Step 1 states the requirement
  precisely: this state must still be rejected **by gate 1** when the ref is gone. Assert that, not
  merely that the fetch failed — a test that only observes a failure cannot tell which gate produced it.
- [ ] **Step 5: `MOVE ... TO DISK` same-name-other-disk** — the case gate 0's "on the matched
  instance's disk" clause exists for.
- [ ] **Step 6: Run and commit.** `python3 -m ci.praktika run "integration" --test test_cas_replicated_relink`

Subject: `ca: integration coverage for the relink confirm's gate-0 part filter`

---

## Task 23: the review's missing tests that are still genuinely missing

Only the uncovered subset — the covered ones are listed under {#partb-review-created-work} above and
must not be re-created. Each item below states what it proves, because "add a test for X" is how a test
that proves nothing gets written.

**Files:**
- Modify: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`,
  `src/Disks/tests/gtest_cas_part_folder_access.cpp`,
  `tests/integration/test_cas_replicated_relink/test.py`

- [ ] **Step 1: a recovery that omitted one `LIST` record, then `confirmExactRef`.** The downgraded
  blocker 1 ({#partb-review-blocker1-downgraded}) says the confirm INHERITS the mount's trust rather
  than creating a new assumption — that is an argument, and this is the test that makes it checkable.
  Recovery publishes `recovered = true` from a paginated listing with no completeness proof; assert what
  the confirm actually answers when that listing was short by one durable removal. **Whatever it
  answers, record it as the pinned behaviour** — this test's job is to make the amplification property
  visible, not to assert a fix that was deliberately not made. The sibling follow-ups plan's
  `HoleyListBackend` (`src/Disks/tests/gtest_cas_holey_list_detector.cpp`, `e01b5cd82be`) is the
  injection tool; do not write a second one.
- [ ] **Step 2: `LandedThenLost` during the ABORT's own removal append.** The precommitAdd and promotion
  halves are covered; the abort half is not. It matters because `abandon` is deliberately retryable
  after an append failure and the destructor retries it — so an ambiguous removal append must not strand
  the cleanup owner or double-remove.
- [ ] **Step 3: zero and multiple routing matches.** Task 11 Step 3 requires "zero or more than one
  match → `Unknown`". `OwnsNamespaceSelectsTheMountByServerRootInEveryLifecycleState` covers
  `ownsNamespace` itself, not the `Service`-side resolution's zero/multiple branch. Two CA disks whose
  instances both claim the namespace is the case that must answer `Unknown`, not pick one.
- [ ] **Step 4: source remount (or recovery) between the offer and the confirm.**
  `CasConfirmExactRef.LostMountFenceIsUnknown` covers a fence-lost mount at the ledger level; the
  end-to-end offer → remount → confirm sequence is not covered anywhere.
- [ ] **Step 5: confirm transport failure and an ABSENT answer at HTTP level.** The wire is binary — only
  `yes` authorizes, and an absent cookie must read as `unproven` (Task 13's corrections). That is a
  fail-closed claim about a peer's behaviour and nothing currently exercises it over HTTP.
- [ ] **Step 6: Run the gate + the integration battery; commit.**

Subject: `ca: close the Part B review's remaining test gaps`

---

## Task 24: `test_replicated_fetch_by_relink` is green for the wrong reason

`tests/integration/test_cas_replicated_relink/test.py:255` still proves a relink ONLY by a flat
`count_blobs()` delta. That is exactly the defect Task 16 found in this plan's own text and fixed for
the seven tests it wrote — and it left the older test standing. **A byte fetch onto a
content-addressed disk writes the same content, deduplicates, and produces the same flat count**, so
this test passes whether or not relink ran. It is the oldest relink test in the file, which makes it the
one most likely to be trusted.

**Files:**
- Modify: `tests/integration/test_cas_replicated_relink/test.py`

- [ ] **Step 1: Re-key the assertion** on the receiver's `Relink of part <p> … finished (no bytes
  transferred).` log line, reachable only after a confirm `yes` and a `Committed` promote. Keep the blob
  count as corroboration, converted to a KEY-SET delta — the fixture runs `gc_interval_sec = 1`, so a
  total count can fall mid-test (Task 16's corrections).
- [ ] **Step 2: Prove the new assertion can FAIL.** Force the byte path once (the existing
  `cas_relink_receiver_force_mechanism_failure` failpoint) and confirm the test goes red. Without this
  the fix is unverified in exactly the way the original was.
- [ ] **Step 3: Sweep the file for any other flat-count proof** and re-key it the same way; state
  explicitly if there are none left.
- [ ] **Step 4: Run and commit.**

Subject: `ca: the oldest relink test proved nothing — re-key it on the relink log line`

---

## Task 25: close the residual post-commit window (`eraseView`, `publishStaging::out_slot`)

Flagged-not-fixed by the Part B review fix and recorded at BACKLOG {#partb-review-resolved}. Part A's
rule is "nothing may throw after a durable commit before the commit is recorded", and `8e6fe6ef0af`
applied it one frame in. Two sites still sit OUTSIDE that frame:
- `eraseView` runs after the durable commit and can throw;
- `ContentAddressedTransaction::publishStaging`'s `out_slot` is assigned only after `promoteBuild`
  returns, so a throw between the two leaves the slot empty over a committed ref.

Small and pre-existing, and **unnamed by the review** — it was found by the implementer while fixing
major 3. Closing it means extending the same no-throw-after-commit discipline one frame outward.

**Files:**
- Modify: `.../Parts/PartFolderAccess.cpp`, `.../ContentAddressedTransaction.cpp`
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

- [ ] **Step 1: Read `APostCommitFailureLeavesTheHandleTerminal`** — it is the shape this task extends,
  and its probe is placed just past the window that was closed. The new probe goes past THIS one.
- [ ] **Step 2: Write the failing tests** — a throw at `eraseView` after a durable commit, and a throw
  between `promoteBuild` returning and `out_slot` being assigned. Both must leave the handle terminal
  and the commit recorded; neither may report the promote as not-committed.
- [ ] **Step 3: Implement** by moving the fallible work before the commit, or the recording before the
  fallible work — whichever keeps the allocation-free region genuinely allocation-free. Do NOT widen
  `DENY_ALLOCATIONS_IN_SCOPE` over an operation that can legitimately allocate.
- [ ] **Step 4: Run the gate; Step 5: Commit.**

Subject: `ca: extend the no-throw-after-commit discipline over eraseView and publishStaging`

---
