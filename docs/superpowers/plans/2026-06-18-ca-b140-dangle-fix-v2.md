# B140-dangle Fix v2 (cursor-in-snap) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the CA GC from deleting a deduplicated blob a live part still references (`fsck dangling`) by making the per-shard fold cursor a single source of truth that lives *inside* the GC snap.

**Architecture:** Today the fold cursor lives in `gc/state.folded_cursor`, separate from the snap edges (`gc/snap/<gen>`), so it can run ahead of the snap's real fold extent (cross-leader); the gated journal trim then over-trims and a live shared-blob edge is lost. The fix moves the cursor *into* the snap (one write-once `{edges, cursor}` object), deletes `gc/state.folded_cursor`, and adds a fail-closed snap↔journal coherence guard as the no-loss net. Validated by `CaB140DangleMerge.tla` (merged = clean, 5.33M states) and the RED gtest `CasGcDangle`.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), TLC (TLA+). Build dir `build/`. Branch `cas-mergetree-poc`.

**Spec:** `docs/superpowers/specs/2026-06-18-ca-b140-dangle-fix-v2-design.md`

**Conventions (CLAUDE.md):** Allman braces; "exception" not "crash"; no `-j`/`nproc` on ninja; redirect every build/test to a log in `build/` and have a subagent summarize it; commit trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; wrap literal `MergeTree`/symbol/log names in backticks in commit messages.

---

## File Structure

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h` — add a `folded_cursor` field to `GcSnap` (public, after `generation`).
- `…/Core/CasGcSnap.cpp` — serialize/deserialize `folded_cursor` in `encodeSnapFields`/`decodeSnapFields`; bump `GC_SNAP_VERSION`.
- `…/Core/CasGcFormats.h` — delete `GcState::folded_cursor`.
- `…/Core/CasGcFormats.cpp` — delete `folded_cursor` from the `gc/state` JSON encode/decode + the validated-keys list.
- `…/Core/CasGc.cpp` — read the cursor from the committed snap (fold seed, recheck, trim) instead of `gc/state`; at commit write the advanced cursor into the persisted snap and stop touching `gc/state.folded_cursor`; add the round-start snap↔journal coherence guard.
- `src/Disks/tests/gtest_cas_b140_dangle.cpp` — adjust the injection to set the cursor in the snap (the field moved); it goes green via the guard.
- `src/Disks/tests/gtest_cas_gc_snap.cpp` — add the structural adoption-rejection unit test.

Build target for all tests: `unit_tests_dbms`. Test binary: `build/src/unit_tests_dbms`.

---

## Task 1: Put the fold cursor inside the snap (codec) + cursor-aware adoption

**Files:**
- Modify: `…/Core/CasGcSnap.h` (struct field, ~`:49`)
- Modify: `…/Core/CasGcSnap.cpp` (`encodeSnapFields` ~`:264`, `decodeSnapFields` ~`:305`, `GC_SNAP_VERSION`)
- Test: `src/Disks/tests/gtest_cas_gc_snap.cpp`

- [ ] **Step 1: Write the failing (compile-RED) structural test**

Append to `src/Disks/tests/gtest_cas_gc_snap.cpp`:

```cpp
/// B140-dangle fix: the per-shard fold cursor is part of the snap's durable identity, so two snaps
/// with identical edges but DIFFERENT cursors encode to DIFFERENT bytes. This is what makes
/// byte-equal generation adoption cursor-aware (a leader cannot adopt an edge-equal snap folded to a
/// different cursor), structurally preventing the cursor-skip under-count.
TEST(CasGcSnap, CursorIsPartOfSnapIdentity)
{
    using namespace DB::Cas;
    GcSnap a;
    a.snap_shard = 0;
    a.generation = 7;
    a.addRootEdge("srv1/tbl/0", "part_1", UInt128{0xABCD});
    GcSnap b = a;                                  /// identical edges/expanded/known

    a.folded_cursor["srv1/tbl/0"] = 10;
    b.folded_cursor["srv1/tbl/0"] = 11;            /// only the cursor differs

    EXPECT_NE(encodeGcSnap(a), encodeGcSnap(b))
        << "snaps folded to different cursors must not be byte-equal (else adoption is cursor-blind)";

    /// Round-trips faithfully.
    EXPECT_EQ(decodeGcSnap(encodeGcSnap(a)).folded_cursor.at("srv1/tbl/0"), 10u);
}
```

- [ ] **Step 2: Build + run; verify it fails to compile (field absent)**

Run (redirect; subagent-summarize):
```
cd build && ninja unit_tests_dbms > build_t1.log 2>&1
```
Expected: compile error — `GcSnap` has no member `folded_cursor`. (This is the RED state.)

- [ ] **Step 3: Add the field**

In `CasGcSnap.h`, immediately after `uint64_t generation = 0;` (~`:49`):

```cpp
    /// Per-root-shard fold watermark ("ns/shard" -> folded shard_version) that THIS snap's edges
    /// represent. The single source of truth for the fold cursor (B140-dangle fix): it lives in the
    /// snap, NOT in gc/state, so a generation's (edges, cursor) can never diverge — they are the same
    /// write-once bytes. With snap_shards==1, shard 0 carries the map over all root shards.
    std::map<String, uint64_t> folded_cursor;
