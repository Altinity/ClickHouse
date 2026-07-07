# Dangling-Precommit Manifest Orphan Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the content-addressed GC from permanently orphaning a part-manifest whose owning precommit was abandoned (never promoted, never removed) on a content-static ref-shard that the fold Skip optimization parks forever.

**Architecture:** Fold-side, watermark-keyed force-Read. `ShardCoverage` records the minimal live precommit binding of a shard; `computeDiscoverDecisions` overrides a would-be `Skip` to `Read` when that precommit is provably dead by the namespace watermark, so the existing `reclaimAbandonedPrecommit` runs, emits the owner-removal, the fold folds the `-1`, and R6 deletes the manifest. The guard is the direct sibling of the existing classification-`4` (clamped) guard and is self-terminating (one forced revisit per abandoned precommit).

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`), GoogleTest (`unit_tests_dbms`), protobuf (`cas_format.proto`), TLA+/TLC (`docs/superpowers/models`).

## Global Constraints

- Design spec: `docs/superpowers/specs/2026-07-07-cas-dangling-precommit-manifest-orphan-fix-design.md` — every task implicitly serves it.
- Allman braces (opening brace on its own line); enforced by CI style check.
- Build into `build/` (NOT `build_asan`). Redirect ninja output to a log in `build/` and have a subagent summarize it. Never pass `-j` or `nproc` to ninja.
- Unit test binary: `build/src/unit_tests_dbms`. Redirect each test run to a unique `build/test_<name>.log`; a subagent summarizes.
- TLA+: run from `docs/superpowers/models/` via `./run_gc_partmanifest.sh <cfg-basename>`; TLC jar at `tmp/tla2tools.jar` (v2.19).
- Pre-release, no persisted CA data: **zero compatibility scaffolding**. New codec fields default cleanly; decode stays **fail-closed** (bad magic / malformed protobuf throws `CORRUPTED_DATA`).
- Say "exception", not "crash".
- Commit on branch `cas-gc-rebuild` (never `master`); add new commits (no rebase/amend). Commit-message trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```
- In commit messages/comments/docs, wrap literal SQL/class/function names in `code`; write a function as `f`, not `f()`.
- `git add` only the specific files each task lists — never `git add -A`/`-u`.

---

## File Structure

- `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` (+ two new `.cfg`) — TLA+ gate (Task 0). Extend the canonical part-manifest model with one sabotage flag + one liveness property.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — the `isPrecommitDead` helper, the `minLivePrecommit` stamping in `Gc::fold`, and the force-Read override in `Gc::computeDiscoverDecisions`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h` / `CasGenerationSeal.cpp` — the new `ShardCoverage` fields + codec.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto` — the three new `FoldShardCoverageProto` fields.
- `src/Common/ProfileEvents.cpp` — the `CasGcPrecommitRevisitForced` counter.
- `src/Disks/tests/gtest_cas_dangling_precommit.cpp` (new) — RED + idempotency + skip-preserved unit tests.
- `utils/ca-soak/scenarios/cards/s28_s33_corner.py` (S30) — scenario regression (Task 6, run only).
- `docs/superpowers/cas/06-tla-models.md`, `utils/ca-soak/scenarios/BACKLOG.md` — docs (Task 6).

---

## Task 0: TLA+ gate — `SkipParksDeadPrecommit`

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_skipparksdeadprecommit.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_fix_skipparksdeadprecommit.cfg`

**Interfaces:**
- Produces: a TLC-checkable liveness property `LiveDeadPrecommitReclaimed` and a sabotage flag `SabotageSkipParksDeadPrecommit` gating the discover force-Read behavior.

**Context:** The model already carries the token-diff skip (`EnableTokenDiff`, `SabotageSkipChangedShard`), precommit lifecycle (`WPrecommitAdd`), the orphan sweep (`EnableOrphanSweep`), and the per-namespace watermark death judgment (the same fact `prefixEligible` uses). We add ONE flag and ONE property. The bug config must produce a counterexample; the fix config must hold.

- [ ] **Step 1: Add the sabotage flag to the CONSTANTS block**

In the `CONSTANTS` list (near the other `Sabotage*` skip flags, e.g. `SabotageSkipChangedShard` around line 26) add:

```tla
    SabotageSkipParksDeadPrecommit,  \* TRUE = discover still SKIPs a token-stable shard even when it holds a live precommit the watermark has proven dead (the shipped bug); the reclaim never runs and the manifest orphans
