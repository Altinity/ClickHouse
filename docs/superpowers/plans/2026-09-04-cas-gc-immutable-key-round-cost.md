# CAS GC round cost on write-once keys — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the per-object request loops of a CAS GC round on keys that are write-once by construction: one mount-floor read per namespace per sweep page, manifest bodies read only for candidates and through a read-ahead, a write-once bulk-delete verb for owner-removed manifests, and ref-object cleanup in revalidated cohorts.

**Architecture:** Four ordered changes, each landable alone. A and B are internal to the orphan-manifest sweep (`Gc/CasOrphanManifestSweep.cpp`) plus one injected reader in the shared recovery walk. C adds one verb through three layers (object storage, `Backend`, `CasOperation`) and consumes it in the `manifest_deletes` phase. D reuses the verb in `cleanupRefObjects` with one authority revalidation per chunk. Blob deletes are untouched everywhere.

**Tech Stack:** C++23 (ClickHouse tree), gtest (`src/Disks/tests/gtest_cas_*.cpp`, binary `unit_tests_dbms`), the CAS in-memory and counting backends for tests, praktika integration tests on RustFS.

**Spec:** `docs/superpowers/specs/2026-09-04-cas-gc-immutable-key-round-cost-design.md` (rev.3). Read it first; every task below cites the section it implements.

## Global Constraints

- **Worktree and branch.** All work happens in the worktree `/home/mfilimonov/workspace/ClickHouse/master` on branch `cas-gc-write-once-keys` (created from `cas-gc-rebuild` at `b7f2b3f38b4`; the `lane-g` worktree is taken by another implementer). The build dir is `/home/mfilimonov/workspace/ClickHouse/master/build` (release, no sanitizer; `unit_tests_dbms` present). Never commit on `cas-gc-rebuild` or `master`. Never push. At the end the branch merges into `cas-gc-rebuild`.
- **Build.** `cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_<task>.log 2>&1; echo NINJA_EXIT=$?`. No `-j`, no `nproc`. Dispatch a subagent to read the log and return a concise summary; never paste the log. A new test file is picked up by the `CONFIGURE_DEPENDS` glob; if `ninja` does not compile it, run `cmake .` in the build dir once.
- **Gate.** `./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_<task>_gate.log 2>&1; echo GTEST_EXIT=$?` from the build dir, plus the task's own suite filter. Suite names MUST start with `CAS`; never widen the filter.
- **`LOGICAL_ERROR` sites.** Never `EXPECT_THROW` them. A test of one goes in a `CAS...DeathTest` suite under `#if defined(DEBUG_OR_SANITIZER_BUILD)` with `EXPECT_DEATH`, the pattern in `src/Disks/tests/gtest_cas_blob_upload_pool.cpp:62-68`.
- **Git.** Stage and commit by explicit path only: `git add -- <paths>` then `git commit -- <paths>`; never `git add -A`; check `git diff --cached --stat` shows only your files; check `git branch --show-current` prints `cas-gc-write-once-keys` after every commit. Commit messages end with:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV
  ```
- **Code style.** Allman braces. No `sleep` to fix a race. Comments keep the reason, never a plan or BACKLOG reference. In prose write `f` not `f()`. Documentation headers carry `{#anchor}`.
- **Spec placements this plan adjusts** (Task 12 records them in the spec): `WriteOnceKey` lives in `Primitives/CasWriteOnceKey.h` so `Backend` does not include the layout; the reader interface lives in `Pool/CasKeyReader.h` because the recovery walk is in `Pool/`, and only the read-ahead-backed implementation lives in `Gc/`; a pool setting `gc_bulk_delete_chunk_keys` (1 to 1000, default 1000) sizes the chunks of both consumers so tests exercise chunk boundaries with small cohorts.
- **Paths below** are relative to the worktree root unless they start with `/`. `CA/` abbreviates `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.
- **`PoolConfig` in tests.** Designated initializers must follow the struct's declaration order (`CA/Pool/CasPool.h`). Where a test below lists fields in an order the struct does not have, assign them one by one on a named `PoolConfig config;` as `makeReadyFixture` does in `src/Disks/tests/gtest_cas_orphan_nomination.cpp:80-90`.
- **Helper signatures.** Where a step says "read the exact signature", do it before writing the call: `src/Disks/tests/cas_test_helpers.h` is the source of truth for every `…ForTest`, `publishAt`, `writeSealAt`, `dropRefTransition`, `catalogLifeIdForTest` call in this plan.

---

### Task 0: Branch in lane-g and the BACKLOG correction

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG.md:62-96` (the entry `[gc-manifests-are-immutable-so-reduce-and-deletes-can-be-cheap]`) and the entry `[gc-multidelete-conditional-gap]` (`:979`)

**Interfaces:** none.

- [ ] **Step 1: Verify the branch**

The `lane-g` worktree is occupied by another implementer, so this plan runs in the `master` worktree (a checkout of the shared repo, NOT the `master` branch). The branch `cas-gc-write-once-keys` was created there from `cas-gc-rebuild` at `b7f2b3f38b4`. The tree carries pre-existing modified files (`utils/ca-soak/...`, `docs/superpowers/cas/BACKLOG/gcs.md`, contrib submodule pointers) that belong to other work: never stage them; every commit names its paths.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git branch --show-current     # cas-gc-write-once-keys
git log --oneline -1
```

- [ ] **Step 2: Rewrite the BACKLOG entry**

Replace the whole entry body under the header at `docs/superpowers/cas/BACKLOG.md:62` (keep the header line and its anchor) with:

```markdown
Measured on the 15-minute real-GCS soak (leader `ca-live-gcs-ch1-1`, `system.cas_gc_log`, phase rows
joined to `Finish` rows through `round_id`): rounds 1.2 s → 18.8 s → 128 s → 584 s → longer. Round 3 =
`fold_reduce` 380 s (3840 GETs: 2870 `CASGCGet`, 814 `CASRootGet`, 100 `CASManifestGet`; 1966 read
errors), `fold_ref_intake` 46 s, `manifest_deletes` 37 s (188 conditional deletes). Round 4 =
`fold_reduce` 300 s (3002 `CASGCGet`, 2006 errors), `ref_object_cleanup` 204 s (512 keys × HEAD + catalog
GET + gc/state GET + conditional DELETE). Round 5 = `fold_reduce` 587 s (3400 GET + 5383 HEAD, all HEADs
inline: `CASGCReadAheadMiss` 5382, hits 0), `manifest_deletes` 617 s (3250 conditional deletes at ~190 ms).
`orphan_sweep` nominated nothing in any round.

The reduce's `CASGCGet` volume is NOT manifest bodies (those are capped at 100 by
`manifest_sweep_delete_budget_keys`): it is `floorForNamespace` in `prefixEligibleOn`
(`Gc/CasOrphanManifestSweep.cpp:481`), called once per listed manifest because the eligibility memo is
keyed by build prefix and every `INSERT` is its own build; each call reads the mount key of every
`/`-prefix of the namespace (`…/store/465` 404, `…/store` 404, `<root>` hit). 956 × 3 = 2868 ≈ 2870
reads; 956 × 2 + 54 reissues = 1966 errors. Rounds 4 and 5: 1000 × 3 + 2 = 3002, 1000 × 2 + 6 = 2006.

Why the per-key conditional deletes are not needed (user, 2026-09-04): manifest keys, ref `_log` and
`_snap` keys are write-once by construction (`op.create`, epoch/sequence/ordinal monotone, life-qualified;
global manifest-key uniqueness rests on decommission being irreversible). Blobs are NOT write-once and
keep the exact-token delete. GCS accepts batch `DeleteObjects` (the live gate proves it; an earlier note
here claiming the XML API lacks it was wrong). Retirements cannot be taken from the source-edge runs:
`sourceEdgeId` (`Gc/CasBlobInDegree.cpp:164`) is an irreversible hash, so a nominated orphan's body is
still read, bounded by the candidate budget.

Design, approved rev.3: `docs/superpowers/specs/2026-09-04-cas-gc-immutable-key-round-cost-design.md`
(A: one floor per namespace per page; B: late manifest reads and read-ahead in the sweep; C:
`removeManyWriteOnce` with `manifest_deletes` as consumer; D: ref-cleanup cohorts). Plan:
`docs/superpowers/plans/2026-09-04-cas-gc-immutable-key-round-cost.md`. Measured rows replace the
predictions here as each step lands.

**Investigation item, not part of the design:** round 5's 5382 inline HEADs. `CASGCReadAheadWasted` is
128 on the round's `Finish` row (two windows of 64 at concurrency 16), `epoch_crossings` is 2 on the
intake row, round 6 without a crossing hinted 76 of 76. Hypothesis: the intake hints ref-log ids past an
epoch seal, they are never taken, they stay in `pending`, and `topUpHeadHints` never finds room. Needs a
local two-epoch cliff run to reproduce; the fix shape is the epoch-crossing discard rule the sweep's
read-ahead gets in the design above, applied at `Gc/CasGc.cpp:2512`.
```

In `[gc-multidelete-conditional-gap]` (`:979`), append one paragraph:

```markdown
**Closed by construction for the three write-once families** (manifest bodies, ref `_log`, ref `_snap`):
see `[gc-manifests-are-immutable-so-reduce-and-deletes-can-be-cheap]` and the design it points to. The
gap remains exactly as stated for blobs.
```

- [ ] **Step 3: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- docs/superpowers/cas/BACKLOG.md
git diff --cached --stat
git commit -m "ca-docs: BACKLOG — GC round cost entry corrected (floor probes, round attribution, GCS batch delete, no reverse edge index) and the round-5 head read-ahead investigation item

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- docs/superpowers/cas/BACKLOG.md
git branch --show-current
```

---

### Task 1: A — one mount floor per namespace per page

Implements spec §A.

**Files:**
- Modify: `CA/Gc/CasOrphanManifestSweep.h` (declare `prefixEligibleUnder`; add `floor_lookups`, `floor_reads` to `ManifestSweepResult`)
- Modify: `CA/Gc/CasOrphanManifestSweep.cpp:45-70` (`floorForNamespace` reports reads), `:481-503` (`prefixEligibleOn` split), `:711-724` (page memo)
- Modify: `CA/Gc/CasGc.cpp:1178-1185` (two new phase metrics)
- Create: `src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp`

**Interfaces:**
- Produces: `bool prefixEligibleUnder(const std::optional<MountLease> & floor, const BuildPrefix & prefix);` (public, pure); `ManifestSweepResult::floor_lookups`, `ManifestSweepResult::floor_reads` (`uint64_t`).

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <limits>

/// The orphan-manifest sweep's request shape per page. The floor a namespace's builds are judged
/// against is one mount body per server root, so a page reads it once per namespace, not once per
/// listed build; the tests below count the mount-key reads and pin the pure eligibility predicate.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// A three-segment namespace, the shape a real table gets (`<root>/store/<3hex>/<uuid>@cas@`), so the
/// floor lookup has three `/`-prefixes to try and two of them miss.
const RootNamespace kNs{"test/store/465/aa@cas@"};

ManifestRef build(uint64_t seq)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = 1};
}

struct PageFixture
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    PoolPtr store;
    uint64_t manifests = 0;

    explicit PageFixture(uint64_t manifests_, uint64_t min_active_build_sequence)
        : manifests(manifests_)
    {
        PoolConfig config;
        config.pool_prefix = "p";
        config.server_root_id = "test";
        config.manifest_sweep_list_budget_keys = 1000;
        config.manifest_sweep_delete_budget_keys = 100;
        config.gc_fold_max_defer_rounds = 0;
        store = Pool::open(backend, config);
        const Layout & layout = store->layout();
        casAdmitEntry(*backend, layout, kNs);
        /// One committed birth log at {1,1} and a checkpoint naming it, so the namespace has a protection
        /// view and every eligible key reaches the premise (which retains it for lack of fold coverage).
        publishAt(*backend, layout, kNs, RefTxnId{1, 1}, "live", /*build_sequence=*/manifests + 1,
                  DB::UInt128(0x7001), /*birth=*/true);
        writeRecoverableCkptForRawFixture(*backend, layout, kNs, RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 1},
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        });
        for (uint64_t seq = 1; seq <= manifests; ++seq)
            writeManifestRaw(*backend, layout, kNs, build(seq), {blobEntryFor("a", DB::UInt128(0x100 + seq))});
        setWatermarkMinActive(*backend, layout, "test", /*writer_epoch=*/1, min_active_build_sequence);
        backend->resetCounts();
    }

    ManifestSweepResult page()
    {
        return planManifestCursorPage(*store, "", /*list_budget=*/1000, /*nomination_budget=*/100,
                                      /*catalog_recovery_authoritative=*/true, nullptr);
    }
};

}

TEST(CASOrphanSweepRequests, FloorIsReadOncePerNamespacePerPage)
{
    PageFixture f(/*manifests=*/50, /*min_active=*/25);
    const Layout & layout = f.store->layout();
    const ManifestSweepResult result = f.page();

    EXPECT_EQ(result.listed, 50u);
    EXPECT_EQ(result.floor_lookups, 1u);
    EXPECT_EQ(result.floor_reads, 3u);
    EXPECT_EQ(f.backend->getCount(layout.mountKey("test/store/465")), 1u);
    EXPECT_EQ(f.backend->getCount(layout.mountKey("test/store")), 1u);
    EXPECT_EQ(f.backend->getCount(layout.mountKey("test")), 1u);
    /// Builds 1..24 are retired under the floor and reach the premise, which retains them for lack of
    /// coverage; builds 25..50 are active and never get that far.
    EXPECT_EQ(result.retained_no_coverage, 24u);
    EXPECT_TRUE(result.nominations.empty());
}

TEST(CASOrphanSweepRequests, AbsentFloorRetainsEverythingWithOneLookup)
{
    PageFixture f(/*manifests=*/10, /*min_active=*/100);
    const Layout & layout = f.store->layout();
    {
        OperationForTest op(*f.backend);
        const auto h = (*op).head(layout.mountKey("test"), Retry::once());
        ASSERT_TRUE(h.has_value());
        ASSERT_EQ((*op).remove(layout.mountKey("test"), h->etag, Retry::once()), Removal::Removed);
    }
    f.backend->resetCounts();
    const ManifestSweepResult result = f.page();

    EXPECT_EQ(result.floor_lookups, 1u);
    EXPECT_EQ(result.floor_reads, 3u);
    EXPECT_EQ(result.listed, 10u);
    EXPECT_EQ(result.skipped, 10u);
    EXPECT_EQ(result.retained_no_coverage, 0u) << "an absent floor admits nothing, so no key reaches the premise";
}

TEST(CASOrphanSweepRequests, PrefixEligibleUnderIsTheFourComparisons)
{
    MountLease floor;
    floor.writer_epoch = 3;
    floor.min_active_build_sequence = 10;
    EXPECT_TRUE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 2, .build_sequence = 999}));
    EXPECT_FALSE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 4, .build_sequence = 1}));
    EXPECT_TRUE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 3, .build_sequence = 9}));
    EXPECT_FALSE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 3, .build_sequence = 10}));
    floor.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 3, .build_sequence = 10}));
    EXPECT_FALSE(prefixEligibleUnder(std::nullopt, BuildPrefix{.writer_epoch = 1, .build_sequence = 1}));
}

namespace
{

/// Deletes the mount key the first time it is read, so the page decides with a floor whose object is
/// gone by the time it decides. Retirement is permanent, so the decisions must be the ones the floor
/// admitted when read, and no active build may be nominated.
class MountVanishesBackend final : public CountingBackend
{
public:
    using CountingBackend::read;
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto got = CountingBackend::read(key, access);
        if (got && key == mount_key && !fired)
        {
            fired = true;
            static_cast<void>(InMemoryBackend::remove(key, got->value, access));
        }
        return got;
    }
    String mount_key;
    bool fired = false;
};

}

