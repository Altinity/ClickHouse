# CAS-30: a `.meta` delete removes only a marker no newer than its condemnation — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A delayed GC `.meta` delete never removes the marker of a pending incarnation whose body is current, and graduation never trusts a marker it did not read.

**Architecture:** Four rules, all in GC. G1: the graduation gate reads the marker every time (the in-process confirmation registry goes). C3: it accepts only `Condemned` at the entry's round or newer. C1: the delete job deletes only `Condemned` at its entry's round or older. C2: every GC marker write stamps the issuing attempt's round and replaces an older-round `Condemned`; the gate's retry stamps the current round. The budget check moves before the read, and the read is prefetched through the fold's `GcReadAhead` in its own window. No format, setting or writer change.

**Tech Stack:** C++23 (ClickHouse), gtest (`unit_tests_dbms`), TLA+/TLC (`tla2tools.jar`), Mermaid in user docs.

**Spec:** `/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-30-cas-gc-meta-delete-names-its-condemnation-design.md` (rev.9, approved). Owner decisions: `/home/mfilimonov/workspace/ClickHouse/master/tmp/cas30/owner_decisions.md`.

**Worktrees.**

| What | Path | Branch |
|---|---|---|
| Code, user docs, gtests (Tasks 0–6, 8–10) | `/home/mfilimonov/workspace/ClickHouse/gc-request-cuts` | `fix/antalya-26.6/cas-gc-stale-meta-delete` @ `e2dd0f5d343` |
| Model (Task 7) | `/home/mfilimonov/workspace/ClickHouse/master` | `cas-gc-rebuild` (shared tree, foreign edits present) |

Paths below are relative to the worktree root of the task. `CA/` abbreviates
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.

## Global Constraints

- C++: Allman braces (CI style check). Match the surrounding code's naming and comment density.
- Comments explain WHY or a non-local invariant, one or two lines. No references to plans, specs, backlog
  items, review rounds, schedules (`S1`), rule ids (`G1`, `C3`) or task numbers; no history ("previously").
  Wrap code names in backticks; write functions as `f`, not `f()`.
- Prose (docs, commit messages) follows `~/.claude/skills/tech-prose/STYLE.md`: point first, plain words, no hype.
- No fallback paths. No `sleep` for races: waits are a `condition_variable::wait_for` or a yield spin with a
  deadline that FAILS the test.
- No new setting. No on-store format change: `marker_confirmed` (bit 1 of the condemned row's `flags`) is still
  written with `delete_pending`. No writer change: `CA/Pool/CasPartWriteTxn.*` is not touched.
- `LOGICAL_ERROR` expectations are death tests with `std::_Exit` in the child (none are planned here; if one
  appears, it must be one).
- Build: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ninja -C build unit_tests_dbms > build/<log> 2>&1; echo NINJA_EXIT=$? >> build/<log>`.
  No `-j`. A test run counts only if the log of the build before it ends in `NINJA_EXIT=0`.
- Every gtest run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='<filter>' > build/<log> 2>&1; echo GATE_EXIT=$? >> build/<log> )`.
- The gate filter is EXACTLY `CAS*`. Every new suite name starts with `CAS`.
- Read build and test logs through a subagent (project rule); its summary must quote the `NINJA_EXIT=` /
  `GATE_EXIT=` line and every failing test name.
- Commits: `git commit -s`, add EXACT file paths (`git add -- <file> <file>`), never a directory, never
  `git add -A`. No rebase, no amend, NO push. Append the attribution trailer your session requires.
- TDD per task: write the tests, run them, see the named ones fail with the expected message, implement,
  run, see them pass, run the full `CAS*` gate, commit.
- TLC: 8 workers, `nice -n 10`, `-Xmx8g`, one run at a time, only through
  `tmp/cas-pub-marker/run.sh` (master worktree) or the promoted `docs/superpowers/models/run_condemnmarker.sh`.
- Values fixed by the spec: `kGraduationLookaheadRows = 16384`; the read-ahead window is
  `GcReadAhead::window()` = `4 × gc_io_concurrency` (32 at concurrency 8, 64 at 16, 0 at 1).

## Review Focus

The spec's tests do not exercise these; each line names the expected behaviour and the test added for it.

1. **`gc_shards > 1`.** Graduation reservations are per shard merge and the round budget is shared: a 40-blob
   cohort over 4 shards with budget 10 graduates exactly 10, reads 10 markers, wastes no read, and reads the
   same keys at concurrency 1 and 8. Test `CASGCReadAhead.GraduationReadAheadAcrossShards` (Task 6).
2. **Unbounded budget, cohort far above the window.** 100 eligible entries at concurrency 8 (window 32) all
   graduate with exactly one marker read each and no waste. Test
   `CASGCReadAhead.GraduationReadAheadReadsALargeCohortOnce` (Task 6).
3. **Pending rows next to eligible rows.** A `delete_pending` row is never hinted: the round redeletes the
   pending cohort and graduates the eligible one, and per-key marker reads equal concurrency 1. Test
   `CASGCReadAhead.GraduationReadAheadNeverHintsPendingRows` (Task 6).
4. **An undecodable marker at the gate.** The round commits, the entry is carried, never graduated, and the
   anomaly counter moves; nothing throws out of the round. Test
   `CASGcMetaWriter.GraduationCarriesAnUndecodableMarker` (Task 3).
5. **An undecodable marker at the delete job.** The job deletes nothing it could not read, counts one anomaly,
   and `drain` does not throw. Test `CASGcMetaWriter.ConfirmedMetaDeleteLeavesAnUndecodableMarker` (Task 1).

---

## Task map

The dispatch grouped the spec's tests by kind; this plan groups them by the rule that turns them green, so each
task's new tests fail first on the tree it starts from. Every test below is red on the base for the reason in
spec §9 unless marked "guard".

| Task | Rule / change | Tests (red first) | Guards (green before and after) |
|---|---|---|---|
| 0 | baseline, base binary | — | full `CAS*` gate |
| 1 | C1: the job reads and checks the round | U1, U2, R7 ×2, R9 | U3, U6, U7, R5 |
| 2 | budget before the gate; the bit no longer skips it | B1, B3, B2, RA2 | — |
| 3 | G1 + C3 + retry at this round; registry removed | R1 ×2, R4, R8, R10, RA1, RA1b, RA3 | — |
| 4 | C2: `writeCondemnedMeta` re-stamps an older round | U4, R2, R3 | U5 |
| 5 | `PriorEdgeCursor::scanAhead`, `GraduationReadHints` in the merge | CB1, CB2, CB3 (new API) | B1, B3 |
| 6 | graduation read-ahead in `Gc::fold`, `decodeLoadedMeta`, `pendingHeads` | RA5 | RA4, RA6, RA7, RA8 |
| 7 | model: new constants, invariants, cfgs, runs, promotion | the §8 table | regression battery |
| 8 | user docs | Mermaid parse | — |
| 9 | gate lanes, mutation checks | — | — |
| 10 | `gc-ab` stand: graduation phase time and memory | — | — |

Deviations from the dispatch, all for test-first order: U3 and U6 are delete-job guards and live in Task 1;
the retry stamp (C2's second half) is one argument of the gate rewritten in Task 3 and lands there with its test
R8; B2 and RA2 test the budget order and land with it in Task 2.

## File structure

| File | Responsibility | Tasks |
|---|---|---|
| `CA/Gc/CasGcMetaWriter.{h,cpp}` | marker jobs: write `Condemned`, delete a confirmed marker; registry removed | 1, 3, 4 |
| `CA/Gc/CasGc.cpp` | gate (`confirm_condemned_marker`), job round, read-ahead closures in `Gc::fold` | 1, 3, 6 |
| `CA/Gc/CasBlobInDegree.{h,cpp}` | budget before the gate, `GraduationReadHints`, `PriorEdgeCursor::scanAhead` | 2, 3, 5 |
| `CA/Gc/CasGcReadAhead.h` | `pendingHeads`, header sentence | 6 |
| `CA/Pool/CasBlobMeta.{h,cpp}` | `decodeLoadedMeta` | 6 |
| `src/Disks/tests/cas_gc_marker_test_support.h` (new) | pool/round/marker/lease helpers, `HeadHookBackend`, `ContentEtagBackend`, `ApplyParking` | 1, 3 |
| `src/Disks/tests/gtest_cas_gc_meta_writer.cpp` | U*, R*, B2, R10 | 1, 2, 3, 4 |
| `src/Disks/tests/gtest_cas_blob_indegree.cpp` | B1, B3, CB1–CB3 | 2, 5 |
| `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` | RA* | 2, 3, 6 |
| `src/Disks/tests/gtest_cas_gc_ack_floor.cpp` | registry comments reworded | 3 |
| `docs/en/antalya/cas/architecture/garbage-collection.md`, `docs/en/sql-reference/statements/system.md`, `docs/en/antalya/cas/operations/debugging.md` | phase 9, costs, `#condemn-delete`, rule B, upgrade procedure | 8 |
| master: `tmp/cas-pub-marker/*`, `docs/superpowers/models/CaGcCondemnMarkerGate*`, `docs/superpowers/models/run_condemnmarker.sh` | model and promotion | 7 |

---

### Task 0: Baseline and base binary

**Files:** none changed. Creates `tmp/cas30/clickhouse-base` in the code worktree (untracked).

**Interfaces:** Produces the base `CAS*` result every later task compares against, and the base binary Task 10 measures.

- [ ] **Step 1: Confirm the worktree and branch**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && git status --short && git log --oneline -1 && git branch --show-current`
Expected: empty status, `e2dd0f5d343 Merge pull request #2440 ...`, `fix/antalya-26.6/cas-gc-stale-meta-delete`.
If the status is not empty, stop and report: another agent owns those edits.

- [ ] **Step 2: Build the unit tests and the server at the base**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ninja -C build unit_tests_dbms clickhouse > build/build_cas30_t0.log 2>&1; echo NINJA_EXIT=$? >> build/build_cas30_t0.log`
Expected: last line `NINJA_EXIT=0`.

- [ ] **Step 3: Save the base binary**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && mkdir -p tmp/cas30 && cp build/programs/clickhouse tmp/cas30/clickhouse-base`

- [ ] **Step 4: Run the base gate**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas30_t0_gate.log 2>&1; echo GATE_EXIT=$? >> build/test_cas30_t0_gate.log )`
Expected: `GATE_EXIT=0`. Record the `[  PASSED  ] N tests` count. A red here is a pre-existing defect: root-cause it
before Task 1 (no known reds are carried).

---

### Task 1: The delete job removes only a marker at its entry's round or older (C1)

**Files:**
- Create: `src/Disks/tests/cas_gc_marker_test_support.h`
- Modify: `CA/Gc/CasGcMetaWriter.h:39-40` (`scheduleConfirmedMetaDelete`)
- Modify: `CA/Gc/CasGcMetaWriter.cpp:82-92` (`deleteConfirmedMeta`), `:160-167` (`GcMetaWriter::scheduleConfirmedMetaDelete`)
- Modify: `CA/Gc/CasGc.cpp:466` (`Gc::applyRedeleteOutcome`)
- Test: `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`

**Interfaces:**
- Produces: `void GcMetaWriter::scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round);`
- Produces (test support, namespace `DB::Cas::tests`): `kGateNs`, `kGateLeaderA`, `kGateLeaderB`, `gateRef`,
  `openGatePool`, `publishBlobs`, `dropBlobs`, `committedGcRound`, `retiredEntryFor`, `markerOf`, `condemnedAt`,
  `cleanMarker`, `setMarker`, `removeMarker`, `blobPresent`, `handGcLeaseTo`, `runGateRound`, `runRoundsUntil`,
  `condemnBlobs`, `writeOnePart`, `awaitCondition`, `unconfirmedCarries`, `metaWriteAnomalies`, `readAheadWasted`,
  `HeadHookBackend`, `ApplyParking`, `parkingHook` — signatures in Step 1.

- [ ] **Step 1: Create the test support header**

Create `src/Disks/tests/cas_gc_marker_test_support.h`:

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/ProfileEvents.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace ProfileEvents
{
extern const Event CASGCCondemnMarkerUnconfirmedCarry;
extern const Event CASGCMetaWriteAnomaly;
extern const Event CASGCReadAheadWasted;
}

/// Fixtures for the condemn-marker gate: real GC rounds over a raw ref log, so a test decides which edges each
/// round folds; real writers where adoption is the subject; backends that act at one chosen request.
namespace DB::Cas::tests
{

inline const RootNamespace kGateNs{"00/aa@cas@"};
inline const UInt128 kGateLeaderA = hexToU128("000000000000000000000000000000a1");
inline const UInt128 kGateLeaderB = hexToU128("000000000000000000000000000000b2");

inline BlobRef gateRef(const UInt128 & hash)
{
    return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)};
}

/// A pool that folds every round, so a round's number is the number of rounds run.
template <typename BackendT>
PoolPtr openGatePool(std::shared_ptr<BackendT> backend, uint64_t io_concurrency = 1, uint64_t graduation_budget = 0,
                     std::function<void(const BlobRef &)> apply_hook = {}, uint64_t gc_shards = 1)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test",
                      .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = io_concurrency};
    config.gc_shards = gc_shards;
    config.gc_round_graduation_budget = graduation_budget;
    config.gc_redelete_apply_hook_for_test = std::move(apply_hook);
    return Pool::open(std::move(backend), std::move(config));
}

/// Publishes `ref_name` in `kGateNs` owning every blob of `hashes`, writing each missing body first.
/// `build_sequence` names the manifest, so it must differ between publications.
inline ManifestRef publishBlobs(Backend & backend, const Layout & layout, const String & ref_name,
                                uint64_t build_sequence, const std::vector<UInt128> & hashes)
{
    std::vector<ManifestEntry> entries;
    for (size_t i = 0; i < hashes.size(); ++i)
    {
        bool present = false;
        {
            OperationForTest op(backend);
            present = (*op).head(layout.blobKey(gateRef(hashes[i])), Retry::standard()).has_value();
        }
        if (!present)
            writeBlobBody(backend, layout, hashes[i]);
        entries.push_back(blobEntryFor("f" + std::to_string(i), hashes[i]));
    }
    const ManifestRef manifest{.writer_epoch = 1, .build_sequence = build_sequence, .manifest_ordinal = 0};
    writeManifestRaw(backend, layout, kGateNs, manifest, entries);
    publishCommittedTransition(backend, layout, kGateNs, ref_name, std::nullopt, manifest);
    return manifest;
}

inline void dropBlobs(Backend & backend, const Layout & layout, const String & ref_name, const ManifestRef & manifest)
{
    dropRefTransition(backend, layout, kGateNs, ref_name, manifest);
}

inline uint64_t committedGcRound(Backend & backend, const Layout & layout)
{
    OperationForTest op(backend);
    const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
    return got ? decodeGcState(got->bytes).round : 0;
}

/// The retired entry of `ref` in the adopted seal, looked up in the blob's own GC shard.
inline std::optional<RetiredEntry> retiredEntryFor(Backend & backend, const Layout & layout, const BlobRef & ref,
                                                   uint64_t gc_shards = 1)
{
    for (const RetiredEntry & entry : currentRetiredSet(backend, layout, blobShard(ref, gc_shards)))
        if (entry.ref == ref)
            return entry;
    return std::nullopt;
}

inline std::optional<LoadedMeta> markerOf(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    return loadMeta(*op, layout, ref);
}

inline BlobMeta condemnedAt(uint64_t round, uint64_t size = 1)
{
    return BlobMeta{.state = MetaState::Condemned, .condemn_round = round, .size = size};
}

inline BlobMeta cleanMarker(uint64_t size = 1)
{
    return BlobMeta{.state = MetaState::Clean, .condemn_round = 0, .size = size};
}

/// Replaces the marker of `ref` with `meta`, or creates it when absent.
inline void setMarker(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta)
{
    OperationForTest op(backend);
    const auto current = loadMeta(*op, layout, ref);
    const WriteResult written = current ? casMeta(*op, layout, ref, current->etag, meta)
                                        : putMetaIfAbsent(*op, layout, ref, meta);
    ASSERT_TRUE(std::holds_alternative<Committed>(written)) << layout.blobMetaKey(ref);
}

inline void removeMarker(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    const auto current = loadMeta(*op, layout, ref);
    ASSERT_TRUE(current.has_value()) << layout.blobMetaKey(ref);
    ASSERT_EQ(deleteMetaExact(*op, layout, ref, current->etag), Removal::Removed);
}

inline bool blobPresent(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    return (*op).head(layout.blobKey(ref), Retry::standard()).has_value();
}

/// Moves the GC lease to `owner` as a steal would, without the observation window.
inline void handGcLeaseTo(Backend & backend, const Layout & layout, const UInt128 & owner)
{
    OperationForTest op(backend);
    const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
    ASSERT_TRUE(got.has_value());
    GcState state = decodeGcState(got->bytes);
    state.lease.owner = owner;
    ++state.lease.seq;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(layout.gcStateKey(), encodeGcState(state), got->etag, Retry::once())));
}

inline RoundReport runGateRound(const PoolPtr & store, Gc & gc, UniversePolicy policy = UniversePolicy::Authoritative)
{
    const RoundReport report = gc.runRegularRound({}, /*allow_steal*/ true, policy);
    store->renewWatermarkOnce();
    return report;
}

/// Runs rounds until `done` holds after one of them; fails the test after `max_rounds`.
inline void runRoundsUntil(const PoolPtr & store, Gc & gc, const std::function<bool()> & done, size_t max_rounds = 12)
{
    for (size_t i = 0; i < max_rounds; ++i)
    {
        runGateRound(store, gc);
        if (done())
            return;
    }
    FAIL() << "the condition did not hold within " << max_rounds << " rounds";
}

/// Publishes `hashes`, folds their edges, drops the ref and runs rounds until every blob is condemned. All of
/// them are condemned in one round, written to `condemn_round`.
inline void condemnBlobs(const PoolPtr & store, Gc & gc, Backend & backend, const String & ref_name,
                         uint64_t build_sequence, const std::vector<UInt128> & hashes, uint64_t & condemn_round)
{
    const Layout & layout = store->layout();
    const uint64_t shards = store->poolConfig().gc_shards;
    const ManifestRef manifest = publishBlobs(backend, layout, ref_name, build_sequence, hashes);
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    dropBlobs(backend, layout, ref_name, manifest);
    store->renewWatermarkOnce();
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc, [&]
    {
        for (const UInt128 & hash : hashes)
            if (!retiredEntryFor(backend, layout, gateRef(hash), shards))
                return false;
        return true;
    }));
    condemn_round = retiredEntryFor(backend, layout, gateRef(hashes.front()), shards)->condemn_round;
}

/// Writes one part holding `payload` through the writer. `Observed` means the writer adopted the stored
/// incarnation; `Published` means it uploaded a fresh one.
inline BlobMaterializationAction writeOnePart(const PoolPtr & store, const String & ns, const String & ref_name,
                                              const String & payload)
{
    const RootNamespace root{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref_name;
    auto build = store->beginPartWrite(info);
    ManifestEntry entry;
    entry.path = "data.bin";
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(root, ref_name, id);
    const BlobUploadResult uploaded = build->uploadBlobDetached(BlobUploadRequest{
        .ref = entry.ref, .source = BlobSource::fromString(payload), .declared_size = payload.size()});
    build->mergeBlobUploadResults(std::span<const BlobUploadResult>(&uploaded, 1));
    build->promote(root, ref_name, build->buildId(), id);
    return uploaded.diagnostics.action;
}

/// A bounded spin that fails the test instead of hanging.
inline void awaitCondition(const std::function<bool()> & condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition())
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the awaited condition never held";
        std::this_thread::yield();
    }
}

inline uint64_t unconfirmedCarries()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCCondemnMarkerUnconfirmedCarry].load();
}

inline uint64_t metaWriteAnomalies()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCMetaWriteAnomaly].load();
}

inline uint64_t readAheadWasted()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadWasted].load();
}

/// Runs a hook once, on the first HEAD of one key after `armOnHead`, before that HEAD is served and with no
/// backend lock held. A writer that commits there lands after the round's cut and before the redelete
/// observes the blob.
class HeadHookBackend : public CountingBackend
{
public:
    void armOnHead(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(hook_mutex);
        hooked_key = key;
        on_head = std::move(hook);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        std::function<void()> fire;
        {
            std::lock_guard lock(hook_mutex);
            if (on_head && key == hooked_key)
                fire = std::exchange(on_head, {});
        }
        if (fire)
            fire();
        return CountingBackend::head(key, access);
    }

private:
    std::mutex hook_mutex;
    String hooked_key;
    std::function<void()> on_head;
};

/// Parks the first redelete apply of `target` until released. The parked leader has removed the body and has
/// not yet scheduled its marker delete: the window a successor's rounds run in.
struct ApplyParking
{
    std::atomic<bool> armed{false};
    BlobRef target{};
    ManualBarrier barrier;
};

/// `target` is set before `armed`, and the hook reads `armed` first, so the two never race.
inline std::function<void(const BlobRef &)> parkingHook(const std::shared_ptr<ApplyParking> & parking)
{
    return [parking](const BlobRef & ref)
    {
        if (parking->armed.load() && ref == parking->target && parking->armed.exchange(false))
            parking->barrier.arriveAndWait();
    };
}

}
```