```

- [ ] **Step 2: Model the discover force-Read in the skip decision**

Find the discover/skip predicate that decides whether a token-stable shard is skipped (the one guarded by `EnableTokenDiff` and broken by `SabotageSkipChangedShard`). A shard is skip-eligible when its listed token equals the sealed folded token. Add a conjunct so that, unless sabotaged, a shard holding a watermark-dead live precommit is NOT skipped (forced to be re-folded so its reclaim can run). Concretely, where the model computes "may skip shard `s` of namespace `n`", replace the skip-eligibility with:

```tla
\* A token-stable shard is skip-eligible ONLY when it does not hold a live precommit the watermark
\* has already proven dead. SabotageSkipParksDeadPrecommit drops this conjunct = the shipped bug
\* (the static shard is parked forever and its dead precommit is never reclaimed).
CanSkipShard(n, s) ==
    /\ TokenStable(n, s)
    /\ (SabotageSkipParksDeadPrecommit \/ ~HasDeadLivePrecommit(n, s))

\* A live (un-removed, un-promoted) precommit binding on this shard whose build is DEAD by the
\* namespace watermark (older epoch, retired sentinel, or build_seq below min_active) — the exact
\* death fact reclaimAbandonedPrecommit/prefixEligible use.
HasDeadLivePrecommit(n, s) ==
    \E m \in ManifestIds :
        /\ owner[m] \in Builds            \* still a precommit owner (never promoted/removed)
        /\ ShardOf(m) = s /\ NsOf(m) = n
        /\ BuildDead(n, m)                \* the same watermark death predicate the sweep uses
```

Reuse the model's existing "build is dead by the watermark" predicate (the one behind the orphan sweep's eligibility / `prefixEligible`); if it is inlined, extract it as `BuildDead(n, m)` so the skip guard and the sweep share it. Use the model's existing accessors for owner/shard/namespace (`owner[m]`, and whatever the model already uses for a manifest's shard and namespace — match the existing names; do not invent `ShardOf`/`NsOf` if the model already spells them differently).

- [ ] **Step 3: Add the liveness property**

Near the other properties/invariants, add:

```tla
\* LIVENESS: a present, unreachable, watermark-dead abandoned precommit manifest is EVENTUALLY reclaimed
\* (its body deleted). Under the fix, the forced re-fold runs reclaimAbandonedPrecommit; under the bug the
\* static shard is parked and the body stutters present forever.
LiveDeadPrecommitReclaimed ==
    \A m \in ManifestIds :
        [] ( ( mBody[m] /\ owner[m] \in Builds /\ BuildDead(NsOf(m), m) )
             => <> (m \in mfDeleted \/ owner[m] \notin Builds) )
```

Match the model's real variable names for "body present" (`mBody`/`present`) and "deleted bodies" (`mfDeleted`) — both appear in the `VARIABLES` block. If the model's fairness is not already strong enough for the fold/reclaim to be forced, add weak fairness on the fold/reclaim action to `Spec` (mirror how `NoLeakForever`-style liveness is made checkable in the sibling models, e.g. `CaResurrectLiveness.tla`).

- [ ] **Step 4: Write the two cfg files**

Copy `CaGcRootLocalPartManifestCore_sab_skipchangedshard.cfg` verbatim to both new cfgs, then in EACH:
- add the new constant line `SabotageSkipParksDeadPrecommit = TRUE` (bug cfg) / `= FALSE` (fix cfg);
- keep `SabotageSkipChangedShard = FALSE` in BOTH (we are isolating the new behavior);
- replace `INVARIANT INV_NO_DANGLE` with `PROPERTY LiveDeadPrecommitReclaimed` (liveness, not a safety invariant), keeping `SPECIFICATION Spec`, the `CONSTRAINT StateConstraint`, and `CHECK_DEADLOCK FALSE` lines.

Name them `CaGcRootLocalPartManifestCore_sab_skipparksdeadprecommit.cfg` and `..._fix_skipparksdeadprecommit.cfg`.

- [ ] **Step 5: Run TLC — bug cfg MUST violate**

Run: `cd docs/superpowers/models && ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_skipparksdeadprecommit`
Expected: TLC reports `LiveDeadPrecommitReclaimed` **violated** (a temporal counterexample / lasso: the dead precommit's body stutters present forever). `exit` non-zero.

- [ ] **Step 6: Run TLC — fix cfg MUST hold**

Run: `cd docs/superpowers/models && ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_fix_skipparksdeadprecommit`
Expected: `Model checking completed` with no violation; `exit=0`.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_skipparksdeadprecommit.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_fix_skipparksdeadprecommit.cfg
git commit  # message: "CAS TLA+: SkipParksDeadPrecommit gate — bug cfg violates LiveDeadPrecommitReclaimed, fix holds" + the two trailers
```

---

## Task 1: RED unit test — dangling precommit orphaned on a static shard

**Files:**
- Create: `src/Disks/tests/gtest_cas_dangling_precommit.cpp`
- Reference (read-only): `src/Disks/tests/cas_test_helpers.h`, `src/Disks/tests/gtest_cas_gc_leak.cpp`