TEST(CASOrphanSweepRequests, MountVanishingMidPageKeepsTheDecisionsOfTheFloorAsRead)
{
    auto backend = std::make_shared<MountVanishesBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .manifest_sweep_list_budget_keys = 1000,
                                                .manifest_sweep_delete_budget_keys = 100,
                                                .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    casAdmitEntry(*backend, layout, kNs);
    publishAt(*backend, layout, kNs, RefTxnId{1, 1}, "live", /*build_sequence=*/51, DB::UInt128(0x7001), /*birth=*/true);
    writeRecoverableCkptForRawFixture(*backend, layout, kNs, RefCkpt{
        .life_epoch = 1, .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt});
    for (uint64_t seq = 1; seq <= 50; ++seq)
        writeManifestRaw(*backend, layout, kNs, build(seq), {blobEntryFor("a", DB::UInt128(0x100 + seq))});
    setWatermarkMinActive(*backend, layout, "test", 1, /*min_active=*/25);
    backend->mount_key = layout.mountKey("test");

    const ManifestSweepResult result = planManifestCursorPage(*store, "", 1000, 100, true, nullptr);
    EXPECT_TRUE(backend->fired);
    EXPECT_EQ(result.floor_lookups, 1u);
    EXPECT_EQ(result.retained_no_coverage, 24u) << "the 24 retired builds were decided under the floor as read";
    EXPECT_TRUE(result.nominations.empty());
    OperationForTest op(*backend);
    EXPECT_FALSE((*op).head(layout.mountKey("test"), Retry::once()).has_value());
}
```

- [ ] **Step 2: Build and run to verify they fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t1a.log 2>&1; echo NINJA_EXIT=$?
```
Expected: compile error, `prefixEligibleUnder` and `floor_lookups` undeclared.

- [ ] **Step 3: Implement**

In `CA/Gc/CasOrphanManifestSweep.h`, in `ManifestSweepResult` after `retained_work_budget`:

```cpp
    /// The floor a namespace's builds are judged against is one mount body per server root. A page
    /// resolves it once per namespace (`floor_lookups`); each lookup reads the mount key of every
    /// `/`-prefix of the namespace until one answers (`floor_reads`), so an absent mount costs the
    /// whole chain once.
    uint64_t floor_lookups = 0;
    uint64_t floor_reads = 0;
```

Next to the `prefixEligible` declaration (`:188`):

```cpp
/// The pure half of `prefixEligible`: whether `prefix` is retired under one observation of the mount
/// floor. `nullopt` (no mount body under any prefix of the namespace) admits nothing. Retirement is
/// permanent -- the epoch and the acknowledgement floor only grow and the farewell is terminal -- so
/// an admission derived from any observation stays true afterwards, which is what lets a page judge
/// every build of a namespace against one read.
bool prefixEligibleUnder(const std::optional<MountLease> & floor, const BuildPrefix & prefix);
```
Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>` if `MountLease` is not already visible there.

In `CA/Gc/CasOrphanManifestSweep.cpp`, change `floorForNamespace` (`:45`) to count reads:

```cpp
std::optional<MountLease> floorForNamespace(CasOperation & op, const Layout & layout, const RootNamespace & ns,
                                            uint64_t * reads = nullptr)
{
    const String & value = ns.string();
    size_t pos = value.size();
    while (true)
    {
        pos = value.rfind('/', pos == 0 ? 0 : pos - 1);
        if (pos == String::npos)
            break;

        const String server_root_id = value.substr(0, pos);
        if (!server_root_id.empty())
        {
            if (reads)
                ++*reads;
            if (const auto got = op.read(layout.mountKey(server_root_id), Retry::standard()))
                return decodeMountLease(got->bytes);
        }
        if (pos == 0)
            break;
    }
    return std::nullopt;
}
```

Replace `prefixEligibleOn` (`:481-496`) with the split:

```cpp
bool prefixEligibleOn(CasOperation & op, const Layout & layout, const RootNamespace & ns, const BuildPrefix & prefix)
{
    return prefixEligibleUnder(floorForNamespace(op, layout, ns), prefix);
}

}

bool prefixEligibleUnder(const std::optional<MountLease> & floor, const BuildPrefix & prefix)
{
    if (!floor)
        return false;

    const MountLease & w = *floor;
    if (prefix.writer_epoch < w.writer_epoch)
        return true;
    if (prefix.writer_epoch > w.writer_epoch)
        return false;
    if (w.min_active_build_sequence == std::numeric_limits<uint64_t>::max())
        return true;   /// farewell/retired sentinel: every seq is retired
    return w.min_active_build_sequence > prefix.build_sequence;
}
```
(the closing `}` of the anonymous namespace moves above the new public function; `prefixEligible(Pool &, …)` stays as it is).

In `planManifestCursorPage`, replace `std::map<String, bool> eligible_by_prefix;` with

```cpp
    /// One floor observation per namespace for the whole page. Every build of a namespace is judged
    /// against the same mount body; reading it per build prefix would cost three requests per listed
    /// key on a pool where every INSERT is its own build.
    std::map<String, std::optional<MountLease>> floor_by_ns;
```
and replace the block at `:711-724` (`if (catalog_entry) { const String eligibility_key … }`) with

```cpp
        if (catalog_entry)
        {
            auto [floor_it, floor_inserted] = floor_by_ns.emplace(parsed->ns.string(), std::nullopt);
            if (floor_inserted)
            {
                ++result.floor_lookups;
                floor_it->second = floorForNamespace(op, layout, parsed->ns, &result.floor_reads);
            }
            if (!prefixEligibleUnder(floor_it->second, parsed->prefix))
            {
                ++result.skipped;
                decided_through = listed.key;
                continue;
            }
        }
```

In `CA/Gc/CasGc.cpp` after `t.metric("listed", sweep.listed);` (`:1181`):

```cpp
        t.metric("floor_lookups", sweep.floor_lookups);
        t.metric("floor_reads", sweep.floor_reads);
```

- [ ] **Step 4: Build, run the new suite, run the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t1b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASOrphanSweepRequests.*' > test_wok_t1_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t1_gate.log 2>&1; echo GTEST_EXIT=$?
```
Expected: both exit 0. If `FloorIsReadOncePerNamespacePerPage` reports `retained_no_coverage` other than 24, read the page's `LOG_DEBUG` lines in the test output: the fixture's checkpoint or birth log is not shaped as the sweep expects, and the fix is in the fixture, not in the sweep.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp
git diff --cached --stat
git commit -m "ca-gc: orphan sweep reads a namespace's mount floor once per page

Eligibility was memoized per build prefix and every INSERT is its own build, so a page of 1000
manifests read the mount key ~3000 times (two guaranteed 404s per namespace prefix chain). The
floor is one body per server root and retirement is permanent, so one observation per namespace
decides every build of the page. New phase metrics floor_lookups and floor_reads.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp
git branch --show-current
```

---

### Task 2: B1 — decide from the key, read the body last

Implements spec §B1.

**Files:**
- Modify: `CA/Gc/CasOrphanManifestSweep.cpp:611-960` (`planManifestCursorPage`)
- Modify: `CA/Gc/CasOrphanManifestSweep.h:190-208` (doc comment of the page function)
- Modify: `CA/ContentAddressedSettings.cpp:64` (setting description)
- Modify: `src/Disks/tests/gtest_cas_orphan_nomination.cpp:80` (`NominationBackend` derives from `CountingBackend`) and add one test
- Modify: `src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp` (one test)

**Interfaces:**
- Consumes: `prefixEligibleUnder`, `floor_by_ns` from Task 1.
- Produces: `planManifestCursorPage` with the same signature and result type; the freeze loop and `observed_candidates` are gone; the body of a key is read only after that key passed every retain check.

- [ ] **Step 1: Write the failing tests**

In `src/Disks/tests/gtest_cas_orphan_nomination.cpp`, change `class NominationBackend : public InMemoryBackend` to `class NominationBackend : public CountingBackend` (the `InMemoryBackend::read/write/remove` calls inside it keep compiling because `CountingBackend` derives from `InMemoryBackend`; use `CountingBackend::remove` in its override so the delete is counted). Then add, after `RetiresExactManifestSourcesBeforeDelete`:

```cpp
/// A page reads the body of a candidate only. Five live manifests share the namespace with the one
/// orphan candidate; they are active under the floor, are retained from their keys alone, and cost no
/// GET. The candidate costs exactly one.
TEST(CASOrphanNomination, OnlyCandidatesCostABodyRead)
{
    ReadyFixture f = makeReadyFixture();
    const Layout & layout = f.store->layout();
    std::vector<String> live_keys;
    for (uint32_t ordinal = 1; ordinal <= 5; ++ordinal)
    {
        const ManifestRef live{.writer_epoch = kCandidateEpoch, .build_sequence = 7, .manifest_ordinal = ordinal};
        writeManifestRaw(*f.backend, layout, f.ns, live, {blobEntryFor("live", DB::UInt128(0xA000 + ordinal))});
        live_keys.push_back(layout.manifestKey(ManifestId{f.ns, live}));
    }
    f.backend->resetCounts();

    const ManifestSweepResult result = planManifestCursorPage(
        *f.store, "", /*list_budget=*/100, /*nomination_budget=*/100, /*catalog_recovery_authoritative=*/true, nullptr);

    ASSERT_EQ(result.nominations.size(), 1u);
    EXPECT_EQ(f.backend->getCount(layout.manifestKey(f.candidate)), 1u);
    for (const String & key : live_keys)
        EXPECT_EQ(f.backend->getCount(key), 0u) << key;
}
```

In `src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp` add:

```cpp
TEST(CASOrphanSweepRequests, RetainedKeysCostNoBodyRead)
{
    PageFixture f(/*manifests=*/50, /*min_active=*/25);
    const Layout & layout = f.store->layout();
    const ManifestSweepResult result = f.page();
    EXPECT_EQ(result.retained_no_coverage, 24u);
    for (uint64_t seq = 1; seq <= 50; ++seq)
        EXPECT_EQ(f.backend->getCount(layout.manifestKey(ManifestId{kNs, build(seq)})), 0u) << seq;
}
```

- [ ] **Step 2: Build and run to verify they fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t2a.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASOrphanNomination.OnlyCandidatesCostABodyRead:CASOrphanSweepRequests.RetainedKeysCostNoBodyRead' > test_wok_t2_fail.log 2>&1; echo GTEST_EXIT=$?
```
Expected: both FAIL on the body-read counts (today the freeze loop reads the first 100 well-formed keys).

- [ ] **Step 3: Restructure the page**

In `planManifestCursorPage`:

1. Delete the freeze block (`:630-655`: the comment starting "Freeze every possible destructive candidate" through the closing brace of `if (nomination_budget > 0) { … }`) and the declaration `std::map<String, std::optional<Object>> observed_candidates;`.
2. Above the decision loop add:

```cpp
    /// Keys that passed every retain check. Their bodies are read AFTER the loop, and after the
    /// catalog cut: a manifest key is write-once, so its bytes do not depend on when they are read,
    /// and the only thing a later read can observe differently is absence, which retains.
    std::vector<ListedManifestObject> candidates;
```
3. In the loop, the budget check at the top becomes
```cpp
        if (budget_exhausted || (nomination_budget > 0 && candidates.size() >= nomination_budget))
```
4. Replace everything from `const auto observed_it = observed_candidates.find(parsed->key);` to the end of the loop body (the decode, identity check and `result.nominations.push_back`) with:
```cpp
        candidates.push_back(*parsed);
        decided_through = listed.key;
```
5. After the loop and before `if (budget_exhausted)`, add the body pass:

```cpp
    for (const ListedManifestObject & candidate : candidates)
    {
        const std::optional<Object> got = op.read(candidate.key, Retry::standard());
        if (!got)
        {
            /// Gone since the LIST: a fresh writer never reuses the key, so there is nothing to
            /// nominate and nothing to retain. The key was decided above; the cursor stands.
            ++result.skipped;
            continue;
        }
        std::optional<PartManifest> body;
        try
        {
            body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
        }
        catch (const Exception &)
        {
            /// A body we cannot decode cannot be shown safe to delete: proving that needs the source
            /// edges this decode would have produced. So retain it, record it, and walk on. The object
            /// stays visible to fsck, which counts it as unreachable; the alternative -- letting this
            /// escape -- aborts the round and every later round on the same object, which stops
            /// reclamation for the whole pool rather than for this one key.
            LOG_ERROR(getLogger("CasOrphanManifestSweep"),
                "CAS orphan sweep: manifest at {} cannot be decoded and was retained; run cas-fsck to "
                "enumerate such objects", candidate.key);
            ++result.undecodable;
            ++result.skipped;
            continue;
        }
        const ManifestId id{candidate.ns, candidate.ref};
        if (!refMatchesBody(id.ref, *body) || !manifestNamespaceMatches(id.root_namespace, *body))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS orphan sweep: manifest identity mismatch at {} while deriving exact source edges",
                candidate.key);

        ManifestSweepResult::Nomination nomination{
            .id = id,
            .key = candidate.key,
            .token = PersistedEtag::capture(got->etag),
            .source_retirements = {}};
        for (const ManifestEntry & entry : body->entries)
            if (entry.placement == EntryPlacement::Blob)
                nomination.source_retirements.push_back(BlobSourceRetirement{
                    .ref = entry.ref,
                    .source_id = sourceEdgeId(id, entry.path)});
        result.nominations.push_back(std::move(nomination));
    }
```
6. Update the header comment of `planManifestCursorPage` (`:190-208`): replace "Every candidate is exact-GET, decoded and identity-validated" with "A key is decided from key-derived facts first; only a candidate's body is read, after the catalog cut, then decoded and identity-validated", and "`work_budget`, when set, bounds the body-GET/retention fan-out to `nomination_budget` well-formed candidates" with "`nomination_budget` is a candidate budget: the page stops deciding once it has that many candidates, and reads exactly that many bodies at most".
7. `CA/ContentAddressedSettings.cpp:64`: description becomes `"Orphan-manifest sweep candidate budget per round: keys that pass every retain check, whose bodies are read and decided"`.

- [ ] **Step 4: Build, run the two suites and the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t2b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASOrphanNomination.*:CASOrphanSweepRequests.*' > test_wok_t2_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t2_gate.log 2>&1; echo GTEST_EXIT=$?
```
Expected: 0, 0. `CorruptManifestIsRetainedAndSurfaced` and `TokenAbaIsRetainedAndSurfaced` must still pass unchanged: the undecodable path and the token capture moved, they did not change.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/tests/gtest_cas_orphan_nomination.cpp src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp
git diff --cached --stat
git commit -m "ca-gc: orphan sweep reads a manifest body only for a candidate, after the catalog cut

The freeze-before-cut read guarded a same-key rebirth that cannot happen: manifest keys are
write-once. Retain decisions come from the key, the floor, the protection view and the premise;
the body is read for candidates only, so a page of live manifests costs no GET. The budget is a
candidate budget and its description says so.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/tests/gtest_cas_orphan_nomination.cpp src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp
git branch --show-current
```

---

### Task 3: B2a — `discardRead` on the read-ahead and the key-reader interface

Implements the interface half of spec §B2.

