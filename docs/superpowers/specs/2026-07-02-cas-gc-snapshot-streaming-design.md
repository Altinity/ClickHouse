# CAS GC snapshot streaming (O(buffer) memory) + reference-parent runs — design

**Status:** DESIGN (2026-07-02). **Branch:** `cas-gc-snapshot-streaming` (off `cas-gc-ack-floor-fence`).
**Supersedes:** `docs/superpowers/deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`
(A1 + A3; written before the ack-floor redesign — the per-candidate `inDegreeInGeneration` seek
path it worried about was removed structurally by the one-pass round).
**Scope decision (2026-07-02):** T2 (streaming reads, memory O(block)) + T0 (reference-parent runs
for empty-delta shards) now; T1 (delta-runs — O(delta) bytes written per pass, periodic
compaction) is the NEXT spec and builds on this one's primitives unchanged.

## Problem

Two independent costs of the in-degree snapshot (the per-gc-shard `RunKind::SourceEdge` run), on
top of the ack-floor one-pass round:

1. **Memory (T2).** Reading a snapshot run materializes it in RAM three times over:
   `CasObjectStorageBackend::get` reads the whole object and serves `Range` by `substr`
   (a post-read slice); `RunFileReader`'s constructor copies the stream into its `full` string;
   `readPriorEdges` collects every surviving edge into a `std::vector`. Peak per pass =
   3 × O(active edges in the shard). At the target scale (100 000 tables × 100 parts × 20 blob
   entries ⇒ ~200 M edges ≈ 8.2 GB per full snapshot at ~41 B/record) that is ~24 GB of RAM for
   one fold — a wall, not a cost.
2. **Idle bytes (T0).** Every pass rewrites every gc-shard's snapshot run even when the shard's
   delta is EMPTY (the `!folded_any` / empty-bucket branches re-read the parent run and write a
   byte-identical successor). An idle pool pays 2 × snapshot bytes per round for zero useful work;
   at 5-minute rounds and the target scale that is terabytes per day of pure churn. (A HOT pool's
   O(snapshot) bytes per pass is the T1 problem — deliberately out of scope here.)

The merge algorithm itself is already streaming (O(1) per current blob, three cursors); only its
INPUTS are materialized. The run FORMAT already supports everything needed: self-framed CRC'd
blocks and a sparse footer index — `RunFileReader`'s `seek`/`loadBlock` machinery exists and
simply indexes into the materialized `full` today.

## T2 — streaming reads

### Backend seam

- `Backend::get(key, Range)` in `ObjectStorageBackend` becomes a TRUE ranged read:
  `object_storage.readObject` + `seek(range.offset)` + `setReadUntilPosition(offset + length)`,
  never read-whole-then-`substr`. (`InMemoryBackend` already serves ranges from memory; its
  behavior is the contract oracle.)
- New seam: `Backend::getStream(const String & key, Range range = {})` returning
  `{std::unique_ptr<ReadBuffer>, Token}` — a forward-only stream, nothing materialized. S3-family
  backends pass the `readObject` buffer through; `InMemoryBackend` returns a buffer over a copy
  (a test backend's memory IS the data; the copy is honest there). CONTRACT: `getStream` is for
  WRITE-ONCE objects only (runs, seals) — their bytes cannot change under the stream; mutable
  objects (shards, gc/state, mounts) stay on `get`. The token identifies the incarnation the
  stream reads, same as `get`.
- Reading one run costs 3 small requests instead of 1 whole-object-into-RAM request:
  `head(key)` (object size) → ranged `get` of the tail (the footer: `footer_len` trailer, CRC'd
  block index) → `getStream` of the body `[header_start, footer_start)`. Bytes on the wire are
  unchanged; resident memory is O(block).

### `RunFileReader`: two modes, `full` dies

- **Borrowed-memory mode** (zero-copy): constructor over `std::string_view` — the caller's bytes
  are the backing store, nothing is copied. Consumers: `CasManifestCodec` (manifest entries are
  parsed from an already-decoded in-memory body — this removes ITS extra copy too) and every
  in-memory test.
- **Streaming mode**: constructor `(Backend &, const String & key)` — performs the
  head/tail/stream dance above, holds ONLY `cur_block` (≤ `kRunHardCapBlockSize` = 1 MiB) plus
  the footer index. `next` pulls blocks sequentially from the stream; `seek(key)` stays supported
  (the sparse index is loaded; the target block is one ranged `get`) — that primitive is exactly
  what T1's point-lookups will use, and it costs nothing to keep.
- Fail-closed unchanged: per-block CRC and the footer CRC verify as today; a short/truncated
  stream read is `CORRUPTED_DATA`, never a partial parse. The block-by-block-ranged-GET
  alternative for full scans (one GET per block, no stream) was REJECTED for the linear paths:
  an 8 GB run is ~32 000 blocks = a request storm per shard per pass; it survives only as the
  `seek` implementation.

### Consumers

- `foldDeltasIntoGeneration`: `readPriorEdges` (the vector) is deleted. The prior cursor of the
  three-cursor merge becomes a streaming `RunFileReader` chain over the shard's run segments (in
  `seq` order, as the vector loop concatenated them), skipping zero-marker rows inline. The merge
  structure — settlement rules, zero markers, output writer — does not change; only the first
  cursor's source does. `RunFileWriter` is untouched (already O(block)), so output bytes are
  byte-identical and `putDeterministicArtifact` semantics are unaffected.
