---
description: 'Implementation plan for CAS codecs v3 phase 5: converting the CARN block-framed binary GC run object (cas_run, the source-edge in-degree data plane) to sorted NDJSON (RecordStream) with a streaming reader, a whole-file seal-checksum verified before use, and the deletion of the seek/inDegree/seekPrefix machinery, the test-only RunMerger, and the part-manifest-cleanup run. Keeps CasRunFile alive as the ManifestEntries embedded-run codec for phase 6. The highest-risk phase (byte-adoption determinism + the GC data plane).'
sidebar_label: 'CAS codecs v3 phase 5 plan'
sidebar_position: 65
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase5-runs
title: 'CAS Codecs V3 — Phase 5: Runs (Record Stream) Text Cutover'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 5: Runs (Record Stream) Text Cutover Implementation Plan {#cas-codecs-v3-phase5}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Base assumption (LANDED, verified 2026-07-15):** phase 2 is integrated on `cas-gc-rebuild` at HEAD `45c53fab543` (the control-plane cutover + the protobuf-graveyard removal). This plan is written against that landed tree, not a patch stack. Concretely it relies on:

- the `Core/Formats/` surface: `CasTextFormat.{h,cpp}` (the file shape + the pinned `jsonWriteSettings` with `escape_forward_slashes = false`), `CasFormat.{h,cpp}` (the registry), `CasWireVocab.{h,cpp}` (`writeBlobRefFields` / `blobHashAlgoFromWord`), and the shared battery `src/Disks/tests/cas_format_test_battery.h`;
- the frozen `FormatId::RunFile = 13` registry entry with the landed `cas_run` `TRAITS` row (`CasFormat.cpp:123`): `RecordStream`, `Strict`, `PinnedRaw`, `object_cap = 0` (uncapped → streamed), `line_cap = 4 KiB`;
- the landed **text** `Core/Formats/CasFoldSealFormat.{h,cpp}` (NOT the removed proto `CasGenerationSeal`) — Task 7 edits this file's `part_manifest_cleanup` field + `pmc` record.

**Proto note:** the protobuf graveyard is already gone (phase-2 commit `45c53fab543`); `cas_run` was never a proto object (it is `CARN` block-framed binary), so there is **no proto remnant in phase-5 scope** and no proto-wiring grep to run here (that was phase 2 / phase 8).

**Goal:** convert the GC data-plane run object (`cas_run`, the source-edge in-degree stream) from the `CARN` dense block-framed binary format to **sorted NDJSON**: a typed header line `{"type":"cas_run","v":N,"kind":"source_edge"}`, one JSON record per line in today's binary sort order, and a `{"n":<count>}` trailer. Integrity moves from the per-block/footer `CRC32C` (internal to `CasRunFile`) to the **whole-file seal-checksum** (`RunRef.checksum`, carried by the referencing fold seal), accumulated with a **streaming** hash during the full sequential read and **verified before the read is acted on** (a deletion decision). `RunFileReader::seek`, `inDegreeInGeneration`, `SourceEdgeKeyCodec::seekPrefix`, the test-only `RunMerger`, the dead `RunKind`s (`BlobDelta`, `TargetShardDelta`), and the **part-manifest-cleanup run** (+ the fold seal's `part_manifest_cleanup` field + `partManifestCleanupKey`) are DELETED. `PinnedRaw` + `Strict`: the run is byte-deterministic for `putDeterministicArtifact` adoption. This is the **highest-risk phase** of the migration.

**What is NOT deleted (survey-corrected, load-bearing):** `Core/CasRunFile.{h,cpp}` — the `CARN` writer/reader/framing — **stays**, because `CasManifestCodec.cpp` embeds a `RunKind::ManifestEntries` `CARN` stream inside every `CAPT` part manifest (`CasManifestCodec.cpp:2` includes `CasRunFile.h`; writer `:123`, reader `:165`) and has no private framing of its own. That embedded stream is **phase 6's** surface. Phase 5 prunes `CasRunFile` down to what the embedded-manifest path needs (drop `seek`, `RunMerger`, the dead kinds) but keeps the sequential `RunFileWriter`/`RunFileReader` + block/footer/CRC framing alive. Phase 6 deletes `CasRunFile` entirely when it converts the embedded stream. (This corrects the phase-2 DAG one-liner at `…-phase2… §dag-phase5`, which loosely said phase 5 deletes `RunKind::ManifestEntries` and the embedded-stream code; the include-graph at HEAD proves that deletion belongs to phase 6.)

**Architecture:** `cas_run` is the `RecordStream` family — unbounded-cardinality sorted records, written once per GC generation/attempt/shard, always read **whole and sequentially**. The new codec is `Core/Formats/CasRecordStreamFormat.{h,cpp}` (backend-free, borrows a `ReadBuffer`/`WriteBuffer`); the `source_edge` record renders today's binary key + payload as named JSON fields whose string sort equals the current binary key order. Accepted cost: hex NDJSON is ≈2× the retired binary (spec §record-streams); measured in soak, fallback = re-binarize `cas_run` alone.

