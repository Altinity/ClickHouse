---
description: 'Implementation plan for GC fold read-ahead: a key-addressed prefetch in front of the fold''s reads so ref-log, checkpoint, manifest and zero-candidate HEAD round trips overlap on a bounded pool while every decision stays on the round thread, in the same order, with the same counters.'
sidebar_label: 'GC fold read-ahead plan'
sidebar_position: 45
slug: /superpowers/plans/cas-gc-fold-read-ahead-plan
title: 'CAS GC fold read-ahead — implementation plan'
doc_type: 'guide'
---

# CAS GC fold read-ahead — implementation plan {#cas-gc-fold-read-ahead-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the wall time of the GC fold phases `fold_ref_intake` and `fold_reduce` by overlapping their small-object round trips on a bounded pool, without moving a single decision, decode, counter or event off the round thread.

**Architecture:** A `GcReadAhead` object sits in front of the fold's one admitted `CasOperation`. Callers *hint* keys whose bytes the sequential walk will need next; workers fetch them with `op.read` / `op.head` under the same admitted generation; the walk *takes* results at exactly the sites and in exactly the order it reads today, and a key nobody hinted is read inline. Four intake sites hint (checkpoints, each namespace's first walk position, a same-epoch lookahead window, the manifest edges of a decoded log) and one reduce site hints (HEADs for the blobs that can reach in-degree zero). Concurrency `1` issues no hints and is byte-for-byte today's behaviour.

**Tech Stack:** C++23 (ClickHouse tree, Allman braces), `ThreadPool` (`src/Common/ThreadPool.h`), `std::promise`/`std::future`, gtest (`unit_tests_dbms`, suites prefixed `CAS`), `InMemoryBackend` / `CountingBackend` test doubles, `utils/ca-soak` for measurement.