**Files:**
- Modify: `CA/Gc/CasGcReadAhead.h` (declare `discardRead`, `discardHead`, private `discard`)
- Modify: `CA/Gc/CasGcReadAhead.cpp` (implement)
- Create: `CA/Pool/CasKeyReader.h` (`KeyReader`, `InlineKeyReader`)
- Create: `CA/Gc/CasGcKeyReader.h` (`ReadAheadKeyReader`)
- Create: `src/Disks/tests/gtest_cas_gc_key_reader.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace DB::Cas {
  class KeyReader { public: virtual ~KeyReader() = default;
      virtual void hint(const String & key) = 0;
      virtual std::optional<Object> take(const String & key) = 0;
      virtual void discard(const String & key) = 0;
      virtual size_t pending() const = 0;
      virtual size_t window() const = 0; };
  class InlineKeyReader final : public KeyReader { public: explicit InlineKeyReader(CasOperation & op_); … };
  class ReadAheadKeyReader final : public KeyReader { public: explicit ReadAheadKeyReader(GcReadAhead & reads_); … };
  void GcReadAhead::discardRead(const String & key); void GcReadAhead::discardHead(const String & key);
  }
  ```

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_gc_key_reader.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcKeyReader.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/CurrentMetrics.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadPool.h>

/// A reader hands a sequential walk its next object and lets the walk say which keys it will want
/// (hint) and which hinted keys it will never take (discard). The inline reader ignores hints; the
/// read-ahead reader turns them into worker requests and counts a discarded one as wasted at once.

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace ProfileEvents
{
    extern const Event CASGCReadAheadWasted;
    extern const Event CASGCReadAheadHit;
    extern const Event CASGCReadAheadMiss;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::openRequestsForTest;

namespace
{

struct Rig
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled, /*max_threads*/ 4, /*max_free_threads*/ 4, /*queue_size*/ 0};

    void put(const String & key, const String & bytes)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::once()))) << key;
    }
};

uint64_t wasted()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadWasted].load();
}

}

TEST(CASGCKeyReader, DiscardCountsWastedAtOnceAndALaterTakeReadsInline)
{
    Rig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    ReadAheadKeyReader reader(reads);
    rig.backend->resetCounts();

    const uint64_t wasted_before = wasted();
    reader.hint("k1");
    EXPECT_EQ(reader.pending(), 1u);
    reader.discard("k1");
    EXPECT_EQ(reader.pending(), 0u);
    EXPECT_EQ(wasted() - wasted_before, 1u);

    const auto got = reader.take("k1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "one");
    EXPECT_EQ(rig.backend->getCount("k1"), 2u) << "the discarded request and the inline one";
}

TEST(CASGCKeyReader, DiscardOfAnUnhintedKeyIsANoOp)
{
    Rig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    ReadAheadKeyReader reader(reads);
    const uint64_t wasted_before = wasted();
    reader.discard("never-hinted");
    EXPECT_EQ(wasted() - wasted_before, 0u);
    EXPECT_EQ(reader.pending(), 0u);
}

TEST(CASGCKeyReader, DiscardSwallowsAWorkerFailureThatATakeWouldRethrow)
{
    Rig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    ReadAheadKeyReader reader(reads);

    rig.backend->failNextReadWith("k1", std::make_exception_ptr(std::runtime_error("injected worker fault")));
    reader.hint("k1");
    EXPECT_NO_THROW(reader.discard("k1"));

    rig.backend->failNextReadWith("k1", std::make_exception_ptr(std::runtime_error("injected worker fault")));
    reader.hint("k1");
    EXPECT_THROW(static_cast<void>(reader.take("k1")), std::runtime_error);
}

TEST(CASGCKeyReader, InlineReaderHintsNothingAndReadsOnTake)
{
    Rig rig;
    rig.put("k1", "one");
    InlineKeyReader reader(rig.op);
    rig.backend->resetCounts();
    EXPECT_EQ(reader.window(), 0u);
    reader.hint("k1");
    EXPECT_EQ(rig.backend->getCount("k1"), 0u);
    EXPECT_EQ(reader.pending(), 0u);
    const auto got = reader.take("k1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "one");
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    reader.discard("k1");
}

TEST(CASGCKeyReader, ReadAheadReaderWindowAndPendingAreTheReadAheads)
{
    Rig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 8);
    ReadAheadKeyReader reader(reads);
    EXPECT_EQ(reader.window(), reads.window());
    EXPECT_EQ(reader.window(), 32u);
    rig.put("a", "1");
    reader.hint("a");
    EXPECT_EQ(reader.pending(), reads.pending());
    static_cast<void>(reader.take("a"));
}
```
If `failNextReadWith` is the arming the read-ahead worker's `read` consumes (it is: the worker calls `worker.read`, which reaches `InMemoryBackend::read`), the third test needs nothing else. If the arming is consumed by a resolve read first, arm twice.

- [ ] **Step 2: Build to verify the failure**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t3a.log 2>&1; echo NINJA_EXIT=$?
```
Expected: compile error, the headers do not exist.

- [ ] **Step 3: Implement**

`CA/Pool/CasKeyReader.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <optional>

namespace DB::Cas
{

/// What a sequential walk needs from whoever fetches its objects. `take` returns the object (or
/// nullopt when absent) and is the only call that decides anything; `hint` may start fetching a key
/// the walk will take later; `discard` drops a hint the walk will never take. A walk that hints
/// nothing and takes everything in order behaves exactly like one that reads inline: the reader is a
/// cache of results, never of decisions.
class KeyReader
{
public:
    virtual ~KeyReader() = default;
    virtual void hint(const String & key) = 0;
    virtual std::optional<Object> take(const String & key) = 0;
    virtual void discard(const String & key) = 0;
    /// Hinted and not yet taken.
    virtual size_t pending() const = 0;
    /// How many hints a walk keeps outstanding; 0 means "do not hint".
    virtual size_t window() const = 0;
};

/// The sequential reader: every take is one inline read on the caller's operation.
class InlineKeyReader final : public KeyReader
{
public:
    explicit InlineKeyReader(CasOperation & op_) : op(op_) {}
    void hint(const String &) override {}
    std::optional<Object> take(const String & key) override { return op.read(key, Retry::standard()); }
    void discard(const String &) override {}
    size_t pending() const override { return 0; }
    size_t window() const override { return 0; }

private:
    CasOperation & op;
};

}
```

`CA/Gc/CasGcKeyReader.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h>

namespace DB::Cas
{

/// The reader over the GC's read-ahead: a hint is a worker request, a take is the worker's result
/// (or an inline read for a key nobody hinted), and a discard drops a hinted key and counts it as
/// wasted at once.
class ReadAheadKeyReader final : public KeyReader
{
public:
    explicit ReadAheadKeyReader(GcReadAhead & reads_) : reads(reads_) {}
    void hint(const String & key) override { reads.hintRead(key); }
    std::optional<Object> take(const String & key) override { return reads.takeRead(key); }
    void discard(const String & key) override { reads.discardRead(key); }
    size_t pending() const override { return reads.pending(); }
    size_t window() const override { return reads.window(); }

private:
    GcReadAhead & reads;
};

}
```

`CA/Gc/CasGcReadAhead.h`, public section after `takeHead`:

```cpp
    /// Drops a hinted key the caller will never take. The request is already in flight or done; the
    /// wait is bounded by that one request, its result and its exception are dropped, and the slot is
    /// counted as wasted now rather than in the destructor. The exception is dropped because no
    /// sequential walk would have issued this request, so none could have failed on it; the fence and
    /// the budget are re-observed by the very next request anyway. A key nobody hinted is a no-op.
    void discardRead(const String & key);
    void discardHead(const String & key);
```
private section:
```cpp
    template <typename T>
    void discard(Slots<T> & slots, const String & key);
```

`CA/Gc/CasGcReadAhead.cpp`, after `take`:

```cpp
template <typename T>
void GcReadAhead::discard(Slots<T> & slots, const String & key)
{
    const auto it = slots.find(key);
    if (it == slots.end())
        return;
    std::shared_ptr<Slot<T>> slot = std::move(it->second);
    slots.erase(it);
    /// `wait`, not `get`: the result and any exception belong to a request nobody wanted.
    slot->future.wait();
    ProfileEvents::increment(ProfileEvents::CASGCReadAheadWasted);
}

void GcReadAhead::discardRead(const String & key)
{
    discard<Object>(reads, key);
}

void GcReadAhead::discardHead(const String & key)
{
    discard<Meta>(heads, key);
}
```
Update the class comment in the header ("Results never taken are awaited by the destructor and counted as wasted") to add "or discarded explicitly by `discardRead`/`discardHead`, which counts them at once".

- [ ] **Step 4: Build, run the suites, run the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t3b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASGCKeyReader.*:CASGCReadAhead.*' > test_wok_t3_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t3_gate.log 2>&1; echo GTEST_EXIT=$?
```

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcKeyReader.h src/Disks/tests/gtest_cas_gc_key_reader.cpp
git diff --cached --stat
git commit -m "ca-gc: key reader with hint, take and discard; GcReadAhead::discardRead counts a dropped hint as wasted at once

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcKeyReader.h src/Disks/tests/gtest_cas_gc_key_reader.cpp
git branch --show-current
```

---

### Task 4: B2b — the sweep's three hinting sites and the epoch-crossing rule

Implements the hinting half of spec §B2.

**Files:**
- Modify: `CA/Pool/CasKeyReader.h` (declare the two ref-log helpers), create `CA/Pool/CasKeyReader.cpp`
- Modify: `CA/Pool/CasRefProtocol.h:798-800` and `CA/Pool/CasRefProtocol.cpp:1042-1105` (`recoverRefTableDetailedFromAuthority` takes `KeyReader *`)
- Modify: `CA/Gc/CasOrphanManifestSweep.h:202-208` and `.cpp` (`activeManifestKeys` takes `KeyReader &`; `planManifestCursorPage` gains `ThreadPool * read_pool, size_t read_concurrency`)
- Modify: `CA/Gc/CasGc.cpp:3210-3218` (the call site passes the pool)
- Modify: `src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp` (two tests)

**Interfaces:**
- Consumes: `KeyReader`, `InlineKeyReader`, `ReadAheadKeyReader`, `GcReadAhead::discardRead` (Task 3).
- Produces:
  ```cpp
  void hintRefLogsWithinEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                              RefTxnId first, const RefTxnId & committed_through);
  void discardRefLogHintsOfEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                                 RefTxnId first, const RefTxnId & committed_through);
  RecoveredRefTable recoverRefTableDetailedFromAuthority(CasOperation & op, const Layout & layout,
      const std::optional<CatalogEntry> & catalog_entry, const std::optional<RefCkpt> & ckpt,
      KeyReader * reader = nullptr);
  ManifestSweepResult planManifestCursorPage(Pool & store, const String & cursor, uint64_t list_budget,
      uint64_t nomination_budget, bool catalog_recovery_authoritative, GcRoundWorkBudget * work_budget = nullptr,
      ThreadPool * read_pool = nullptr, size_t read_concurrency = 1);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp` (add `#include <Common/ThreadPool.h>`, `#include <Common/CurrentMetrics.h>`, `#include <Common/ProfileEvents.h>`, the `CurrentMetrics` externs `LocalThread`, `LocalThreadActive`, `LocalThreadScheduled`, and the `ProfileEvents` externs `CASGCReadAheadWasted`, `CASGCReadAheadHit`):

```cpp
namespace
{

ThreadPool makeReadPool(size_t threads)
{
    return ThreadPool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                      CurrentMetrics::LocalThreadScheduled, threads, threads, /*queue_size*/ 0};
}

/// Every GET the page issued, by key, in whichever thread it ran.
std::map<String, uint64_t> getsOf(CountingBackend & backend)
{
    std::map<String, uint64_t> gets;
    for (const String & key : backend.touchedKeys())
        if (const uint64_t n = backend.getCount(key); n != 0)
            gets[key] = n;
    return gets;
}

struct PageOutcome
{
    uint64_t listed, skipped, nominations, retained_no_coverage, retained_hold, retained_tail_removal, floor_lookups;
    String next_cursor;
    bool operator==(const PageOutcome &) const = default;
};

PageOutcome outcomeOf(const ManifestSweepResult & r)
{
    return {r.listed, r.skipped, r.nominations.size(), r.retained_no_coverage, r.retained_hold,
            r.retained_tail_removal, r.floor_lookups, r.next_cursor};
}

}

/// The read-ahead reader must issue the same GETs against the same keys as the inline reader and
/// decide the same way; only when the bytes arrive moves.
TEST(CASOrphanSweepRequests, PageIsIdenticalInlineAndWithReadAhead)
{
    PageFixture inline_f(/*manifests=*/40, /*min_active=*/30);
    const ManifestSweepResult inline_r = inline_f.page();
    const auto inline_gets = getsOf(*inline_f.backend);

    PageFixture ahead_f(/*manifests=*/40, /*min_active=*/30);
    ThreadPool pool = makeReadPool(4);
    const ManifestSweepResult ahead_r = planManifestCursorPage(
        *ahead_f.store, "", 1000, 100, true, nullptr, &pool, /*read_concurrency=*/16);
    const auto ahead_gets = getsOf(*ahead_f.backend);

    EXPECT_EQ(outcomeOf(inline_r), outcomeOf(ahead_r));
    EXPECT_EQ(inline_gets, ahead_gets);
}

/// A committed tail that spans two epochs: the walk hints ids past the seal in the old epoch, which
/// do not exist, discards them at the crossing (at most one window), and hints the new epoch's ids.
TEST(CASOrphanSweepRequests, EpochCrossingDiscardsAtMostOneWindowAndTheNewEpochIsHinted)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .manifest_sweep_list_budget_keys = 1000,
                                                .manifest_sweep_delete_budget_keys = 100,
                                                .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    casAdmitEntry(*backend, layout, kNs);
    /// Epoch 1: birth at {1,1}, six ordinary logs {1,2..7}, seal at {1,8}. Epoch 2: {2,1..40}.
    publishAt(*backend, layout, kNs, RefTxnId{1, 1}, "t1", /*build_sequence=*/100, DB::UInt128(0x7001), /*birth=*/true);
    for (uint64_t seq = 2; seq <= 7; ++seq)
        publishAt(*backend, layout, kNs, RefTxnId{1, seq}, "t" + std::to_string(seq), 100 + seq, DB::UInt128(0x7000 + seq), /*birth=*/false);
    writeSealAt(*backend, layout, kNs, RefTxnId{1, 8});
    publishAt(*backend, layout, kNs, RefTxnId{2, 1}, "u1", 200, DB::UInt128(0x8001), /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 8});
    for (uint64_t seq = 2; seq <= 40; ++seq)
        publishAt(*backend, layout, kNs, RefTxnId{2, seq}, "u" + std::to_string(seq), 200 + seq, DB::UInt128(0x8000 + seq), /*birth=*/false);
    writeRecoverableCkptForRawFixture(*backend, layout, kNs, RefCkpt{
        .life_epoch = 1, .committed_through = RefTxnId{2, 40},
        .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = RefTxnId{1, 8}});
    writeManifestRaw(*backend, layout, kNs, build(1), {blobEntryFor("a", DB::UInt128(0x100))});
    setWatermarkMinActive(*backend, layout, "test", 2, /*min_active=*/1000);
    backend->resetCounts();

    const uint64_t wasted_before = ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadWasted].load();
    const uint64_t hits_before = ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadHit].load();
    ThreadPool pool = makeReadPool(4);
    const ManifestSweepResult result = planManifestCursorPage(*store, "", 1000, 100, true, nullptr, &pool, 16);
    const uint64_t wasted = ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadWasted].load() - wasted_before;
    const uint64_t hits = ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadHit].load() - hits_before;

    EXPECT_EQ(result.listed, 1u);
    EXPECT_LE(wasted, 64u) << "one window at concurrency 16";
    EXPECT_GE(hits, 30u) << "the new epoch's logs were hinted and taken";
    /// The inline walk reads the same logs once each.
    for (uint64_t seq = 1; seq <= 40; ++seq)
        EXPECT_EQ(backend->getCount(layout.refLogKey(NamespaceLifeId::fromCatalogEntry(kNs, catalogLifeIdForTest(*backend, layout, kNs)), RefTxnId{2, seq})), 1u) << seq;
}
```
`catalogLifeIdForTest` is `cas_test_helpers.h:1070`; check its exact signature before using it and adjust the call. The recovery walk and the tail walk both read epoch 2's logs; if the counts come out as 2 per key that is today's behaviour too (two walks), so assert against the inline fixture's counts instead of the literal `1u` if that is what the inline run shows.

- [ ] **Step 2: Build to verify the failure**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t4a.log 2>&1; echo NINJA_EXIT=$?
```
Expected: compile error on the new `planManifestCursorPage` parameters.

- [ ] **Step 3: Implement the helpers**

`CA/Pool/CasKeyReader.h`, after `InlineKeyReader` (add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>`):