Spec: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md` §migration-order step 5 + §container-design (Runs bullet); reference: `docs/superpowers/cas/codecs_proposal_v3.md` §record-streams + §determinism.

**Tech Stack:** C++ (`dbms`), the phase-1 `CasTextFormat` helpers (`readLine`/`writeKey`/`writeStringValue`/`writeTrailerLine`/`checkCompatibility`), phase-2 `CasWireVocab`, `ReadHelpers`/`WriteHelpers`, `IO/HashingReadBuffer.h` + `IO/HashingWriteBuffer.h` (the streaming CityHash128), gtest (`unit_tests_dbms`).

## The streaming/seek reconciliation (the lead's critical question) {#streaming-seek-reconciliation}

The spec's Runs bullet (§container-design) says runs are **"sorted NDJSON, no blocks, no footer, no `seek`"** and deletes `RunFileReader::seek` / `inDegreeInGeneration` / `SourceEdgeKeyCodec::seekPrefix`. That does **not** contradict the T2/T0 **streaming** reader — the two words mean different things and the design keeps one while dropping the other:

- **STREAMING is kept.** `cas_run` has `object_cap = 0` in the `TRAITS` table (`CasFormat.cpp:123`) — it is explicitly *never* materialized whole. The v3 reader reads the object one NDJSON line at a time over a `ReadBuffer` (O(one 4 KiB line) resident, `line_cap = 4 KiB`), exactly the sequential ranged/`getStream` pattern the T2/T0 streaming `RunFileReader` used. Every production consumer (fold two-cursor merge, `zeroInDegree`, `previewDeletes`, `fsck`) is a full sequential scan.
- **SEEK is dropped.** `RunFileReader::seek` (random re-positioning to a key ≥ target, `CasRunFile.cpp:480`) has exactly **one** production caller — `inDegreeInGeneration` (`CasBlobInDegree.cpp:600`) — which is itself **dead** (only unit tests call it; the doc comment claiming `previewDeletes` uses it is FALSE — `previewDeletes` uses `zeroInDegree` + a raw `kCondemned` scan). No consumer point-looks-up into a run. `SourceEdgeKeyCodec::seekPrefix` (`CasBlobInDegree.cpp:307`) exists only to feed `seek` and dies with it.
- **Why dropping seek is a *prerequisite*, not a regression:** the whole-file seal-checksum model requires the reader to observe **every** byte of the object to accumulate the hash. A `seek` that skips bytes is fundamentally incompatible with a whole-file checksum. So the sequential-full-read access pattern and the whole-file-checksum integrity model are the *same* decision — deleting seek is what makes the checksum well-defined.

**Conclusion:** runs stay a **line-structured NDJSON stream with no offset index and no random access**; the reader streams sequentially and the seal-checksum is accumulated over the streamed bytes. No structure stays binary except the phase-6-owned embedded-manifest `CARN` stream (untouched here). Spec citation: §container-design "Runs" bullet + §record-streams; migration-order step 5.

## Global Constraints {#global-constraints}

- **Allman braces** everywhere (CI style check).
- **Layering (physical, phase-1 rule):** `Core/Formats/CasRecordStreamFormat.h` may include only other `Formats/` headers (`CasFormat.h`, `CasTextFormat.h`, `CasWireVocab.h`), the identifier vocabulary (`CasBlobRef.h`, `CasBlobHasher.h`, `CasBlobDigest.h`, `CasToken.h`, `CasRefIds.h`), `base/`, `src/IO/` (incl. `HashingReadBuffer.h`/`HashingWriteBuffer.h`), `src/Common/`. NEVER `CasBackend.h` / `CasStore.h` / `CasLayout.h` / `CasBlobInDegree.h`. A codec that wants a backend must not compile.
  - **Interface decision (RESOLVED here, was open in the scoping draft):** the streaming reader must read a run object off a `Backend`, but `Formats/` may not include `CasBackend.h`. Resolution: the `Formats/` reader/writer take a **`ReadBuffer &` / `WriteBuffer &`** and stay backend-free. `Core/` owns the stream. `openSourceEdgeRun(Backend &, key)` stays in `Core/CasBlobInDegree` (its current home; the two overloads live at `CasBlobInDegree.h:105-106`) as a thin adapter: it opens the backend stream and returns a small **owning holder** `{ std::unique_ptr<ReadBuffer> stream; SourceEdgeRunReader reader; }` (move-only) so the reader borrows a `ReadBuffer` whose lifetime the holder guarantees — mirroring how today's streaming `RunFileReader` owns its `body_stream`. Document this shape in the Task-1 header comment.
- **Determinism (HARD — this phase's headline risk):** `cas_run` is `PinnedRaw` + `Strict` and goes through `putDeterministicArtifact` (a retrying/failing-over leader re-encodes and the backend rejects any byte drift as `CORRUPTED_DATA`). The writer MUST be byte-reproducible: records emitted in the exact binary sort order (tuple over fixed-width lowercase hex = today's key order), fixed field order per record, strict keys, **no compression** (`PinnedRaw` → `sealObject(FormatId::RunFile, ·)` is identity; the codec must never route through the `Always`/zstd arm), no floats. String determinism inherits the phase-1 `escape_forward_slashes = false` pin (already landed; run keys/tokens are `/`-dense). The determinism test is a hard gate on the writer (Task 2).
- **Seal-checksum integrity (HARD — with a flagged mechanism change):** the read unit is the WHOLE object; the guard is `RunRef.checksum` carried by the fold seal. Every consumer accumulates the hash **while streaming** and verifies it against the seal **before acting on what it read**. Because `object_cap = 0` (no full materialization allowed), the hash must be **streaming-computable identically on write and read** — see the flagged decision in Task 3: use ClickHouse `HashingWriteBuffer`/`HashingReadBuffer` (chained CityHash128) on both sides, which **changes the checksum algorithm** from today's one-shot `cityHash128(run_bytes)` at the two seal-compute sites. Acceptable pre-release, but both sites and the read-side verify use the identical routine and the seal goldens regenerate. The trailer `n` additionally catches truncation at a line boundary (NDJSON's blind spot, orthogonal to the hash).
- **Pre-release, hard cutover, no dual-read:** the `CARN` path for `cas_run` is removed; no "sniff CARN vs NDJSON". (`CasRunFile`'s `CARN` code survives only for the phase-6-owned embedded-manifest stream — a different object, not `cas_run`.)
- **Error taxonomy:** malformed / truncated / trailer-count mismatch / seal-checksum mismatch / unknown `kind` / out-of-order key / duplicate key / over-`line_cap` → `CORRUPTED_DATA`; future `v` / unknown `!`-key → `UNKNOWN_FORMAT_VERSION`.
- **Consumer-sweep discipline (mandated — the T9/phase-7 blast-radius lesson):** EVERY deletion (`seek`, `inDegreeInGeneration`, `seekPrefix`, `RunMerger`, the dead `RunKind`s, the part-manifest-cleanup run, the fold-seal `pmc` field, `partManifestCleanupKey`) has an explicit `grep`-sweep step in its task that enumerates every consumer and shows **zero residual references** BEFORE the deletion lands.
- **Build discipline:** substitute the real configured build dir (`ls -d build*`; examples use `build_debug`). Foreground only, no `-j`, no `nproc`, redirect to a per-task log in the build dir, read back `NINJA_EXIT=`, and use a subagent to analyze any build log and return only a summary:

  ```
  flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p5t<N>.log 2>&1; echo "NINJA_EXIT=$?"
  ```

- **Commit discipline:** commit after every task; never rebase/amend; branch `cas-gc-rebuild`; explicit-path `git add` (never `git add -A` on this shared worktree — other agents' files must not be swept in), and verify `git log -1 --stat` names only your files before moving on. Commit trailer on every commit:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## Interfaces consumed from phases 1–2 {#consumed-interfaces}

Real signatures in the landed tree (do not re-declare — include the headers):

```cpp
// Core/Formats/CasTextFormat.h  (phase 1)
namespace DB::Cas
{
void writeKey(WriteBuffer & out, std::string_view key, bool & first);
void writeStringValue(WriteBuffer & out, std::string_view s);
void writeHex128Value(WriteBuffer & out, const UInt128 & v);
void writeU64StringValue(WriteBuffer & out, uint64_t v);
void closeObject(WriteBuffer & out, bool & first);
void writeTrailerLine(WriteBuffer & out, uint64_t n);            // {"n":N}\n
String readLine(ReadBuffer & in, uint64_t line_cap, std::string_view what);  // excl '\n'; over-cap -> CORRUPTED_DATA
struct TextHeader { String type; uint32_t v = 0; };
TextHeader expectHeaderLine(ReadBuffer & in, FormatId id);       // gates type + v (tolerates extra header keys, does NOT return them)
class JsonObjectReader { /* nextKey/readString/readHex128/readU64String/readU64Number/skipUnknown; ctor consumes '{' */ };
String sealObject(FormatId id, String text);                     // PinnedRaw -> identity
String openObject(FormatId id, std::string_view stored);         // PinnedRaw -> identity

// Core/Formats/CasFormat.h  (phase 1)
constexpr uint32_t G_BUILD = 3;                                  // the `v` every writer stamps
enum class FormatId : uint16_t { /* … */ RunFile = 13, /* … */ };
void checkCompatibility(uint32_t v, std::string_view what);      // v > G_BUILD -> UNKNOWN_FORMAT_VERSION
const FormatTraits & traitsFor(FormatId id);                     // RunFile: RecordStream/Strict/PinnedRaw, object_cap 0, line_cap 4 KiB

// Core/Formats/CasWireVocab.h  (phase 2)
void writeBlobRefFields(WriteBuffer & out, bool & first, const BlobRef & r);  // keys ha + h
BlobHashAlgo blobHashAlgoFromWord(std::string_view w, std::string_view what);

// IO/HashingWriteBuffer.h / IO/HashingReadBuffer.h
//   HashingWriteBuffer / HashingReadBuffer : IHashingBuffer<…>, getHash() -> CityHash_v1_0_2::uint128 (chained, streaming)
}
```

**Note — the `kind` header key.** `expectHeaderLine` gates `type` + `v` and (via the Tolerant `parseHeaderObject`) *skips* any extra header key but does not return it. `cas_run`'s header carries a third key `kind`, so the codec cannot get `kind` from `expectHeaderLine`. The codec therefore reads line 1 itself with a codec-local `expectRunHeaderLine(ReadBuffer &, std::string_view expected_kind)` that reads the line via `readLine`, parses `type`/`v`/`kind` with a `JsonObjectReader`, gates `type == "cas_run"` and `checkCompatibility(v, "cas_run")`, and fails closed on `kind != expected_kind`. Symmetrically, `writeHeaderLine(out, FormatId::RunFile)` writes only `{"type":"cas_run","v":3}`; the codec writes its own header line to append `"kind":"source_edge"` (fixed key order `type`,`v`,`kind` for determinism).

## Interface produced: `Core/Formats/CasRecordStreamFormat.h` {#produced-interface}

```cpp
namespace DB::Cas
{

/// One source-edge in-degree row: a blob reference, its source id, and the row-tag marker. The tuple
/// (ref bytes, source_id) is the sort key; string-sorting the emitted (b, s) fields reproduces the
/// current binary (algo, digest, source_id) byte order (fixed-width lowercase hex preserves it).
struct SourceEdgeRow
{
    BlobRef ref{};
    UInt128 source_id{};
    uint8_t marker = 0;   /// kEdgeActive / kZeroMarker / kCondemned (values owned by CasBlobInDegree.h)
};

/// v3 cas_run header line word for the only live kind.
inline constexpr std::string_view kSourceEdgeKindWord = "source_edge";

/// Sorted NDJSON writer over a caller-owned WriteBuffer (backend-free). append() asserts non-decreasing
/// (ref, source_id) order and throws on a regression; finish() writes the {"n":count} trailer. PinnedRaw:
/// the caller does NOT compress the result. Byte-deterministic (fixed field order, no floats).
class SourceEdgeRunWriter
{
public:
    explicit SourceEdgeRunWriter(WriteBuffer & out_);   /// writes the typed header line {"type":"cas_run","v":3,"kind":"source_edge"}
    void append(const SourceEdgeRow & row);             /// throws CORRUPTED_DATA/LOGICAL_ERROR on out-of-order input
    void finish();                                      /// writes {"n":count}\n
};

/// Sequential streaming reader over a caller-owned ReadBuffer (backend-free, O(one line) resident).
/// The ctor reads + gates the typed header line. next() yields records in stored order; the whole-object
/// bytes are fed through a chained CityHash128 as they stream. After the trailer is consumed and its
/// count checked, verifyAgainst() compares the accumulated hash to the seal's RunRef.checksum.
class SourceEdgeRunReader
{
public:
    explicit SourceEdgeRunReader(ReadBuffer & in_);     /// gates type/v/kind before any record
    bool next(SourceEdgeRow & row);                     /// false once the {"n"} trailer is consumed (count verified here)
    void verifyAgainst(const UInt128 & expected) const; /// accumulated whole-object hash != expected -> CORRUPTED_DATA
    UInt128 accumulatedChecksum() const;                /// for the seal-side compute path to reuse the same routine
};

}
```

`Core/CasBlobInDegree` keeps the backend-facing adapters (its current home, `CasBlobInDegree.h:105-106`), re-pointed at the new reader:

```cpp
// Core/CasBlobInDegree.h  (adapter overloads STAY in Core/, wrap the Formats/ reader)
struct OwnedSourceEdgeRun            /// move-only: owns the stream the reader borrows
{
    std::unique_ptr<ReadBuffer> stream;
    SourceEdgeRunReader reader;
};
OwnedSourceEdgeRun openSourceEdgeRun(Backend & backend, const String & key);
SourceEdgeRunReader openSourceEdgeRun(std::string_view bytes);   /// borrowed-memory overload (wraps a ReadBufferFromMemory the caller keeps alive)
```

## Object-to-codec-file map {#object-file-map}

| What | New location (phase 5) | Moves from | Notes |
|---|---|---|---|
| `cas_run` codec (`source_edge` NDJSON) | `Core/Formats/CasRecordStreamFormat.{h,cpp}` | new file (the `CARN` `cas_run` path in `CasRunFile` is deleted) | backend-free; `ReadBuffer`/`WriteBuffer` only |
| Source-edge run adapters + `openSourceEdgeRun` | stay in `Core/CasBlobInDegree.{h,cpp}` | — | thin backend wrappers over the codec |
| `SourceEdgeRow` marker constants `kEdgeActive`/`kZeroMarker`/`kCondemned` | stay in `Core/CasBlobInDegree.h:46-48` | — | the codec includes `CasBlobInDegree.h`? **No** (layering) — the marker byte values are duplicated as codec-local constants OR moved to a shared id header; decide in Task 1 (recommended: a tiny `CasSourceEdgeMarkers.h` in the vocabulary layer both include) |
| `CARN` framing (`RunFileWriter`/`RunFileReader` sequential, block/footer/CRC) | **stays** in `Core/CasRunFile.{h,cpp}` | — | phase-6-owned; pruned of `seek`/`RunMerger`/dead kinds only |

**Marker-constant layering (Task-1 decision).** `SourceEdgeRow` needs `kEdgeActive`/`kZeroMarker`/`kCondemned`, which live in `Core/CasBlobInDegree.h` — a header the `Formats/` codec may not include (it pulls in `Backend`). Resolve by extracting the three byte constants into a tiny identifier-layer header (`Core/CasSourceEdgeMarkers.h`, no subsystem deps) that both `CasBlobInDegree.h` and the codec include; do NOT duplicate the literals. Flag the chosen header name in the Task-1 commit.

## The v3 record-stream shape {#record-shape}

```text
{"type":"cas_run","v":3,"kind":"source_edge"}      header line (type + v=G_BUILD + kind gate)
{"b":"01<digest-hex>","s":"<32hex>","m":"edge"}     one record per line, SORTED by (b, s) = binary key order
{"b":"01<digest-hex>","s":"<32hex>","m":"zero"}
...
{"n":184267}                                        trailer: record count (line-truncation guard)
```

### Per-`RunKind` disposition {#kind-fields}

Only **`source_edge`** is a live `cas_run` object family. The rest (survey-confirmed at HEAD):

| `RunKind` (value) | Live for `cas_run`? | Phase-5 disposition |
|---|---|---|
| `SourceEdge` (3) | YES — the GC in-degree data plane | the only `cas_run` kind; full NDJSON mapping below |
| `BlobDelta` (2) | DEAD — no producer/consumer | delete from `CasRunFile`'s `RunKind` (Task 5) |
| `TargetShardDelta` (5) | DEAD — no producer/consumer | delete from `CasRunFile`'s `RunKind` (Task 5) |
| `ManifestEntries` (4) | NOT a `cas_run` object | **KEEP** in `CasRunFile`'s `RunKind` — it is the embedded stream inside a `CAPT` part manifest (`CasManifestCodec.cpp:121-126` writer / `:165-169` reader, `key_schema 0`), owned by **phase 6**. Phase 5 does not encode/decode it as `cas_run`. |

**`source_edge` record mapping.** Row tags (`CasBlobInDegree.h:46-48`): `kEdgeActive = 0x01`, `kZeroMarker = 0x00`, `kCondemned = 0x02`. Today's key is `SourceEdgeKeyCodec` bytes `(algo, digest, source_id)` (key schema 3, `CasBlobInDegree.h:55`); the sort key is that tuple in BYTE order.

| JSON key | From | Rendering | Sort role |
|---|---|---|---|
| `b` | `BlobRef` (algo + digest) | algo-prefixed lowercase hex: `<algo-byte 2-hex><digest-hex-at-algo-width>` (via `codecFor(algo).toHex`) | primary sort component |
| `s` | source id (`UInt128`) | 32-char lowercase hex | secondary sort component |
| `m` | row tag | word: `edge` (`kEdgeActive`) / `zero` (`kZeroMarker`) / `condemned` (`kCondemned`) | payload, not a sort component |

**HARD sort-order requirement** (`gtest_cas_gc_source_edge.cpp` ordering tests, `:18`/`:65`): string-sorting records by `(b, s)` MUST equal the current binary `(algo, digest, source_id)` byte order. Fixed-width lowercase hex preserves unsigned byte order (digits `0`–`9` sort below `a`–`f`), and the algo byte is emitted first, so the property holds by construction — but it is a golden-test-pinned invariant, not a convention (Task 2).

## Tasks {#tasks}

Per the DAG (`…-phase2… §dag-phase5`): Tasks 1–6 are **[INDEPENDENT]** (draftable against the landed phase-2 wire vocabulary); Task 7 is **[PHASE-2-DEPENDENT]** (edits the landed text `CasFoldSealFormat`). Every deletion carries an explicit consumer-sweep whose grep MUST print zero residual references before it lands.

---

### Task 1 [INDEPENDENT] — `Formats/CasRecordStreamFormat` skeleton: markers header, typed header line, trailer, record struct {#task1}

**Files:**
- Create: `Core/Formats/CasRecordStreamFormat.{h,cpp}` (backend-free per §produced-interface)
- Create: `Core/CasSourceEdgeMarkers.h` (the three marker byte constants, identifier layer) — and re-point `Core/CasBlobInDegree.h:46-48` to include it instead of re-declaring the literals
- Test: `src/Disks/tests/gtest_cas_record_stream_format.cpp` (new)

**Interfaces:**
- Consumes: phase-1 `CasTextFormat` (`readLine`, `JsonObjectReader`, `writeKey`, `writeStringValue`, `writeTrailerLine`, `checkCompatibility`), `CasFormat` (`FormatId::RunFile`, `G_BUILD`, `traitsFor`), phase-2 `CasWireVocab` (`writeBlobRefFields`, `blobHashAlgoFromWord`), `CasBlobRef` (`BlobRef`, `codecFor`), the new `CasSourceEdgeMarkers.h`.
- Produces: the `SourceEdgeRow` struct, `kSourceEdgeKindWord`, `writeRunHeaderLine`/`expectRunHeaderLine` (codec-local), and empty `SourceEdgeRunWriter`/`SourceEdgeRunReader` shells whose `append`/`next` are stubbed to compile (real records land in Task 2/3). The **interface decision** (backend-free `ReadBuffer &`/`WriteBuffer &` + `Core/`-owned `OwnedSourceEdgeRun` holder) is documented in the header comment (§Global-Constraints layering bullet).

**Steps:**
- [ ] **Step 1: Failing test** — a header round-trip: write the typed header line, read it back through `expectRunHeaderLine(in, kSourceEdgeKindWord)` (green); a wrong-`kind` header (`"kind":"blob_delta"`) → `CORRUPTED_DATA`; a wrong-`type` header → `CORRUPTED_DATA`; a `v+1` header → `UNKNOWN_FORMAT_VERSION`; the empty-run round-trip (header + `{"n":0}` trailer, zero records).
- [ ] **Step 2: Register + verify compile failure.** `add_headers_and_sources` already globs `Core/Formats`; build → `NINJA_EXIT=1` (`CasRecordStreamFormat.h` missing).
- [ ] **Step 3: Implement** — `writeRunHeaderLine(WriteBuffer &, std::string_view kind)` writes `{"type":"cas_run","v":G_BUILD,"kind":"<kind>"}\n` (fixed key order); `expectRunHeaderLine(ReadBuffer &, std::string_view expected_kind)` reads line 1 via `readLine(in, traitsFor(FormatId::RunFile).line_cap, "cas_run")`, parses `type`/`v`/`kind` with a `JsonObjectReader`, gates `type == "cas_run"` (else `CORRUPTED_DATA`), `checkCompatibility(v, "cas_run")`, and `kind == expected_kind` (else `CORRUPTED_DATA`, "unknown run kind"); `SourceEdgeRow`; stubbed writer/reader ctors + `finish`/trailer. Extract the marker constants into `CasSourceEdgeMarkers.h` and re-point `CasBlobInDegree.h`.
- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasRecordStream*'` green; the full `Cas*` slice still green (the marker-header extraction touched `CasBlobInDegree.h`).
- [ ] **Step 5: Commit** — `git add` `Formats/CasRecordStreamFormat.*`, `Core/CasSourceEdgeMarkers.h`, `Core/CasBlobInDegree.h`, `gtest_cas_record_stream_format.cpp`. Message `cas: formats v3 phase 5 — CasRecordStreamFormat skeleton (typed header + markers header)` + trailer.