- [ ] **Step 2: Write the failing tests**

In `src/Disks/tests/gtest_cas_gc_meta_writer.cpp` add `#include <Disks/tests/cas_gc_marker_test_support.h>` and
`#include <base/scope_guard.h>` to the includes, and `using namespace DB::Cas::tests;` after the existing
`using` lines. Leave `RealConfirmedMetaDeleteCompletesAcrossOwnerDestruction` unchanged in this step (it uses the
base API; Step 4 passes its round). Append at the end of the file:

```cpp
namespace
{

/// The delete job, scheduled the way `Gc::applyRedeleteOutcome` schedules it, and drained.
void runConfirmedMetaDelete(Gc & gc, const BlobRef & ref, [[maybe_unused]] uint64_t condemn_round)
{
    gc.metaWriterForTest().scheduleConfirmedMetaDelete(ref);
    gc.metaWriterForTest().drain();
}

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend)
{
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteSparesANewerRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u1");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(5)));
    const auto before = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(before.has_value());

    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, ref, 3);

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value()) << "a job for round 3 deleted a Condemned(5) marker";
    EXPECT_EQ(after->meta.condemn_round, 5u);
    EXPECT_EQ(after->etag, before->etag);
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteSparesACleanMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u2");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, cleanMarker()));

    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, ref, 3);

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value()) << "a job deleted a Clean marker";
    EXPECT_EQ(after->meta.state, MetaState::Clean);
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteRemovesItsOwnOrAnOlderRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef own = idOf("u3-own");
    const BlobRef older = idOf("u3-older");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), own, condemnedAt(3)));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), older, condemnedAt(2)));

    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, own, 3);
    runConfirmedMetaDelete(gc, older, 3);

    EXPECT_FALSE(markerOf(*backend, store->layout(), own).has_value());
    EXPECT_FALSE(markerOf(*backend, store->layout(), older).has_value());
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteRefusesAMarkerChangedAfterItsRead)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u6");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(3)));

    backend->setHoldDeletes(true);
    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, ref, 3);
    ASSERT_EQ(backend->pendingDeletes(), 1u) << "the job read Condemned(3) and sent its delete";

    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(5)));
    EXPECT_EQ(backend->landPendingDelete(0), Backend::RawRemoval::Mismatch);

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->meta.condemn_round, 5u);
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteLeavesAnUndecodableMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u7");
    const String key = store->layout().blobMetaKey(ref);
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, "not a marker", Retry::once())));
    }

    const uint64_t anomalies_before = metaWriteAnomalies();
    Gc gc(store, u128Of(kGcId));
    ASSERT_NO_THROW(runConfirmedMetaDelete(gc, ref, 3));

    EXPECT_EQ(metaWriteAnomalies() - anomalies_before, 1u);
    OperationForTest op(*backend);
    const auto raw = (*op).read(key, Retry::standard());
    ASSERT_TRUE(raw.has_value()) << "the job deleted a marker it could not read";
    EXPECT_EQ(raw->bytes, "not a marker");
}

TEST(CASGcMetaWriter, NewLeaderDeletesAGraduatedEntry)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x5005);
    const BlobRef x = gateRef(hash);
    {
        Gc gc_a(store, kGateLeaderA);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc_a, *backend, "tbl", 1, {hash}, c));
        ASSERT_EQ(runGateRound(store, gc_a).graduated, 1u);
        ASSERT_TRUE(retiredEntryFor(*backend, layout, x)->delete_pending);
    }

    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderB));
    Gc gc_b(store, kGateLeaderB);
    const RoundReport report = runGateRound(store, gc_b);

    EXPECT_EQ(report.deleted, 1u);
    EXPECT_FALSE(blobPresent(*backend, layout, x));
    EXPECT_FALSE(markerOf(*backend, layout, x).has_value()) << "the new leader's job left an orphan marker";
}

TEST(CASGcMetaWriter, GraduationAcceptsANewerRoundMarker)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x9009);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, condemnedAt(c + 3)));

    const uint64_t carries_before = unconfirmedCarries();
    backend->resetCounts();
    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
    EXPECT_EQ(unconfirmedCarries() - carries_before, 0u);
    EXPECT_EQ(backend->writeCount(layout.blobMetaKey(x)), 0u) << "an accepted gate writes nothing";

    EXPECT_EQ(runGateRound(store, gc).redeleted, 1u);
    EXPECT_FALSE(blobPresent(*backend, layout, x));
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value()) << "the job for round " << c << " deleted a newer marker";
    EXPECT_EQ(marker->meta.condemn_round, c + 3);
}

namespace
{

enum class ReleaseLeaderA
{
    BeforeGraduation,
    AfterGraduation,
};

/// A deposed leader removes incarnation T0 and parks before its marker delete. Its successor finishes T0,
/// condemns the next incarnation T1 of the same content, and graduates it. The deposed leader's delayed job
/// must leave T1's marker, so a writer arriving after the delete round's cut republishes instead of adopting.
void staleMetaDeleteSparesSuccessorMarker(ReleaseLeaderA release)
{
    auto parking = std::make_shared<ApplyParking>();
    auto backend = std::make_shared<HeadHookBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 1, /*graduation_budget*/ 0, parkingHook(parking));
    const Layout & layout = store->layout();
    const String ns = "test/tbl";
    const String payload = "stale-meta-delete-payload";
    const BlobRef x = idOf(payload);
    parking->target = x;

    ASSERT_EQ(writeOnePart(store, ns, "p0", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p0");
    store->renewWatermarkOnce();
    Gc gc_a(store, kGateLeaderA);
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc_a, [&]
    {
        const auto entry = retiredEntryFor(*backend, layout, x);
        return entry && entry->delete_pending;
    }));
    const uint64_t c0 = retiredEntryFor(*backend, layout, x)->condemn_round;

    /// A's commit `CAS` loses once B has the lease; whether its round throws or returns, only the committed
    /// round is asserted.
    parking->armed.store(true);
    std::exception_ptr leader_a_failure;
    std::thread leader_a([&]
    {
        try
        {
            runGateRound(store, gc_a);
        }
        catch (...)
        {
            leader_a_failure = std::current_exception();
        }
    });
    SCOPE_EXIT({
        parking->barrier.release();
        if (leader_a.joinable())
            leader_a.join();
    });
    parking->barrier.waitUntilArrived();
    ASSERT_FALSE(blobPresent(*backend, layout, x)) << "A removed T0 before parking";

    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderB));
    Gc gc_b(store, kGateLeaderB);
    runGateRound(store, gc_b);
    ASSERT_FALSE(retiredEntryFor(*backend, layout, x).has_value());
    ASSERT_FALSE(markerOf(*backend, layout, x).has_value()) << "B's own job removes T0's marker";

    ASSERT_EQ(writeOnePart(store, ns, "p1", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p1");
    store->renewWatermarkOnce();
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc_b, [&] { return retiredEntryFor(*backend, layout, x).has_value(); }));
    const uint64_t c1 = retiredEntryFor(*backend, layout, x)->condemn_round;
    ASSERT_GE(c1, c0 + 2);

    const auto releaseLeaderA = [&]
    {
        parking->barrier.release();
        leader_a.join();
    };
    if (release == ReleaseLeaderA::BeforeGraduation)
        releaseLeaderA();
    ASSERT_EQ(runGateRound(store, gc_b).graduated, 1u);
    if (release == ReleaseLeaderA::AfterGraduation)
        releaseLeaderA();
    ASSERT_EQ(committedGcRound(*backend, layout), c1 + 1) << "A's attempt committed after losing its lease";

    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value()) << "A's delayed job removed the marker of T1";
    EXPECT_EQ(marker->meta.state, MetaState::Condemned);
    EXPECT_EQ(marker->meta.condemn_round, c1);

    std::optional<BlobMaterializationAction> w2;
    backend->armOnHead(layout.blobKey(x), [&] { w2 = writeOnePart(store, ns, "p2", payload); });
    const RoundReport report = runGateRound(store, gc_b);
    ASSERT_TRUE(w2.has_value()) << "the redelete never observed the blob";
    EXPECT_EQ(*w2, BlobMaterializationAction::Published) << "W2 adopted the incarnation B deletes";
    EXPECT_EQ(report.replaced, 1u);
    ASSERT_TRUE(blobPresent(*backend, layout, x));
    OperationForTest op(*backend);
    const auto body = (*op).read(layout.blobKey(x), Retry::standard());
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE(body->bytes.ends_with(payload)) << "W2's part names a blob that no longer holds its content";
}

}

TEST(CASGcMetaWriter, StaleMetaDeleteSparesSuccessorMarkerBeforeGraduation)
{
    staleMetaDeleteSparesSuccessorMarker(ReleaseLeaderA::BeforeGraduation);
}

TEST(CASGcMetaWriter, StaleMetaDeleteSparesSuccessorMarkerAfterGraduation)
{
    staleMetaDeleteSparesSuccessorMarker(ReleaseLeaderA::AfterGraduation);
}
```

- [ ] **Step 3: Build and run the new tests to see them fail**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ninja -C build unit_tests_dbms > build/build_cas30_t1_red.log 2>&1; echo NINJA_EXIT=$? >> build/build_cas30_t1_red.log`
Then: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CASGcMetaWriter.*' > build/test_cas30_t1_red.log 2>&1; echo GATE_EXIT=$? >> build/test_cas30_t1_red.log )`
Expected: `NINJA_EXIT=0`; FAILED exactly:
- `ConfirmedMetaDeleteSparesANewerRound` — "a job for round 3 deleted a Condemned(5) marker"
- `ConfirmedMetaDeleteSparesACleanMarker` — "a job deleted a Clean marker"
- `GraduationAcceptsANewerRoundMarker` — "the job for round ... deleted a newer marker"
- `StaleMetaDeleteSparesSuccessorMarkerBeforeGraduation` — "A's delayed job removed the marker of T1"
- `StaleMetaDeleteSparesSuccessorMarkerAfterGraduation` — "A's delayed job removed the marker of T1"

Every other `CASGcMetaWriter` test passes. Any other red set is a fixture defect: fix the fixture, not the code.

- [ ] **Step 4: Implement C1**

`CA/Gc/CasGcMetaWriter.h`, replace the declaration and its comment (lines 39–40):

```cpp
    /// Drop the marker of a blob whose body is confirmed deleted or absent, but only while it is `Condemned`
    /// at `condemn_round` or older: a newer marker, or `Clean`, belongs to a later incarnation.
    void scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round);
```

`CA/Gc/CasGcMetaWriter.cpp`, replace `deleteConfirmedMeta` and its comment (lines 82–92):

```cpp
/// A later incarnation that can own a job is always condemned at a larger round, so a newer marker, or
/// `Clean`, is live. `If-Match` the etag read here: a marker changed since the read is left alone.
void deleteConfirmedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref, uint64_t condemn_round)
{
    const auto lm = loadMeta(op, layout, ref);
    if (!lm || lm->meta.state != MetaState::Condemned || lm->meta.condemn_round > condemn_round)
        return;
    deleteMetaExact(op, layout, ref, lm->etag);
}
```

and `GcMetaWriter::scheduleConfirmedMetaDelete` (lines 160–167):

```cpp
void GcMetaWriter::scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round)
{
    submit([st = state, ref, condemn_round]()
    {
        CasOperation op = st->store->openRequests().admit();
        deleteConfirmedMeta(op, st->store->layout(), ref, condemn_round);
    });
}
```

`CA/Gc/CasGc.cpp:466`, in `Gc::applyRedeleteOutcome`:

```cpp
        meta_writer->scheduleConfirmedMetaDelete(entry.ref, entry.condemn_round);
```

In `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`: in `runConfirmedMetaDelete` drop `[[maybe_unused]]` and pass
the round (`scheduleConfirmedMetaDelete(ref, condemn_round)`); in
`RealConfirmedMetaDeleteCompletesAcrossOwnerDestruction` call `scheduleConfirmedMetaDelete(ref, /*condemn_round=*/1)`
(the marker it seeds is `Condemned(1)`).

- [ ] **Step 5: Build and run the meta-writer suite**

Run the Step 3 build (log `build/build_cas30_t1.log`) and the suite (log `build/test_cas30_t1.log`).
Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`, all `CASGcMetaWriter.*` pass.

- [ ] **Step 6: Run the full gate**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas30_t1_gate.log 2>&1; echo GATE_EXIT=$? >> build/test_cas30_t1_gate.log )`
Expected: `GATE_EXIT=0`; test count = Task 0 count + 9.

- [ ] **Step 7: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git status --short
git add -- src/Disks/tests/cas_gc_marker_test_support.h src/Disks/tests/gtest_cas_gc_meta_writer.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
git commit -s -m "Delete a CAS blob marker only at its entry's round or older" \
  -m "A delayed GC marker delete removed whatever marker it read, including the Condemned marker of a later incarnation of the same content. The job now carries its entry's condemn round and leaves Clean and newer-round markers alone."
```

---

### Task 2: The graduation budget is checked before the gate

**Files:**
- Modify: `CA/Gc/CasBlobInDegree.cpp:485-514` (the graduation branch of `settleEntry`)
- Modify: `CA/Gc/CasBlobInDegree.h:39-44` (`RetiredEntry::marker_confirmed` comment), `:355-359` and `:389-397` (merge contract comments)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`, `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`, `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`

**Interfaces:**
- Consumes: Task 1 test support (`openGatePool`, `condemnBlobs`, `runGateRound`, `setMarker`, `publishBlobs`, `readAheadWasted`, `gateRef`, `kGateLeaderA`).
- Produces: `settleEntry` admits a graduation candidate only while `GcRoundWorkBudget::graduationAvailable()`, before calling `confirm_condemned_marker`; a refusal consumes no slot; `marker_confirmed` never skips the gate.

- [ ] **Step 1: Write the failing tests**

`src/Disks/tests/gtest_cas_blob_indegree.cpp`: make `condemnedRec` carry the bit (existing callers pass rows with
`marker_confirmed = false`, so their bytes do not change):

```cpp
SourceEdgeRecord condemnedRec(UInt128 h, const CondemnedRow & row)
{
    return SourceEdgeRecord{.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h)},
                            .source_id = UInt128{0}, .marker = RunMarker::Condemned,
                            .delete_pending = row.delete_pending, .token = row.token,
                            .size = row.size, .condemn_round = row.condemn_round,
                            .marker_confirmed = row.marker_confirmed};
}
```

Append:

```cpp
TEST(CASThreeCursorMerge, GraduationBudgetIsCheckedBeforeTheGate)
{
    for (const size_t refusals : {size_t{0}, size_t{2}})
    {
        SCOPED_TRACE(refusals);
        InMemoryBackend backend;
        DB::Cas::tests::OperationForTest backend_req(backend);
        Layout layout{"pool"};
        const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(10, /*condemn_round*/1, false));

        GcRoundWorkBudget budget;
        budget.max_graduations = 3;
        size_t gate_calls = 0;
        const std::function<bool(const RetiredEntry &)> gate = [&](const RetiredEntry &) { return ++gate_calls > refusals; };

        std::vector<RunRef> runs2;
        RetiredMergeResult rmr;
        foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {}, runs2,
            /*current_round*/5, /*condemn_round*/6, /*head_blob*/{}, /*peek_head*/{}, gate,
            &rmr, /*suppress_destructive*/false, /*out_applied_by_txn_ordinal*/nullptr,
            /*source_retirements*/{}, &budget);

        EXPECT_EQ(gate_calls, 3 + refusals) << "the gate read entries the budget had no slot for";
        EXPECT_EQ(rmr.graduated.size(), 3u);
        EXPECT_EQ(budget.graduations_used, 3u);
        EXPECT_EQ(rmr.still_retired.size(), 10u);
    }
}

TEST(CASThreeCursorMerge, MarkerConfirmedBitDoesNotSkipTheGate)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    CondemnedRow row = condemnedRowFor(/*condemn_round*/1, "t1");
    row.marker_confirmed = true;
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, {{b(1), row}});

    size_t gate_calls = 0;
    const std::function<bool(const RetiredEntry &)> refuse = [&](const RetiredEntry &) { ++gate_calls; return false; };
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {}, runs2,
        /*current_round*/5, /*condemn_round*/6, /*head_blob*/{}, /*peek_head*/{}, refuse, &rmr);

    EXPECT_EQ(gate_calls, 1u) << "a non-pending row's bit skipped the gate";
    EXPECT_TRUE(rmr.graduated.empty());
    ASSERT_EQ(rmr.still_retired.size(), 1u);
    EXPECT_FALSE(rmr.still_retired.front().delete_pending);
}
```

`src/Disks/tests/gtest_cas_gc_meta_writer.cpp`, append:

```cpp
TEST(CASGcMetaWriter, GraduationReadsStayWithinTheBudget)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 1);
        const Layout & layout = store->layout();
        const std::vector<UInt128> hashes{DB::UInt128(0x2001), DB::UInt128(0x2002), DB::UInt128(0x2003)};
        {
            Gc gc(store, kGateLeaderA);
            uint64_t c = 0;
            ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, hashes, c));
        }

        /// A fresh `Gc`: nothing in process can stand in for a read.
        Gc gc(store, kGateLeaderA);
        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        EXPECT_EQ(runGateRound(store, gc).graduated, 1u);

        uint64_t gets = 0;
        for (const UInt128 & hash : hashes)
            gets += backend->getCount(layout.blobMetaKey(gateRef(hash)));
        EXPECT_EQ(gets, 1u) << "the gate read entries the budget did not admit";
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u);
    }
}
```

`src/Disks/tests/gtest_cas_gc_read_ahead.cpp`: add `#include <Disks/tests/cas_gc_marker_test_support.h>` and
`#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>` to the includes, and
`using namespace DB::Cas::tests;` after the existing `using` lines (the names it already imports one by one stay
valid). Append:

```cpp
namespace
{

/// What one graduation round read and wrote for a cohort of ten condemned blobs.
struct GraduationRoundCounts
{
    uint64_t graduated = 0;
    uint64_t gets = 0;      /// `.meta` GETs of the cohort: the gate's and the retries'
    uint64_t writes = 0;    /// `.meta` writes of the cohort: the retries'
    uint64_t wasted = 0;
    std::map<String, uint64_t> gets_by_key;
};

enum class CohortTwist
{
    None,
    TwoClean,           /// the two smallest refs read `Clean` at the gate
    OneReReferenced,    /// the smallest ref gains an owner before the round
    Suppressed,         /// the round's universe is not authoritative
};

/// Condemns ten blobs, rebuilds the `Gc` so nothing in process stands in for a read, applies `twist`, and
/// measures the round due to graduate them under a graduation budget of three.
void measureGraduationRound(uint64_t concurrency, CohortTwist twist, GraduationRoundCounts & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 3);
    const Layout & layout = store->layout();
    std::vector<UInt128> hashes;
    for (uint64_t i = 1; i <= 10; ++i)
        hashes.push_back(DB::UInt128(0x5000 + i));
    {
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, hashes, c));
    }

    std::vector<BlobRef> refs;
    for (const UInt128 & hash : hashes)
        refs.push_back(gateRef(hash));
    std::sort(refs.begin(), refs.end());
    if (twist == CohortTwist::TwoClean)
        for (size_t i = 0; i < 2; ++i)
            ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, refs[i], cleanMarker()));
    if (twist == CohortTwist::OneReReferenced)
        publishBlobs(*backend, layout, "again", 2, {refs[0].digest.toU128()});
    store->renewWatermarkOnce();

    Gc gc(store, kGc);
    backend->resetCounts();
    const uint64_t wasted_before = readAheadWasted();
    const RoundReport report = runGateRound(store, gc,
        twist == CohortTwist::Suppressed ? UniversePolicy::StageA_Suppressed : UniversePolicy::Authoritative);

    out.graduated = report.graduated;
    out.wasted = readAheadWasted() - wasted_before;
    for (const BlobRef & ref : refs)
    {
        const String key = layout.blobMetaKey(ref);
        out.gets += backend->getCount(key);
        out.writes += backend->writeCount(key);
        out.gets_by_key[key] = backend->getCount(key);
    }
}

}

TEST(CASGCReadAhead, GraduationReadAheadRequestCounts)
{
    struct Expected
    {
        CohortTwist twist;
        uint64_t graduated;
        uint64_t gets;
        uint64_t writes;
    };
    for (const Expected & expected : {Expected{CohortTwist::None, 3, 3, 0},
                                      Expected{CohortTwist::TwoClean, 3, 7, 2},
                                      Expected{CohortTwist::OneReReferenced, 3, 3, 0},
                                      Expected{CohortTwist::Suppressed, 0, 0, 0}})
    {
        SCOPED_TRACE(static_cast<int>(expected.twist));
        GraduationRoundCounts sequential;
        GraduationRoundCounts ahead;
        ASSERT_NO_FATAL_FAILURE(measureGraduationRound(1, expected.twist, sequential));
        ASSERT_NO_FATAL_FAILURE(measureGraduationRound(8, expected.twist, ahead));

        EXPECT_EQ(ahead.graduated, expected.graduated);
        EXPECT_EQ(ahead.gets, expected.gets) << "5 gate reads and 2 retry reads when two refuse; 3 otherwise";
        EXPECT_EQ(ahead.writes, expected.writes);
        EXPECT_EQ(ahead.wasted, 0u);
        EXPECT_EQ(ahead.gets_by_key, sequential.gets_by_key);
        EXPECT_EQ(ahead.graduated, sequential.graduated);
        EXPECT_EQ(ahead.writes, sequential.writes);
    }
}
```

- [ ] **Step 2: Build and run to see them fail**

Build (log `build/build_cas30_t2_red.log`), then run with
`--gtest_filter='CASThreeCursorMerge.GraduationBudgetIsCheckedBeforeTheGate:CASThreeCursorMerge.MarkerConfirmedBitDoesNotSkipTheGate:CASGcMetaWriter.GraduationReadsStayWithinTheBudget:CASGCReadAhead.GraduationReadAheadRequestCounts'`
(log `build/test_cas30_t2_red.log`).
Expected FAILED, all four:
- `GraduationBudgetIsCheckedBeforeTheGate` — "the gate read entries the budget had no slot for" (10 calls)
- `MarkerConfirmedBitDoesNotSkipTheGate` — "a non-pending row's bit skipped the gate" (0 calls)
- `GraduationReadsStayWithinTheBudget` — "the gate read entries the budget did not admit" (3)
- `GraduationReadAheadRequestCounts` — `ahead.gets` 10 for `None`

- [ ] **Step 3: Implement**

`CA/Gc/CasBlobInDegree.cpp`, replace the graduation branch of `settleEntry` (from
`else if (!suppress_destructive && e.condemn_round < current_round)` to its closing brace, lines 485–514):

```cpp
        else if (!suppress_destructive && e.condemn_round < current_round)
        {
            /// Budget before the gate: a carried entry must not cost a marker read every round. A refusal
            /// consumes no slot, so refusals cannot crowd out graduations.
            if (work_budget && !work_budget->graduationAvailable())
                rmr.still_retired.push_back(e);
            else if (!confirm_condemned_marker || confirm_condemned_marker(e))
            {
                if (work_budget)
                    ++work_budget->graduations_used;
                RetiredEntry pending = e;       /// newly floor-passed: publish pending; delete NEXT pass
                pending.delete_pending = true;
                pending.marker_confirmed = true;
                rmr.graduated.push_back(pending);
                rmr.still_retired.push_back(std::move(pending));
            }
            else
                rmr.still_retired.push_back(e); /// the gate refused: carried; it has rewritten the marker
        }
```

`CA/Gc/CasBlobInDegree.h`, replace the `marker_confirmed` member comment of `RetiredEntry` (lines 39–44):

```cpp
    bool marker_confirmed = false;   /// Written with `delete_pending`, as released builds write it. The
                                     /// graduation gate never reads it: it reads the marker itself.
```

In the `foldDeltasIntoGeneration` comment, replace the graduation rule line (lines 355–359):

```cpp
///   d = 0 and condemn_round < current_round     -> graduated: REPUBLISHED as delete_pending (two-phase
///                                                 graduation) — deleted the NEXT pass. Only while the
///                                                 round's graduation budget has a slot, and only if
///                                                 `confirm_condemned_marker` accepts; otherwise carried;
```

and the gate paragraph (lines 389–397):

```cpp
/// `confirm_condemned_marker` is the GRADUATION GATE: graduation to `delete_pending` is the one edge that
/// authorizes an irreversible delete, and the per-hash marker is the writer's adopt gate. It is called only
/// for an entry the graduation budget admits, and returns whether the entry may graduate; on false the entry
/// is carried unchanged. Unset (default `{}`) means UNGATED — merge-mechanics unit tests only; the real GC
/// round always passes the gate.
```

- [ ] **Step 4: Build and run the four tests and the three touched suites**

Build (log `build/build_cas30_t2.log`); run `--gtest_filter='CASThreeCursorMerge.*:CASTwoCursorMerge.*:CASBlobInDegree.*:CASGcMetaWriter.*:CASGCReadAhead.*'` (log `build/test_cas30_t2.log`).
Expected: `GATE_EXIT=0`.

- [ ] **Step 5: Full gate**

Run the gate (log `build/test_cas30_t2_gate.log`). Expected `GATE_EXIT=0`. A request-count golden that changed is a
real change in behaviour only if its delta is "fewer gate reads for entries past the budget"; anything else is a
defect to root-cause.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git status --short
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h \
  src/Disks/tests/gtest_cas_blob_indegree.cpp src/Disks/tests/gtest_cas_gc_meta_writer.cpp \
  src/Disks/tests/gtest_cas_gc_read_ahead.cpp
git commit -s -m "Check the CAS graduation budget before the marker gate" \
  -m "A carried entry past the round's graduation budget cost a marker read every round. The merge now admits an entry before the gate, and the marker_confirmed bit no longer skips the gate."
```

---

### Task 3: Graduation reads the marker and accepts only its round or newer (G1, C3)

**Files:**
- Modify: `CA/Gc/CasGcMetaWriter.h` (whole class body: registry out, `scheduleCondemnMarkerWrite` loses `token`)
- Modify: `CA/Gc/CasGcMetaWriter.cpp:29-34` (`condemnMarkerKey`, removed), `:36-59` (rationale comment), `:61-80` (`writeCondemnedMeta`), `:147-158` (`scheduleCondemnMarkerWrite`), `:204-235` (registry, removed)
- Modify: `CA/Gc/CasGc.cpp:469` (`forgetCondemnMarker` in `applyRedeleteOutcome`), `:975-977` (spared loop), `:1015-1026` (replaced loop), `:2036-2047` (`head_blob` write), `:2075-2113` (`confirm_condemned_marker`)
- Modify: `src/Disks/tests/cas_gc_marker_test_support.h` (add `ContentEtagBackend`, `ApplyParking` stays)
- Modify: `src/Disks/tests/gtest_cas_gc_ack_floor.cpp:1412-1457` (comments only)
- Test: `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`, `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`

**Interfaces:**
- Consumes: Task 1 support; Task 2's `settleEntry` (gate called only for admitted entries).
- Produces: `void GcMetaWriter::scheduleCondemnMarkerWrite(const BlobRef & ref, uint64_t condemn_round, uint64_t size);`
  No `noteCondemnMarkerDurable`, `condemnMarkerConfirmedInProcess`, `forgetCondemnMarker`. The gate accepts iff
  the marker read now is `Condemned` with `condemn_round >= entry.condemn_round`; on refusal it schedules
  `scheduleCondemnMarkerWrite(entry.ref, <this round>, entry.size)`.
- Produces (test support): `class ContentEtagBackend : public HeadHookBackend`.

- [ ] **Step 1: Add `ContentEtagBackend` to the support header**

Append inside `namespace DB::Cas::tests` of `src/Disks/tests/cas_gc_marker_test_support.h` (add `#include <expected>`):

```cpp
/// Marker etags derived from the bytes, as S3 derives them: equal bytes carry equal etags, so a replaced
/// marker's etag comes back when the same bytes are written again. Every other key keeps minted values.
class ContentEtagBackend : public HeadHookBackend
{
public:
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = HeadHookBackend::read(key, access);
        if (raw && isMarker(key))
            raw->value = contentValue(raw->bytes);
        return raw;
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = HeadHookBackend::head(key, access);
        if (meta && isMarker(key))
            if (const auto raw = InMemoryBackend::read(key, access))
                meta->value = contentValue(raw->bytes);
        return meta;
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = HeadHookBackend::list(prefix, cursor, limit, access);
        for (RawListedKey & listed : page.keys)
            if (listed.value && isMarker(listed.key))
                if (const auto raw = InMemoryBackend::read(listed.key, access))
                    listed.value = contentValue(raw->bytes);
        return page;
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (!isMarker(key))
            return HeadHookBackend::remove(key, expected_value, access);
        std::lock_guard lock(marker_mutex);
        const auto current = InMemoryBackend::read(key, access);
        if (!current)
            return RawRemoval::Gone;
        if (contentValue(current->bytes) != expected_value)
            return RawRemoval::Mismatch;
        return HeadHookBackend::remove(key, current->value, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (!isMarker(key))
            return HeadHookBackend::write(key, bytes, expected_value, access);
        std::lock_guard lock(marker_mutex);
        std::optional<String> minted_expected;
        if (expected_value)
        {
            const auto current = InMemoryBackend::read(key, access);
            if (!current || contentValue(current->bytes) != *expected_value)
                return std::unexpected(RawConflict{});
            minted_expected = current->value;
        }
        const auto written = HeadHookBackend::write(key, bytes, minted_expected, access);
        if (!written)
            return written;
        return contentValue(bytes);
    }

private:
    static bool isMarker(const String & key) { return key.ends_with(".meta"); }
    static String contentValue(const String & bytes) { return "content-" + hexOf(bytes); }

    std::mutex marker_mutex;
};
```

- [ ] **Step 2: Write the failing tests**

`src/Disks/tests/gtest_cas_gc_meta_writer.cpp`, append:

```cpp
TEST(CASGcMetaWriter, GraduationRereadsTheMarkerOwnMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x1001);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, cleanMarker()));

    const uint64_t carries_before = unconfirmedCarries();
    EXPECT_EQ(runGateRound(store, gc).graduated, 0u) << "graduated over a Clean marker";
    EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->meta.state, MetaState::Condemned);
    EXPECT_EQ(marker->meta.condemn_round, c + 1) << "the retry stamps the round that refused";

    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
}

TEST(CASGcMetaWriter, GraduationRereadsTheMarkerLostAttemptMarker)
{
    auto parking = std::make_shared<ApplyParking>();
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 1, /*graduation_budget*/ 0, parkingHook(parking));
    const Layout & layout = store->layout();
    const UInt128 x_hash = DB::UInt128(0x1101);
    const UInt128 y_hash = DB::UInt128(0x1102);
    const BlobRef x = gateRef(x_hash);
    parking->target = gateRef(y_hash);

    /// X is owned throughout; Y is condemned and graduated, so A's next round both redeletes Y (and parks on it)
    /// and condemns X.
    Gc gc_a(store, kGateLeaderA);
    const ManifestRef x_manifest = publishBlobs(*backend, layout, "x", 1, {x_hash});
    uint64_t y_round = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc_a, *backend, "y", 2, {y_hash}, y_round));
    ASSERT_EQ(runGateRound(store, gc_a).graduated, 1u);
    dropBlobs(*backend, layout, "x", x_manifest);
    store->renewWatermarkOnce();
    const uint64_t c = committedGcRound(*backend, layout) + 1;

    parking->armed.store(true);
    std::exception_ptr leader_a_failure;
    std::thread leader_a([&]
    {
        try
        {
            runGateRound(store, gc_a);
        }
        catch (...)
        {
            leader_a_failure = std::current_exception();
        }
    });
    SCOPE_EXIT({
        parking->barrier.release();
        if (leader_a.joinable())
            leader_a.join();
    });
    parking->barrier.waitUntilArrived();
    ASSERT_NO_FATAL_FAILURE(awaitCondition([&]
    {
        const auto marker = markerOf(*backend, layout, x);
        return marker && marker->meta.state == MetaState::Condemned;
    }));

    /// B commits round c on its own condemnation of X, and the marker A's losing attempt wrote goes.
    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderB));
    {
        Gc gc_b(store, kGateLeaderB);
        runGateRound(store, gc_b);
    }
    ASSERT_EQ(committedGcRound(*backend, layout), c);
    ASSERT_EQ(retiredEntryFor(*backend, layout, x)->condemn_round, c);
    ASSERT_NO_FATAL_FAILURE(removeMarker(*backend, layout, x));

    parking->barrier.release();
    leader_a.join();
    ASSERT_EQ(committedGcRound(*backend, layout), c) << "A's losing attempt committed";

    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderA));
    const uint64_t carries_before = unconfirmedCarries();
    EXPECT_EQ(runGateRound(store, gc_a).graduated, 0u) << "A graduated on its own attempt's marker, which is gone";
    EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->meta.condemn_round, c + 1);
    EXPECT_EQ(runGateRound(store, gc_a).graduated, 1u);
}

TEST(CASGcMetaWriter, GraduationAndDeleteRequestCounts)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x4004);
        const String meta_key = layout.blobMetaKey(gateRef(hash));
        Gc gc(store, kGateLeaderA);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));

        backend->resetCounts();
        ASSERT_EQ(runGateRound(store, gc).graduated, 1u);
        EXPECT_EQ(backend->getCount(meta_key), 1u) << "graduation reads the marker";
        EXPECT_EQ(backend->writeCount(meta_key), 0u);
        EXPECT_EQ(backend->deleteCount(meta_key), 0u);

        backend->resetCounts();
        ASSERT_EQ(runGateRound(store, gc).redeleted, 1u);
        EXPECT_EQ(backend->getCount(meta_key), 1u) << "a pending entry passes no gate; only its job reads";
        EXPECT_EQ(backend->deleteCount(meta_key), 1u);
        EXPECT_FALSE(markerOf(*backend, layout, gateRef(hash)).has_value());
    }
}

TEST(CASGcMetaWriter, LatePublisherCleanMissesTheRetriedMarker)
{
    auto backend = std::make_shared<ContentEtagBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const String ns = "test/tbl";
    const String payload = "late-publisher-clean";
    const BlobRef x = idOf(payload);

    ASSERT_EQ(writeOnePart(store, ns, "p1", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p1");
    store->renewWatermarkOnce();
    Gc gc(store, kGateLeaderA);
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc, [&] { return retiredEntryFor(*backend, layout, x).has_value(); }));
    const uint64_t c = retiredEntryFor(*backend, layout, x)->condemn_round;
    const auto condemned = markerOf(*backend, layout, x);
    ASSERT_TRUE(condemned && condemned->meta.state == MetaState::Condemned);
    const Etag m = condemned->etag;
    const BlobMeta clean{.state = MetaState::Clean, .condemn_round = 0, .size = condemned->meta.size};

    /// The publisher's reissued `Clean If-Match M` lands; its first attempt is still in flight.
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(casMeta(*op, layout, x, m, clean)));
    }

    const uint64_t carries_before = unconfirmedCarries();
    EXPECT_EQ(runGateRound(store, gc).graduated, 0u) << "graduated over the publisher's Clean";
    EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
    const auto retried = markerOf(*backend, layout, x);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->meta.condemn_round, c + 1);
    EXPECT_NE(retried->etag, m) << "the retry recreated the bytes the late request names";
    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);

    /// The first attempt lands now: its precondition names bytes no marker carries.
    {
        OperationForTest op(*backend);
        EXPECT_FALSE(std::holds_alternative<Committed>(casMeta(*op, layout, x, m, clean)));
    }

    std::optional<BlobMaterializationAction> w2;
    backend->armOnHead(layout.blobKey(x), [&] { w2 = writeOnePart(store, ns, "p2", payload); });
    const RoundReport report = runGateRound(store, gc);
    ASSERT_TRUE(w2.has_value()) << "the redelete never observed the blob";
    EXPECT_EQ(*w2, BlobMaterializationAction::Published) << "W2 adopted the incarnation GC deletes";
    EXPECT_EQ(report.replaced, 1u);
    EXPECT_TRUE(blobPresent(*backend, layout, x));
}

TEST(CASGcMetaWriter, GraduationCarriesAnUndecodableMarker)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x1010);
        const BlobRef x = gateRef(hash);
        Gc gc(store, kGateLeaderA);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
        {
            OperationForTest op(*backend);
            const String key = layout.blobMetaKey(x);
            const auto current = (*op).read(key, Retry::standard());
            ASSERT_TRUE(current.has_value());
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, "not a marker", current->etag, Retry::once())));
        }

        const uint64_t anomalies_before = metaWriteAnomalies();
        const uint64_t carries_before = unconfirmedCarries();
        RoundReport report;
        ASSERT_NO_THROW(report = runGateRound(store, gc));
        EXPECT_EQ(report.graduated, 0u) << "graduated on a marker nobody could read";
        EXPECT_EQ(metaWriteAnomalies() - anomalies_before, 2u) << "the gate's read and the retry's read";
        EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
        EXPECT_EQ(committedGcRound(*backend, layout), c + 1) << "the round committed";
        EXPECT_FALSE(retiredEntryFor(*backend, layout, x)->delete_pending);
        EXPECT_TRUE(blobPresent(*backend, layout, x));
    }
}
```

`src/Disks/tests/gtest_cas_gc_read_ahead.cpp`, append:

```cpp
namespace
{

/// Journals every GET in arrival order, so a test can place the gate's marker read after the round's cut.
class GetJournalBackend : public CountingBackend
{
public:
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        {
            std::lock_guard lock(journal_mutex);
            journal.push_back(key);
        }
        return CountingBackend::read(key, access);
    }

    std::vector<String> takeJournal()
    {
        std::lock_guard lock(journal_mutex);
        return std::exchange(journal, {});
    }

private:
    std::mutex journal_mutex;
    std::vector<String> journal;
};

/// Once armed, rewrites `key` with `replacement` right after a GET of it returns, with no backend lock held:
/// the value the reader got is stale by the time it arrives.
class ReadCompletionHookBackend : public CountingBackend
{
public:
    void armAfterRead(const String & key, String replacement)
    {
        std::lock_guard lock(hook_mutex);
        hooked_key = key;
        replacement_bytes = std::move(replacement);
        armed = true;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = CountingBackend::read(key, access);
        std::optional<String> rewrite;
        {
            std::lock_guard lock(hook_mutex);
            if (armed && raw && key == hooked_key)
            {
                armed = false;
                rewrite = replacement_bytes;
            }
        }
        if (rewrite)
            (void)InMemoryBackend::write(key, *rewrite, raw->value, access);
        return raw;
    }

private:
    std::mutex hook_mutex;
    String hooked_key;
    String replacement_bytes;
    bool armed = false;
};

}

TEST(CASGCReadAhead, GraduationReadAheadReadsEachRound)
{
    auto backend = std::make_shared<GetJournalBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 8);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x3001);
    const BlobRef x = gateRef(hash);
    const String meta_key = layout.blobMetaKey(x);
    Gc gc(store, kGc);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, cleanMarker()));

    /// An unrelated publication gives each measured round ref-log reads to order the gate's read against.
    uint64_t noise = 100;
    const auto roundAfterPublication = [&]
    {
        ++noise;
        publishBlobs(*backend, layout, "noise" + std::to_string(noise), noise, {DB::UInt128(0x3100 + noise)});
        store->renewWatermarkOnce();
        backend->resetCounts();
        backend->takeJournal();
        const RoundReport report = runGateRound(store, gc);
        return std::make_pair(report, backend->takeJournal());
    };
    const auto expectReadAfterCut = [&](const std::vector<String> & journal)
    {
        const auto meta_at = std::find(journal.begin(), journal.end(), meta_key);
        ASSERT_NE(meta_at, journal.end());
        const auto last_log = std::find_if(journal.rbegin(), journal.rend(),
                                           [](const String & key) { return key.contains("/_log/"); });
        ASSERT_NE(last_log, journal.rend()) << "the round read no ref log";
        const size_t last_log_at = static_cast<size_t>(journal.rend() - last_log) - 1;
        EXPECT_LT(last_log_at, static_cast<size_t>(meta_at - journal.begin()))
            << "the gate read the marker before its round's cut";
    };

    const auto [refused, refused_journal] = roundAfterPublication();
    EXPECT_EQ(refused.graduated, 0u);
    EXPECT_EQ(backend->getCount(meta_key), 2u) << "the gate's read and the retry's read";
    EXPECT_EQ(backend->writeCount(meta_key), 1u);
    expectReadAfterCut(refused_journal);
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->meta.condemn_round, c + 1);

    const auto [accepted, accepted_journal] = roundAfterPublication();
    EXPECT_EQ(accepted.graduated, 1u);
    EXPECT_EQ(backend->getCount(meta_key), 1u) << "every round reads the marker again";
    expectReadAfterCut(accepted_journal);
}

TEST(CASGCReadAhead, GraduationReadAheadDecidesOnTheFetchedValue)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<ReadCompletionHookBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x3101);
        const BlobRef x = gateRef(hash);
        const String meta_key = layout.blobMetaKey(x);
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));

        backend->resetCounts();
        backend->armAfterRead(meta_key, encodeBlobMeta(cleanMarker()));
        EXPECT_EQ(runGateRound(store, gc).graduated, 1u) << "the gate did not decide on the value it fetched";
        EXPECT_EQ(backend->getCount(meta_key), 1u);
        const auto after = markerOf(*backend, layout, x);
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->meta.state, MetaState::Clean) << "the hook never fired";
    }
}

TEST(CASGCReadAhead, GraduationReadAheadSurfacesAReadError)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x3201);
        const String meta_key = layout.blobMetaKey(gateRef(hash));
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));

        backend->failNextReadWith(meta_key, std::make_exception_ptr(std::runtime_error("injected marker read fault")));
        const uint64_t anomalies_before = metaWriteAnomalies();
        const uint64_t carries_before = unconfirmedCarries();
        RoundReport report;
        ASSERT_NO_THROW(report = runGateRound(store, gc));
        EXPECT_EQ(report.graduated, 0u) << "graduated without reading the marker";
        EXPECT_EQ(metaWriteAnomalies() - anomalies_before, 1u);
        EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
        EXPECT_EQ(committedGcRound(*backend, layout), c + 1) << "a failed marker read failed the round";
        EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
    }
}
```

- [ ] **Step 3: Build and run to see them fail**

Build (log `build/build_cas30_t3_red.log`); run
`--gtest_filter='CASGcMetaWriter.GraduationRereads*:CASGcMetaWriter.GraduationAndDeleteRequestCounts:CASGcMetaWriter.LatePublisherCleanMissesTheRetriedMarker:CASGcMetaWriter.GraduationCarriesAnUndecodableMarker:CASGCReadAhead.GraduationReadAheadReadsEachRound:CASGCReadAhead.GraduationReadAheadDecidesOnTheFetchedValue:CASGCReadAhead.GraduationReadAheadSurfacesAReadError'`
(log `build/test_cas30_t3_red.log`).
Expected FAILED, all eight: the in-process confirmation graduates each at its first eligible round
("graduated over a Clean marker", "A graduated on its own attempt's marker, which is gone", "graduation reads the
marker" (0), "graduated over the publisher's Clean", "graduated on a marker nobody could read", the RA1 count 0,
"the hook never fired", "graduated without reading the marker").

- [ ] **Step 4: Remove the registry and the token from `GcMetaWriter`**

`CA/Gc/CasGcMetaWriter.h`: replace the `scheduleCondemnMarkerWrite` declaration and comment (lines 32–37):

```cpp
    /// Write `Condemned` at `condemn_round` for one blob: create it over an absent marker, replace `Clean`,
    /// leave a `Condemned` marker alone. A failed write is counted and logged; the graduation gate reads the
    /// marker itself and schedules the write again.
    void scheduleCondemnMarkerWrite(const BlobRef & ref, uint64_t condemn_round, uint64_t size);
```

Delete the registry block (lines 54–61: the comment and the three methods) and, in `struct State`, the members
`condemn_marker_mutex`, `condemn_markers_confirmed` and the three method declarations (lines 71–76). `State`
keeps `store`, `logger`, `scheduled`, `completed`.

`CA/Gc/CasGcMetaWriter.cpp`:
- Delete `condemnMarkerKey` (lines 29–34) and the registry definitions (lines 204–235).
- Replace the rationale comment (lines 36–59) with:

```cpp
/// The per-hash marker operations GC schedules on the bounded pool. The marker is the writer's adopt gate:
/// a writer that reads `Condemned` republishes instead of adopting the incarnation GC will delete. The ledger
/// and the exact-token body delete stay the safety core; round rules keep a marker from standing for the
/// wrong incarnation:
///   - every GC write stamps the round of the attempt that issues it;
///   - graduation accepts only `Condemned` at the entry's round or newer, read in that round (`Gc::fold`);
///   - a delete job removes only `Condemned` at its entry's round or older, `If-Match` the etag it read.
/// A later incarnation that can own a job is always condemned at a larger round, so no job removes its marker.
///
/// GC never writes `Clean`, not even on a spare: a deposed leader that cleared a spare's marker and then lost
/// its round CAS would leave `Clean` over a still-condemned body, and a stale exact-token redelete would delete
/// what a writer adopted. Only a writer that displaced the body writes `Clean` (`PartWriteTxn::ensureBlobPresent`).
```

- Replace `writeCondemnedMeta` and its comment (lines 61–80):

```cpp
void writeCondemnedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref,
                        uint64_t condemn_round, uint64_t size)
{
    const auto lm = loadMeta(op, layout, ref);
    const BlobMeta desired{.state = MetaState::Condemned, .condemn_round = condemn_round, .size = size};
    if (!lm)
        putMetaIfAbsent(op, layout, ref, desired);
    else if (lm->meta.state != MetaState::Condemned)
        casMeta(op, layout, ref, lm->etag, desired);
}
```

- Replace `GcMetaWriter::scheduleCondemnMarkerWrite` (lines 147–158):

```cpp
void GcMetaWriter::scheduleCondemnMarkerWrite(const BlobRef & ref, uint64_t condemn_round, uint64_t size)
{
    /// The job admits its OWN operation: a `CasOperation` carries per-call state and belongs to one
    /// task, while several of these run concurrently on the pool.
    submit([st = state, ref, condemn_round, size]()
    {
        CasOperation op = st->store->openRequests().admit();
        writeCondemnedMeta(op, st->store->layout(), ref, condemn_round, size);
    });
}
```

- [ ] **Step 5: Rewrite the gate and drop the registry calls in `CA/Gc/CasGc.cpp`**

- `Gc::applyRedeleteOutcome`: delete the two lines
  `/// The entry left the pipeline — drop its in-process condemn-marker confirmation.` and
  `meta_writer->forgetCondemnMarker(entry.ref, entry.token);` (lines 468–469).
- The spared loop: delete the same two lines at 976–977.
- The replaced loop (lines 1015–1026): replace the comment and the two calls with

```cpp
            /// The supersede is also a blob entering the retired set: write its marker like a fresh
            /// `head_blob` condemn, so a writer's point-read sees it. `peek_head` stays side-effect-free.
            meta_writer->scheduleCondemnMarkerWrite(entry.ref, entry.condemn_round, entry.size);
```

- `head_blob`, the write and its comment (lines 2036–2047):

```cpp
        /// This candidate always becomes a fresh `RetiredEntry` in `closeBlob` (the only caller of
        /// `head_blob`), so this is the round's side-effecting condemn site. The job captures everything by
        /// value: the fold mutates `cur_blob` while the job is queued.
        meta_writer->scheduleCondemnMarkerWrite(ref, condemn_round, adjusted.size);
```

- `confirm_condemned_marker` and its comment (lines 2075–2113):

```cpp
    /// Graduation gate. Publishing `delete_pending` authorizes an irreversible delete, and the marker is the
    /// writer's adopt gate, so the marker is read every time, after the round's cut. A writer that adopted saw
    /// a non-`Condemned` marker before this read; its edge is then in the delete round's cut and spares the
    /// entry. A refusal carries the entry and rewrites the marker; a failed read is missing evidence and
    /// never wedges the round.
    const auto confirm_condemned_marker = [&](const RetiredEntry & entry) -> bool
    {
        try
        {
            /// An older round may be another incarnation's marker, which that incarnation's delete job may remove.
            if (const auto lm = loadMeta(op, layout, entry.ref);
                lm && lm->meta.state == MetaState::Condemned && lm->meta.condemn_round >= entry.condemn_round)
                return true;
        }
        catch (...)
        {
            ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);
            tryLogCurrentException(logger,
                "CAS gc: the graduation gate failed to read the condemn marker (the entry is carried; "
                "never wedges the round)");
        }
        ProfileEvents::increment(ProfileEvents::CASGCCondemnMarkerUnconfirmedCarry);
        /// This round, not the entry's: a publisher's late `Clean If-Match` names the bytes it read, and on S3
        /// an entry-round rewrite recreates exactly those bytes.
        meta_writer->scheduleCondemnMarkerWrite(entry.ref, condemn_round, entry.size);
        return false;
    };
```

- [ ] **Step 6: Update the existing tests and comments**

`src/Disks/tests/gtest_cas_gc_meta_writer.cpp`:
- Delete `TEST(CASGcMetaWriter, CondemnMarkerConfirmationIsVisibleAfterDrain)` and its comment.
- In `RealCondemnMarkerJobCompletesAcrossOwnerDestruction` delete the `token` local and call
  `scheduleCondemnMarkerWrite(ref, /*condemn_round=*/1, /*size=*/128)`.
- In the comment of `ThrowingRoundDrainsBeforeReturning`, replace "their effects would land in the registry the
  next round's graduation gate reads, and inside its counter deltas" with "their effects would land inside the
  next round's marker reads and counter deltas".

`src/Disks/tests/gtest_cas_gc_ack_floor.cpp`, test `LoadMetaFallbackConfirmsGraduationAfterLeaderRestart`
(lines 1412–1457): comments only. Replace the header comment with

```cpp
/// The leader-restart path: a fresh `Gc` under the same identity models a process restart that resumes its own
/// lease. It never observed the condemn round, and still graduates on the durable `Condemned` marker it reads.
```

replace "Its `condemn_markers_confirmed` registry dies with it." with "Nothing it observed survives it.", and replace
the comment before `Gc gc2` with `/// A brand-new `Gc` under the same identity: only the marker read can authorize graduation.`

Grep for leftovers: `grep -rn "condemnMarkerConfirmedInProcess\|noteCondemnMarkerDurable\|forgetCondemnMarker\|condemn_markers_confirmed\|in-process (hash, token)\|in-process confirmation" src/Disks`
Expected: no output. Reword any remaining comment hit; it describes code that no longer exists.

- [ ] **Step 7: Build and run the eight tests, then the gate**

Build (log `build/build_cas30_t3.log`); run the Step 3 filter (log `build/test_cas30_t3.log`), expected
`GATE_EXIT=0`; then the full gate (log `build/test_cas30_t3_gate.log`).
Expected `GATE_EXIT=0`. A `CountingBackend` golden that now sees one more `.meta` `GET` per graduation is the
intended change: update its expected count and its comment ("one marker read per graduation"); any other delta is
a defect to root-cause.

- [ ] **Step 8: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git status --short
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/tests/cas_gc_marker_test_support.h src/Disks/tests/gtest_cas_gc_meta_writer.cpp \
  src/Disks/tests/gtest_cas_gc_read_ahead.cpp src/Disks/tests/gtest_cas_gc_ack_floor.cpp
# plus every golden test file Step 7 updated, by exact path
git commit -s -m "Read the CAS condemn marker at every graduation" \
  -m "Graduation trusted an in-process record of a successful marker write, which outlived failed attempts and a publisher's Clean. The gate now reads the marker each time, accepts only Condemned at the entry's round or newer, and stamps its retry with the current round."
```

---

### Task 4: A GC marker write replaces an older-round `Condemned` (C2)

**Files:**
- Modify: `CA/Gc/CasGcMetaWriter.cpp` (`writeCondemnedMeta`, rationale bullet)
- Modify: `CA/Gc/CasGcMetaWriter.h` (`scheduleCondemnMarkerWrite` comment)
- Test: `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`

**Interfaces:**
- Consumes: Task 3's `scheduleCondemnMarkerWrite(ref, condemn_round, size)`.
- Produces: `writeCondemnedMeta(r)` leaves the marker only if it is `Condemned` at a round `>= r`; it replaces
  absent, `Clean` and an older-round `Condemned`.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`:

```cpp
TEST(CASGcMetaWriter, CondemnMarkerWriteRestampsAnOlderRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u4");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(3, 64)));

    Gc gc(store, u128Of(kGcId));
    gc.metaWriterForTest().scheduleCondemnMarkerWrite(ref, /*condemn_round=*/5, /*size=*/64);
    gc.metaWriterForTest().drain();

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->meta.state, MetaState::Condemned);
    EXPECT_EQ(after->meta.condemn_round, 5u) << "an older-round marker may be another incarnation's";
}

TEST(CASGcMetaWriter, CondemnMarkerWriteLeavesAnEqualOrNewerRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef equal = idOf("u5-equal");
    const BlobRef newer = idOf("u5-newer");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), equal, condemnedAt(5, 64)));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), newer, condemnedAt(7, 64)));
    const auto equal_before = markerOf(*backend, store->layout(), equal);
    const auto newer_before = markerOf(*backend, store->layout(), newer);

    Gc gc(store, u128Of(kGcId));
    gc.metaWriterForTest().scheduleCondemnMarkerWrite(equal, /*condemn_round=*/5, /*size=*/64);
    gc.metaWriterForTest().scheduleCondemnMarkerWrite(newer, /*condemn_round=*/5, /*size=*/64);
    gc.metaWriterForTest().drain();

    EXPECT_EQ(markerOf(*backend, store->layout(), equal)->etag, equal_before->etag);
    EXPECT_EQ(markerOf(*backend, store->layout(), newer)->etag, newer_before->etag);
    EXPECT_EQ(markerOf(*backend, store->layout(), newer)->meta.condemn_round, 7u);
}

TEST(CASGcMetaWriter, GraduationRefusesAnOlderRoundMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x2202);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_GE(c, 2u);
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, condemnedAt(c - 1)));

    EXPECT_EQ(runGateRound(store, gc).graduated, 0u) << "accepted an older-round marker";
    const auto restamped = markerOf(*backend, layout, x);
    ASSERT_TRUE(restamped.has_value());
    EXPECT_EQ(restamped->meta.condemn_round, c + 1) << "the retry left the older round in place";

    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
    EXPECT_EQ(runGateRound(store, gc).redeleted, 1u);
    EXPECT_FALSE(blobPresent(*backend, layout, x));
    const auto orphan = markerOf(*backend, layout, x);
    ASSERT_TRUE(orphan.has_value()) << "the job for round " << c << " removed a newer marker";
    EXPECT_EQ(orphan->meta.condemn_round, c + 1);
}

TEST(CASGcMetaWriter, SparedThenRecondemnedBlobGraduates)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x3303);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t r1 = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, r1));

    /// A new owner spares the entry; GC never clears a marker, so `Condemned(r1)` stays.
    const ManifestRef again = publishBlobs(*backend, layout, "tbl2", 2, {hash});
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    ASSERT_FALSE(retiredEntryFor(*backend, layout, x).has_value());
    ASSERT_EQ(markerOf(*backend, layout, x)->meta.condemn_round, r1);

    dropBlobs(*backend, layout, "tbl2", again);
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    const auto recondemned = retiredEntryFor(*backend, layout, x);
    ASSERT_TRUE(recondemned.has_value());
    ASSERT_GT(recondemned->condemn_round, r1);

    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc, [&]
    {
        const auto entry = retiredEntryFor(*backend, layout, x);
        return entry && entry->delete_pending;
    }, /*max_rounds*/ 2)) << "a spared, re-condemned blob never graduates";
}
```

- [ ] **Step 2: Build and run to see them fail**

Build (log `build/build_cas30_t4_red.log`); run
`--gtest_filter='CASGcMetaWriter.CondemnMarkerWrite*:CASGcMetaWriter.GraduationRefusesAnOlderRoundMarker:CASGcMetaWriter.SparedThenRecondemnedBlobGraduates'`
(log `build/test_cas30_t4_red.log`).
Expected FAILED: `CondemnMarkerWriteRestampsAnOlderRound` ("an older-round marker may be another incarnation's"),
`GraduationRefusesAnOlderRoundMarker` ("the retry left the older round in place"),
`SparedThenRecondemnedBlobGraduates` ("the condition did not hold within 2 rounds").
`CondemnMarkerWriteLeavesAnEqualOrNewerRound` passes (guard).

- [ ] **Step 3: Implement**

`CA/Gc/CasGcMetaWriter.cpp`, `writeCondemnedMeta`:

```cpp
void writeCondemnedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref,
                        uint64_t condemn_round, uint64_t size)
{
    const auto lm = loadMeta(op, layout, ref);
    const BlobMeta desired{.state = MetaState::Condemned, .condemn_round = condemn_round, .size = size};
    if (!lm)
        putMetaIfAbsent(op, layout, ref, desired);
    /// An older round may be another incarnation's marker, which that incarnation's delete job may remove.
    else if (lm->meta.state != MetaState::Condemned || lm->meta.condemn_round < condemn_round)
        casMeta(op, layout, ref, lm->etag, desired);
}
```

In the rationale comment replace the first bullet with
`///   - every GC write stamps the round of the attempt that issues it, and replaces an absent marker, `Clean`
///     or an older-round `Condemned`;`

`CA/Gc/CasGcMetaWriter.h`, the `scheduleCondemnMarkerWrite` comment:

```cpp
    /// Write `Condemned` at `condemn_round` for one blob: create it over an absent marker, replace `Clean` or an
    /// older-round `Condemned`, leave one at `condemn_round` or newer. A failed write is counted and logged; the
    /// graduation gate reads the marker itself and schedules the write again.
```

- [ ] **Step 4: Build, run the four tests, then the gate**

Build (log `build/build_cas30_t4.log`); the Step 2 filter (log `build/test_cas30_t4.log`) and the gate
(log `build/test_cas30_t4_gate.log`). Expected `GATE_EXIT=0` both.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git status --short
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp \
  src/Disks/tests/gtest_cas_gc_meta_writer.cpp
git commit -s -m "Re-stamp an older-round CAS condemn marker on every GC write" \
  -m "A Condemned marker of an earlier incarnation could stand for the current one, and a spared then re-condemned blob never passed the round check at graduation. A GC marker write now replaces an older-round Condemned."
```

---

### Task 5: The merge offers later condemned rows to a graduation read-ahead

**Files:**
- Modify: `CA/Gc/CasBlobInDegree.h` (after `CondemnedRow`: `GraduationReadHints`, `kGraduationLookaheadRows`; `foldDeltasIntoGeneration` gains `graduation_reads`, line 421)
- Modify: `CA/Gc/CasBlobInDegree.cpp:53-160` (`PriorEdgeCursor`), `:371` (definition parameter), the graduation branch of `settleEntry`; add `#include <deque>`
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Consumes: Task 2's budget-first `settleEntry`.
- Produces:

```cpp
struct GraduationReadHints
{
    std::function<bool(const BlobRef &)> admit;
    std::function<bool(const BlobRef &, const CondemnedRow &)> offer;
};
inline constexpr size_t kGraduationLookaheadRows = 16384;

void foldDeltasIntoGeneration(..., GcRoundWorkBudget * work_budget = nullptr,
                              const GraduationReadHints * graduation_reads = nullptr);
```

  With `graduation_reads` set, `settleEntry` calls `admit(e.ref)` instead of `work_budget->graduationAvailable()`,
  and after an admission calls `cursor.scanAhead(kGraduationLookaheadRows, graduation_reads->offer)` before the gate.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_blob_indegree.cpp` (add `#include <set>`):

```cpp
namespace
{

GraduationReadHints acceptAllHints(std::vector<BlobRef> & offered)
{
    return GraduationReadHints{
        .admit = [](const BlobRef &) { return true; },
        .offer = [&offered](const BlobRef & ref, const CondemnedRow &)
        {
            offered.push_back(ref);
            return true;
        }};
}

std::vector<std::pair<UInt128, CondemnedRow>> condemnedRowsOf(const std::vector<BlobRef> & refs)
{
    std::vector<std::pair<UInt128, CondemnedRow>> rows;
    for (size_t i = 0; i < refs.size(); ++i)
        rows.push_back({refs[i].digest.toU128(), condemnedRowFor(1, "t" + std::to_string(i))});
    return rows;
}

}

TEST(CASThreeCursorMerge, ScanAheadStopsAtTheRowCap)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const uint64_t n = kGraduationLookaheadRows + 100;
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(n, 1, false));

    std::vector<BlobRef> offered;
    const GraduationReadHints hints = acceptAllHints(offered);
    std::optional<size_t> offers_at_first_gate;
    const std::function<bool(const RetiredEntry &)> gate = [&](const RetiredEntry &)
    {
        if (!offers_at_first_gate)
            offers_at_first_gate = offered.size();
        return true;
    };

    std::vector<RunRef> with_scan;
    RetiredMergeResult scanned;
    foldDeltasIntoGeneration(*backend_req, layout, {gen1}, 2, 0, 0, {}, with_scan, /*current_round*/5,
        /*condemn_round*/6, {}, {}, gate, &scanned, false, nullptr, {}, nullptr, &hints);

    ASSERT_TRUE(offers_at_first_gate.has_value());
    EXPECT_EQ(*offers_at_first_gate, kGraduationLookaheadRows + 1) << "the current row and a full buffer, no more";
    EXPECT_EQ(offered.size(), n - 1) << "every row after the first is offered";
    EXPECT_EQ(std::set<BlobRef>(offered.begin(), offered.end()).size(), n - 1) << "a row was offered twice";

    /// Same output key: a different run would fail the write-once replay with `CORRUPTED_DATA`.
    std::vector<RunRef> without_scan;
    RetiredMergeResult plain;
    const std::function<bool(const RetiredEntry &)> accept = [](const RetiredEntry &) { return true; };
    foldDeltasIntoGeneration(*backend_req, layout, {gen1}, 2, 0, 0, {}, without_scan, 5, 6, {}, {}, accept, &plain);
    ASSERT_EQ(with_scan.size(), 1u);
    ASSERT_EQ(without_scan.size(), 1u);
    EXPECT_EQ(with_scan.front().checksum, without_scan.front().checksum);
    EXPECT_EQ(scanned.graduated.size(), plain.graduated.size());
}

TEST(CASThreeCursorMerge, ScanAheadStopsAtTheSegmentEnd)
{
    DB::Cas::tests::CountingBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    std::vector<BlobRef> refs;
    for (uint64_t i = 1; i <= 20; ++i)
        refs.push_back(bh(i));
    std::sort(refs.begin(), refs.end());
    const std::vector<BlobRef> first(refs.begin(), refs.begin() + 10);
    const std::vector<BlobRef> second(refs.begin() + 10, refs.end());
    const RunRef seg1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedRowsOf(first));
    const RunRef seg2 = writeSourceEdgeRun(backend, layout, 1, 1, 0, condemnedRowsOf(second));
    const std::set<BlobRef> second_set(second.begin(), second.end());

    size_t early_offers = 0;
    std::vector<BlobRef> offered;
    const GraduationReadHints hints{
        .admit = [](const BlobRef &) { return true; },
        .offer = [&](const BlobRef & ref, const CondemnedRow &)
        {
            if (second_set.contains(ref) && backend.getStreamCount(seg2.key) == 0)
                ++early_offers;
            offered.push_back(ref);
            return true;
        }};
    std::vector<BlobRef> gated;
    const std::function<bool(const RetiredEntry &)> gate = [&](const RetiredEntry & e) { gated.push_back(e.ref); return true; };

    std::vector<RunRef> with_scan;
    foldDeltasIntoGeneration(*backend_req, layout, {seg1, seg2}, 2, 0, 0, {}, with_scan, 5, 6, {}, {}, gate,
        nullptr, false, nullptr, {}, nullptr, &hints);

    EXPECT_EQ(early_offers, 0u) << "a row of segment 2 was offered before the merge opened it";
    EXPECT_EQ(offered.size(), 19u) << "offers resume once the next segment is open";
    ASSERT_EQ(gated.size(), 20u);
    EXPECT_EQ(gated[9], first.back()) << "the last entry of segment 1 is still admitted and read";

    std::vector<RunRef> without_scan;
    const std::function<bool(const RetiredEntry &)> accept = [](const RetiredEntry &) { return true; };
    foldDeltasIntoGeneration(*backend_req, layout, {seg1, seg2}, 2, 0, 0, {}, without_scan, 5, 6, {}, {}, accept);
    EXPECT_EQ(with_scan.front().checksum, without_scan.front().checksum);
}

TEST(CASThreeCursorMerge, ScanAheadKeepsRowAndChecksumValidation)
{
    Layout layout{"pool"};
    std::vector<BlobRef> unused;
    const GraduationReadHints hints = acceptAllHints(unused);
    const std::function<bool(const RetiredEntry &)> accept = [](const RetiredEntry &) { return true; };

    for (const bool with_scan : {false, true})
    {
        SCOPED_TRACE(with_scan);
        /// A duplicate sentinel in the middle of a condemned cohort.
        {
            InMemoryBackend backend;
            DB::Cas::tests::OperationForTest op(backend);
            std::vector<SourceEdgeRecord> recs;
            for (uint64_t i = 1; i <= 6; ++i)
                recs.push_back(condemnedRec(b(i), condemnedRowFor(1, "t" + std::to_string(i))));
            std::sort(recs.begin(), recs.end(), [](const SourceEdgeRecord & l, const SourceEdgeRecord & r) { return l.ref < r.ref; });
            const SourceEdgeRecord duplicate = recs[2];
            recs.insert(recs.begin() + 3, duplicate);
            DB::WriteBufferFromOwnString out;
            SourceEdgeRunWriter writer(out);
            for (const SourceEdgeRecord & rec : recs)
                writer.append(rec);
            writer.finish();
            out.finalize();
            const String bytes = out.str();
            const RunRef bad{.key = layout.blobTargetRunKey(1, 0, 0, 0), .checksum = sourceEdgeRunChecksum(bytes),
                             .shard = 0, .key_generation = 1};
            (*op).create(bad.key, bytes, Retry::once());

            std::vector<RunRef> out_runs;
            DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
            {
                foldDeltasIntoGeneration(*op, layout, {bad}, 2, 0, 0, {}, out_runs, 5, 6, {}, {}, accept, nullptr,
                    false, nullptr, {}, nullptr, with_scan ? &hints : nullptr);
            });
            EXPECT_TRUE(out_runs.empty());
            EXPECT_FALSE((*op).read(layout.blobTargetRunKey(2, 0, 0, 0), Retry::once()).has_value()) << "a successor run was written";
        }
        /// A bad seal checksum on segment 2.
        {
            InMemoryBackend backend;
            DB::Cas::tests::OperationForTest op(backend);
            std::vector<BlobRef> refs;
            for (uint64_t i = 1; i <= 8; ++i)
                refs.push_back(bh(i));
            std::sort(refs.begin(), refs.end());
            const RunRef seg1 = writeSourceEdgeRun(backend, layout, 1, 0, 0,
                condemnedRowsOf(std::vector<BlobRef>(refs.begin(), refs.begin() + 4)));
            RunRef seg2 = writeSourceEdgeRun(backend, layout, 1, 1, 0,
                condemnedRowsOf(std::vector<BlobRef>(refs.begin() + 4, refs.end())));
            seg2.checksum = seg2.checksum + 1;

            std::vector<RunRef> out_runs;
            DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
            {
                foldDeltasIntoGeneration(*op, layout, {seg1, seg2}, 2, 0, 0, {}, out_runs, 5, 6, {}, {}, accept,
                    nullptr, false, nullptr, {}, nullptr, with_scan ? &hints : nullptr);
            });
            EXPECT_TRUE(out_runs.empty());
            EXPECT_FALSE((*op).read(layout.blobTargetRunKey(2, 0, 0, 0), Retry::once()).has_value()) << "a successor run was written";
        }
    }
}
```

- [ ] **Step 2: Build to see the tests fail to compile**

Build (log `build/build_cas30_t5_red.log`).
Expected: `NINJA_EXIT=1`; errors in `gtest_cas_blob_indegree.cpp`: unknown type `GraduationReadHints`, undeclared
`kGraduationLookaheadRows`, no matching `foldDeltasIntoGeneration` for the extra argument. No other error.

- [ ] **Step 3: Add the interface**

`CA/Gc/CasBlobInDegree.h`, after `decodeCondemnedRow`:

```cpp
/// Look-ahead for the graduation gate's marker read, supplied by the GC round. The gate still decides on the
/// value it takes; only the moment of the fetch moves.
struct GraduationReadHints
{
    /// Reserves a budget slot for the entry at the gate and hints its read if the window has room. False: no
    /// budget left; the entry is carried unread. Idempotent for an entry that already holds a slot.
    std::function<bool(const BlobRef &)> admit;
    /// Reserves and hints a later condemned row, or skips a row that will not reach the gate. False: the window
    /// or the budget is full; the scan stops and offers the row again later.
    std::function<bool(const BlobRef &, const CondemnedRow &)> offer;
};

/// Rows the merge decodes ahead of its position to find graduation candidates. Bounds memory, not requests.
inline constexpr size_t kGraduationLookaheadRows = 16384;
```

Append to the `foldDeltasIntoGeneration` comment:

```cpp
/// `graduation_reads`, when set, admits each graduation candidate against the round's budget before the gate and
/// offers later condemned rows of the open run segment so their marker reads can start early. Null reads every
/// admitted marker at the gate.
```

and the parameter (after `work_budget`):

```cpp
                              GcRoundWorkBudget * work_budget = nullptr,
                              const GraduationReadHints * graduation_reads = nullptr);
```

- [ ] **Step 4: Implement `PriorEdgeCursor::scanAhead` and wire `settleEntry`**

`CA/Gc/CasBlobInDegree.cpp`: add `#include <deque>`. Replace the body of `class PriorEdgeCursor` from
`public:` to the closing `};` (keep the class comment above it):

```cpp
public:
    /// The key codec is stateless and self-describing, so a run may freely mix supported hash algorithms.
    PriorEdgeCursor(CasOperation & op_, const std::vector<RunRef> & segments_)
        : op(op_), segments(segments_)
    {
        advance();
    }

    bool valid() const { return has_current; }
    const String & key() const { return current.key; }
    /// The value byte of the current row: `RunMarker::Edge` (a surviving edge) or `RunMarker::Condemned` (a retired
    /// sentinel row). Zero markers are never surfaced (dropped on carry).
    RunMarker rowType() const { return current.type; }
    /// The decoded retired sentinel for the current row (only valid when `rowType() == RunMarker::Condemned`).
    const CondemnedRow & condemnedRow() const { return current.condemned; }

    /// Advance to the next surviving edge OR retired sentinel, dropping zero markers, enforcing the
    /// row/key invariants, and crossing segment boundaries.
    void advance()
    {
        if (!lookahead.empty())
        {
            current = std::move(lookahead.front());
            lookahead.pop_front();
            current_offered = offered_ahead > 0;
            if (offered_ahead > 0)
                --offered_ahead;
            has_current = true;
            return;
        }
        current_offered = false;
        while (true)
        {
            if (reader)
            {
                if (auto row = pullRow())
                {
                    current = std::move(*row);
                    has_current = true;
                    return;
                }
                /// Segment fully drained: verify its whole-file checksum against
                /// the seal's RunRef.checksum BEFORE the fold acts on any of its rows. This is the fold —
                /// the checksum's most important consumer (it decides deletions). Fail-closed on mismatch.
                reader->verifyAgainst(segments[seg_idx].checksum);
                reader.reset();
                ++seg_idx;
            }

            /// Open the next resolved segment; the segment list is exhausted => the chain is done.
            if (seg_idx >= segments.size())
            {
                has_current = false;
                return;
            }
            /// Typed open validates the NDJSON header before any row is consumed. Each row carries its
            /// own algorithm byte, so no separate width gate is needed.
            reader = openSourceEdgeRun(op, segments[seg_idx].key);
            reader_drained = false;
        }
    }

    /// Offers each condemned row after the current position exactly once, in order: the current row, then rows
    /// of the OPEN segment decoded into a buffer of at most `max_rows`. Stops when `offer` returns false (that
    /// row is offered again next time), the buffer is full, or the open segment ends: opening the next segment,
    /// and verifying the checksum of the one that ends, stay with `advance`. Rows are validated as `advance`
    /// validates them, so a corrupt row throws here a few rows early.
    void scanAhead(size_t max_rows, const std::function<bool(const BlobRef &, const CondemnedRow &)> & offer)
    {
        if (has_current && !current_offered)
        {
            if (!offerRow(current, offer))
                return;
            current_offered = true;
        }
        while (offered_ahead < lookahead.size())
        {
            if (!offerRow(lookahead[offered_ahead], offer))
                return;
            ++offered_ahead;
        }
        while (reader && lookahead.size() < max_rows)
        {
            auto row = pullRow();
            if (!row)
                return;
            lookahead.push_back(std::move(*row));
            if (!offerRow(lookahead.back(), offer))
                return;
            ++offered_ahead;
        }
    }

private:
    struct Row
    {
        String key;
        RunMarker type = RunMarker::Edge;
        CondemnedRow condemned;
    };

    static bool offerRow(const Row & row, const std::function<bool(const BlobRef &, const CondemnedRow &)> & offer)
    {
        if (row.type != RunMarker::Condemned)
            return true;
        BlobRef ref;
        UInt128 source_id;
        SourceEdgeKeyCodec::parse(row.key, ref, source_id);
        return offer(ref, row.condemned);
    }

    /// The next surviving edge or retired sentinel of the open segment, validated; nullopt once it is drained.
    std::optional<Row> pullRow()
    {
        if (reader_drained)
            return std::nullopt;
        String k;
        String p;
        while (reader->next(k, p))
        {
            BlobRef bh;
            UInt128 sid;
            /// `parse` throws CORRUPTED_DATA on a malformed size / NOT_IMPLEMENTED on an
            /// unknown algo byte (fail-closed).
            SourceEdgeKeyCodec::parse(k, bh, sid);
            if (p.empty())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: empty row payload");
            const RunMarker v = runMarkerFromByte(p[0], "CAS source-edge run");
            const bool sentinel_key = (sid == kZeroSourceId);

            if (sentinel_key)
            {
                /// A sentinel key carries exactly one row per blob and never an edge.
                if (v == RunMarker::Edge)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS source-edge run: active edge at the reserved sentinel source_id 0");
                if (have_sentinel_blob && sentinel_blob == bh)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS source-edge run: duplicate sentinel row for one blob");
                have_sentinel_blob = true;
                sentinel_blob = bh;
                if (v == RunMarker::Zero)
                    continue;   // A zero marker is per-generation and is dropped on carry.
                /// A retired sentinel: decode and surface it (settled at close-out, not an edge).
                return Row{.key = k, .type = RunMarker::Condemned, .condemned = decodeCondemnedRow(p)};
            }

            /// A non-sentinel key must carry a surviving edge and nothing else.
            if (v != RunMarker::Edge)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS source-edge run: sentinel row type 0x{:02x} at a non-sentinel key",
                    static_cast<uint8_t>(v));
            return Row{.key = k, .type = RunMarker::Edge, .condemned = {}};
        }
        reader_drained = true;
        return std::nullopt;
    }

    CasOperation & op;
    const std::vector<RunRef> & segments;

    size_t seg_idx = 0;
    std::optional<SourceEdgeRunView> reader;
    /// The open segment returned its last row; `advance` verifies it and opens the next.
    bool reader_drained = false;
    Row current;
    bool has_current = false;
    bool current_offered = false;
    /// Rows decoded ahead of `current`; the first `offered_ahead` of them were offered.
    std::deque<Row> lookahead;
    size_t offered_ahead = 0;

    /// Duplicate-sentinel guard: the last blob for which a sentinel row was seen (across skipped zero
    /// markers too). Rows are globally sorted, so two sentinels for one blob are adjacent.
    bool have_sentinel_blob = false;
    BlobRef sentinel_blob{};
};
```