**Spec:** none as a file — this was a bounded brainstorm (session `https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s`, 2026-09-03). The design and its safety argument are recorded in full in [Design](#design) below so the plan survives the session. The measurements that motivated it: `docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md` (`fold_ref_intake` = 2303 s of 4352 s phase wall, 52.9%) and finding F15 in `docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md` (one fold round holds the lease for hours on GCS; `fold_ref_intake` unfinished after 97 minutes).

## Global Constraints {#global-constraints}

- **Worktree and branch.** Work in `/home/mfilimonov/workspace/ClickHouse/lane-g` (it has `build/` with `ENABLE_TESTS=ON` and `contrib/` populated). Branch `cas-gc-fold-read-ahead`, created from `cas-gc-rebuild` (`94aaedb8a1e` or later). On completion the branch is merged back into `cas-gc-rebuild` with `--no-ff` from the `master` worktree (Task 7). Never rebase or amend; add commits. Never push.
- **Commit discipline (shared worktrees, HARD RULE).** Always `git commit -m "ca-gc: cas_gc_read_concurrency setting and the read-ahead counters

The fold's read-ahead pool size, plumbed like gc_meta_pool_size, refused at 0 like gc_shards;
three ProfileEvents for hits, misses and wasted results.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" -- <paths>` with named files; before every commit run `git diff --cached --stat` and `git status --short` and refuse to commit if anything foreign is staged. Verify `git branch --show-current` after each commit. Never `git add -A`.
- **Commit trailers:** `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s`.
- **Build and test command shapes** (always redirected, always with the marker, logs read by a subagent, never into the implementer's context; no `-j`, no `nproc`):
  - `ninja -C /home/mfilimonov/workspace/ClickHouse/lane-g/build unit_tests_dbms > /home/mfilimonov/workspace/ClickHouse/lane-g/build/build_<task>.log 2>&1; echo NINJA_EXIT=$? >> /home/mfilimonov/workspace/ClickHouse/lane-g/build/build_<task>.log`
  - `/home/mfilimonov/workspace/ClickHouse/lane-g/build/src/unit_tests_dbms --gtest_filter='<filter>' > /home/mfilimonov/workspace/ClickHouse/lane-g/build/test_<task>.log 2>&1; echo GTEST_EXIT=$? >> /home/mfilimonov/workspace/ClickHouse/lane-g/build/test_<task>.log`
  - A green test log after a failed build is the wrong binary: read `NINJA_EXIT=` first, every time.
- **Gate.** The CAS unit-test gate filter is exactly `CAS*`, never widened; the new suite is `CASGCReadAhead`. No known reds: any red is root-caused or becomes a tracked return item before the task closes.
- **Code rules.** Allman braces. No fallback paths that substitute a value on failure (a miss reading inline is not a fallback: it is today's code path, and it is counted). No `sleep` to fix a race. `LOGICAL_ERROR` is an "exception", never a "crash". Function names in prose as `f`, not `f()`. Comments carry the reason, never plan/BACKLOG/session provenance.
- **Docs rules.** Every header in a file under `docs/` carries an explicit `{#anchor}`; new docs files carry the frontmatter block.
- **Line numbers** below are as of `cas-gc-rebuild` at `b04f12d89b0`; re-locate by the quoted code, not by the number.

## Design {#design}

### Where the sequential round trips are {#design-where}

Phase 8/18 `fold_ref_intake` (`Gc::fold`, `Gc/CasGc.cpp`):

1. `readCheckpointWitnesses` — one GET of `_ckpt` per `Live`/`Removing` namespace, in `std::set` order, sequential (lines 1288–1306).
2. Per namespace, the first walk position `cursor + 1` (or `{life_epoch, 1}` on a first fold) — read only when it is at or below the checkpoint's `committed_through`; a quiet namespace (`cursor == committed_through`) is proven by the ceiling test and reads nothing (lines 2229–2233, 2322).
3. Per record inside a namespace — one GET at the arithmetic next id `{epoch, seq + 1}`, sequential, bounded above by `committed_through` (line 2322).
4. Per manifest edge of a decoded log — one GET in `foldManifestEdges` (line 1195), sequential over the log's edges (call at 2418).

Phase 9/18 `fold_reduce`: the prior run is one streamed object per shard (bandwidth, not round trips). The round trips are `head_blob` (one HEAD per fresh zero-in-degree candidate, line 1704), `peek_head` (one HEAD per carried-condemned blob that lost activity, line 1753), and `confirm_condemned_marker`'s `loadMeta` GET — all issued inline from the streaming merge's `closeBlob`. With the default `gc_shards = 1` there is nothing to parallelise across shards.

The paired HEAD before every GET that the 2026-08-04 baseline measured (`DiskS3HeadObject == DiskS3GetObject`, HEAD time ≈ GET time) was removed by `e272e18f02c` (`read` now goes through `readSmallObjectAndGetObjectMetadata`, one GET). It is not part of this plan; Task 5 closes its BACKLOG entry.

### The mechanism {#design-mechanism}

`GcReadAhead` is a cache of *results*, not of decisions. A hinted key is fetched on the pool by a worker that resumes a `CasOperation` under the fold's own admitted generation (`CasRequests::resume(op.generation())` — the fold's `op` is a plain `admit()` with no liveness, so the resumed operation gates identically). `takeRead` / `takeHead` return the fetched result and rethrow the worker's exception at the take site, or, for a key nobody hinted, perform the request inline on the fold's `op`. Every decode, every `ProfileEvents` increment that a phase row reports (`CASRefLogBodyGets`, `CASRefManifestBodyFoldGets`, `absent_probes`, …), every `EventEmitter` event and every ledger update stays at the take site on the round thread.

Why a result may move earlier in time without changing a decision:

- Ref logs and manifests are write-once. A present body is the same body at any later instant.
- A position at or below `committed_through` was durable before the round started (the checkpoint is snapshotted once per namespace and never re-read). An absent answer there is a gap whenever it is read. Hints never go above `committed_through`, so no read is issued that the sequential walk would not issue.
- A manifest body that is still being uploaded when the early read lands produces the same `ManifestBodyMissing` hold that a slightly earlier sequential read produces today; it clears the next round exactly as today.
- `_ckpt` is mutable but is read once per namespace per round in both designs.
- A worker exception surfaces at the take site — "a backend throw still propagates and fails the round, exactly as it always did".

Memory is bounded by the callers: `pending()` counts hinted-but-untaken slots, `window()` is `4 × concurrency`, and every hinting site keeps at most `window()` (the lookahead site at most `2 × window()`) in flight. Results hinted and never taken (a namespace held below its lookahead, a HEAD candidate that kept an edge) are awaited in the destructor and counted as `CASGCReadAheadWasted`.

### R2: HEAD read-ahead in reduce, and its argument {#design-r2}

`closeBlob` calls `head_blob` when `cur_edges == 0 && cur_touched && !cur_condemned` and `peek_head` when `cur_edges == 0 && cur_touched && cur_condemned`. `cur_touched` needs a prior edge row, a delta or a retirement for the blob; `cur_edges == 0` needs every prior edge removed by a `-1` delta or a source retirement and every `+1` delta's last verdict to be a removal (or the edge retired). So every candidate of either lambda has at least one removal this round and no surviving `+1`. That set is computable before the merge from the in-memory `deltas` and `orphan_source_retirements`, is a superset of the candidates, and is hinted per shard in ascending `BlobRef` order — the merge's own consumption order — `window()` deep. A named blob that keeps untouched prior edges costs one HEAD the merge never takes (wasted, counted); a candidate the set misses is HEADed inline (miss, counted). Correctness never depends on the set.

The invariant the HEAD keeps: **it is issued after the cut is frozen** (intake has finished, the deltas are in memory) and only moves earlier *within* `fold_reduce`. The four outcomes of "HEAD earlier" against "HEAD at `closeBlob`" all map onto existing paths: same token → identical; older token, blob republished in between → the exact-token delete is a no-op and the entry is `replaced` by the next round's `peek_head`; present earlier, absent at close → `objects_absent` at delete time; absent earlier, present at close → not condemned this round, a `Zero` marker, re-examined when a delta next touches the blob — the same outcome as a writer landing right after today's HEAD. The condemn-marker write still happens at the take site, after the HEAD, as today. Task 4 gets two independent `ca-arch` consults on this argument before any R2 code is written; a non-clean verdict ends Task 4 with a BACKLOG entry and leaves Tasks 1–3 intact.

### Reporting effect, stated once {#design-reporting}

`GcPhaseTimer` reports the ProfileEvents delta of the *round thread*. Requests that workers perform (`DiskS3GetObject`, `S3ReadMicroseconds`, …) leave the phase row and land on worker-thread counters, the same gap the baseline report already names for `meta_pool_wait` and `orphan_sweep`. The CAS-specific counters incremented at take sites are unchanged. The three new counters (`CASGCReadAheadHit`, `CASGCReadAheadMiss`, `CASGCReadAheadWasted`) are incremented on the round thread, so they do appear on the row.

## File structure {#file-structure}

- **Create** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h` — the class: hints, takes, `pending`, `window`; owns nothing but slots.
- **Create** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.cpp` — scheduling, futures, counters.
- **Create** `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` — suite `CASGCReadAhead`: unit tests of the class, fold-level determinism tests, worker-fault propagation, R2 equivalence.
- **Modify** `Gc/CasGc.h` — `read_pool` member; `readCheckpointWitnesses` and `foldManifestEdges` signatures.
- **Modify** `Gc/CasGc.cpp` — pool construction in the constructor; `GcReadAhead` in `fold` and in the rebuild; the four intake hint sites; the reduce candidate set and the two HEAD lambdas.
- **Modify** `Pool/CasPool.h`, `ContentAddressedSettings.cpp`, `ContentAddressedMetadataStorage.h`, `ContentAddressedMetadataStorage.cpp` — the `gc_read_concurrency` setting, mirroring `gc_meta_pool_size` line for line.
- **Modify** `src/Common/ProfileEvents.cpp` — three counters next to `CASGCGet`.
- **Modify** `docs/en/operations/storing-data.md`, `docs/en/antalya/cas/architecture/garbage-collection.md`, `docs/en/antalya/cas/configuration.md` — one setting row each; `docs/superpowers/cas/BACKLOG.md` — close one entry, add two.

---

### Task 0: Branch, baseline build, baseline gate {#task-0}

**Files:** none changed.

**Interfaces:**
- Produces: branch `cas-gc-fold-read-ahead` in `lane-g`; `lane-g/build/test_task0.log` with the baseline `CAS*` pass count that later tasks must not regress.

- [ ] **Step 1: Check lane-g is idle and clean of tracked changes**

Run: `git -C /home/mfilimonov/workspace/ClickHouse/lane-g status --short | grep -v '^??' ; git -C /home/mfilimonov/workspace/ClickHouse/lane-g branch --show-current`
Expected: no tracked modifications printed; current branch `feature/antalya-26.6/CAS-improvements`. If tracked modifications exist, stop and ask — another lane owner may be mid-task.

- [ ] **Step 2: Create the branch from `cas-gc-rebuild`**

```bash
git -C /home/mfilimonov/workspace/ClickHouse/lane-g fetch . 2>/dev/null; \
git -C /home/mfilimonov/workspace/ClickHouse/lane-g checkout -b cas-gc-fold-read-ahead cas-gc-rebuild && \
git -C /home/mfilimonov/workspace/ClickHouse/lane-g log --oneline -1
```
Expected: the last line names the `cas-gc-rebuild` head commit (`94aaedb8a1e` or later). The worktrees share one repository, so the branch is visible without a fetch.

- [ ] **Step 3: Bring submodules to the branch's pointers (only those that moved)**

Run: `git -C /home/mfilimonov/workspace/ClickHouse/lane-g submodule status | grep '^[+-]' | awk '{print $2}'`
For each path printed: `git -C /home/mfilimonov/workspace/ClickHouse/lane-g submodule update --init -- <path>`. If nothing is printed, nothing to do.

- [ ] **Step 4: Baseline build**

Run: `ninja -C /home/mfilimonov/workspace/ClickHouse/lane-g/build unit_tests_dbms > /home/mfilimonov/workspace/ClickHouse/lane-g/build/build_task0.log 2>&1; echo NINJA_EXIT=$? >> /home/mfilimonov/workspace/ClickHouse/lane-g/build/build_task0.log`
Then dispatch a `ca-review-lite` subagent to read the log and return: the `NINJA_EXIT=` value, the number of compile errors, and the first error if any. Expected: `NINJA_EXIT=0`.

- [ ] **Step 5: Baseline gate**

Run: `/home/mfilimonov/workspace/ClickHouse/lane-g/build/src/unit_tests_dbms --gtest_filter='CAS*' > /home/mfilimonov/workspace/ClickHouse/lane-g/build/test_task0.log 2>&1; echo GTEST_EXIT=$? >> /home/mfilimonov/workspace/ClickHouse/lane-g/build/test_task0.log`
Subagent returns: `GTEST_EXIT=`, the `[  PASSED  ] N tests` line, and every `[  FAILED  ]` line. Expected: `GTEST_EXIT=0`. Record N; it is the floor for Tasks 3–5.

---

### Task 1: The `gc_read_concurrency` setting and the three counters {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp:78` (DECLARE list) and `:226-229` (the `>= 1` check)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:612`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:83`, `:302`, `:767`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:170`
- Modify: `src/Common/ProfileEvents.cpp:876` (after `CASGCList`)
- Modify: `docs/en/operations/storing-data.md:546`, `docs/en/antalya/cas/architecture/garbage-collection.md:228`, `docs/en/antalya/cas/configuration.md:102`

**Interfaces:**
- Produces: `PoolConfig::gc_read_concurrency` (`uint64_t`, default `16`, declared immediately after `gc_meta_pool_size` — designated initializers in tests must list it after `gc_meta_pool_size`); user setting `cas_gc_read_concurrency`; `ProfileEvents::CASGCReadAheadHit`, `CASGCReadAheadMiss`, `CASGCReadAheadWasted`.

- [ ] **Step 1: Declare the setting**

In `ContentAddressedSettings.cpp`, directly after the `gc_meta_pool_size` line:

```cpp
    DECLARE(UInt64, gc_read_concurrency, 16, "Bounded pool size for the GC fold's read-ahead of checkpoints, ref logs, manifest bodies and zero-candidate HEADs; 1 disables read-ahead", 0) \
```

Extend the existing check so a zero is refused the way `gc_shards = 0` is:

```cpp
    if (settings[ContentAddressedSetting::gc_interval_sec] == 0 || settings[ContentAddressedSetting::gc_shards] == 0
        || settings[ContentAddressedSetting::gc_read_concurrency] == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "content_addressed disk: cas_gc_interval_sec, cas_gc_shards and cas_gc_read_concurrency must be >= 1 (got {}, {}, {})",
            settings[ContentAddressedSetting::gc_interval_sec].value, settings[ContentAddressedSetting::gc_shards].value,
            settings[ContentAddressedSetting::gc_read_concurrency].value);
```

(Keep the surrounding lines exactly as they are; only the condition and the message change.)

- [ ] **Step 2: Plumb it like `gc_meta_pool_size`**

`ContentAddressedMetadataStorage.h`, after `const uint64_t gc_meta_pool_size;`:

```cpp
    /// Bounded pool size for the GC fold's read-ahead; 1 disables it.
    const uint64_t gc_read_concurrency;
```

`ContentAddressedMetadataStorage.cpp`: after `extern const ContentAddressedSettingsUInt64 gc_meta_pool_size;` add `extern const ContentAddressedSettingsUInt64 gc_read_concurrency;`; after `, gc_meta_pool_size(settings_[ContentAddressedSetting::gc_meta_pool_size].value)` add `, gc_read_concurrency(settings_[ContentAddressedSetting::gc_read_concurrency].value)`; after `pool_config.gc_meta_pool_size = gc_meta_pool_size;` add `pool_config.gc_read_concurrency = gc_read_concurrency;`.

`Pool/CasPool.h`, after `uint64_t gc_meta_pool_size = 16;`:

```cpp
    /// Bounded pool size for the fold's read-ahead of checkpoints, ref logs, manifest bodies and
    /// zero-candidate HEADs. Every decision stays on the round thread; only the fetch overlaps. `1`
    /// issues no read-ahead at all and is the sequential round, request for request.
    uint64_t gc_read_concurrency = 16;
```

- [ ] **Step 3: Add the counters**

`src/Common/ProfileEvents.cpp`, after the `CASGCList` line:

```cpp
    M(CASGCReadAheadHit,    "Number of CAS GC fold reads and HEADs answered by the fold's read-ahead. Growth means the round's small-object round trips overlapped instead of serializing.", ValueType::Number) \
    M(CASGCReadAheadMiss,   "Number of CAS GC fold reads and HEADs performed inline because nothing was hinted for the key. A large value against hits means a hint set is narrower than the walk.", ValueType::Number) \
    M(CASGCReadAheadWasted, "Number of CAS GC read-ahead results fetched and never taken: a namespace held below its lookahead, or a HEAD candidate that kept an edge. Bounded by the read-ahead window per namespace.", ValueType::Number) \
```

- [ ] **Step 4: Docs rows**

`docs/en/operations/storing-data.md`, after the `cas_gc_meta_pool_size` bullet:

```markdown
- `cas_gc_read_concurrency` — `16` by default. Bounded thread-pool size for the GC fold's read-ahead of
  checkpoints, ref logs, manifest bodies and zero-candidate `HEAD`s. The fold's decisions stay on the
  round thread in their original order; only the fetches overlap. `1` disables read-ahead.
```

`docs/en/antalya/cas/architecture/garbage-collection.md`, after the `cas_gc_meta_pool_size` row:

```markdown
| `cas_gc_read_concurrency` | 16 | bounded pool for the fold's read-ahead; `1` disables |
```

`docs/en/antalya/cas/configuration.md`, after the `cas_gc_meta_pool_size` row:

```markdown
| `cas_gc_read_concurrency` | `16` | Bounded pool size for the GC fold's read-ahead of checkpoints, ref logs, manifests and zero-candidate HEADs; `1` disables |
```

- [ ] **Step 5: Build**

Run the build command shape with `<task>` = `task1`; subagent reads the log. Expected: `NINJA_EXIT=0`.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g && git status --short | grep -v '^??' && git diff --cached --stat && \
git commit -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Common/ProfileEvents.cpp docs/en/operations/storing-data.md \
  docs/en/antalya/cas/architecture/garbage-collection.md docs/en/antalya/cas/configuration.md && git branch --show-current
```

---

### Task 2: `GcReadAhead` with its unit tests {#task-2}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.cpp`
- Create: `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`

**Interfaces:**
- Consumes: `CasOperation` (`Backend/CasRequests.h`: `read(key, Retry)`, `head(key, Retry)`, `generation()`), `CasRequests::resume(uint64_t)`, `Retry::standard()` (`Backend/CasRetry.h`), `Object`/`Meta` (`Backend/CasWriteResult.h`), `ThreadPool`, the Task 1 counters.
- Produces:

```cpp
namespace DB::Cas
{
class GcReadAhead
{
public:
    GcReadAhead(CasOperation & op_, CasRequests & requests_, ThreadPool & pool_, size_t concurrency_);
    ~GcReadAhead();
    void hintRead(const String & key);
    void hintHead(const String & key);
    std::optional<Object> takeRead(const String & key);
    std::optional<Meta> takeHead(const String & key);
    size_t pending() const;
    size_t window() const;   /// 0 when concurrency <= 1, else 4 * concurrency
};
}
```

- [ ] **Step 1: Write the failing unit tests**

`src/Disks/tests/gtest_cas_gc_read_ahead.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>

#include <stdexcept>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::openRequestsForTest;

namespace
{

struct ReadAheadRig
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 4, /*max_free_threads*/ 4, /*queue_size*/ 0};

    void put(const String & key, const String & bytes)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::once()))) << key;
    }
};

}

TEST(CASGCReadAhead, HitReturnsTheHintedBytesWithOneRequest)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.hintRead("k1");
    EXPECT_EQ(reads.pending(), 1u);
    const auto got = reads.takeRead("k1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "one");
    EXPECT_EQ(reads.pending(), 0u);
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
}

TEST(CASGCReadAhead, MissReadsInlineOnTheCallersOperation)
{
    ReadAheadRig rig;
    rig.put("k2", "two");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    const auto got = reads.takeRead("k2");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "two");
    EXPECT_EQ(rig.backend->getCount("k2"), 1u);
}

TEST(CASGCReadAhead, AbsentKeyIsNulloptHintedOrNot)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.hintRead("absent-hinted");
    EXPECT_FALSE(reads.takeRead("absent-hinted").has_value());
    EXPECT_FALSE(reads.takeRead("absent-inline").has_value());
}

TEST(CASGCReadAhead, DuplicateHintIsOneRequest)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.hintRead("k1");
    reads.hintRead("k1");
    EXPECT_EQ(reads.pending(), 1u);
    ASSERT_TRUE(reads.takeRead("k1").has_value());
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
}

TEST(CASGCReadAhead, WorkerExceptionRethrowsAtTheTakeSiteAndDoesNotPoisonTheKey)
{
    ReadAheadRig rig;
    rig.put("k3", "three");
    /// A non-Poco exception is classified as not retryable, so the engine throws it on the first attempt.
    rig.backend->failNextReadWith("k3", std::make_exception_ptr(std::runtime_error("injected read fault")));
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.hintRead("k3");
    EXPECT_THROW(reads.takeRead("k3"), std::runtime_error);
    EXPECT_EQ(reads.pending(), 0u);
    const auto again = reads.takeRead("k3");   /// the fault was consumed; an inline read now succeeds
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->bytes, "three");
}

TEST(CASGCReadAhead, ConcurrencyOneNeverHints)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 1);
    EXPECT_EQ(reads.window(), 0u);
    reads.hintRead("k1");
    reads.hintHead("k1");
    EXPECT_EQ(reads.pending(), 0u);
    ASSERT_TRUE(reads.takeRead("k1").has_value());
    ASSERT_TRUE(reads.takeHead("k1").has_value());
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, DestructorWaitsForOutstandingRequests)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    {
        GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
        reads.hintRead("k1");
        reads.hintHead("k1");
    }
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, HeadHitCarriesSizeAndAbsentIsNullopt)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.hintHead("k1");
    const auto meta = reads.takeHead("k1");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->size, 3u);
    EXPECT_FALSE(reads.takeHead("absent").has_value());
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, WindowIsFourTimesConcurrency)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 8);
    EXPECT_EQ(reads.window(), 32u);
}
```

If `CountingBackend` or `openRequestsForTest` live under a different namespace than `DB::Cas::tests`, use the namespace `src/Disks/tests/cas_test_helpers.h` declares them in (check the `namespace` line above `class CountingBackend`); do not copy the class.

- [ ] **Step 2: Run to verify the tests fail to compile**

Run the build command shape with `<task>` = `task2a`. Expected: `NINJA_EXIT=1`, first error names `CasGcReadAhead.h`.

- [ ] **Step 3: Write the header**

`Gc/CasGcReadAhead.h`:

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Common/ThreadPool.h>

#include <future>
#include <memory>
#include <optional>
#include <unordered_map>

namespace DB::Cas
{

/// Read-ahead in front of ONE admitted operation. A caller HINTS keys the sequential code will read
/// next; workers fetch them on `pool`, each through an operation resumed under the SAME admitted
/// generation as `op` (no liveness -- exactly the fold's own admission); a TAKE returns the fetched
/// result, rethrows the worker's exception, or -- for a key nobody hinted -- performs the request
/// inline on `op`. This is a cache of RESULTS, never of decisions: every decode, counter and event
/// stays at the take site, so a round at concurrency 1 (nothing is ever hinted) and a round at 16
/// read the same keys in the same order and decide the same way; only WHEN the bytes were fetched
/// moves.
///
/// Why a result may be fetched early: the objects the fold reads are write-once (a present body is
/// the same body later), an absent position at or below a checkpoint's `committed_through` was
/// durable before the round began (it is a gap whenever it is read), and a manifest body still being
/// uploaded when the early read lands yields the same hold a slightly earlier sequential read yields
/// today. Nothing is hinted above `committed_through`, so no request is issued that the sequential
/// walk would not issue.
///
/// Memory is the CALLER's to bound: `pending` counts hinted-but-untaken slots and `window` is how
/// many a hinting site keeps in flight. A key hinted twice is one request. Results never taken are
/// awaited by the destructor and counted as wasted. Only the owning thread touches the maps; a
/// worker touches only its own slot.
class GcReadAhead
{
public:
    GcReadAhead(CasOperation & op_, CasRequests & requests_, ThreadPool & pool_, size_t concurrency_);
    ~GcReadAhead();

    GcReadAhead(const GcReadAhead &) = delete;
    GcReadAhead & operator=(const GcReadAhead &) = delete;

    void hintRead(const String & key);
    void hintHead(const String & key);

    std::optional<Object> takeRead(const String & key);
    std::optional<Meta> takeHead(const String & key);

    size_t pending() const { return reads.size() + heads.size(); }
    size_t window() const { return concurrency <= 1 ? 0 : 4 * concurrency; }

private:
    template <typename T>
    struct Slot
    {
        std::promise<std::optional<T>> promise;
        std::future<std::optional<T>> future;
        Slot() : future(promise.get_future()) {}
    };

    template <typename T>
    using Slots = std::unordered_map<String, std::shared_ptr<Slot<T>>>;

    template <typename T, typename Request>
    void hint(Slots<T> & slots, const String & key, Request && request);

    template <typename T, typename Inline>
    std::optional<T> take(Slots<T> & slots, const String & key, Inline && inline_request);

    CasOperation & op;
    CasRequests & requests;
    ThreadPool & pool;
    const size_t concurrency;
    const uint64_t generation;

    Slots<Object> reads;
    Slots<Meta> heads;
};

}
```

- [ ] **Step 4: Write the implementation**

`Gc/CasGcReadAhead.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
    extern const Event CASGCReadAheadHit;
    extern const Event CASGCReadAheadMiss;
    extern const Event CASGCReadAheadWasted;
}

namespace DB::Cas
{

GcReadAhead::GcReadAhead(CasOperation & op_, CasRequests & requests_, ThreadPool & pool_, size_t concurrency_)
    : op(op_), requests(requests_), pool(pool_), concurrency(concurrency_), generation(op_.generation())
{
}

GcReadAhead::~GcReadAhead()
{
    /// A worker holds its slot by value and touches `requests`, which outlives this object; waiting
    /// here is what keeps every worker inside the round that issued it. `wait`, not `get`: an
    /// exception nobody took is dropped with the result.
    size_t wasted = 0;
    for (auto & [key, slot] : reads)
    {
        slot->future.wait();
        ++wasted;
    }
    for (auto & [key, slot] : heads)
    {
        slot->future.wait();
        ++wasted;
    }
    if (wasted != 0)
        ProfileEvents::increment(ProfileEvents::CASGCReadAheadWasted, wasted);
}

template <typename T, typename Request>
void GcReadAhead::hint(Slots<T> & slots, const String & key, Request && request)
{
    if (concurrency <= 1 || slots.contains(key))
        return;
    auto slot = std::make_shared<Slot<T>>();
    slots.emplace(key, slot);
    try
    {
        pool.scheduleOrThrowOnError([slot, key, &requests_ref = requests, gen = generation, request]
        {
            try
            {
                CasOperation worker = requests_ref.resume(gen);
                slot->promise.set_value(request(worker, key));
            }
            catch (...)
            {
                slot->promise.set_exception(std::current_exception());
            }
        });
    }
    catch (...)
    {
        /// The slot's promise would never be satisfied; a later take would wait forever on it.
        slots.erase(key);
        throw;
    }
}

template <typename T, typename Inline>
std::optional<T> GcReadAhead::take(Slots<T> & slots, const String & key, Inline && inline_request)
{
    const auto it = slots.find(key);
    if (it == slots.end())
    {
        ProfileEvents::increment(ProfileEvents::CASGCReadAheadMiss);
        return inline_request(op, key);
    }
    std::shared_ptr<Slot<T>> slot = std::move(it->second);
    slots.erase(it);
    ProfileEvents::increment(ProfileEvents::CASGCReadAheadHit);
    return slot->future.get();   /// rethrows the worker's exception at the site that would have read inline
}

void GcReadAhead::hintRead(const String & key)
{
    hint<Object>(reads, key, [](CasOperation & worker, const String & k) { return worker.read(k, Retry::standard()); });
}

void GcReadAhead::hintHead(const String & key)
{
    hint<Meta>(heads, key, [](CasOperation & worker, const String & k) { return worker.head(k, Retry::standard()); });
}

std::optional<Object> GcReadAhead::takeRead(const String & key)
{
    return take<Object>(reads, key, [](CasOperation & inline_op, const String & k) { return inline_op.read(k, Retry::standard()); });
}

std::optional<Meta> GcReadAhead::takeHead(const String & key)
{
    return take<Meta>(heads, key, [](CasOperation & inline_op, const String & k) { return inline_op.head(k, Retry::standard()); });
}

}
```

`CasOperation` is move-only and `resume` returns one by value; the worker's operation lives for the one request. If the compiler reports `Retry::standard()` as not found, `Retry` lives in `Backend/CasRetry.h` under `DB::Cas` — that include is already there.

- [ ] **Step 5: Build and run the suite**

Build with `<task>` = `task2b`; expected `NINJA_EXIT=0`. Then run the test command shape with filter `CASGCReadAhead.*` and `<task>` = `task2`. Subagent returns `GTEST_EXIT=` and the PASSED/FAILED lines. Expected: 9 tests pass.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g && git status --short | grep -v '^??' && git diff --cached --stat && \
git commit -m "ca-gc: GcReadAhead, a key-addressed read-ahead in front of one admitted operation

Hints fetch on a bounded pool under the fold's own admitted generation; takes return the result at
the site that would have read inline, rethrow the worker's exception there, or read inline for a key
nobody hinted. Decisions never move; only the fetch does.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.cpp \
  src/Disks/tests/gtest_cas_gc_read_ahead.cpp && git branch --show-current
```

---

### Task 3: Wire read-ahead into intake (A1–A4) with the fold-level tests {#task-3}

**Files:**
- Modify: `Gc/CasGc.h:748` (`readCheckpointWitnesses`), `:788` (`foldManifestEdges`), `:953` (members)
- Modify: `Gc/CasGc.cpp:301-336` (constructor), `:1183-1195` (`foldManifestEdges`), `:1265-1330` (`readCheckpointWitnesses`), `:1561` (`fold`'s `op`), `:1913` (witness call), `:2034` (walk loop), `:2322` (record read), `:2412-2418` (edges), `:3929` (rebuild witness call), `:4099`, `:4115`, `:4187` (rebuild `foldManifestEdges` calls)
- Test: `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` (append)

**Interfaces:**
- Consumes: `GcReadAhead` (Task 2), `PoolConfig::gc_read_concurrency` (Task 1).
- Produces: `Gc::readCheckpointWitnesses(GcReadAhead & reads, const std::map<String, RefTableListing> & ref_tables, const CasRefCatalog::Snapshot & catalog_cut)`; `Gc::foldManifestEdges(GcReadAhead & reads, const ManifestId & id, int sign, std::vector<BlobDelta> & deltas, std::map<ManifestId, Etag> & mf_cleanup, uint32_t txn_ordinal)`; `Gc::read_pool` (`std::unique_ptr<ThreadPool>`).

- [ ] **Step 1: Write the failing fold-level tests**

Append to `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` (add the includes at the top of the file):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>

#include <atomic>
#include <map>
#include <thread>

using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;
using Rec = DB::Cas::GcRoundLogRecord;

namespace
{

const UInt128 kGc = u128Of("gc-read-ahead");

/// Same shape as `publishPart` in gtest_cas_gc_log.cpp: one blob per part, payload chosen by the caller
/// so that distinct parts never share a blob.
ManifestId publishPart(const PoolPtr & s, const String & ns, const String & ref, const String & payload)
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);

    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(payload))};
    e.blob_size = payload.size();

    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}