---

### Task 2 [INDEPENDENT] — NDJSON writer + `source_edge` encoder + determinism + sort-order golden {#task2}

**Files:** Modify `Core/Formats/CasRecordStreamFormat.{h,cpp}`; extend `gtest_cas_record_stream_format.cpp`.

**Interfaces:** Consumes Task 1, `CasWireVocab::writeBlobRefFields`, `codecFor(algo).toHex`.

**Steps:**
- [ ] **Step 1: Failing test.** (a) a golden NDJSON text file for a fixed multi-record `source_edge` run (the §record-shape bytes, `escape_forward_slashes = false` inherited); (b) **determinism**: build the same sorted `SourceEdgeRow` sequence twice → `encode(a) == encode(b)` byte-for-byte; (c) **sort-order golden** (the HARD §kind-fields invariant): a fixed set of `(algo, digest, source_id)` tuples spanning the algo/digest/source boundaries (include a `sha256` algo digest at its wider width and the sentinel `source_id = 0` at len-32), assert that string-sorting the emitted `(b, s)` lines equals the binary `(algo, digest, source_id)` byte order — reproduces `gtest_cas_gc_source_edge.cpp` ordering; (d) `append` out of `(ref, source_id)` order → throws `CORRUPTED_DATA`/`LOGICAL_ERROR`.
- [ ] **Step 2: Verify RED** (writer body still stubbed).
- [ ] **Step 3: Implement** — `SourceEdgeRunWriter::append`: assert non-decreasing `(ref bytes, source_id)` (replaces the old `prev_key` monotonicity check in `CasRunFile`), then emit one line: `writeBlobRefFields(out, first, row.ref)` (`ha`+`h`) **rendered as `b`** — decision: keep the plan's compact `b` (algo-prefixed single hex string) **or** reuse `CasWireVocab`'s `ha`+`h` sibling pair; recommended = the compact `b` because the sort contract is over the concatenated `(algo-byte, digest)` bytes and a single field makes the string-sort trivially correct (document the choice; if `ha`+`h` is chosen, the sort test compares the tuple `(ha, h, s)`). Then `s` (`writeHex128Value` of `source_id`) and `m` (marker word via a codec-local 3-entry word map). `finish()` writes the `{"n":count}` trailer. No compression.
- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasRecordStream*'` green.
- [ ] **Step 5: Commit** — `cas: formats v3 phase 5 — source_edge NDJSON writer (deterministic, sort-order pinned)` + trailer.