**Interfaces:**
- Consumes (harness helpers, verified present in `cas_test_helpers.h`): `writeManifestRaw(backend, layout, ns, ...) -> ManifestId`; `addPrecommitTransition(backend, layout, ns, build_id, final_ref_name, old_ref, new_ref, shard) -> uint64_t`; `setWatermarkMinActive(backend, layout, server_root_id, writer_epoch, min_active)`; the store/GC open helper used by `gtest_cas_gc_leak.cpp` (e.g. `openTestStore` + `Gc gc(store, ...)`); `gc.runRegularRound()`.
- Produces: `TEST(CasDanglingPrecommit, AbandonedPrecommitOrphansManifestUntilFix)`.

**Context:** The bug only reproduces when the token-diff Skip engages, which requires `backend.supportsListTokens()` — `InMemoryBackend` returns true, so assert it to document the dependency. The namespace whose watermark governs is rooted at a `server_root_id` prefix of `ns` (see `floorForNamespace`); `setWatermarkMinActive` seeds the `MountLease` at `mountKey(server_root_id)`. Build a namespace whose `server_root_id` is unambiguous (e.g. `ns = RootNamespace{"srv/tbl@cas@"}` with `server_root_id = "srv"`), and pass that same `server_root_id` to `setWatermarkMinActive`.

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;

/// An abandoned precommit (never promoted, never removed) on a content-static ref-shard is parked by the
/// fold Skip optimization; once the watermark advances past its build_sequence it is PROVABLY dead, yet
/// reclaimAbandonedPrecommit never re-runs for the parked shard, so its manifest orphans. Fixed in Task 4.
TEST(CasDanglingPrecommit, AbandonedPrecommitOrphansManifestUntilFix)
{
    auto backend = std::make_shared<InMemoryBackend>();
    ASSERT_TRUE(backend->supportsListTokens()) << "repro needs the token-diff Skip to engage";
    auto store = openTestStore(backend);   /// same opener gtest_cas_gc_leak.cpp uses
    const Layout & layout = store->layout();

    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};
    const uint64_t shard = 0;
    const uint64_t dead_epoch = 1;
    const uint64_t precommit_seq = 5;   /// build_sequence of the abandoned precommit

    /// A precommit manifest whose body is PRESENT (so it activates and folds), then NEVER promoted/removed.
    const ManifestId id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = dead_epoch, .build_sequence = precommit_seq, .manifest_ordinal = 1});
    addPrecommitTransition(*backend, layout, ns, /*build_id*/ hexToU128("000000000000000000000000000000aa"),
        /*final_ref_name*/ "all_0_0_0", /*old_ref*/ std::nullopt, id.ref, shard);

    /// Precommit still ALIVE (min_active <= precommit_seq): fold it in, seal coverage, do NOT reclaim.
    setWatermarkMinActive(*backend, layout, server_root_id, dead_epoch, /*min_active*/ precommit_seq);
    Gc gc(store, hexToU128("00000000000000000000000000000101"));
    gc.runRegularRound();
    ASSERT_TRUE(backend->head(layout.manifestKey(id)).exists) << "body present while precommit alive";

    /// Watermark advances PAST the precommit (other builds retired): it is now provably dead.
    setWatermarkMinActive(*backend, layout, server_root_id, dead_epoch, /*min_active*/ precommit_seq + 1);

    /// Drive several rounds. The shard is token-stable => Skipped => reclaim never runs.
    for (int i = 0; i < 5; ++i)
        gc.runRegularRound();

    /// BUG (pre-fix): the manifest body is orphaned — still present, never reclaimed.
    /// After Task 4 this assertion is INVERTED (see Task 4 Step 4).
    EXPECT_TRUE(backend->head(layout.manifestKey(id)).exists)
        << "PRE-FIX: dangling precommit manifest is orphaned (Skip parks the shard, reclaim never runs)";
}
```

- [ ] **Step 2: Add the test file to the unit-test build**

Confirm `src/Disks/tests/gtest_cas_dangling_precommit.cpp` is picked up (the tests dir globs `gtest_*.cpp`; if there is an explicit list, add it). Verify by configuring: the file must appear in the `unit_tests_dbms` sources.

- [ ] **Step 3: Build the unit test binary**

Run (redirect + subagent-summarize per Global Constraints):
```bash
ninja -C build unit_tests_dbms > build/build_dangling_precommit_t1.log 2>&1
```
Expected: exit 0.

- [ ] **Step 4: Run the test — verify it PASSES pre-fix (documents the bug)**

```bash
build/src/unit_tests_dbms --gtest_filter='CasDanglingPrecommit.AbandonedPrecommitOrphansManifestUntilFix' > build/test_dangling_precommit_t1.log 2>&1
```
Expected: PASS — the `EXPECT_TRUE(...exists)` holds, proving the orphan reproduces (the manifest is NOT reclaimed). This is the "RED" state: the test encodes the buggy behavior; Task 4 inverts the assertion to the fixed behavior.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/tests/gtest_cas_dangling_precommit.cpp
git commit  # "CAS test: reproduce dangling-precommit manifest orphan (Skip parks static shard)" + trailers
```