/// Three namespaces: `wide` has a backlog of 60 ref logs (wider than any window this file uses),
/// `quiet` has one part and no drops (its frontier is proven by the checkpoint ceiling, nothing is
/// read), `gone` is emptied entirely.
void populate(const PoolPtr & store)
{
    for (int i = 0; i < 40; ++i)
        publishPart(store, "srv1/wide", fmt::format("part_{}", i), fmt::format("wide-payload-{}", i));
    for (int i = 0; i < 20; ++i)
        store->dropRef(RootNamespace{"srv1/wide"}, fmt::format("part_{}", i));
    publishPart(store, "srv1/quiet", "only", "quiet-payload");
    publishPart(store, "srv1/gone", "a", "gone-payload-a");
    publishPart(store, "srv1/gone", "b", "gone-payload-b");
    store->dropRef(RootNamespace{"srv1/gone"}, "a");
    store->dropRef(RootNamespace{"srv1/gone"}, "b");
    store->renewWatermarkOnce();
}

struct FoldRun
{
    std::vector<String> seals;                        /// fold seal bytes after each round
    std::vector<std::map<String, UInt64>> intake;     /// `fold_ref_intake` phase metrics per round
    std::vector<std::map<String, UInt64>> reduce;     /// `fold_reduce` phase metrics per round
    std::map<String, uint64_t> gets;                  /// key -> GET count over the run
    std::map<String, uint64_t> heads;                 /// key -> HEAD count over the run
    std::vector<UInt64> condemned;                    /// `entries_condemned` per Finish row
};

