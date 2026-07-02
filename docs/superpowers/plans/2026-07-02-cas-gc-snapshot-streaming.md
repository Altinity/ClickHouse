# CAS GC Snapshot Streaming (T2) + Reference-Parent Runs (T0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Snapshot-run reads at O(block) resident memory (streaming reader over a true ranged/stream
backend seam) and zero run I/O for empty-delta gc-shards (seal refs to the parent generation's runs),
per `docs/superpowers/specs/2026-07-02-cas-gc-snapshot-streaming-design.md`.

**Architecture:** Bottom-up: ranged `get` → `getStream` seam → `RunFileReader` borrowed/streaming
modes → streaming prior cursor in the fold → preview consumers → T0 seal-ref resolution → ref-aware
retention + hand-off delete. Every task is independently green; the run FORMAT does not change.

**Tech Stack:** C++ (ClickHouse tree, Allman braces), gtest `unit_tests_dbms` (`InMemoryBackend`,
`CountingBackend`, `LocalObjectStorage` via `makeLocalObjectStorageForTest`), protobuf additive
fields only.

## Global Constraints

- Branch: `cas-gc-snapshot-streaming` off `cas-gc-ack-floor-fence` (create at Task 1; plain commits,
  no rebase/amend).
- Run format bytes unchanged; `RunFileWriter` untouched; output runs byte-identical
  (`putDeterministicArtifact` semantics preserved) — spec §"What deliberately does NOT change".
- `getStream` contract: WRITE-ONCE objects only; fail-closed `CORRUPTED_DATA` on short/truncated/
  CRC-failing reads — never a partial parse (spec §Backend seam, §Testing gate 6).
- Streaming open request profile: exactly `head` + tail ranged `get` + body `getStream`
  (3 requests; gate 4). `seek` = +1 ranged `get` per touched block.
- Resident-state proof is structural + asserted: the reader has NO whole-run member (the `full`
  string is deleted), `cur_block.size() <= kRunHardCapBlockSize`, and every ranged request length
  ≤ `kRunHardCapBlockSize + footer` (gate 3 via `CountingBackend` request-size recording).
- Build: `ninja -C build unit_tests_dbms > build/build_<task>.log 2>&1` (no `-j`); full
  `ninja -C build clickhouse` at Tasks 7–8 only. Tests into unique logs; analyze via subagent.
- Baseline: full `--gtest_filter='Cas*'` = 408 green before Task 1; must stay green after every task.
- `Core/` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`.

---

### Task 1: true ranged `get` in `ObjectStorageBackend`

**Files:**
- Modify: `Core/CasObjectStorageBackend.cpp` (`readObjectRanged`, ~line 269)
- Test: `src/Disks/tests/gtest_cas_backend.cpp`

**Interfaces:**
- Consumes: `IObjectStorage::readObject(StoredObject, ReadSettings, read_hint)`; the returned
  buffer is a `ReadBufferFromFileBase` (supports `seek`) for S3/Local object storages.
- Produces: `Backend::get(key, Range{offset, length})` reads ONLY the requested window from the
  object storage (plus stream-buffer granularity), same return values as today (clamping semantics
  preserved: `offset >= size` ⇒ empty; open-ended length ⇒ to EOF).

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_backend.cpp`; it already has a
  Local-object-storage backend fixture — reuse its construction):

```cpp
TEST(CasObjectStorageBackend, RangedGetReadsOnlyTheWindow)
{
    /// Construct the emulated backend exactly as the existing CasBackend tests in this file do
    /// (LocalObjectStorage via tests::makeLocalObjectStorageForTest + ObjectStorageBackend), then:
    const String payload = String(300000, 'a') + String(300000, 'b') + String(300000, 'c');
    backend->putIfAbsent("p/obj", payload);

    const auto mid = backend->get("p/obj", DB::Cas::Range{.offset = 300000, .length = 300000});
    ASSERT_TRUE(mid.has_value());
    EXPECT_EQ(mid->bytes, String(300000, 'b'));

    const auto tail = backend->get("p/obj", DB::Cas::Range{.offset = 600000, .length = std::nullopt});
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->bytes, String(300000, 'c'));

    const auto past = backend->get("p/obj", DB::Cas::Range{.offset = 1000000, .length = 10});
    ASSERT_TRUE(past.has_value());
    EXPECT_TRUE(past->bytes.empty());
}
```

  This asserts VALUES (InMemory-oracle parity). The only-the-window property is enforced in the
  implementation step below and cross-checked by Task 3's request-size gate.

