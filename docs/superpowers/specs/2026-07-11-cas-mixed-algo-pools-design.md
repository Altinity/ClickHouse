# CAS mixed-algo pools (additive blob-hash switching) — Design

**Status:** designed 2026-07-11 (user-driven brainstorm; decisions by Mikhail). Builds directly on
pluggable-blob-hash Phase 2 (`2026-07-11-cas-pluggable-blob-hash-design.md`, commits
`b932a0430eb..c5a7c0409fb`). This is effectively **Phase 3**.

## 1. Problem {#problem}

Phase 2 records ONE `blob_hash_algo` per pool and `PoolMeta::createOrValidate` fail-closes any reopen
with a different algo (`checkBlobHashAlgoMatches`: "never re-hash an existing pool"). Changing the
algo therefore requires creating a NEW pool and re-inserting everything — operationally unacceptable
for large pools (potentially petabytes rewritten just to change a hash function).

**Decision (user):** allow a pool to carry blobs under SEVERAL algos simultaneously. No data
migration, no re-hashing: old blobs stay under their original algo forever; new writes use the
configured algo. Dedup degrades gracefully — it works only within each algo subset (a one-time,
accepted cost of switching). GC/fsck become algo-aware.

## 2. The real invariant (narrower than Phase 2 enforced) {#real-invariant}

The physically necessary invariant is **never re-hash an existing BLOB** (its digest is its key;
re-hashing changes the key). Phase 2's pool-level fail-close was a conservative over-approximation.
Mixed-algo pools preserve the blob-level invariant: a blob's `(algo, digest)` identity is immutable
from upload to exact-token delete.

The blob identity everywhere becomes the PAIR `(BlobHashAlgo algo, BlobDigest digest)` — digest alone
is no longer unique: `ch128` and `xxh3` are both 16-byte, and the same 16-byte value under the two
algos names two DIFFERENT objects (distinct paths `blobs/ch128/…` vs `blobs/xxh3/…`).

## 3. What Phase 2 already gives us {#already-there}

- **Algo in the object key**: `blobs/<algo>/<shard2>/<hex>` (`ch128`|`xxh3`|`sha256`). Reads, exact-token
  deletes, and `.meta` siblings need no format change — the key already disambiguates.
- **Per-blob self-description**: the envelope header stamps `hash_algo` (`CasEnvelope.h:61`).
- **Variable-width digests**: `BlobDigest` + `DigestCodec`; source-edge run schemas 1 (16 B) / 2 (32 B)
  with fail-closed width-coherence gates (`SourceEdgeKeyCodec`, `PriorEdgeCursor` schema gate,
  `foldManifestEdges` width gate). These gates remain — repurposed from "forbid the feature" to
  "forbid ACCIDENTAL width mixing inside one run partition".

## 4. Why manifests must be per-ENTRY algo (the central decision) {#per-entry-algo}

Carry-forward is real: a mutation's destination build adopts existing entries
(`ContentAddressedTransaction.cpp:214-220` — `recordPendingBlobDep(entry.blob_hash)` +
`adoptEvidence(entry)`). After an algo switch, a new manifest legitimately references OLD-algo blobs
(carried columns) AND NEW-algo blobs (rewritten columns) in the same part. Forcing single-algo
manifests would force full part rewrites on every mutation touching an old part — the re-insert cost
the user rejected. Therefore:

- `ManifestEntry` gains `uint8_t algo` (a `BlobHashAlgo` value). Entry payload:
  `placement u8, algo u8, blob_hash <lenFor(algo)> raw BE bytes, blob_size u64, inline_len u32, inline`.
- The per-manifest `blob_hash_len` header field (Phase 2 T3) is **removed** — width is per-entry,
  derived from the entry's `algo` via `blobHashLenFor`. Decode fail-closes on an unknown algo id
  (`CORRUPTED_DATA`). CA is pre-release: no back-compat path, no version bump.
- The Phase 2 T4 `foldManifestEdges` check `body.blob_hash_len == poolMeta().blob_hash_len` is
  replaced by per-entry validation: `entry.algo` must be a member of the pool's recorded algo SET
  (§5); an unknown/never-enabled algo in a manifest is `CORRUPTED_DATA` (same injection-route
  protection, per-entry granularity).

## 5. PoolMeta: write-algo + ever-used set {#poolmeta}

Replace the single fail-closed `blob_hash_algo` with:

- `write_algo` (u8): the algo NEW blobs are hashed with. Follows the disk config `<blob_hash>` on
  every (re)open — changing config is now legal and simply switches `write_algo`.
- `algos_used` (repeated u8, append-only set): every algo that has ever been `write_algo` for this
  pool. Reopen with a new algo CAS-appends it (idempotent, first-writer-wins merge like other pool
  meta fields). Never removed — GC/fsck must keep classifying old-algo blobs forever (until the last
  one is reclaimed; keeping the entry after that is harmless and simpler).

Fail-close remains for: unknown algo id anywhere (`NOT_IMPLEMENTED`), and any manifest entry / listed
blob under an algo NOT in `algos_used` (foreign-object protection). `checkBlobHashAlgoMatches` is
deleted; its test flips to assert the additive-switch behavior.

## 6. GC: partition settlement by algo {#gc-partition}

The settlement identity gains the algo dimension by PARTITIONING, not by widening every key:

- **Run namespace**: `blobTargetRunKey(generation, attempt, shard, seq)` (`CasLayout.h:192`) gains an
  algo segment → `…/gc/gen/<generation>/<algo>/<shard>/<seq>` (exact layout at implementation).
  Each (algo × shard) partition holds ONLY that algo's edges/condemns — every run stays
  width-homogeneous, so the entire Phase 2 T4 machinery (key schemas 1/2, `SourceEdgeKeyCodec`,
  coherence gates, 2-cursor merge, retired-in-snapshot `kCondemned` rows, zero-sentinel) is reused
  UNCHANGED inside a partition. No cross-algo merge ever happens by construction; the coherence gate
  now guards against a mis-routed segment (corruption), its proper defense-in-depth role.
- **Fold**: `foldManifestEdges` buckets each `BlobDelta` by `(entry.algo, blobShard(digest))`.
  `BlobDelta`/`BlobCandidate` gain `algo` (u8). One `ShardReducer` per (algo × shard); rounds and
  generations stay GLOBAL (one `gc/state`, one fold seal per generation).
- **Seal totality**: `CasFoldSeal.blob_target_runs` + `condemned_summary` become total over
  (algos_used × gc_shards) — the coverage/`carryParentRefs` totality checks iterate the recorded algo
  set. An algo with zero blobs still gets its (empty) partition entries so totality stays checkable.
- **Delete/condemn identity**: `RetiredEntry`, `OutcomeEntry`, `head_blob`/`peek_head`, `blobKeyOf`,
  `.meta` API take `(algo, digest)` (a small `BlobRef {uint8_t algo; BlobDigest digest;}` struct).
  The exact-token delete is untouched (full key + token, algo already in the key).
- **Ack-floor / graduation**: unchanged — floors are per-mount and rounds are global; graduation
  paces per condemned row inside its partition exactly as today.

## 7. Sweep, fsck, dedup, inspect {#consumers}

- **Condemn-sweep & fsck** derive `(algo, width)` from the PATH segment (`blobs/<algo>/…`) instead of
  pool meta — strictly cleaner than today (the Phase 2 T5 ports read pool width; they will read the
  per-key segment). A key under a segment not in `algos_used` is foreign debris (skip in sweep;
  `unaccounted` in fsck). Sets keyed by `BlobRef`.
- **Dedup cache** keys on `BlobRef`. New writes hash with `write_algo`, so cross-algo dedup misses
  naturally (same content, different key) — the accepted degradation. No lookup into old-algo subsets.
- **Inspect / event log** render `algo:hex` (e.g. `sha256:ab…`) wherever a blob hash is shown.

## 8. What does NOT change {#unchanged}

Token model (ETag exact-token delete), source_id/`sourceEdgeId` (`UInt128` internal id), root-shard
journal/refs, manifests' ref/identity model, ack-floor protocol, retired-in-snapshot semantics,
S3-staging, envelope format (already has `hash_algo`). No TLA+ re-gate expected: partitioning is a
namespace split running N independent instances of the SAME verified protocol under one global round
counter; flag for re-check only if seal-totality interacts with graduation in an unforeseen way.

## 9. Testing gates {#testing}

1. Additive switch: create pool at `ch128`, insert; reopen at `sha256`, insert → both subsets
   readable; `algos_used = {ch128, sha256}`; old data intact (e2e, mirrors the 2026-07-11 fail-close
   e2e but asserting SUCCESS).
2. Mixed manifest: mutation carry-forward across the switch → one manifest with entries of both
   algos; round-trip; fold settles both partitions; `dangling==0` after DROP of either or both.
3. GC totality: forced GC on a 2-algo pool reclaims orphans of BOTH algos to `physical_bytes=0`
   (the anti-silent-leak crux, now per algo-partition).
4. fsck classifies both subsets; a blob under a never-enabled algo segment → `unaccounted`, sweep
   skips it.
5. Same-digest/different-algo: craft two blobs whose 16-byte digests collide across `ch128`/`xxh3`
   (trivially constructible — same digest VALUE, different algo) → distinct objects end-to-end
   (keys, meta, settlement rows, deletes).
6. Regression: single-algo pools byte-identical in behavior (existing 802-test battery + scenarios).
7. Soak: phase-3 chaos on a pool switched mid-soak (`ch128` → `sha256` at t≈50%), `dangling==0`.

## 10. Non-goals {#non-goals}

- Re-hashing/migrating existing blobs to the new algo (explicitly rejected — the whole point).
- Cross-algo dedup (content-equality across algos is unknowable without re-hashing).
- Per-table/per-insert algo selection — the switch is pool-level (disk config), coarse by design.
- Removing algos from `algos_used`.

## 11. Effort & risk {#effort}

Medium. The bulk is §6 (settlement partitioning: run-key namespace, per-(algo×shard) reducers, seal
totality) and the `BlobRef` plumbing through GC/meta/fsck; §4/§5/§7 are mechanical. Biggest risk:
seal-totality over the algo set (a missed empty partition would trip the totality fail-close, or —
worse — a totality check that silently iterates only `write_algo` would leak the other subset).
Mitigate with test §9.3 as the crux gate, mirroring Phase 2's silent-site discipline. Two-consult
review of the settlement-partitioning task before implementation (same discipline as Phase 2 T4).
