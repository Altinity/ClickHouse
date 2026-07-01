# Deferred: O(buffer) streaming for CA GC run-file merge (A1 + A3)

**Status:** DEFERRED backlog (optimization, NOT a correctness bug). Recorded 2026-07-01.
**Origin:** the source-edge in-degree fix (`docs/superpowers/specs/2026-07-01-cas-gc-indegree-refold-undercount-design.md`, §streaming) stated an O(buffer)-memory HARD requirement for the snapshot merge. The merge *algorithm* meets it (O(1) per current blob); the *inputs* do not — they materialize whole runs in memory. The spec overstated the status; this doc is the honest scope.

## What the debt is

The GC blob-in-degree snapshot is now a **set of source edges** persisted as a `RunKind::SourceEdge` run keyed by `(blob_hash, source_id)`. `foldDeltasIntoGeneration` (`CasBlobInDegree.cpp`) merges the prior run + this round's edge deltas.

- The **two-cursor merge itself is streaming**: it holds only O(1) per current blob (`cur_blob`, `cur_edges`, `cur_touched`) plus one front record per input, and emits surviving edges + zero-markers in one pass. This part already satisfies the spec's §streaming intent.
- But the **inputs are fully materialized**, so actual peak memory is **O(active edges in the shard)**, not O(block):
  1. `readPriorEdges` (`CasBlobInDegree.cpp`) collects ALL prior surviving-edge rows into a `std::vector` before merging. **(A3)**
  2. `RunFileReader` (`CasRunFile.cpp:155-161` ctor) reads the **entire** stream into a `full` std::string, then serves blocks from that in-memory copy. Its block index + `seek` + `loadBlock` exist, but the source is the fully-materialized `full`.
  3. `Backend::get(key, Range)` (`CasBackend.h:140`) HAS a range parameter, but `CasObjectStorageBackend::get` (`CasObjectStorageBackend.cpp:271-283`) reads the **whole object** via `object_storage.readObject(...)` then `content.substr(offset, length)` — the range is a post-read slice, saving no memory.

The `RunMerger` doc comment (`CasRunFile.h`, ~line 125) already flags this: *"The O(inputs * block_size) bound applies ONCE block-ranged reads replace the Phase-1a whole-run `full` materialization."*

## Why it matters (and why it's deferred)

- **Not a correctness bug.** The fix is correct; determinism and reclaim all hold. This is a scalability/memory property.
- **Worse in absolute terms than before the edge-set change:** the snapshot went from 1 row/blob (the old integer count) to N rows/blob (one per active source edge), so the whole-run materialization is N× larger for a high-fan-in blob. At current dev/soak scale it does not bite; it bites at large pools with high fan-in.
- Deferred because a proper fix is **multi-layer, high-risk, and touches the backend abstraction every run consumer shares** — worth doing as its own focused project when scale demands it, not squeezed into the undercount fix.

## The fix scope (three coordinated layers)

1. **Backend — real ranged read.** `CasObjectStorageBackend::get(key, Range{offset,length})` must issue a TRUE ranged read at the object-storage layer (`object_storage.readObject` with a seek to `offset` + `setReadUntilPosition(offset+length)` in `ReadSettings`), not read-whole-then-`substr`. `InMemoryBackend` needs matching ranged support so unit tests exercise the same path.
2. **RunFileReader — ranged consumption.** Stop materializing `full`. Read the footer via one ranged get (tail `footer_len` bytes — read the trailing u32 first to learn `footer_len`), then read each `DataBlock` on demand via a ranged get at `block_offset .. +block_size`, holding only `cur_block`. This **changes RunFileReader's interface** (from `ReadBuffer`-over-materialized-bytes to a backend+key / seekable source), and RunFileReader is shared by EVERY run consumer (blob in-degree, blob deltas, source edges, target-shard deltas, cursors) — all call sites must be updated together.
3. **Merge / `readPriorEdges` (A3).** Once the reader streams, `readPriorEdges` becomes a straight `RunFileReader::next()` stream feeding the two-cursor merge — drop the intermediate `std::vector`. The delta side (`scattered`) is this round's journal-window edges, bounded and small; keep it as-is or stream it too.

## Hard constraints for the fix

- **Determinism / byte-reproducibility MUST hold.** Sealed runs go through `putDeterministicArtifact` (byte-equal-or-`CORRUPTED_DATA` on resume). Ranged reads must produce byte-identical runs to the whole-object path.
- **Fail-closed on short/partial reads** (never over-read a self-inconsistent block; keep the existing `requireBytes`/CRC guards).
- Ritual when picked up: `superpowers:writing-plans` → `superpowers:subagent-driven-development` with **TDD**. Key test gates: (a) a run larger than one block reads correctly via ranged reads; (b) byte-reproducibility across whole-object vs ranged read; (c) a resident-memory assertion or instrumentation proving O(block); (d) all existing `CasGc*`/`CasBlobInDegree*`/`CasSourceEdge*` stay green.

## Related items (recorded for completeness)

- **A2 — DROPPED (not a debt).** `source_id` is already a 16-byte `cityHash128` (not the raw `(ManifestId, path)`), so there is no large-key bloat; the N-rows-per-blob cost is inherent to the edge-set model, and dictionary/RLE gives nothing on high-entropy hashes (the format itself notes this). Only marginal block-level `blob_hash`-prefix compression is theoretically possible — against the format's no-per-record-overhead design; not worth it.
- **A3** folds into A1 (layer 3) — no independent value until the reader streams (its floor is set by `RunFileReader`'s `full`).

## Code anchors

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp` — `readPriorEdges`, `foldDeltasIntoGeneration` two-cursor merge.
- `.../Core/CasRunFile.cpp:155-161` — `RunFileReader` ctor `full` materialization; `loadFooter`/`loadBlock`/`seek`/`next`.
- `.../Core/CasRunFile.h` — `RunFileReader`/`RunFileWriter`/`RunMerger` (the O(inputs*block) comment).
- `.../Core/CasBackend.h:140` — `get(key, Range)` interface.
- `.../Core/CasObjectStorageBackend.cpp:271-283` — read-whole-then-substr (the layer-1 fix site).
