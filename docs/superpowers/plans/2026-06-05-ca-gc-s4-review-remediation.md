# CA GC S4 Review Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Build/test discipline (project rules):** Build to a log in the build dir (`ninja -C build clickhouse > build/<log> 2>&1`, NO `-j`, NO `nproc`); have a **subagent** summarize the log and return only a concise result. Tests run **FOREGROUND**, bounded (`timeout 590`), with a **non-empty `--test`**; never `clickhouse local`; never background a build or test. Redirect each test to a unique log under the build dir and summarize via a subagent. The worktree dir is named `master` but the branch is `cas-mergetree-poc` — **verify `git branch --show-current` says `cas-mergetree-poc` before every commit** (never commit to `master`). No `rebase`/`amend` — add new commits. Allman braces. Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Goal:** Make the lockless S4 GC path correct (Tier 1: #1, #6, #2, #5, #7) and prove it with oracles that exercise the real writer→log→compaction path, then restore the G1/G3 goals (Tier 2: #3, #4) — without touching Scan B (the surviving `markReachableBlobs` delete gate).

**Architecture:** Tier 1 first (generation accounting + fail-closed session coverage + the data race + the stale lock contracts) so the lockless layer is generation-correct and proven; then Tier 2 (lock-free `GcLogWriter` I/O + a `gc/sealed/<shard>` candidate-discovery index replacing the per-round full bucket scan). The new oracles are the gate. Scan B (the generation-blind `markReachableBlobs`/`identity_reachable_in` re-validate) is the over-protective safety net and is **deliberately untouched** — its replacement is deferred as backlog **B78**.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), gtest (`src/Disks/tests/gtest_content_addressed_gc_s4.cpp`), praktika stateless smoke (`04279_content_addressed_gc`).

**Spec:** `docs/superpowers/specs/2026-06-05-ca-gc-s4-review-remediation-design.md`. **Branch:** `cas-mergetree-poc`. **Backlog tracker:** B70 (this work), B78 (the deferred Scan-B replacement), B71–B77 (deferred minors).

---

## File Structure

All paths under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless noted.

| File | Responsibility | Touched by |
|---|---|---|
| `GcLogWriter.cpp` / `.h` | per-pool delta writer: split, buffer, flush, re-append | #1 (`splitDeltaByShard`), #3 (lock-free I/O + epoch cache), #2 (`flushBufferLocked` clear-before-write) |
| `GcDelta.h` / `.cpp` | the `+`/`-` delta + its serialize/fold codec | reference only (already carries `pin_generations`/`manifest_generation`) |
| `GcCompaction.cpp` / `.h` | per-shard fold → count-0 candidates; `isEpochFolded` | reference only (fold already reads the generations) |
| `PartManifest.h` / `.cpp` | `RefSidecar` (`.meta` bundle) codec | #6 (add settled generations to the sidecar) |
| `ContentAddressedTransaction.cpp` / `.h` | commit (`+` append) + drop (`-` append) + session lifetime | #6 (commit write / drop read), #2 (fail-closed session), #3 fold-in (remove double `persistSession`) |
| `WriteSession.h` / `.cpp` | the durable session (the §7 flag) codec | #2 (sticky state + stored failed delta) |
| `ContentAddressedGC.cpp` / `.h` | the sweep: candidate → seal → grace → re-check → sweep; the reaper | #5 (`pinned_snapshot` into the reconciliation collector), #7 (rename `*Locked`), #4 (`gc/sealed` index), #2 (sticky-session reaping exemption + bounded re-log) |
| `ContentAddressedMetadataStorage.cpp` / `.h` | owns the writer + `in_flight_pinned_blobs`; `shutdown` | #3 fold-in (`shutdown → flushAll`) |
| `PoolPaths.h` / `.cpp` | object-key builders + shard fn | #4 (`gcSealedPrefix`/`gcSealedKey` + a part-shard fn) |
| `src/Disks/tests/gtest_content_addressed_gc_s4.cpp` | the lockless-path oracles | oracles 1–6 |

---

## Phase 1 — #1 generation drop in `splitDeltaByShard` (the blocker) + oracle 1

### Task 1: Failing oracle — generations survive the real writer→log→compaction path

**Files:**
- Test: `src/Disks/tests/gtest_content_addressed_gc_s4.cpp` (add one `TEST_F`)

The current oracles bypass `splitDeltaByShard` by hand-crafting sessions; oracle 2 (`Sec5_1_AppendAsEpochFolds…`, line 279) drives the real `GcLogWriter::appendAndFlushForCommit` → `GcCompaction::compactShard` chain but never with a `g>0` delta. This test pushes a `g>0` delta through the **real** writer and asserts the fold keys it at its real generation — it FAILS today because `splitDeltaByShard` (`GcLogWriter.cpp:76-78`) drops `pin_generations`/`manifest_generation`.

- [ ] **Step 1: Read the `GcCompaction` candidate API.** Open `GcCompaction.h` and confirm the `CompactionResult` shape: it has `uint64_t new_epoch` and a `candidates` collection whose elements expose `.key` of type `CountKey{ KeyKind kind; std::string identity; uint64_t generation; }` (existing test uses `folded.candidates` + `c.key.identity` at `gtest_content_addressed_gc_s4.cpp:310-313`). Note the exact element type name for the loop below.

- [ ] **Step 2: Write the failing test** (append to `gtest_content_addressed_gc_s4.cpp`, before the final closing of the file):

```cpp
TEST_F(ContentAddressedGcS4, Sec6_GenerationsSurviveTheRealWriterPath_WouldHaveCaughtBlocker1)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    /// Pre-advance every shard so the open epoch is 1 (mirrors oracle 2).
    for (ShardId s = 0; s < kGcShardCount; ++s)
        ASSERT_EQ(compaction.compactShard(s, kStillLeader).new_epoch, 1u);

    const BlobHash b = blobHash("g01");
    const PartId p0 = partId("g0");   /// pins b at generation 0
    const PartId p1 = partId("g1");   /// pins b at generation 1 (a resurrected blob)

    /// + for (b, g=0) under part p0 (manifest mg=0).
    {
        GcDelta d;
        d.op = GcDelta::Op::Add;
        d.part_id = p0;
        d.manifest_generation = 0;
        d.pins = {b};
        d.pin_generations = {0};
        d.event_id = GcDelta::computeEventId(p0, GcDelta::Op::Add, 0);
        writer.appendAndFlushForCommit(d);
    }
    /// + for (b, g=1) under part p1 (manifest mg=1) — the resurrected generation.
    {
        GcDelta d;
        d.op = GcDelta::Op::Add;
        d.part_id = p1;
        d.manifest_generation = 1;
        d.pins = {b};
        d.pin_generations = {1};
        d.event_id = GcDelta::computeEventId(p1, GcDelta::Op::Add, 1);
        writer.appendAndFlushForCommit(d);
    }
    /// - for (b, g=0): drop p0. b@g0 now nets to zero; b@g1 is still pinned by p1.
    {
        GcDelta d;
        d.op = GcDelta::Op::Remove;
        d.part_id = p0;
        d.manifest_generation = 0;
        d.pins = {b};
        d.pin_generations = {0};
        d.event_id = GcDelta::computeEventId(p0, GcDelta::Op::Remove, 0);
        writer.enqueue(d);
    }
    writer.flushAll();

    /// Fold every shard and collect the count-0 candidates with their generation.
    bool b_g0_is_candidate = false;
    bool b_g1_is_candidate = false;
    for (ShardId s = 0; s < kGcShardCount; ++s)
    {
        const auto folded = compaction.compactShard(s, kStillLeader);
        for (const auto & c : folded.candidates)
        {
            if (c.key.identity == b.string() && c.key.generation == 0)
                b_g0_is_candidate = true;
            if (c.key.identity == b.string() && c.key.generation == 1)
                b_g1_is_candidate = true;
        }
    }

    /// With #1 fixed: (b,0) nets +1(p0) -1(dropP0) = 0 -> a count-0 candidate; (b,1) is +1(p1) -> NOT a
    /// candidate (the resurrected generation survives, keyed independently at g=1).
    EXPECT_TRUE(b_g0_is_candidate) << "g=0 must net to zero and become a candidate";
    EXPECT_FALSE(b_g1_is_candidate) << "g=1 is still pinned and must NOT be swept — proves the generation survived splitDeltaByShard";
    /// With the #1 bug everything collapses to g=0: count(b,0)=+1+1-1=+1 -> b_g0_is_candidate is FALSE and
    /// no (b,1) key exists -> this test fails, exactly catching the blocker.
}
```

- [ ] **Step 3: Build the unit test and run only this oracle to confirm it FAILS.**

Build (subagent summarizes the log):
```bash
ninja -C build unit_tests_dbms > build/gcrem_t1_build.log 2>&1; echo "exit=$?"
```
Run:
```bash
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressedGcS4.Sec6_GenerationsSurviveTheRealWriterPath_WouldHaveCaughtBlocker1' > build/gcrem_t1_run.log 2>&1; echo "exit=$?"
```
Expected: **FAIL** at `EXPECT_TRUE(b_g0_is_candidate)` (the bug folds everything to g=0; `(b,0)` count is +1, not 0). A subagent reads `build/gcrem_t1_run.log` and confirms the failure is the expected assertion.

- [ ] **Step 4: Commit the failing test.**
```bash
git add src/Disks/tests/gtest_content_addressed_gc_s4.cpp
git commit -m "CA GC remediation: failing oracle 1 — generations must survive the real writer path (#1)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 2: Fix `splitDeltaByShard` — carry the generations into every shard fragment

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp:59-80`

- [ ] **Step 1: Replace the body of `splitDeltaByShard`** with the generation-carrying version. The two changes: (a) copy `delta.manifest_generation` into each fragment's delta in the `inserted` block; (b) iterate `delta.pins` by index and push the paired `delta.pin_generations[i]` (default 0).

Current (verbatim):
```cpp
    auto fragment_for = [&](ShardId shard) -> Fragment &
    {
        auto [it, inserted] = by_shard.try_emplace(shard);
        if (inserted)
        {
            it->second.delta.op = delta.op;
            it->second.delta.event_id = delta.event_id;
            it->second.delta.part_id = delta.part_id;
        }
        return it->second;
    };
    for (const auto & pin : delta.pins)
        fragment_for(shardForHash(pin)).delta.pins.push_back(pin);
    fragment_for(shardForPartId(delta.part_id)).carries_part_edge = true;
    return by_shard;
```
Replace with:
```cpp
    auto fragment_for = [&](ShardId shard) -> Fragment &
    {
        auto [it, inserted] = by_shard.try_emplace(shard);
        if (inserted)
        {
            it->second.delta.op = delta.op;
            it->second.delta.event_id = delta.event_id;
            it->second.delta.part_id = delta.part_id;
            /// CA GC S3 (#1 fix): carry the resolved manifest generation onto every shard fragment. The
            /// fold only applies the (part_id) edge on the home shard (shardForPartId guard), so a fragment
            /// that does not own the edge carries an unused mg — harmless; the home-shard fragment now keys
            /// the manifest at its real `mg` instead of 0.
            it->second.delta.manifest_generation = delta.manifest_generation;
        }
        return it->second;
    };
    /// CA GC S3 (#1 fix): each pin goes to its hash-prefix shard CARRYING its resolved generation (parallel
    /// to delta.pins). An empty pin_generations (an S2 delta, or one read from an older log object) takes
    /// every g as 0, matching the codec/fold default — so the fold keys CountKey{Blob,H,g} at the real g.
    for (size_t i = 0; i < delta.pins.size(); ++i)
    {
        Fragment & fragment = fragment_for(shardForHash(delta.pins[i]));
        fragment.delta.pins.push_back(delta.pins[i]);
        fragment.delta.pin_generations.push_back(i < delta.pin_generations.size() ? delta.pin_generations[i] : 0);
    }
    fragment_for(shardForPartId(delta.part_id)).carries_part_edge = true;
    return by_shard;
```