---

### Task 3 [INDEPENDENT] — NDJSON reader + `{"n"}` verify + NET-NEW streaming whole-file seal-checksum {#task3}

**Survey finding — this is NEW functionality, not a port.** `RunRef.checksum` is COMPUTED today at exactly two seal sites (`CasBlobInDegree.cpp:561`, `CasGc.cpp:1624`) but **verified on NO read path** — the only on-read integrity today is the per-block/footer `CRC32C` internal to `CasRunFile` (which the `cas_run` path stops using). So the spec's "whole-file seal-checksum verified before use" must be BUILT here and WIRED into every reader (Task 6).

**FLAGGED DECISION — streaming hash, algorithm change.** `object_cap = 0` forbids materializing the whole run to hash it one-shot. Therefore the checksum must be **streaming-computable identically on write and read**. Use ClickHouse `HashingReadBuffer` (read) / `HashingWriteBuffer` (write), whose `getHash()` is a chained CityHash128 over the streamed bytes. This **changes the checksum value/algorithm** from today's one-shot `cityHash128(run_bytes)`. Because CAS is pre-release (no persisted data) and the checksum is recomputed on both sides, this is safe **iff** the write-side compute (`CasBlobInDegree.cpp:561`, rewired in Task 6) and the read-side accumulate use the identical routine and byte range (the WHOLE stored object: header line + records + trailer). The `RunRef.checksum` field type is unchanged (`UInt128`). Flag this in the Task-6 commit; regenerate any checksum golden.