void runFolds(uint64_t concurrency, size_t rounds, FoldRun & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0,
                   .gc_read_concurrency = concurrency});
    populate(store);

    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca",
                                  [&](const Rec & r) { rows.push_back(r); });
    sched.gc().setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            out.intake.push_back(rec.metrics);
        if (rec.phase == "fold_reduce")
            out.reduce.push_back(rec.metrics);
    });

    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    for (size_t round = 0; round < rounds; ++round)
    {
        sched.runOneRoundNow(Rec::Trigger::Manual);
        store->renewWatermarkOnce();
        ASSERT_EQ(rows.back().event_type, Rec::EventType::Finish);
        ASSERT_TRUE(rows.back().error.empty()) << rows.back().error;
        out.condemned.push_back(rows.back().entries_condemned);
        const GcState st = decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes);
        out.seals.push_back(op.read(layout.foldSealKey(st.snap_generation, st.snap_attempt), Retry::once())->bytes);
    }
    for (const String & key : backend->touchedKeys())
    {
        if (backend->getCount(key) != 0)
            out.gets[key] = backend->getCount(key);
        if (backend->headCount(key) != 0)
            out.heads[key] = backend->headCount(key);
    }
}

}

TEST(CASGCReadAhead, FoldIsIdenticalAtConcurrencyOneAndEight)
{
    FoldRun one;
    FoldRun eight;
    runFolds(1, 3, one);
    runFolds(8, 3, eight);
    ASSERT_EQ(one.seals.size(), 3u);
    ASSERT_EQ(eight.seals.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(one.seals[i], eight.seals[i]) << "fold seal bytes differ at round " << i;
    }
    EXPECT_EQ(one.intake, eight.intake);
    EXPECT_EQ(one.reduce, eight.reduce);
    EXPECT_EQ(one.condemned, eight.condemned);
    /// Every namespace here is healthy, so no hint is ever wasted: the same keys are GET the same
    /// number of times. (HEADs are compared in Task 4's test.)
    EXPECT_EQ(one.gets, eight.gets);
    ASSERT_FALSE(one.intake.empty());
    EXPECT_GE(one.intake[0].at("logs_applied"), 60u) << "the wide namespace must exercise the lookahead";
}