- [ ] **Step 2: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t2_build.log 2>&1; echo "exit=$?"
```
A subagent reads `build/gcrem_t2_build.log` and reports `error:`/`FAILED:` count (expect 0).

- [ ] **Step 3: Run oracle 1 — confirm it now PASSES.**
```bash
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressedGcS4.Sec6_GenerationsSurviveTheRealWriterPath_WouldHaveCaughtBlocker1' > build/gcrem_t2_run.log 2>&1; echo "exit=$?"
```
Expected: **PASS**. Subagent confirms.

- [ ] **Step 4: Run the full `ContentAddressed*` gtest suite — no regression.**
```bash
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t2_suite.log 2>&1; echo "exit=$?"
```
Expected: all pass (the prior 148 + the new oracle). Subagent reports the pass/fail tail.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp
git commit -m "CA GC remediation #1: splitDeltaByShard carries the resolved generations

Blocker fix: the split dropped pin_generations and manifest_generation, so every gc/log
fragment was keyed g=0 and the S3 generation accounting was inert (leak of resurrected
generations). Carry delta.manifest_generation onto each fragment and push the paired
pin_generations[i] (default 0). Oracle 1 (real writer->log->compaction) now green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 2 — #6 drop-path settled generation via the `.meta` sidecar

After #1, a `-` keyed at `active`'s *current* generation (read via `readActiveGenHint`, `ContentAddressedTransaction.cpp:1936,1947`) will not net against a `+` settled at a different `g` if a resurrection intervened. The fix records the settled `(H,g)` pinset + `mg` in the per-part `RefSidecar` (`.meta` bundle, `refMetaKey`) at commit, and reads it back at drop — no new object.

### Task 3: Extend `RefSidecar` to carry the settled generations

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h:86-96`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.cpp:72-103`

- [ ] **Step 1: Add the generation fields to the `RefSidecar` struct** (`PartManifest.h`). Current:
```cpp
struct RefSidecar
{
    std::map<std::string, std::string> files;

    std::string serialize() const;
    static RefSidecar deserialize(const std::string & bytes);

    /// 4-byte magic `CASC` ("Content-Addressed SideCar") + a 1-byte version, per the shared codec.
    static constexpr FormatMagic MAGIC = makeMagic("CASC");
    static constexpr uint8_t VERSION = 1;
};
```
Replace with:
```cpp
struct RefSidecar
{
    std::map<std::string, std::string> files;

    /// CA GC S3 (#6) — the resolved generations the commit's `+` settled on, recorded per-part so the
    /// DROP path emits its `-` at the SAME generation the `+` used (re-deriving from the racy `active`
    /// hint would mis-key after an intervening resurrection, leaving the old generation's count >0
    /// forever). `manifest_generation` is the part manifest's `mg`; `pin_generations` maps each pinned
    /// bare blob-hash string to its resolved `g`. Empty on a mutable-only/legacy sidecar (every g=0).
    uint64_t manifest_generation = 0;
    std::map<std::string, uint64_t> pin_generations;

    std::string serialize() const;
    static RefSidecar deserialize(const std::string & bytes);

    /// 4-byte magic `CASC` ("Content-Addressed SideCar") + a 1-byte version, per the shared codec.
    static constexpr FormatMagic MAGIC = makeMagic("CASC");
    /// Version 2 (CA GC S3 #6) appends manifest_generation + the (blob-hash -> g) map. A v3 pool is
    /// created fresh (PoolMeta v3, no back-compat), so no v1 sidecar can exist in it — reading only v2
    /// is correct and fail-closed.
    static constexpr uint8_t VERSION = 2;
};
```

- [ ] **Step 2: Extend `serialize`/`deserialize`** (`PartManifest.cpp`). After the existing `files` loop in `serialize` (after line 84, before `buf.finalize()`):
```cpp
    /// CA GC S3 (#6, version 2): the resolved manifest generation, then the (blob-hash -> g) map.
    DB::writeVarUInt(manifest_generation, buf);
    DB::writeVarUInt(pin_generations.size(), buf);
    for (const auto & [hash, g] : pin_generations)
    {
        DB::writeStringBinary(hash, buf);
        DB::writeVarUInt(g, buf);
    }
```
In `deserialize`, after the existing `files` loop (after line 101, before `return s;`):
```cpp
    /// CA GC S3 (#6, version 2): the resolved generations (always present in a v2 object).
    DB::readVarUInt(s.manifest_generation, buf);
    uint64_t ng = 0;
    DB::readVarUInt(ng, buf);
    for (uint64_t i = 0; i < ng; ++i)
    {
        std::string hash;
        DB::readStringBinary(hash, buf);
        uint64_t g = 0;
        DB::readVarUInt(g, buf);
        s.pin_generations.emplace(std::move(hash), g);
    }
```

- [ ] **Step 3: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t3_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 4: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.cpp
git commit -m "CA GC remediation #6: RefSidecar v2 carries the settled generations

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 4: Populate the sidecar generations at commit; preserve them on a mutable-only commit

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (the content-publish sidecar write `1551-1556`, the mutable-only branch `1420-1424`)

- [ ] **Step 1: Populate the generations in the content-publish sidecar write.** At the content-publish branch (`commitOnePart`), the sidecar is built at lines 1551-1553:
```cpp
        ContentAddressed::RefSidecar sidecar;
        sidecar.files = merged_mutable;
        const std::string meta_bytes = sidecar.serialize();
```
`manifest_gen` (line 1489) and `resolved_blob_gen` (line 1471, a `std::map<BlobHash,uint64_t>`) are already in scope. Insert between `sidecar.files = merged_mutable;` and `const std::string meta_bytes = …`:
```cpp
        /// CA GC S3 (#6): record the resolved generations the `+` settled on, so the DROP path emits its
        /// `-` at the matching generation (not the racy `active` hint). resolved_blob_gen / manifest_gen
        /// are the same values threaded into the `+` delta above.
        sidecar.manifest_generation = manifest_gen;
        for (const auto & [hash, g] : resolved_blob_gen)
            sidecar.pin_generations.emplace(hash.string(), g);
```

- [ ] **Step 2: Preserve the generations on a mutable-only commit.** The mutable-only branch (lines 1420-1424) rewrites the sidecar at the SAME `refMetaKey` but for a metadata-only update (no new pin set). It must NOT clobber the generations a prior content publish recorded. Read the existing sidecar first and carry its generations forward. Locate the mutable-only sidecar write (around line 1420):
```cpp
        ContentAddressed::RefSidecar sidecar;
        sidecar.files = merged_mutable;
```
Replace with:
```cpp
        ContentAddressed::RefSidecar sidecar;
        sidecar.files = merged_mutable;
        /// CA GC S3 (#6): a mutable-only update must not erase the generations a prior content publish
        /// recorded (the DROP path reads them). Carry them forward from the existing sidecar if present.
        if (auto existing = metadata_storage.readSmallObjectIfExists(meta_key); existing && !existing->empty())
        {
            try
            {
                const auto prior = ContentAddressed::RefSidecar::deserialize(*existing);
                sidecar.manifest_generation = prior.manifest_generation;
                sidecar.pin_generations = prior.pin_generations;
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
                /// A missing/corrupt prior sidecar leaves the generations empty (every g=0) — the same
                /// conservative default as a legacy object; the DROP path then resolves g=0.
            }
        }
```
NOTE: confirm the mutable-only branch computes `meta_key` before this point (it uses `refMetaKey` per the agent's finding at `1530-1532` for the content branch; the mutable-only branch has its own `meta_key` — if the variable name differs, use that branch's key variable). Verify by reading lines 1410-1430 before editing.

- [ ] **Step 3: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t4_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 4: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "CA GC remediation #6: record settled generations in the sidecar at commit

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 5: Drop path reads the sidecar generations instead of the `active` hint

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` — `unlinkPartDirRefs` (the `-` delta construction `1929-1948`, the part resolve block `1883-1894`)

- [ ] **Step 1: Read the sidecar at drop.** In `unlinkPartDirRefs`, the live part is resolved at lines 1883-1894 (`dropped_part_id`, `dropped_manifest`). Add reading the `RefSidecar` there so its generations are available when the `-` delta is built. After `dropped_manifest = std::move(manifest);` (line 1892), but still inside the `if (auto part_id = …)` block, add:
```cpp
            /// CA GC S3 (#6): read the per-part generations the `+` settled on, so the `-` is keyed to match.
            try
            {
                const std::string meta_key
                    = ContentAddressed::refMetaKey(key_prefix, p->table_uuid, p->part_name).string();
                if (auto bytes = metadata_storage.readSmallObjectIfExists(meta_key); bytes && !bytes->empty())
                    dropped_sidecar = ContentAddressed::RefSidecar::deserialize(*bytes);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
                /// A missing/corrupt sidecar leaves dropped_sidecar empty -> the `-` falls back to g=0
                /// (the legacy/common case). Never block the drop on it.
            }
```
And declare `dropped_sidecar` next to the other optionals (near lines 1883-1884):
```cpp
    std::optional<ContentAddressed::RefSidecar> dropped_sidecar;
```
VERIFY the exact `refMetaKey` overload signature — the live-part key is `refMetaKey(key_prefix, table_uuid, part_name)` (per the commit-side write at `1530-1532` which uses `refMetaKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_)`). Match the SAME overload the commit write used (it includes `server_id`); read `PoolPaths.h:113` and the commit write before editing to copy the exact argument list. At drop, `metadata_storage.server_id`, `p->table_uuid`, `p->part_name` are the corresponding values.