**Files:** Modify `Core/Formats/CasRecordStreamFormat.{h,cpp}`; extend `gtest_cas_record_stream_format.cpp`.

**Steps:**
- [ ] **Step 1: Failing tests.** (a) round-trip: write N records, stream them back in order, `accumulatedChecksum()` equals `HashingWriteBuffer`-of-the-same-bytes; (b) `verifyAgainst(correct)` passes, `verifyAgainst(flipped)` throws `CORRUPTED_DATA`; (c) flip one byte anywhere → `verifyAgainst` throws **before** any record is consumed for a decision (assert the throw path is reachable pre-decision — the test drains then verifies, mirroring the consumer contract); (d) drop the trailer / wrong `n` → `CORRUPTED_DATA` at `next()`'s trailer step; (e) truncate at a line boundary → `CORRUPTED_DATA`; (f) over-`line_cap` line → `CORRUPTED_DATA` (via `readLine`).
- [ ] **Step 2: Verify RED.**
- [ ] **Step 3: Implement** — `SourceEdgeRunReader` wraps the borrowed `ReadBuffer &` in an internal `HashingReadBuffer` and reads **everything** (header line + records + trailer) through it, so `getHash()` covers the whole object. The ctor calls `expectRunHeaderLine(hashing_in, kSourceEdgeKindWord)` (both gates and hashes line 1). `next(row)` reads one record line; when it reads a line whose first key is `n`, it verifies the count against the records seen, checks EOF (no bytes after trailer → else `CORRUPTED_DATA`), marks exhausted, and returns `false`. `verifyAgainst(expected)`: `if (accumulated != expected) throw CORRUPTED_DATA` (naming the run key/consumer). `accumulatedChecksum()` exposes `getHash()` for the seal-side compute to reuse the identical routine.
- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasRecordStream*'` green.
- [ ] **Step 5: Commit** — `cas: formats v3 phase 5 — source_edge NDJSON streaming reader + whole-file seal-checksum verify` + trailer.