- [ ] **Step 2: Run to verify state** — the values test may already PASS (substr fakes it). That is
  expected: this task's deliverable is the implementation change; keep the test as the behavioral
  pin. Run: `./build/src/unit_tests_dbms --gtest_filter='CasObjectStorageBackend.RangedGet*'`.

- [ ] **Step 3: Implement** — replace `readObjectRanged` in `Core/CasObjectStorageBackend.cpp`:

```cpp
/// Read `range` of the object at `path` as a TRUE ranged read: seek to the offset and bound the
/// read window (spec 2026-07-02 snapshot-streaming §Backend seam). Never read-whole-then-substr —
/// the snapshot runs this serves are GBs at scale and the caller's memory budget is O(block).
static String readObjectRanged(IObjectStorage & object_storage, const String & path, Range range)
{
    auto buf = object_storage.readObject(StoredObject(path), getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    if (range.whole())
    {
        readStringUntilEOF(content, *buf);
        return content;
    }
    if (range.length.has_value())
        buf->setReadUntilPosition(range.offset + *range.length);
    buf->seek(static_cast<off_t>(range.offset), SEEK_SET);
    readStringUntilEOF(content, *buf);
    /// Clamp exactly like the old substr path: a window past EOF yields the readable prefix
    /// (possibly empty) — the object storage read stops at EOF, so `content` is already clamped.
    return content;
}
```

  Note: `seek` past EOF on `ReadBufferFromFileBase` either yields an immediately-empty read or
  throws depending on the storage; verify against `LocalObjectStorage` in the test and, if it
  throws, guard with the object size from `getObjectMetadata` (fail-closed comment either way).

- [ ] **Step 4: Run** `--gtest_filter='CasObjectStorageBackend*:CasBackend*'` → PASS; full `Cas*`
  sweep → 408.

- [ ] **Step 5: Commit** — `git checkout -b cas-gc-snapshot-streaming` first, then
  `git add Core/CasObjectStorageBackend.cpp src/Disks/tests/gtest_cas_backend.cpp` and
  `git commit -m "CAS backend: true ranged get (seek + bounded window, no substr fake)"`.

---

### Task 2: `Backend::getStream` seam

**Files:**
- Modify: `Core/CasBackend.h` (interface + `GetStreamResult`), `Core/CasInMemoryBackend.{h,cpp}`,
  `Core/CasObjectStorageBackend.{h,cpp}`, `Core/CasInstrumentedBackend.{h,cpp}`
- Modify (test decorators — pure-virtual fix-ups, plain delegation): `src/Disks/tests/cas_test_helpers.h`
  (`CountingBackend` inherits `InMemoryBackend` — no change needed unless it overrides),
  `src/Disks/tests/gtest_cas_store.cpp` (`WriteCountingBackend`, `GetFailingBackend`),
  `src/Disks/tests/gtest_cas_mount.cpp` (the fence-out race decorator)
- Test: `src/Disks/tests/gtest_cas_backend.cpp`

**Interfaces:**
- Produces (exact, all later tasks consume):

```cpp
/// A forward-only read of a WRITE-ONCE object (runs, seals): nothing is materialized by the seam.
/// MUTABLE objects (root shards, gc/state, mounts) MUST keep using `get` — their bytes may change
/// under an open stream. `token` identifies the incarnation the stream reads, same as `get`.
struct GetStreamResult
{
    std::unique_ptr<ReadBuffer> stream;
    Token token;
};

virtual std::optional<GetStreamResult> getStream(const String & key, Range range = {}) = 0;   /// nullopt = absent
```