- [ ] **Step 2: Key the `-` delta from the sidecar.** Replace the `active`-hint resolution in the `-` delta construction (lines 1935-1947). Current:
```cpp
            delta.manifest_generation
                = readActiveGenHint(ContentAddressed::partActiveKey(key_prefix, *dropped_part_id));
            delta.event_id
                = ContentAddressed::GcDelta::computeEventId(*dropped_part_id, delta.op, delta.manifest_generation);
            std::set<ContentAddressed::BlobHash> seen;
            delta.pins.reserve(dropped_manifest->blobs.size());
            delta.pin_generations.reserve(dropped_manifest->blobs.size());
            for (const auto & [file, entry] : dropped_manifest->blobs)
            {
                if (!seen.insert(entry.key).second)
                    continue;
                delta.pins.push_back(entry.key);
                delta.pin_generations.push_back(readActiveGenHint(ContentAddressed::blobActiveKey(key_prefix, entry.key)));
            }
```
Replace with:
```cpp
            /// CA GC S3 (#6): key the `-` at the generation the `+` SETTLED on, recorded in the sidecar at
            /// commit. A resurrection between commit and drop changes `active`, so re-deriving from the hint
            /// would mis-key the `-` and leave the old generation's count >0 forever. Fall back to g=0 (the
            /// `active` hint, the legacy/common case) only when the sidecar is absent.
            const auto sidecar_mg = [&]() -> uint64_t
            {
                if (dropped_sidecar)
                    return dropped_sidecar->manifest_generation;
                return readActiveGenHint(ContentAddressed::partActiveKey(key_prefix, *dropped_part_id));
            };
            delta.manifest_generation = sidecar_mg();
            delta.event_id
                = ContentAddressed::GcDelta::computeEventId(*dropped_part_id, delta.op, delta.manifest_generation);
            std::set<ContentAddressed::BlobHash> seen;
            delta.pins.reserve(dropped_manifest->blobs.size());
            delta.pin_generations.reserve(dropped_manifest->blobs.size());
            for (const auto & [file, entry] : dropped_manifest->blobs)
            {
                if (!seen.insert(entry.key).second)
                    continue;
                delta.pins.push_back(entry.key);
                uint64_t g = 0;
                if (dropped_sidecar)
                {
                    if (auto it = dropped_sidecar->pin_generations.find(entry.key.string());
                        it != dropped_sidecar->pin_generations.end())
                        g = it->second;
                }
                else
                    g = readActiveGenHint(ContentAddressed::blobActiveKey(key_prefix, entry.key));
                delta.pin_generations.push_back(g);
            }
```

- [ ] **Step 2a: Add a `RefSidecar` include if needed.** `unlinkPartDirRefs` already uses `PartManifest`; confirm `PartManifest.h` (which declares `RefSidecar`) is included in `ContentAddressedTransaction.cpp` — it is (the commit side constructs `RefSidecar`). No new include.

- [ ] **Step 3: Write a gtest that proves +/- net across an intervening resurrection.** Append to `gtest_content_addressed_gc_s4.cpp`. This is a sidecar-codec + drop-keying unit test driven at the storage level. Use the fixture's `put`/`get`. Construct a `RefSidecar` with `manifest_generation=1` and `pin_generations={{b.string(),1}}`, serialize→deserialize, and assert the round-trip preserves them:
```cpp
TEST_F(ContentAddressedGcS4, Sec6_RefSidecarRoundTripsSettledGenerations)
{
    const BlobHash b = blobHash("s01");
    RefSidecar in;
    in.files["uuid.txt"] = "deadbeef";
    in.manifest_generation = 1;
    in.pin_generations[b.string()] = 1;

    const RefSidecar out = RefSidecar::deserialize(in.serialize());
    EXPECT_EQ(out.files.at("uuid.txt"), "deadbeef");
    EXPECT_EQ(out.manifest_generation, 1u);
    ASSERT_TRUE(out.pin_generations.contains(b.string()));
    EXPECT_EQ(out.pin_generations.at(b.string()), 1u);
}
```
Ensure `#include …/PartManifest.h` is visible to the test (the test file already includes the CA headers via the existing `RefSidecar`-free helpers; add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>` if `RefSidecar` is unresolved).

- [ ] **Step 4: Build + run the new test + full suite.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t5_build.log 2>&1; echo "exit=$?"
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t5_suite.log 2>&1; echo "exit=$?"
```
Expected: 0 build errors; all `ContentAddressed*` pass. Subagent summarizes both logs.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/tests/gtest_content_addressed_gc_s4.cpp
git commit -m "CA GC remediation #6: drop path keys the - from the sidecar generations

The DROP path resolved the - delta's generation from the racy active hint; after #1 that
mis-keys against a + settled at a different g (intervening resurrection), orphaning the old
generation's count. Read the settled (H,g)/mg from the RefSidecar recorded at commit; fall
back to the active hint (g=0) only when the sidecar is absent.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 3 — #2 fail-closed session coverage on `+`-flush failure + oracle 4

Today the `+`-append try/catch (`ContentAddressedTransaction.cpp:1573-1611`) swallows the exception; the ref is still published; `settled_delta_epochs` stays empty; `commit`'s `allSettledEpochsFolded({})` returns `true` (`:1740`) → `releaseSession()` (`:1727`). The reference ends up covered by neither the log nor a session. Fix: a third **sticky** session state (`deltas_failed`) that retains the session, exempts it from lease-reaping, stores the failed `+` delta, and is cleared only when a bounded re-log (run by the GC reaper) lands the `+` and `isEpochFolded` confirms it.

### Task 6: `flushBufferLocked` clears the buffer before the throwing write (zombie-fragment fix)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp:104-130`

- [ ] **Step 1: Move `buffer.fragments.clear()` before the S3 write.** Current `flushBufferLocked` moves fragments into `batch` (lines 113-114) then writes (124-126) then `buffer.fragments.clear()` (129). If the write throws, the moved-from fragments stay in `buffer.fragments` and the next flush serializes junk. Restructure so the buffer is cleared as soon as the batch is built (the fragments are already moved-from at that point), before the throwing write. Current tail:
```cpp
    auto out = object_storage->writeObject(StoredObject(object_key.string()), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();

    last_batch_size.store(batch.deltas.size(), std::memory_order_relaxed);
    buffer.fragments.clear();
}
```
Replace with:
```cpp
    /// Clear the buffer BEFORE the (throwing) write: batch.deltas already MOVED every fragment's delta out,
    /// so buffer.fragments now holds moved-from zombies. If the write throws, leaving them in place would
    /// have the next flush serialize junk gc/log entries. The data we are about to write is captured in
    /// `bytes`; on a throw the caller's #2 fail-closed path re-logs from the durable failed-delta record.
    buffer.fragments.clear();

    auto out = object_storage->writeObject(StoredObject(object_key.string()), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();

    last_batch_size.store(batch.deltas.size(), std::memory_order_relaxed);
}
```

- [ ] **Step 2: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t6_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 3: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp
git commit -m "CA GC remediation #2: flushBufferLocked clears the buffer before the throwing write

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 7: `WriteSession` gains the sticky state + the stored failed delta

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h:26-72`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.cpp` (serialize `13-41`, deserialize `43-80`)

- [ ] **Step 1: Add the sticky fields to `WriteSession`** (`WriteSession.h`). After `delta_epochs` (line 53):
```cpp
    /// CA GC S4 (#2) — fail-closed session coverage. Set true when the commit's `+`-flush threw: the
    /// reference is published but no `+` is durable, so the session must NOT be released and must NOT be
    /// lease-reaped (a crashed-writer leak is acceptable; a dropped pin under the future §6.2 gate is data
    /// loss). `pending_add_delta` is the serialized `+` GcDelta the GC reaper re-logs (idempotent by
    /// event_id) on a bounded path; once re-logged AND folded, the reaper clears sticky and reaps.
    bool deltas_failed = false;
    std::string pending_add_delta;
```

- [ ] **Step 2: Bump the encoding version.** Change `ENCODING_VERSION = 2;` to `3` and update the doc-comment to note v3 appends `deltas_failed` + `pending_add_delta` (fresh-pool, no back-compat).
```cpp
    /// Version 3 (CA GC S4 #2) appends the sticky `deltas_failed` flag and the serialized `pending_add_delta`
    /// the reaper re-logs. A v3 pool is created fresh (PoolMeta v3, no back-compat), so reading only v3 is
    /// correct and fail-closed.
    static constexpr uint8_t ENCODING_VERSION = 3;
```

- [ ] **Step 3: Extend serialize/deserialize** (`WriteSession.cpp`). In `serialize`, after the `delta_epochs` loop (the v2 tail), append:
```cpp
    /// CA GC S4 (#2, v3): the sticky fail-closed state.
    DB::writeBinaryLittleEndian(static_cast<uint8_t>(deltas_failed ? 1 : 0), buf);
    DB::writeStringBinary(pending_add_delta, buf);
```
In `deserialize`, after the `delta_epochs` loop (before `return session;`, after line 78):
```cpp
    /// CA GC S4 (#2, v3): the sticky fail-closed state (always present in a v3 object).
    uint8_t deltas_failed_raw = 0;
    DB::readBinaryLittleEndian(deltas_failed_raw, buf);
    session.deltas_failed = deltas_failed_raw != 0;
    DB::readStringBinary(session.pending_add_delta, buf);
```
Confirm the `serialize` writer object is the same `buf` (named in lines 13-41) and uses `writeBinaryLittleEndian`/`writeStringBinary` (the v2 fields do — match them).

- [ ] **Step 4: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t7_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.cpp
git commit -m "CA GC remediation #2: WriteSession v3 — sticky deltas_failed + stored + delta

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 8: Commit marks the session sticky (not released) when the `+`-flush throws

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` — the `+`-append try/catch (`1573-1611`), `commit`'s release block (`1720-1730`), the `.h` (a member to carry the failure across the part loop)

- [ ] **Step 1: Add a transaction-level failure carrier.** In `ContentAddressedTransaction.h`, near the other commit/session state, add:
```cpp
    /// CA GC S4 (#2): set when a part's `+`-flush threw during commit. The serialized `+` delta is kept so
    /// commit() can stamp it onto the durable session for the GC reaper's bounded re-log.
    bool commit_delta_flush_failed = false;
    std::string failed_add_delta_bytes;
```

- [ ] **Step 2: On a `+`-flush throw, record the delta instead of silently swallowing.** In the `catch (...)` at lines 1604-1610 of `commitOnePart`, after the existing `tryLogCurrentException`, add the failure record. The `delta` local is in scope in the `try`; to serialize it in the `catch`, build it before the `try`'s flush or re-serialize. Simplest: serialize the delta right after constructing it (inside the `try`, before `appendAndFlushForCommit`) into a local, and on catch store that local. Restructure the `try` block:
```cpp
        try
        {
            ContentAddressed::GcDelta delta;
            delta.op = ContentAddressed::GcDelta::Op::Add;
            delta.part_id = part_id;
            delta.manifest_generation = manifest_gen;
            delta.event_id = ContentAddressed::GcDelta::computeEventId(part_id, delta.op, manifest_gen);
            std::set<ContentAddressed::BlobHash> seen;
            delta.pins.reserve(resolved_blob_gen.size());
            delta.pin_generations.reserve(resolved_blob_gen.size());
            for (const auto & [file, entry] : manifest.blobs)
            {
                if (!seen.insert(entry.key).second)
                    continue;
                delta.pins.push_back(entry.key);
                delta.pin_generations.push_back(resolved_blob_gen.at(entry.key));
            }
            /// CA GC S4 (#2): keep the serialized `+` so a flush failure below can stamp it onto the durable
            /// session for the GC reaper's bounded re-log (fail-closed: never drop the session's coverage).
            const std::string this_add_delta_bytes = ContentAddressed::serializeGcDeltaForSession(delta);
            try
            {
                for (auto & shard_epoch : metadata_storage.gcLogWriter()->appendAndFlushForCommit(delta))
                    settled_delta_epochs.push_back(shard_epoch);
            }
            catch (...)
            {
                /// FAIL-CLOSED (#2): the ref is about to be published but the `+` did not land durably.
                /// Record the delta + mark the transaction so commit() makes the session STICKY (retained,
                /// lease-exempt) instead of releasing it. The GC reaper re-logs `pending_add_delta`.
                commit_delta_flush_failed = true;
                failed_add_delta_bytes = this_add_delta_bytes;
                tryLogCurrentException(
                    getLogger("ContentAddressedTransaction"),
                    "CA GC S4 (#2): + flush failed for part " + part_id.string()
                        + " — session will be retained sticky and the + re-logged by the GC reaper");
            }
        }
        catch (...)
        {
            tryLogCurrentException(
                getLogger("ContentAddressedTransaction"),
                "CA GC S2: failed to build the + delta for part " + part_id.string());
        }
```
NOTE the two nested try/catch: the inner one catches a **flush** failure (the #2 fail-closed path); the outer catches a **delta-construction** failure (left swallowed — it cannot leave a published-ref-without-coverage because the ref is published *after*, and a construction throw means nothing was logged and the session is still uncommitted/owned). Keep the ref-publish (lines 1617-1623) unchanged — the data is durable; only the `+` coverage is deferred to the sticky session.

- [ ] **Step 3: Add the session-side GcDelta (de)serialize helpers.** The session stores a serialized GcDelta; reuse the existing `GcDelta` codec. In `GcDelta.h`/`.cpp` the batch codec is `GcLogBatch`; a single-delta helper keeps it simple. Add to `GcDelta.h` (free functions in `namespace DB::ContentAddressed`):
```cpp
/// CA GC S4 (#2): serialize/parse a single GcDelta for durable storage in a WriteSession (the sticky
/// fail-closed `pending_add_delta`). Mirrors the per-delta codec used inside GcLogBatch.
std::string serializeGcDeltaForSession(const GcDelta & delta);
GcDelta deserializeGcDeltaFromSession(const std::string & bytes);
```
Implement in `GcDelta.cpp` by delegating to the existing per-delta `serialize`/`deserialize` (the agent confirmed `GcDelta::serialize`/`deserialize` exist at `GcDelta.cpp:56-115` as the batch element codec — if they are free helpers named differently, reuse those exact symbols). Concretely:
```cpp
std::string serializeGcDeltaForSession(const GcDelta & delta)
{
    /// One delta wrapped in a single-element GcLogBatch — reuses the versioned batch codec so the on-disk
    /// shape is identical to a gc/log object and the reaper can append it verbatim.
    GcLogBatch batch;
    batch.deltas.push_back(delta);
    return batch.serialize();
}

GcDelta deserializeGcDeltaFromSession(const std::string & bytes)
{
    const GcLogBatch batch = GcLogBatch::deserialize(bytes);
    if (batch.deltas.size() != 1)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "ContentAddressed session pending_add_delta must hold exactly one delta, got {}", batch.deltas.size());
    return batch.deltas.front();
}
```
VERIFY the `GcLogBatch` type + its `serialize`/`deserialize` are accessible from `GcDelta.cpp` (they live in `GcLogWriter.h`/`GcDelta.h` — include as needed). Confirm `ErrorCodes::CORRUPTED_DATA` is declared in `GcDelta.cpp` (the deserialize already throws it at line 105).

- [ ] **Step 4: `commit` stamps the sticky session instead of releasing.** Replace the release block (`commit`, lines 1720-1730):
```cpp
    if (session_open)
    {
        session.committed = true;
        session.delta_epochs = settled_delta_epochs;
        persistSession();

        if (allSettledEpochsFolded(settled_delta_epochs))
            releaseSession();
    }