---

## Task 2: Extract the shared `isPrecommitDead` helper

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` (near `floorForNamespace` at ~L1955 and `reclaimAbandonedPrecommit` at ~L1979)

**Interfaces:**
- Produces (file-local free function in `CasGc.cpp`): `bool isPrecommitDead(uint64_t writer_epoch, uint64_t build_sequence, const MountLease & w)`.

**Context:** `reclaimAbandonedPrecommit` currently inlines the death judgment (~L2026–2031). Extract it verbatim so the Task 4 discover override and reclaim share ONE predicate (no drift). Pure refactor — no behavior change.

- [ ] **Step 1: Add the helper above `reclaimAbandonedPrecommit`**

```cpp
/// A precommit binding is provably DEAD by the durable namespace watermark FACT (control #9) — never a
/// frozen-seq / judged-dead guess. Dead iff its incarnation's writer_epoch is older than the live mount
/// epoch, the mount carries the farewell/retired sentinel (min_active == UINT64_MAX = every seq retired),
/// or its build_sequence is below the live floor. A future epoch or a build at/above the floor is NOT dead.
/// Shared by reclaimAbandonedPrecommit (which acts on it) and computeDiscoverDecisions (which forces a
/// re-fold on it) so the two can never disagree.
bool isPrecommitDead(uint64_t writer_epoch, uint64_t build_sequence, const MountLease & w)
{
    if (writer_epoch > w.writer_epoch)
        return false;
    if (writer_epoch < w.writer_epoch)
        return true;
    if (w.min_active == std::numeric_limits<uint64_t>::max())
        return true;
    return w.min_active > build_sequence;
}
```

- [ ] **Step 2: Rewrite the death judgment in `reclaimAbandonedPrecommit` to call it**

Replace the inlined block (the `if (binding.manifest_ref.writer_epoch > w.writer_epoch) continue;` … `const bool is_dead = …;`) with:

```cpp
        if (binding.owner_kind != OwnerKind::Precommit)
            continue;
        const bool retired_sentinel = w.min_active == std::numeric_limits<uint64_t>::max();
        if (isPrecommitDead(binding.manifest_ref.writer_epoch, binding.manifest_ref.build_sequence, w))
            dead.push_back(DeadPrecommit{binding, retired_sentinel});
```

Keep `retired_sentinel` (the `DeadPrecommit` struct still records it). Behavior is identical.

- [ ] **Step 3: Build**

```bash
ninja -C build unit_tests_dbms > build/build_dangling_precommit_t2.log 2>&1
```
Expected: exit 0.

- [ ] **Step 4: Run the existing GC unit suites — no regressions**

```bash
build/src/unit_tests_dbms --gtest_filter='CasGc*:CasBlobIndegree*:CasFsck*:CasRetireView*:CasDanglingPrecommit*' > build/test_dangling_precommit_t2.log 2>&1
```
Expected: all PASS (including the Task 1 test still PASS — no behavior change yet).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp
git commit  # "CAS gc: extract shared isPrecommitDead helper (no behavior change)" + trailers
```

---

## Task 3: Record the minimal live precommit in `ShardCoverage`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp`
- Test: `src/Disks/tests/gtest_cas_dangling_precommit.cpp`

**Interfaces:**
- Produces: `ShardCoverage` fields `bool has_live_precommit`, `uint64_t min_live_precommit_writer_epoch`, `uint64_t min_live_precommit_build_sequence`, round-tripped through `encodeFoldSeal`/`decodeFoldSeal`.

**Context:** Purely additive scaffolding. The fields are written/read but nothing consumes them yet (Task 4 does), so no behavior changes. No compatibility scaffolding (pre-release); decode stays fail-closed.

- [ ] **Step 1: Add the fields to `ShardCoverage`** (`CasGenerationSeal.h`, in `struct ShardCoverage`, after `incarnation`)

```cpp
    /// The lexicographically-minimal {writer_epoch, build_sequence} among this shard's LIVE precommit
    /// owner bindings (un-promoted, un-removed) at fold time; has_live_precommit == false when there are
    /// none. Consumed by computeDiscoverDecisions to force a re-fold once the watermark proves this
    /// precommit dead, so reclaimAbandonedPrecommit runs even on an otherwise token-stable (Skip) shard.
    bool has_live_precommit = false;
    uint64_t min_live_precommit_writer_epoch = 0;
    uint64_t min_live_precommit_build_sequence = 0;
```

- [ ] **Step 2: Add the proto fields** (`cas_format.proto`, in `message FoldShardCoverageProto`, after field 7)