- [ ] **Step 1: Write the failing test**:

```cpp
TEST(CasBackendStream, StreamsBodyWindow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    backend->putIfAbsent("k", "0123456789");
    auto got = backend->getStream("k", DB::Cas::Range{.offset = 2, .length = 5});
    ASSERT_TRUE(got.has_value());
    String out;
    DB::readStringUntilEOF(out, *got->stream);
    EXPECT_EQ(out, "23456");
    EXPECT_FALSE(got->token.empty());
    EXPECT_FALSE(backend->getStream("absent").has_value());
}
```

- [ ] **Step 2: Run to verify it fails to compile** (no `getStream` member).

- [ ] **Step 3: Implement**:
  - `InMemoryBackend`: copy the (windowed) bytes into an owning buffer:
    `std::make_unique<ReadBufferFromOwnString>(sliced_copy)` (declare the owning-buffer type it
    already uses elsewhere or add `#include <IO/ReadBufferFromString.h>`); token = the entry's
    current token (same source as `get`).
  - `ObjectStorageBackend`: mirror the emulated-vs-native split the file uses for `get`:
    `readObject` + (range: `setReadUntilPosition`/`seek` exactly as Task 1) and RETURN the buffer
    instead of draining it; token = the same incarnation token `get` reports for the key (reuse the
    file's existing token-derivation for reads; for the emulated single-process mode it may
    delegate to the same helper `get` uses).
  - `InstrumentedBackend`: count + delegate (one new op counter following the file's per-op
    pattern).
  - Test decorators: `getStream` = `return inner->getStream(key, range);` (and the failing/fault
    decorators keep their fault key checks on `get` only — no behavior change).

- [ ] **Step 4: Run** `--gtest_filter='CasBackendStream*'` + full `Cas*` → 408 + 1.

- [ ] **Step 5: Commit** — `"CAS backend: getStream seam (forward-only, write-once objects)"`.

---

### Task 3: `RunFileReader` borrowed-memory + streaming modes

**Files:**
- Modify: `Core/CasRunFile.h`, `Core/CasRunFile.cpp`
- Modify (call-site ctor swaps): `Core/CasBlobInDegree.cpp` (3 sites), `Core/CasManifestCodec.cpp`
  (1 site — passes the decoded body as `std::string_view`, removing its copy)
- Test: `src/Disks/tests/gtest_cas_run_file.cpp` (12 existing ctor sites become borrowed-mode; new
  streaming cases)

**Interfaces:**
- Produces (exact):

```cpp
class RunFileReader
{
public:
    /// Borrowed-memory mode: zero-copy over caller-owned bytes (the caller must keep them alive
    /// for the reader's lifetime). Replaces the old copying ReadBuffer constructor.
    explicit RunFileReader(std::string_view bytes);

    /// Streaming mode: head + tail-footer ranged get + body getStream; resident state is the
    /// footer index + ONE current block (<= kRunHardCapBlockSize). Throws CORRUPTED_DATA on an
    /// absent key, truncated stream, or any CRC failure.
    RunFileReader(Backend & backend, const String & key);

    bool next(String & key, String & payload);   /// unchanged semantics
    void seek(std::string_view key);             /// unchanged semantics; streaming: ranged get per touched block
    RunKind kind() const;
    uint8_t keySchema() const;
    ...
};
```

- [ ] **Step 1: Port the existing suite to borrowed mode + write the new failing streaming tests**
  (in `gtest_cas_run_file.cpp`): every `ReadBufferFromMemory in(...); RunFileReader r(in);` becomes
  `RunFileReader r(std::string_view(bytes));`. New cases:

```cpp
/// Build a >3-block run (force block_size = 4096 via RunHeader), write it into an InMemoryBackend,
/// and stream-read it: identical record sequence to borrowed mode.
TEST(CasRunFileStreaming, MultiBlockStreamMatchesBorrowed) { ... }

/// seek() in streaming mode lands on the right record and costs exactly one extra ranged get
/// (CountingBackend::getCount on the run key), after the 3-request open profile.
TEST(CasRunFileStreaming, SeekUsesOneRangedGet) { ... }

/// Open profile: exactly head=1, get(tail)=1, getStream(body)=1 (CountingBackend totals; extend
/// CountingBackend with a getStream counter + per-request Range length recording).
TEST(CasRunFileStreaming, OpenIsThreeRequestsAndBlockBoundedRanges)
{
    /// ALSO asserts every recorded ranged-get length <= kRunHardCapBlockSize + 64 * 1024 (footer
    /// allowance) — the resident-memory bound of the Global Constraints, enforced at the seam.
}

/// Truncation + corruption fail closed: cut the object mid-block via putOverwrite of a prefix;
/// flip a payload byte (block CRC); flip a footer byte (footer CRC) => CORRUPTED_DATA on
/// construction or on the next() that reaches the damage; never a partial record.
TEST(CasRunFileStreaming, TruncatedOrCorruptFailsClosed) { ... }
```

  (Write the four bodies out fully in the test file; the helpers to build runs already exist in the
  suite — reuse `RunFileWriter` with a small `block_size`.)

- [ ] **Step 2: Run to verify compile failure** (old ctor gone / new ones missing).

- [ ] **Step 3: Implement** in `CasRunFile.{h,cpp}`:
  - Members replace `String full;` with:

```cpp
    std::string_view mem;                       /// borrowed mode; empty => streaming mode
    Backend * backend = nullptr;                /// streaming mode
    String key;                                 ///   "
    std::unique_ptr<ReadBuffer> body_stream;    ///   " (positioned at the first block)
    uint64_t body_pos = 0;                      ///   " absolute offset the stream is positioned at
    uint64_t data_end = 0;                      /// first footer byte (both modes)
```

  - Constructor (borrowed): parse header from `mem` (same 13-byte parse as today, over the view),
    `loadFooter` over the view's tail — the existing bound-checked parsing changes only its byte
    source (`mem` instead of `full`).
  - Constructor (streaming): `head(key)` (absent ⇒ CORRUPTED_DATA "run object absent"); tail
    ranged `get` of `min(size, kRunHardCapBlockSize + 64KB)` suffix bytes; parse the `footer_len`
    trailer + footer (existing logic over the tail buffer, offsets rebased); compute `data_end`;
    ranged-parse the 13-byte header (`get(key, {0, 13})`) — or fold it into the tail read when the
    object is small enough that the suffix covers it; then `getStream(key, {13, data_end - 13})`
    for the body positioned at the first block.
  - `loadBlock(block_no)`:
    - borrowed: exactly today's code over `mem`;
    - streaming, sequential (`block_no == cur_block_idx + 1` or first): read the block frame from
      `body_stream` (block_len u32 + body), CRC-verify, `cur_block` swap; `requireBytes`
      equivalents become "stream must yield exactly N bytes or CORRUPTED_DATA";
    - streaming, random (`seek`): ranged `get(key, {index[block_no].block_offset, frame_len})`
      where `frame_len` is derived from the NEXT index entry's offset (or `data_end`) — parse +
      CRC + install; sequential reading continues from the stream only if the stream position
      matches, else subsequent `next` re-syncs via ranged gets (simplest correct rule: after a
      seek, ALL subsequent blocks come from ranged gets; the pure-linear fold path never seeks).
  - Call sites: `CasBlobInDegree.cpp` keeps materialized `got->bytes` + borrowed view for now
    (Task 4 flips the fold to true streaming); `CasManifestCodec.cpp` passes its decoded string as
    a view (drop its `ReadBufferFromMemory`).

- [ ] **Step 4: Run** `--gtest_filter='CasRunFile*:CasThreeCursorMerge*:CasBlobInDegree*:CasManifest*'`
  → PASS; full `Cas*` sweep green.

- [ ] **Step 5: Commit** — `"CAS runs: RunFileReader borrowed-memory + streaming modes (full dies)"`.

---

### Task 4: streaming prior cursor in the fold