- `zeroInDegree` and `inDegreeInGeneration` (both now serve only `Gc::previewDeletes` and tests)
  switch to streaming mode mechanically.
- The scattered delta side stays as-is: it is this round's journal window, bounded and already
  in memory; sorting it is not a memory concern (documented bound, not a change).

## T0 — reference-parent runs for empty-delta shards

- When a gc-shard's delta bucket is empty for a pass, the fold neither reads nor writes that
  shard's run: the new `fold_seal` carries the PARENT generation's `RunRef` (key + checksum)
  verbatim for that shard. Deterministic by construction (same inputs ⇒ same refs), so seal
  determinism and crash-replay adoption are unchanged.
- `RunRef` gains an explicit `shard` field (additive proto) so per-shard association never parses
  key paths.
- **Consumers resolve runs through seal refs, never by key construction.** Today
  `readPriorEdges`/`zeroInDegree`/`inDegreeInGeneration` build `blobTargetRunKey(generation, …)`
  themselves — with T0 the run for generation G may physically live at an older generation's key.
  The fold already reads the parent seal (`readSealedCursors`); the same seal's
  `blob_target_runs` refs become the run source. `previewDeletes` resolves gc/state → adopted
  seal → refs.
- **Ref-aware retention.** `pruneSupersededGenerations` must not delete a run object the LIVE
  adopted seal still references: the prune's LIST loop skips exactly those keys (the referenced
  set is one seal already in hand — O(gc_shards) keys). The prune cursor may advance past a
  generation that retained referenced objects. To keep that leak-free, the hand-off rule: when a
  later pass finally REPLACES a shard's parent-ref with a freshly written run, it deletes the
  superseded old-generation run object post-CAS (same best-effort regime as the manifest cleanup;
  a crash in that window strands at most one small object per shard, visible to fsck). A run
  referenced by the live seal has exactly one referrer (its shard's chain), so the hand-off
  delete never races another reader older than the retention window.
- Idle-round effect: a round where nothing changed touches ZERO run objects (no GET, no PUT) —
  combined with the ack-floor round's request profile, an idle round is a LIST sweep + heartbeats
  + one gc/state CAS and nothing else.

## What deliberately does NOT change

- Run format bytes (header/blocks/footer): none. No schema version, no new `RunKind`.
- `RunFileWriter`, merge settlement rules, retired-list handling, attempt scoping, the round
  protocol.
- T1 (delta-runs: O(delta) bytes written per pass, stack + compaction, adaptive point-lookup vs
  streaming-join settlement) — next spec; it consumes this spec's `getStream`, ranged `get`,
  streaming reader with `seek`, seal-ref resolution, and ref-aware retention as-is.

## Testing gates (all TDD)

1. A run larger than one block (multi-block, forced small `block_size`) reads correctly in
   streaming mode: same record sequence as borrowed-memory mode.
2. Byte-reproducibility: a fold whose prior run is consumed via streaming produces a
   byte-identical output run (and identical `RunRef` checksums) to the materialized path.
3. Resident-memory proof, not a proxy: an instrumented stream/backend asserts the reader never
   holds more than `footer + one block` of run bytes at any moment during a multi-block scan.
4. Request profile (`CountingBackend`): streaming open = exactly 3 requests per run
   (HEAD + tail GET + body stream); `seek` = +1 ranged GET per touched block.
5. T0: an empty-delta round performs 0 GET/PUT on the untouched shard's runs; consumers
   (fold next round, `previewDeletes`) still resolve the snapshot through the seal refs; the
   prune retains referenced old-generation runs and the hand-off delete reclaims a superseded
   ref (plus the fsck-visible crash-window case documented).
6. Truncated/corrupt stream (cut mid-block, bad block CRC, bad footer CRC) ⇒ `CORRUPTED_DATA`,
   fail-closed, no partial merge output adopted.
7. Full existing suites green: `CasGc*`, `CasBlobInDegree*`, `CasSourceEdge*`, `CasThreeCursorMerge*`,
   `CasManifest*`, `CasRunFile*`.

## Non-goals

- T1 delta-runs / compaction (next spec).
- Compression codecs for runs (high-entropy keys; rejected in the run-format design).
- Streaming the scattered delta input (bounded by the journal window; not a memory driver).
- Any change to blob/content read paths (this is GC-metadata plumbing only).
