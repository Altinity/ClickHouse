# Promote-over-committed leak + abandon-retire-ordering fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop `Build::promote` from silently overwriting a committed ref (orphaning the old manifest), make `republishRef` re-drives idempotent, and fix `Build::abandon`'s retire-before-removal ordering.

**Architecture:** Writer-side, fail-close. `promote` throws `ABORTED` when a committed ref already names a *different* manifest (restoring the model's `RefFreeFor` guard). `republishRef` becomes idempotent on the destination (skip publish when dst is already committed with the same content; `ABORTED` on a different-content conflict). `abandon` retires the build_seq only *after* its precommit-removal CAS.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`), GoogleTest (`unit_tests_dbms`), TLA+/TLC (`docs/superpowers/models`).

## Global Constraints

- Design spec: `docs/superpowers/specs/2026-07-08-cas-promote-over-committed-leak-fix-design.md`.
- **No `LOGICAL_ERROR`.** Refusal cases throw `ErrorCodes::ABORTED` (the code `promote` already uses for its fail-closed branches; `LOGICAL_ERROR` is CI-checked and reserved for must-not-happen invariants).
- Allman braces (opening brace on its own line); enforced by CI.
- Build into `build/` (NOT build_asan), **in the FOREGROUND**: run `ninja -C build unit_tests_dbms > build/<log> 2>&1` and WAIT for it to exit in the same step. **Never background the build / never use a background mechanism / never return before ninja exits.** No `-j`/`nproc`. Summarize the log; report a concise result.
- Unit test binary: `build/src/unit_tests_dbms`. Redirect each run to a unique `build/test_<name>.log`.
- TLA+: run from `docs/superpowers/models/` via `./run_gc_partmanifest.sh <cfg-basename>`; jar at `tmp/tla2tools.jar` (v2.19).
- Commit on branch `cas-gc-rebuild` (never master); add new commits (no rebase/amend); `git add` only the specific files each task lists (never `git add -A`). Commit-message trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```
- In commit messages/comments, wrap literal SQL/class/function names in `code`; write a function as `f`, not `f()`.
- **Sibling-cfg lesson (from the dangling-precommit fix):** adding a `CONSTANT` to `CaGcRootLocalPartManifestCore.tla` makes every cfg that does not assign it fail TLC fatally (`exit=151`). All 47 `CaGcRootLocalPartManifestCore*.cfg` must assign any new constant. Adding a new *invariant definition* does NOT break cfgs (invariants are opt-in per cfg via `INVARIANT`).

---

## File Structure

- `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` (+ cfgs) — Task 0 TLA+ gate.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` — `Build::promote` fail-close (Task 2), `Build::abandon` reorder (Task 4).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` — `republishRef` idempotency + `ABORTED` extern (Task 3).
- `src/Disks/tests/gtest_cas_promote_republish.cpp` (new) — unit tests for all three bugs.
- `utils/ca-soak/scenarios/`, `docs/superpowers/cas/06-tla-models.md`, `utils/ca-soak/scenarios/BACKLOG.md` — Task 5 (scenario + docs).

---

## Task 0: TLA+ gate — `AtMostOneCommittedManifestPerRef` + `SabotagePromoteOverwritesCommitted`

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Modify: all `docs/superpowers/models/CaGcRootLocalPartManifestCore*.cfg` (assign the new constant)
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_promoteoverwritescommitted.cfg`

**Interfaces:**
- Produces: invariant `AtMostOneCommittedManifestPerRef`; constant `SabotagePromoteOverwritesCommitted`.

**Context:** `WPromote` (`.tla:283-296`) and `WPublishCommitted` (`.tla:318-332`) already guard with `RefFreeFor(ref, m)` (`.tla:215-219`) — the model already forbids a ref owning two committed manifests (= BUG 1's fix). This task makes that property an explicit checked invariant and adds a negative control proving the guard is load-bearing.

- [ ] **Step 1: Add the invariant definition** (near the other `INV_*`/invariant defs). `owner[m] = r` for `r \in Refs` means m is committed-owned by ref r:

```tla
\* BUG 1 (promote-over-committed): a ref owns AT MOST ONE committed manifest. RefFreeFor (the WPromote /
\* WPublishCommitted guard) maintains this; SabotagePromoteOverwritesCommitted drops that guard = the
\* shipped C++ promote's silent overwrite, which leaves two committed bindings for one ref (the leaked T_old).
AtMostOneCommittedManifestPerRef ==
    \A r \in Refs : Cardinality({m \in ManifestIds : owner[m] = r}) <= 1
```

(Use the model's real `Cardinality` import — `FiniteSets` is already extended if `Cardinality` appears elsewhere; if not, add `Cardinality({...})` via the existing set-size idiom the model uses.)

- [ ] **Step 2: Add the sabotage constant** to the `CONSTANTS` block (near `SabotageSplitPromote`):

```tla
    SabotagePromoteOverwritesCommitted,  \* TRUE = WPromote/WPublishCommitted drop the RefFreeFor guard (the shipped C++ promote's silent overwrite of a committed ref) -> two committed manifests for one ref
```

- [ ] **Step 3: Weaken the guard under the sabotage** in `WPromote` (`.tla:286`) and `WPublishCommitted` (`.tla:321`): replace `/\ RefFreeFor(ref, m)` with `/\ (SabotagePromoteOverwritesCommitted \/ RefFreeFor(ref, m))` in BOTH.

- [ ] **Step 4: Assign the constant in ALL 47 sibling cfgs** (`= FALSE`), plus the new bug cfg. Do it mechanically:

```bash
cd docs/superpowers/models
for f in CaGcRootLocalPartManifestCore*.cfg; do
  grep -q "SabotagePromoteOverwritesCommitted" "$f" || \
    sed -i 's/^\( *\)SabotageSplitPromote = .*/&\n\1SabotagePromoteOverwritesCommitted = FALSE/' "$f"
done
```
Then create `CaGcRootLocalPartManifestCore_sab_promoteoverwritescommitted.cfg` by copying an existing invariant cfg (e.g. `CaGcRootLocalPartManifestCore_sab_twoowners.cfg`), setting `SabotagePromoteOverwritesCommitted = TRUE`, keeping all other `Sabotage* = FALSE`, and using `INVARIANT AtMostOneCommittedManifestPerRef` (plus `SPECIFICATION Spec`, `CONSTRAINT StateConstraint`, `CHECK_DEADLOCK FALSE`). Add `INVARIANT AtMostOneCommittedManifestPerRef` to `stage2`, `stage3` (or the smallest positive cfg that exercises promote/publish — `stage2` covers "owner transitions + precommit + promote") so the FIX side is checked too.

- [ ] **Step 5: Run TLC — bug cfg MUST violate**

Run: `cd docs/superpowers/models && ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_promoteoverwritescommitted`
Expected: `AtMostOneCommittedManifestPerRef` **violated** (a ref reaches two committed manifests), non-zero exit.

- [ ] **Step 6: Run TLC — fix side MUST hold**

Run: `cd docs/superpowers/models && ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage2`
Expected: `Model checking completed. No error has been found.` (`AtMostOneCommittedManifestPerRef` holds under the enforced `RefFreeFor`).

- [ ] **Step 7: Re-verify no masking (scoped).** Re-run the cfgs whose behavior the guard-weakening could touch — every cfg is now inert to it (constant `FALSE`), so a spot check suffices: run `CaGcRootLocalPartManifestCore_sab_twoowners` (must still violate `INV_NO_LOSS`) and `CaGcRootLocalPartManifestCore_stage0` (must still pass, no `exit=151`). Confirm no cfg reports "not assigned a value".

- [ ] **Step 8: Commit**

```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla docs/superpowers/models/CaGcRootLocalPartManifestCore*.cfg
git commit  # "CAS TLA+: AtMostOneCommittedManifestPerRef invariant + SabotagePromoteOverwritesCommitted control (promote-over-committed gate)" + trailers
```

---

## Task 1: RED unit tests — the two BUG-1 leaks + a placeholder BUG-2 (all failing/characterizing)

**Files:**
- Create: `src/Disks/tests/gtest_cas_promote_republish.cpp`
- Reference (read-only): `src/Disks/tests/gtest_cas_build.cpp` (`openStore`, `startBuildFor`, the write-flow helper ~L80-97), `src/Disks/tests/cas_test_helpers.h`, `CasManifestCodec.h` (`ManifestEntry`/`EntryPlacement`), `CasStore.h` (`resolveRef`→`std::optional<Resolved>{manifest_id, mutable_files}`, `readManifest`).

**Interfaces:**
- Produces: `TEST(CasPromoteRepublish, PromoteOverDifferentCommittedRefFailsClosed)`, `TEST(CasPromoteRepublish, RepublishReDriveOverCommittedDstIsIdempotent)`.

**Context:** Both tests drive the REAL `Build`/`Store` (and, for republish, the same `stageManifest`/`precommitAdd`/`promote`/`dropRef` primitives `republishRef` uses). Use INLINE manifest entries (`EntryPlacement::Inline`) so no real blobs need to exist (`promote`'s blob revalidation skips non-`Blob` entries). Construct two DISTINCT-content manifests by differing the inline bytes. Both tests fail (or throw-not-thrown) pre-fix and pass post-fix; they are inverted-assertion RED tests where noted.

- [ ] **Step 1: Write the BUG-1a test (fails pre-fix: promote does NOT throw)**

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Common/Exception.h>

using namespace DB::Cas;
namespace ErrorCodes { extern const int ABORTED; }   // ClickHouse ErrorCodes (test-local extern)

namespace
{
StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}
/// One inline-entry manifest with the given path+bytes (distinct bytes => distinct content).
std::vector<ManifestEntry> inlineEntries(const String & path, const String & bytes)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Inline;
    e.inline_data = bytes;   /// (field name per CasManifestCodec.h — verify exact spelling)
    return {e};
}
/// Publish a committed ref over `ref` naming a fresh manifest of `entries`. Returns its ManifestId.
ManifestId publishCommitted(const StorePtr & s, const RootNamespace & ns, const String & ref,
                            const std::vector<ManifestEntry> & entries)
{
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}
}

/// BUG 1a: promoting a DIFFERENT manifest onto an already-committed ref must fail closed (ABORTED),
/// not silently overwrite (which orphans the old manifest). Pre-fix promote does not throw => RED.
TEST(CasPromoteRepublish, PromoteOverDifferentCommittedRefFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    const String ref = "all_0_0_0";

    publishCommitted(s, ns, ref, inlineEntries("f", "AAA"));   // committed T_old

    auto build2 = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id2 = build2->stageManifest(inlineEntries("f", "BBB"));   // DIFFERENT content
    build2->precommitAdd(ns, ref, id2);

    try
    {
        build2->promote(ns, ref, build2->buildId(), id2);
        FAIL() << "PRE-FIX: promote silently overwrote a committed ref (leak); POST-FIX must throw ABORTED";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::ABORTED);
    }
}
```

- [ ] **Step 2: Write the BUG-1a idempotent-re-promote test (must pass pre- and post-fix)**

```cpp
/// Re-promoting the SAME manifest_ref onto its own committed ref must NOT throw (idempotent).
TEST(CasPromoteRepublish, PromoteSameManifestIsIdempotent)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    const String ref = "all_0_0_0";
    const ManifestId id = publishCommitted(s, ns, ref, inlineEntries("f", "AAA"));
    // Re-precommit + re-promote the SAME id onto the same ref: allowed (same manifest_ref).
    auto build2 = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    build2->precommitAdd(ns, ref, id);
    EXPECT_NO_THROW(build2->promote(ns, ref, build2->buildId(), id));
}
```

(If re-precommitting the same `id` is rejected earlier by an unrelated guard, the implementer instead asserts the same-manifest branch directly at the `promote` level; note any such adaptation in the report. The intent: the fix's guard must key on a *different* `manifest_ref`, not merely "ref already committed".)

- [ ] **Step 3: Build (FOREGROUND) + run — 1a test FAILS (RED), same-manifest test behavior noted**

```bash
ninja -C build unit_tests_dbms > build/build_promote_republish_t1.log 2>&1   # wait for exit
build/src/unit_tests_dbms --gtest_filter='CasPromoteRepublish.*' > build/test_promote_republish_t1.log 2>&1
```
Expected: `PromoteOverDifferentCommittedRefFailsClosed` FAILS (promote did not throw — the leak reproduces). Report whether `PromoteSameManifestIsIdempotent` passes today.

- [ ] **Step 4: Commit the RED tests**

```bash
git add src/Disks/tests/gtest_cas_promote_republish.cpp
git commit  # "CAS test: RED — promote-over-committed silent overwrite (fails: no fail-close)" + trailers
```

---

## Task 2: BUG 1a — `Build::promote` fail-close guard

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (`Build::promote` closure, ~L905 before the `root.refs[final_ref_name] = ...` write)
- Test: `src/Disks/tests/gtest_cas_promote_republish.cpp` (Task 1 tests turn GREEN)

**Interfaces:**
- Consumes: `RootShard::refs` (`std::map<String, RootRef>`; `RootRef::manifest_ref` is a `ManifestRef`), `id.ref` (the `ManifestRef` being promoted), `ErrorCodes::ABORTED` (already externed in `CasBuild.cpp:31`).

- [ ] **Step 1: Add the guard** in the `promote` `mutateShard` closure, immediately before `root.refs[final_ref_name] = RootRef{...}` (`CasBuild.cpp` ~L905):

```cpp
        /// BUG 1a: refuse to overwrite a live committed ref that already names a DIFFERENT manifest — that
        /// would orphan the old manifest (its owner-removal `-1` is never emitted). This enforces the
        /// model's `RefFreeFor` guard (WPromote requires it). A re-promote of the SAME manifest_ref is
        /// idempotent and allowed. Fail-closed with ABORTED (not LOGICAL_ERROR): a conflicting durable
        /// state the caller handles (republishRef is made idempotent so its legitimate re-drive skips
        /// promote entirely), never a must-not-happen invariant.
        if (const auto it = root.refs.find(final_ref_name);
            it != root.refs.end() && it->second.manifest_ref != id.ref)
            throw Exception(ErrorCodes::ABORTED,
                "promote: ref '{}' already names a different committed manifest — refusing to overwrite "
                "(unique-ref invariant; use republishRef for an intended repoint)", final_ref_name);