namespace
{

/// Throws on the first read issued from a thread other than `owner` once armed: exactly a read-ahead
/// worker's request, never the round thread's own.
class WorkerReadFaultBackend : public CountingBackend
{
public:
    void armAgainstThreadsOtherThan(std::thread::id owner_)
    {
        owner = owner_;
        armed = true;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (armed && std::this_thread::get_id() != owner)
        {
            armed = false;
            throw std::runtime_error("injected worker read fault");
        }
        return CountingBackend::read(key, access);
    }

private:
    std::atomic<bool> armed{false};
    std::thread::id owner;
};

}

TEST(CASGCReadAhead, WorkerReadFaultFailsTheRoundAndTheNextRoundRecovers)
{
    auto backend = std::make_shared<WorkerReadFaultBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0,
                   .gc_read_concurrency = 8});
    populate(store);

    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca",
                                  [&](const Rec & r) { rows.push_back(r); });

    backend->armAgainstThreadsOtherThan(std::this_thread::get_id());
    sched.runOneRoundNow(Rec::Trigger::Manual);
    ASSERT_EQ(rows.back().event_type, Rec::EventType::Finish);
    EXPECT_NE(rows.back().error.find("injected worker read fault"), String::npos) << rows.back().error;

    store->renewWatermarkOnce();
    sched.runOneRoundNow(Rec::Trigger::Manual);
    ASSERT_EQ(rows.back().event_type, Rec::EventType::Finish);
    EXPECT_TRUE(rows.back().error.empty()) << rows.back().error;
}
```

If `CasGcScheduler` exposes its `Gc` under a name other than `gc()`, use that accessor (grep `Gc &` in `Gc/CasGcScheduler.h`); if it exposes none, add `Gc & gc() { return *gc_; }` (matching the member's real name) to the scheduler in this task and name it in the commit message. If `Rec::error` is not the field carrying the failure text, use the one `ContentAddressedGarbageCollectionLog.cpp` maps to the `error` column.

- [ ] **Step 2: Run to verify the new tests fail**

Build (`<task>` = `task3a`) — expected `NINJA_EXIT=1` if the scheduler accessor is missing, otherwise `NINJA_EXIT=0`; then run filter `CASGCReadAhead.*` (`<task>` = `task3a`). Expected: `WorkerReadFaultFailsTheRoundAndTheNextRoundRecovers` fails (no worker ever reads, so no fault fires and the first round succeeds) and `FoldIsIdenticalAtConcurrencyOneAndEight` passes trivially (both runs are sequential). Both outcomes confirm the tests observe the wiring.

- [ ] **Step 3: Add the pool to `Gc`**

`Gc/CasGc.h`: add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>` next to the `CasGcMetaWriter.h` include, and after the `meta_writer` member:

```cpp
    /// The fold's read-ahead pool, sized by `gc_read_concurrency`. A `unique_ptr` for the same reason
    /// as `meta_writer`: the size is read from `store->poolConfig()` after the constructor has
    /// validated `store`.
    std::unique_ptr<ThreadPool> read_pool;
```

`Gc/CasGc.cpp`: if the file does not already declare them, add near the top:

```cpp
namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}
```

and in the constructor body, right after `meta_writer = std::make_unique<GcMetaWriter>(...)`:

```cpp
    /// Unbounded queue: the hinting sites bound what is in flight through `GcReadAhead::window`, so a
    /// bounded queue would only make `hintRead` block the round thread instead.
    const size_t read_concurrency = std::max<size_t>(1, store->poolConfig().gc_read_concurrency);
    read_pool = std::make_unique<ThreadPool>(
        CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
        /*max_threads*/ read_concurrency, /*max_free_threads*/ read_concurrency, /*queue_size*/ 0);
```

- [ ] **Step 4: Construct the read-ahead in `fold` and change the two member signatures**

In `Gc::fold`, right after `CasOperation op = store->openRequests().admit();` (line 1561):

```cpp
    GcReadAhead reads(op, store->openRequests(), *read_pool, store->poolConfig().gc_read_concurrency);
```

`Gc/CasGc.h`: change the two declarations to

```cpp
    CheckpointWitnesses readCheckpointWitnesses(GcReadAhead & reads, const std::map<String, RefTableListing> & ref_tables,
                                                const CasRefCatalog::Snapshot & catalog_cut);
```

```cpp
    bool foldManifestEdges(GcReadAhead & reads, const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Etag> & mf_cleanup, uint32_t txn_ordinal);
```

In `foldManifestEdges` (line 1183) change the parameter and the one read:

```cpp
bool Gc::foldManifestEdges(GcReadAhead & reads, const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Etag> & mf_cleanup, uint32_t txn_ordinal)
```