The definition of `foldDeltasIntoGeneration` gains the parameter `const GraduationReadHints * graduation_reads`
after `GcRoundWorkBudget * work_budget`. Replace the graduation branch of `settleEntry` (Task 2's version) with:

```cpp
        else if (!suppress_destructive && e.condemn_round < current_round)
        {
            /// Budget before the gate: a carried entry must not cost a marker read every round. A refusal
            /// consumes no slot, so refusals cannot crowd out graduations.
            const bool admitted = graduation_reads ? graduation_reads->admit(e.ref)
                                                   : (!work_budget || work_budget->graduationAvailable());
            if (!admitted)
                rmr.still_retired.push_back(e);
            else
            {
                /// After the admission: a look-ahead hint must never take the settling entry's slot.
                if (graduation_reads)
                    cursor.scanAhead(kGraduationLookaheadRows, graduation_reads->offer);
                if (!confirm_condemned_marker || confirm_condemned_marker(e))
                {
                    if (work_budget)
                        ++work_budget->graduations_used;
                    RetiredEntry pending = e;       /// newly floor-passed: publish pending; delete NEXT pass
                    pending.delete_pending = true;
                    pending.marker_confirmed = true;
                    rmr.graduated.push_back(pending);
                    rmr.still_retired.push_back(std::move(pending));
                }
                else
                    rmr.still_retired.push_back(e); /// the gate refused: carried; it has rewritten the marker
            }
        }
```

- [ ] **Step 5: Build and run the merge suites, then the gate**

Build (log `build/build_cas30_t5.log`); run `--gtest_filter='CASThreeCursorMerge.*:CASTwoCursorMerge.*:CASBlobInDegree.*'`
(log `build/test_cas30_t5.log`) and the gate (log `build/test_cas30_t5_gate.log`). Expected `GATE_EXIT=0` both.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git status --short
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp \
  src/Disks/tests/gtest_cas_blob_indegree.cpp
git commit -s -m "Let the CAS fold merge offer later condemned rows to a read-ahead" \
  -m "PriorEdgeCursor decodes rows of the open run segment into a bounded buffer and offers each condemned row once. The merge admits a graduation candidate through the hints and scans ahead before the gate. The segment checksum is still verified when the segment drains."
```

---

### Task 6: The fold prefetches graduation reads in their own window

**Files:**
- Modify: `CA/Pool/CasBlobMeta.h:38` (add `decodeLoadedMeta`), `CA/Pool/CasBlobMeta.cpp:15-22`
- Modify: `CA/Gc/CasGcReadAhead.h:24-32` (header sentence), `:63` (add `pendingHeads`)
- Modify: `CA/Gc/CasGc.cpp` in `Gc::fold`: `topUpHeadHints` (line ~1986), before `confirm_condemned_marker` (declare `graduation_reserved`), `confirm_condemned_marker`, the `last_verdict_is_remove` block (line ~3454), both `foldDeltasIntoGeneration` calls (lines ~3486 and ~3530)
- Test: `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`

**Interfaces:**
- Consumes: Task 5's `GraduationReadHints`, `kGraduationLookaheadRows`, `foldDeltasIntoGeneration(..., graduation_reads)`.
- Produces: `std::optional<LoadedMeta> decodeLoadedMeta(std::optional<Object> object);`
  `size_t GcReadAhead::pendingHeads() const;`

- [ ] **Step 1: Write the tests**

Append to `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`:

```cpp
namespace
{

/// Parks the first GET of a watched key until `quorum` more GETs of watched keys arrive, or a bounded wait ends.
/// Serial reads never release it; overlapping ones do.
class ParkFirstWatchedReadBackend : public CountingBackend
{
public:
    void watch(std::set<String> keys, size_t quorum_)
    {
        std::lock_guard lock(park_mutex);
        watched = std::move(keys);
        quorum = quorum_;
        armed = true;
    }

    bool timedOut() const
    {
        std::lock_guard lock(park_mutex);
        return timed_out;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        {
            std::unique_lock lock(park_mutex);
            if (armed && watched.contains(key))
            {
                if (!parked)
                {
                    parked = true;
                    if (!arrived.wait_for(lock, std::chrono::seconds(10), [&] { return arrivals >= quorum; }))
                        timed_out = true;
                    armed = false;
                }
                else
                {
                    ++arrivals;
                    arrived.notify_all();
                }
            }
        }
        return CountingBackend::read(key, access);
    }

private:
    mutable std::mutex park_mutex;
    std::condition_variable arrived;
    std::set<String> watched;
    size_t quorum = 0;
    size_t arrivals = 0;
    bool armed = false;
    bool parked = false;
    bool timed_out = false;
};

std::vector<UInt128> hashRange(uint64_t base, uint64_t count)
{
    std::vector<UInt128> out;
    for (uint64_t i = 1; i <= count; ++i)
        out.push_back(DB::UInt128(base + i));
    return out;
}

/// `.meta` GETs per key over `hashes`.
std::map<String, uint64_t> markerGets(const CountingBackend & backend, const Layout & layout, const std::vector<UInt128> & hashes)
{
    std::map<String, uint64_t> out;
    for (const UInt128 & hash : hashes)
        out[layout.blobMetaKey(gateRef(hash))] = backend.getCount(layout.blobMetaKey(gateRef(hash)));
    return out;
}

struct SupersedeRun
{
    std::vector<uint64_t> graduated;
    std::map<String, uint64_t> superseded_gets;
    uint64_t wasted = 0;
};

/// Twenty condemned blobs; before each of three rounds a writer republishes ten of them under a new token and
/// drops them again, so those ten are superseded every round.
void runSupersedeRounds(uint64_t concurrency, SupersedeRun & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend, concurrency);
    const Layout & layout = store->layout();
    const std::vector<UInt128> cohort = hashRange(0x6000, 20);
    const std::vector<UInt128> superseded(cohort.begin(), cohort.begin() + 10);
    Gc gc(store, kGc);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));

    backend->resetCounts();
    const uint64_t wasted_before = readAheadWasted();
    for (uint64_t round = 0; round < 3; ++round)
    {
        for (const UInt128 & hash : superseded)
            displaceBlobToken(*backend, layout, gateRef(hash));
        const String ref_name = "touch" + std::to_string(round);
        const ManifestRef touch = publishBlobs(*backend, layout, ref_name, 10 + round, superseded);
        dropBlobs(*backend, layout, ref_name, touch);
        store->renewWatermarkOnce();
        out.graduated.push_back(runGateRound(store, gc).graduated);
    }
    out.wasted = readAheadWasted() - wasted_before;
    out.superseded_gets = markerGets(*backend, layout, superseded);
}

}

TEST(CASGCReadAhead, GraduationReadAheadSkipsSupersededEntries)
{
    SupersedeRun sequential;
    SupersedeRun ahead;
    ASSERT_NO_FATAL_FAILURE(runSupersedeRounds(1, sequential));
    ASSERT_NO_FATAL_FAILURE(runSupersedeRounds(8, ahead));

    EXPECT_EQ(ahead.superseded_gets, sequential.superseded_gets) << "a superseded entry's marker was read ahead";
    EXPECT_EQ(ahead.wasted, 0u);
    EXPECT_EQ(ahead.graduated, sequential.graduated);
    EXPECT_EQ(sequential.graduated.front(), 10u);
}

TEST(CASGCReadAhead, GraduationReadAheadIgnoresUnusedHeadHints)
{
    auto backend = std::make_shared<ParkFirstWatchedReadBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 8);
    const Layout & layout = store->layout();

    /// Roles by key order: the smallest blob is condemned fresh in the measured round, so its HEAD tops the head
    /// hints up with `kept` (which keep an edge, so nobody takes them) before any graduation candidate settles.
    std::vector<BlobRef> refs;
    for (const UInt128 & hash : hashRange(0x7000, 61))
        refs.push_back(gateRef(hash));
    std::sort(refs.begin(), refs.end());
    const auto hashesOf = [&](size_t from, size_t to)
    {
        std::vector<UInt128> out;
        for (size_t i = from; i < to; ++i)
            out.push_back(refs[i].digest.toU128());
        return out;
    };
    const std::vector<UInt128> fresh = hashesOf(0, 1);
    const std::vector<UInt128> graduating = hashesOf(1, 21);
    const std::vector<UInt128> kept = hashesOf(21, 61);

    const ManifestRef kept_a = publishBlobs(*backend, layout, "kept_a", 1, kept);
    publishBlobs(*backend, layout, "kept_b", 2, kept);
    {
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "graduating", 3, graduating, c));
    }
    const ManifestRef fresh_manifest = publishBlobs(*backend, layout, "fresh", 4, fresh);
    dropBlobs(*backend, layout, "fresh", fresh_manifest);
    dropBlobs(*backend, layout, "kept_a", kept_a);
    store->renewWatermarkOnce();

    std::set<String> graduation_keys;
    for (const UInt128 & hash : graduating)
        graduation_keys.insert(layout.blobMetaKey(gateRef(hash)));
    backend->watch(graduation_keys, /*quorum*/ 8);

    Gc gc(store, kGc);
    const uint64_t wasted_before = readAheadWasted();
    const RoundReport report = runGateRound(store, gc);
    EXPECT_FALSE(backend->timedOut()) << "the graduation reads never overlapped";
    EXPECT_EQ(report.graduated, 20u);
    EXPECT_EQ(report.condemned, 1u);
    EXPECT_GE(readAheadWasted() - wasted_before, 31u) << "the fixture must leave the head window full of untaken hints";
}

TEST(CASGCReadAhead, GraduationReadAheadAcrossShards)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 10, {}, /*gc_shards*/ 4);
        const Layout & layout = store->layout();
        const std::vector<UInt128> cohort = hashRange(0x8000, 40);
        std::set<uint64_t> shards;
        for (const UInt128 & hash : cohort)
            shards.insert(blobShard(gateRef(hash), 4));
        ASSERT_GT(shards.size(), 1u) << "the cohort must span shards";
        {
            Gc gc(store, kGc);
            uint64_t c = 0;
            ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));
        }

        Gc gc(store, kGc);
        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        EXPECT_EQ(runGateRound(store, gc).graduated, 10u);
        const auto by_key = markerGets(*backend, layout, cohort);
        uint64_t total = 0;
        for (const auto & [key, n] : by_key)
            total += n;
        EXPECT_EQ(total, 10u) << "the shards together read past the shared budget";
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u);
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}

TEST(CASGCReadAhead, GraduationReadAheadReadsALargeCohortOnce)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const std::vector<UInt128> cohort = hashRange(0x8800, 100);
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));

        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        EXPECT_EQ(runGateRound(store, gc).graduated, 100u);
        const auto by_key = markerGets(*backend, layout, cohort);
        for (const auto & [key, n] : by_key)
            EXPECT_EQ(n, 1u) << key;
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u);
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}

TEST(CASGCReadAhead, GraduationReadAheadNeverHintsPendingRows)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const std::vector<UInt128> pending = hashRange(0x9100, 10);
        const std::vector<UInt128> eligible = hashRange(0x9200, 10);
        const ManifestRef eligible_manifest = publishBlobs(*backend, layout, "eligible", 1, eligible);
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "pending", 2, pending, c));
        dropBlobs(*backend, layout, "eligible", eligible_manifest);
        store->renewWatermarkOnce();
        const RoundReport setup = runGateRound(store, gc);
        ASSERT_EQ(setup.graduated, 10u);
        ASSERT_EQ(setup.condemned, 10u);

        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        const RoundReport report = runGateRound(store, gc);
        EXPECT_EQ(report.redeleted, 10u);
        EXPECT_EQ(report.graduated, 10u);
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u) << "a pending row was hinted and never taken";
        auto by_key = markerGets(*backend, layout, pending);
        by_key.merge(markerGets(*backend, layout, eligible));
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}
```

- [ ] **Step 2: Build and run to see RA5 fail**

Build (log `build/build_cas30_t6_red.log`); run `--gtest_filter='CASGCReadAhead.GraduationReadAhead*'`
(log `build/test_cas30_t6_red.log`; RA5 takes about 10 s here).
Expected: `GraduationReadAheadIgnoresUnusedHeadHints` FAILED with "the graduation reads never overlapped".
Every other `GraduationReadAhead*` test passes (guards: without hints there is nothing to waste or misplace).

- [ ] **Step 3: `decodeLoadedMeta` and `pendingHeads`**

`CA/Pool/CasBlobMeta.h`, after the `loadMeta` declaration:

```cpp
/// The decode half of `loadMeta`, for a read issued elsewhere (the GC read-ahead). Decoding errors propagate.
std::optional<LoadedMeta> decodeLoadedMeta(std::optional<Object> object);
```

`CA/Pool/CasBlobMeta.cpp`, replace `loadMeta`:

```cpp
std::optional<LoadedMeta> decodeLoadedMeta(std::optional<Object> object)
{
    if (!object)
        return std::nullopt;
    return LoadedMeta{.meta = decodeBlobMeta(object->bytes), .etag = std::move(object->etag)};
}

std::optional<LoadedMeta> loadMeta(CasOperation & op, const Layout & layout, const BlobRef & ref,
                                   const Retry & policy)
{
    return decodeLoadedMeta(op.read(layout.blobMetaKey(ref), policy));
}
```

`CA/Gc/CasGcReadAhead.h`: after `pending()` add

```cpp
    /// Hinted HEADs not yet taken: the head hinting site throttles on its own count.
    size_t pendingHeads() const { return heads.size(); }
```

and append to the "Why a result may be fetched early" paragraph (after "...counted wasted."):

```cpp
/// A blob marker is not write-once: the fold hints it only after the round's cut, which is all the
/// graduation gate needs.
```

- [ ] **Step 4: Wire the hints in `Gc::fold`**

`CA/Gc/CasGc.cpp`:

`topUpHeadHints`:

```cpp
    /// Head hints throttle on their own count: an untaken one (a candidate that keeps an edge) stays pending
    /// until the fold ends, and must not crowd out the graduation reads, which keep their own window.
    const auto topUpHeadHints = [&]
    {
        const std::vector<BlobRef> & shard_candidates = head_candidates[head_hint_shard];
        while (next_head_hint < shard_candidates.size() && reads.pendingHeads() < reads.window())
            reads.hintHead(layout.blobKey(shard_candidates[next_head_hint++]));
    };
```

Immediately before the gate's comment, declare:

```cpp
    /// Marker keys admitted to a graduation read and not yet taken: every outstanding hint and the entry at the
    /// gate. With `graduations_used` it never exceeds the graduation budget.
    std::unordered_set<String> graduation_reserved;
```

`confirm_condemned_marker` (Task 3's version) — the read goes through the read-ahead and releases the reservation:

```cpp
    const auto confirm_condemned_marker = [&](const RetiredEntry & entry) -> bool
    {
        const String key = layout.blobMetaKey(entry.ref);
        graduation_reserved.erase(key);
        try
        {
            /// An older round may be another incarnation's marker, which that incarnation's delete job may remove.
            if (const auto lm = decodeLoadedMeta(reads.takeRead(key));
                lm && lm->meta.state == MetaState::Condemned && lm->meta.condemn_round >= entry.condemn_round)
                return true;
        }
        catch (...)
        {
            ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);
            tryLogCurrentException(logger,
                "CAS gc: the graduation gate failed to read the condemn marker (the entry is carried; "
                "never wedges the round)");
        }
        ProfileEvents::increment(ProfileEvents::CASGCCondemnMarkerUnconfirmedCarry);
        /// This round, not the entry's: a publisher's late `Clean If-Match` names the bytes it read, and on S3
        /// an entry-round rewrite recreates exactly those bytes.
        meta_writer->scheduleCondemnMarkerWrite(entry.ref, condemn_round, entry.size);
        return false;
    };