```proto
  bool   has_live_precommit                 = 8;  // shard holds >=1 live (un-promoted, un-removed) precommit
  uint64 min_live_precommit_writer_epoch    = 9;  // lexicographically-min live precommit writer_epoch
  uint64 min_live_precommit_build_sequence  = 10; // lexicographically-min live precommit build_sequence
```

- [ ] **Step 3: Encode/decode the fields** (`CasGenerationSeal.cpp`)

In `encodeFoldSeal`, inside the `per_ns_shard` loop, after `set_incarnation_build_sequence`:
```cpp
        e->set_has_live_precommit(cov.has_live_precommit);
        e->set_min_live_precommit_writer_epoch(cov.min_live_precommit_writer_epoch);
        e->set_min_live_precommit_build_sequence(cov.min_live_precommit_build_sequence);
```
In `decodeFoldSeal`, extend the `ShardCoverage{...}` aggregate:
```cpp
            .incarnation = ShardIncarnation{e.incarnation_writer_epoch(), e.incarnation_build_sequence()},
            .has_live_precommit = e.has_live_precommit(),
            .min_live_precommit_writer_epoch = e.min_live_precommit_writer_epoch(),
            .min_live_precommit_build_sequence = e.min_live_precommit_build_sequence()};
```

- [ ] **Step 4: Write the round-trip test** (append to `gtest_cas_dangling_precommit.cpp`)

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>

TEST(CasDanglingPrecommit, ShardCoverageRoundTripsMinLivePrecommit)
{
    CasFoldSeal seal;
    seal.generation = 3;
    seal.parent_generation = 2;
    ShardCoverage cov;
    cov.classification = 1;
    cov.folded_cursor = 7;
    cov.has_live_precommit = true;
    cov.min_live_precommit_writer_epoch = 1;
    cov.min_live_precommit_build_sequence = 5;
    seal.per_ns_shard["srv/tbl@cas@/0"] = cov;

    const CasFoldSeal back = decodeFoldSeal(encodeFoldSeal(seal));
    const ShardCoverage & r = back.per_ns_shard.at("srv/tbl@cas@/0");
    EXPECT_TRUE(r.has_live_precommit);
    EXPECT_EQ(r.min_live_precommit_writer_epoch, 1u);
    EXPECT_EQ(r.min_live_precommit_build_sequence, 5u);

    /// Default (no live precommit) round-trips as absent.
    CasFoldSeal empty_seal;
    empty_seal.per_ns_shard["srv/tbl@cas@/1"] = ShardCoverage{};
    const CasFoldSeal e_back = decodeFoldSeal(encodeFoldSeal(empty_seal));
    EXPECT_FALSE(e_back.per_ns_shard.at("srv/tbl@cas@/1").has_live_precommit);
}
```

- [ ] **Step 5: Regenerate protobuf + build**

```bash
ninja -C build unit_tests_dbms > build/build_dangling_precommit_t3.log 2>&1
```
Expected: exit 0 (the proto is regenerated as part of the `clickhouse_cas_proto` target dependency).

- [ ] **Step 6: Run the round-trip test**

```bash
build/src/unit_tests_dbms --gtest_filter='CasDanglingPrecommit.ShardCoverageRoundTripsMinLivePrecommit' > build/test_dangling_precommit_t3.log 2>&1
```
Expected: PASS. Also rerun `CasDanglingPrecommit.AbandonedPrecommitOrphansManifestUntilFix` — still PASS (fields inert, bug unchanged).

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp \
        src/Disks/tests/gtest_cas_dangling_precommit.cpp
git commit  # "CAS gc: record minimal live precommit in ShardCoverage (scaffold)" + trailers
```

---

## Task 4: The fix — stamp in `fold`, force-Read in `computeDiscoverDecisions`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Modify: `src/Common/ProfileEvents.cpp`
- Test: `src/Disks/tests/gtest_cas_dangling_precommit.cpp`

**Interfaces:**
- Consumes: `isPrecommitDead` (Task 2), the `ShardCoverage` fields (Task 3), file-local `floorForNamespace(Store &, const RootNamespace &)` (`CasGc.cpp:1955`).
- Produces: the force-Read behavior + `ProfileEvents::CasGcPrecommitRevisitForced`.

**Context:** `Gc::fold` stamps `cov` at ~L789 (where `cov.folded_token`/`folded_cursor`/`incarnation` are set) after the shard journal has been read (and after `reclaimAbandonedPrecommit` at L733 already removed any dead precommit this round). `computeDiscoverDecisions` sets `Skip` at ~L1440 only after the classification-4 / absent / ambiguity guards; the override slots in just before that `Skip` assignment.

- [ ] **Step 1: Add the counter** (`src/Common/ProfileEvents.cpp`, next to `CasGcRetireReplaced`)