```cpp
/// Hints the ref-log ids of `first`'s epoch, from `first` upward, while the reader has window and the
/// id is within the committed frontier. Only this epoch: past its seal the ids do not exist, and a
/// walk learns where the seal is only by decoding it.
void hintRefLogsWithinEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                            RefTxnId first, const RefTxnId & committed_through);

/// The other half of the rule above, called when a walk crosses an epoch: every hint of the old epoch
/// from `first` up to one window is dropped, so the window is free for the new epoch. Discarding an
/// unhinted key is a no-op, so over-asking by a window is harmless.
void discardRefLogHintsOfEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                               RefTxnId first, const RefTxnId & committed_through);
```

`CA/Pool/CasKeyReader.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h>

#include <limits>

namespace DB::Cas
{

void hintRefLogsWithinEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                            RefTxnId first, const RefTxnId & committed_through)
{
    while (reader.pending() < reader.window() && first <= committed_through)
    {
        reader.hint(layout.refLogKey(life, first));
        if (first.ref_sequence == std::numeric_limits<uint64_t>::max())
            return;
        ++first.ref_sequence;
    }
}

void discardRefLogHintsOfEpoch(KeyReader & reader, const Layout & layout, const NamespaceLifeId & life,
                               RefTxnId first, const RefTxnId & committed_through)
{
    for (size_t n = 0; n < reader.window() && first <= committed_through; ++n)
    {
        reader.discard(layout.refLogKey(life, first));
        if (first.ref_sequence == std::numeric_limits<uint64_t>::max())
            return;
        ++first.ref_sequence;
    }
}

}
```
`first.ref_sequence` must be nonzero (`renderRefTxnId` throws on zero); every caller passes `id.ref_sequence + 1` of a valid id.

- [ ] **Step 4: Thread the reader through the recovery walk**

`CA/Pool/CasRefProtocol.h:798`: add the parameter `KeyReader * reader = nullptr` and `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h>`; extend the doc comment: "`reader`, when set, fetches the logs; it may prefetch ids of the current epoch and drops them at a seal. Every decision and every error path is the same with and without it."

`CA/Pool/CasRefProtocol.cpp:1069-1100`, the walk becomes:

```cpp
    if (grounding.walk_from && grounding.committed_through)
    {
        RefTxnId id = *grounding.walk_from;
        while (id <= *grounding.committed_through)
        {
            const String key = layout.refLogKey(life, id);
            if (reader && id.ref_sequence < std::numeric_limits<uint64_t>::max())
                hintRefLogsWithinEpoch(*reader, layout, life, RefTxnId{id.writer_epoch, id.ref_sequence + 1},
                                       *grounding.committed_through);
            const auto got = reader ? reader->take(key) : op.read(key, Retry::standard());
            if (!got)
            {
                /// (unchanged CORRUPTED_DATA throw)
            }

            RefLogTxn txn = decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
            const bool is_seal = refLogTxnIsEpochSeal(txn);
            /// (unchanged footprint accounting and builder.applyOne)

            if (const std::optional<RefTxnId> next = nextRefLogIdWithinCommittedFrontier(
                    id, is_seal, *grounding.committed_through))
            {
                if (is_seal && reader && id.ref_sequence < std::numeric_limits<uint64_t>::max())
                    discardRefLogHintsOfEpoch(*reader, layout, life, RefTxnId{id.writer_epoch, id.ref_sequence + 1},
                                              *grounding.committed_through);
                id = *next;
            }
            else
                break;
        }
    }
```
Keep the existing throw text and the footprint block byte-for-byte; only the read and the two helper calls are new. Every other caller (`Tools/CasFsck.cpp:140`, `:636`, `Gc/CasGc.cpp:4327`) keeps the default `nullptr`.

- [ ] **Step 5: Thread the reader through the sweep**

In `CA/Gc/CasOrphanManifestSweep.cpp`:

1. `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcKeyReader.h>` and `#include <Common/ThreadPool.h>`.
2. `activeManifestKeys` gains a parameter `KeyReader & reader` after `op`; the call to the recovery at `:199` passes `&reader`; the tail walk at `:288-330` reads through the reader with the same hint/discard shape as Step 4:
```cpp
    while (id <= *ckpt.committed_through)
    {
        if (work_budget && !work_budget->sweepRecoveryOpAvailable())
        {
            protection.recovery_incomplete = true;
            break;
        }
        if (id.ref_sequence < std::numeric_limits<uint64_t>::max())
            hintRefLogsWithinEpoch(reader, layout, life, RefTxnId{id.writer_epoch, id.ref_sequence + 1}, *ckpt.committed_through);
        const auto got = reader.take(layout.refLogKey(life, id));
        …
        if (const std::optional<RefTxnId> next = nextRefLogIdWithinCommittedFrontier(id, is_seal, *ckpt.committed_through))
        {
            if (is_seal)
            {
                if (id.ref_sequence < std::numeric_limits<uint64_t>::max())
                    discardRefLogHintsOfEpoch(reader, layout, life, RefTxnId{id.writer_epoch, id.ref_sequence + 1}, *ckpt.committed_through);
                prior.reset();
                prior_is_seal.reset();
            }
            else
            {
                prior = id;
                prior_is_seal = false;
            }
            id = *next;
        }
        else
            break;
    }
```
The cursor read at `:252` and the `cross_from_missing_cursor` path stay inline on `op`.
3. `planManifestCursorPage` gains `ThreadPool * read_pool, size_t read_concurrency` (header and definition). After `CasOperation op = store.openRequests().admit();`:
```cpp
    /// The page's reader. With a pool and concurrency above one the candidates' bodies and the two
    /// ref-stream walks overlap their round trips; otherwise every read is inline and the page is the
    /// sequential one, request for request.
    std::optional<GcReadAhead> read_ahead;
    std::unique_ptr<KeyReader> reader;
    if (read_pool && read_concurrency > 1)
    {
        read_ahead.emplace(op, store.openRequests(), *read_pool, read_concurrency);
        reader = std::make_unique<ReadAheadKeyReader>(*read_ahead);
    }
    else
        reader = std::make_unique<InlineKeyReader>(op);
```
`read_ahead` must be declared AFTER `op` (its destructor waits on workers that resume under `op`'s generation, and `op` must outlive it) and BEFORE `reader`.
4. The `activeManifestKeys(op, layout, *catalog_entry, ckpt->ckpt, view_it->second.coverage, work_budget)` call becomes `activeManifestKeys(op, *reader, layout, …)`.
5. The body pass of Task 2 hints ahead:
```cpp
    size_t next_body_hint = 0;
    for (const ListedManifestObject & candidate : candidates)
    {
        while (next_body_hint < candidates.size() && reader->pending() < reader->window())
            reader->hint(candidates[next_body_hint++].key);
        const std::optional<Object> got = reader->take(candidate.key);
        …
```
6. `sweepNamespace` (`:505`) constructs `InlineKeyReader reader(op);` and passes it to `activeManifestKeys`.
7. `CA/Gc/CasGc.cpp:3210`: the call passes `&work_budget, read_pool.get(), store->poolConfig().gc_read_concurrency`.

- [ ] **Step 6: Build, run the suites, run the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t4b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASOrphanSweepRequests.*:CASOrphanNomination.*:CASRefRecoveryCasWalk.*:CASGCReadAhead.*' > test_wok_t4_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t4_gate.log 2>&1; echo GTEST_EXIT=$?
```
`CASGCReadAhead.FoldIsIdenticalAtConcurrencyOneAndEight` compares the fold's GET multiset at two concurrencies; the sweep now runs inside that fold with the read-ahead too, and its hints are discarded at seals, so the multisets must still be equal. If they differ, the discard rule is not firing at a crossing.

- [ ] **Step 7: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp
git diff --cached --stat
git commit -m "ca-gc: orphan sweep reads candidate bodies and both ref-stream walks through the read-ahead; hints stop at the epoch and are discarded at a seal

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Disks/tests/gtest_cas_orphan_sweep_requests.cpp
git branch --show-current
```

---

### Task 5: C1 — `WriteOnceKey` and its three factories

Implements spec §C "The key type".

**Files:**
- Create: `CA/Primitives/CasWriteOnceKey.h`
- Modify: `CA/Formats/CasLayout.h` (include; three factories next to `manifestKey`, `refLogKey`, `refSnapshotKey`)
- Create: `src/Disks/tests/gtest_cas_write_once_key.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace DB::Cas {
  class WriteOnceKey { public: const String & str() const; private: friend class Layout; explicit WriteOnceKey(String); String key; };
  WriteOnceKey Layout::writeOnceManifestKey(const ManifestId &) const;
  WriteOnceKey Layout::writeOnceRefLogKey(const NamespaceLifeId &, const RefTxnId &) const;
  WriteOnceKey Layout::writeOnceRefSnapshotKey(const NamespaceLifeId &, const RefTxnId &) const;
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_write_once_key.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasWriteOnceKey.h>
#include <Disks/tests/cas_test_helpers.h>

#include <type_traits>

/// A `WriteOnceKey` names an object of one of the three families that are written once and never
/// rewritten: a part manifest, a ref log, a ref snapshot. Only `Layout` can mint one, from a typed
/// identity, so a verb that takes the type cannot be handed a mutable control key.

using namespace DB::Cas;

static_assert(!std::is_default_constructible_v<WriteOnceKey>);
static_assert(!std::is_constructible_v<WriteOnceKey, String>);
static_assert(!std::is_constructible_v<WriteOnceKey, const char *>);

TEST(CASWriteOnceKey, FactoriesMintTheSameStringsAsThePlainKeyFunctions)
{
    const Layout layout{"p"};
    const RootNamespace ns{"test/aa@cas@"};
    const ManifestId manifest{ns, ManifestRef{.writer_epoch = 3, .build_sequence = 9, .manifest_ordinal = 2}};
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, DB::UInt128(0x1234));
    const RefTxnId id{5, 7};

    EXPECT_EQ(layout.writeOnceManifestKey(manifest).str(), layout.manifestKey(manifest));
    EXPECT_EQ(layout.writeOnceRefLogKey(life, id).str(), layout.refLogKey(life, id));
    EXPECT_EQ(layout.writeOnceRefSnapshotKey(life, id).str(), layout.refSnapshotKey(life, id));
    EXPECT_TRUE(layout.parseManifestKey(layout.writeOnceManifestKey(manifest).str()).has_value());
    EXPECT_TRUE(layout.parseRefObjectKey(layout.writeOnceRefLogKey(life, id).str()).has_value());
}
```

- [ ] **Step 2: Build to verify the failure**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t5a.log 2>&1; echo NINJA_EXIT=$?
```

- [ ] **Step 3: Implement**

`CA/Primitives/CasWriteOnceKey.h`:

```cpp
#pragma once
#include <base/types.h>
#include <utility>

namespace DB::Cas
{

class Layout;

/// The key of an object that is written once and never rewritten: a part manifest, a ref log, a ref
/// snapshot. Only `Layout` mints one, from the typed identity of such an object, so a verb that
/// accepts this type can delete without a precondition: whatever body the key holds is the one body
/// it ever held. A mutable control object (a checkpoint, the catalog, `gc/state`, a mount lease) has
/// no path to this type.
class WriteOnceKey
{
public:
    const String & str() const { return key; }

private:
    friend class Layout;
    explicit WriteOnceKey(String key_) : key(std::move(key_)) {}
    String key;
};

}
```

`CA/Formats/CasLayout.h`: add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasWriteOnceKey.h>`; after `refSnapshotKey` (`:169`):

```cpp
    /// The write-once forms of `refLogKey` and `refSnapshotKey`: the same strings, typed as keys a
    /// precondition-free delete may take. Both objects are published with a create-only write at a
    /// life-qualified key.
    WriteOnceKey writeOnceRefLogKey(const NamespaceLifeId & ns_id, const RefTxnId & id) const
    {
        return WriteOnceKey(refLogKey(ns_id, id));
    }
    WriteOnceKey writeOnceRefSnapshotKey(const NamespaceLifeId & ns_id, const RefTxnId & id) const
    {
        return WriteOnceKey(refSnapshotKey(ns_id, id));
    }
```
after `manifestKey` (`:274`):
```cpp
    /// The write-once form of `manifestKey`: a manifest is published with a create-only write at a key
    /// whose epoch, build sequence and ordinal never repeat under one server root.
    WriteOnceKey writeOnceManifestKey(const ManifestId & id) const
    {
        return WriteOnceKey(manifestKey(id));
    }
```

- [ ] **Step 4: Build, run, gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t5b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASWriteOnceKey.*:CASLayout*' > test_wok_t5_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t5_gate.log 2>&1; echo GTEST_EXIT=$?
```

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasWriteOnceKey.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h src/Disks/tests/gtest_cas_write_once_key.cpp
git diff --cached --stat
git commit -m "ca-layout: WriteOnceKey, mintable only by Layout for manifests, ref logs and ref snapshots

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasWriteOnceKey.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h src/Disks/tests/gtest_cas_write_once_key.cpp
git branch --show-current
```

---

### Task 6: C2 — the object-storage batch overload and its S3 implementation

Implements spec §C "Object storage".

**Files:**
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h:371-374` (declare), `IObjectStorage.cpp:91-97` (default)
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h:124-130`, `:200-216` (declare override and impl)
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp` (implement, next to `removeObjectIfTokenMatchesImpl`)
- Test: the default is exercised in Task 7's suite on `LocalObjectStorage`; the S3 body is exercised by Task 10 and the GCS live gate.

**Interfaces:**
- Produces: `virtual void IObjectStorage::removeObjectsIfExistUnderProfile(const StoredObjects & objects, ObjectStorageRetryProfile profile, uint64_t request_timeout_ms);` (default throws `NOT_IMPLEMENTED`); S3 override; private `void S3ObjectStorage::removeObjectsIfExistImpl(const StoredObjects &, const std::shared_ptr<const S3::Client> &)`.

- [ ] **Step 1: Declare**

`IObjectStorage.h`, after the profile overload of `removeObjectIfTokenMatches` (`:373`):

```cpp
    /// Removes every object in ONE request with no per-key precondition; an absent object is success.
    /// Content-addressed callers use it for write-once keys only, at most 1000 per call. Throws on a
    /// request-level failure and on any per-key error other than "not found", naming the failed keys.
    /// Same profile note as `iterate`. Backends without a batch delete keep the default, which refuses.
    virtual void removeObjectsIfExistUnderProfile(
        const StoredObjects & objects, ObjectStorageRetryProfile profile, uint64_t request_timeout_ms);
```
`IObjectStorage.cpp`, after the existing default at `:97`:
```cpp
void IObjectStorage::removeObjectsIfExistUnderProfile(const StoredObjects &, ObjectStorageRetryProfile, uint64_t)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support batch removal under a retry profile", getName());
}
```
`S3ObjectStorage.h`: public, after the profile overload at `:126-127`:
```cpp
    /// One `DeleteObjects` for the given objects (the caller chunks to at most 1000); absence is success.
    void removeObjectsIfExistUnderProfile(
        const StoredObjects & objects, ObjectStorageRetryProfile profile, uint64_t request_timeout_ms) override;