```cpp
    const auto got = reads.takeRead(key);
```

(The comment above that read stays; append one sentence to it: "The bytes may have been fetched ahead by `GcReadAhead` when the log that named this edge was decoded; the absence signal and the decode are still read here.")

Update the call at line 1913 to `readCheckpointWitnesses(reads, ref_tables, catalog_snapshot)` and the call at 2418 to `foldManifestEdges(reads, edge.manifest_id, ...)`.

For the rebuild: the call at line 3929 and the three `foldManifestEdges` calls at 4099, 4115 and 4187 sit in functions that each admit their own `CasOperation op` (grep upward from each call for `CasOperation op = store->openRequests().admit();`). Right after each such `op`, add

```cpp
    GcReadAhead reads(op, store->openRequests(), *read_pool, store->poolConfig().gc_read_concurrency);
```

and pass `reads` in place of `op`. The rebuild hints nothing in this plan; its takes are inline reads, request for request what it does today.

- [ ] **Step 5: A1 — checkpoint witnesses through the read-ahead**

Rewrite the loop of `readCheckpointWitnesses` (lines 1277–1330). Delete its `CasOperation op = store->openRequests().admit();`. Replace the single loop with a key pass and a take pass:

```cpp
    struct WitnessKey
    {
        String ns;
        String ckpt_key;
    };
    std::vector<WitnessKey> keys;
    keys.reserve(witness_namespaces.size());
    for (const String & ns_str : witness_namespaces)
    {
        const RootNamespace ns{ns_str};
        /// Review C3: use the SAME complete catalog cut the round's walk resolved, never an independent
        /// catalog re-read. A namespace absent from the cut, or present only as a non-walkable
        /// `Creating` row, has no admitted witness key to read this round.
        const auto entry_it = std::lower_bound(
            catalog_cut.catalog.entries.begin(), catalog_cut.catalog.entries.end(), ns,
            [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
        if (entry_it == catalog_cut.catalog.entries.end() || entry_it->ns != ns
            || (entry_it->state != NsState::Live && entry_it->state != NsState::Removing))
            continue;
        keys.push_back({ns_str, layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(entry_it->ns, entry_it->incarnation))});
    }

    /// The keys are known before any is read, so they are fetched `window` deep ahead of the take
    /// order below; the take order is the `std::set` order it always was.
    size_t next_hint = 0;
    const auto topUp = [&]
    {
        while (next_hint < keys.size() && reads.pending() < reads.window())
            reads.hintRead(keys[next_hint++].ckpt_key);
    };

    CheckpointWitnesses out;
    for (const WitnessKey & wk : keys)
    {
        const String & ns_str = wk.ns;
        const String & ckpt_key = wk.ckpt_key;
        topUp();
        const std::optional<Object> got = reads.takeRead(ckpt_key);
        /// ... the existing body from "ABSENT IS NORMAL AND IS NOT A WITNESS" to the end of the loop,
        /// unchanged: `if (!got) continue;`, the split decode with its per-namespace catch, the three
        /// `out.*.emplace` calls ...
    }
    return out;
```

Keep the existing comment block that explains the GET/decode split; it is still true.

- [ ] **Step 6: A2 — each namespace's first walk position**

Immediately before `for (const WalkTarget & target : walk_targets)` (line 2034), after the classification loop that fills `walk_targets`:

```cpp
    /// Read-ahead of each namespace's FIRST walk position, in walk order, kept `window` deep. The key
    /// is recomputed from exactly the inputs the walk below uses -- the parent cursor, the catalog
    /// entry and the checkpoint grounding -- and only when the walk WILL read it: a position above the
    /// checkpoint's `committed_through` is refused by the ceiling test before any read, so hinting it
    /// would issue a request the sequential walk never makes. A namespace the grounding refuses is
    /// held without reading and is skipped here for the same reason.
    std::vector<String> first_keys;
    first_keys.reserve(walk_targets.size());
    for (const WalkTarget & target : walk_targets)
    {
        if (checkpoints.undecodable.contains(target.ns))
            continue;
        const RootNamespace ns{target.ns};
        const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, target.life_id);
        const auto cursor_it = parent_ref_lives.find(target.life_id);
        const RefTxnId cursor = cursor_it != parent_ref_lives.end()
            ? cursor_it->second.coverage.last_folded_ref_id : RefTxnId{};
        const auto catalog_entry_it = std::lower_bound(
            catalog_snapshot.catalog.entries.begin(), catalog_snapshot.catalog.entries.end(), ns,
            [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
        std::optional<RefCkpt> checkpoint;
        if (const auto checkpoint_it = checkpoints.recovery_checkpoints.find(target.ns);
            checkpoint_it != checkpoints.recovery_checkpoints.end())
            checkpoint = checkpoint_it->second;
        std::optional<RecoveryGrounding> grounding;
        try
        {
            grounding = chooseRecoveryGrounding(std::optional<CatalogEntry>{*catalog_entry_it}, checkpoint);
        }
        catch (const Exception &)
        {
            continue;
        }
        if (!grounding->committed_through)
            continue;
        const RefTxnId expected = cursor != RefTxnId{}
            ? RefTxnId{cursor.writer_epoch, cursor.ref_sequence + 1}
            : RefTxnId{*checkpoint->life_epoch, 1};
        if (*grounding->committed_through < expected)
            continue;
        first_keys.push_back(layout.refLogKey(life, expected));
    }
    size_t next_first_key = 0;
    const auto topUpFirstKeys = [&]
    {
        while (next_first_key < first_keys.size() && reads.pending() < reads.window())
            reads.hintRead(first_keys[next_first_key++]);
    };
```

and as the first statement inside the walk loop body (before `const String & ns_str = target.ns;`):

```cpp
        topUpFirstKeys();
```

The `*checkpoint->life_epoch` dereference mirrors line 2233, which the grounding already guarantees; do not add a guard the walk does not have.

- [ ] **Step 7: A3 — same-epoch lookahead inside a namespace**

Replace line 2322, `const auto got = op.read(layout.refLogKey(life, *expected), Retry::standard());`, with:

```cpp
            /// Lookahead over the arithmetic positions of THIS epoch, never past the ceiling the test
            /// above enforces, so every hinted position is one the walk reads if nothing holds it first.
            /// A hold or an epoch crossing leaves at most `window` fetched results untaken.
            for (uint64_t k = 1; k <= reads.window(); ++k)
            {
                const RefTxnId ahead{expected->writer_epoch, expected->ref_sequence + k};
                if (*grounding->committed_through < ahead)
                    break;
                reads.hintRead(layout.refLogKey(life, ahead));
            }
            const auto got = reads.takeRead(layout.refLogKey(life, *expected));
```

- [ ] **Step 8: A4 — the manifest edges of a decoded log**

After the `try { txn = decodeRefLogTxn(...); edges = manifestEdgesOfTxn(txn); } catch (...) { ...; break; }` block (ends around line 2404) and before `std::vector<BlobDelta> log_deltas;`, insert:

```cpp
            /// Every edge of this log names its manifest key now; fetch them together and fold them in
            /// edge order below. A log with one edge gains nothing; a merge or a mutation log with
            /// dozens turns dozens of serial round trips into one.
            for (const RefManifestEdge & edge : edges)
                reads.hintRead(layout.manifestKey(edge.manifest_id));
```

- [ ] **Step 9: Build, run the suite, run the gate**

Build (`<task>` = `task3b`), expected `NINJA_EXIT=0`. Run filter `CASGCReadAhead.*` (`<task>` = `task3b`): expected all pass, including `WorkerReadFaultFailsTheRoundAndTheNextRoundRecovers` (a worker now reads). Then run the full gate `CAS*` (`<task>` = `task3gate`): expected `GTEST_EXIT=0` and a PASSED count of at least the Task 0 floor plus the new tests. Any red: root-cause before proceeding; the frontier-gate and orphan-nomination suites read `fold_ref_intake` metrics and must be unchanged by construction.

- [ ] **Step 10: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g && git status --short | grep -v '^??' && git diff --cached --stat && \
git commit -m "ca-gc: fold_ref_intake fetches ahead -- checkpoints, first positions, same-epoch lookahead, manifest edges

Four hinting sites in front of the walk's reads; the walk itself, its order, its counters and its
holds are unchanged, and concurrency 1 is the sequential round request for request. A worker's read
fault fails the round at the take site and the next round recovers.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h \
  src/Disks/tests/gtest_cas_gc_read_ahead.cpp && git branch --show-current