```cpp
    M(CasGcPrecommitRevisitForced, "CA gc forced re-folds of an otherwise-Skip (token-stable) ref-shard because it holds a live precommit the namespace watermark has proven dead — so reclaimAbandonedPrecommit runs and its orphaned manifest is reclaimed", ValueType::Number) \
```

Add the extern to `CasGc.cpp`'s `ProfileEvents` extern block (near `extern const Event CasGcRetireReplaced;`):
```cpp
    extern const Event CasGcPrecommitRevisitForced;
```

- [ ] **Step 2: Add a file-local `minLivePrecommit` helper in `CasGc.cpp`** (near `isPrecommitDead`)

```cpp
/// The lexicographically-minimal {writer_epoch, build_sequence} among the shard's LIVE precommit bindings
/// (owner-state replay: accumulate new_binding, drop old_binding; keep OwnerKind::Precommit survivors).
/// nullopt when the shard holds no live precommit. This is the SAME replay reclaimAbandonedPrecommit does.
std::optional<std::pair<uint64_t, uint64_t>> minLivePrecommit(const RootShard & root)
{
    std::vector<OwnerBinding> live;
    for (const RootOwnerEvent & e : root.journal)
    {
        if (e.old_binding)
            std::erase(live, *e.old_binding);
        if (e.new_binding)
            live.push_back(*e.new_binding);
    }
    std::optional<std::pair<uint64_t, uint64_t>> best;
    for (const OwnerBinding & b : live)
    {
        if (b.owner_kind != OwnerKind::Precommit)
            continue;
        const std::pair<uint64_t, uint64_t> cur{b.manifest_ref.writer_epoch, b.manifest_ref.build_sequence};
        if (!best || cur < *best)
            best = cur;
    }
    return best;
}
```

- [ ] **Step 3: Stamp the coverage in `fold`** (`CasGc.cpp` ~L789, where `cov` is populated on the Read path)

After the existing `cov.folded_cursor = …; cov.incarnation = root.incarnation;` assignments and before `result.fold_seal.per_ns_shard[cursor_key] = cov;`:

```cpp
        if (const auto mlp = minLivePrecommit(root))
        {
            cov.has_live_precommit = true;
            cov.min_live_precommit_writer_epoch = mlp->first;
            cov.min_live_precommit_build_sequence = mlp->second;
        }
```

(Leave the carried-forward Skip branch at ~L716 unchanged — it copies the whole `ShardCoverage`, so the field rides along verbatim.)

- [ ] **Step 4: Force-Read override in `computeDiscoverDecisions`** (`CasGc.cpp`, replace the final Skip assignment at ~L1440)

```cpp
        if (listed_it->second == sealed_it->second.folded_token)
        {
            /// Force-Read guard (sibling of the classification-4 clamped guard): a token-stable shard that
            /// holds a live precommit the namespace watermark has proven dead must be re-folded so
            /// reclaimAbandonedPrecommit runs — otherwise Skip parks the static shard forever and the
            /// abandoned precommit's manifest orphans (INV-2). Self-terminating: the reclaim's removal
            /// changes the shard token, so next round it is a normal changed->unchanged shard.
            if (sealed_it->second.has_live_precommit)
            {
                if (const auto floor = floorForNamespace(*store, ns);
                    floor && isPrecommitDead(sealed_it->second.min_live_precommit_writer_epoch,
                                             sealed_it->second.min_live_precommit_build_sequence, *floor))
                {
                    ProfileEvents::increment(ProfileEvents::CasGcPrecommitRevisitForced);
                    continue;   /// forced Read (already defaulted); do NOT mark Skip
                }
            }
            decisions[ck] = DiscoverDecision::Skip;
        }
```

- [ ] **Step 5: Invert the Task 1 assertion to the FIXED behavior**

In `gtest_cas_dangling_precommit.cpp`, change the final assertion of `AbandonedPrecommitOrphansManifestUntilFix` from `EXPECT_TRUE(...exists)` to:

```cpp
    /// FIXED: the watermark-dead precommit forces a re-fold => reclaimAbandonedPrecommit emits the removal
    /// => the fold folds the -1 => R6 deletes the owner-removed manifest body.
    EXPECT_FALSE(backend->head(layout.manifestKey(id)).exists)
        << "POST-FIX: dangling precommit manifest is reclaimed once the watermark proves it dead";
```

(Optionally rename the test to `AbandonedPrecommitReclaimedAfterWatermarkAdvances` — if renamed, update the `--gtest_filter` in later steps.)

- [ ] **Step 6: Build**

```bash
ninja -C build unit_tests_dbms > build/build_dangling_precommit_t4.log 2>&1
```
Expected: exit 0.

- [ ] **Step 7: Run — the fix makes the test GREEN**