```
private, after `removeObjectIfTokenMatchesImpl` (`:213-214`):
```cpp
    void removeObjectsIfExistImpl(const StoredObjects & objects, const std::shared_ptr<const S3::Client> & used_client);
```

- [ ] **Step 2: Implement**

`S3ObjectStorage.cpp`, after `removeObjectIfTokenMatchesImpl`:

```cpp
void S3ObjectStorage::removeObjectsIfExistUnderProfile(
    const StoredObjects & objects, ObjectStorageRetryProfile profile, uint64_t request_timeout_ms)
{
    refreshAndRetryOnExpiredCredentials([&]
    {
        removeObjectsIfExistImpl(objects, clientForRetryProfile(profile, request_timeout_ms));
        return 0;
    });
}

void S3ObjectStorage::removeObjectsIfExistImpl(const StoredObjects & objects, const std::shared_ptr<const S3::Client> & used_client)
{
    if (objects.empty())
        return;

    std::vector<Aws::S3::Model::ObjectIdentifier> identifiers; // STYLE_CHECK_ALLOW_STD_CONTAINERS
    identifiers.reserve(objects.size());
    for (const auto & object : objects)
    {
        Aws::S3::Model::ObjectIdentifier identifier;
        identifier.SetKey(object.remote_path);
        identifiers.push_back(std::move(identifier));
    }
    Aws::S3::Model::Delete to_delete;
    to_delete.SetObjects(std::move(identifiers));
    /// Quiet: only failed keys come back. A key that is gone or was never there is not a failure here
    /// (`NoSuchKey` below), and the caller has no use for the per-key successes.
    to_delete.SetQuiet(true);

    S3::DeleteObjectsRequest request;
    request.SetBucket(uri.bucket);
    request.SetDelete(std::move(to_delete));

    ProfileEvents::increment(ProfileEvents::DiskS3DeleteObjects);
    auto outcome = used_client->DeleteObjects(request);

    /// Every key lands in system.blob_storage_log, as the single-key paths do; the batch's outcome is
    /// stamped on each of them.
    if (auto blob_storage_log = BlobStorageLogWriter::create(disk_name))
    {
        for (const auto & object : objects)
            blob_storage_log->addEvent(BlobStorageLogElement::EventType::Delete,
                                       uri.bucket, object.remote_path,
                                       object.local_path, object.bytes_size,
                                       /* elapsed_microseconds */ 0,
                                       outcome.IsSuccess() ? 0 : static_cast<Int32>(outcome.GetError().GetErrorType()),
                                       outcome.IsSuccess() ? "" : outcome.GetError().GetMessage());
    }

    if (!outcome.IsSuccess())
    {
        const auto & err = outcome.GetError();
        throw S3Exception(err.GetErrorType(), "{} (Code: {}) while removing {} objects from S3 in one request",
                          err.GetMessage(), static_cast<size_t>(err.GetErrorType()), objects.size());
    }

    String failed_keys;
    std::optional<Aws::S3::S3Errors> first_error_type;
    for (const auto & err : outcome.GetResult().GetErrors())
    {
        const auto error_type = static_cast<Aws::S3::S3Errors>(
            Aws::S3::S3ErrorMapper::GetErrorForName(err.GetCode().c_str()).GetErrorType());
        if (S3::isNotFoundError(error_type))
            continue;
        if (!failed_keys.empty())
            failed_keys += ", ";
        failed_keys += err.GetKey() + " (" + err.GetCode() + ": " + err.GetMessage() + ")";
        if (!first_error_type)
            first_error_type = error_type;
    }
    if (first_error_type)
        throw S3Exception(*first_error_type, "batch removal left objects behind: [{}]", failed_keys);
}
```
Includes: the same the batch helper uses (`src/IO/S3/deleteFileFromS3.cpp:1-8`: `IO/S3/Requests.h`, `IO/S3/getObjectInfo.h`, `Common/BlobStorageLogWriter.h`); `S3::isNotFoundError` is in `IO/S3/getObjectInfo.h` or `IO/S3Common.h`, grep for its declaration. If `refreshAndRetryOnExpiredCredentials` requires a non-void return, the `return 0;` above satisfies it; otherwise drop it.

- [ ] **Step 3: Build the server target that includes S3 (the unit-test binary links it too)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t6.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t6_gate.log 2>&1; echo GTEST_EXIT=$?
```
Expected: builds; the gate is unchanged (nothing calls the overload yet).

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.cpp src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
git diff --cached --stat
git commit -m "object storage: removeObjectsIfExistUnderProfile, one DeleteObjects under a chosen retry profile with absence as success

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.cpp src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
git branch --show-current
```

---

### Task 7: C3 — `Backend::removeManyWriteOnce` in every backend

Implements spec §C "Backend".

**Files:**
- Modify: `CA/Backend/CasBackend.h:194` (pure virtual after `publish`)
- Modify: `CA/Backend/CasObjectStorageBackend.h`, `.cpp` (both modes; extract `emuForgetDeletedToken`)
- Modify: `CA/Backend/CasInMemoryBackend.h`, `.cpp` (verb, `failNextBulkRemoveWith`, `onBeforeBulkRemove`, `bulkRemoveCalls`)
- Modify: `CA/Backend/CasInstrumentedBackend.h`, `.cpp` (verb, `CASBulkDeleteRequests`)
- Modify: `CA/Backend/CasThrottlingBackend.h` (verb)
- Modify: `src/Common/ProfileEvents.cpp:918` (new event next to `CASGCEnumerationPages`)
- Modify: `src/Disks/tests/cas_test_helpers.h` (`CountingBackend` counts the verb), `src/Disks/tests/gtest_cas_part_write.cpp:1645-1690` and `src/Disks/tests/gtest_cas_decommission.cpp:1252-1290` (the two fakes forward)
- Create: `src/Disks/tests/gtest_cas_bulk_delete_backend.cpp`

**Interfaces:**
- Consumes: `WriteOnceKey` (Task 5), `removeObjectsIfExistUnderProfile` (Task 6).
- Produces: `virtual void Backend::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess &) = 0;`; `InMemoryBackend::failNextBulkRemoveWith(std::exception_ptr)`, `InMemoryBackend::onBeforeBulkRemove(std::function<void()>)`, `size_t InMemoryBackend::bulkRemoveCalls() const`; `ProfileEvents::CASBulkDeleteRequests`.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_bulk_delete_backend.cpp`:

```cpp
#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>

/// `removeManyWriteOnce` deletes up to 1000 write-once keys in one request with no precondition:
/// an absent key is success, a present one is gone afterwards, and every backend honours the same
/// fault knobs the single-key delete has.

namespace ProfileEvents
{
    extern const Event CASBulkDeleteRequests;
    extern const Event CASManifestDelete;
    extern const Event CASRootDelete;
}

namespace DB::ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::openRequestsForTest;

namespace
{

const Layout kLayout{"p"};
const RootNamespace kNs{"test/aa@cas@"};

ManifestId manifest(uint32_t ordinal)
{
    return ManifestId{kNs, ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = ordinal}};
}

/// Three manifest keys with bodies and one that was never written.
struct Keys
{
    std::vector<WriteOnceKey> present;
    WriteOnceKey absent;
};

Keys seed(CasOperation & op)
{
    Keys keys{.present = {}, .absent = kLayout.writeOnceManifestKey(manifest(4))};
    for (uint32_t ordinal = 1; ordinal <= 3; ++ordinal)
    {
        const WriteOnceKey key = kLayout.writeOnceManifestKey(manifest(ordinal));
        EXPECT_TRUE(std::holds_alternative<Committed>(op.create(key.str(), "body-" + std::to_string(ordinal), Retry::once())));
        keys.present.push_back(key);
    }
    return keys;
}

}

TEST(CASBulkDeleteBackend, InMemoryDeletesPresentKeysAndTreatsAbsentAsSuccess)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Keys keys = seed(op);
    std::vector<WriteOnceKey> batch = keys.present;
    batch.push_back(keys.absent);

    op.removeManyWriteOnce(batch, Retry::once());

    for (const WriteOnceKey & key : batch)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
    EXPECT_EQ(backend->bulkRemoveCalls(), 1u);
}

TEST(CASBulkDeleteBackend, InMemoryHeldDeletesLandLater)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Keys keys = seed(op);

    backend->holdDeletes(true);
    op.removeManyWriteOnce(keys.present, Retry::once());
    for (const WriteOnceKey & key : keys.present)
        EXPECT_TRUE(op.head(key.str(), Retry::once()).has_value()) << "held, not landed: " << key.str();
    backend->landPendingDeletes();
    for (const WriteOnceKey & key : keys.present)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
}

TEST(CASBulkDeleteBackend, InMemoryArmedFailureFiresOnceAndTheHookRunsBeforeTheDeletes)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Keys keys = seed(op);

    size_t hook_runs = 0;
    backend->onBeforeBulkRemove([&] { ++hook_runs; });
    backend->failNextBulkRemoveWith(std::make_exception_ptr(Poco::TimeoutException("injected")));

    op.removeManyWriteOnce(keys.present, Retry::standard());   /// the engine reissues the chunk
    EXPECT_EQ(backend->bulkRemoveCalls(), 2u);
    EXPECT_EQ(hook_runs, 1u) << "the hook runs on the attempt that deletes, not on the refused one";
    for (const WriteOnceKey & key : keys.present)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
}

TEST(CASBulkDeleteBackend, InstrumentedCountsOneRequestAndOneDeletePerKeyClass)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto backend = std::make_shared<InstrumentedBackend>(inner);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Keys keys = seed(op);
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(kNs, DB::UInt128(0x55));
    const WriteOnceKey log = kLayout.writeOnceRefLogKey(life, RefTxnId{1, 1});
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(log.str(), "log", Retry::once())));

    const auto requests_before = ProfileEvents::global_counters[ProfileEvents::CASBulkDeleteRequests].load();
    const auto manifest_before = ProfileEvents::global_counters[ProfileEvents::CASManifestDelete].load();
    const auto root_before = ProfileEvents::global_counters[ProfileEvents::CASRootDelete].load();

    std::vector<WriteOnceKey> batch = keys.present;
    batch.push_back(log);
    op.removeManyWriteOnce(batch, Retry::once());

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASBulkDeleteRequests].load() - requests_before, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASManifestDelete].load() - manifest_before, 3u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRootDelete].load() - root_before, 1u);
}

TEST(CASBulkDeleteBackend, ThrottlingRefusesTheChunkOnceAndTheEngineReissuesIt)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto backend = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::FirstPerKey, 1, 429);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Keys keys = seed(op);   /// the creates take the first-per-key refusal for these keys' writes

    op.removeManyWriteOnce(keys.present, Retry::standard());
    for (const WriteOnceKey & key : keys.present)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
    EXPECT_GE(backend->refusals(keys.present.front().str()), 1u);
}

#if USE_AWS_S3
TEST(CASBulkDeleteBackend, EmulatedModeDeletesUnderTheEmulationLockAndForgetsTheTokens)
{
    auto storage = DB::Cas::tests::makeLocalObjectStorageForTest();
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::EmulatedSingleProcess);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    Keys keys = seed(op);
    std::vector<WriteOnceKey> batch = keys.present;
    batch.push_back(keys.absent);

    op.removeManyWriteOnce(batch, Retry::once());

    for (const WriteOnceKey & key : batch)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
    /// A recreate at a deleted key must mint a fresh incarnation, which is what the token bookkeeping
    /// after a delete exists for.
    EXPECT_TRUE(std::holds_alternative<Committed>(op.create(keys.present.front().str(), "again", Retry::once())));
}

TEST(CASBulkDeleteBackend, LocalObjectStorageRefusesTheProfileOverload)
{
    auto storage = DB::Cas::tests::makeLocalObjectStorageForTest();
    DB::StoredObjects objects{DB::StoredObject("p/anything")};
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&]
    {
        storage->removeObjectsIfExistUnderProfile(objects, DB::ObjectStorageRetryProfile::SingleAttempt, 1000);
    });
}
#endif
```
`holdDeletes` and `landPendingDeletes` are the in-memory backend's existing hold knobs; read their exact names at `CA/Backend/CasInMemoryBackend.h:95-200` (`hold_deletes_`, `pending_deletes_`) and use those. `CasOperation::removeManyWriteOnce` is Task 8; until then, call the backend verb directly through `OperationForTest`'s access if you want the backend tests green before Task 8, or run Tasks 7 and 8 back to back and gate once. This plan gates once after Task 8.

- [ ] **Step 2: Add the event and the pure virtual**

`src/Common/ProfileEvents.cpp`, after the `CASGCEnumerationPages` line (`:918`):
```cpp
    M(CASBulkDeleteRequests, "Number of CAS batch delete requests: one DeleteObjects carrying up to 1000 write-once keys (manifest bodies, ref logs, ref snapshots). The per-key class counters (CASManifestDelete, CASRootDelete) say how many keys each request carried.", ValueType::Number) \
```
`CA/Backend/CasBackend.h`, after `publish` (`:194`), with `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasWriteOnceKey.h>` and `#include <vector>`:
```cpp
    /// Removes up to 1000 WRITE-ONCE keys in one request with no per-key precondition; an absent key
    /// is success. The caller proves the keys are write-once by minting them as `WriteOnceKey`, so a
    /// backend never sees a mutable control key through this verb. Throws on any failure; the whole
    /// chunk is reissued by the engine, which is sound because a key already deleted is absent, and
    /// absence is success.
    virtual void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess &) = 0;
```

- [ ] **Step 3: Implement the object-storage backend**

`CA/Backend/CasObjectStorageBackend.h`: declare `void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override;` after `remove`; add to the emulated helpers (`:244-280`): `/// Caller holds emu_mutex: the token bookkeeping after a delete at `key`. void emuForgetDeletedToken(const String & key);`.

`CA/Backend/CasObjectStorageBackend.cpp`: move the tail of the emulated `removeUnder` (from `if (auto it = emu_token_state.find(key); …` through the `else emu_token_expiry.push_back(…)` block) into
```cpp
void ObjectStorageBackend::emuForgetDeletedToken(const String & key)
{
    /// Keep the deleted incarnation's last-minted etag around ONLY while a same-mtime-quantum
    /// collision with an immediate recreate is still possible (emuMintToken) — once it is
    /// comfortably old, erase it so `emu_token_state` does not grow for the lifetime of the backend
    /// instance.
    if (auto it = emu_token_state.find(key); it != emu_token_state.end())
    {
        const uint64_t now_ns = emuNowNs();
        if (etagComfortablyInThePast(it->second.first, now_ns))
            emu_token_state.erase(it);
        else
            emu_token_expiry.push_back(EmuTokenExpiry{now_ns, key, it->second});
    }
}
```
and call it from `removeUnder` where the block was. Then:
```cpp
void ObjectStorageBackend::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess &)
{
    if (keys.empty())
        return;
    if (mode == Mode::Native)
    {
        StoredObjects objects;
        objects.reserve(keys.size());
        for (const WriteOnceKey & key : keys)
            objects.emplace_back(key.str());
        /// `NOT_IMPLEMENTED` from a storage without a batch delete propagates -- fail-closed by construction.
        object_storage->removeObjectsIfExistUnderProfile(objects, controlPlaneProfile(), attempt_timeout_ms);
        return;
    }

    std::lock_guard lock(emu_mutex);
    for (const WriteOnceKey & key : keys)
    {
        if (!emuExists(key.str()))
            continue;
        object_storage->removeObjectIfExists(StoredObject(emuPath(key.str())));
        emuForgetDeletedToken(key.str());
    }
}
```