```

(Drop `CasGcScheduler.h` from the path list if Step 1 needed no accessor.)

---

### Task 4: R2 — HEAD read-ahead in `fold_reduce`, gated by two consults {#task-4}

**Files:**
- Create: `docs/superpowers/worklogs/2026-09-03-cas-gc-head-read-ahead-consult.md`
- Modify: `Gc/CasGc.cpp:1690-1760` (the two HEAD lambdas), `:3006-3025` (between `orphan_source_retirements` and the shard split), `:3041`, `:3083` (before each reducer call)
- Test: `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` (append)

**Interfaces:**
- Consumes: `GcReadAhead::hintHead`/`takeHead`, `blobShard(ref, shards)` (already used at line 3070), `BlobDelta{ref, source_id, remove, txn_ordinal}`, `BlobSourceRetirement{ref, source_id}`, `BlobRef::operator<=>`.
- Produces: nothing new outside `fold`.

- [ ] **Step 1: Two independent consults on the R2 argument**

Dispatch two `ca-arch` agents (opus; at most these two at once), each with the same brief and neither told of the other. Brief: the text of [R2: HEAD read-ahead in reduce, and its argument](#design-r2) plus the `head_blob`, `peek_head`, `confirm_condemned_marker` lambdas (`Gc/CasGc.cpp:1690-1800`) and `closeBlob` (`Gc/CasBlobInDegree.cpp:470-540`); the question: "Is issuing the HEAD for a zero-transition candidate earlier within `fold_reduce` — after intake has frozen the cut, before the merge reaches the blob — safe under the pool's protocol (HEAD-before-PUT writers, exact-token deletes, the condemn marker written at the take site)? Name any interleaving whose outcome is not one of the four listed." Each agent writes its verdict as a section of the worklog file above (`## Consult A {#consult-a}` / `## Consult B {#consult-b}`, verdict on the first line: `SAFE` or `UNSAFE: <interleaving>`).

Decision rule: both `SAFE` → continue with Step 2. Anything else → skip Steps 2–6, add the BACKLOG entry `[gc-reduce-head-read-ahead]` in Task 5 quoting the refuting interleaving verbatim, and commit only the worklog. The consult's own reasoning must not restate the design's claims; the memory rule is two consults that try to refute each other.

- [ ] **Step 2: Write the failing R2 test**

Append to `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`:

```cpp
TEST(CASGCReadAhead, ReduceCondemnsTheSameBlobsWithTheSameHeadsAtConcurrencyOneAndEight)
{
    /// `populate` drops parts whose blobs are unique to them, so every blob with a removal reaches
    /// zero and no blob with a surviving add has a removal: the hinted HEAD set equals the set the
    /// merge takes, and the per-key HEAD counts must match exactly.
    FoldRun one;
    FoldRun eight;
    runFolds(1, 3, one);
    runFolds(8, 3, eight);
    EXPECT_EQ(one.condemned, eight.condemned);
    EXPECT_EQ(one.heads, eight.heads);
    ASSERT_FALSE(one.reduce.empty());
    EXPECT_GT(one.reduce[0].at("condemned"), 0u) << "the scenario must condemn in the first fold";
}
```

Run filter `CASGCReadAhead.ReduceCondemns*` after building (`<task>` = `task4a`): expected PASS already (both runs HEAD inline). Keep it: after Step 4 it proves the hinted HEADs replaced, not added to, the inline ones.

- [ ] **Step 3: Candidate set and top-up before the shard split**

Before the `head_blob` lambda (line 1690), declare the shared state the lambdas need:

```cpp
    /// HEAD read-ahead for `fold_reduce`. `head_candidates[shard]` is filled in that phase with the
    /// blobs the merge can close at in-degree zero, in the merge's own ascending key order; the two
    /// HEAD lambdas below top the hints up `window` deep before each take. Empty until the reduce
    /// phase, so a take before it is an inline HEAD.
    std::vector<std::vector<BlobRef>> head_candidates(state.gc_shards);
    size_t head_hint_shard = 0;
    size_t next_head_hint = 0;
    const auto topUpHeads = [&]
    {
        const std::vector<BlobRef> & shard_candidates = head_candidates[head_hint_shard];
        while (next_head_hint < shard_candidates.size() && reads.pending() < reads.window())
            reads.hintHead(layout.blobKey(shard_candidates[next_head_hint++]));
    };
```

In `head_blob`, replace `const std::optional<Meta> observed = op.head(layout.blobKey(ref), Retry::standard());` with

```cpp
        topUpHeads();
        const std::optional<Meta> observed = reads.takeHead(layout.blobKey(ref));
```

and in `peek_head`, replace `std::optional<Meta> hr = op.head(layout.blobKey(ref), Retry::standard());` with

```cpp
        topUpHeads();
        std::optional<Meta> hr = reads.takeHead(layout.blobKey(ref));
```

Then, in the reduce phase, after the block that fills `orphan_source_retirements` (ends before `if (state.gc_shards == 1)`, around line 3025) and before the shard split, insert:

```cpp
    /// The blobs the merge below can bring to in-degree zero. A blob closes at zero only if every
    /// edge it had is gone, which needs at least one removal this round (a `-1` delta or a source
    /// retirement) and no `+1` whose last verdict survives; this is that superset, per shard, in
    /// ascending `BlobRef` order -- the order `closeBlob` reaches them. A named blob that keeps other
    /// prior edges costs one HEAD the merge never takes (counted wasted); a candidate this set misses
    /// is HEADed inline (counted a miss). The HEAD is issued from this phase and never earlier: the
    /// cut is frozen before the first hint.
    {
        std::map<std::pair<BlobRef, UInt128>, bool> last_verdict_is_remove;
        for (const BlobDelta & d : deltas)
            last_verdict_is_remove[{d.ref, d.source_id}] = d.remove;
        for (const BlobSourceRetirement & r : orphan_source_retirements)
            last_verdict_is_remove[{r.ref, r.source_id}] = true;
        std::set<BlobRef> removed;
        std::set<BlobRef> surviving_add;
        for (const auto & [edge, remove] : last_verdict_is_remove)
            (remove ? removed : surviving_add).insert(edge.first);
        for (const BlobRef & ref : removed)
            if (!surviving_add.contains(ref))
                head_candidates[blobShard(ref, state.gc_shards)].push_back(ref);
    }
```

Before the single-shard reducer call (line 3041) add `head_hint_shard = 0; next_head_hint = 0;`; inside the sharded loop, before the call at 3083, add `head_hint_shard = shard; next_head_hint = 0;`.

`std::set<BlobRef>` iterates in `operator<=>` order, `(algo, digest)`, which is the order `SourceEdgeKeyCodec::key` sorts by (see the comparator comment at the top of `foldDeltasIntoGeneration`); if that ever diverged, hits would become misses, never wrong answers.

- [ ] **Step 4: Build, run the suite and the gate**

Build (`<task>` = `task4b`), expected `NINJA_EXIT=0`. Run `CASGCReadAhead.*` (`<task>` = `task4b`): all pass; the R2 test now runs with hinted HEADs at concurrency 8 and its per-key HEAD equality is the proof no HEAD was duplicated. Run the gate `CAS*` (`<task>` = `task4gate`): `GTEST_EXIT=0`, count at or above the Task 3 count plus one.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g && git status --short | grep -v '^??' && git diff --cached --stat && \
git commit -m "ca-gc: fold_reduce fetches zero-candidate HEADs ahead of the merge

The superset of blobs that can close at in-degree zero -- at least one removal this round and no
surviving add -- is hinted per shard in the merge's order; head_blob and peek_head take from it. The
HEAD is issued after the cut is frozen and only moves earlier within the phase; two independent
consults found no interleaving outside the four the design maps onto existing paths.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/tests/gtest_cas_gc_read_ahead.cpp \
  docs/superpowers/worklogs/2026-09-03-cas-gc-head-read-ahead-consult.md && git branch --show-current
```

- [ ] **Step 6: If the consult refused** — commit only the worklog with message `ca-gc: R2 head read-ahead consult -- refused, see worklog` and the same trailers; Task 5 records the entry.

---

### Task 5: BACKLOG bookkeeping and a comment-level review {#task-5}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG.md:633-651` (`[gc-fold-intake-readbuffer-head]`) and the end of its open-items section.

- [ ] **Step 1: Close the paired-HEAD entry**

Change the header at line 633 to

```markdown
## `[gc-fold-intake-readbuffer-head]` ✅ CLOSED by the request-contract read path (`e272e18f02c`, 2026-09-03) {#gc-fold-intake-readbuffer-head}
```

