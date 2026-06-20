# CA GC snap-generation retention — implementation plan (B174)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline) or
> subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Bound `gc/snap` storage by pruning superseded snap generations during each GC round,
keeping the last `gc_snap_generations_to_keep` (default 3; 0 = keep-all).

**Architecture:** A new `PoolConfig` knob + a new durable cursor `GcState::snap_pruned_through`.
`Gc::cascade`, after computing `adopted_generation` and before the round-commit `gc/state` CAS,
prunes generations `(snap_pruned_through, adopted_generation − keep]` (bounded per round) by
`HEAD`+`deleteExact` of each snap shard, and folds the advanced cursor into the same CAS.

**Tech stack:** C++ (ClickHouse), gtest, Poco JSON codec.

Spec: `docs/superpowers/specs/2026-06-21-ca-gc-snap-generation-retention-design.md`.

---

### Task 1: `PoolConfig` knob + plumbing

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}`

- [ ] **Step 1:** In `CasStore.h` `struct PoolConfig`, after the `dedup_head_first_min_bytes` block, add:

```cpp
    /// B174 (gc/snap retention): how many superseded snap generations to retain. After committing
    /// generation G, generations <= G - this are pruned (bounded per round). 0 = keep ALL
    /// (debug/forensics — replay GC's in-degree view as-of a past round). Default 3 = the safety
    /// margin covering any in-flight/resuming leader (a leader more than `keep` generations behind
    /// has lost its lease; its round-commit CAS fails).
    uint64_t gc_snap_generations_to_keep = 3;
```

- [ ] **Step 2:** In `MetadataStorageFactory.cpp`, next to the `content_addressed_dedup_*` parse
  (~line 247), add:

```cpp
        const uint64_t gc_snap_generations_to_keep = config.getUInt64(config_prefix + ".content_addressed_gc_snap_generations_to_keep", 3);
```

  and pass it as the new trailing ctor arg to the `ContentAddressedMetadataStorage` construction
  (after `dedup_head_first_min_bytes`).

- [ ] **Step 3:** In `ContentAddressedMetadataStorage.h`, add a ctor param
  `uint64_t gc_snap_generations_to_keep_ = 3` (after `dedup_head_first_min_bytes_`) and a member
  `const uint64_t gc_snap_generations_to_keep;`.

- [ ] **Step 4:** In `ContentAddressedMetadataStorage.cpp`, add the ctor init
  `, gc_snap_generations_to_keep(gc_snap_generations_to_keep_)` and, where `pool_config` is built
  (next to `pool_config.dedup_head_first_min_bytes = ...`), add
  `pool_config.gc_snap_generations_to_keep = gc_snap_generations_to_keep;`.

- [ ] **Step 5:** Commit: `git commit -m "CA B174: PoolConfig gc_snap_generations_to_keep knob + plumbing"`

---

### Task 2: `GcState::snap_pruned_through` field + codec (TDD)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_formats.cpp`

- [ ] **Step 1: Write failing tests** — append to `gtest_cas_gc_formats.cpp`:

```cpp
TEST(CasGcFormats, GcStateSnapPrunedThroughRoundTrip)
{
    GcState s;
    s.snap_shards = 2;
    s.snap_generation = 42;
    s.snap_pruned_through = 38;
    auto d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.snap_pruned_through, 38u);
}

TEST(CasGcFormats, GcStateSnapPrunedThroughBackCompatDefaultsZero)
{
    /// An old gc/state written before B174 has no "snap_pruned_through" key — must decode to 0,
    /// not throw on the strict unknown-key check.
    const String old_state =
        R"({"format":"cas_gc_state","version":3,"round":1,"fence_seq":0,"snap_shards":1,)"
        R"("snap_generation":5,"lease":{"owner":"00000000000000000000000000000000","seq":0},)"
        R"("fence_version":{}})";
    auto d = decodeGcState(old_state);
    EXPECT_EQ(d.snap_pruned_through, 0u);
}
```

- [ ] **Step 2:** Run (expect FAIL: no member `snap_pruned_through`):
  `./build/src/unit_tests_dbms --gtest_filter='CasGcFormats.GcStateSnapPrunedThrough*'`

- [ ] **Step 3: Implement.** In `CasGcFormats.h` `struct GcState`, after `snap_generation`, add:

```cpp
    uint64_t snap_pruned_through = 0;   /// B174: highest snap generation fully pruned (retention cursor)
```

  In `CasGcFormats.cpp` `encodeGcState`, after the `snap_generation` block (before the `lease`
  block), add:

```cpp
    writeJsonKey(out, "snap_pruned_through");
    writeIntText(state.snap_pruned_through, out);
    writeChar(',', out);
```

  In `decodeGcState`, add `"snap_pruned_through"` to the `checkNoUnknownKeys` allowed set, and after
  `state.snap_generation = requireU64(...)` add (optional read for back-compat):

```cpp
        if (obj->has("snap_pruned_through"))
            state.snap_pruned_through = requireU64(*obj, "snap_pruned_through", "gc/state");
```

- [ ] **Step 4:** Run, expect PASS (also re-run `CasGcFormats.*` to confirm no regression).

- [ ] **Step 5:** Commit: `git commit -m "CA B174: GcState.snap_pruned_through retention cursor + codec"`

---

### Task 3: prune step in `Gc::cascade`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`

- [ ] **Step 1:** Near the top-of-function constants in `cascade` (next to
  `constexpr uint64_t max_generation_probes = 1000;`, ~line 653) add:

```cpp
    constexpr uint64_t MAX_PRUNE_GENERATIONS_PER_ROUND = 64;   /// B174: bound the per-round prune burst
```