- [ ] **Step 4: Implement the in-memory backend and its knobs**

`CA/Backend/CasInMemoryBackend.h`: declare
```cpp
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override;
    /// The next `removeManyWriteOnce` throws `error` instead of deleting anything; one-shot, like
    /// `failNextWriteWith`.
    void failNextBulkRemoveWith(std::exception_ptr error);
    /// Runs before a `removeManyWriteOnce` applies, with no backend lock held, on the attempt that
    /// will delete (an armed failure fires first and skips the hook).
    void onBeforeBulkRemove(std::function<void()> hook);
    /// How many `removeManyWriteOnce` calls reached the store, armed failures included.
    size_t bulkRemoveCalls() const;
```
and members `std::vector<std::exception_ptr> armed_bulk_remove_failures_; std::function<void()> before_bulk_remove_hook_; size_t bulk_remove_calls_ = 0;`.

`CA/Backend/CasInMemoryBackend.cpp`:
```cpp
void InMemoryBackend::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess &)
{
    std::exception_ptr armed;
    std::function<void()> hook;
    {
        std::lock_guard lock(mutex_);
        ++bulk_remove_calls_;
        if (!armed_bulk_remove_failures_.empty())
        {
            armed = armed_bulk_remove_failures_.front();
            armed_bulk_remove_failures_.erase(armed_bulk_remove_failures_.begin());
        }
        hook = before_bulk_remove_hook_;
    }
    if (armed)
        std::rethrow_exception(armed);
    if (hook)
        hook();

    std::lock_guard lock(mutex_);
    for (const WriteOnceKey & key : keys)
    {
        auto it = store_.find(key.str());
        if (it == store_.end())
            continue;   /// absent is success
        if (hold_deletes_)
        {
            PendingDelete pd;
            pd.key = key.str();
            pd.value = it->second.value;
            pending_deletes_.push_back(std::move(pd));
            continue;
        }
        store_.erase(it);
    }
}

void InMemoryBackend::failNextBulkRemoveWith(std::exception_ptr error)
{
    std::lock_guard lock(mutex_);
    armed_bulk_remove_failures_.push_back(std::move(error));
}

void InMemoryBackend::onBeforeBulkRemove(std::function<void()> hook)
{
    std::lock_guard lock(mutex_);
    before_bulk_remove_hook_ = std::move(hook);
}

size_t InMemoryBackend::bulkRemoveCalls() const
{
    std::lock_guard lock(mutex_);
    return bulk_remove_calls_;
}
```
If `mutex_` is not `mutable`, make `bulkRemoveCalls` take it the way the other const accessors do.

- [ ] **Step 5: Implement the two wrappers and the test doubles**

`CA/Backend/CasInstrumentedBackend.h`, after `remove` (`:126`), with `extern const Event CASBulkDeleteRequests;` in the file's `ProfileEvents` block (or the `.cpp`'s):
```cpp
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        inner->removeManyWriteOnce(keys, access);
        ProfileEvents::increment(ProfileEvents::CASBulkDeleteRequests);
        for (const WriteOnceKey & key : keys)
            incrementCasEvent(classifyCasNs(key.str()), CasOp::Delete);
    }
```
`CA/Backend/CasThrottlingBackend.h`, after `remove` (`:89`):
```cpp
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        for (const WriteOnceKey & key : keys)
            refuseOrPass(key.str());
        inner->removeManyWriteOnce(keys, access);
    }
```
`src/Disks/tests/cas_test_helpers.h`, in `CountingBackend`: an override that ticks `bulk_remove_total` and per-key `delete_counts` for every key, then forwards to `InMemoryBackend::removeManyWriteOnce`; expose `size_t bulkRemoveTotal() const`. `src/Disks/tests/gtest_cas_part_write.cpp:1645` (`LocalCountingBackend`) and `src/Disks/tests/gtest_cas_decommission.cpp:1252` (`FailDeletesUnderPrefixBackend`): add `void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override { inner->removeManyWriteOnce(keys, access); }`.

- [ ] **Step 6: Build; the gate is run after Task 8**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t7.log 2>&1; echo NINJA_EXIT=$?
```
Expected: the tree compiles except `gtest_cas_bulk_delete_backend.cpp`, which needs `CasOperation::removeManyWriteOnce` from Task 8. If the executor wants a green build here, temporarily exclude nothing: go straight to Task 8.

- [ ] **Step 7: Commit (with Task 8, see below)**

---

### Task 8: C4 — the engine verb and the chunk-size setting

Implements spec §C "Engine".

**Files:**
- Modify: `CA/Backend/CasRequests.h` (declare `removeManyWriteOnce`, `kBulkDeleteMaxKeys`), `CA/Backend/CasRequests.cpp` (implement next to `removeUnder`)
- Modify: `CA/ContentAddressedSettings.cpp:64-80`, `:224-236` (setting and validation), `CA/Pool/CasPool.h:90` (`PoolConfig` field), `CA/ContentAddressedMetadataStorage.cpp:70-90`, `:290-305` (plumbing)
- Create: `src/Disks/tests/gtest_cas_bulk_delete_engine.cpp`

**Interfaces:**
- Consumes: `Backend::removeManyWriteOnce` (Task 7).
- Produces: `inline constexpr size_t kBulkDeleteMaxKeys = 1000;` and `void CasOperation::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, const Retry & policy);` (one chunk, at most 1000 keys, `LOGICAL_ERROR` above that); `PoolConfig::gc_bulk_delete_chunk_keys` (default 1000); setting `gc_bulk_delete_chunk_keys`.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_bulk_delete_engine.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>

/// The engine sends one chunk of at most 1000 write-once keys as one request under the ordinary
/// attempt loop; a failed attempt reissues the whole chunk, which is sound because a key the failed
/// attempt already deleted is absent on the reissue, and absence is success.

namespace ProfileEvents
{
    extern const Event CASRequestReissue;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::openRequestsForTest;

namespace
{

const Layout kLayout{"p"};
const RootNamespace kNs{"test/aa@cas@"};

std::vector<WriteOnceKey> manifestKeys(uint32_t count)
{
    std::vector<WriteOnceKey> keys;
    for (uint32_t ordinal = 1; ordinal <= count; ++ordinal)
        keys.push_back(kLayout.writeOnceManifestKey(
            ManifestId{kNs, ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = ordinal}}));
    return keys;
}

}

TEST(CASBulkDeleteEngine, AFailedAttemptReissuesTheWholeChunkAndAlreadyDeletedKeysAreSuccess)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const std::vector<WriteOnceKey> keys = manifestKeys(5);
    for (const WriteOnceKey & key : keys)
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key.str(), "b", Retry::once())));
    /// Half the chunk is gone before the failed attempt reports: the reissue must still succeed.
    {
        const auto h = op.head(keys[0].str(), Retry::once());
        ASSERT_TRUE(h.has_value());
        ASSERT_EQ(op.remove(keys[0].str(), h->etag, Retry::once()), Removal::Removed);
    }
    backend->failNextBulkRemoveWith(std::make_exception_ptr(Poco::TimeoutException("injected")));
    const auto reissues_before = ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load();

    op.removeManyWriteOnce(keys, Retry::standard());

    EXPECT_EQ(backend->bulkRemoveCalls(), 2u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load() - reissues_before, 1u);
    for (const WriteOnceKey & key : keys)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
}

TEST(CASBulkDeleteEngine, AnEmptyChunkIsNoRequest)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    op.removeManyWriteOnce({}, Retry::once());
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
}

TEST(CASBulkDeleteEngine, ExactlyOneThousandKeysIsOneRequest)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    op.removeManyWriteOnce(manifestKeys(1000), Retry::once());   /// all absent: success, one request
    EXPECT_EQ(backend->bulkRemoveCalls(), 1u);
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASBulkDeleteEngineDeathTest, MoreThanOneThousandKeysIsACallerBug)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    EXPECT_DEATH({ op.removeManyWriteOnce(manifestKeys(1001), Retry::once()); }, "");
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
}
#endif
```
Use `std::_Exit` semantics as the other CAS death tests do if the child hangs on the global thread pool; read `gtest_cas_blob_upload_pool.cpp:62-68` for the exact shape.

- [ ] **Step 2: Implement the engine verb**

`CA/Backend/CasRequests.h`: near the top of the `CasOperation` declaration block, `inline constexpr size_t kBulkDeleteMaxKeys = 1000;`; after `removeCurrent` (`:244`):
```cpp
    /// Deletes ONE chunk of up to `kBulkDeleteMaxKeys` write-once keys as one request under the
    /// policy: admission, fence, budget, deadline, backoff and reissue exactly as `remove`. A reissue
    /// resends the whole chunk; a key the failed attempt already deleted is absent, and absence is
    /// success. Throws when the policy is exhausted. More keys than the cap is a caller bug: the
    /// consumer chunks, so that every chunk that succeeded is recorded before a later one can fail.
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, const Retry & policy);
```
`CA/Backend/CasRequests.cpp`, after `removeUnder`:
```cpp
void CasOperation::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, const Retry & policy)
{
    if (keys.size() > kBulkDeleteMaxKeys)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS removeManyWriteOnce: {} keys in one chunk, the limit is {}; the consumer chunks its input",
            keys.size(), kBulkDeleteMaxKeys);
    if (keys.empty())
        return;
    const Retry::Bound bound = policy.bind(owner.now_ms());
    const String subject = fmt::format("{} (+{} keys)", keys.front().str(), keys.size() - 1);
    readLoop("removeManyWriteOnce", subject, policy, bound, [&](auto & access)
    {
        owner.backend->removeManyWriteOnce(keys, access);
        return true;
    });
}
```
`readLoop` classifies the attempt's exception the way every read verb does (`refreshAndClassifyReadFault`); a `Poco::TimeoutException` and an S3 `SlowDown` are transient and reissued, a `LOGICAL_ERROR` is not.

- [ ] **Step 3: The setting**

`CA/ContentAddressedSettings.cpp`, after the `gc_read_concurrency` line (`:79`):
```cpp
    DECLARE(UInt64, gc_bulk_delete_chunk_keys, 1000, "Keys per batch delete request in GC's write-once families (owner-removed manifest bodies, covered ref logs and snapshots); 1 to 1000", 0) \
```
In the validation block (`:227-233`), add a second check:
```cpp
    if (settings[ContentAddressedSetting::gc_bulk_delete_chunk_keys] == 0
        || settings[ContentAddressedSetting::gc_bulk_delete_chunk_keys] > 1000)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "content_addressed disk: gc_bulk_delete_chunk_keys must be between 1 and 1000 (got {})",
            settings[ContentAddressedSetting::gc_bulk_delete_chunk_keys].value);
```
`CA/Pool/CasPool.h`, after `manifest_sweep_delete_budget_keys` (`:90`): `uint64_t gc_bulk_delete_chunk_keys = 1000;` with the comment `/// Keys per batch delete request for the write-once families; tests lower it to exercise chunk boundaries.` `CA/ContentAddressedMetadataStorage.cpp`: the `extern const ContentAddressedSettingsUInt64 gc_bulk_delete_chunk_keys;` line next to `:70` and `, gc_bulk_delete_chunk_keys(settings_[ContentAddressedSetting::gc_bulk_delete_chunk_keys].value)` next to `:290`. Add a settings test in `src/Disks/tests/gtest_cas_settings.cpp` following its existing shape: `1001` throws `BAD_ARGUMENTS`, `1` is accepted.

- [ ] **Step 4: Build, run the three bulk-delete suites, run the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t8.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASBulkDelete*:CASWriteOnceKey.*:CASSettings*' > test_wok_t8_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t8_gate.log 2>&1; echo GTEST_EXIT=$?
```

- [ ] **Step 5: Commit Tasks 7 and 8 together**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/tests/cas_test_helpers.h src/Disks/tests/gtest_cas_part_write.cpp src/Disks/tests/gtest_cas_decommission.cpp src/Disks/tests/gtest_cas_bulk_delete_backend.cpp src/Disks/tests/gtest_cas_bulk_delete_engine.cpp src/Disks/tests/gtest_cas_settings.cpp
git diff --cached --stat
git commit -m "ca-backend: removeManyWriteOnce — one batch delete of up to 1000 write-once keys through every backend and the engine

Keys are WriteOnceKey, mintable only by Layout for manifests, ref logs and ref snapshots. Native
mode issues one DeleteObjects under the control-plane profile; the emulated single-process mode
deletes under its lock with the same token bookkeeping as the single-key delete. The engine sends
one chunk under the ordinary attempt loop and reissues the whole chunk on a fault. New setting
gc_bulk_delete_chunk_keys (1 to 1000) sizes the consumers' chunks.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/tests/cas_test_helpers.h src/Disks/tests/gtest_cas_part_write.cpp src/Disks/tests/gtest_cas_decommission.cpp src/Disks/tests/gtest_cas_bulk_delete_backend.cpp src/Disks/tests/gtest_cas_bulk_delete_engine.cpp src/Disks/tests/gtest_cas_settings.cpp
git branch --show-current
```

---

### Task 9: C5 — `manifest_deletes` consumes the verb

Implements spec §C "Consumer".

**Files:**
- Modify: `CA/Gc/CasGc.cpp:1083-1122` (the phase), `:104-113` (`removalName` untouched; a new literal outcome)
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:48` and `docs/en/operations/system-tables/cas_gc_log.md:47` (column text)
- Create: `src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp`

**Interfaces:**
- Consumes: `CasOperation::removeManyWriteOnce`, `PoolConfig::gc_bulk_delete_chunk_keys`, `kBulkDeleteMaxKeys` (Task 8), `Layout::writeOnceManifestKey` (Task 5), `InMemoryBackend::bulkRemoveCalls`, `onBeforeBulkRemove` (Task 7).
- Produces: phase metrics `attempted`, `accepted`, `requests`, `suppressed` on the `manifest_deletes` row; `ManifestDelete` events with outcome `deleted_or_absent`; `RoundReport::manifests_deleted` counts accepted keys.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp`. The seeding of an owner-removed manifest copies `RoundSummaryCountsManifestBodyDeletes` (`src/Disks/tests/gtest_cas_gc_round.cpp:843-865`): write a blob body and a manifest, `publishCommittedTransition` it under a table name, then the drop helper on the same table (read the exact signature of `dropRefTransition` at `cas_test_helpers.h:491` and of `publishCommittedTransition` at `:474` before writing the calls).

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>

/// The manifest_deletes phase sends owner-removed manifest bodies to the store in chunks of
/// write-once keys, one request per chunk, and records every chunk that succeeded before a later
/// one can fail.

namespace ProfileEvents
{
    extern const Event CASBulkDeleteRequests;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");
const RootNamespace kNs{"00/aa@cas@"};

ManifestRef ref(uint64_t seq)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = 1};
}

/// `count` tables, each with one manifest, published committed and then dropped, so the fold sees
/// `count` owner removals and `mf_cleanup` carries `count` bodies.
std::vector<ManifestId> seedDroppedManifests(Backend & backend, const Layout & layout, uint64_t count)
{
    std::vector<ManifestId> ids;
    for (uint64_t i = 1; i <= count; ++i)
    {
        const ManifestRef r = ref(i);
        writeBlobBody(backend, layout, DB::UInt128(0x1000 + i));
        writeManifestRaw(backend, layout, kNs, r, {blobEntryFor("a", DB::UInt128(0x1000 + i))});
        const String table = "t" + std::to_string(i);
        publishCommittedTransition(backend, layout, kNs, table, std::nullopt, r);
        dropRefTransition(backend, layout, kNs, table, r);   /// adjust to the helper's real signature
        ids.push_back(ManifestId{kNs, r});
    }
    return ids;
}

