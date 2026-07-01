# CA GC Shard Incarnation + Registry Removal — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. This is an existing large C++ codebase: each task cites exact files and `file:line` anchors — read the cited functions before editing; do not restructure unrelated code.

**Goal:** Delete `gc/registry` and reclaim empty ref-shard objects by collapsing GC's create-ordering to two coordinates — a durable per-`(ns,shard)` incarnation and the pool-global GC round — so `dropNamespace` no longer leaks a monotone registry + empty shard objects (soak S30).

**Architecture:** Discovery moves from the registry to `LIST(cas/refs/)`. Each ref-shard object carries an immutable `incarnation` (the `(writer_epoch, build_sequence)` of the build that created it); the GC fold cursor is keyed by `(ns, shard, incarnation)`, so a delete+recreate at the same path can never be ABA-confused. A newborn namespace is a **precommit-state shard** born fenced to the current `gcRound` (self-floor), and GC fences every present shard by LIST each round. Ref-shard objects are reclaimed like blobs: `dropNamespace` writes an in-band tombstone, and GC deletes an empty + tombstoned + fully-folded shard via a token-guarded `deleteExact`.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), protobuf (`Proto/cas_format.proto`), GoogleTest (`src/Disks/tests/`), TLA+/TLC for the regression gate.

**Design spec:** `docs/superpowers/specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`.
**Phase-0 TLA+ gate:** already GREEN — `docs/superpowers/models/CaGcShardIncarnationCore.tla` + `_RESULTS.md`. This plan is phases 1–5 of §8.

## Global Constraints