```

- [ ] **Step 2: Build (FOREGROUND) + run — 1a tests GREEN**

```bash
ninja -C build unit_tests_dbms > build/build_promote_republish_t2.log 2>&1   # wait for exit
build/src/unit_tests_dbms --gtest_filter='CasPromoteRepublish.PromoteOver*:CasPromoteRepublish.PromoteSame*' > build/test_promote_republish_t2.log 2>&1
```
Expected: `PromoteOverDifferentCommittedRefFailsClosed` PASS (throws `ABORTED`); `PromoteSameManifestIsIdempotent` PASS.

- [ ] **Step 3: Regression** (promote is on the hot commit path):

```bash
build/src/unit_tests_dbms --gtest_filter='CasBuild*:CasBuildReuseBlob*:CaTransaction*:CaWiring*:CasStore*:CasInlinePlacement*' > build/test_promote_republish_t2_regr.log 2>&1
```
Expected: all PASS. (If any test legitimately promotes over an existing committed ref with different content — it should not, given unique part names — investigate before proceeding; report it.)

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp
git commit  # "CAS build: promote fail-closes on a different pre-existing committed ref (fix promote-over-committed leak)" + trailers
```

---

## Task 3: BUG 1c — `republishRef` idempotent on the destination

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (`republishRef`, L143-176; add `ABORTED` to the ErrorCodes block L49-54)
- Test: `src/Disks/tests/gtest_cas_promote_republish.cpp`