```

- [ ] **Step 4: Serialize it (and reset the codec version to v1)**

In `CasGcSnap.cpp`: reset the version constant (find `GC_SNAP_VERSION` — currently `3`). Per the spec/user, the feature was never on prod, so there is no migration — the cursor-carrying snap is a fresh v1 lineage with no back-compat. A pre-existing snap of any other version is rejected by the existing version guard (`:417-420`, fail-closed) and the GC rebuilds from cursor 0:

```cpp
constexpr uint8_t GC_SNAP_VERSION = 1;   /// v1 of the cursor-carrying snap (B140-dangle fix); no migration, no back-compat.
```

In `encodeSnapFields`, after the `known` block (before `return std::move(body.str());`, ~`:298`):

```cpp
    writeBinaryLittleEndian(static_cast<uint32_t>(snap.folded_cursor.size()), body);
    for (const auto & [key, version] : snap.folded_cursor)        /// std::map => sorted key order (canonical)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(key.size()), body);
        writeString(key, body);
        writeBinaryLittleEndian(version, body);
    }
```

In `decodeSnapFields`, after the `known` loop (before `return snap;`, ~`:368`):

```cpp
    uint32_t cursor_count = 0;
    readBinaryLittleEndian(cursor_count, body);
    for (uint32_t i = 0; i < cursor_count; ++i)
    {
        uint16_t key_len = 0;
        readBinaryLittleEndian(key_len, body);
        const String key = readFixedBytes(body, key_len);
        uint64_t version = 0;
        readBinaryLittleEndian(version, body);
        snap.folded_cursor[key] = version;
    }