---

### Task 4 [INDEPENDENT] — re-point the fold's two-cursor merge; delete the test-only `RunMerger` {#task4}

**Survey finding — there is NO production `RunMerger`.** The fold's merge is the hand-rolled **two-cursor loop inside `foldDeltasIntoGeneration`** (`CasBlobInDegree.cpp:330`, decl `CasBlobInDegree.h:216`), driven by `PriorEdgeCursor` (class `CasBlobInDegree.cpp:50`; it opens the prior generation's run segments at `CasBlobInDegree.cpp:140`) merged against a manually-indexed `scattered` delta vector (cursor at `:358`, loop from ~`:366`). `RunMerger` (`CasRunFile.h:166` / `CasRunFile.cpp:537,544,551`) has **no production caller** — only `gtest_cas_run_file.cpp:313,332,354`. It is genuinely dead once `source_edge` leaves `CARN`, and the fold is effectively 2-way (prior stream vs new deltas), not k-way, so **no line-based k-way merger is built** (YAGNI).

**Files:** Modify `Core/CasBlobInDegree.{h,cpp}` (re-point `PriorEdgeCursor` + `foldDeltasIntoGeneration` at `SourceEdgeRunReader`); modify `Core/CasRunFile.{h,cpp}` + `gtest_cas_run_file.cpp` (delete `RunMerger` + its 3 tests).

**Steps:**
- [ ] **Step 1: Sweep** (RunMerger has zero production callers):
  ```bash
  grep -rn 'RunMerger' src/ | grep -v gtest_cas_run_file.cpp   # EXPECT: only CasRunFile.h/.cpp def sites
  ```
- [ ] **Step 2: Failing/behavioral test.** Re-point `PriorEdgeCursor::advance` to open segments via the Task-1 borrowed/streaming reader (`openSourceEdgeRun`). Keep the existing fold behavioral tests (`gtest_cas_gc_fold.cpp`, `gtest_cas_blob_indegree.cpp`) as the safety net — they must pass after the re-point. Add a **fold determinism** assertion if not already covered: the merged/sealed run for a fixed delta+prior multiset is byte-equal across two runs (the writer's determinism plus the merge being a pure function of the multiset + key order). Add a **duplicate-key** test: a key present across prior + new (and duplicated within one) yields every payload in the deterministic sorted order.
- [ ] **Step 3: Implement** — `PriorEdgeCursor` holds a `SourceEdgeRunReader` (via the `Core/` adapter) instead of a `RunFileReader`; its `advance()` calls `reader.next(row)` and, on segment exhaustion, calls `reader.verifyAgainst(seal RunRef.checksum for that segment)` **before** the segment's rows influence a durable decision (see Task 6 for the exact checksum plumbing; Task 4 wires the reader type, Task 6 wires the verify source). Delete `RunMerger` (class + methods) from `CasRunFile.{h,cpp}` and its 3 tests from `gtest_cas_run_file.cpp`.
- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasGcFold*:CasBlobInDegree*:CasRunFile*'` green.
- [ ] **Step 5: Commit** — `cas: formats v3 phase 5 — fold two-cursor merge on the NDJSON reader; delete dead RunMerger` + trailer.

---

### Task 5 [INDEPENDENT] — delete `seek` / `inDegreeInGeneration` / `seekPrefix` + dead `RunKind`s (each with its sweep) {#task5}

The seek chain is **fully dead** (§streaming-seek-reconciliation). Per-deletion sweeps (each must print zero non-def residual before deleting):

- [ ] **`inDegreeInGeneration`** (def `CasBlobInDegree.cpp:590`, decl `CasBlobInDegree.h:236`):
  ```bash
  grep -rn 'inDegreeInGeneration' src/ | grep -v gtest_   # EXPECT: only def/decl + the FALSE doc comment at CasBlobInDegree.h:234
  ```
  Delete the function + decl + its unit tests (`gtest_cas_blob_indegree.cpp:102,752-753,767-768`, `gtest_cas_gc_shard_plan.cpp` refs, `gtest_cas_pluggable_hash.cpp:919-920`, `cas_test_helpers.h:681`). **Fix the stale doc comment** at `CasBlobInDegree.h:234` (it falsely claims `previewDeletes` uses `inDegreeInGeneration`; `previewDeletes` uses `zeroInDegree` + a `kCondemned` scan).
- [ ] **`RunFileReader::seek`** (def `CasRunFile.cpp:480`, decl in `CasRunFile.h`):
  ```bash
  grep -rn '\.seek(\|->seek(' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -viE 'CasObjectStorageBackend|ReadBuffer'
  # EXPECT after inDegreeInGeneration is gone: only the def in CasRunFile.cpp + the 5 seek tests in
  # gtest_cas_run_file.cpp (:89,:112,:136,:409,:554). CasObjectStorageBackend.cpp:456/492 are ReadBuffer::seek — UNRELATED, keep.
  ```
  Delete `RunFileReader::seek` + the sparse footer-index fields it needs ONLY for seek (`BlockIndexEntry index`, the `seeked` ranged-get path) **iff** the embedded-manifest reader does not use them — the embedded `CasManifestCodec` reader is sequential (`r.next` only, `CasManifestCodec.cpp:169`), so the footer *index* is seek-only and can go, but `loadFooter` (which also yields `total_count`/`data_end`) stays. Delete the 5 seek tests.
- [ ] **`SourceEdgeKeyCodec::seekPrefix`** (def `CasBlobInDegree.cpp:307`, decl `CasBlobInDegree.h:75`):
  ```bash
  grep -rn 'seekPrefix' src/   # EXPECT after seek is gone: only def/decl (its one caller was CasBlobInDegree.cpp:600 inside inDegreeInGeneration)
  ```
  Delete. **Also sweep `SourceEdgeKeyCodec::encode`**: after `source_edge` moved to NDJSON (Task 2/6), the packed byte-key is no longer produced. `grep -rn 'SourceEdgeKeyCodec' src/` — if `encode` has no non-test caller, delete it too (the sort contract now lives in the NDJSON `(b,s)` fields + the Task-2 golden, not the packed key); if a test still asserts the packed-key order, migrate it to assert the NDJSON `(b,s)` order. Decide + document at draft.
- [ ] **Dead `RunKind`s** in `Core/CasRunFile.h:21` (`enum class RunKind`): delete `BlobDelta = 2` and `TargetShardDelta = 5`; **keep** `ManifestEntries = 4` (phase-6 embedded stream) and (transitionally, until Task 7 removes its last user) note that `SourceEdge = 3` is no longer produced/consumed via `CARN` — delete `SourceEdge` from `CasRunFile`'s `RunKind` **only after Task 6** re-points every `cas_run` producer/consumer to the NDJSON codec (sequence Task 5's `RunKind` prune after Task 6 lands, or split it). Sweep:
  ```bash
  grep -rn 'RunKind::BlobDelta\|RunKind::TargetShardDelta\|RunKind::SourceEdge' src/
  ```