```
with:
```cpp
    if (session_open)
    {
        session.committed = true;
        session.delta_epochs = settled_delta_epochs;
        /// CA GC S4 (#2): if a part's `+` flush threw, the ref is published but no `+` is durable. Mark the
        /// session STICKY (retained, lease-exempt, carrying the serialized `+`) so neither the lease reaper
        /// nor the folded reaper drops it; the GC reaper re-logs `pending_add_delta` and clears sticky once
        /// the re-logged `+` is folded. NEVER release the session in this state.
        if (commit_delta_flush_failed)
        {
            session.deltas_failed = true;
            session.pending_add_delta = failed_add_delta_bytes;
            persistSession();
            return; /// fail-closed: keep the sticky session; do not run the folded-release below.
        }
        persistSession();

        if (allSettledEpochsFolded(settled_delta_epochs))
            releaseSession(); /// already folded — the snapshot covers the reference, drop the pin now.
        /// else: leave the durable committed session for the folded-watermark reaper.
    }
```

- [ ] **Step 5: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t8_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.cpp
git commit -m "CA GC remediation #2: commit makes the session sticky (not released) on + -flush failure

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 9: GC reaper exempts sticky sessions, re-logs the stored `+`, clears on fold; lease-checks exempt sticky

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp` — `reapFoldedSessions` (`890-953`), the lease checks in `sessionPinnedBlobs`/`sessionPinnedPartKeys` (`179`, `210`)

- [ ] **Step 1: Exempt sticky sessions from the lease check** so an expired-but-undurable `+` still pins. In `sessionPinnedBlobs` (line 179) and `sessionPinnedPartKeys` (line 210), the check is:
```cpp
        if (static_cast<int64_t>(session.lease_deadline_unix) < now)
            continue;
```
Replace BOTH with:
```cpp
        /// CA GC S4 (#2): a sticky session (its `+` flush failed and is not yet durably re-logged) is EXEMPT
        /// from lease-expiry reaping — its reference is covered by neither the log nor a fresh session, so
        /// dropping the pin would be the under-count the design forbids. A crashed-writer leak is acceptable
        /// (reclaimed once the reaper re-logs + folds, or by reconciliation); a dropped pin is not.
        if (!session.deltas_failed && static_cast<int64_t>(session.lease_deadline_unix) < now)
            continue;
```

- [ ] **Step 2: In `reapFoldedSessions`, handle the sticky session before the normal rules.** After deserializing the session (line 925) and before the rule-(a) `if (!session.committed) continue;` (line 929), insert the sticky-session bounded re-log:
```cpp
        /// CA GC S4 (#2): a sticky session's `+` flush failed at commit — the ref is published but no `+` is
        /// durable. Re-log the stored `+` (idempotent by event_id), record its settled epochs, and clear the
        /// sticky flag. The session then converts to a normal committed-until-folded session and is reaped by
        /// the folded gate below on a later round. NEVER reap it while still sticky.
        if (session.deltas_failed)
        {
            try
            {
                if (!session.pending_add_delta.empty())
                {
                    const GcDelta add = deserializeGcDeltaFromSession(session.pending_add_delta);
                    GcLogWriter relog_writer(object_storage, key_prefix);
                    std::vector<std::pair<ShardId, uint64_t>> settled = relog_writer.appendAndFlushForCommit(add);
                    relog_writer.flushAll();
                    session.delta_epochs = std::move(settled);
                }
                /// The re-log landed durably: drop sticky. Persist the converted session so a crash after this
                /// point sees a normal committed session, not a sticky one.
                session.deltas_failed = false;
                session.pending_add_delta.clear();
                rewriteSession(key, session);
            }
            catch (...)
            {
                /// The re-log failed again (still-throttled S3): keep the session sticky for the next round.
                tryLogCurrentException(getLogger("ContentAddressedGC"),
                    "CA GC S4 (#2): sticky-session + re-log failed; session kept for the next round");
            }
            continue; /// never reap a session on the same round it converts — re-check foldedness next round.
        }
```

- [ ] **Step 2a: Add a `rewriteSession` helper.** `reapFoldedSessions` reads sessions via `readSmallObject`; it needs to rewrite one. Add a small private helper to `ContentAddressedGC` (declare in `.h`, define in `.cpp`):
```cpp
void ContentAddressedGC::rewriteSession(const std::string & session_key, const WriteSession & session)
{
    const std::string bytes = session.serialize();
    auto out = object_storage->writeObject(StoredObject(session_key), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();
}
```
Confirm `WriteMode` / `StoredObject` are already used in this file (they are — the sweep writes tombstones). The `key` variable in the loop is the session object key (the LIST element), so `rewriteSession(key, session)` is correct.

- [ ] **Step 3: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t9_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 4: Write oracle 4 — fault-injection on the `+`-flush.** Append to `gtest_content_addressed_gc_s4.cpp`. This oracle exercises the reaper directly via a hand-crafted sticky session (the reaper is reached through `runSweepOnce`). It proves: (a) a sticky session survives a sweep even past its lease; (b) after the reaper re-logs the stored `+` and the epoch folds, the session is reaped. Build the `pending_add_delta` with the real `serializeGcDeltaForSession` so the reaper's re-log path is exercised:
```cpp
TEST_F(ContentAddressedGcS4, Sec2_StickySession_NotReapedUntilRelogged_ThenReleasedOnFold)
{
    const BlobHash b = blobHash("k01");
    const PartId p = partId("k01");

    /// A committed sticky session: its `+` flush "failed", so it carries the serialized `+` and deltas_failed.
    /// Its lease is already in the PAST — a non-sticky session would be lease-reclaimed; a sticky one is not.
    GcDelta add;
    add.op = GcDelta::Op::Add;
    add.part_id = p;
    add.pins = {b};
    add.pin_generations = {0};
    add.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);

    WriteSession s;
    s.server_id = "srv";
    s.lease_deadline_unix = 1; /// far in the past relative to the sweep clock below
    s.committed = true;
    s.deltas_failed = true;
    s.pending = {b};
    s.pending_add_delta = serializeGcDeltaForSession(add);
    put(sessionKey(prefix, "sess-sticky"), s.serialize());

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);

    /// Round 1: the reaper re-logs the `+`, clears sticky, and does NOT reap (foldedness rechecked next round).
    gc.runSweepOnce(/*now=*/1'000'000, /*grace=*/0);
    EXPECT_TRUE(exists(sessionKey(prefix, "sess-sticky"))) << "a sticky session is never reaped on the round it converts";
    {
        const WriteSession after = WriteSession::deserialize(get(sessionKey(prefix, "sess-sticky")));
        EXPECT_FALSE(after.deltas_failed) << "the reaper cleared sticky after the bounded re-log landed";
        EXPECT_FALSE(after.delta_epochs.empty()) << "the re-log recorded the settled (shard, epoch)";
    }

    /// Fold every recorded epoch, then run the reaper again: the now-normal committed session is reaped.
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };
    const WriteSession converted = WriteSession::deserialize(get(sessionKey(prefix, "sess-sticky")));
    for (const auto & [shard, epoch] : converted.delta_epochs)
        for (int i = 0; i < 64 && !compaction.isEpochFolded(shard, epoch); ++i)
            compaction.compactShard(shard, kStillLeader);
    gc.runSweepOnce(/*now=*/2'000'000, /*grace=*/0);
    EXPECT_FALSE(exists(sessionKey(prefix, "sess-sticky"))) << "once re-logged + folded, the converted session is reaped";
}
```