/// Runs rounds until every listed manifest is gone or `max_rounds` passed; returns the sum of
/// `manifests_deleted` over the rounds that led.
uint64_t reclaim(Gc & gc, PoolPtr store, Backend & backend, const std::vector<ManifestId> & ids, size_t max_rounds)
{
    uint64_t total = 0;
    for (size_t round = 0; round < max_rounds; ++round)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        if (rep.acquired_lease)
            total += rep.manifests_deleted;
        store->renewWatermarkOnce();
        bool any_left = false;
        OperationForTest op(backend);
        for (const ManifestId & id : ids)
            any_left |= (*op).head(store->layout().manifestKey(id), Retry::once()).has_value();
        if (!any_left)
            break;
    }
    return total;
}

}

TEST(CASGCManifestBulkDelete, FiveBodiesInChunksOfTwoAreThreeRequests)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0, .gc_bulk_delete_chunk_keys = 2});
    const auto ids = seedDroppedManifests(*backend, store->layout(), 5);
    const auto requests_before = ProfileEvents::global_counters[ProfileEvents::CASBulkDeleteRequests].load();

    Gc gc(store, kGc);
    const uint64_t deleted = reclaim(gc, store, *backend, ids, 16);

    EXPECT_EQ(deleted, 5u);
    EXPECT_EQ(backend->bulkRemoveCalls(), 3u) << "2 + 2 + 1";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASBulkDeleteRequests].load() - requests_before, 3u);
    OperationForTest op(*backend);
    for (const ManifestId & id : ids)
        EXPECT_FALSE((*op).head(store->layout().manifestKey(id), Retry::once()).has_value());
}

TEST(CASGCManifestBulkDelete, AThrowInTheSecondChunkKeepsTheFirstChunksAuditAndAbortsTheRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0, .gc_bulk_delete_chunk_keys = 2});
    const auto ids = seedDroppedManifests(*backend, store->layout(), 5);

    std::vector<std::map<String, UInt64>> manifest_phase_rows;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "manifest_deletes")
            manifest_phase_rows.push_back(rec.metrics);
    });
    /// Rounds until the fold has adopted the removals; the first round whose manifest_deletes phase
    /// has work is the one the fault is armed for.
    size_t calls = 0;
    backend->onBeforeBulkRemove([&]
    {
        if (++calls == 2)
            throw Poco::TimeoutException("injected into the second chunk, every attempt");
    });
    /// The hook throws on EVERY attempt of the second chunk, so the engine's policy is exhausted and
    /// the round aborts; the counter keeps climbing across the reissues, which is why the arm is
    /// "second call and later" rather than "exactly the second call".
    backend->onBeforeBulkRemove([&]
    {
        if (++calls >= 2)
            throw Poco::TimeoutException("injected into the second chunk, every attempt");
    });

    bool aborted = false;
    for (size_t round = 0; round < 16 && !aborted; ++round)
    {
        try
        {
            static_cast<void>(runRegularRoundReclaiming(gc));
        }
        catch (const Poco::Exception &)
        {
            aborted = true;
        }
        catch (const DB::Exception &)
        {
            aborted = true;
        }
        store->renewWatermarkOnce();
    }
    ASSERT_TRUE(aborted);
    ASSERT_FALSE(manifest_phase_rows.empty());
    /// The aborted round's row was never emitted (the phase threw), so the last emitted row belongs
    /// to an earlier, empty round; what proves the first chunk's audit survived is the store: exactly
    /// the first chunk's two bodies are gone.
    OperationForTest op(*backend);
    size_t gone = 0;
    for (const ManifestId & id : ids)
        gone += !(*op).head(store->layout().manifestKey(id), Retry::once()).has_value();
    EXPECT_EQ(gone, 2u);
}

TEST(CASGCManifestBulkDelete, ASuppressedRoundMakesNoRequest)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0});
    const auto ids = seedDroppedManifests(*backend, store->layout(), 3);
    Gc gc(store, kGc);
    for (size_t round = 0; round < 4; ++round)
    {
        static_cast<void>(gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::StageA_Suppressed));
        store->renewWatermarkOnce();
    }
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
    OperationForTest op(*backend);
    for (const ManifestId & id : ids)
        EXPECT_TRUE((*op).head(store->layout().manifestKey(id), Retry::once()).has_value());
}
```
In the second test keep only ONE `onBeforeBulkRemove` registration (the `>= 2` one); the first is shown to explain why. The `manifest_phase_rows` capture stays as evidence in the failure message if the store check fails. If `runRegularRound` swallows the exception into an `Aborted` outcome instead of throwing, assert `rep.outcome` (read `RoundReport` in `CA/Gc/CasGc.h`) instead of catching.

- [ ] **Step 2: Build and run to verify they fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t9a.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASGCManifestBulkDelete.*' > test_wok_t9_fail.log 2>&1; echo GTEST_EXIT=$?
```
Expected: the first test fails on `bulkRemoveCalls() == 0` (the phase still uses `op.remove` per key).

- [ ] **Step 3: Rewrite the phase**

Replace the body of the `manifest_deletes` block (`CA/Gc/CasGc.cpp:1085-1120`) with:

```cpp
        GcPhaseTimer t(phase_sink, "manifest_deletes");
        const uint64_t manifests_deleted_before = report.manifests_deleted;
        /// GATED. A manifest body is content the ref graph still describes until its decrements are both
        /// sealed AND taken on a round that could prove its frontier -- an unprovable round's `-1` may
        /// itself be the observation that is missing an owner elsewhere, so deleting the body on it is
        /// exactly the irreversible step the gate exists to withhold.
        static const std::map<ManifestId, Etag> kNoManifestCleanup;
        const std::map<ManifestId, Etag> & mf_cleanup_now =
            suppress_destructive ? kNoManifestCleanup : folded.mf_cleanup;

        /// Chunks of write-once keys, one request each, with no per-key precondition: a manifest key is
        /// never written twice, so the body at it is the one the fold observed or nothing. The engine
        /// reissues a failed chunk whole; a chunk that exhausts its policy throws here, and the chunks
        /// before it are already recorded below. The etag the fold observed rides the event as
        /// information only.
        const size_t chunk_keys = std::clamp<size_t>(store->poolConfig().gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys);
        uint64_t attempted = 0;
        uint64_t requests = 0;
        std::vector<WriteOnceKey> chunk;
        std::vector<const std::pair<const ManifestId, Etag> *> chunk_entries;
        const auto flush = [&]
        {
            if (chunk.empty())
                return;
            op.removeManyWriteOnce(chunk, Retry::standard());
            ++requests;
            for (const auto * entry : chunk_entries)
            {
                ++report.manifests_deleted;
                EventEmitter{*store}.emit([&](CasEvent & e)
                {
                    e.type = CasEventType::ManifestDelete;
                    e.namespace_ = entry->first.root_namespace.string();
                    e.object_kind = CasEventObjectKind::Manifest;
                    e.object_hash = manifestRefDebugString(entry->first.ref);
                    e.token = entry->second.render();
                    e.round = new_round;
                    e.gen = generation;
                    e.outcome = "deleted_or_absent";
                    e.reason = "owner-removed manifest body; batch delete of a write-once key after decrements adopted";
                });
            }
            chunk.clear();
            chunk_entries.clear();
        };
        for (const auto & entry : mf_cleanup_now)
        {
            ++attempted;
            chunk.push_back(layout.writeOnceManifestKey(entry.first));
            chunk_entries.push_back(&entry);
            if (chunk.size() >= chunk_keys)
                flush();
        }
        flush();
        t.metric("attempted", attempted);
        t.metric("accepted", report.manifests_deleted - manifests_deleted_before);
        t.metric("requests", requests);
        t.metric("suppressed", suppress_destructive ? 1 : 0);
```
Add `#include <algorithm>` if `std::clamp` is not visible. The phase comment above the block (`:1074-1082`) stays; append one sentence: "The bodies go in batch requests of write-once keys; see the block."

- [ ] **Step 4: The column text and the user page**

`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:48`: `"Owner-removed manifest bodies deleted or found already absent this round (a batch delete of write-once keys cannot tell the two apart), counted separately from blob deletes."`. `docs/en/operations/system-tables/cas_gc_log.md:47`: the same sentence after the type. Grep `utils/ca-soak` and `docs/` for a reader of the old `deleted` metric on the `manifest_deletes` row (`grep -rn "manifest_deletes" utils/ca-soak docs/en | grep -i deleted`) and rename to `accepted` where found.

- [ ] **Step 5: Build, run, gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t9b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASGCManifestBulkDelete.*:CASGCRound.*:CASGCLog*' > test_wok_t9_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t9_gate.log 2>&1; echo GTEST_EXIT=$?
```
`CASGCRound.RoundSummaryCountsManifestBodyDeletes` must still pass: it asserts `manifests_deleted >= 1`, which accepted keys satisfy.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Interpreters/ContentAddressedGarbageCollectionLog.cpp docs/en/operations/system-tables/cas_gc_log.md src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp
git diff --cached --stat
git commit -m "ca-gc: manifest_deletes sends owner-removed bodies in batch requests of write-once keys

One DeleteObjects per chunk instead of one conditional delete per manifest; every chunk that
succeeded is recorded before a later one can fail. manifests_deleted now counts keys deleted or
found already absent, which a batch response cannot tell apart; the column text says so.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Interpreters/ContentAddressedGarbageCollectionLog.cpp docs/en/operations/system-tables/cas_gc_log.md src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp
git branch --show-current
```

---

### Task 10: C6 — the wire test on RustFS

Implements spec §C "Tests", wire part.

**Files:**
- Create: `tests/integration/test_cas_gc_bulk_delete/__init__.py` (empty), `configs/storage_conf.xml` (copy of `tests/integration/test_cas_gc_s3/configs/storage_conf.xml` with the endpoint path `cas_gc_bulk/` and the policy name `cas_gc_bulk`), `test.py`

**Interfaces:** none.

- [ ] **Step 1: Write the test**

`tests/integration/test_cas_gc_bulk_delete/test.py`:

```python
import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "cas_gc_bulk"
MANIFESTS_PREFIX = "cas_gc_bulk/cas/manifests/"
RECLAIM_RETRIES = 90
RECLAIM_SLEEP = 1.0


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def list_manifests():
    return [
        o.object_name
        for o in cluster.rustfs_client.list_objects(cluster.rustfs_bucket, MANIFESTS_PREFIX, recursive=True)
    ]


def gc_rows(node, phase):
    node.query("SYSTEM FLUSH LOGS")
    return node.query(
        f"SELECT phase_metrics['attempted'], phase_metrics['accepted'], phase_metrics['requests'], "
        f"ProfileEvents['CASBulkDeleteRequests'], ProfileEvents['S3DeleteObjects'] "
        f"FROM system.cas_gc_log WHERE event_type = 'Phase' AND phase = '{phase}' "
        f"AND phase_metrics['attempted'] > 0 ORDER BY event_time"
    ).strip().splitlines()


def test_manifest_deletes_go_in_one_request_and_absent_keys_are_success():
    node = cluster.instances["node"]
    node.query("DROP TABLE IF EXISTS t SYNC")
    node.query(
        f"CREATE TABLE t (k UInt64, v String) ENGINE = MergeTree ORDER BY k "
        f"SETTINGS storage_policy = '{STORAGE_POLICY}'"
    )
    for i in range(6):
        node.query(f"INSERT INTO t SELECT number, toString(number) FROM numbers(1000) SETTINGS max_insert_block_size = 1000")
    manifests_before = list_manifests()
    assert len(manifests_before) >= 6

    # One manifest is removed out from under GC: the batch must report it as accepted, not fail.
    cluster.rustfs_client.remove_object(cluster.rustfs_bucket, manifests_before[0])

    node.query("DROP TABLE t SYNC")

    rows = []
    for _ in range(RECLAIM_RETRIES):
        node.query("SYSTEM CAS GC RUN")
        rows = gc_rows(node, "manifest_deletes")
        if rows:
            break
        time.sleep(RECLAIM_SLEEP)
    assert rows, "no manifest_deletes phase with work within the wait"

    attempted, accepted, requests, bulk_requests, s3_deletes = (int(x) for x in rows[0].split("\t"))
    assert attempted >= 6
    assert accepted == attempted, "the key removed out from under GC is accepted as absent"
    assert requests == 1, f"{attempted} keys fit one chunk"
    assert bulk_requests == requests
    assert s3_deletes == requests, "one DeleteObjects per chunk, no singular fallback"

    for _ in range(RECLAIM_RETRIES):
        if not list_manifests():
            break
        node.query("SYSTEM CAS GC RUN")
        time.sleep(RECLAIM_SLEEP)
    assert not list_manifests()
```
Read `tests/integration/test_cas_gc_s3/test.py` for the exact way it waits on rounds and adjust: the `SYSTEM CAS GC RUN` statement name and whether the phase's `ProfileEvents['S3DeleteObjects']` lands on the row (the counter is on the round thread, so it does). The `remove_object` call is the RustFS client's; confirm the method name on `cluster.rustfs_client`.

- [ ] **Step 2: Run it**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
python3 -m ci.praktika run "integration" --test test_cas_gc_bulk_delete > build/test_wok_t10_integration.log 2>&1; echo PRAKTIKA_EXIT=$?
```
The binary praktika runs is `ci/tmp/clickhouse`; build `clickhouse` in `build/` first and symlink it there as the CAS praktika recipe says (`docs/superpowers/cas/AGENTS.md` §3). Never run this while a ca-soak is live on the host. Have a subagent read the log and report pass or the failing assertion.

- [ ] **Step 3: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- tests/integration/test_cas_gc_bulk_delete/__init__.py tests/integration/test_cas_gc_bulk_delete/configs/storage_conf.xml tests/integration/test_cas_gc_bulk_delete/test.py
git diff --cached --stat
git commit -m "ca-tests: integration test — manifest_deletes is one DeleteObjects per chunk on RustFS and an absent key is accepted

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- tests/integration/test_cas_gc_bulk_delete/__init__.py tests/integration/test_cas_gc_bulk_delete/configs/storage_conf.xml tests/integration/test_cas_gc_bulk_delete/test.py
git branch --show-current
```

---

### Task 11: D — ref-object cleanup in revalidated cohorts

Implements spec §D. Lands only after the review gate in Step 6.

**Files:**
- Modify: `CA/Gc/CasGc.cpp:3490-3600` (`cleanupRefObjects`)
- Modify: `src/Disks/tests/gtest_cas_ref_gc.cpp:540-665` (the race backend and the four authority tests; two new tests; the oracle)

**Interfaces:**
- Consumes: `CasOperation::removeManyWriteOnce`, `PoolConfig::gc_bulk_delete_chunk_keys`, `kBulkDeleteMaxKeys` (Task 8), `Layout::writeOnceRefLogKey`, `writeOnceRefSnapshotKey` (Task 5), `InMemoryBackend::onBeforeBulkRemove` (Task 7).
- Produces: `cleanupRefObjects` issues, per namespace, one revalidation (catalog GET, `gc/state` GET) per chunk and one batch delete per chunk; no `HEAD`; `CASRefCleanupObjectsDeleted` increments by the chunk size.

- [ ] **Step 1: Rework the race backend and write the failing tests**

In `src/Disks/tests/gtest_cas_ref_gc.cpp`, `RefCleanupAuthorityRaceBackend`:

