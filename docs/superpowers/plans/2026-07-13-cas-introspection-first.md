# CAS Introspection-First (§0 of Round B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** first-class ProfileEvents for every `.meta` write class, GC bounded-pool meta ops, GC enumeration pages, and dedup-cache hits/misses — so the Round-B levers (and the rev.6 soak task) measure their effects directly instead of by subtraction.

**Architecture:** counters only — no behavior change, no new tables, no config. Choke-point totals live in `CasBlobMeta.cpp`; per-reason counters at the `CasBuild.cpp` call sites; GC-side counters increment inside the round so they land in the GC log's `ProfileEvents` map.

**Tech Stack:** ClickHouse ProfileEvents (`M(...)` macro), gtest with counter-delta assertions (pattern: `ProfileEvents::global_counters[ProfileEvents::X].load()` before/after, as in `gtest_cas_ref_writer.cpp`'s `CasRefSweepRearmed` tests from commit `2174a893f33`).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-13-cas-memory-s3-budget-optimizations-design.md` §0.
- Pure instrumentation: ZERO behavior change; every new counter has ≥1 emit site and ≥1 test asserting a nonzero delta (the S13 INTROSPECTION-1 lesson — no dead counters).
- Allman braces; ProfileEvents descriptions follow the `Cas*` house style ("CA <subsystem>: <what> (spec §...)").
- After each task: filtered sweep `timeout 900 build/src/unit_tests_dbms --gtest_filter='*Cas*:RefWriter*:*RefTableCache*:CaWiring*:CaPartPathParser*'` — expect baseline 944 + new tests, 0 failed.
- Build only via `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/introspect_build.log 2>&1` (a prod-scale scenario campaign may be running — NEVER build the `clickhouse` binary target; the controller rebuilds it after the plan completes).
- New commits only; stage files by explicit path (shared worktree); verify HEAD after each commit; commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` + `Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27`

---

### Task 1: `.meta` write counters (choke points + per-reason call sites)

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (append after the `CasGcMetaWriteAnomaly` line, `:838`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.cpp` (functions `putMetaIfAbsent`, `casMeta`, `deleteMetaExact` — declared `CasBlobMeta.h:54-56`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp:334` (adopt backfill), `:445` (create-Clean after body), and the `writeResurrectMetaClean` lambda (`:456-470`)
- Test: `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces:**
- Produces ProfileEvents: `CasMetaPut`, `CasMetaCas`, `CasMetaDelete` (choke-point totals); `CasMetaCreateClean`, `CasMetaAdoptBackfill`, `CasMetaResurrectClean` (reasons). Later levers and §5's baseline soak consume these names verbatim.

- [ ] **Step 1: Write the failing test**

In `gtest_cas_build.cpp`, next to the existing putBlob tests (reuse their fixture/stub backend):

```cpp
TEST(CasBuildMetaCounters, CreateCleanAndChokePointCountOnFreshBody)
{
    /// Fresh body upload writes the Clean meta exactly once: CasMetaPut +1 (choke point)
    /// and CasMetaCreateClean +1 (reason). Reuse the fixture of the nearest putBlob test.
    const auto put_before = ProfileEvents::global_counters[ProfileEvents::CasMetaPut].load();
    const auto reason_before = ProfileEvents::global_counters[ProfileEvents::CasMetaCreateClean].load();

    /* <fixture: run one successful putBlob of NOVEL content through the stub backend,
       exactly as the nearest existing fresh-create test does> */

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasMetaPut].load() - put_before, 1);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasMetaCreateClean].load() - reason_before, 1);
}

TEST(CasBuildMetaCounters, ResurrectCountsCasAndReason)
{
    const auto cas_before = ProfileEvents::global_counters[ProfileEvents::CasMetaCas].load();
    const auto reason_before = ProfileEvents::global_counters[ProfileEvents::CasMetaResurrectClean].load();

    /* <fixture: drive the condemned-occupant resurrect path exactly as the existing
       resurrect/displacement test does (stub meta = condemned, body present)> */

    EXPECT_GE(ProfileEvents::global_counters[ProfileEvents::CasMetaCas].load() - cas_before, 1);
    EXPECT_GE(ProfileEvents::global_counters[ProfileEvents::CasMetaResurrectClean].load() - reason_before, 1);
}
```

The `/* <fixture: ...> */` bodies are copied from the named sibling tests in the same file — the
assertions above are the new content.

- [ ] **Step 2: Run to verify failure**

Run: `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/introspect_build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasBuildMetaCounters.*'`
Expected: COMPILE ERROR — `CasMetaPut` not a member of `ProfileEvents` (the RED for counter tasks).

- [ ] **Step 3: Implement**

`src/Common/ProfileEvents.cpp`, after `:838`:

```cpp
    M(CasMetaPut,          "CA blob meta: .meta object creations via putMetaIfAbsent (choke-point total; reasons split by CasMetaCreateClean/CasMetaAdoptBackfill)", ValueType::Number) \
    M(CasMetaCas,          "CA blob meta: .meta conditional rewrites via casMeta (choke-point total)", ValueType::Number) \
    M(CasMetaDelete,       "CA blob meta: .meta exact-token deletions via deleteMetaExact (choke-point total)", ValueType::Number) \
    M(CasMetaCreateClean,  "CA blob meta: Clean meta created right after a fresh body commit (CasBuild create path; Round-B §5 baseline)", ValueType::Number) \
    M(CasMetaAdoptBackfill,"CA blob meta: Clean meta backfilled for a hash that had none at adopt time (pre-protocol blob or lost race)", ValueType::Number) \
    M(CasMetaResurrectClean,"CA blob meta: meta driven back to Clean on the resurrect path (writeResurrectMetaClean)", ValueType::Number) \
```

`CasBlobMeta.cpp`: at the top add the `ProfileEvents` extern block (mirror `CasBuild.cpp`'s);
first statement of `putMetaIfAbsent` → `ProfileEvents::increment(ProfileEvents::CasMetaPut);`,
of `casMeta` → `CasMetaCas`, of `deleteMetaExact` → `CasMetaDelete`.

`CasBuild.cpp`: immediately before the `putMetaIfAbsent` call at `:334` →
`ProfileEvents::increment(ProfileEvents::CasMetaAdoptBackfill);`; before the call at `:445` →
`CasMetaCreateClean`; inside `writeResurrectMetaClean` before the attempt loop →
`CasMetaResurrectClean`. Extend the file's existing `ProfileEvents` extern block with the three
names.

- [ ] **Step 4: Run to verify pass**

Run: same build + `--gtest_filter='CasBuildMetaCounters.*'`
Expected: 2 PASSED.

- [ ] **Step 5: Filtered sweep + commit**

Run the Global-Constraints sweep; expect baseline+2, 0 failed. Then:

```bash
git add src/Common/ProfileEvents.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
  src/Disks/tests/gtest_cas_build.cpp
git commit -m "cas: .meta write counters — choke-point totals + per-reason call sites"
```

---

### Task 2: GC attribution counters (bounded meta pool + enumeration pages)

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (after Task 1's block)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — the meta-pool `run` lambda scheduled at `:193` (pool built at `:163-167`), and every GC-owned enumeration loop that calls the backend list/iterate (find them: `grep -n "\->list(" CasGc.cpp CasOrphanManifestSweep.cpp` — the candidate scan and the orphan sweep page loops)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

**Interfaces:**
- Produces ProfileEvents: `CasGcMetaOps` (per-hash freshness-meta operations executed on the round's bounded pool), `CasGcEnumerationPages` (physical-universe LIST pages fetched by GC's own loops).
- MUST increment on the ROUND's thread scope where possible: `CasGcEnumerationPages` increments in the page loop (round thread — lands in the GC log map); `CasGcMetaOps` increments inside the pool lambda (pool thread — document in the description that it is pool-scoped and therefore GLOBAL-only, closing audit artifact #6 by name rather than by scope).

- [ ] **Step 1: Failing test** — in `gtest_cas_gc_round.cpp`, extend the smallest existing full-round test (one condemn+delete round over the stub backend):

```cpp
    const auto meta_ops_before = ProfileEvents::global_counters[ProfileEvents::CasGcMetaOps].load();
    const auto pages_before = ProfileEvents::global_counters[ProfileEvents::CasGcEnumerationPages].load();
    /* <existing round-driving body unchanged> */
    EXPECT_GE(ProfileEvents::global_counters[ProfileEvents::CasGcMetaOps].load() - meta_ops_before, 1);
    EXPECT_GE(ProfileEvents::global_counters[ProfileEvents::CasGcEnumerationPages].load() - pages_before, 1);
```

- [ ] **Step 2: Verify compile-RED** (same command shape as Task 1, filter `CasGcRound.*` for the extended test).
- [ ] **Step 3: Implement** — `M(CasGcMetaOps, "CA gc: per-hash freshness-meta ops executed on the round's bounded meta pool (pool-scoped: GLOBAL counter only — closes metrics-audit attribution artifact #6)", ValueType::Number)` and `M(CasGcEnumerationPages, "CA gc: physical-universe LIST pages fetched by the round's own enumeration loops (round-scoped: lands in the GC log ProfileEvents map — closes artifact #3)", ValueType::Number)`; increments per the Interfaces block.
- [ ] **Step 4: Verify pass.**
- [ ] **Step 5: Sweep + commit** (explicit paths: `ProfileEvents.cpp`, `CasGc.cpp`, `CasOrphanManifestSweep.cpp` if touched, `gtest_cas_gc_round.cpp`).

---

### Task 3: dedup-cache hit/miss counters

**Files:**
- Modify: `src/Common/ProfileEvents.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` — the `dedup_cache` lookup site(s) (`dedup_cache` member declared `CasStore.h:931-932`; find lookups: `grep -n "dedup_cache->" CasStore.cpp`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp` (or wherever the existing dedup-cache tests live — `grep -rn "dedup_cache" src/Disks/tests/`)

**Interfaces:**
- Produces ProfileEvents: `CasDedupCacheHits`, `CasDedupCacheMisses`. §2's soak matrix consumes them.

- [ ] **Step 1: Failing test** — two-lookup test: first observeAndAdmit of a hash = miss (+1 miss), repeat of the same hash = hit (+1 hit), using the existing dedup-cache test fixture (cache enabled at default size).

```cpp
    const auto hits_before = ProfileEvents::global_counters[ProfileEvents::CasDedupCacheHits].load();
    const auto miss_before = ProfileEvents::global_counters[ProfileEvents::CasDedupCacheMisses].load();
    /* <fixture: two putBlob calls with IDENTICAL content through the stub backend> */
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasDedupCacheMisses].load() - miss_before, 1);
    EXPECT_GE(ProfileEvents::global_counters[ProfileEvents::CasDedupCacheHits].load() - hits_before, 1);
```

- [ ] **Step 2: Verify compile-RED.**
- [ ] **Step 3: Implement** — `M(CasDedupCacheHits, "CA dedup cache: lookups answered from the in-memory BlobRef presence cache (each hit avoids an occupancy HEAD; Round-B §2)", ValueType::Number)`, `M(CasDedupCacheMisses, "CA dedup cache: lookups that fell through to the backend probe", ValueType::Number)`; increment at the lookup site's hit/miss branches (if `CacheBase::get` is the seam, wrap it: `if (auto v = dedup_cache->get(key)) { increment(Hits); ... } else { increment(Misses); ... }` — matching the existing control flow, no behavior change).
- [ ] **Step 4: Verify pass.**
- [ ] **Step 5: Sweep + commit** (explicit paths).

---

## Self-review notes

- Spec §0.1 → Task 1 (incl. `CasMetaDelete` for the §5 tombstone-delete future — it has a live emit
  site TODAY: `deleteMetaExact` exists and is called by GC); §0.2 → Task 2; §0.3 → Task 3;
  §0.4 deliberately absent (lands with levers — spec amended `ace3549b071`+); §0.5 holds (no tables).
- No dead counters: every counter has an emit site and a delta-asserting test.
- Fixture placeholders in test steps are explicit copy-from-named-sibling instructions, not TBDs:
  the implementer copies the adjacent test's body verbatim (fixtures are file-local and current on
  HEAD; inlining them here would drift).