**Interfaces:**
- Consumes: `store()->resolveRef(ns, ref)` → `std::optional<Resolved>` (`.manifest_id`, `.mutable_files`); `store()->readManifest(id)` → `PartManifest` (`.entries` is a path-sorted `std::vector<ManifestEntry>`); `store()->dropRef(ns, ref)`.

- [ ] **Step 1: Write the BUG-1c RED test** (append to `gtest_cas_promote_republish.cpp`). Simulate the post-crash state (src committed AND dst committed with the same content) by publishing both, then drive `republishRef` and assert idempotency:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
// (plus whatever gtest_ca_transaction.cpp includes to build a ContentAddressedTransaction over a Store)

/// BUG 1c: a republishRef re-drive where dst is ALREADY committed with the SAME content (the prior
/// attempt's promote landed; only dropRef(src) was interrupted) must be idempotent: skip the publish,
/// drop src, mint no new manifest, leave no orphan. Pre-fix it re-stages + re-promotes => leaks the
/// prior dst manifest => RED (a third manifest / orphaned dst manifest appears).
TEST(CasPromoteRepublish, RepublishReDriveOverCommittedDstIsIdempotent)
{
    // ... open Store + a ContentAddressedTransaction (mirror gtest_ca_transaction.cpp setup) ...
    const RootNamespace ns{"srv/tbl@cas@"};
    const auto entries = inlineEntries("f", "AAA");
    publishCommitted(s, ns, "src", entries);          // src committed (T_a-equivalent)
    publishCommitted(s, ns, "dst", entries);          // dst ALREADY committed, SAME content (prior attempt)
    const size_t manifests_before = /* count manifest objects under cas/manifests/ via the backend LIST */;

    const bool moved = txn.republishRef(ns, "src", ns, "dst");

    EXPECT_TRUE(moved);
    EXPECT_FALSE(s->resolveRef(ns, "src").has_value());               // src dropped (rename completed)
    EXPECT_TRUE(s->resolveRef(ns, "dst").has_value());                // dst still committed
    const size_t manifests_after = /* count again */;
    EXPECT_EQ(manifests_after, manifests_before)                      // NO third manifest minted
        << "PRE-FIX: re-drive re-staged a fresh manifest and orphaned the prior dst manifest";
}
```

(Counting manifest objects: LIST the backend under the manifests prefix, or assert via fsck that no unreachable manifest remains. The implementer picks the mechanism available in the transaction-test harness; the load-bearing assertion is "no new/orphaned manifest".)

- [ ] **Step 2: Add a conflict RED/GREEN test** (different content at dst → `ABORTED`):

```cpp
TEST(CasPromoteRepublish, RepublishOverCommittedDstDifferentContentFailsClosed)
{
    // ... publish src with "AAA", dst with "BBB" (DIFFERENT content) ...
    EXPECT_THROW(txn.republishRef(ns, "src", ns, "dst"), DB::Exception);   // ABORTED post-fix
}
```

- [ ] **Step 3: Build (FOREGROUND) + run — confirm 1c tests fail/behave pre-fix**

```bash
ninja -C build unit_tests_dbms > build/build_promote_republish_t3red.log 2>&1   # wait
build/src/unit_tests_dbms --gtest_filter='CasPromoteRepublish.Republish*' > build/test_promote_republish_t3red.log 2>&1
```
Expected: `RepublishReDriveOverCommittedDstIsIdempotent` FAILS pre-fix (a new manifest is minted / dst manifest orphaned; and note: pre-fix `promote` now throws ABORTED from Task 2 when it overwrites dst — so pre-Task-3 the re-drive may THROW rather than leak. Either way the test is not green pre-fix). Report the exact pre-fix behavior.

- [ ] **Step 4: Add `ABORTED` extern** to `ContentAddressedTransaction.cpp` ErrorCodes block (L49-54):

```cpp
    extern const int ABORTED;