```

In the `last_verdict_is_remove` block, hoist the touched set: declare `std::set<BlobRef> touched_blobs;` just
before the block's opening `{`, and replace the loop

```cpp
        for (const auto & [edge, is_remove] : last_verdict_is_remove)
            (is_remove ? removed : surviving_add).insert(edge.first);
```

with

```cpp
        for (const auto & [edge, is_remove] : last_verdict_is_remove)
        {
            touched_blobs.insert(edge.first);
            (is_remove ? removed : surviving_add).insert(edge.first);
        }
```

Right after that block (before `if (state.gc_shards == 1)`):

```cpp
    /// Graduation read-ahead. A carried row that no delta or retirement touches this round has no edge and is
    /// not superseded, so it reaches the gate; its read is hinted once the budget admits it. Hints are issued in
    /// the merge, after intake, and the fold writes no marker of a hinted hash before the gate takes the read.
    const auto graduationBudgetFull = [&]
    {
        return work_budget.max_graduations != 0
            && work_budget.graduations_used + graduation_reserved.size() >= work_budget.max_graduations;
    };
    const GraduationReadHints graduation_hints{
        .admit = [&](const BlobRef & ref)
        {
            const String key = layout.blobMetaKey(ref);
            if (graduation_reserved.contains(key))
                return true;
            if (graduationBudgetFull())
                return false;
            if (graduation_reserved.size() < reads.window())
                reads.hintRead(key);
            graduation_reserved.insert(key);
            return true;
        },
        .offer = [&](const BlobRef & ref, const CondemnedRow & row)
        {
            if (row.delete_pending || row.condemn_round >= current_round || suppress_destructive
                || touched_blobs.contains(ref))
                return true;
            if (graduationBudgetFull() || graduation_reserved.size() >= reads.window())
                return false;
            if (const String key = layout.blobMetaKey(ref); graduation_reserved.insert(key).second)
                reads.hintRead(key);
            return true;
        }};
    const GraduationReadHints * graduation_reads = reads.window() > 0 ? &graduation_hints : nullptr;
    /// A reservation left after a shard's merge belongs to no gate.
    const auto discardGraduationReads = [&]
    {
        for (const String & key : graduation_reserved)
            reads.discardRead(key);
        graduation_reserved.clear();
    };
```

Both `foldDeltasIntoGeneration` calls in `Gc::fold`: append `, graduation_reads` after `&work_budget`, and call
`discardGraduationReads();` on the line after each call.

- [ ] **Step 5: Build, run the read-ahead suite, then the gate**

Build (log `build/build_cas30_t6.log`); run `--gtest_filter='CASGCReadAhead.*:CASGcMetaWriter.*'`
(log `build/test_cas30_t6.log`); then the gate (log `build/test_cas30_t6_gate.log`). Expected `GATE_EXIT=0`
both; `FoldIsIdenticalAtConcurrencyOneAndEight` stays green (equal per-key GETs at 1 and 8).

- [ ] **Step 6: Re-check the spec's reading claims against the diff**

Run `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && git diff e2dd0f5d343 -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp | grep -n "scheduleCondemnMarkerWrite\|hintRead\|discardRead"`
and confirm by reading the surrounding code, writing one line each into the commit message body:
- no marker write of a hinted hash happens between hint and take (the only writes are `head_blob`, the supersede,
  and the gate's retry; the first two touch only `touched_blobs`, the third follows the take);
- an untouched carried condemned row has no edge (RA4, RA6–RA8 assert zero waste);
- `graduation_reserved` is empty when each shard's merge starts (it is cleared after each call).

- [ ] **Step 7: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git status --short
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/tests/gtest_cas_gc_read_ahead.cpp
git commit -s -m "Prefetch CAS graduation marker reads through the fold read-ahead"
```

(add the three Step 6 lines as the body with a second `-m`).

---

### Task 7: Model: new switches, invariants, required runs, promotion

Worktree: `/home/mfilimonov/workspace/ClickHouse/master`. TLC only through `tmp/cas-pub-marker/run.sh`
(8 workers, `nice -n 10`, `-Xmx8g`); one run at a time; long runs with `nohup` and a marker line.

**Files:**
- Modify: `tmp/cas-pub-marker/CaGcCondemnMarkerGate.tla`, `tmp/cas-pub-marker/mkcfg.sh`, `tmp/cas-pub-marker/run.sh`, every `tmp/cas-pub-marker/CaGcCondemnMarkerGate_*.cfg` (four constant lines appended)
- Create: the PR1 cfgs in `tmp/cas-pub-marker/` listed in Step 5
- Modify (promotion): `docs/superpowers/models/CaGcCondemnMarkerGate.tla`, `docs/superpowers/models/CaGcCondemnMarkerGate_{bug,bug_both,bug_both_2w,fix,fix_pconf,fix_prefetch,fix_both,fix_both_2w}.cfg`, `docs/superpowers/models/run_condemnmarker.sh`, `docs/superpowers/models/CaGcCondemnMarkerGate_RESULTS.md`
- Create (promotion): `docs/superpowers/models/CaGcCondemnMarkerGate_<row>.cfg` for every Step 5 row

**Interfaces:**
- Produces the constants `RetryStampsCurrent`, `OutstandingReconcile`, `EtagIsContent`, `TrackRetry`; the variables
  `late`, `late_moved`; `mjob` as a record; `ret.retried`; invariants `JobSparesPendingGuard`, `NoWedgedCarry`.
  Defaults (`FALSE`, `"none"`, `TRUE`, `FALSE`) are inert: every existing cfg keeps its distinct count.

- [ ] **Step 1: Snapshot the module**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/tmp/cas-pub-marker && cp CaGcCondemnMarkerGate.tla CaGcCondemnMarkerGate.v2.tla`

- [ ] **Step 2: Edit the module**

`tmp/cas-pub-marker/CaGcCondemnMarkerGate.tla`:

1. Header comment, after the `SuppressRounds` lines add:

```
(*   RetryStampsCurrent the gate's retry write stamps the refusing round, not the      *)
(*                      entry's.                                                       *)
(*   OutstandingReconcile "none" | "resolved" | "gaveup": a reconcile may leave one    *)
(*                      Clean If-Match request in flight after its task ends.          *)
(*   EtagIsContent      a late request matches equal bytes (S3); FALSE: it matches     *)
(*                      only while no write replaced the version it read (GCS).        *)
(*   TrackRetry         records a completed retry write for NoWedgedCarry.             *)
```

2. `CONSTANTS`: append `, RetryStampsCurrent, OutstandingReconcile, EtagIsContent, TrackRetry` to the last line.

3. Replace `NoRet` and add the new constants' values:

```
NoRet == [st |-> "none", tok |-> NoTok, cr |-> 0, conf |-> FALSE, pconf |-> FALSE, retried |-> FALSE]
NoJob == [round |-> 0, retry |-> FALSE]
NoLate == [pending |-> FALSE, expect |-> Absent]
```

(`NoLate` uses `Absent`, so place it after `Absent`.)

4. `VARIABLES`: change `mjob` comment to `\* the queued condemn-marker job: the round it stamps, and whether it is a gate retry`
   and append after `sa_done`:

```
  , late       \* a publisher's Clean If-Match request still in flight: [pending, expect]
  , late_moved \* the marker changed since that request read it
```

   and append `late, late_moved` to `vars`.

5. `TypeOK`: replace `mjob \in BOOLEAN` in the conjunct `dirty \in BOOLEAN /\ touched \in BOOLEAN /\ mjob \in BOOLEAN` with
   `mjob \in [round : 0..MaxRounds, retry : BOOLEAN]`, and add

```
  /\ late \in [pending : BOOLEAN, expect : MetaVals]
  /\ late_moved \in BOOLEAN
```

6. `Init`: `mjob = NoJob`; add `/\ late = NoLate /\ late_moved = FALSE`.

7. Add after `Touch`:

```
RetryStamp == IF RetryStampsCurrent THEN round ELSE ret.cr
LateStart == OutstandingReconcile /= "none" /\ ~late.pending /\ Cond
```

8. `WReconcile(w)` becomes:

```
WReconcile(w) ==
  /\ recon[w]
  /\ recon' = [recon EXCEPT ![w] = FALSE]
  /\ \/ /\ meta' = ReconcileResult(w)
        /\ adopted' = adopted
        /\ late' = late
     \/ /\ OutstandingReconcile = "resolved" /\ LateStart
        /\ meta' = ReconcileResult(w)
        /\ adopted' = adopted
        /\ late' = [pending |-> TRUE, expect |-> meta]
     \/ /\ ReconcileFault
        /\ meta' = meta
        /\ adopted' = [adopted EXCEPT ![w] = NoTok]
        /\ late' = late
     \/ /\ OutstandingReconcile = "gaveup" /\ ReconcileFault /\ LateStart
        /\ meta' = meta
        /\ adopted' = [adopted EXCEPT ![w] = NoTok]
        /\ late' = [pending |-> TRUE, expect |-> meta]
  /\ UNCHANGED <<round, cut_taken, folded, body, next_gen, seeded, edge, committed, ret, observed, observed_tok, mdq,
                 dirty, touched, premeta, mjob, wit, reg, sa_done>>
```

9. Add after `WReconcile`:

```
(* The request a reconcile left in flight lands, or dies. It writes Clean only if its precondition still
   matches: equal bytes under EtagIsContent, otherwise an unreplaced version. *)
WLateLand ==
  /\ late.pending
  /\ IF EtagIsContent THEN meta = late.expect ELSE ~late_moved
  /\ meta' = CleanMeta
  /\ late' = NoLate
  /\ UNCHANGED <<round, cut_taken, folded, body, next_gen, seeded, edge, adopted, committed, ret, observed,
                 observed_tok, mdq, recon, dirty, touched, premeta, mjob, wit, reg, sa_done>>

WLateDie ==
  /\ late.pending
  /\ late' = NoLate
  /\ UNCHANGED <<round, cut_taken, folded, body, meta, next_gen, seeded, edge, adopted, committed, ret, observed,
                 observed_tok, mdq, recon, dirty, touched, premeta, mjob, wit, reg, sa_done>>
```

10. `GCut`: `~mjob` → `mjob = NoJob`. `GRestart`: `~mjob` → `mjob = NoJob`.

11. `Condemn(t, r)`: in both `ret'` records add `, retried |-> FALSE`; the async branch sets
    `mjob' = [round |-> r, retry |-> FALSE]` instead of `mjob' = TRUE`.

12. `GMarkerJob`:

```
GMarkerJob ==
  /\ mjob /= NoJob
  /\ mjob' = NoJob
  /\ \/ /\ meta' = CondemnedAfterWrite(mjob.round)
        /\ ret' = [ret EXCEPT !.conf = TRUE, !.pconf = PersistConf, !.retried = ret.retried \/ mjob.retry]
        /\ reg' = RegAdd(ret.tok, ret.cr)
     \/ /\ meta' = meta
        /\ ret' = ret
        /\ reg' = reg
  /\ UNCHANGED <<round, cut_taken, folded, body, next_gen, seeded, edge, adopted, committed, observed, observed_tok,
                 mdq, recon, dirty, touched, premeta, wit, sa_done>>
```

13. `GSettleCarryRetry`, the `IF AsyncMarker` part:

```
  /\ IF AsyncMarker
       THEN /\ meta' = meta /\ ret' = ret /\ mjob' = [round |-> RetryStamp, retry |-> TrackRetry] /\ reg' = reg
       ELSE /\ mjob' = mjob
            /\ \/ /\ meta' = CondemnedAfterWrite(RetryStamp)
                  /\ ret' = [ret EXCEPT !.conf = TRUE, !.pconf = PersistConf, !.retried = ret.retried \/ TrackRetry]
                  /\ reg' = RegAdd(ret.tok, ret.cr)
               \/ /\ meta' = meta
                  /\ ret' = ret
                  /\ reg' = reg
```

14. Replace `Next`:

```
OtherNext ==
  \/ Seed
  \/ \E w \in Writers : WPrecommit(w) \/ WAdopt(w) \/ WPublish(w) \/ WCommit(w) \/ WDrop(w) \/ WRespawn(w)
  \/ GCut \/ GObserve \/ GMarkerJob
  \/ GSettleCondemn \/ GSettleIdle \/ GSettleSpare
  \/ GSettleGraduate \/ GSettleGraduateDeferred \/ GSettleSupersede \/ GSettleCarryRetry
  \/ GSettleSpareLoud \/ GSettleRedelete \/ GSettleSuppressed
  \/ \E c \in mdq : GMetaDeleteJob(c)
  \/ \E r \in 1..round : GStaleMarkerWrite(r)
  \/ GRestart
  \/ \E r \in 1..round : GStaleAttempt(r)

(* Every step that changes the marker moves the version a late request read. *)
LateMovedTrack ==
  late_moved' = IF ~late'.pending THEN FALSE
                ELSE IF ~late.pending THEN meta' /= late'.expect
                ELSE late_moved \/ meta' /= meta

Next ==
  /\ \/ (OtherNext /\ late' = late)
     \/ \E w \in Writers : WReconcile(w)
     \/ WLateLand \/ WLateDie
  /\ LateMovedTrack
```

    `WReconcile(w)` leaves the writer disjunction of `OtherNext`: it assigns `late'` itself.

15. Invariants, after `MarkerPreservedConfirmed`:

```
(* No queued job can remove the guard of a pending entry whose token is current. *)
JobWouldDelete(c) == ~JobRoundCheck \/ (Cond /\ meta.cr <= c)
JobSparesPendingGuard == \A c \in mdq : ~(ret.st = "pending" /\ TokCurrent /\ JobWouldDelete(c))

(* After a retry write that completed, the marker is not an older round than the entry's. *)
NoWedgedCarry == ~(ret.st = "condemned" /\ ret.retried /\ Cond /\ meta.cr < ret.cr)
```

Parse check: `cd /home/mfilimonov/workspace/ClickHouse/master/tmp/cas-pub-marker && nice -n 10 java -cp ../tla2tools.jar tla2sany.SANY CaGcCondemnMarkerGate.tla > logs/sany.log 2>&1; echo SANY_EXIT=$? >> logs/sany.log`
Expected: `SANY_EXIT=0`, no "Semantic errors".

- [ ] **Step 3: Give every existing cfg the new constants at their defaults**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/cas-pub-marker
python3 - <<'EOF'
import glob
extra = ['  RetryStampsCurrent = FALSE\n', '  OutstandingReconcile = "none"\n', '  EtagIsContent = TRUE\n', '  TrackRetry = FALSE\n']
for path in sorted(glob.glob('CaGcCondemnMarkerGate_*.cfg')):
    lines = open(path).readlines()
    if any('RetryStampsCurrent' in l for l in lines):
        continue
    at = next(i for i, l in enumerate(lines) if l.startswith('INVARIANT'))
    open(path, 'w').writelines(lines[:at] + extra + lines[at:])
    print('updated', path)
EOF
```

`mkcfg.sh`: add `[RetryStampsCurrent]=FALSE [OutstandingReconcile]='"none"' [EtagIsContent]=TRUE [TrackRetry]=FALSE`
to the defaults and `RetryStampsCurrent OutstandingReconcile EtagIsContent TrackRetry` to the end of `order`.
`run.sh`: replace `timeout 1200` with `timeout "${TLC_TIMEOUT:-1200}"`.

- [ ] **Step 4: Regression battery (inert defaults)**

Run (background, `nohup`, about 1.5 h):

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/cas-pub-marker
sed -e 's/final2_/final3_/g' -e 's/battery2.done/battery3.done/' battery2.sh > battery3.sh && chmod +x battery3.sh
nohup ./battery3.sh > logs/battery3.out 2>&1 &
```

Wait for `battery3.done`. Then compare per row:
`for f in regress neg pos; do join <(awk 'NR>1{print $1, $2, $3, $4}' final2_$f.txt | sort) <(awk 'NR>1{print $1, $2, $3, $4}' final3_$f.txt | sort); done`
Expected: every row has the same RESULT; every green row has the same DISTINCT. A difference means a new
constant is not inert: fix the module, not the expectation. (`pm_g1_big` timed out before; it may time out again.)

- [ ] **Step 5: Generate the PR1 cfgs**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/cas-pub-marker
PR1='GateAlwaysReads=TRUE Registry=FALSE RegRound=FALSE RetryStampsCurrent=TRUE'
ALL='AsyncMarker=TRUE Prefetch=TRUE ReconcileFault=TRUE StaleMarkers=TRUE StaleAttempts=TRUE SuppressRounds=TRUE'
INV='TypeOK NoDangle MarkerPreservedPending MarkerPreservedConfirmed JobSparesPendingGuard NoWedgedCarry'
./mkcfg.sh pm_g1_noregistry "$PR1" $INV
./mkcfg.sh pm_g1_all_noregistry "$PR1 $ALL PersistConf=TRUE" $INV
./mkcfg.sh rv_g1_noregistry "$PR1 AsyncMarker=TRUE Prefetch=TRUE ReconcileFault=TRUE StaleMarkers=TRUE StaleAttempts=TRUE PersistConf=TRUE ExactAbsentPath=TRUE" $INV
./mkcfg.sh pm_g1_f1 "$PR1 $ALL PersistConf=TRUE SplitPublish=FALSE" $INV
./mkcfg.sh pm_g1_live "$PR1 TrackRetry=TRUE AsyncMarker=TRUE ReconcileFault=TRUE SuppressRounds=TRUE" $INV
./mkcfg.sh pm_g1_late_resolved "$PR1 OutstandingReconcile=\"resolved\" AsyncMarker=TRUE" $INV
./mkcfg.sh pm_g1_late_resolved_entry "$PR1 OutstandingReconcile=\"resolved\" AsyncMarker=TRUE RetryStampsCurrent=FALSE" TypeOK NoDangle
./mkcfg.sh pm_g1_late_resolved_version "$PR1 OutstandingReconcile=\"resolved\" AsyncMarker=TRUE RetryStampsCurrent=FALSE EtagIsContent=FALSE StaleMarkers=TRUE StaleAttempts=TRUE" $INV
./mkcfg.sh pm_g1_late_resolved_stale "$PR1 OutstandingReconcile=\"resolved\" AsyncMarker=TRUE StaleMarkers=TRUE StaleAttempts=TRUE" TypeOK NoDangle
./mkcfg.sh pm_g1_late_gaveup "$PR1 OutstandingReconcile=\"gaveup\" ReconcileFault=TRUE" TypeOK NoDangle
./mkcfg.sh pm_g1_nogateround_noregistry "$PR1 $ALL PersistConf=TRUE GateRoundCheck=FALSE" TypeOK NoDangle
./mkcfg.sh pm_g1_nojob "$PR1 JobRoundCheck=FALSE StaleMarkers=TRUE" TypeOK JobSparesPendingGuard NoDangle
./mkcfg.sh pm_g1_norestamp_live "$PR1 TrackRetry=TRUE AsyncMarker=TRUE ReconcileFault=TRUE SuppressRounds=TRUE Restamp=FALSE" TypeOK NoWedgedCarry
./mkcfg.sh pm_g1_extdrop_noregistry "$PR1 ExternalDrop=TRUE" TypeOK NoDangle
./mkcfg.sh pm_g1_big_noregistry "$PR1 $ALL PersistConf=TRUE MaxGen=4 MaxRounds=8" $INV
grep -c "OutstandingReconcile = \"resolved\"" CaGcCondemnMarkerGate_pm_g1_late_resolved.cfg
```

Expected: the last command prints `1` (the quotes survive). The existing `pm_g1_nogateround`, `pm_g1_extdrop` and
`pm_g1_big` are `Registry TRUE` evidence; they stay as regression rows, so the PR1 rows carry `_noregistry`.

- [ ] **Step 6: Run the required rows**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/cas-pub-marker
nohup sh -c './run.sh pm_g1_noregistry:green pm_g1_all_noregistry:green rv_g1_noregistry:green pm_g1_f1:green \
  pm_g1_live:green pm_g1_late_resolved:green pm_g1_late_resolved_entry:violation pm_g1_late_resolved_version:green \
  pm_g1_late_resolved_stale:violation pm_g1_late_gaveup:violation pm_g1_nogateround_noregistry:violation \
  pm_g1_nojob:violation:JobSparesPendingGuard pm_g1_norestamp_live:violation:NoWedgedCarry \
  pm_g1_extdrop_noregistry:violation rv_bug:violation f1_bug:violation; echo RUN_EXIT=$?' > final3_pr1.txt 2>&1 &
```

Wait for the `RUN_EXIT=` line. Expected `RUN_EXIT=0`. `pm_g1_nojob` may violate `NoDangle` instead (the spec allows
either); if so, read the trace, confirm it is the stale-job schedule, change its row to `:violation:NoDangle` and
rerun that row only.

Then the non-required big run (2 h cap): `TLC_TIMEOUT=7200 nohup sh -c './run.sh pm_g1_big_noregistry:green; echo RUN_EXIT=$?' > final3_big.txt 2>&1 &`.
An unfinished run is reported with its distinct count.

- [ ] **Step 7: Read every violation trace**

For each violation row, open `logs/<row>.log` and write one line per row into `tmp/cas-pub-marker/final3_traces.md`:
the action sequence of the counterexample and the schedule it matches (S4 for `late_resolved_entry`, L1b for
`late_resolved_stale`, L2 for `late_gaveup`, S3 or a C3 schedule for `nogateround_noregistry`, S1 for `nojob`,
the spare-then-re-condemn wedge for `norestamp_live`, the namespace-removal schedule for `extdrop_noregistry`,
S2 / returning leader for `rv_bug` / `f1_bug`). A green-expected row that violates, or a violation that matches
none of these, is a new schedule: stop and report it; it reopens the spec section it contradicts.

- [ ] **Step 8: Promote**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
for pair in bug:old_bug bug_both:old_bug_both bug_both_2w:old_bug_both_2w fix:old_fix fix_pconf:old_fix_pconf \
            fix_prefetch:old_fix_prefetch fix_both:old_fix_both fix_both_2w:old_fix_both_2w; do
  d=${pair%%:*}; t=${pair#*:}
  grep -Fxvf tmp/cas-pub-marker/CaGcCondemnMarkerGate_$t.cfg docs/superpowers/models/CaGcCondemnMarkerGate_$d.cfg \
    && echo "MISMATCH $d"   # prints only lines of the promoted cfg absent from its variant twin
  cp tmp/cas-pub-marker/CaGcCondemnMarkerGate_$t.cfg docs/superpowers/models/CaGcCondemnMarkerGate_$d.cfg
done
cp tmp/cas-pub-marker/CaGcCondemnMarkerGate.tla docs/superpowers/models/CaGcCondemnMarkerGate.tla
for row in pm_g1_noregistry pm_g1_all_noregistry rv_g1_noregistry pm_g1_f1 pm_g1_live pm_g1_late_resolved \
           pm_g1_late_resolved_entry pm_g1_late_resolved_version pm_g1_late_resolved_stale pm_g1_late_gaveup \
           pm_g1_nogateround_noregistry pm_g1_nojob pm_g1_norestamp_live pm_g1_extdrop_noregistry rv_bug f1_bug; do
  cp tmp/cas-pub-marker/CaGcCondemnMarkerGate_$row.cfg docs/superpowers/models/
done
```

Expected: no `MISMATCH` line (a promoted cfg equals its variant twin except for the added constants).

`docs/superpowers/models/run_condemnmarker.sh`:
- Replace the `CONFIGS=( ... )` array with:

```bash
CONFIGS=(
    "bug                          violation  NoDangle"
    "bug_both                     violation  NoDangle"
    "bug_both_2w                  violation  NoDangle"
    "rv_bug                       violation  NoDangle"
    "f1_bug                       violation  NoDangle"
    "pm_g1_late_resolved_entry    violation  NoDangle"
    "pm_g1_late_resolved_stale    violation  NoDangle"
    "pm_g1_late_gaveup            violation  NoDangle"
    "pm_g1_nogateround_noregistry violation  NoDangle"
    "pm_g1_nojob                  violation  JobSparesPendingGuard"
    "pm_g1_norestamp_live         violation  NoWedgedCarry"
    "pm_g1_extdrop_noregistry     violation  NoDangle"
    "fix                          green      -"
    "fix_pconf                    green      -"
    "fix_prefetch                 green      -"
    "fix_both                     green      -"
    "fix_both_2w                  green      -"
    "pm_g1_noregistry             green      -"
    "pm_g1_all_noregistry         green      -"
    "rv_g1_noregistry             green      -"
    "pm_g1_f1                     green      -"
    "pm_g1_live                   green      -"
    "pm_g1_late_resolved          green      -"
    "pm_g1_late_resolved_version  green      -"
)
```

  (use the invariant name Step 6 observed for `pm_g1_nojob`).
- Replace the `java` invocation with

```bash
    timeout "${TLC_TIMEOUT:-3600}" nice -n 10 /usr/bin/java -XX:+UseParallelGC -Xmx8g ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
        -metadir "$meta" -workers "${TLC_WORKERS:-8}" -deadlock -config "$cfg" "$MODULE.tla" >"$log" 2>&1
```

- After `elapsed=...` add `distinct="$(grep -oE '[0-9]+ distinct states found' "$log" | tail -1 | awk '{print $1}')"`,
  widen the `printf` header and row to print `DISTINCT` between `RESULT` and `SECONDS`
  (`printf '%-30s %-11s %-32s %12s %-8s %s\n' ... "${distinct:-?}" ...`).

Run the promoted battery's quick rows to prove the promotion: `cd /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models && ./run_condemnmarker.sh bug fix pm_g1_noregistry rv_bug > ../../../tmp/cas30/promoted_check.txt 2>&1; echo RUN_EXIT=$? >> ../../../tmp/cas30/promoted_check.txt`
Expected: `ALL EXPECTATIONS MET`, `RUN_EXIT=0`; distinct counts equal Step 4 / Step 6.

- [ ] **Step 9: Regenerate `CaGcCondemnMarkerGate_RESULTS.md`**

Rewrite `docs/superpowers/models/CaGcCondemnMarkerGate_RESULTS.md`, keeping its frontmatter, with these sections
(anchors required), every number copied from `final3_*.txt` at write time:
- `## Verdict {#verdict}` — one paragraph: the PR1 configuration is green in every required row, each sabotage row
  violates the named invariant, and the inert defaults reproduce every earlier count.
- `## Checker and finite scope {#checker-and-finite-scope}` — TLC version, jar SHA-256
  (`sha256sum tmp/tla2tools.jar`), 8 workers, `MaxGen 3`, `MaxRounds 7`, `TwoWriters`, what the model does not show
  (spec §8 "What the model does not show", restated in two bullets).
- `## Switches {#switches}` — the table of spec §8 "PR1 switches" with the constant names.
- `## Invariants {#invariants}` — `NoDangle`, `MarkerPreservedPending`, `MarkerPreservedConfirmed`,
  `JobSparesPendingGuard`, `NoWedgedCarry` with their TLA text.
- `## Required runs {#required-runs}` — table: cfg, expected, observed, distinct, seconds, trace (the
  `final3_traces.md` line for violations).
- `## Regression {#regression}` — table: cfg, final2 result/distinct, final3 result/distinct.
- `## Big run {#big-run}` — `pm_g1_big_noregistry` result or distinct at timeout.

- [ ] **Step 10: Commit (master worktree, exact files)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git status --short docs/superpowers/models/
git add -- docs/superpowers/models/CaGcCondemnMarkerGate.tla docs/superpowers/models/run_condemnmarker.sh \
  docs/superpowers/models/CaGcCondemnMarkerGate_RESULTS.md \
  docs/superpowers/models/CaGcCondemnMarkerGate_bug.cfg docs/superpowers/models/CaGcCondemnMarkerGate_bug_both.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_bug_both_2w.cfg docs/superpowers/models/CaGcCondemnMarkerGate_fix.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_fix_pconf.cfg docs/superpowers/models/CaGcCondemnMarkerGate_fix_prefetch.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_fix_both.cfg docs/superpowers/models/CaGcCondemnMarkerGate_fix_both_2w.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_noregistry.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_all_noregistry.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_rv_g1_noregistry.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_f1.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_live.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_late_resolved.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_late_resolved_entry.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_late_resolved_version.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_late_resolved_stale.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_late_gaveup.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_nogateround_noregistry.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_nojob.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_norestamp_live.cfg docs/superpowers/models/CaGcCondemnMarkerGate_pm_g1_extdrop_noregistry.cfg \
  docs/superpowers/models/CaGcCondemnMarkerGate_rv_bug.cfg docs/superpowers/models/CaGcCondemnMarkerGate_f1_bug.cfg
git commit -s -m "model(cas): condemn-marker gate with round rules, PR1 cfgs and results"
```

---

### Task 8: User docs

Worktree: code. **Files:**
- Modify: `docs/en/antalya/cas/architecture/garbage-collection.md` (phase 9 reads, line ~342; cost summary table, line ~744; `cas_gc_io_concurrency` row, line ~784; `#condemn-delete`, lines 668–694; per-phase cost table, line ~880)
- Modify: `docs/en/sql-reference/statements/system.md` (`SYSTEM CAS GC REBUILD`, after line ~517)
- Modify: `docs/en/antalya/cas/operations/debugging.md` (`cas-gc-rebuild` row, line 245; new section before `## The CLICKHOUSE_USER_FILES gotcha`)

**Interfaces:** none (prose). Every new header ends with `{#kebab-anchor}`.

- [ ] **Step 1: `garbage-collection.md`**

Phase 9 **Reads** bullet: replace "one `.meta` `GET` per graduation candidate lacking in-process marker
confirmation" with "one `.meta` `GET` per graduation candidate the round's graduation budget admits, read ahead
when `cas_gc_io_concurrency` is above 1".

Cost summary table (after the `HEAD` blobs row) add:

```markdown
| `GET` blob `.meta` at graduation | 1 per entry the graduation budget admits, plus 1 `GET` and 1 `PUT` per refusal (the marker is rewritten) |
| `GET` + conditional `DELETE` blob `.meta` after a redelete | 1 `GET` per `Deleted` or `Absent` outcome; the `DELETE` only while the marker is `Condemned` at the entry's round or older |
```

`cas_gc_io_concurrency` row: after "zero-candidate `HEAD`s (phases 8, 9)" insert ", the graduation `.meta` reads (phase 9)".

Per-phase cost table, the blob `.meta` row: `| blob `.meta` | `GET` | one per graduation candidate the graduation budget admits |`.

`#condemn-delete` flowchart: replace the three edges

```
    D -->|"still zero, confirmed durable Condemned evidence for hash and t"| G["GRADUATED -- delete_pending"]
    D -->|"still zero, evidence unconfirmed"| C2["carried unchanged, retry the marker, never throw"]
    ...
    E -->|"Deleted or Absent"| F["then drop the .meta"]
```

with

```
    D -->|"still zero, reads .meta: Condemned at round n or newer"| G["GRADUATED -- delete_pending"]
    D -->|"still zero, .meta absent, Clean, older or unreadable"| C2["carried, .meta rewritten at this round, never throw"]
    ...
    E -->|"Deleted or Absent"| F["then drop the .meta if it is still Condemned at round n or older"]
```

and replace the paragraph under the diagram with:

```markdown
The `.meta` sidecar carries **no token** — it is a per-hash hint. The exact incarnation token lives in the
condemned sentinel row inside the run, together with the condemn round and two flags, `delete_pending` and
`marker_confirmed`; `GC` writes the second with the first and never reads it. Every `GC` marker write stamps the
round that issues it and replaces an absent marker, `Clean`, or an older-round `Condemned`; `GC` never writes
`Clean`, not even when sparing — only a writer that has already displaced the body clears it.

Graduation reads the marker in the round that graduates and accepts only `Condemned` at the entry's round or
newer: an older round may belong to another incarnation of the same content. The marker delete after a redelete
removes only `Condemned` at the entry's round or older, conditional on the version it read, so a delayed delete
never removes the marker of a later incarnation. It can leave an orphan `Condemned` marker over an absent body;
the next writer of that hash republishes and clears it, and `cas-fsck` counts it in `meta_without_body`.

A blob whose in-degree reaches zero in round `n` is retired with `condemn_round = n`; it can graduate to
`delete_pending` in round `n+1` at the earliest and be deleted in round `n+2`, so a minimum of two full rounds
separate condemnation from deletion. `delete_pending` is never cleared in place, but it authorizes a delete only
while in-degree stays zero: a fresh edge folded in a later round spares the entry and removes it from the retired
pipeline (recovery wins, even past the floor).
```

- [ ] **Step 2: `system.md`, `SYSTEM CAS GC REBUILD`**

After the paragraph that ends "...when it carried none." add:

```markdown
Before rebuilding a lost `gc/state`, stop or restart every server of the pool. A request a server sent before it
stopped can still reach the object store afterwards; this rule does not cover it.
```

- [ ] **Step 3: `debugging.md`**

`cas-gc-rebuild` row: append to its cell " Before rebuilding a lost `gc/state`, stop or restart every server of the
pool; a request sent before a server stopped can still reach the object store afterwards, and this rule does not
cover it."

Before `## The CLICKHOUSE_USER_FILES gotcha when reproducing a test manually {#user-files-gotcha}` add:

```markdown
## Upgrading a pool with GC enabled {#upgrade-gc}

`cas_gc_enabled` is read once, when the disk is constructed: only a restart applies a change.

1. On every server still on the old version, set `<cas_gc_enabled>false</cas_gc_enabled>` in each CAS disk's
   configuration and restart the server. It then runs no GC and refuses `SYSTEM CAS GC RUN`, `START` and `REBUILD`.
2. Only after step 1 has finished on every server, upgrade the servers, removing the setting on each upgraded one.
3. Never run `SYSTEM CAS GC` commands on, or re-enable GC on, a server still on the old version.

A delete request sent before a restart can still reach the object store later and remove a marker GC relies on,
so a blob can be lost; nothing bounds when such requests stop. Entries the old version already scheduled for
deletion are deleted by the upgraded GC without a fresh marker read. Downgrades follow the same procedure.
```

- [ ] **Step 4: Validate the Mermaid block**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && mkdir -p tmp/mermaid-check && cd tmp/mermaid-check
[ -d node_modules/mermaid ] || npm install mermaid@11 jsdom --no-audit --no-fund > npm.log 2>&1
cat > check.mjs <<'EOF'
import { readFileSync } from 'fs';
import { JSDOM } from 'jsdom';
const dom = new JSDOM('<!doctype html><html><body></body></html>', {pretendToBeVisual: true});
globalThis.window = dom.window; globalThis.document = dom.window.document;
Object.defineProperty(globalThis, 'navigator', {value: dom.window.navigator, configurable: true});
for (const k of ['Element', 'SVGElement', 'HTMLElement', 'DOMParser', 'Node']) globalThis[k] = dom.window[k];
globalThis.getComputedStyle = dom.window.getComputedStyle;
const mermaid = (await import('mermaid')).default;
mermaid.initialize({startOnLoad: false});
const text = readFileSync(process.argv[2], 'utf8');
const blocks = [...text.matchAll(/```mermaid\n([\s\S]*?)```/g)].map(m => m[1]);
let bad = 0;
for (const [i, b] of blocks.entries()) { try { await mermaid.parse(b); } catch (e) { bad++; console.log('BLOCK', i, e.message); } }
console.log(`blocks=${blocks.length} bad=${bad}`);
EOF
node check.mjs ../../docs/en/antalya/cas/architecture/garbage-collection.md
```

Expected: `bad=0`.

- [ ] **Step 5: Anchor check and commit**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && git diff -U0 -- docs/en | grep '^+#' | grep -v '{#'`
Expected: no output.

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
git add -- docs/en/antalya/cas/architecture/garbage-collection.md docs/en/sql-reference/statements/system.md \
  docs/en/antalya/cas/operations/debugging.md
git commit -s -m "Document the CAS condemn marker round rules and the upgrade procedure"
```

---

### Task 9: Gate lanes and mutation checks

Worktree: code. **Files:** none committed, except a golden fix if a lane finds one (by exact path).

- [ ] **Step 1: Release gate on the final tree**

Build (log `build/build_cas30_t9.log`) and run the `CAS*` gate (log `build/test_cas30_t9_gate.log`).
Expected `NINJA_EXIT=0`, `GATE_EXIT=0`.

- [ ] **Step 2: ASan and TSan lanes**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
for san in address thread; do
  dir=build_${san:0:1}san; mkdir -p $dir
  [ -f $dir/build.ninja ] || cmake -S . -B $dir -G Ninja -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DSANITIZE=$san -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=1 > $dir/cmake.log 2>&1
  ninja -C $dir unit_tests_dbms > $dir/build_cas30.log 2>&1; echo NINJA_EXIT=$? >> $dir/build_cas30.log
done
```

(`build_asan`, `build_tsan`.) Then each lane, under a cgroup cap instead of `ulimit -v` (sanitizers reserve shadow
address space that a virtual-memory limit refuses):

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
for dir in build_asan build_tsan; do
  systemd-run --user --scope -p MemoryMax=32G -p MemorySwapMax=0 \
    $dir/src/unit_tests_dbms --gtest_filter='CAS*' > $dir/test_cas30_gate.log 2>&1; echo GATE_EXIT=$? >> $dir/test_cas30_gate.log
done
```

Expected `GATE_EXIT=0` in both. A sanitizer report in any new test is a defect: fix it in the task's code and
commit the fix by exact path.

- [ ] **Step 3: Fast-test compile of the touched translation units**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
mkdir -p build_fast
[ -f build_fast/build.ninja ] || cmake -S . -B build_fast -G Ninja -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
    -DENABLE_LIBRARIES=0 -DENABLE_TESTS=0 > build_fast/cmake.log 2>&1
targets=$(ninja -C build_fast -t targets all | grep -oE '^[^:]*ContentAddressed/(Gc/CasGc|Gc/CasGcMetaWriter|Gc/CasBlobInDegree|Pool/CasBlobMeta)\.cpp\.o')
ninja -C build_fast $targets > build_fast/build_cas30.log 2>&1; echo NINJA_EXIT=$? >> build_fast/build_cas30.log
```

Expected `NINJA_EXIT=0` and four objects built.

- [ ] **Step 4: Mutation checks (release build)**

For each row: make the one-line change, build (log `build/build_cas30_mut<N>.log`), run the named tests (log
`build/test_cas30_mut<N>.log`), confirm each named test FAILS, then restore the file with
`git checkout -- <that file>` (the mutation is that file's only diff; check `git diff --stat` first).

| N | Mutation | File | Must fail |
|---|---|---|---|
| 1 | `deleteConfirmedMeta`: drop `\|\| lm->meta.condemn_round > condemn_round` | `CasGcMetaWriter.cpp` | `ConfirmedMetaDeleteSparesANewerRound`, `GraduationAcceptsANewerRoundMarker`, `StaleMetaDeleteSparesSuccessorMarker*` |
| 2 | gate: drop `&& lm->meta.condemn_round >= entry.condemn_round` | `CasGc.cpp` | `GraduationRefusesAnOlderRoundMarker` |
| 3 | gate: `>=` → `==` | `CasGc.cpp` | `GraduationAcceptsANewerRoundMarker` |
| 4 | gate: `return true;` as its first statement | `CasGc.cpp` | `GraduationRereadsTheMarker*`, `GraduationAndDeleteRequestCounts`, `GraduationReadAheadReadsEachRound` |
| 5 | `writeCondemnedMeta`: drop `\|\| lm->meta.condemn_round < condemn_round` | `CasGcMetaWriter.cpp` | `CondemnMarkerWriteRestampsAnOlderRound`, `SparedThenRecondemnedBlobGraduates` |
| 6 | retry: `condemn_round` → `entry.condemn_round` | `CasGc.cpp` | `LatePublisherCleanMissesTheRetriedMarker` |
| 7 | `settleEntry`: gate before the budget (swap the two `if` arms of Task 2) | `CasBlobInDegree.cpp` | `GraduationBudgetIsCheckedBeforeTheGate`, `GraduationReadsStayWithinTheBudget`, `GraduationReadAheadRequestCounts` |
| 8 | `settleEntry`: prepend `e.marker_confirmed \|\|` to the gate condition | `CasBlobInDegree.cpp` | `MarkerConfirmedBitDoesNotSkipTheGate` |
| 9 | `offer`: `graduation_reserved.size() >= reads.window()` → `reads.pending() >= reads.window()` | `CasGc.cpp` | `GraduationReadAheadIgnoresUnusedHeadHints` |
| 10 | `offer`: drop `\|\| touched_blobs.contains(ref)` | `CasGc.cpp` | `GraduationReadAheadSkipsSupersededEntries` |
| 11 | `offer`: drop `row.delete_pending \|\|` | `CasGc.cpp` | `GraduationReadAheadNeverHintsPendingRows` |

Write the table with the observed failing test and its message into `tmp/cas30/mutations.md` (code worktree). A
mutation that fails no named test is a gap: add the missing assertion to the owning test, commit it by exact path,
and rerun that row.

- [ ] **Step 5: Confirm the tree is clean**

Run: `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && git status --short && git log --oneline e2dd0f5d343..HEAD`
Expected: no modified tracked file; the commits of Tasks 1–6 and 8 (and any lane fix).

---

### Task 10: Measure the graduation phase on the `gc-ab` stand

Worktree: code (binaries); scripts from the master checkout. Needs docker; no stack `ca-soak-*` may be up.

**Files:** creates `tmp/cas30/gc-io-1.xml`, `tmp/cas30/gc-io-16.xml`, `tmp/cas30/clickhouse-pr1`, run directories
under `tmp/cas30/gc-ab/` (untracked). Writes the report `tmp/cas30/gc-ab/REPORT.md`.

- [ ] **Step 1: Binaries and configs**

```bash
cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts
ninja -C build clickhouse > build/build_cas30_t10.log 2>&1; echo NINJA_EXIT=$? >> build/build_cas30_t10.log
cp build/programs/clickhouse tmp/cas30/clickhouse-pr1
for n in 1 16; do cat > tmp/cas30/gc-io-$n.xml <<EOF
<clickhouse>
    <storage_configuration>
        <disks>
            <ca>
                <gc_io_concurrency>$n</gc_io_concurrency>
            </ca>
        </disks>
    </storage_configuration>
</clickhouse>
EOF
done
```

- [ ] **Step 2: Runs**

Two runs per (binary, concurrency); `S11`, `ci` scale.

```bash
export GC_AB_OUT=/home/mfilimonov/workspace/ClickHouse/gc-request-cuts/tmp/cas30/gc-ab && mkdir -p $GC_AB_OUT && cd $GC_AB_OUT
S=/home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak/gc-ab
B=/home/mfilimonov/workspace/ClickHouse/gc-request-cuts/tmp/cas30
for i in 1 2; do
  for n in 1 16; do
    for side in base pr1; do
      label=$side-io$n-s11ci-$i
      GC_EXTRA_CONFIG=$B/gc-io-$n.xml $S/run-stand.sh run $label $B/clickhouse-$side \
        --scenario S11 --seed 20260930 --duration 300s --scale ci > $label.log 2>&1
      python3 $S/extract.py runs/$label > $label.txt
      $S/run-stand.sh down
    done
  done
done
```

- [ ] **Step 3: Report**

`python3 $S/confirm.py 'BASE-IO1=base-io1-s11ci-1,base-io1-s11ci-2' 'PR1-IO1=pr1-io1-s11ci-1,pr1-io1-s11ci-2' 'BASE-IO16=base-io16-s11ci-1,base-io16-s11ci-2' 'PR1-IO16=pr1-io16-s11ci-1,pr1-io16-s11ci-2' > confirm.txt`
and `python3 $S/t3.py runs/<label>` per run. Write `REPORT.md`: per group, median (min..max) of `fold_reduce`
time in rounds that graduated, graduations per round, `.meta` `GET`s per round (`ProfileEvents` from
`extract.py`), peak resident memory from `metric_log`; one paragraph on the effect of `kGraduationLookaheadRows`
(peak buffer = rows actually buffered; at most 16384 rows). Figures are re-read from the files at write time.

---

## Self-review

**Spec coverage.**

| Spec | Task |
|---|---|
| §1 G1, C3 | 3 |
| §1 C1 | 1 |
| §1 C2 (re-stamp) | 4; (retry at this round) 3 |
| §1 budget before the read | 2, 6 |
| §1 no format / memory / setting / fallback / writer change | Global Constraints; `marker_confirmed` still written (Task 2) |
| §5.4 what is removed | 3 (registry, token, `bool` result, memo, `forget` calls) |
| §5.5 bit not read, still written | 2 (B3) |
| §5.6 read-ahead: own window, reservations, `scanAhead`, segment edge, checksum, row cap, concurrency 1, `touched_blobs` | 5, 6 |
| §5.7 `decodeLoadedMeta`, `pendingHeads`, header sentence, `graduation_reads` only when `window() > 0`, discard after a shard | 6 |
| §5.7 user-doc edges | 8 |
| §7.2 rule B text | 8 |
| §8 model switches, invariants, required cfgs, regression, big run, trace reading, promotion, RESULTS | 7 |
| §9 tests U1–U6, R1–R9 (R6 does not exist in the spec), B1–B3, RA1–RA5, CB1–CB3, changed tests, goldens | 1–6 |
| §9 gate lanes ASan, TSan, fast test | 9 |
| §9 mutation checks | 9 |
| §11 upgrade procedure, operator note | 8 |
| §13 assumption 1: `gc-ab` report of graduation time and memory, `kGraduationLookaheadRows` effect | 10 |
| Limits: reading claims re-checked against the final diff | 6 Step 6; `cas_gc_enabled` claim (F18) is pre-existing code, not changed here |

Spec §9 says "commit 1: every test against today's API, commit 2: the fix". This plan keeps the property that
claim protects — each test is seen red, for the reason in the spec's table, before the change that turns it
green — but commits per rule, as the dispatch asked. Tests whose red reason (the registry, the gate before the
budget) is removed by an earlier task are written in that earlier task.

**Type consistency.** `scheduleConfirmedMetaDelete(const BlobRef &, uint64_t)` (Task 1) is used in Task 1's helper
and `applyRedeleteOutcome`. `scheduleCondemnMarkerWrite(const BlobRef &, uint64_t, uint64_t)` (Task 3) is used in
Tasks 3, 4, 6. `GraduationReadHints{admit, offer}` and `kGraduationLookaheadRows` (Task 5) are used in Tasks 5–6.
`decodeLoadedMeta(std::optional<Object>)` and `pendingHeads()` (Task 6) are used only in Task 6. Test support
names are defined in Task 1 Step 1 and Task 3 Step 1 and used unchanged afterwards.

**Placeholder scan.** Model counts, trace lines and stand figures are run outputs, not placeholders; every code
step has its code.

**Known risks for the executor.**
- R7 and R1/LostAttemptMarker run a second leader while the first is parked inside its round; both share one
  `Pool` object, as `CASGCFrontierGate.StaleIssuedCatalogCasLosesAfterNewLeaderHelpsBeforeListing` already does.
- The writer inside `HeadHookBackend`'s hook runs on the redelete's thread (the round thread at concurrency 1);
  `Gc` holds no lock across the redelete, so it cannot self-deadlock there.
- `ContentEtagBackend` is test-only; it gives `.meta` keys S3's content-etag behaviour, which the in-memory backend
  (minted values) cannot show.