```

(Adoption is automatically cursor-aware: `cascadeAndPersist` compares generations by `encodeGcSnap` bytes — `CasGc.cpp:434` `existing->bytes != body` — so a different cursor now makes a generation diverge and probe upward. No change needed there.)

- [ ] **Step 5: Build + run the test; verify it passes**

```
cd build && ninja unit_tests_dbms > build_t1.log 2>&1
./src/unit_tests_dbms --gtest_filter='CasGcSnap.*' > test_t1.log 2>&1
```
Expected: `CasGcSnap.CursorIsPartOfSnapIdentity` PASSED; all other `CasGcSnap.*` still PASS.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.cpp \
        src/Disks/tests/gtest_cas_gc_snap.cpp
git commit -m "CA B140-dangle: fold cursor is part of the GcSnap (codec reset to v1, cursor-aware adoption)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Delete `gc/state.folded_cursor`; read/advance the cursor via the snap

**Files:**
- Modify: `…/Core/CasGcFormats.h` (`GcState::folded_cursor`, `:51`)
- Modify: `…/Core/CasGcFormats.cpp` (encode `:89-90`, decode `:130`, validated keys `:115`)
- Modify: `…/Core/CasGc.cpp` (fold seed `:124`, recheck `:195`, `trim` `:118-152`, `cascadeAndPersist` `:448-459`)

This task has no new test of its own — it is a behavior-preserving relocation guarded by the **existing** GC round/leak/reuse tests (Step 6). It must compile and keep those green.

- [ ] **Step 1: Add a cursor accessor on the committed snap**

In `CasGc.cpp`, near the top of the anonymous helpers, add a free helper (the GC holds the round's snap as `std::map<uint64_t, GcSnap> snap` keyed by snap_shard; `snap_shards==1` is enforced at `:1005`):

```cpp
namespace
{
/// The committed fold cursor for (ns, shard), read from the snap (the single source of truth).
/// snap_shards==1 is enforced, so the whole folded_cursor map lives in snap shard 0.
uint64_t cursorOf(const std::map<uint64_t, GcSnap> & snap, const String & cursor_key)
{
    const auto shard_it = snap.find(0);
    if (shard_it == snap.end())
        return 0;
    const auto it = shard_it->second.folded_cursor.find(cursor_key);
    return it != shard_it->second.folded_cursor.end() ? it->second : 0;
}
}
```

- [ ] **Step 2: Read the cursor from the snap at the fold seed and recheck**

`CasGc.cpp:124-127` (fold seed) — replace the `state.folded_cursor.find(cursor_key)` lookup with `cursorOf(<the round's snap>, cursor_key)`. `CasGc.cpp:195-196` (recheck) — same. (Both currently do `const auto cursor_it = state.folded_cursor.find(cursor_key); … cursor = cursor_it != end ? cursor_it->second : 0;` — replace with `const uint64_t cursor = cursorOf(snap, cursor_key);` using the snap object in scope at each site.)

- [ ] **Step 3: Make `trim` gate by the snap cursor**

`CasGc.cpp:118-152` `Gc::trim` — change its signature to also take the committed snap and read the cursor from it:

```cpp
void Gc::trim(const std::map<uint64_t, GcSnap> & snap,
              const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards)
{
    for (const auto & [ns, shard] : root_shards)
    {
        const String cursor_key = ns.string() + "/" + std::to_string(shard);
        const uint64_t cursor = cursorOf(snap, cursor_key);
        if (cursor == 0)
            continue;
        /// … rest unchanged (the std::erase_if at :147 still gates by `record.at_version <= cursor`) …
```

Update the declaration in `CasGc.h:280` and the two call sites (`CasGc.cpp:99` regular round, `:994` full GC) to pass the round's snap (`folded.snap` at `:99`; the discovered snap at `:994`).

- [ ] **Step 4: At commit, write the cursor into the snap; stop touching `gc/state`**

`CasGc.cpp` `cascadeAndPersist`:
- Where the post-strip snap is built/persisted (`:418-446`), set, for each fenced `(cursor_key, fence_version)` in `fence_it->second` (skip `"_registry"`), `shard_snap.folded_cursor[cursor_key] = std::max(shard_snap.folded_cursor[cursor_key], fence_version)` **before** `encodeGcSnap` — so the persisted bytes carry the advanced cursor. (Do this on snap shard 0.)
- Delete the `for (const auto & [cursor_key, fence_version] : fence_it->second) { … next.folded_cursor … }` block at `:450-459`.
- Keep the `gc/state` CAS, now advancing only `snap_generation` (and erasing `fence_version[<= round]`).
- **Idle-round behavior:** the existing `snap_changed` guard (`:414`) already skips persisting an unchanged snap. With the cursor in the snap, an idle round (no strips, no fence-window records, no prune) does NOT persist and so does NOT advance the cursor — which is correct and conservative (next round re-folds an empty window). Leave the `snap_changed` skip as is.

- [ ] **Step 5: Delete `GcState::folded_cursor` and its codec**

- `CasGcFormats.h:51` — delete `std::map<String, uint64_t> folded_cursor;`.
- `CasGcFormats.cpp:89-90` — delete the `writeJsonKey(out, "folded_cursor"); writeU64MapObject(out, state.folded_cursor);` lines.
- `CasGcFormats.cpp:115` — remove `"folded_cursor"` from the validated-keys list.
- `CasGcFormats.cpp:130` — delete the `state.folded_cursor = u64MapFromObject(...)` line.
- Bump the `gc/state` JSON version if the codec carries one (search `gc/state` version constant near the encoder); old `gc/state` objects are treated as absent and rebuilt (no migration — pre-GA).

- [ ] **Step 6: Build + run the GC regression suite; verify green**

```
cd build && ninja unit_tests_dbms > build_t2.log 2>&1
./src/unit_tests_dbms --gtest_filter='CasGcRound.*:CasGcSnap.*:CasReuseGcRace.*' > test_t2.log 2>&1
```
Expected: all PASS (behavior-preserving relocation). `CasGcLeak.*` stays its known RED; `CasGcDangle.*` is handled in Task 4.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp
git commit -m "CA B140-dangle: delete gc/state.folded_cursor; read/advance the cursor via the snap

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Round-start snap↔journal coherence guard (the fail-closed no-loss net)

**Files:**
- Modify: `…/Core/CasGc.cpp` (add the guard; call it at the start of `runRegularRound` before any retire/delete)

- [ ] **Step 1: Implement the guard**

Add to `CasGc.cpp` (a private method `Gc::assertSnapJournalCoherent`, declared in `CasGc.h`). For each live root shard, fold the shard's journal records up to the snap cursor in memory and require the snap to reflect every *latest-for-ref* `Add` at or below the cursor as a present root edge:

```cpp
void Gc::assertSnapJournalCoherent(const std::map<uint64_t, GcSnap> & snap,
                                   const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards)
{
    const auto snap_it = snap.find(0);
    if (snap_it == snap.end())
        return;                                   /// cold start: nothing committed yet
    const GcSnap & s = snap_it->second;
    for (const auto & [ns, shard] : root_shards)
    {
        const String cursor_key = ns.string() + "/" + std::to_string(shard);
        const uint64_t cursor = cursorOf(snap, cursor_key);
        if (cursor == 0)
            continue;
        const auto [root, token] = store->readShard(ns, shard);
        /// latest record per ref name within the journal:
        std::map<String, const JournalRecord *> latest;
        for (const JournalRecord & rec : root.journal)
        {
            auto it = latest.find(rec.ref_name);
            if (it == latest.end() || it->second->at_version < rec.at_version)
                latest[rec.ref_name] = &rec;
        }
        for (const auto & [ref_name, rec] : latest)
        {
            if (rec->op != JournalRecord::Op::Add || rec->at_version > cursor)
                continue;                          /// only a folded, still-live Add must be reflected
            const UInt128 tree_hash = hexToU128(rec->tree_id.string());
            if (!s.isKnown(ObjectKind::Tree, tree_hash))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc: snap/journal incoherent — ref '{}' (ns {} shard {}) Add@{} is at or below the "
                    "folded cursor {} but its tree {} is absent from the snap (B140-dangle cursor-skip); "
                    "refusing to retire/delete this round",
                    ref_name, ns.string(), shard, rec->at_version, cursor, rec->tree_id.string());
        }
    }
}
```

(Match the real `JournalRecord` field/enum names — `op`, `ref_name`, `tree_id`, `at_version`, and the `Add` enumerator — confirmed in `CasRootShardCodec.h`; adjust if they differ. `isKnown(Tree, …)` is the cheap "reflected" check: a folded live Add expands its tree, seeding it into `known`.)

- [ ] **Step 2: Call it before any delete**

In `Gc::runRegularRound`, immediately after the fold produces `folded.snap` and before `recheck`/`retire` (around `CasGc.cpp:91`), add:

```cpp
    assertSnapJournalCoherent(folded.snap, folded.root_shards);
```

- [ ] **Step 3: Build**

```
cd build && ninja unit_tests_dbms > build_t3.log 2>&1
```
Expected: compiles clean.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h
git commit -m "CA B140-dangle: fail-closed snap<->journal coherence guard before any GC delete

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Turn the `CasGcDangle` RED gtest green (cursor moved to the snap)

**Files:**
- Modify: `src/Disks/tests/gtest_cas_b140_dangle.cpp` (set the cursor in the snap, not `gc/state`)

- [ ] **Step 1: Adjust the injection to set the cursor in the snap**

In `gtest_cas_b140_dangle.cpp`, the injection currently sets `GcState st; st.folded_cursor[cursor_key] = cursor_past_both;`. `GcState::folded_cursor` no longer exists. Move the cursor into the snap and drop it from the state:

```cpp
    snap.folded_cursor[cursor_key] = cursor_past_both;   /// cursor AHEAD of the snap's real extent
    snap.snap_shard = 0;
    snap.generation = 1;

    GcState st;
    st.snap_generation = 1;
    st.snap_shards = 1;
    /// (no folded_cursor on GcState anymore — it lives in the snap)
```

Leave the `EXPECT_EQ(rep.dangling, 0u)` assertion unchanged.

- [ ] **Step 2: Build + run; verify GREEN**

```
cd build && ninja unit_tests_dbms > build_t4.log 2>&1
./src/unit_tests_dbms --gtest_filter='CasGcDangle.*' > test_t4.log 2>&1
```
Expected: `CasGcDangle.SharedBlobUnderCountDeletesLivePinnedBlob` PASSED — the round-start guard (Task 3) detects that `rb_cur`'s Add is at/below the cursor but `T_cur` is absent from the snap, throws `CORRUPTED_DATA`, and the GC round deletes nothing, so `B` survives and `dangling == 0`. (`runGcToFixpoint` swallows the round; confirm `B_present` is true / `dangling==0`.)

- [ ] **Step 3: Run the full GC test set; confirm the invariants**

```
./src/unit_tests_dbms --gtest_filter='CasGcDangle.*:CasReuseGcRace.*:CasGcLeak.*:CasGcRound.*:CasGcSnap.*' > test_t4_all.log 2>&1
```
Expected: all PASS **except** the intentional `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` (B140-leak, still RED, out of scope). `CasReuseGcRace.ReuseOfBlobDeletedBeforePublish` PASS; `CasGcDangle.*` PASS.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/tests/gtest_cas_b140_dangle.cpp
git commit -m "CA B140-dangle: CasGcDangle goes green — cursor-in-snap + coherence guard

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Full suite + TLA+ confirmation + spec/backlog update

**Files:**
- Modify: `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (mark B140-dangle fixed)

- [ ] **Step 1: Run the whole CA unit-test surface**

```
./src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*' > test_t5.log 2>&1
```
Expected: green except the one known B140-leak RED. Have a subagent summarize `test_t5.log`.

- [ ] **Step 2: Re-confirm the TLA+ oracle (unchanged, sanity)**

```
cd docs/superpowers/models && JAR=../../../tmp/tla2tools.jar
java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto -config m_cursorskip.cfg CaB140DangleMerge.tla   # dangle
java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto -config m_merged.cfg CaB140DangleMerge.tla       # clean
```
Expected: `m_cursorskip` violated; `m_merged` clean (5.33M states).

- [ ] **Step 3: Update the backlog entry**

In `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`, mark `B140-dangle` FIXED, cite the fix commits and the spec, and note the soak re-validation (Task 124) is pending.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/deferred_backlog/cas-mergetree-integration.md
git commit -m "CA backlog: B140-dangle FIXED (cursor-in-snap + coherence guard); soak re-validation pending

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After the plan
Re-soak (separate task #124): rebuild the release `clickhouse`, quick soak (~15 min) → analyze (`dangling=0`, `forgotten_*` healthy), then a 12h soak with chaos; verify `dangling` stays 0 across all `gc_checkpoint`s.