- **CasBuild edits are permitted for D1** (the standing "never touch CasBuild" rule is lifted for this work), BUT any change to `Build::precommitAdd` / `Build::promote` REQUIRES re-running the `CaBuildRootPrecommit` TLA+ model afterward (Task 7) — precommit/promote are TLA+-proven.
- **CA is pre-release: NO compat/migration scaffolding.** Proto field renumbering is safe; decode is fail-closed (a missing incarnation where one is required ⇒ `CORRUPTED_DATA`).
- **Incarnation = `(writer_epoch, build_sequence)`** of the creating build (reuse; no new persistent object). The load-bearing invariant is **INC-MONO**: a recreate of the same `(ns,shard)` must draw a strictly greater incarnation. If Task 2 finds a drop+recreate can occur within one build without advancing `build_sequence`, switch to a dedicated sticky per-`server_root` incarnation allocator (same mechanism as `allocateWriterEpoch`, `CasServerRoot.cpp:245`).
- **LIST-consistency is load-bearing** once discovery authority moves to `LIST(cas/refs/)`. `InMemoryBackend` and any real backend must give read-your-writes enumeration; a backend that cannot is unsupported (fail-closed, documented) — confirm in Task 4.
- **Allman braces** (opening brace on its own line) — enforced by CI style check.
- **Never use `sleep` for concurrency.** Prefer letting errors propagate over silent fallbacks (fail-closed).
- **Build:** run `ninja` (no `-j`, no `nproc`) redirecting to a build-dir log; analyze the log via a subagent (concise summary only). **Tests:** redirect each gtest run to a uniquely named log under the build dir; analyze via a subagent.
- **New GC fold/discovery tests must run with `gc_shards>1` as well as `gc_shards=1`** (close the `gc_shards=1` blind spot).
- **Commit trailers** on every commit:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`

---

## File Structure

- `Core/CasRootShardCodec.h` / `.cpp` — `RootShard` gains `incarnation`; a drop tombstone owner-event.
- `Core/Proto/cas_format.proto` — `RootShardManifest` gains `incarnation` fields; a tombstone marker on the owner-event enum.
- `Core/CasGenerationSeal.h` — `ShardCoverage` gains `incarnation`; cursor keyed by it.
- `Core/CasGc.{h,cpp}` — fold cursor incarnation-keyed + reset-on-mismatch; `discoverUniverse` → LIST; fence only present shards; delete the registry-fence sub-step; tombstone reclaim step.
- `Core/CasStore.{h,cpp}` — `mutateShard` stamps incarnation at create; delete `ensureRegistered`/`registered_cache`; `listNamespaces` → LIST; `dropNamespace` writes the tombstone.
- `Core/CasBuild.cpp` — `precommitAdd`/`promote`: drop `ensureRegistered`, supply the incarnation source, self-floor a newborn to `gcRound`, gate `promote` on the shard fence.
- `Core/CasRootsRegistry.{h,cpp}`, `Core/CasLayout.h::rootsRegistryKey`, proto `RootsRegistryProto` — **deleted**.
- `Core/CasFsck.cpp`, `ContentAddressedTransaction.cpp` — `listNamespaces` consumers migrate to LIST.
- Tests: `src/Disks/tests/gtest_cas_gc_shard_incarnation.cpp` (new), plus edits to `gtest_cas_gc_fold.cpp`, `gtest_cas_store.cpp`, `gtest_cas_fsck.cpp`.

---

### Task 1: `incarnation` field on `RootShard` (codec + proto + seal coverage)

**Files:**
- Modify: `Core/Proto/cas_format.proto:90-98` (`RootShardManifest`)
- Modify: `Core/CasRootShardCodec.h:77-84` (`struct RootShard`), `Core/CasRootShardCodec.cpp` (`encodeRootShard`/`decodeRootShard`)
- Modify: `Core/CasGenerationSeal.h:28-33` (`struct ShardCoverage`) + its codec in `Core/CasGenerationSeal.cpp`
- Test: `src/Disks/tests/gtest_cas_codecs.cpp` (add cases; it already covers `RootShard` round-trip) and `gtest_cas_generation_seal.cpp`

**Interfaces:**
- Produces: `RootShard::incarnation` — a `ManifestRef`-shaped `{uint64 writer_epoch; uint64 build_sequence;}` pair (reuse the existing 2-tuple; ignore `manifest_ordinal`). Represent as a small struct `ShardIncarnation { uint64_t writer_epoch = 0; uint64_t build_sequence = 0; bool operator==...; bool operator<...; }` in `CasRootShardCodec.h`. `{0,0}` = unstamped (legacy/never-created).
- Produces: `ShardCoverage::incarnation` (same type) — the incarnation the cursor was sealed against.

- [ ] **Step 1: Write the failing codec round-trip test**

In `gtest_cas_codecs.cpp` add:
```cpp
TEST(CasRootShardCodec, IncarnationRoundTrips)
{
    RootShard root;
    root.shard_version = 7;
    root.fence_round = 3;
    root.incarnation = ShardIncarnation{.writer_epoch = 5, .build_sequence = 42};
    const RootShard back = decodeRootShard(encodeRootShard(root));
    EXPECT_EQ(back.incarnation.writer_epoch, 5u);
    EXPECT_EQ(back.incarnation.build_sequence, 42u);
    EXPECT_EQ(back, root);
}
```

- [ ] **Step 2: Run it, verify it fails to compile** (`root.incarnation` undefined).

Build only the disk unit-test target and grep for the error:
`ninja -C build clickhouse_disks_unit_tests > build/build_task1.log 2>&1` (target name: confirm via `ninja -C build -t targets | grep -i unit`). Expected: compile error `no member named 'incarnation'`.

- [ ] **Step 3: Add the field + proto + codec.**

`cas_format.proto` `RootShardManifest` — add (fields 2–5 exist; use 6–7):
```proto
  uint64 incarnation_writer_epoch  = 6;
  uint64 incarnation_build_sequence = 7;