```

- [ ] **Step 5: Implement the idempotency guard** — rewrite the top of `republishRef` (`ContentAddressedTransaction.cpp:152-155`):

```cpp
    auto resolved = metadata_storage.store()->resolveRef(src_ns, src_ref);
    if (!resolved)
        return false;
    const Cas::PartManifest src_manifest = metadata_storage.store()->readManifest(resolved->manifest_id);

    /// BUG 1c: idempotent re-drive. If dst is ALREADY committed, the prior attempt's promote landed and
    /// only dropRef(src) was interrupted. Compare CONTENT (path-sorted `entries`, not the whole manifest —
    /// ref/namespace/digest legitimately differ): same content => finish the rename by dropping src; a
    /// different-content dst is a genuine conflict => fail closed (never silently drop src's content).
    if (auto dst_resolved = metadata_storage.store()->resolveRef(dst_ns, dst_ref))
    {
        const Cas::PartManifest dst_manifest = metadata_storage.store()->readManifest(dst_resolved->manifest_id);
        if (dst_manifest.entries != src_manifest.entries)
            throw Exception(ErrorCodes::ABORTED,
                "republishRef: destination '{}' is already committed with different content — refusing "
                "(rename/attach conflict)", dst_ns.string() + "/" + dst_ref);
        metadata_storage.store()->dropRef(src_ns, src_ref);
        return true;
    }