1. `Authority` gains `CatalogRebirth`; `Timing` gains `DuringChunk`.
2. Add the override:
```cpp
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, DB::Cas::TransportAccess & access) override
    {
        const bool names_first = std::any_of(keys.begin(), keys.end(),
            [&](const WriteOnceKey & key) { return key.str() == first_cleanup_key; });
        if (armed && timing == Timing::DuringChunk && names_first)
            moveAuthority(access);   /// after the revalidation, before the deletes land
        CountingBackend::removeManyWriteOnce(keys, access);
        if (armed && timing == Timing::AfterFirstDelete && names_first)
            moveAuthority(access);
    }
```
The `head` override's `BeforeFirstDelete` seam no longer fires (the cleanup issues no `HEAD`); move that seam to `read`: when armed with `BeforeFirstDelete` and `key == gc_state_key` (the revalidation's second read), call `moveAuthority` AFTER returning the bytes the cleanup will compare, i.e. capture the result, move, return the captured result. Read the existing `head` override and mirror it on `read`.
3. `moveAuthority` gains the rebirth branch:
```cpp
        if (authority == Authority::CatalogRebirth)
        {
            RefCatalog catalog = decodeRefCatalog(bytes);
            for (CatalogEntry & entry : catalog.entries)
                if (entry.ns == reborn_ns)
                    entry.incarnation = entry.incarnation + 1;
            bytes = encodeRefCatalog(catalog);
        }
```
with a `RootNamespace reborn_ns` member set by `arm`. Read `decodeRefCatalog`'s exact name and signature at `CA/Formats/CasRefCatalogFormat.h:116-125`.

Then:

- The two `BetweenKeys` tests become `BetweenChunks`: open the pool with `Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_bulk_delete_chunk_keys = 1})` and keep their assertions (first key gone, second retained).
- The two `BeforeFirstDelete` tests keep their assertions; their seam moved to `read` of `gc/state`.
- New:
```cpp
TEST(CASRefGcCleanupAuthority, LeaseMoveDuringAChunkLetsTheChunkCompleteAndNothingElse)
{
    auto backend = std::make_shared<RefCleanupAuthorityRaceBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0, .gc_bulk_delete_chunk_keys = 1});
    const Layout & layout = store->layout();
    const RefCleanupFixture keys = seedTwoCoveredLogs(*backend, layout, RootNamespace{"00/aa@cas@"});
    backend->arm(RefCleanupAuthorityRaceBackend::Authority::GcFence,
                 RefCleanupAuthorityRaceBackend::Timing::DuringChunk, layout, keys.first_log_key);

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    OperationForTest raw_op(*backend);
    EXPECT_FALSE((*raw_op).head(keys.first_log_key, Retry::once()).has_value()) << "the chunk in flight completes";
    EXPECT_TRUE((*raw_op).head(keys.second_log_key, Retry::once()).has_value()) << "the next chunk's revalidation refuses";
    EXPECT_EQ(backend->deleteCount(keys.first_log_key), 1u);
    EXPECT_EQ(backend->deleteCount(keys.second_log_key), 0u);
}

TEST(CASRefGcCleanupAuthority, RebirthDuringAChunkDeletesOnlyTheOldLifesKeys)
{
    auto backend = std::make_shared<RefCleanupAuthorityRaceBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0, .gc_bulk_delete_chunk_keys = 1});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const RefCleanupFixture keys = seedTwoCoveredLogs(*backend, layout, ns);
    backend->arm(RefCleanupAuthorityRaceBackend::Authority::CatalogRebirth,
                 RefCleanupAuthorityRaceBackend::Timing::DuringChunk, layout, keys.first_log_key, ns);

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    OperationForTest raw_op(*backend);
    EXPECT_FALSE((*raw_op).head(keys.first_log_key, Retry::once()).has_value());
    EXPECT_TRUE((*raw_op).head(keys.second_log_key, Retry::once()).has_value());
    /// Whatever the reborn life owns is untouched: its keys carry a different life id and were never
    /// in the cohort. Every key under the new life's stream prefix is still present.
    const CasRefCatalog::Snapshot cut = CasRefCatalog::read(*raw_op, layout);
    const auto entry = std::find_if(cut.catalog.entries.begin(), cut.catalog.entries.end(),
                                    [&](const CatalogEntry & e) { return e.ns == ns; });
    ASSERT_NE(entry, cut.catalog.entries.end());
    const NamespaceLifeId reborn = NamespaceLifeId::fromCatalogEntry(entry->ns, entry->incarnation);
    ListPage page = (*raw_op).list(layout.namespaceStreamPrefix(reborn), "", 1000, Retry::once());
    for (const ListedKey & listed : page.keys)
        EXPECT_EQ(backend->deleteCount(listed.key), 0u) << listed.key;
}

TEST(CASRefGc, RefObjectCleanupDeletesExactlyThePlannedSet)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    /// Seeding: copy lines 398-456 of this file (`RefObjectCleanupRetainsCheckpointNamedTriple`) verbatim
    /// up to its `Gc gc(store, kGc);` line. That fixture defines the life, the listing of covered logs
    /// and snapshots, the durable cursor, the checkpoint id and the retained-seal proof; bind them to
    /// `life`, `listing`, `durable_cursor`, `checkpoint_snapshot_id`, `retained_log_proof` here.
    /// Before the round: compute the plan the same way the pass does.
    OperationForTest op(*backend);
    const RefCleanupPlan plan = planRefCleanup(listing, durable_cursor, checkpoint_snapshot_id, retained_log_proof);
    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    for (const RefTxnId & id : plan.deletable_logs)
        EXPECT_FALSE((*op).head(layout.refLogKey(life, id), Retry::once()).has_value());
    for (const RefTxnId & id : plan.deletable_snapshots)
        EXPECT_FALSE((*op).head(layout.refSnapshotKey(life, id), Retry::once()).has_value());
    /// and every listed key NOT in the plan is present
}
```
Fill the oracle test's seeding and the "not in the plan" loop from the fixture at `:398-456`, which already builds `listing`, `life` and the checkpoint. The `arm` overload with a namespace is `arm(authority, timing, layout, first_key, RootNamespace ns)`.

- [ ] **Step 2: Build and run to verify the new tests fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t11a.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASRefGcCleanupAuthority.*:CASRefGc.RefObjectCleanup*' > test_wok_t11_fail.log 2>&1; echo GTEST_EXIT=$?
```
Expected: the `DuringChunk` tests fail (no bulk call happens yet), the `BetweenChunks` ones still pass on the per-key path.

- [ ] **Step 3: Rewrite `cleanupRefObjects`**

Replace the `deleteRefObject` lambda and the two delete loops (`CA/Gc/CasGc.cpp` from `const auto deleteRefObject = [&](const String & key)` through the end of the snapshots loop) with:

```cpp
        /// Every irreversible delete is still licensed by the SAME complete catalog observation and GC
        /// lease that adopted the fold, re-read immediately before the deletes it licenses. The unit
        /// of licence is one chunk of write-once keys: a `_log` or `_snap` key is published once at a
        /// life-qualified key and never rewritten, so there is nothing to HEAD, and the plan below is
        /// derived from durable state a successor leader derives too. A moved incarnation, changed
        /// row/life, missing or unreadable authority object, or changed owner/sequence stops the whole
        /// cleanup pass; continuing with another chunk would turn a refusal into a fallback.
        const auto authorityHolds = [&](const String & first_key) -> bool
        {
            try
            {
                const CasRefCatalog::Snapshot current_catalog = CasRefCatalog::read(op, layout);
                current_catalog.life_index.throwIfAmbiguous("CAS GC ref cleanup revalidation");
                const auto current_entry_it = std::lower_bound(
                    current_catalog.catalog.entries.begin(), current_catalog.catalog.entries.end(), ns,
                    [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
                const std::optional<NamespaceLifeId> current_life
                    = current_catalog.life_index.resolve(life.incarnation);
                if (current_catalog.etag != folded.catalog_cut->etag
                    || current_entry_it == current_catalog.catalog.entries.end()
                    || current_entry_it->ns != ns || *current_entry_it != observed_entry
                    || !current_life || *current_life != life)
                {
                    LOG_DEBUG(logger,
                        "CAS GC ref cleanup stopped before the chunk starting at '{}': catalog observation/life moved",
                        first_key);
                    return false;
                }

                const auto current_state_object = op.read(layout.gcStateKey(), Retry::standard());
                if (!current_state_object)
                {
                    LOG_WARNING(logger,
                        "CAS GC ref cleanup stopped before the chunk starting at '{}': mandatory gc/state is absent",
                        first_key);
                    return false;
                }
                const GcState current_state = decodeGcState(current_state_object->bytes);
                if (current_state.lease.owner != adopted_lease.owner
                    || current_state.lease.seq != adopted_lease.seq)
                {
                    LOG_DEBUG(logger,
                        "CAS GC ref cleanup stopped before the chunk starting at '{}': GC fence moved", first_key);
                    return false;
                }
            }
            catch (const std::exception & e)
            {
                LOG_WARNING(logger,
                    "CAS GC ref cleanup stopped before the chunk starting at '{}': authority revalidation failed: {}",
                    first_key, e.what());
                return false;
            }
            return true;
        };

        /// (the `row_it`/`durable_cursor`, checkpoint and `retained_log_proof` derivation stays exactly as it is)

        const RefCleanupPlan plan = planRefCleanup(
            listing, durable_cursor, checkpoint_snapshot_id, retained_log_proof);
        std::vector<WriteOnceKey> cohort;
        cohort.reserve(plan.deletable_logs.size() + plan.deletable_snapshots.size());
        for (const RefTxnId & log_id : plan.deletable_logs)
            cohort.push_back(layout.writeOnceRefLogKey(life, log_id));
        for (const RefTxnId & snap_id : plan.deletable_snapshots)
        {
            /// The snapshot the checkpoint names is the one a recovering reader will sample, so it must
            /// survive every cleanup that the same checkpoint authorized.
            chassert(snap_id < checkpoint_snapshot_id);
            cohort.push_back(layout.writeOnceRefSnapshotKey(life, snap_id));
        }

        const size_t chunk_keys = std::clamp<size_t>(store->poolConfig().gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys);
        for (size_t begin = 0; begin < cohort.size(); begin += chunk_keys)
        {
            /// Cumulative per-round cap in KEYS, exactly as before; a chunk is cut to what remains. The
            /// plan recomputes the same remaining candidates from durable state next round, so nothing
            /// here needs its own cursor.
            if (!work_budget.refCleanupAvailable())
                return;
            size_t end = std::min(cohort.size(), begin + chunk_keys);
            if (work_budget.max_ref_cleanup_objects != 0)
                end = std::min(end, begin + (work_budget.max_ref_cleanup_objects - work_budget.ref_cleanup_objects_used));
            std::vector<WriteOnceKey> chunk(cohort.begin() + begin, cohort.begin() + end);
            if (!authorityHolds(chunk.front().str()))
                return;
            op.removeManyWriteOnce(chunk, Retry::standard());
            work_budget.ref_cleanup_objects_used += chunk.size();
            ProfileEvents::increment(ProfileEvents::CASRefCleanupObjectsDeleted, chunk.size());   /// cleanup object deletion
        }
```
`life`, `ns`, `observed_entry`, `folded`, `adopted_lease` are the names already in scope in the function.

- [ ] **Step 4: Build, run, gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_wok_t11b.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CASRefGc*' > test_wok_t11_suite.log 2>&1; echo GTEST_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t11_gate.log 2>&1; echo GTEST_EXIT=$?
```
`CASRefGc.RefObjectCleanupRespectsRoundBudgetAndConvergesAcrossRounds` (`:457`) asserts the per-round key cap; the chunk truncation above preserves it.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Disks/tests/gtest_cas_ref_gc.cpp
git diff --cached --stat
git commit -m "ca-gc: ref-object cleanup deletes covered logs and snapshots in revalidated cohorts

One catalog and gc/state revalidation per chunk of write-once keys, then one batch delete; no
per-key HEAD. The authority window widens from one key to one chunk: what it can delete is this
life's keys named by this round's durable plan, which a successor derives too.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_0124yU8tjzfrEGtNdZhS4TnV" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp src/Disks/tests/gtest_cas_ref_gc.cpp
git branch --show-current
```

- [ ] **Step 6: The safety review, before this commit may be merged**

Dispatch a `ca-review` agent (opus, high) with: the spec's §D and §Invariants, the diff of this task's commit, and the question "does the widened window admit deleting any key a successor leader's plan would retain, or any key of a reborn life?". A `MAJOR` finding blocks the merge of this task only; Tasks 0 to 10 are independent of it and merge on their own gate.

---

### Task 12: Gate, measure on GCS, record

**Files:**
- Modify: `docs/superpowers/specs/2026-09-04-cas-gc-immutable-key-round-cost-design.md` (placements; measured rows), `docs/superpowers/cas/BACKLOG.md` (measured rows), `utils/ca-soak/scenarios/RUN_HISTORY.md` (the run row)

- [ ] **Step 1: The whole gate on the committed HEAD**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master && git status --short | grep -v '^??'
cd build && ninja > build_wok_t12_full.log 2>&1; echo NINJA_EXIT=$?
./src/unit_tests_dbms --gtest_filter='CAS*' > test_wok_t12_gate.log 2>&1; echo GTEST_EXIT=$?
```
The tracked-modified list must be empty (a green suite after an uncommitted edit is evidence about a different tree). Subagent reads both logs.

- [ ] **Step 2: The 15-minute GCS soak with this binary**

Follow the latest GCS row in `utils/ca-soak/scenarios/RUN_HISTORY.md` for the image build and the driver invocation (the image recipe is the one that row used; `docker compose down -v` first). When the run ends, on the leader:

```sql
WITH fin AS (SELECT round_id, round FROM system.cas_gc_log WHERE event_type = 'Finish')
SELECT fin.round AS r, p.phase, round(p.phase_duration_microseconds / 1e6, 1) AS sec,
       p.ProfileEvents['S3GetObject'] AS get, p.ProfileEvents['S3HeadObject'] AS head,
       p.ProfileEvents['S3DeleteObjects'] AS del, p.ProfileEvents['CASGCGet'] AS cas_get,
       p.ProfileEvents['CASBulkDeleteRequests'] AS bulk, p.ProfileEvents['CASGCReadAheadHit'] AS hit,
       p.ProfileEvents['CASGCReadAheadWasted'] AS wasted, p.phase_metrics AS pm
FROM system.cas_gc_log AS p INNER JOIN fin ON p.round_id = fin.round_id
WHERE p.event_type = 'Phase'
  AND p.phase IN ('fold_reduce', 'manifest_deletes', 'ref_object_cleanup', 'orphan_sweep', 'fold_ref_intake')
ORDER BY r, p.event_time FORMAT PrettyCompactNoEscapes
```
Check against the spec's falsification list: `CASGCGet` on the reduce row is no longer about three times `listed`; `floor_lookups` equals the namespaces on the page; `manifest_deletes` shows `requests` equal to ⌈attempted/1000⌉ and `bulk` equal to `requests`; `ref_object_cleanup` shows two GETs per chunk and one `del`; the sweep's `wasted` is at most one window per crossing.

- [ ] **Step 3: Record**

In the spec: replace the "Expected effect, predictions" table's cells with the measured seconds, mark the status line `IMPLEMENTED` with the commit range, and add the three placement adjustments from this plan's Global Constraints to a short "Implementation record" section. In the BACKLOG entry: the measured rows. In `RUN_HISTORY.md`: the run row in the file's table format. Commit the three by path with one message.

- [ ] **Step 4: Hand back**

Report to the user: the branch, the commit list, the gate result with its log paths, the measured table against the predicted one, and whether Task 11's review returned a `MAJOR`.