```bash
build/src/unit_tests_dbms --gtest_filter='CasDanglingPrecommit.*' > build/test_dangling_precommit_t4.log 2>&1
```
Expected: all PASS — the reclaim now fires and the manifest is deleted.

- [ ] **Step 8: Regression — the broader GC suite still passes**

```bash
build/src/unit_tests_dbms --gtest_filter='CasGc*:CasBlobIndegree*:CasFsck*:CasRetireView*:CasReuseGcRace*:CasDanglingPrecommit*' > build/test_dangling_precommit_t4_regr.log 2>&1
```
Expected: all PASS.

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Common/ProfileEvents.cpp \
        src/Disks/tests/gtest_cas_dangling_precommit.cpp
git commit  # "CAS gc: force-Read a shard whose watermark-dead precommit Skip would park (fix dangling-precommit manifest orphan)" + trailers
```

---

## Task 5: Idempotency + skip-optimization-preserved tests

**Files:**
- Test: `src/Disks/tests/gtest_cas_dangling_precommit.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1–4; `ProfileEvents` counters (read via the global `ProfileEvents::global_counters` or the store's counter snapshot — match how `gtest_cas_gc_leak.cpp` reads counters); `gc.discoverDecisionsForTest()` (`CasGc.cpp:1447`) for a write-free decision probe.

**Context:** Two guarantees the reviewer must see proven: (a) the fix is idempotent — after reclaim, extra rounds neither re-reclaim nor churn; (b) the Skip optimization is preserved — a shard with no precommit, or a live-but-not-yet-dead precommit, is still `Skip` (no forced Read).

- [ ] **Step 1: Idempotency test**

```cpp
TEST(CasDanglingPrecommit, ReclaimIsIdempotentAndSelfTerminating)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openTestStore(backend);
    const Layout & layout = store->layout();
    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};
    const ManifestId id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1});
    addPrecommitTransition(*backend, layout, ns, hexToU128("000000000000000000000000000000aa"),
        "all_0_0_0", std::nullopt, id.ref, /*shard*/ 0);
    setWatermarkMinActive(*backend, layout, server_root_id, 1, 5);
    Gc gc(store, hexToU128("00000000000000000000000000000102"));
    gc.runRegularRound();
    setWatermarkMinActive(*backend, layout, server_root_id, 1, 6);   /// now dead
    for (int i = 0; i < 3; ++i)
        gc.runRegularRound();
    ASSERT_FALSE(backend->head(layout.manifestKey(id)).exists) << "reclaimed";

    /// Extra rounds after reclaim: no forced revisit (nothing left to reclaim), no exception, no re-delete.
    const uint64_t forced_before = /* read ProfileEvents::CasGcPrecommitRevisitForced snapshot */ 0;
    for (int i = 0; i < 3; ++i)
        gc.runRegularRound();
    const uint64_t forced_after = /* read the same counter */ 0;
    EXPECT_EQ(forced_after, forced_before) << "no further forced revisits after the shard is clean";
}
```

Wire the two counter reads to whatever snapshot accessor `gtest_cas_gc_leak.cpp` already uses for `CasGc*` counters (do not invent a new accessor). If no per-store counter snapshot exists, assert instead on `gc.discoverDecisionsForTest()` showing the shard is `Skip` after reclaim.

- [ ] **Step 2: Skip-preserved test**

```cpp
TEST(CasDanglingPrecommit, SkipPreservedForLivePrecommitAndForNoPrecommit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openTestStore(backend);
    const Layout & layout = store->layout();
    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};

    /// A LIVE (not-yet-dead) precommit: build_sequence 5, min_active 5 (5 is NOT below the floor => alive).
    const ManifestId id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1});
    addPrecommitTransition(*backend, layout, ns, hexToU128("000000000000000000000000000000aa"),
        "all_0_0_0", std::nullopt, id.ref, /*shard*/ 0);
    setWatermarkMinActive(*backend, layout, server_root_id, 1, 5);
    Gc gc(store, hexToU128("00000000000000000000000000000103"));
    gc.runRegularRound();   /// seals coverage with the live precommit

    /// The token-stable shard with a LIVE precommit must still be Skip (optimization preserved).
    const auto decisions = gc.discoverDecisionsForTest();
    EXPECT_EQ(decisions.at(cursorKeyForTest(ns, 0)), Gc::DiscoverDecision::Skip)
        << "a live (not-yet-dead) precommit must NOT force a re-fold";
    /// The manifest is NOT reclaimed while the precommit is alive.
    EXPECT_TRUE(backend->head(layout.manifestKey(id)).exists);
}
```

Match `Gc::DiscoverDecision` visibility (it is a public enum on `Gc`; if not, use the existing test accessor `discoverDecisionsForTest` return type). Use `cursorKeyForTest` (`cas_test_helpers.h:541`).

- [ ] **Step 3: Build + run**

```bash
ninja -C build unit_tests_dbms > build/build_dangling_precommit_t5.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasDanglingPrecommit.*' > build/test_dangling_precommit_t5.log 2>&1
```
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/tests/gtest_cas_dangling_precommit.cpp
git commit  # "CAS test: dangling-precommit reclaim idempotency + Skip-optimization-preserved" + trailers
```

---

## Task 6: S30 scenario regression + docs + finalize

**Files:**
- Run only: `utils/ca-soak` S30 scenario
- Modify: `docs/superpowers/cas/06-tla-models.md`, `utils/ca-soak/scenarios/BACKLOG.md`

**Context:** The `clickhouse` binary must be rebuilt so the ca-soak stand mounts the fix. The S30 card (`utils/ca-soak/scenarios/cards/s28_s33_corner.py`) already asserts `no unbounded leftovers`; with the fix it must lose the `_manifests` leak.

- [ ] **Step 1: Rebuild the server binary**

```bash
ninja -C build clickhouse > build/build_clickhouse_dangling_precommit.log 2>&1
```
Expected: exit 0 (subagent-summarize the log).

- [ ] **Step 2: Run S30 (hard reset remounts the fresh binary)**

```bash
cd utils/ca-soak && python3 -m scenarios.run --scenario S30 --scale dev > tmp/s30_dangling_precommit.log 2>&1
```
Expected: `S30 DONE: status=PASS`. Specifically the `no unbounded leftovers` verdict must be `pass` with `leak={}` (no `_manifests`), pipeline blobs allowed.

- [ ] **Step 3: If S30 still shows a `_manifests` leak, STOP and diagnose**

Do not paper over it. Decode the residual manifest's shard journal (ephemeral `mc` + the codec) as in the root-cause investigation and confirm whether it is the same abandoned-precommit shape (then the fix has a gap) or a different class (then log a new BACKLOG entry). Report before proceeding.

- [ ] **Step 4: Update `docs/superpowers/cas/06-tla-models.md`**

Add a short subsection under the part-manifest model notes recording the `SkipParksDeadPrecommit` gate (flag + `LiveDeadPrecommitReclaimed` property; bug cfg violates, fix cfg holds) and that the C++ fix landed (force-Read on a watermark-dead live precommit in `computeDiscoverDecisions`). Use `{#kebab-anchor}` headers.

- [ ] **Step 5: Mark the BACKLOG entry resolved**

In `utils/ca-soak/scenarios/BACKLOG.md`, under the `DANGLING-PRECOMMIT` root-cause note (S30 entry), append a `RESOLVED 2026-07-07` line pointing at the fix commits and the passing S30 run.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/cas/06-tla-models.md utils/ca-soak/scenarios/BACKLOG.md
git commit  # "docs(cas): dangling-precommit fix — S30 green, TLA+ gate + BACKLOG resolved" + trailers
```

---

## Self-Review

**Spec coverage:**
- Problem/mechanism → Task 1 (RED reproduces it) + Task 0 (TLA+ models it). ✓
- Fix (force-Read keyed on watermark-dead min live precommit) → Task 4. ✓
- What-we-store (minimal live precommit in `ShardCoverage`, carried on Skip) → Task 3 (fields+codec) + Task 4 Step 3 (stamped in fold; carried branch untouched). ✓
- `isPrecommitDead` shared helper → Task 2. ✓
- `CasGcPrecommitRevisitForced` + `PrecommitReclaim` now fires → Task 4 Step 1 + Task 5 (asserts the counter). ✓
- Safety (no live precommit reclaimed; Skip preserved; idempotency) → Task 5. ✓
- TLA+ gate → Task 0. ✓
- Testing incl. S30 regression → Task 1/5/6. ✓
- Out of scope (INTROSPECTION-1/2, sweep-side) → not tasked. ✓

**Placeholder scan:** the only intentionally-open spots are the two counter reads in Task 5 Step 1, which explicitly instruct the implementer to wire to the existing `gtest_cas_gc_leak.cpp` counter accessor (a named, existing pattern) or fall back to `discoverDecisionsForTest`; and the TLA+ accessor-name matching in Task 0 (the model's real variable spellings), which the implementer confirms against the model. No `TODO`/`TBD`.

**Type consistency:** `ShardCoverage.{has_live_precommit,min_live_precommit_writer_epoch,min_live_precommit_build_sequence}` are defined in Task 3 and consumed with the same names in Task 4. `isPrecommitDead(uint64_t, uint64_t, const MountLease &)` is defined in Task 2 and called with the same signature in Task 4. `minLivePrecommit(const RootShard &) -> optional<pair<uint64_t,uint64_t>>` defined and used in Task 4. `floorForNamespace` returns `std::optional<MountLease>` (`CasGc.cpp:1955`), dereferenced as `*floor` in Task 4. ✓