- [ ] **Step 5: Build + run oracle 4 + full suite.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t9b_build.log 2>&1; echo "exit=$?"
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t9b_suite.log 2>&1; echo "exit=$?"
```
Expected: 0 build errors; all `ContentAddressed*` pass (incl. oracle 4 and the unchanged reaper-race oracle). Subagent summarizes.

- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h src/Disks/tests/gtest_content_addressed_gc_s4.cpp
git commit -m "CA GC remediation #2: reaper exempts + re-logs sticky sessions; oracle 4

Sticky sessions (a + flush that failed at commit) are exempt from lease-reaping and the
folded reaper; the reaper re-logs the stored + (idempotent by event_id), records the settled
epochs, clears sticky, and reaps only once the re-logged + is folded. Oracle 4 proves the
fail-closed retain + bounded re-log + folded-release.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 4 — #5 unlocked read + #7 stale `*Locked` contracts (do together)

`collectReconciliationCandidatesLocked` reads the shared `in_flight_pinned_blobs` `std::set` unlocked (`ContentAddressedGC.cpp:883`) — UB while a commit mutates it. The fix passes the `pinned_snapshot` (already taken at both call sites) into the collector, mirroring `sweepCandidatesLocked` (line 458). The same change drops the now-misleading `*Locked` names/contracts (S4 removed the lock-held precondition; the callers hold no lock).

### Task 10: Thread `pinned_snapshot` into the reconciliation collector (#5) and rename the three functions (#7)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp` (`359`, `608`, `835` defs; `806`, `814`, `821`, `964`, `968`, `970` call sites)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h` (the three declarations)

- [ ] **Step 1: Fix the data race (#5) — `collectReconciliationCandidates` reads from a snapshot.** Change the signature to take a `pinned_snapshot` and replace the unlocked read. Current definition head (line 835):
```cpp
std::set<std::string> ContentAddressedGC::collectReconciliationCandidatesLocked(int64_t now)
```
New (renamed per #7 + the new param):
```cpp
std::set<std::string> ContentAddressedGC::collectReconciliationCandidates(
    int64_t now, const std::set<std::string> & pinned_snapshot)
```
Replace the unlocked read at lines 883-884:
```cpp
        if (in_flight_pinned_blobs && in_flight_pinned_blobs->contains(identity_key.string()))
            continue;
```
with:
```cpp
        /// CA GC S4 (#5): read the B52 in-flight pin from the lock-free SNAPSHOT taken at the start of the
        /// sweep, not from the shared std::set a concurrent commit is mutating (the data race this fixes).
        if (pinned_snapshot.contains(identity_key.string()))
            continue;
```

- [ ] **Step 2: Rename the other two functions + drop the stale contracts (#7).** Rename `sweepCandidatesLocked` → `sweepCandidates` and `collectSealedTombstoneCandidatesLocked` → `collectSealedTombstoneCandidates` (definitions at lines 359, 608; declarations in `.h`). For each of the three, rewrite the doc-contract: replace the "MUST be called with gc_lock held" sentence with the S4 lock-free convention. Concretely:
  - `sweepCandidates` (line 366-368): replace "MUST be called with gc_lock held by the caller." with: `/// CA GC S4 (G1): runs LOCK-FREE — the gc_lock is no longer held across the sweep. Reads in-flight pins only from the supplied lock-free pinned_snapshot (never the shared set).`
  - `collectSealedTombstoneCandidates` (line 613): replace "MUST be called with gc_lock held." with: `/// CA GC S4 (G1): runs lock-free (the gc_lock is not held across the sweep).`
  - `collectReconciliationCandidates` (line 839): replace "MUST be called with gc_lock held." with: `/// CA GC S4 (G1/#5): runs lock-free; reads in-flight pins only from the supplied pinned_snapshot.`

- [ ] **Step 3: Update the `.h` declarations.** In `ContentAddressedGC.h`, rename the three declarations to drop `Locked`, add the `pinned_snapshot` parameter to `collectReconciliationCandidates`, and update each declaration's doc-comment to the lock-free convention (mirror Step 2). Confirm `sweepCandidates` keeps its existing `pinned_snapshot` parameter.

- [ ] **Step 4: Update the call sites.** In `runSweepOnce` (lines 806, 814, 821) and `runReconciliationScan` (lines 964, 968, 970):
  - line 806: `for (const auto & key : collectSealedTombstoneCandidates())`
  - line 814: `for (const auto & key : collectReconciliationCandidates(now, pinned_snapshot))`
  - line 821: `return sweepCandidates(candidate_object_keys, pinned_snapshot, now, grace, held);`
  - line 964: `std::set<std::string> candidate_object_keys = collectReconciliationCandidates(now, pinned_snapshot);`
  - line 968: `for (const auto & key : collectSealedTombstoneCandidates())`
  - line 970: `return sweepCandidates(candidate_object_keys, pinned_snapshot, now, grace, held);`
  Both call sites already have a `pinned_snapshot` local in scope (lines 750, 963) before the reconciliation collector is called.

- [ ] **Step 5: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t10_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors (a missed call site is a compile error — useful).

- [ ] **Step 6: Run the full suite — no behavior change.**
```bash
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t10_suite.log 2>&1; echo "exit=$?"
```
Expected: all pass (#5 is a race fix with identical results in the single-threaded gtest; #7 is a rename). Subagent summarizes.

- [ ] **Step 7: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h
git commit -m "CA GC remediation #5/#7: reconciliation collector reads the pin snapshot; drop stale *Locked

#5: collectReconciliationCandidates read the shared in_flight_pinned_blobs set unlocked (UB
under reconciliation). Thread the lock-free pinned_snapshot in, mirroring sweepCandidates.
#7: drop the Locked suffix + the 'MUST hold gc_lock' contracts S4 removed (the callers hold
no lock across the sweep) — the maintenance time-bomb that let #5 slip in.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 5 — #3 lock-free `GcLogWriter` I/O + fold-ins + oracle 3

`GcLogWriter` holds `mtx` across `readShardEpoch` (HEAD+GET) and `flushBufferLocked` (PUT), the re-append looping up to 8× under the lock — and the writer is one per-pool instance shared by all committers, so every concurrent commit blocks all others for multiple ~300 ms round-trips (the opposite of G1). Fix: take `mtx` only to move fragments in/out of the buffer; do the S3 I/O outside the lock; cache the per-shard epoch.

### Task 11: Add a per-shard epoch cache; read the epoch outside the lock

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h` (members) and `GcLogWriter.cpp` (`readShardEpoch`)

- [ ] **Step 1: Add the epoch cache members.** In `GcLogWriter.h`, after `std::map<ShardEpoch, Buffer> buffers;` (line 129):
```cpp
    /// CA GC S4 (#3, G1): cache of shard -> last-observed open epoch, so the hot append path does not do a
    /// HEAD+GET under the lock on every commit. The epoch advances at most once per GC round (a fold); a
    /// stale-low cache only widens the window the writer logs into (the re-append catches the advance), and
    /// a stale-high cache cannot happen (the cache is only ever refreshed from object storage). Guarded by
    /// `epoch_cache_mtx` (distinct from `mtx`, which guards the buffers), so an epoch refresh never blocks a
    /// concurrent buffer move.
    mutable std::mutex epoch_cache_mtx;
    mutable std::map<ShardId, uint64_t> epoch_cache;
```

- [ ] **Step 2: Add a cached epoch reader + a refresher.** In `GcLogWriter.cpp`, add two helpers. `cachedShardEpoch` returns the cached value (refreshing once on a cold miss); `refreshShardEpoch` does the HEAD+GET and updates the cache. Both take NO `mtx`:
```cpp
uint64_t GcLogWriter::refreshShardEpoch(ShardId shard) const
{
    /// HEAD+GET the authoritative epoch (outside any lock) and update the cache. Returns the fresh value.
    const uint64_t epoch = readShardEpoch(shard);
    std::lock_guard<std::mutex> guard(epoch_cache_mtx);
    auto & cached = epoch_cache[shard];
    if (epoch > cached)
        cached = epoch; /// monotonic: never let a racing lower read move the cache backwards.
    return cached;
}

uint64_t GcLogWriter::cachedShardEpoch(ShardId shard) const
{
    {
        std::lock_guard<std::mutex> guard(epoch_cache_mtx);
        if (auto it = epoch_cache.find(shard); it != epoch_cache.end())
            return it->second;
    }
    /// Cold miss: refresh once (the only HEAD+GET on the warm path is gone — subsequent commits hit the cache;
    /// the re-append's refresh keeps it current when a fold advances the epoch).
    return refreshShardEpoch(shard);
}
```
Declare both in `GcLogWriter.h` (private, `const`). Keep `readShardEpoch` as-is (the raw HEAD+GET).

- [ ] **Step 3: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t11_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 4: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp
git commit -m "CA GC remediation #3: GcLogWriter per-shard epoch cache (no HEAD+GET on the warm path)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 12: Restructure the flush path so S3 I/O runs outside the lock

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp` — `flushBufferLocked` → split into a lock-held drain + a lock-free write; `enqueue`, `appendAndFlushForCommit`, `reappendIfAdvancedLocked`, `flushAll`

**Design (the invariant):** `mtx` guards ONLY `buffers` (the in-memory map). The S3 object write and the epoch read must NOT happen under `mtx`. Restructure `flushBufferLocked` into two pieces: `drainBufferLocked` (under `mtx`: move the fragments out into a `GcLogBatch` + compute the object key, clear the buffer entry) returning a small `PendingWrite{ object_key, bytes }`; and `writePending` (no lock: the serialize + PUT). Each public method takes `mtx` only around the drain, then writes outside it.

- [ ] **Step 1: Add the `PendingWrite` type + the two helpers.** In `GcLogWriter.h` (private):
```cpp
    /// CA GC S4 (#3): a drained buffer ready to be written WITHOUT the lock. Produced under `mtx`
    /// (drainBufferLocked), consumed lock-free (writePending).
    struct PendingWrite
    {
        std::string object_key;
        std::string bytes;
        size_t delta_count = 0;
    };
    std::optional<PendingWrite> drainBufferLocked(ShardId shard, uint64_t epoch, Buffer & buffer);
    void writePending(const PendingWrite & pending);
```
In `GcLogWriter.cpp`, define them by splitting the current `flushBufferLocked` (lines 104-130):
```cpp
std::optional<GcLogWriter::PendingWrite> GcLogWriter::drainBufferLocked(ShardId shard, uint64_t epoch, Buffer & buffer)
{
    /// Under `mtx`: coalesce the buffered fragments into ONE GcLogBatch, compute the deterministic object key
    /// (named by the first fragment's event_id), and CLEAR the buffer. No S3 I/O here — the serialized bytes
    /// are returned for a lock-free write so the mutex is never held across a PUT (G1).
    if (buffer.fragments.empty())
        return std::nullopt;
    GcLogBatch batch;
    batch.deltas.reserve(buffer.fragments.size());
    for (auto & fragment : buffer.fragments)
        batch.deltas.push_back(std::move(fragment.delta));
    const std::string & object_event_id = batch.deltas.front().event_id;
    const GcLogObjectKey object_key = gcLogEventKey(key_prefix, epoch, shard, object_event_id);
    PendingWrite pending;
    pending.object_key = object_key.string();
    pending.bytes = batch.serialize();
    pending.delta_count = batch.deltas.size();
    buffer.fragments.clear();
    return pending;
}

void GcLogWriter::writePending(const PendingWrite & pending)
{
    /// Lock-free: the S3 PUT. Two writers targeting the same (shard, epoch, first-event_id) write identical
    /// bytes to the same key (idempotent — the batch is named by the first event_id and re-appends reuse it).
    auto out = object_storage->writeObject(StoredObject(pending.object_key), WriteMode::Rewrite);
    out->write(pending.bytes.data(), pending.bytes.size());
    out->finalize();
    last_batch_size.store(pending.delta_count, std::memory_order_relaxed);
}
```
Then DELETE the old `flushBufferLocked` (its callers are rewritten below). NOTE: the Task 6 "clear before write" change is now structurally guaranteed — the buffer is cleared inside `drainBufferLocked` under the lock, and the write happens later outside it.

