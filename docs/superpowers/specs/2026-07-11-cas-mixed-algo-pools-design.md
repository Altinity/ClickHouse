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
  `foldManifestEdges` width gate). Schema 3 (§6) supersedes schemas 1/2; the codec/fail-close
  discipline carries over, and the algo-first key removes the cross-width order hazard entirely.

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

## 6. GC: algo-prefixed settlement keys (no per-algo partitions) {#gc-keys}

**Decision (user, superseding the first draft):** do NOT partition the run namespace per algo.
Settlement identity gains the algo dimension IN THE ROW KEY, inside the same `gc_shards` runs:

- **Source-edge key schema 3** (replaces schemas 1/2 wholesale — pre-release, no back-compat):
  `algo(u8) ++ digest[blobHashLenFor(algo)] ++ source_id(16 BE)`, i.e. 33 bytes for 16-byte algos,
  49 for sha256. The algo byte comes FIRST, which makes the variable-width keys totally ordered by
  construction: keys of different algos diverge at byte 0, so digest bytes of different widths are
  NEVER compared against each other — the Phase 2 T4 cross-width hazard is eliminated, not guarded.
  Within one algo all keys share one width. Order == `(algo, digest, source_id)`; the zero-`source_id`
  sentinel still sorts first within its blob group. Overhead vs today: +1 byte/row (runs are
  uncompressed, `kRunCodecNone`, payloads ~1 byte — acceptable).
- **Self-describing parse**: read the algo byte -> `len = blobHashLenFor(algo)` -> the key must be
  exactly `1+len+16` bytes (`CORRUPTED_DATA` otherwise); an unknown algo byte is `NOT_IMPLEMENTED`.
  One `SourceEdgeKeyCodec` successor owns build/parse/seekPrefix (same len-drift discipline as T4);
  `assertSourceEdgeRunHeader` accepts schema 3 only.
- **No algo loop anywhere in settlement.** The fold/merge processes self-describing rows; it does not
  know or iterate the pool's algo set. `blobShard` stays digest-only (`bytes[0:8] % gc_shards`) —
  same-value digests under two algos co-shard harmlessly; existing pools keep their routing.
  `BlobDelta`/`BlobCandidate` gain `algo` (u8); the merge comparator becomes
  `(algo, digest, source_id)` — byte-identical to the key order. `foldManifestEdges` emits deltas
  with `entry.algo`.
- **Seal unchanged**: `blob_target_runs` + `condemned_summary` stay total over `gc_shards` exactly as
  today. No algo dimension in the seal, no empty-partition minting, no `algos_used`-driven iteration
  whose incompleteness could silently leak a subset (the first draft's headline risk is structurally
  gone).
- **Delete/condemn identity**: `RetiredEntry`, `OutcomeEntry`, `head_blob`/`peek_head`, `blobKeyOf`,
  the `.meta` API take `(algo, digest)` (`BlobRef {uint8_t algo; BlobDigest digest;}`). The
  exact-token delete is untouched (full key + token; algo already in the object key).
- **Ack-floor / graduation / rounds / generations**: unchanged.

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
S3-staging, envelope format (already has `hash_algo`), the fold seal shape and its `gc_shards`
totality. No TLA+ re-gate expected: the settlement protocol is unchanged — only the row-key alphabet
widens (algo-prefixed keys), below the model's abstraction level.

## 9. Testing gates {#testing}

1. Additive switch: create pool at `ch128`, insert; reopen at `sha256`, insert → both subsets
   readable; `algos_used = {ch128, sha256}`; old data intact (e2e, mirrors the 2026-07-11 fail-close
   e2e but asserting SUCCESS).
2. Mixed manifest: mutation carry-forward across the switch → one manifest with entries of both
   algos; round-trip; fold settles both partitions; `dangling==0` after DROP of either or both.
3. GC completeness (crux): forced GC on a 2-algo pool reclaims orphans of BOTH algos to
   `physical_bytes=0` — proves settlement handles mixed rows end-to-end with no per-algo blind spot.
3a. Schema-3 key order & parse: `(algo, digest, source_id)` byte order == logical order across mixed
   widths (incl. adversarial digests whose bytes would mis-compare without the algo prefix — the T4
   teeth-test construction, now expected to be UNREACHABLE by key design); parse fail-closes on a
   wrong-length key and on an unknown algo byte; sentinel-first per blob group at both widths.
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

Medium-small (down from the first draft: no run-namespace change, no seal change, no partition
lifecycle). The bulk is the schema-3 key codec + `BlobRef` plumbing through fold/GC/meta/fsck and the
per-entry manifest algo; PoolMeta relax is mechanical. Biggest risk: key build/parse discipline — one
missed call site building a digest-first (schema 1/2 style) key would corrupt merge order; mitigate
exactly as T4 did (ONE codec owns build/parse/seekPrefix, no bare helpers; grep-gate that schemas 1/2
constructors are deleted). Second risk: a consumer narrowing `BlobRef` back to bare digest (identity
collision across algos) — the strong pair type + deleted single-arg overloads make this
compiler-visible. Two-consult review of the schema-3 task before implementation (Phase 2 T4
discipline).