```

(The existing `startBuild`/`stageManifest`/`precommitAdd`/`promote`/`dropRef` sequence below stays as the dst-absent path.)

- [ ] **Step 6: Build (FOREGROUND) + run — 1c tests GREEN**

```bash
ninja -C build unit_tests_dbms > build/build_promote_republish_t3.log 2>&1   # wait
build/src/unit_tests_dbms --gtest_filter='CasPromoteRepublish.*' > build/test_promote_republish_t3.log 2>&1
```
Expected: all `CasPromoteRepublish.*` PASS (idempotent re-drive; conflict throws `ABORTED`; and the re-drive no longer reaches `promote`, so it never hits Task 2's guard).

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_cas_promote_republish.cpp
git commit  # "CAS transaction: republishRef idempotent on an already-committed destination (fix rename/attach re-drive leak)" + trailers
```

---

## Task 4: BUG 2 — `Build::abandon` retire-after-removal

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (`Build::abandon`, L925-956)
- Test: `src/Disks/tests/gtest_cas_promote_republish.cpp`

**Interfaces:** none new.

- [ ] **Step 1: Write the ordering test** (append). Assert the precommit removal is committed to the shard before `retireBuildSeq` would let it be reclaimed — a focused test that after `abandon`, the shard journal already carries the removal (so a subsequent GC reclaim finds nothing live to double-remove):