- [ ] **Step 2: Rewrite `appendAndFlushForCommit` to drain-under-lock then write-outside.** Replace the body (lines 161-191):
```cpp
std::vector<std::pair<ShardId, uint64_t>> GcLogWriter::appendAndFlushForCommit(const GcDelta & delta)
{
    auto by_shard = splitDeltaByShard(delta);

    std::vector<std::pair<ShardId, uint64_t>> settled;
    settled.reserve(by_shard.size());
    for (auto & [shard, fragment] : by_shard)
    {
        /// Read the open epoch from the cache (no HEAD+GET on the warm path). Buffer the fragment + drain
        /// under `mtx`; do the PUT outside it (G1: the mutex is never held across S3 I/O).
        const uint64_t epoch = cachedShardEpoch(shard);
        std::optional<PendingWrite> pending;
        std::vector<Fragment> retained;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto & buffer = buffers[{shard, epoch}];
            if (buffer.fragments.empty())
                buffer.opened_at = std::chrono::steady_clock::now();
            buffer.fragments.push_back(std::move(fragment));
            retained = buffer.fragments; /// copy for the rule-2 re-append BEFORE the drain clears the buffer.
            pending = drainBufferLocked(shard, epoch, buffer);
        }
        if (pending)
            writePending(*pending);
        /// CA GC S4 (#3): the rule-2 re-append re-reads the epoch and re-drains OUTSIDE the per-shard lock
        /// window above (it takes its own short lock per attempt). Returns the final settled epoch.
        const uint64_t final_epoch = reappendIfAdvanced(shard, epoch, retained);
        settled.emplace_back(shard, final_epoch);
    }
    return settled;
}
```

- [ ] **Step 3: Rewrite `reappendIfAdvancedLocked` → `reappendIfAdvanced` (lock-free I/O).** Replace (lines 132-159). It no longer assumes the caller holds `mtx`; each attempt takes `mtx` only around the drain:
```cpp
uint64_t GcLogWriter::reappendIfAdvanced(ShardId shard, uint64_t written_epoch, const std::vector<Fragment> & retained)
{
    /// §5.1 rule 2: while the shard epoch has advanced PAST the epoch we wrote, re-buffer the SAME fragments
    /// (same event_ids — deduped on fold) into the now-open epoch and re-flush, so a straggler is re-logged
    /// rather than lost. CA GC S4 (#3, G1): the epoch refresh (HEAD+GET) and the re-flush PUT run OUTSIDE the
    /// lock; `mtx` is taken only to move the fragments into the buffer and drain them. Bounded retry.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint64_t current = refreshShardEpoch(shard); /// HEAD+GET outside the lock; updates the cache.
        if (current <= written_epoch)
            return written_epoch;
        std::optional<PendingWrite> pending;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto & open_buffer = buffers[{shard, current}];
            if (open_buffer.fragments.empty())
                open_buffer.opened_at = std::chrono::steady_clock::now();
            for (const auto & f : retained)
                open_buffer.fragments.push_back(f);
            pending = drainBufferLocked(shard, current, open_buffer);
        }
        if (pending)
            writePending(*pending);
        written_epoch = current;
    }
    return written_epoch;
}
```
Rename the declaration in `GcLogWriter.h:121` to `reappendIfAdvanced` and update its doc-comment (drop "under the caller's held mtx").

- [ ] **Step 4: Rewrite `enqueue` (the drop-path append) the same way.** Replace (lines 82-102):
```cpp
std::vector<ShardId> GcLogWriter::enqueue(const GcDelta & delta)
{
    auto by_shard = splitDeltaByShard(delta);

    std::vector<ShardId> result;
    result.reserve(by_shard.size());
    const auto now = std::chrono::steady_clock::now();
    for (auto & [shard, fragment] : by_shard)
    {
        const uint64_t epoch = cachedShardEpoch(shard);
        std::lock_guard<std::mutex> lock(mtx);
        auto & buffer = buffers[{shard, epoch}];
        if (buffer.fragments.empty())
            buffer.opened_at = now;
        buffer.fragments.push_back(std::move(fragment));
        result.push_back(shard);
    }
    return result;
}
```
(`enqueue` only buffers — no flush — so it just needs the cached epoch instead of the under-lock HEAD+GET; the actual write happens on a later `flushAll`/`flushDueWindows`.)

- [ ] **Step 5: Rewrite `flushAll` and `flushDueWindows` to drain-then-write.** For `flushAll` (lines 220-236):
```cpp
void GcLogWriter::flushAll()
{
    /// Snapshot the keys under `mtx`, then for each: drain under `mtx` and write outside it, plus the rule-2
    /// re-append (also lock-free I/O). No S3 PUT is ever held under the lock (G1).
    std::vector<ShardEpoch> all;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto & [shard_epoch, buffer] : buffers)
            if (!buffer.fragments.empty())
                all.push_back(shard_epoch);
    }
    for (const auto & shard_epoch : all)
    {
        std::optional<PendingWrite> pending;
        std::vector<Fragment> retained;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto & buffer = buffers[shard_epoch];
            if (buffer.fragments.empty())
                continue;
            retained = buffer.fragments;
            pending = drainBufferLocked(shard_epoch.first, shard_epoch.second, buffer);
        }
        if (pending)
            writePending(*pending);
        reappendIfAdvanced(shard_epoch.first, shard_epoch.second, retained);
    }
}
```
Apply the same drain-then-write restructuring to `flushDueWindows` (lines 193-218): collect the due `(shard, epoch)` keys + their `retained` under `mtx`, then drain+write+reappend outside. Read the current body before editing and mirror the `flushAll` shape.

- [ ] **Step 6: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t12_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 7: Run the full suite (esp. oracle 2 / the append-as-epoch-folds path).**
```bash
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t12_suite.log 2>&1; echo "exit=$?"
```
Expected: all pass — the re-append + dedup behavior is unchanged; only the locking discipline changed. Subagent summarizes.

- [ ] **Step 8: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h
git commit -m "CA GC remediation #3: GcLogWriter does S3 I/O outside the mutex (G1)

Take mtx only to move fragments in/out of the buffer (drainBufferLocked); do the epoch read
and the object PUT lock-free (writePending). Concurrent commits no longer serialize on the
per-pool writer across multiple ~300ms round-trips.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 13: Fold-ins — shutdown→flushAll; remove the double `persistSession`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` — `shutdown` (`334-338`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` — `commit` (`1672-1687`)

- [ ] **Step 1: `shutdown` flushes the writer.** Current:
```cpp
void ContentAddressedMetadataStorage::shutdown()
{
    if (gc_thread)
        gc_thread->shutdown();
}
```
Replace with:
```cpp
void ContentAddressedMetadataStorage::shutdown()
{
    if (gc_thread)
        gc_thread->shutdown();
    /// CA GC S4 (#3 fold-in): flush any buffered `-` deltas before teardown — otherwise a drop's `-` that
    /// was only buffered (enqueue without an immediate flush) is silently lost, leaving a stale `+` count
    /// (over-count/leak). Best-effort: a flush failure here must not throw out of shutdown.
    if (gc_log_writer)
    {
        try
        {
            gc_log_writer->flushAll();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            tryLogCurrentException(getLogger("ContentAddressedMetadataStorage"), "CA GC: flushAll on shutdown failed (buffered deltas may be re-logged on the next run)");
        }
    }
}
```
Confirm `tryLogCurrentException` + `getLogger` are available in this file (they are used elsewhere in the CA code; add the includes if the build complains).

- [ ] **Step 2: Remove the double `persistSession`.** In `commit`, lines 1672-1687 call `persistSession()` twice with only comments in between (no state change). Remove the FIRST (lines 1672-1673), keeping the second (the §7.1 step-2 re-assert at 1686-1687) and its comment. Delete:
```cpp
    if (session_open)
        persistSession();

```
(the block at 1672-1673, leaving the comment block + the 1686-1687 call). VERIFY by reading 1666-1688 that exactly one `persistSession()` remains before the per-part loop.