Consumes: Tasks 1–4 (and Task 6 for the `SourceEdge` kind removal).

- [ ] **Verify + Commit** — `unit_tests_dbms --gtest_filter='Cas*'` green; `cas: formats v3 phase 5 — delete dead seek/inDegree/seekPrefix machinery + dead RunKinds` + trailer.

---

### Task 6 [INDEPENDENT] — migrate the `source_edge` producer + read consumers + wire verify-before-use {#task6}

Rewire every live `source_edge` run site to `CasRecordStreamFormat` + the Task-3 verify-before-use contract. Exact sites (survey, verified at HEAD):

- **Producer** — `CasBlobInDegree.cpp:364` (the new-generation source-edge run writer; `header.kind = RunKind::SourceEdge` at `:362`, `key_schema` at `:363`; appends `kEdgeActive :551`, `kZeroMarker :501`; `finish() :557`) → replace with `SourceEdgeRunWriter` into a `WriteBufferFromOwnString`. The checksum computed at `CasBlobInDegree.cpp:561` switches from one-shot `cityHash128(run_bytes)` (helper `:32`) to the **streaming chained CityHash128** matching the reader (feed the produced bytes through a `HashingWriteBuffer`, or reuse `SourceEdgeRunReader::accumulatedChecksum()` over the produced bytes) — the FLAGGED decision from Task 3. This is the sole surviving seal-side compute after Task 7 (the `CasGc.cpp:1624` pmc compute is deleted in Task 7).
- **Read consumers** (each: open via `openSourceEdgeRun`, stream records, then `verifyAgainst(seal RunRef.checksum)` BEFORE the records influence a durable decision):
  - **fold two-cursor merge** — `PriorEdgeCursor` (`CasBlobInDegree.cpp:140`) driven from `foldDeltasIntoGeneration` (`:330`). **The checksum's most important consumer — the fold decides deletions.** Verify each segment on exhaustion, before the merged result seals a new generation.
  - **`zeroInDegree`** scan — def `CasBlobInDegree.cpp:565`, opens run at `:574`.
  - **`previewDeletes`** `kCondemned` scan — `CasGc.cpp:2235`; opens at `:2283`, tests `payload[0] != kCondemned` at `:2288`.
  - **`fsck`** source-edge read — `CasFsck.cpp:456` (`reader.next :459`, parse `:463`, `kCondemned` test `:467`).