```
`CasRootShardCodec.h`: define `struct ShardIncarnation { uint64_t writer_epoch = 0; uint64_t build_sequence = 0; bool operator==(const ShardIncarnation &) const = default; bool operator<(const ShardIncarnation & o) const { return std::tie(writer_epoch, build_sequence) < std::tie(o.writer_epoch, o.build_sequence); } };` and add `ShardIncarnation incarnation;` to `RootShard` (after `fence_round`).
`CasRootShardCodec.cpp`: `encodeRootShard` sets `msg.set_incarnation_writer_epoch(root.incarnation.writer_epoch)` etc.; `decodeRootShard` reads them back. No fail-closed needed (0/0 default is valid = unstamped).

- [ ] **Step 4: Extend `ShardCoverage`.** In `CasGenerationSeal.h` add `ShardIncarnation incarnation;` to `struct ShardCoverage`; encode/decode it in `CasGenerationSeal.cpp` (mirror `folded_token`/`folded_cursor`). Add a seal round-trip assertion in `gtest_cas_generation_seal.cpp` covering a non-zero incarnation.

- [ ] **Step 5: Run tests to verify pass.**

`ninja -C build clickhouse_disks_unit_tests > build/build_task1.log 2>&1` then
`build/.../unit_tests_dbms --gtest_filter='CasRootShardCodec.*:CasGenerationSeal.*' > build/test_task1.log 2>&1`
Expected: PASS. (Redirect + analyze via subagent.)

- [ ] **Step 6: Commit** (`git add` the proto, codec, seal, tests; message `CA: add incarnation to RootShard + ShardCoverage (codec/proto)`).

---

### Task 2: Stamp the incarnation at shard birth

**Files:**
- Modify: `Core/CasStore.cpp:730` (`Store::mutateShard`) — stamp incarnation on the create-if-absent path.
- Modify: `Core/CasStore.h` (`mutateShard` signature: add an optional incarnation arg) and every `mutateShard` caller.
- Modify: `Core/CasBuild.cpp:582` (`precommitAdd`) — pass `ShardIncarnation{writer_epoch, build_seq}`.
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `RootShard::incarnation` (Task 1).
- Produces: `mutateShard(ns, shard, mutate, out_committed_version, origin, kind, ShardIncarnation birth_incarnation = {})` — when the create-if-absent path materializes a fresh `RootShard` (no prior object), set `root.incarnation = birth_incarnation` before applying `mutate`; when the object already exists, leave `incarnation` untouched (immutable for the object's life).

- [ ] **Step 1: Write the failing test** (stamp on birth; INC-MONO decision point).

In `gtest_cas_store.cpp` (use the existing in-memory `Store` harness in that file):
```cpp
TEST(CasStore, ShardBornCarriesIncarnation)
{
    auto store = makeInMemoryStore(/*root_shards*/4);   // existing helper in this test file
    const RootNamespace ns{"00/aa@cas@"};
    store->mutateShard(ns, 0, [](RootShard &){}, nullptr,
                       RootMutationOrigin::Writer, RootMutationKind::Precommit,
                       ShardIncarnation{.writer_epoch = 9, .build_sequence = 2});
    const auto [root, tok] = store->readShard(ns, 0);
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(root.incarnation.writer_epoch, 9u);
    EXPECT_EQ(root.incarnation.build_sequence, 2u);
}
```

- [ ] **Step 2: Run, verify it fails** (signature has no incarnation arg). Build + filter `CasStore.ShardBornCarriesIncarnation`.

- [ ] **Step 3: Implement the stamp.** In `mutateShard`, detect the create-if-absent case (the branch that constructs a fresh `RootShard` when `readShard` returned no token) and set `root.incarnation = birth_incarnation` there, before the `mutate` callback. Thread the new defaulted arg through `CasStore.h` and update callers: `precommitAdd` (`CasBuild.cpp:599`) passes `ShardIncarnation{build_id-derived writer_epoch, build_seq}` — read the actual `writer_epoch`/`build_seq` members on `Build` (see `CasBuild.cpp:637` context and `CasServerRoot`); other callers (`drop`, `updateRefPayload`, `dropNamespace`, GC fence) pass `{}` (they never create a first object, or their create is not a namespace birth).

- [ ] **Step 4: Resolve INC-MONO (the Global-Constraint decision).** Inspect the build/publish path (`CasBuild.cpp` build lifecycle) and confirm: can the SAME `(ns,shard)` be reclaimed and recreated within a single build without `build_sequence` advancing? Write a test asserting a second birth at the same path carries a strictly greater `(writer_epoch, build_sequence)`:
```cpp
TEST(CasStore, RebornShardIncarnationStrictlyGreater)
{
    // create @ (we=9,bs=2); simulate reclaim (drop the object); recreate @ (we=9,bs=3)
    // ASSERT reborn.incarnation > first.incarnation lexicographically
}
```
If the invariant cannot be guaranteed from `(writer_epoch, build_sequence)`, add a dedicated sticky per-`server_root` incarnation counter (mechanism: `allocateWriterEpoch`, `CasServerRoot.cpp:245-259`) and stamp from it instead. Record the decision in the test's comment and in the spec §1 open-question line.

- [ ] **Step 5: Run tests, verify pass.** Filter `CasStore.ShardBornCarriesIncarnation:CasStore.RebornShardIncarnationStrictlyGreater`. Redirect + subagent-analyze.

- [ ] **Step 6: Commit** (`CA: stamp shard incarnation at birth (INC-MONO)`).

---

### Task 3: Incarnation-keyed fold cursor (reset-on-mismatch)

**Files:**
- Modify: `Core/CasGc.cpp` — the fold (`fold`, ~`:264-385`), the recheck window (`:710-730`), and the discover-decision (`computeDiscoverDecisions`, `:1334+`).
- Test: `src/Disks/tests/gtest_cas_gc_fold.cpp`

**Interfaces:**
- Consumes: `ShardCoverage::incarnation` (Task 1), `RootShard::incarnation` (Task 2).
- Produces: fold behavior — when the sealed `per_ns_shard[ck].incarnation != readShard(ns,shard).incarnation`, the fold treats the prior cursor as absent: it restarts at `folded_cursor = 0`, drops this shard's stale edges from the carried generation, and stamps `cov.incarnation = live incarnation`. `ShardFolded`/discover `Skip` additionally require `sealed.incarnation == live incarnation` (an incarnation mismatch forces `Read` and a from-0 fold).

- [ ] **Step 1: Write the failing test.** Construct a shard whose sealed coverage is at incarnation `I1` while the live object is at `I2` (a recreate), with the live journal starting fresh at version 1; assert the fold applies the fresh events (does NOT skip them by the stale `folded_cursor`).
```cpp
TEST(CasGcFold, IncarnationMismatchRestartsFoldAtZero)
{
    // seal per_ns_shard["ns/0"] = {folded_cursor=5, incarnation=I1}
    // live shard @ incarnation I2, journal = [add(b1)] (transition_version 1)
    // run fold; ASSERT rootEdge (ns,b1) present  (would be skipped if cursor kept at 5)
}
```
(Model this on the existing `gtest_cas_gc_fold.cpp` harness — reuse its Store/seal setup helpers.)

- [ ] **Step 2: Run, verify it fails** (fold keeps `folded_cursor=5`, skips the event; edge absent).

- [ ] **Step 3: Implement reset-on-mismatch.** In `fold`, where the parent cursor is resolved (`CasGc.cpp:299`, `const uint64_t cursor = ... folded_cursor`), gate it: if the sealed `cov.incarnation != root.incarnation`, use `0` and drop this shard's carried edges (the carried generation must not retain `(ns,shard,*)` edges from the prior incarnation). Stamp `cov.incarnation = root.incarnation` when writing `result.fold_seal.per_ns_shard[cursor_key]`. In `computeDiscoverDecisions`, add the incarnation-equality conjunct to the `Skip` guard (alongside the `folded_token` check at `CasGc.h:265-269`) so a recreated shard is always `Read`.

- [ ] **Step 4: Run tests, verify pass.** Filter `CasGcFold.*`. Also run the full `gtest_cas_gc_*` set to confirm no regression. Redirect + subagent-analyze.

- [ ] **Step 5: Commit** (`CA: key GC fold cursor by shard incarnation (ABA-proof)`).

---

### Task 4: Discovery from `LIST(cas/refs/)` + delete the registry

**Files:**
- Modify: `Core/CasGc.cpp` — `discoverUniverse` (`:1277-1291`) → LIST; the fence loop (`:632-648`) fences only present shards; delete the registry-fence sub-step (`:602-630`) and the `_registry` cursor special-case (`:712`).
- Delete: `Core/CasRootsRegistry.{h,cpp}`; `Core/CasLayout.h::rootsRegistryKey` (`:61-64`); `RootsRegistryProto` (`cas_format.proto:195`) + `FormatId::RootsRegistry` (`CasFormat.h:29`, `CasFormat.cpp:38,64`).
- Modify: `Core/CasStore.{h,cpp}` — delete `ensureRegistered` (`:1007`), `registered_cache`/`registered_mutex` (`CasStore.h:431-432`); reimplement `listNamespaces` (`:1060`) as a LIST over `cas/refs/` (distinct `<ns>` segments) unioned with `roots/` (mirror `listMirroredChildren`, `:1078`).
- Modify: `Core/CasBuild.cpp` — remove the two `ensureRegistered` calls (`:594`, `:637`); `promote` no longer consumes `registry_fence` (Task 5 replaces its gate).
- Modify: `Core/CasFsck.cpp:114,238` and `ContentAddressedTransaction.cpp:707` — `listNamespaces` callers now hit the LIST implementation (no code change if the signature is unchanged; verify parity).
- Test: `src/Disks/tests/gtest_cas_gc_shard_incarnation.cpp` (new file), `gtest_cas_fsck.cpp`.

**Interfaces:**
- Produces: `discoverUniverse()` returns `{(ns, shard) : the object cas/refs/<ns>/<shard> is present}` (LIST-derived), replacing the `registry × root_shards` product. `listNamespaces(prefix)` returns namespaces having ≥1 present ref-shard (or verbatim files) under `prefix`.

- [ ] **Step 1: Write the failing tests.**
```cpp
TEST(CasGcShardIncarnation, DiscoveryEqualsPresentShards)
{
    auto store = makeInMemoryStore(4);
    // publish a ref into ns A shard 0 only; ns B has no shard object
    // ASSERT discoverUniverse() == {(A,0)}  (no registry; B absent)
}
TEST(CasGcShardIncarnation, ListNamespacesFromRefsNotRegistry)
{
    // publish into ns A; ASSERT listNamespaces("") == {A}; drop+reclaim -> {} (with Task 6) ; here: A present
}
```

- [ ] **Step 2: Run, verify they fail to compile / fail** (discovery still reads the registry; helpers referenced).

- [ ] **Step 3: Implement LIST discovery + delete the registry.** Rewrite `discoverUniverse` to LIST `casRefsPrefix()` (reuse `listRootShardTokens`, `CasGc.cpp:1303`, which already pages `cas/refs/`) and parse `<ns>/<shard>` keys. Delete the registry-fence sub-step and make the fence loop iterate the LIST-discovered present shards. Delete `CasRootsRegistry.{h,cpp}`, `rootsRegistryKey`, `ensureRegistered`, the registry proto/format id, and the `_registry` cursor branch. Reimplement `listNamespaces` via LIST. Remove the two `ensureRegistered` calls in `CasBuild.cpp` (leave `promote`'s gate temporarily reading `man[s].fence`; Task 5 finalizes it). Fix all compile breaks.

- [ ] **Step 4: Confirm LIST-consistency.** Verify `InMemoryBackend::list` gives read-your-writes enumeration (it does — in-memory map); add a one-line comment at `discoverUniverse` naming the backend requirement. Note in the task report which real backends were confirmed (S3 strong-consistent; RustFS to confirm in soak).

- [ ] **Step 5: Run tests, verify pass.** New tests + `gtest_cas_fsck.*` + the full `gtest_cas_gc_*` set + `gtest_cas_store.*`. Run each with `gc_shards=1` AND a `gc_shards>1` store. Redirect + subagent-analyze.

- [ ] **Step 6: Commit** (`CA: discover namespaces from LIST(cas/refs/); delete gc/registry`).

---

### Task 5: Newborn = precommit-state shard (self-floor to `gcRound`)

**Files:**
- Modify: `Core/CasBuild.cpp:582` (`precommitAdd`) — when the target shard is absent, create it fenced to the current `gcRound` (read from `gc/state`); `Core/CasBuild.cpp:628` (`promote`) — gate on the shard's `fence_round` instead of the deleted `registry_fence`.
- Modify: `Core/CasStore.{h,cpp}` — a `Store` helper `uint64_t currentGcRound()` reading `gc/state` (reuse the existing `gcStateKey`/`decodeGcState` path); `mutateShard` create path stamps `fence_round = birth_floor` when a `birth_floor` is supplied.
- Test: `gtest_cas_gc_shard_incarnation.cpp`, and the existing `gtest_cas_gc_leak.cpp` / `gtest_cas_retire_view.cpp` create-race harness.

**Interfaces:**
- Consumes: `mutateShard(..., ShardIncarnation, uint64_t birth_floor)` — extend Task 2's create path to also stamp `root.fence_round = birth_floor` on a fresh object.
- Produces: a newborn ref-shard object present + fenced to `gcRound` at first `precommitAdd`; `promote` gated on `wView >= man[shardOf(ref)].fence` (the shard self-floor) — mirrors the phase-0 model's `WPublish` gate.

- [ ] **Step 1: Write the failing create-race test** (the THM-NO-RETURN case).
```cpp
TEST(CasGcShardIncarnation, NewbornPrecommitProtectsDedupBlobAgainstConcurrentDrop)
{
    // ns A shard s1 holds a committed ref to blob b1 (indeg 1).
    // Start a GC round R (fence). Concurrently: writer publishes b1 (dedup) into NEWBORN ns B
    //   via precommitAdd -> shard born fenced to R; then WDrop b1 from A.
    // Run GC fold/retire/recheck. ASSERT b1 is NOT deleted (B's precommit +1 or the self-floor
    //   blocks the promote) -> no committed ref to B dangles.
}
```

- [ ] **Step 2: Run, verify it fails** (without the self-floor, a newborn's fence is 0 → create-race dangles — mirrors `SabotageNewbornNoFloor`).

- [ ] **Step 3: Implement the self-floor + promote gate.** In `precommitAdd`, before the `mutateShard`, read `store->currentGcRound()` and pass it as `birth_floor` to `mutateShard` (only meaningful on create-if-absent; an existing shard keeps its fence). In `promote`, replace the `registry_fence` gate with `wView >= man[shardOf(final_ref_name)].fence` semantics — i.e., ensure the writer's retire-view has reached the shard's `fence_round` before committing (the existing publish-gate / retire-view refresh path; keep `DepOK`/`CondemnedAtView` intact). Confirm the abandoned-newborn path: a crashed precommit is reclaimed by the existing `reclaimAbandonedPrecommit` watermark (`CasGc.cpp:1604`) — no new logic.

- [ ] **Step 4: Run tests, verify pass.** The create-race test + `gtest_cas_gc_leak.*` + `gtest_cas_retire_view.*` + `gtest_cas_build.*`, `gc_shards` 1 and >1. Redirect + subagent-analyze.

- [ ] **Step 5: Commit** (`CA: newborn shard self-floors to current gcRound (registry-free create-ordering)`).

---

### Task 6: Reclaim ref-shard objects like blobs (tombstone + token-guarded delete)

**Files:**
- Modify: `Core/CasRootShardCodec.h` + `cas_format.proto` — a drop-tombstone owner-event (extend `RootOwnerEvent`/its proto with a `tomb` marker, or a dedicated boolean on the shard; prefer an in-band journal event so "last event == tombstone" is checkable and the fold covers it).
- Modify: `Core/CasStore.cpp:942` (`dropNamespace`) — after clearing refs, append the tombstone event as the last journal entry per touched shard.
- Modify: `Core/CasGc.cpp` — a reclaim step: for each discovered shard that is empty (no refs), tombstoned (last journal event is the tombstone), and fully folded past the fence at the current incarnation, `deleteExact(rootShardKey(ns,shard), token)` (mirror the blob delete at `CasGc.cpp:800`).
- Test: `gtest_cas_gc_shard_incarnation.cpp`, `gtest_cas_truncate_reclaim.cpp`.

**Interfaces:**
- Consumes: incarnation-keyed fold (Task 3), LIST discovery (Task 4).
- Produces: GC deletes empty+tombstoned+fully-folded ref-shard objects; a concurrent writer append (revive) changes the token → `deleteExact` returns `TokenMismatch` → the object survives (fail-closed, no journal loss); a later publish recreates the object with a strictly greater incarnation (Task 2) → Task 3 folds it from 0.

- [ ] **Step 1: Write the failing tests.**
```cpp
TEST(CasGcShardIncarnation, DroppedShardObjectIsReclaimed)
{
    // publish + drop all refs + dropNamespace (tombstone); run GC rounds until fully folded
    // ASSERT rootShardKey(ns,0) object is deleted (backend GET returns nullopt)
}
TEST(CasGcShardIncarnation, ReviveRacesReclaimAborts)
{
    // between GC's read and delete, a writer appends (revive) -> token changes
    // ASSERT deleteExact returns TokenMismatch and the object survives with the new content
}
TEST(CasGcShardIncarnation, IdleButLiveShardNotReclaimed)
{
    // shard empty (all parts dropped) but NO tombstone (table alive) -> NOT reclaimed
}
```

- [ ] **Step 2: Run, verify they fail** (no tombstone; no reclaim step).

- [ ] **Step 3: Implement tombstone + reclaim.** Add the tombstone owner-event to the codec/proto (fail-closed decode). `dropNamespace` appends it as the last event per touched shard (after the removal events at `CasStore.cpp:962-969`). Add a GC reclaim step (in the round, after recheck/trim) that, for each discovered shard, checks empty ∧ last-event-is-tombstone ∧ `cursor.incarnation == live incarnation` ∧ `cursor.pos >= Len(journal)` (fully folded, tombstone included) and issues the token-guarded `deleteExact`; tolerate `NotFound`/`TokenMismatch` (never throw — [[feedback_ca_gc_never_throw_on_404]]).

- [ ] **Step 4: Run tests, verify pass.** The three new tests + `gtest_cas_truncate_reclaim.*` + full `gtest_cas_gc_*`, `gc_shards` 1 and >1. Redirect + subagent-analyze.

- [ ] **Step 5: Commit** (`CA: reclaim empty tombstoned ref-shard objects (token-guarded)`).

---

### Task 7: TLA+ regression + scenario/soak validation

**Files:**
- `docs/superpowers/models/CaBuildRootPrecommit.tla` (+ its cfgs) — re-run; extend only if precommit/promote's modeled contract changed.
- `utils/ca-soak/scenarios/cards/` — a create/drop-churn scenario card.
- Test: TLC; the scenario suite; a soak run.

**Interfaces:**
- Consumes: the Task 5 precommit/promote changes.

- [ ] **Step 1: Re-run `CaBuildRootPrecommit` (mandatory, Global Constraint).**
`java -XX:+UseParallelGC -cp <tla2tools.jar> tlc2.TLC -workers auto -config CaBuildRootPrecommit_fixed.cfg CaBuildRootPrecommit.tla` and each other `_*.cfg`. Expected: the `_fixed`/witness configs hold, the sabotage configs still produce counterexamples. If Task 5 changed the modeled precommit/promote ordering (self-floor replacing the registry floor), add a variant reflecting it and confirm the safety property holds. Record results in `CaBuildRootPrecommit_RESULTS.md`.

- [ ] **Step 2: Add a create/drop-churn scenario card.** In `utils/ca-soak/scenarios/cards/`, add a scenario that creates and drops many namespaces, then asserts (via `assert_reclaimable_drained`) `reclaimable == 0` AND the "other" residual is bounded (registry + empty shard objects gone), and that per-round GC work is ∝ live namespaces. Follow the existing card structure and the prefix-aware drain assertion.

- [ ] **Step 3: Run the scenario suite.** Confirm S30 converges (no monotone "other" growth) and all drain verdicts pass. Redirect output; subagent-analyze.

- [ ] **Step 4: Soak.** Rebuild the binary; fresh ca-soak restart per [[reference_ca_soak_fresh_restart]]; run a bounded soak (create/drop churn workload); confirm `fsck unreachable=0, dangling=0`, `gc_residual=0`, and bounded storage. Subagent-analyze the soak logs.

- [ ] **Step 5: Commit** (`CA: TLA+ CaBuildRootPrecommit regression + create/drop churn scenario (D1 validation)`).

---

## Self-Review

**Spec coverage:** §1 incarnation → Tasks 1–3; §2 discovery-from-refs + registry deletion → Task 4; §3 precommit-newborn self-floor → Task 5; §4 tombstone reclaim → Task 6; §5 fence rework → folded into Task 4 (fence only present shards) + Task 5 (self-floor); §6 invariants → phase-0 gate (done) + Task 7 regression; §9 testing (gc_shards>1, scenario, soak) → Tasks 4/6 (sharded) + Task 7. INC-MONO obligation → Task 2 Step 4. LIST-consistency → Task 4 Step 4. All spec sections mapped.

**Placeholder scan:** No "TBD"/"handle edge cases". Test bodies with pseudocode comments (e.g. Task 5 Step 1) describe the exact scenario a fresh implementer builds with the cited existing harness; the assertions are concrete. This is intentional for the create-race/reclaim tests, which are scenario-shaped rather than single-call.

**Type consistency:** `ShardIncarnation{writer_epoch, build_sequence}` used identically in Tasks 1–3, 5, 6. `mutateShard(..., ShardIncarnation birth_incarnation, uint64_t birth_floor)` introduced in Task 2, extended in Task 5 — the plan states the extension explicitly. `discoverUniverse` return shape consistent across Tasks 4/6.

**Ordering note:** Tasks 1→2→3 are prerequisite-linked (field → stamp → cursor). Task 4 (registry deletion) depends only on Task 1's discovery not needing the registry — but Task 4 removes `ensureRegistered`, so Task 5 (which reworks the gate that used its return) must follow Task 4. Task 6 (reclaim) needs Tasks 2+3 (incarnation) and 4 (discovery). Execute in numeric order.