- [ ] **Step 2:** Replace the `next` construction + CAS block (currently lines ~698-709,
  from `GcState next = state;` through `state_token = committed_token;`) with the prune-then-CAS
  version. Insert the prune BEFORE `casPut`, folding the cursor into `next`:

```cpp
    GcState next = state;
    next.snap_generation = adopted_generation;   /// unchanged when the persist was skipped
    /// fence_version[<= round] served its recheck - erase it (gc/state must not grow forever).
    std::erase_if(next.fence_version, [&](const auto & kv) { return kv.first <= round; });

    /// B174: prune superseded snap generations. loadSnap reads ONLY gcSnapKey(snap_generation),
    /// so any generation strictly below the committed one is dead; keep `keep` as the safety margin
    /// for in-flight/resuming leaders (a leader more than `keep` generations behind has lost its
    /// lease — its commit CAS fails). Walk forward from the durable cursor, bounded per round, so a
    /// large legacy backlog drains over many rounds without ever LISTing the generation directories.
    /// Done BEFORE the gc/state CAS so the advanced cursor rides the same write: if the CAS then
    /// loses the lease, the deletes were still below the winner's even-higher floor (safe) and the
    /// cursor is not durably advanced (idempotent retry).
    const uint64_t keep = store->poolConfig().gc_snap_generations_to_keep;
    if (keep > 0 && adopted_generation > keep)
    {
        const uint64_t prune_floor = adopted_generation - keep;   /// prune <= prune_floor
        uint64_t g = next.snap_pruned_through + 1;
        uint64_t pruned = 0;
        for (; g <= prune_floor && pruned < MAX_PRUNE_GENERATIONS_PER_ROUND; ++g, ++pruned)
        {
            for (uint64_t snap_shard = 0; snap_shard < state.snap_shards; ++snap_shard)
            {
                const String snap_key = layout.gcSnapKey(g, snap_shard);
                const HeadResult hr = backend.head(snap_key);
                if (hr.exists)
                    backend.deleteExact(snap_key, hr.token);   /// NotFound/TokenMismatch tolerated
            }
        }
        next.snap_pruned_through = g - 1;   /// highest generation fully processed this round
    }

    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token, &committed_token)
        != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc cascade: gc/state moved during the cascade persist (another leader advanced it); "
            "retry next round");
    state = std::move(next);
    state_token = committed_token;
```

  (Note: `backend`, `layout`, `store`, `state`, `round`, `adopted_generation`, `state_token` are all
  already in scope in `cascade`; `HeadResult`/`PutOutcome`/`CasOutcome` are the existing backend
  types.)

- [ ] **Step 3:** Commit: `git commit -m "CA B174: prune superseded gc/snap generations in Gc::cascade"`

---

### Task 4: retention integration tests (TDD)

**Files:**
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

The existing harness: `openTestStore(b)`, `Gc gc(s, leader)`, `gc.runRegularRound()`,
`readState(*b, *s)`, `b->get(s->layout().gcSnapKey(gen, shard))`. Generations advance when a round
strips edges — reuse the churn pattern from the existing leak/round tests (create a ref via a build
publish, then drop it, then run rounds) to force `snap_generation` to climb past `keep`.

- [ ] **Step 1: Write failing test** — append a test that:
  1. opens a store whose `PoolConfig.gc_snap_generations_to_keep = 3` (use the test store opener; if
     it does not expose the knob, open via `Store::open(b, PoolConfig{.pool_prefix="p", .gc_snap_generations_to_keep=3})`);
  2. drives enough churning rounds (publish+drop refs, `runRegularRound`) that `readState(...).snap_generation >= 6`;
  3. asserts the last 3 generations' snap objects exist
     (`b->get(gcSnapKey(gen, 0))` non-null for `gen ∈ {G, G-1, G-2}`),
  4. asserts generations `<= G-3` are pruned (`b->get(gcSnapKey(gen, 0))` is null),
  5. asserts `readState(...).snap_pruned_through == G - 3`.

  Add a second test with `gc_snap_generations_to_keep = 0` asserting NO generation is pruned
  (all `gcSnapKey(gen,0)` for `gen ∈ [1, G]` remain non-null and `snap_pruned_through == 0`).

  (Author the exact churn loop by mirroring the nearest existing generation-advancing test in this
  file; if no `openTestStore` overload takes a `PoolConfig`, add one or open the store directly.)

- [ ] **Step 2:** Run, expect FAIL before Task 3 is built / PASS after.
  `./build/src/unit_tests_dbms --gtest_filter='CasGc*Retention*:CasGc*SnapPrune*'`

- [ ] **Step 3:** Commit: `git commit -m "CA B174: gc/snap retention integration tests"`

---

### Task 5: build + run the CA gtest suite

- [ ] **Step 1:** `ninja -C build unit_tests_dbms clickhouse > build/build_b174.log 2>&1`
  (analyze the log via a subagent; expect clean).
- [ ] **Step 2:** `./build/src/unit_tests_dbms --gtest_filter='CasGc*:CaDedupCache.*' > build/test_b174.log 2>&1`
  — expect the new tests PASS and the suite otherwise unchanged (the 2 known pre-existing reds
  `CaWiringOps.FreezeViaHardLinksIntoShadow` (B186) + `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`
  (B140) may still fail; nothing else).

---

### Task 6: soak validation + backlog

- [ ] **Step 1:** Restart the ca-soak cluster on the new binary; run a multi-hour soak
  (`python3 -m soak.run --seed <s> --phase 3 --duration 2h --no-chaos`), tracking `gc/` storage
  (e.g. `mc du`/object counts per prefix) to confirm `gc/` is BOUNDED (sawtooth) rather than
  monotonically growing, and `gc/state.snap_pruned_through` advances.
- [ ] **Step 2:** Update backlog B174 → DONE with the result; commit.