- Each consumer gets a RED test proving a seal-checksum mismatch aborts THAT entry point before a delete/decision (the lead's "RED test per consumer entry point"). Note `fsck` is a read-only auditor — its verify raises a finding rather than gating a delete; assert it reports `CORRUPTED_DATA`/a dangling finding rather than silently trusting.

Consumes: Tasks 1–5.

- [ ] **Verify + Commit** — `unit_tests_dbms --gtest_filter='Cas*'` green; the GC round/fold/fsck suites (`gtest_cas_gc_round*`, `gtest_cas_gc_fold`, `gtest_cas_gc_ack_floor`, fsck tests) green. `cas: formats v3 phase 5 — source_edge producer/consumers on NDJSON + streaming seal-checksum verify` + trailer.

---

### Task 7 [PHASE-2-DEPENDENT] — remove the part-manifest-cleanup run + fold-seal `pmc` field + key {#task7}

**Survey finding — the part-manifest-cleanup run has NO reader.** Its deletes execute inline from the in-memory `folded.mf_cleanup` map (`CasGc.cpp:637` loop, `deleteExact` at `:639`); the run object + its seal `RunRef`s are **record-only** (produced by `writePartManifestCleanupBundle` — `RunFileWriter` at `CasGc.cpp:1612`, `header.kind = RunKind::ManifestEntries :1610`, `key_schema = 1 :1611`, key from `partManifestCleanupKey :1618`, checksum `:1624`, stored into `fold_seal.part_manifest_cleanup :1333`; rendered by `CasInspect.cpp:360-363,380`; one gtest at the seal/key level). Removing it breaks NO functional consumer — it is dead weight. Deletions, each with its sweep:

- [ ] **The cleanup-run writer** — `writePartManifestCleanupBundle` (`CasGc.cpp:1612` writer, `:1618` `partManifestCleanupKey`). Delete the writer + its call at `CasGc.cpp:1332`. Note it used `RunKind::ManifestEntries` with `key_schema = 1` — deleting it removes the *only non-manifest* `ManifestEntries` user, leaving `CasManifestCodec` (`key_schema 0`) as the sole one (phase-6 surface). Sweep:
  ```bash
  grep -rn 'partManifestCleanupKey' src/   # EXPECT after: only the builder decl CasLayout.h:363 (deleted next) once the writer is gone
  grep -rn 'writePartManifestCleanupBundle' src/   # EXPECT: zero after
  ```
- [ ] **`CasLayout::partManifestCleanupKey`** (`CasLayout.h:363`) — delete after the writer is gone; migrate/drop `gtest_cas_layout.cpp:126,135`.
- [ ] **The fold seal's `part_manifest_cleanup` field** — on the landed tree this is `Core/Formats/CasFoldSealFormat.h:99` (the `std::vector<RunRef> part_manifest_cleanup;`) + the `pmc` record arm in `CasFoldSealFormat.cpp` (`writeSortedRuns(out, "pmc", …)` on the write side; the `kind == "pmc"` branch on the read side). Remove the field, the `pmc` write arm, and the `pmc` read branch; remove the populate at `CasGc.cpp:1333` and the `CasInspect.cpp:360-363,380` render. Sweep:
  ```bash
  grep -rn 'part_manifest_cleanup\|"pmc"\|mf_cleanup' src/   # EXPECT after: only the folded.mf_cleanup inline-delete path (CasGc.cpp:637-639), which STAYS (it is the in-memory delete driver, not the run)
  ```
  (Keep the `folded.mf_cleanup` map + its inline `deleteExact` loop — that is how manifest cleanups actually execute; only the durable *run object* and the seal *record* go.)
- [ ] **Update the fold-seal tests** — `gtest_cas_fold_seal_format.cpp:20,59` assert a `part_manifest_cleanup` `RunRef` round-trips through the seal; remove those assertions. Because removing the `pmc` record changes the seal bytes (a fresh format generation of a deterministic object; no persisted data pre-release), **the phase-2 seal determinism/golden tests must be regenerated in the SAME commit**: `CasFoldSeal`/`CasFoldSealFormat` `EncodingIsByteDeterministic` + `TextIsByteDeterministic` + the battery golden for `cas_fold_seal`. The seal stays `PinnedRaw` + `Strict`; only its record set shrinks.

Consumes: landed `CasFoldSealFormat`, Task 6.

- [ ] **Verify + Commit** — `unit_tests_dbms --gtest_filter='CasFoldSeal*:CasGc*:CasLayout*:CasInspect*'` green. `cas: formats v3 phase 5 — remove part-manifest-cleanup run + fold-seal pmc field + partManifestCleanupKey` + trailer.

---

### Task 8 — golden text, full CAS slice, README flip, soak-cost hook {#task8}

**Files:** `gtest_cas_record_stream_format.cpp` (final golden); `Core/Formats/README.md` (flip the runs row); the soak hook.

**Steps:**
- [ ] **Golden** — the final `source_edge` NDJSON golden (the §record-shape shape, sort-order-pinned) is in place and green; add a `cas_run` `FormatBatteryCase` row exercising the SHAPE (header/trailer/truncation/`v+1`/wrong-type/leading-garbage). **Battery note:** the row's `decode` performs a *structural* decode over `ReadBufferFromMemory` (header gate + records + `{"n"}` count) but does **not** call `verifyAgainst` — the seal-checksum is a cross-object guard living in the referencing seal, tested separately in Task 3/Task 6, not intrinsic to the object bytes. `PinnedRaw` → `openObject` is identity and `looksZstd` is false, so the battery's compressed-arm check is skipped by construction.
- [ ] **README** — in `Core/Formats/README.md`, flip the runs bucket-map row from `CasRecordStreamFormat`**\*** (still-legacy marker) to `CasRecordStreamFormat` (done), and move `cas_run` in the phase-status sentence from "remaining `*` rows" into the DONE set. Same commit as the cutover-completing change (README rule).
- [ ] **Soak-cost hook** — REGISTER (do not pre-build a fallback) the 2×-byte-cost measurement: at the phase-5 completion soak, log the per-full-fold source-edge run-object byte totals for the before/after comparison. The fallback (re-binarize `cas_run` alone behind the same typed-open API) is written ONLY on soak evidence.
- [ ] **Verify + Commit** — `unit_tests_dbms --gtest_filter='Cas*'` fully green. `cas: formats v3 phase 5 — cas_run golden + battery row + README flip + soak-cost hook` + trailer.

## Phase-5 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- `cas_run` is sorted NDJSON (typed header + records + `{"n"}` trailer) via `Core/Formats/CasRecordStreamFormat`; the `CARN` path for `cas_run` is gone; STREAMING is preserved (`object_cap = 0`, O(one line) resident) and `seek` is deleted.
- **Determinism proven:** same sorted record set → byte-equal run (writer test); adoption via `putDeterministicArtifact` still works (a GC resume/failover e2e passes); the sort-order golden pins the `(b,s)` = binary-key-order invariant.
- **Seal-checksum proven:** the whole-file **streaming** chained CityHash128 is verified before use; a flipped byte → `CORRUPTED_DATA` before any deletion decision (unit + a GC-fold-path test); trailer `n` catches line truncation; both seal-compute sites and the read-side use the identical routine.
- `RunFileReader::seek`, `inDegreeInGeneration`, `SourceEdgeKeyCodec::seekPrefix`, the test-only `RunMerger`, the dead `RunKind`s (`BlobDelta`/`TargetShardDelta`, plus `SourceEdge` from `CasRunFile` once every `cas_run` site is on NDJSON), the part-manifest-cleanup run, the fold-seal `part_manifest_cleanup` field, and `partManifestCleanupKey` are DELETED — each with a consumer-sweep showing zero residual references.
- **`CasRunFile` (`CARN`) is intentionally NOT deleted** — it survives (pruned) as the phase-6-owned embedded-manifest (`RunKind::ManifestEntries`) codec used by `CasManifestCodec`. `CasManifestCodec` still builds and its manifest tests pass.
- **2×-byte-cost measurement (named soak step):** at the completion soak, capture source-edge run-object byte totals per full fold and compare to the pre-cutover baseline; record the ratio. If the soak shows the ≈2× cost is a real bottleneck, the **localized fallback is to re-binarize `cas_run` ALONE** behind the same typed-open API (no other object affected), per spec §record-streams. Do NOT pre-build it.
- Phase 6 (part manifest) can then delete `RunKind::ManifestEntries` + the embedded-stream path + `CasRunFile` entirely (its own JIT plan).

## Phases: this is JIT — draft gate {#draft-gate}

Foundation is LANDED (phase 2 at HEAD `45c53fab543`), so this plan is a full task plan, not a scoping spec. Per the lead: the **code draft** starts only after the lead clears it (phase-2 soak + stateless gate on mainline). Tasks 1–6 are independent and draft against the landed phase-2 wire vocabulary; Task 7 edits the landed text `CasFoldSealFormat` directly (never a patch stack). The lead reviews this plan before any code lands.