```cpp
/// BUG 2: abandon must emit its precommit removal BEFORE retiring the build_seq, so GC's
/// reclaimAbandonedPrecommit can never observe a live-and-watermark-dead precommit to double-remove.
/// Assert the removal is present in the shard journal immediately after abandon() returns.
TEST(CasPromoteRepublish, AbandonEmitsRemovalBeforeRetire)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_0_0_0", .intended_namespace = ns});
    const ManifestId id = build->stageManifest(inlineEntries("f", "AAA"));
    build->precommitAdd(ns, "all_0_0_0", id);
    build->abandon();
    // The precommit-removal event (old=Precommit, new=none) is present in the target shard journal.
    // Read the shard and assert a removal of the precommit binding exists.
    // (Use the backend + decodeRootShard, or a cas_test_helpers.h reader, to inspect root.journal.)
    // EXPECT that some RootOwnerEvent has old_binding=Precommit(all_0_0_0,...) and new_binding=none.
}
```

(This test passes both before and after the reorder — the removal is emitted either way. Its value is as a regression guard that `abandon` still emits the removal after the reorder. The reorder's *ordering* correctness is covered structurally + by the TLA+ `WAbandonPrecommit` model; a live double-removal race is not deterministically reproducible in a unit test, so do not attempt a timing-based assertion — note this in the report.)

- [ ] **Step 2: Reorder `abandon`** — move `store->retireBuildSeq(build_seq);` from `CasBuild.cpp:929` to AFTER the `if (precommitted) { … }` block (after L956), keeping `alive = false;` at L930:

```cpp
void Build::abandon()
{
    requireAlive();
    alive = false;
    /// (removed the early retireBuildSeq here — see below)

    if (precommitted)
    {
        store->mutateShard(precommit_target_ns, store->shardOf(precommit_final_ref), MutationScope::ref(precommit_final_ref), [&](RootShard & root)
        {
            root.journal.push_back(RootOwnerEvent{ /* … precommit removal, unchanged … */ });
        }, nullptr, RootMutationOrigin::Writer, RootMutationKind::Abandon);
        precommitted = false;
    }

    /// BUG 2: retire the build_seq only AFTER the precommit removal is durably committed (mirrors
    /// `Build::promote`, which retires after its CAS). Retiring first lets `min_active` advance and GC's
    /// `reclaimAbandonedPrecommit` observe a live-and-dead precommit, racing a double removal. Idempotent.
    store->retireBuildSeq(build_seq);

    /// … best-effort staged-manifest debris cleanup (unchanged) …
}
```

Update the retire comment at the top of `abandon` accordingly (the old L928 comment moves down with the call).

- [ ] **Step 3: Build (FOREGROUND) + run**

```bash
ninja -C build unit_tests_dbms > build/build_promote_republish_t4.log 2>&1   # wait
build/src/unit_tests_dbms --gtest_filter='CasPromoteRepublish.Abandon*:CasBuild*:CasGc*:CasDanglingPrecommit*' > build/test_promote_republish_t4.log 2>&1
```
Expected: all PASS (abandon still emits the removal; no GC/dangling-precommit regression from the reorder).

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_promote_republish.cpp
git commit  # "CAS build: abandon retires build_seq after the precommit-removal CAS (close double-removal window)" + trailers
```

---

## Task 5: Scenario regression + docs

**Files:**
- Run: `utils/ca-soak` (a rename/attach-churn card, or the closest existing lifecycle card, e.g. `s15_s18_shards_lifecycle.py`)
- Modify: `docs/superpowers/cas/06-tla-models.md`, `utils/ca-soak/scenarios/BACKLOG.md`

- [ ] **Step 1: Rebuild the server binary (FOREGROUND)**

```bash
ninja -C build clickhouse > build/build_clickhouse_promote_republish.log 2>&1   # wait for exit
```

- [ ] **Step 2: Run a lifecycle/rename scenario** that exercises RENAME/DETACH-ATTACH churn (the closest existing card). Confirm PASS with no `owner↔refs` divergence and no unreachable manifest at the quiesced fixpoint:

```bash
cd utils/ca-soak && python3 -m scenarios.run --scenario S15 --scale dev > tmp/s15_promote_republish.log 2>&1
```
Expected: `status=PASS`. If no card exercises rename/attach re-drive, note that the unit tests + TLA+ are the primary evidence and record which card was used as the general no-regression check.

- [ ] **Step 3: Update `docs/superpowers/cas/06-tla-models.md`** — add a short subsection recording the `AtMostOneCommittedManifestPerRef` invariant + `SabotagePromoteOverwritesCommitted` control (bug violates, `stage2` holds), and that the C++ fix landed (promote fail-close + republishRef idempotency). Use `{#kebab-anchor}` headers.

- [ ] **Step 4: Mark BACKLOG resolved** — in `utils/ca-soak/scenarios/BACKLOG.md`, append `RESOLVED 2026-07-08` notes to `PROMOTE-OVER-COMMITTED-LEAK` and `ABANDON-RETIRE-ORDERING` with the commit trail.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/cas/06-tla-models.md utils/ca-soak/scenarios/BACKLOG.md
git commit  # "docs(cas): promote-over-committed + abandon-ordering fix landed — TLA+ gate + BACKLOG resolved" + trailers
```

---

## Self-Review

**Spec coverage:** BUG 1a fail-close → Task 2 (+ RED Task 1); BUG 1c republishRef idempotency → Task 3; BUG 2 abandon reorder → Task 4; TLA+ gate → Task 0; ABORTED-not-LOGICAL_ERROR → enforced in Tasks 2/3 (extern added in Task 3); entries-only path-sorted comparison → Task 3 Step 5; scenario+docs → Task 5. All covered.

**Placeholder scan:** the only deferred specifics are (a) the exact `ManifestEntry` inline-data field name (Task 1 — "verify exact spelling per `CasManifestCodec.h`"), (b) the manifest-count mechanism in the 1c test (Task 3 — LIST or fsck, harness-dependent), and (c) the ContentAddressedTransaction test-construction boilerplate (Task 3 — "mirror `gtest_ca_transaction.cpp`"). Each names a concrete existing source to copy from; no invented APIs.

**Type consistency:** `ErrorCodes::ABORTED` (externed in `CasBuild.cpp:31`, added to `ContentAddressedTransaction.cpp` in Task 3). `RootRef::manifest_ref` (`ManifestRef`) compared to `id.ref` (`ManifestRef`, `operator== = default`) in Task 2. `PartManifest::entries` (`std::vector<ManifestEntry>`, `operator== = default`) compared in Task 3. `resolveRef` → `std::optional<Resolved>{manifest_id, mutable_files}`; `readManifest(ManifestId)` → `PartManifest`. All consistent with the headers read.