**Files:**
- Modify: `Core/CasBlobInDegree.cpp` (delete `readPriorEdges`; the merge's first cursor)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Consumes: Task 3 streaming `RunFileReader`.
- Produces: `foldDeltasIntoGeneration` signature UNCHANGED; internal `PriorEdgeCursor`:

```cpp
/// Streams the prior generation's surviving edges for one shard: chains the shard's run segments
/// in seq order (absent seq-0 => empty baseline), skips zero-marker rows, exposes lookahead.
/// Resident state: one RunFileReader (one block) at a time.
class PriorEdgeCursor
{
public:
    PriorEdgeCursor(Backend & backend, const Layout & layout,
                    uint64_t generation, uint64_t attempt, uint64_t shard);
    bool valid() const;              /// a current edge key is loaded
    const String & key() const;      /// 32-byte (blob_hash, source_id) key
    void advance();                  /// to the next surviving edge (skips zero markers, crosses segments)
};
```

- [ ] **Step 1: Failing test** — memory-shape + equivalence:

```cpp
/// A prior run spanning >3 blocks folds correctly with the streaming cursor AND the backend sees
/// only block-bounded ranged requests for it (no whole-object get of the prior run key).
TEST(CasBlobInDegree, FoldStreamsPriorRunBlockBounded)
{
    /// CountingBackend; build gen-1 with ~2000 edges at block_size 4096 (via a direct
    /// RunFileWriter write of the gen-1 run + matching fold_seal is NOT needed — drive
    /// foldDeltasIntoGeneration twice as the existing tests do, first fold creating gen 1);
    /// reset counters; fold gen-2 with a small delta; assert:
    ///   - out run bytes identical to the pre-change expectation (byte-reproducibility gate);
    ///   - getCount(whole-object) on the gen-1 run key == 0 (every get carried a Range / stream);
    ///   - max recorded request length <= kRunHardCapBlockSize + 64KB.
}
```

  (Extend `CountingBackend` — if not already done in Task 3 — to record, per key, whether a `get`
  was whole-object and the max ranged length; plus `getStream` counts.)

- [ ] **Step 2: Run to verify failure** (whole-object get still happens).

- [ ] **Step 3: Implement** — in `foldDeltasIntoGeneration`, replace

```cpp
    const auto prior = readPriorEdges(backend, layout, prior_generation, prior_attempt, shard);
```

  with a `PriorEdgeCursor cursor(backend, layout, prior_generation, prior_attempt, shard);` and the
  merge loop's `pi < prior.size()` / `prior[pi].first` / `++pi` with `cursor.valid()` /
  `cursor.key()` / `cursor.advance()`. Delete `readPriorEdges`. `PriorEdgeCursor` internals: hold
  the current `RunFileReader` (streaming mode) + current segment seq; `advance` pulls `next` until
  a `kEdgeActive` payload or the segment ends (then `head` the next seq key; absent ⇒ done). The
  cursor probes segment existence with `head` (cheap) before opening.

- [ ] **Step 4: Run** — the new test + `CasThreeCursorMerge*` + `CasBlobInDegree*` + full sweep;
  byte-reproducibility must hold (`RunsAreByteDeterministic` and `FoldDeltaByteEqualReplayAdopts`
  are the canaries).

- [ ] **Step 5: Commit** — `"CAS gc: the fold's prior cursor streams (readPriorEdges dies; memory O(block))"`.

---

### Task 5: preview consumers stream

**Files:**
- Modify: `Core/CasBlobInDegree.cpp` (`zeroInDegree`, `inDegreeInGeneration`)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:** signatures unchanged; internals swap `backend.get(key)` + borrowed reader for the
streaming constructor (and `inDegreeInGeneration` keeps its `seek` — now a ranged-get path).

- [ ] **Step 1: Failing test** — `ZeroInDegreeStreamsBlockBounded`: same CountingBackend pattern as
  Task 4 over a multi-block run: no whole-object `get` of the run key; results equal the old path.
- [ ] **Step 2: verify failure. Step 3: implement (mechanical ctor swap; the seq loop keeps
  `head`-probing).**
- [ ] **Step 4: Run** suite + full sweep. **Step 5: Commit** —
  `"CAS gc: preview consumers stream the runs"`.

---

### Task 6: T0 — reference-parent runs for empty-delta shards

**Files:**
- Modify: `Core/CasGenerationSeal.h` (`RunRef` fields), `Core/Proto/cas_format.proto`
  (`RunRefProto` additive `shard`, `generation`), `Core/CasGenerationSeal.cpp` (codec lines)
- Modify: `Core/CasGc.cpp` (fold: empty-bucket ref-carry; run resolution via parent seal refs),
  `Core/CasBlobInDegree.{h,cpp}` (`PriorEdgeCursor` + preview consumers take explicit
  `std::vector<RunRef>` instead of constructing keys), `Core/CasGcShardPlan.cpp` (reduce
  forwards the shard's prior refs)
- Modify: `src/Disks/tests/cas_test_helpers.h` (`inDegreeOf`/`foldCursorOf` resolve refs via the seal)
- Test: `src/Disks/tests/gtest_cas_gc_fold.cpp`, `gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Produces:

```cpp
struct RunRef
{
    String key;
    UInt128 checksum{};
    uint64_t shard = 0;        /// gc-shard this run belongs to (REQUIRED for blob_target_runs)
    uint64_t generation = 0;   /// generation whose key namespace holds the object (for retention)
    bool operator==(const RunRef &) const = default;
};
```

  - `PriorEdgeCursor(Backend &, const std::vector<RunRef> & segments)` — resolution moves to the
    caller; `foldDeltasIntoGeneration` gains `const std::vector<RunRef> & prior_runs` (replacing
    the `prior_generation/prior_attempt` pair for the FIRST cursor; those params stay for the
    output key namespace only — pin exact final signature in the fold header comment).
  - Fold rule (per gc-shard): `bucket.empty() && parent seal has refs for shard` ⇒ copy the
    parent's `RunRef`s (key/checksum/shard/generation verbatim) into the new `fold_seal` and skip
    the merge for that shard ENTIRELY — except the retired cursor must still settle (a shard with
    an empty delta can still hold retired entries): settlement for empty-delta shards runs
    `foldDeltasIntoGeneration` with empty deltas ONLY when the shard's `retired_merge` inputs are
    non-empty (prior retired entries or redeletes exist); a shard with empty delta AND empty
    retired list is pure ref-carry, zero I/O.

- [ ] **Step 1: Failing tests**:

```cpp
/// An empty round (no journal changes, no retired entries) touches ZERO run objects: after one
/// populated round, reset CountingBackend, run a no-op round; assert getCount+putCount == 0 for
/// every key under gc/gen/*/blob_target/; the new fold_seal's blob_target_runs EQUAL the parent's
/// (same keys, same checksums, parent generation).
TEST(CasGcFold, EmptyDeltaShardCarriesParentRunRef)

/// The NEXT round with a real delta folds THROUGH the carried ref (reads the old-generation key)
/// and produces the correct merged run under the new generation.
TEST(CasGcFold, FoldResolvesThroughCarriedRef)

/// previewDeletes and inDegreeOf resolve through refs (helper rewiring) — assert on a pool whose
/// current seal carries a parent ref.
TEST(CasGcFold, PreviewResolvesCarriedRef)
```

- [ ] **Step 2: verify failures. Step 3: implement** (proto additive fields + codec; fold ref-carry
  + ref-based resolution threaded through `fold` → `foldDeltasIntoGeneration`/`reduce` →
  `PriorEdgeCursor`; preview/test helpers resolve `gc/state → readFoldSeal(snap_generation,
  snap_attempt) → blob_target_runs` filtered by shard).
- [ ] **Step 4: Run** fold + indegree + ack-floor suites + full sweep.
- [ ] **Step 5: Commit** — `"CAS gc: empty-delta shards reference the parent run (idle rounds touch zero run objects)"`.

---

### Task 7: ref-aware retention + hand-off delete

**Files:**
- Modify: `Core/CasGc.cpp` (`pruneSupersededGenerations` skip-set; `runRegularRound` post-CAS
  hand-off delete)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (or `gtest_cas_gc_fold.cpp` — wherever the prune
  tests live today; extend there)

**Interfaces:**
- Consumes: Task 6 `RunRef.generation`.
- Produces:
  - `pruneSupersededGenerations(...)` takes the adopted seal's referenced-key set
    (`std::set<String>` built from the NEW `fold_seal.blob_target_runs` in `runRegularRound`) and
    SKIPS those keys in its LIST-delete loop (log one line per retained key: retained-by-ref).
  - Post-CAS hand-off in `runRegularRound`: for every shard whose new seal ref REPLACED a parent
    ref (old key != new key) where `old_ref.generation <= state.snap_pruned_through`, issue
    `deleteExact(old_key, head(old_key).token)` best-effort (NotFound/TokenMismatch tolerated,
    logged) — the wholesale prune already passed that generation and will never revisit. Runs in
    not-yet-pruned generations are left to the normal prune (their keys are no longer in the live
    ref set).

- [ ] **Step 1: Failing tests**:

```cpp
/// Retention keeps a referenced old-generation run alive: idle-carry a ref across > keep
/// generations (gc_snap_generations_to_keep is a PoolConfig knob — set keep=1 for the test), run
/// rounds until the prune passes the ref's generation; assert the referenced run object still
/// exists and folding through it still works.
TEST(CasGcRetention, PruneRetainsLiveReferencedRun)

/// When a later delta finally replaces the carried ref, the superseded old-generation run object
/// is hand-off deleted post-CAS (its generation was already pruned).
TEST(CasGcRetention, HandOffDeletesSupersededRef)
```

- [ ] **Step 2: verify failures. Step 3: implement. Step 4: run** retention/round/ack-floor suites,
  full `Cas*` sweep, and full `ninja -C build clickhouse`.
- [ ] **Step 5: Commit** — `"CAS gc: ref-aware retention + post-CAS hand-off delete for superseded parent refs"`.

---

### Task 8: docs, roadmap, closure

**Files:**
- Modify: `docs/superpowers/cas/04-gc-protocol.md` (snapshot-read subsection: streaming profile,
  T0 ref-carry), `docs/superpowers/cas/07-s3-budget.md` (idle-round = zero run bytes; 3-request
  open profile), `docs/superpowers/cas/ROADMAP.md` (flip the delta-runs-adjacent rows: T2+T0 DONE,
  T1 stays DESIRABLE/TODO with a pointer to this spec's primitives),
  `docs/superpowers/deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md` (top banner:
  SUPERSEDED by the 2026-07-02 snapshot-streaming spec; implemented).
- Test: none (docs); final full `Cas*` sweep + full build as the gate.

- [ ] **Step 1: Write the docs updates** (house rules: anchors on headers, inline code for literal
  names, present tense + short history notes).
- [ ] **Step 2: Final gate** — full sweep green, `ninja -C build clickhouse` exit 0.
- [ ] **Step 3: Commit** — `"docs(cas): snapshot streaming + reference-parent runs (T2+T0) landed"`.

---

## Self-review notes (already applied)

- Spec §Testing gates → tasks: gate 1,4,6 = Task 3; gate 2 = Task 4 (canaries named); gate 3 =
  Global Constraints + Task 3/4 request-size assertions (structural: no `full` member) — the plan
  deliberately enforces the memory bound at the SEAM (request sizes) plus structure, not via an
  allocator hook; gate 5 = Tasks 6–7; gate 7 = every task's sweep step.
- T0 nuance the spec implies but the plan makes explicit: an empty-DELTA shard with a non-empty
  retired list still runs the merge (settlement must happen every pass); pure ref-carry requires
  BOTH empty. Spec stays correct (it says "neither reads nor writes that shard's run" — with
  retired entries present the run IS read); recorded here as the governing interpretation.
- Type consistency: `PriorEdgeCursor` appears in Tasks 4 (gen/attempt ctor) and 6 (RunRef-vector
  ctor) — Task 6 REPLACES the ctor; both tasks state it.