and prepend one paragraph to its body:

```markdown
**Closed.** `ObjectStorageBackend::read` no longer HEADs before it GETs: since `e272e18f02c` it goes
through `readSmallObjectAndGetObjectMetadata`, one `GetObject` whose response carries the incarnation.
The 2026-09-01 soak (`utils/ca-soak/scenarios/runs/20260901T053522_S02_seed20260831`) still shows the
1:1 pairing because its binary predates that commit; the next soak against a post-`e272e18f02c` build
is the confirmation. The rest of this entry is kept as the record of how the pairing was found.
```

- [ ] **Step 2: Add two entries**

At the end of the open GC items, following the file's `## `[id]` title {#anchor}` shape:

```markdown
## `[gc-reduce-confirm-marker-read-ahead]` `confirm_condemned_marker`'s `loadMeta` GET is still inline {#gc-reduce-confirm-marker-read-ahead}

The fold's read-ahead (`GcReadAhead`, `cas_gc_read_concurrency`) covers checkpoints, ref logs,
manifest edges and zero-candidate HEADs. The one remaining serial round trip in `fold_reduce` is the
graduation gate's `loadMeta` re-check, issued from `settleEntry` for a carried condemned entry that
has no in-process confirmation — after a restart or a leadership change, that is every entry
graduating that round. Its candidates are known only from the prior run's condemned sentinel rows,
which the merge streams, so hinting them needs a lookahead on `PriorEdgeCursor` (peek `window` rows
ahead, hint the sentinels' meta keys) rather than a pre-pass over in-memory input. **Not sized:**
measure `CASGCReadAheadMiss` against `graduated` on a round following a restart before building it.

## `[gc-phase-rows-lose-worker-requests]` phase rows do not see requests made on worker pools {#gc-phase-rows-lose-worker-requests}

`GcPhaseTimer` diffs the round thread's ProfileEvents. Every request a `GcReadAhead` worker or a
`meta_pool` job performs lands on that worker's counters, so `DiskS3GetObject` / `DiskS3HeadObject`
on `fold_ref_intake` and `fold_reduce` now under-count by exactly the hinted requests, the way
`meta_pool_wait` already under-counts its writes. The CAS counters incremented at take sites
(`CASRefLogBodyGets`, `CASRefManifestBodyFoldGets`, `CASGCReadAheadHit`/`Miss`/`Wasted`) are on the
row and are the ones to read. The fix is attribution at the worker boundary (attach the workers to
the round's thread group, or sum per-pool deltas into the row), which is a `GcPhaseTimer` change and
not a read-ahead one.
```

If Task 4 refused, add a third entry `[gc-reduce-head-read-ahead]` quoting the consult's refuting interleaving and pointing at the worklog.

- [ ] **Step 3: Comment review of the diff**

Dispatch one `ca-review-lite` agent on `git -C /home/mfilimonov/workspace/ClickHouse/lane-g diff cas-gc-rebuild...cas-gc-fold-read-ahead` with the checklist: every changed comment still true after the change (the `foldManifestEdges` "ONE ROUND TRIP PER EDGE" comment, the `readCheckpointWitnesses` GET/decode split comment, the intake "one GET per record" phase comment at line 1889 — the last one should now say the GETs are issued ahead and taken in order); no plan/BACKLOG/session provenance in code comments; Allman braces; `f` not `f()` in prose. Fix what it finds, then rebuild (`<task>` = `task5`) and rerun `CASGCReadAhead.*`.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g && git status --short | grep -v '^??' && git diff --cached --stat && \
git commit -m "ca-backlog: close the paired-HEAD entry, record the two read-ahead follow-ups

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" -- docs/superpowers/cas/BACKLOG.md src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp && git branch --show-current
```

(Drop `CasGc.cpp` from the path list if Step 3 changed no comment.)

---

### Task 6: Measure {#task-6}

**Files:**
- Modify: `utils/ca-soak/configs/storage_conf_ch1.xml` and `storage_conf_ch2.xml` (one line each, only for the `A` run below; revert after)
- Create: `docs/superpowers/worklogs/2026-09-03-cas-gc-fold-read-ahead-measurement.md`

- [ ] **Step 1: Build the soak binary from the branch**

Follow `utils/ca-soak/README.md` for the image/binary build and the fresh-restart rule (`down -v` for a clean remount; host logs survive teardown). Two runs of the same scenario and seed, phase 3 `--duration` (a phase 1 `--ops` run finishes too fast to fold anything):

- Run `A`: `<cas_gc_read_concurrency>1</cas_gc_read_concurrency>` added under the disk's settings in both `storage_conf_ch*.xml`.
- Run `B`: the line removed (default 16).

- [ ] **Step 2: Extract the phase wall per round**

For each run's `raw/gc_log_ca-soak-ch1-1.tsv`:

```bash
clickhouse-local --query "
SELECT phase, count() AS n, round(sum(phase_duration_microseconds)/1e6, 1) AS wall_s,
       round(max(phase_duration_microseconds)/1e6, 1) AS max_s,
       sumMap(ProfileEvents)['CASGCReadAheadHit'] AS hits,
       sumMap(ProfileEvents)['CASGCReadAheadMiss'] AS misses,
       sumMap(ProfileEvents)['CASGCReadAheadWasted'] AS wasted
FROM file('<run>/raw/gc_log_ca-soak-ch1-1.tsv', TabSeparatedWithNames)
WHERE phase IN ('fold_ref_intake', 'fold_reduce')
GROUP BY phase ORDER BY phase"
```

- [ ] **Step 3: Record**

Write the worklog with frontmatter and anchored headers: the two commands, the two tables, the ratio per phase, and one sentence on what it does and does not show (RustFS answers in about 0.7 ms per request, so the ratio here is a lower bound on the GCS ratio; the GCS number needs a live run under `docs/superpowers/cas/BACKLOG/gcs.md`'s prerequisites and is recorded there when it exists). Add the run ids to `utils/ca-soak/scenarios/RUN_HISTORY.md` in its table format.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g && git status --short | grep -v '^??' && git diff --cached --stat && \
git commit -m "ca-soak: fold read-ahead measurement, concurrency 1 against 16

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" -- docs/superpowers/worklogs/2026-09-03-cas-gc-fold-read-ahead-measurement.md utils/ca-soak/scenarios/RUN_HISTORY.md && git branch --show-current
```

---

### Task 7: Merge back into `cas-gc-rebuild` {#task-7}

**Files:** none changed by hand.

- [ ] **Step 1: Confirm the branch is complete and green**

`git -C /home/mfilimonov/workspace/ClickHouse/lane-g log --oneline cas-gc-rebuild..cas-gc-fold-read-ahead` lists the Task 1–6 commits; the last gate log (`test_task4gate.log` or `test_task5.log`) shows `GTEST_EXIT=0`.

- [ ] **Step 2: Merge from the `master` worktree, which has `cas-gc-rebuild` checked out**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master && git status --short | grep -v '^??'; git branch --show-current && \
git merge --no-ff cas-gc-fold-read-ahead -m "Merge cas-gc-fold-read-ahead: GC fold read-ahead (cas_gc_read_concurrency)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExU35Ljr6yxMDCmKC4191s" && git log --oneline -1
```

Stop before merging if `git status` shows tracked modifications in `master` — another task may be mid-commit there; the merge waits.

- [ ] **Step 3: Return lane-g to its own branch**

`git -C /home/mfilimonov/workspace/ClickHouse/lane-g checkout feature/antalya-26.6/CAS-improvements` and repeat Task 0 Step 3 for submodules that moved. No push anywhere.

## Self-review {#self-review}

- Design coverage: A1 (Task 3 Step 5), A2 (Step 6), A3 (Step 7), A4 (Step 8), R2 (Task 4), setting and counters (Task 1), tests for determinism, worker fault, R2 equivalence (Tasks 3–4), reporting gap and R3 recorded (Task 5), measurement (Task 6), merge-back (Task 7).
- Names used across tasks: `GcReadAhead` (`hintRead`, `hintHead`, `takeRead`, `takeHead`, `pending`, `window`), `Gc::read_pool`, `PoolConfig::gc_read_concurrency`, `ProfileEvents::CASGCReadAheadHit/Miss/Wasted`, `readCheckpointWitnesses(reads, …)`, `foldManifestEdges(reads, …)` — consistent between Tasks 1–4.
- Conditional instructions that remain (scheduler accessor name, `Rec::error` field, `CurrentMetrics` externs already present, whether Task 4 proceeds) are each resolved by a named grep or by the consult's verdict, not by the implementer's taste.