- [ ] **Step 3: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t13_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 4: Write oracle 3 — `reappendIfAdvanced` actually fires.** Append to `gtest_content_addressed_gc_s4.cpp`. Enqueue into epoch E via `enqueue` (which buffers), externally advance the shard epoch by folding (close E→E+1), then `flushAll`; assert the delta is re-logged into the new epoch and folds with the correct (deduped) count:
```cpp
TEST_F(ContentAddressedGcS4, Sec5_1_ReappendIfAdvancedActuallyFires)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    const BlobHash b = blobHash("r01");
    const PartId p = partId("r01");

    /// Determine the home shard of the part so we can advance exactly it.
    const ShardId home = GcLogWriter::shardForPartId(p);

    /// Buffer a `+` into the CURRENT open epoch of the home shard (enqueue buffers; no flush yet).
    GcDelta add;
    add.op = GcDelta::Op::Add;
    add.part_id = p;
    add.pins = {b};
    add.pin_generations = {0};
    add.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);
    writer.enqueue(add);

    /// Externally CLOSE the home shard's epoch (advance it) WHILE the `+` sits buffered in the old epoch.
    const auto first_fold = compaction.compactShard(home, kStillLeader);
    EXPECT_GE(first_fold.new_epoch, 1u);

    /// flushAll: the buffered `+` flushes into the now-stale epoch, then reappendIfAdvanced detects the
    /// advance and re-logs it into the open epoch. The next fold of the home shard must then COUNT b (not
    /// drop it as a count-0 candidate) — proving the re-append carried the straggler forward.
    writer.flushAll();

    bool b_dropped_as_candidate = false;
    for (int i = 0; i < 4; ++i)
    {
        const auto folded = compaction.compactShard(home, kStillLeader);
        for (const auto & c : folded.candidates)
            if (c.key.identity == b.string())
                b_dropped_as_candidate = true;
    }
    EXPECT_FALSE(b_dropped_as_candidate) << "the re-appended + must keep b counted across the epoch advance";
}
```
VERIFY `GcLogWriter::shardForPartId` is public/static-accessible from the test (the test already calls `GcLogWriter::shardForHash`-style helpers; if `shardForPartId` is not public, use `shardForHash(b)` and advance that shard instead, adjusting the assertion to the blob's shard).

- [ ] **Step 5: Build + run oracle 3 + full suite.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t13b_build.log 2>&1; echo "exit=$?"
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t13b_suite.log 2>&1; echo "exit=$?"
```
Expected: 0 build errors; all pass. Subagent summarizes.

- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/tests/gtest_content_addressed_gc_s4.cpp
git commit -m "CA GC remediation #3 fold-ins: shutdown->flushAll, drop double persistSession; oracle 3

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 6 — #4 `gc/sealed/<shard>` candidate-discovery index (Scan A) + oracle 6

`collectSealedTombstoneCandidates` LISTs the entire `blobs/`+`parts/` tree every round (`ContentAddressedGC.cpp:649-650`) to re-present sealed-but-unswept tombstones — this is **Scan A**, so the "0 LISTs on the normal compaction path" claim is false at scale. Maintain a compact `gc/sealed/<shard>` index: seal adds an entry, recover removes it; the sweep LISTs only that index. **This does NOT touch Scan B** (the `markReachableBlobs` delete gate, which still over-protects) — #4 is a perf/G3 fix, not a safety-semantics change.

### Task 14: Add the `gc/sealed/<shard>` path helpers + a part-shard fn

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h` / `PoolPaths.cpp`

- [ ] **Step 1: Add a `shardForPartId` (if not already present) so part tombstones map to a shard.** `shardForHash(const BlobHash &)` exists (`PoolPaths.cpp:303`); `GcLogWriter::shardForPartId` exists (used by the writer). Add a free `shardForPartId(const PartId &)` in `PoolPaths` mirroring `shardForHash` (fold up to 4 leading hex nibbles of the part-id string, mask `& (kGcShardCount-1)`) so the sealed-index key for a part tombstone is shard-consistent. If `GcLogWriter::shardForPartId` already encapsulates this, expose/reuse it instead of duplicating — read both before adding to avoid two divergent part-shard functions.

- [ ] **Step 2: Add the sealed-index key builders.** In `PoolPaths.h`, next to `gcSnapKey`/`gcLogPrefix`:
```cpp
/// CA GC S4 (#4): the per-shard sealed-tombstone index. A compact set of "open" (sealed-but-unswept)
/// tombstones the sweep re-presents each round — replacing the full blobs/+parts/ bucket LIST (Scan A).
/// One tiny object per open tombstone: <prefix>/gc/sealed/<shard>/<identity>.<generation>.<b|p>
std::string gcSealedPrefix(const std::string & key_prefix, ShardId shard);              // <prefix>/gc/sealed/<shard>/
std::string gcSealedKey(const std::string & key_prefix, ShardId shard, const std::string & identity, uint64_t generation, bool is_blob);
```
Implement in `PoolPaths.cpp` with the same `withPrefix` style the other builders use. The entry's body is empty (the key encodes everything); the key must be parseable back into `(identity, generation, is_blob)` so the sweep can map an index entry to its generation object key. Match the existing key-parse helpers (`parseGenObjectKey`/`parseGenFromKey`) — reuse the same `<g>`/`.tombstone` encoding conventions so a single parser covers both.

- [ ] **Step 3: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t14_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 4: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.cpp
git commit -m "CA GC remediation #4: gc/sealed/<shard> index path builders + part-shard fn

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 15: Maintain the index (seal adds, recover removes) and discover candidates from it

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp` — the seal write (`512`), the recover removal (`593-594`), `collectSealedTombstoneCandidates` (`608-656`)

- [ ] **Step 1: On SEAL, add a `gc/sealed` index entry.** At the seal in `sweepCandidates` (line 512), after `condCreateIfAbsent(*object_storage, tombstone, …)` succeeds, write the index entry (best-effort — the durable tombstone is still the truth; the index is an accelerator):
```cpp
        if (condCreateIfAbsent(*object_storage, tombstone, /*bytes=*/std::string()))
        {
            ProfileEvents::increment(ProfileEvents::ContentAddressedTombstonesTotal);
            /// CA GC S4 (#4): record the open tombstone in the per-shard sealed index so the next round
            /// discovers it WITHOUT a full blobs/+parts/ LIST. Best-effort: a missed write only falls back
            /// to the (still-correct) tombstone object; the reconciliation cross-check bounds any drift.
            try
            {
                const ShardId shard = parts->is_blob
                    ? shardForHash(BlobHash(parts->identity))
                    : shardForPartId(PartId(parts->identity));
                const std::string sealed_key
                    = gcSealedKey(key_prefix, shard, parts->identity, parts->generation, parts->is_blob);
                condCreateIfAbsent(*object_storage, sealed_key, /*bytes=*/std::string());
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
```

- [ ] **Step 2: On RECOVER, remove the index entry (un-seal).** At the recover removal (lines 592-594), alongside queuing the `.tombstone` for removal, also remove the sealed-index entry:
```cpp
            if (object_storage->tryGetObjectMetadata(tombstone, /*with_tags=*/false).has_value())
                tombstones_to_remove.emplace_back(tombstone);
            /// CA GC S4 (#4): recover un-seals -> drop the sealed-index entry too (the tombstone is gone).
            {
                const ShardId shard = parts->is_blob
                    ? shardForHash(BlobHash(parts->identity))
                    : shardForPartId(PartId(parts->identity));
                tombstones_to_remove.emplace_back(
                    gcSealedKey(key_prefix, shard, parts->identity, parts->generation, parts->is_blob));
            }
```
(Adding the sealed key to the same `tombstones_to_remove` batch — both go through `removeObjectsIfExist` at line 603-604, which is a harmless no-op if the entry is absent.) NOTE: on SWEEP the generation object is deleted but the `.tombstone` gravestone is kept (line 538-539); the sealed-index entry should be removed on sweep too (an already-swept generation must not be re-presented forever). Add the same `gcSealedKey` removal to the sweep branch's `to_remove` (around lines 560-575) — confirm the swept entry's index key is dropped so a swept generation stops re-appearing.

- [ ] **Step 3: Rewrite `collectSealedTombstoneCandidates` to LIST the index, not the bucket.** Replace the body (lines 608-656). The new version LISTs `gc/sealed/<shard>` for every shard (16 small prefixes) instead of the full `blobs/`+`parts/` tree, and maps each index entry back to its generation object key:
```cpp
std::set<std::string> ContentAddressedGC::collectSealedTombstoneCandidates()
{
    /// CA GC S4 (#4, G3, Scan A): discover sealed-but-unswept tombstones from the compact per-shard
    /// gc/sealed/<shard> index instead of LISTing the entire blobs/+parts/ tree every round. Each index
    /// entry maps back to its generation object key (the re-presented candidate). The durable .tombstone
    /// object remains the source of truth; the index is the accelerator (a missed entry is bounded by the
    /// reconciliation cross-check + Scan B still gates the delete). Runs lock-free (CA GC S4 G1).
    std::set<std::string> candidates;
    for (ShardId shard = 0; shard < kGcShardCount; ++shard)
    {
        for (const auto & key : listKeysUnder(object_storage, gcSealedPrefix(key_prefix, shard)))
        {
            /// Parse the index entry back to (identity, generation, is_blob) and rebuild the generation
            /// object key it condemns (blobs/<H>/<g> or parts/<id>/<g>) — the same key Scan B re-checks.
            const auto entry = parseSealedIndexKey(key_prefix, key);
            if (!entry)
                continue;
            const std::string gen_object_key = entry->is_blob
                ? blobGenKey(key_prefix, BlobHash(entry->identity), entry->generation).string()
                : partGenKey(key_prefix, PartId(entry->identity), entry->generation).string();
            candidates.insert(gen_object_key);
        }
    }
    return candidates;
}
```
Add the `parseSealedIndexKey` helper (free fn in `PoolPaths`/`ContentAddressedGC`) returning `std::optional<struct { std::string identity; uint64_t generation; bool is_blob; }>` — implement it as the inverse of `gcSealedKey` (Task 14 Step 2). NOTE: the §13 observability tally that the old Scan A did (`ContentAddressedGenerationsObserved`/`ContentAddressedHashesObserved`, lines 621-654) is now only meaningful during reconciliation (the full scan) — move that tally into `collectReconciliationCandidates` (which still does the full LIST) so the counters are still emitted on the reconciliation path; do not emit them from the index path (the index does not see the full generation population). Confirm this preserves the counters' meaning and does not double-count.

- [ ] **Step 4: Build the unit test.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t15_build.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 errors.

- [ ] **Step 5: Run the full suite — the sweep semantics are unchanged.**
```bash
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t15_suite.log 2>&1; echo "exit=$?"
```
Expected: all pass — seal/grace/recover/sweep semantics are identical; only the candidate-discovery source changed. The existing S3 tombstone oracles (in `gtest_content_addressed_gc_s3.cpp`) are the cross-check that a sealed-but-unswept tombstone is still re-presented. If any S3 tombstone oracle relies on a generation object existing without a `gc/sealed` entry (e.g. a tombstone written directly by a test, not via the seal path), it will surface here — fix by having such tests seal through the real path or seed the index. Subagent summarizes.

- [ ] **Step 6: Write oracle 6 — the index replaces the bucket scan.** Append to `gtest_content_addressed_gc_s4.cpp`. Assert (a) a sealed-but-unswept tombstone is re-presented across rounds via the index, and (b) a normal sweep round issues **0** `blobs/`+`parts/` LISTs. For (b), since the gtest uses `LocalObjectStorage` (no op counter), assert it structurally: after a round that seals via the index, a follow-up round that finds the candidate must read only `gc/sealed/<shard>` — verify by checking the `gc/sealed` entry exists after seal and the candidate persists across rounds:
```cpp
TEST_F(ContentAddressedGcS4, Sec4_SealedIndex_RePresentsAcrossRounds_NoBucketScan)
{
    const BlobHash b = blobHash("x01");

    /// Put an unreferenced generation object (no ref pins it) so the sweep seals it.
    put(blobGenKey(prefix, b, 0).string(), "ORPHAN");

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.setReconciliationCadenceRounds(1); /// first round populates via reconciliation (full scan) -> seals

    /// Round 1: discover (reconciliation), seal (H,0), arm grace. The seal must create a gc/sealed entry.
    gc.runSweepOnce(/*now=*/0, /*grace=*/100);
    const ShardId shard = shardForHash(b);
    EXPECT_TRUE(exists(gcSealedKey(prefix, shard, b.string(), 0, /*is_blob=*/true)))
        << "seal must record the open tombstone in the gc/sealed index";

    /// Round 2 WITHOUT reconciliation: the candidate must still be re-presented purely from the index, and
    /// (grace satisfied) swept. The generation object is gone; the gravestone+index reflect the sweep.
    gc.setReconciliationCadenceRounds(0);
    gc.runSweepOnce(/*now=*/1000, /*grace=*/100);
    EXPECT_FALSE(exists(blobGenKey(prefix, b, 0).string())) << "the index re-presented the candidate and it was swept";
    EXPECT_FALSE(exists(gcSealedKey(prefix, shard, b.string(), 0, /*is_blob=*/true)))
        << "a swept generation's index entry is removed (not re-presented forever)";
}
```
VERIFY the exact `setReconciliationCadenceRounds(0)` semantics mean "no reconciliation this round" (the sealed-index path is the only candidate source) — read `reconciliationDue()`/`setReconciliationCadenceRounds` before finalizing the assertion; if cadence `0` means "never reconcile", the round-1 discovery must instead be seeded differently (e.g. seal directly through a one-shot reconciliation round, then set cadence high). Adjust to whatever makes round 2 index-only.

- [ ] **Step 7: Build + run oracle 6 + full suite.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t15b_build.log 2>&1; echo "exit=$?"
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t15b_suite.log 2>&1; echo "exit=$?"
```
Expected: 0 build errors; all pass. Subagent summarizes.

- [ ] **Step 8: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h src/Disks/tests/gtest_content_addressed_gc_s4.cpp
git commit -m "CA GC remediation #4: gc/sealed/<shard> index replaces the per-round bucket scan (Scan A, G3)

Seal adds a compact index entry; recover/sweep removes it; the sweep discovers candidates by
LISTing only gc/sealed/<shard> (16 small prefixes) instead of the full blobs/+parts/ tree. Does
NOT touch Scan B (the markReachableBlobs delete gate) — perf/G3 fix only. Oracle 6 proves
re-presentation across rounds + index lifecycle.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 7 — remaining oracles (2, 5), regression, backlog, push

### Task 16: Oracle 2 — real lockless interleaving; oracle 5 — negative codec tests

**Files:**
- Test: `src/Disks/tests/gtest_content_addressed_gc_s4.cpp`

- [ ] **Step 1: Oracle 2 — a `+` lands as its epoch folds (lock-free) → the blob survives; a committed-but-unfolded sticky session protects a dropped-ref blob.** This composes oracle 3 (re-append) with the §7 session pin. Append:
```cpp
TEST_F(ContentAddressedGcS4, Sec7_RealLocklessInterleaving_PlusLandsAsEpochFolds_BlobSurvives)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    const BlobHash b = blobHash("i01");
    const PartId p = partId("i01");

    /// reconciliationCadence stays at its default (off): the candidate source is the compaction/lockless
    /// path only — exactly the regime the spec says the old oracles never exercised.
    GcDelta add;
    add.op = GcDelta::Op::Add;
    add.part_id = p;
    add.pins = {b};
    add.pin_generations = {0};
    add.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);
    const auto settled = writer.appendAndFlushForCommit(add);
    ASSERT_FALSE(settled.empty());

    /// Close each settled epoch (fold) — the `+` is carried into the snapshot, b is COUNTED, never a
    /// count-0 candidate (the lock-free re-append + dedup made the log complete).
    for (const auto & [s, e] : settled)
    {
        const auto folded = compaction.compactShard(s, kStillLeader);
        EXPECT_GT(folded.new_epoch, e);
        for (const auto & c : folded.candidates)
            EXPECT_NE(c.key.identity, b.string()) << "a live blob must never fall out as a count-0 candidate under the lockless fold";
    }
}
```

- [ ] **Step 2: Oracle 5 — negative codec tests for `GcLogBatch` + the session delta.** Mirror the `WriteSession`-style fail-closed checks: bad magic, bumped version, truncated/bad op must throw, not misparse. Append:
```cpp
TEST_F(ContentAddressedGcS4, Sec5_NegativeCodec_GcDeltaSession_FailsClosed)
{
    const PartId p = partId("n01");
    GcDelta d;
    d.op = GcDelta::Op::Add;
    d.part_id = p;
    d.pins = {blobHash("n01")};
    d.pin_generations = {0};
    d.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);

    const std::string good = serializeGcDeltaForSession(d);
    EXPECT_NO_THROW(deserializeGcDeltaFromSession(good));

    /// Corrupt the magic (first byte) -> must throw (fail-closed), not misparse.
    std::string bad_magic = good;
    bad_magic[0] = static_cast<char>(bad_magic[0] ^ 0xFF);
    EXPECT_ANY_THROW(deserializeGcDeltaFromSession(bad_magic));

    /// Truncate the body -> must throw.
    EXPECT_ANY_THROW(deserializeGcDeltaFromSession(good.substr(0, good.size() / 2)));
}
```
If `GcLogBatch` exposes a dedicated negative-codec test elsewhere, align with it; otherwise this covers the session delta path #2 introduces. Confirm `serializeGcDeltaForSession`/`deserializeGcDeltaFromSession` are declared in a header the test includes.

- [ ] **Step 3: Build + run the two oracles + full suite.**
```bash
ninja -C build unit_tests_dbms > build/gcrem_t16_build.log 2>&1; echo "exit=$?"
timeout 590 build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcrem_t16_suite.log 2>&1; echo "exit=$?"
```
Expected: 0 build errors; all pass. Subagent summarizes.

- [ ] **Step 4: Commit.**
```bash
git add src/Disks/tests/gtest_content_addressed_gc_s4.cpp
git commit -m "CA GC remediation: oracles 2 (lockless interleaving) + 5 (negative codec, fail-closed)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 17: CA stateless smoke + non-CA regression

**Files:** none (test run only)

- [ ] **Step 1: Confirm the binary symlink, then build the server binary.**
```bash
ls -la ci/tmp/clickhouse
ninja -C build clickhouse > build/gcrem_server_build.log 2>&1; echo "exit=$?"
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
```
A subagent reads `build/gcrem_server_build.log` and confirms 0 `error:`/`FAILED:`.

- [ ] **Step 2: Run the CA-default GC + transaction stateless smoke (FOREGROUND, bounded).**
```bash
timeout 590 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "04279_content_addressed_gc 04278_content_addressed_disk 04292_content_addressed_mutations 05003_content_addressed_freeze 05004_content_addressed_transactions" > build/gcrem_smoke.log 2>&1; echo "exit=$?"
```
A subagent reads `build/gcrem_smoke.log` (and `ci/tmp/test_result.txt`) and reports the `Failed:`/`Passed:` line. Expected: 0 failed. Especially `04279_content_addressed_gc` must be green (the sweep still deletes exactly as before — Scan B unchanged).

- [ ] **Step 3: Non-CA regression — a couple of plain MergeTree GC/merge tests unaffected.**
```bash
timeout 590 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "00754_first_significant_subtype 01710_minmax_count_projection" > build/gcrem_noncas.log 2>&1; echo "exit=$?"
```
Subagent confirms 0 failed (the CA changes are all behind `isContentAddressed`/the CA metadata storage — plain disks are untouched). If these specific tests are not representative, pick two recent fast MergeTree tests; the point is to confirm no global regression.

- [ ] **Step 4: Commit (if any test-only fixes were needed); otherwise skip.** Only commit if a gated test needed un-gating/re-gating; document the reason in the message.

### Task 18: Backlog reconciliation + push

**Files:**
- Modify: `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`

- [ ] **Step 1: Mark B70 done; confirm B78 (Scan-B replacement) + B71–B77 remain open.** Update the B70 row: `IN PROGRESS / PLANNED` → `DONE` — Tier 1 (#1, #6, #2, #5, #7) + Tier 2 (#3 + fold-ins, #4) landed with oracles 1–6 green; Scan B untouched (B78 still the deferred data-loss-transition follow-up). Cross-ref: B70 discharges the **code** portion of B69's attended-review gate; the human sign-off + the B78 Scan-B replacement remain before the compaction count is trusted alone. Note which findings stayed deferred (B71–B77).

- [ ] **Step 2: Verify the branch, then push.**
```bash
git branch --show-current   # MUST print cas-mergetree-poc
git add docs/superpowers/deferred_backlog/cas-mergetree-integration.md
git commit -m "CAS backlog: B70 GC S4 remediation DONE (Tier 1 + Tier 2, oracles 1-6); B78 still open

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push filimonov cas-mergetree-poc > build/gcrem_push.log 2>&1; echo "exit=$?"
```
Subagent confirms the push succeeded and the branch is `cas-mergetree-poc` (NOT master).

---

## Done criteria

- **#1:** `splitDeltaByShard` carries `pin_generations`/`manifest_generation`; oracle 1 proves a resurrected `g=1` blob survives while its `g=0` drop nets to zero (generation-keyed independently).
- **#6:** the `RefSidecar` (v2) records the settled generations at commit (preserved on a mutable-only update); the drop keys its `-` from the sidecar, not the racy `active` hint.
- **#2:** a `+`-flush failure makes the session **sticky** (retained, lease-exempt, carrying the serialized `+`); the GC reaper re-logs it (idempotent) and reaps only once folded; `flushBufferLocked`/`drainBufferLocked` clears the buffer before the write. Oracle 4 proves it.
- **#5:** `collectReconciliationCandidates` reads the lock-free `pinned_snapshot`, not the shared set.
- **#7:** the three sweep functions dropped the `*Locked` suffix + the stale "MUST hold gc_lock" contracts.
- **#3:** `GcLogWriter` does all S3 I/O outside `mtx` (drain-under-lock, write-outside); a per-shard epoch cache removes the HEAD+GET from the warm path; shutdown flushes; the double `persistSession` is gone. Oracle 3 proves the re-append still fires under the new locking.
- **#4:** a `gc/sealed/<shard>` index replaces the full `blobs/`+`parts/` bucket LIST for candidate discovery (Scan A); Scan B (the `markReachableBlobs` delete gate) is **untouched**. Oracle 6 proves re-presentation across rounds + the index lifecycle.
- All `ContentAddressed*` gtests green (148 prior + oracles 1–6); the CA-default stateless smoke green (esp. `04279_content_addressed_gc`); non-CA regression unchanged.
- Backlog: B70 DONE; **B78 (replace Scan B with the §6.2 sessions+compaction gate) remains the deferred, data-loss-critical follow-up** with its own attended-review gate; B71–B77 still deferred. The `gc_lock` stays dropped (S4) throughout — these are fixes *on* the lockless path.

## Self-Review notes (for the executor)

- **Verify-before-edit anchors:** the spec line numbers are from the review snapshot; each task says where to re-read before editing (the `refMetaKey` overload in Task 5, the mutable-only `meta_key` var in Task 4, `shardForPartId` visibility in Tasks 13/14, `setReconciliationCadenceRounds(0)` semantics in Task 15, the `CompactionResult`/`CountKey` field names in Task 1). Always re-confirm the exact current text before applying an `Edit`.
- **Phase ordering is load-bearing for correctness of the index, not for safety:** Tier 1 (Phases 1–4) must land before Tier 2 #4 (Phase 6) because the `gc/sealed` seal/sweep/recover bookkeeping keys on #1's correct per-generation candidates. Do not reorder.
- **Scan B is never modified** in this plan. If any task tempts you to narrow `markReachableBlobs`/`identity_reachable_in` to be generation-aware, STOP — that is B78 (a separate data-loss-critical stage), explicitly out of scope.
- **No sleeps** anywhere (the sticky-session re-log converges across GC rounds, driven by `runSweepOnce`, never a timer). Allman braces. Foreground bounded test runs, subagent log summaries, branch check before every commit.
