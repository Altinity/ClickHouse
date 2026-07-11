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

**The blob identity is the PAIR `BlobRef{algo, digest}` — everywhere, period (user decree).** A bare
digest CEASES TO EXIST as a blob identity: no API parameter, no container key, no on-wire key, no
rendered id may carry a blob content hash without its algo. There are NO compatibility modes, NO
legacy overloads, NO dual paths — digest-only blob-identity code is DELETED, not deprecated, so a
missed site is a compile error. (Digest alone is not even unique: `ch128` and `xxh3` are both
16-byte; the same 16-byte value under the two algos names two DIFFERENT objects.)

**BlobRef construction is restricted to the two places where algo and digest are born or read
TOGETHER:** (1) the write mint — the hasher that just produced the digest returns a `BlobRef` (it
knows its own algo); (2) durable-form parsers — the settlement key codec, the blob-path parser, the
manifest decoder, the envelope reader. Every other site COPIES BlobRefs. No site may assemble a
BlobRef from a digest and an algo obtained separately — this structurally kills both the
wrong-algo-into-BlobRef hazard and the W-DEP-SET cross-satisfaction hazard (§11), instead of
guarding them. Non-identity 128-bit hashes (`payload_digest`, `sourceEdgeId`, `RunRef.checksum`,
lease owners, build ids) are `UInt128` and are NOT blob identities — unaffected.

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

- `ManifestEntry` carries ONE identity field: `BlobRef ref` (not separate `algo` + `blob_hash`
  members — the pair is never split). Entry payload:
  `placement u8, algo u8, digest <lenFor(algo)> raw BE bytes, blob_size u64, inline_len u32, inline`
  (the two on-wire scalars decode into the single `BlobRef`).
- The per-manifest `blob_hash_len` header field (Phase 2 T3) is **removed** — width is per-entry,
  derived from the entry's `algo` via `blobHashLenFor`. Decode fail-closes on an unknown algo id
  (`CORRUPTED_DATA`). CA is pre-release: no back-compat path, no version bump.
- The Phase 2 T4 `foldManifestEdges` check `body.blob_hash_len == poolMeta().blob_hash_len` is
  replaced by per-entry validation: `entry.algo` must be a member of the pool's recorded algo SET
  (§5); an unknown/never-enabled algo in a manifest is `CORRUPTED_DATA` (same injection-route
  protection, per-entry granularity).

## 5. PoolMeta: write-algo + ever-used set {#poolmeta}

Replace the single fail-closed `blob_hash_algo` with (consulted 2026-07-11, §12):

- **Admission of a NEW algo is EXPLICIT OPT-IN; the default stays fail-closed (user decision).**
  A reopen whose configured `<blob_hash>` is NOT in the pool's `algos_used` FAILS with
  `BAD_ARGUMENTS` exactly like Phase 2 today ("pool has {ch128}; config requests sha256; set
  `<blob_hash_allow_new>1</blob_hash_allow_new>` to admit a new algo into this pool") — a changed
  config alone must never silently turn a pool mixed (config drift / copy-paste protection; admission
  is irreversible: permanent `algos_used` entry, per-subset dedup, reader-generation bump). With the
  flag set, the algo is admitted via the CAS-union below; once ADMITTED, subsequent opens with that
  algo need no flag (membership, not the flag, is the steady-state check). The flag gates admission
  only — it is not a persistent mode.
- **The write algo is NODE-LOCAL config, NOT durable pool state.** Two live nodes may intentionally
  write with different (already-admitted) algos, so no single truthful pool-wide value exists;
  persisting one would be misleading metadata + CAS churn. `PoolConfig`/`Store` carry it; `PoolMeta`
  does not.
- `algos_used` (repeated u8, append-only, canonically sorted): every algo ever ADMITTED to the pool.
  **Register-before-first-write**: a node MUST durably CAS-union its configured algo into `algos_used`
  (permitted by the flag above) BEFORE writing any blob/manifest/ref that names it. CAS-union loop: read+token -> already present?
  done -> insert -> CAS -> on conflict re-read and retry (recompute from the fresh value; never encode
  a stale whole `PoolMeta`). Union-only => no ABA. The `createOrValidate` creation-race loser UNIONS
  its algo instead of today's fail-close. First registration of a schema-3-bearing algo also raises
  `min_reader_generation` so pre-Phase-3 builds refuse the pool (they cannot read schema-3 runs).
- **Validation protocol (the stale-cache fix, §12.3)**: validators keep a MONOTONE local cache of
  `algos_used`. Check the cache; on a miss for a KNOWN-to-the-build algo, synchronously re-read
  `_pool_meta`; if the refreshed set contains it — accept and union into the cache; only if the
  AUTHORITATIVE set still omits it — `CORRUPTED_DATA`. Refresh on every miss (a long fold can overlap
  a later registration), and apply at the CENTRAL manifest-read boundary, not only in GC (else plain
  reads accept what GC rejects). Unknown-to-the-build algo id stays `NOT_IMPLEMENTED`.

`checkBlobHashAlgoMatches` is not deleted but RELAXED into the admission check: config algo
member of `algos_used` -> OK; not a member + flag set -> CAS-union (admit); not a member + no flag ->
`BAD_ARGUMENTS` (today's behavior, message now naming the flag). Its Phase 2 test keeps asserting the
default fail-close; a new test asserts flag-gated admission. The
`algos_used` membership check is kept deliberately: decode answers "does this BUILD understand algo 3?",
the registry answers "was algo 3 ADMITTED to this POOL?" — namespace/integrity admission against a
recognized-but-never-enabled or forged segment.

## 6. GC: algo-prefixed settlement keys (no per-algo partitions) {#gc-keys}

**Decision (user, superseding the first draft):** do NOT partition the run namespace per algo.
Settlement identity gains the algo dimension IN THE ROW KEY, inside the same `gc_shards` runs:

- **Source-edge key schema 3 — and schemas 1/2 CEASE TO EXIST** (user decision: "старого не должно
  существовать вообще; есть только новый"). The same change DELETES the schema-1/2 constants, the
  digest-first `key`/`parse`/`seekPrefix` code paths, and every `BlobDigest`-only keyed GC container —
  there is exactly ONE way to build/parse a settlement key and ONE identity type (`BlobRef`) after the
  change, so a "missed old-style call site" is a compile error, not a convention risk. No transitional
  shims (unlike Phase 2's staged `.toU128()`/`legacyBlobId128` migration): the replacement is atomic.
  A pool holding old-format runs is recovered by `SYSTEM CONTENT ADDRESSED GC REBUILD` (runs are
  derived state, rebuilt from manifests) — pre-release, no read path for old runs. Key layout:
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
- **Consult confirmations (§12)**: the run-file format needs NO change (records/blocks/footer are
  already variable-key-length; the writer already enforces non-decreasing raw keys); blob-prefix
  `seek(algo ++ digest)` is correct with mixed widths. The duplicate-sentinel guard and the merge
  group state must key on `BlobRef` (a `BlobDigest`-keyed guard would falsely merge `ch128:X` and
  `xxh3:X`). `blobShard` takes `BlobRef` and deliberately ignores `algo` internally (callers cannot
  accidentally discard identity). Codify key comparison as unsigned-octet lexicographic.

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
totality. No full TLA+ re-gate (consult §12.6): the models treat blobs as opaque atoms — re-run the relevant
model with `ch128:X`/`xxh3:X` as two distinct atoms to confirm no hidden digest-equality assumption.
The registry refresh race (§5) is covered by the concurrency gtest §9.8 (or a small dedicated state
model), not by reopening the settlement proof.

## 9. Testing gates {#testing}

1. Additive switch is flag-gated: create pool at `ch128`, insert; reopen at `sha256` WITHOUT the
   flag → `BAD_ARGUMENTS`, pool untouched (today's e2e stays green); reopen WITH
   `blob_hash_allow_new=1` → admitted, insert → both subsets readable; `algos_used={ch128, sha256}`;
   old data intact; third open at `sha256` WITHOUT the flag → OK (already admitted).
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
8. **Stale-registry race (consult §12.3, the hole's regression test):** GC node B opens with cached
   `algos_used={ch128}`; node A CAS-registers sha256 AFTER B's open and publishes a manifest with a
   sha256 entry; B folds it → must refresh `_pool_meta`, accept, and emit a schema-3 row with the
   correct `BlobRef`. (Constructing B after A's update misses the race — B must be open BEFORE.)
9. **W-DEP-SET cross-satisfaction (consult §12.5, the dangle-class risk):** one manifest referencing
   `ch128:X` and `xxh3:X` (same digest value), only ONE physical object present → dependency evidence
   for one must NOT let `promote` skip validation of the other; publish must fail-close, never commit
   a manifest whose other-algo twin is absent.
10. Run-file mixed-width micro-test: tiny block size; a 33→49-byte key transition exactly at a block
   boundary; sentinel + active rows; present and absent seek prefixes; an exact duplicate key spanning
   blocks.

## 10. Non-goals {#non-goals}

- Re-hashing/migrating existing blobs to the new algo (explicitly rejected — the whole point).
- Cross-algo dedup (content-equality across algos is unknowable without re-hashing).
- Per-table/per-insert algo selection — the switch is pool-level (disk config), coarse by design.
- Removing algos from `algos_used`.

## 11. Effort & risk {#effort}

Medium-small (down from the first draft: no run-namespace change, no seal change, no partition
lifecycle). The bulk is the schema-3 key codec + `BlobRef` plumbing through fold/GC/meta/fsck and the
per-entry manifest algo; PoolMeta relax is mechanical. The old forms (schemas 1/2, digest-first
helpers, `BlobDigest`-only GC identity) are DELETED in the same change (§6), so "building a key the
old way" is a compile error by construction — the residual risks are (a) a call site passing the
WRONG algo into a `BlobRef` (e.g. `write_algo` instead of the row's own algo — compiles, parses,
names a different blob; mitigated by pair-typed `key(BlobRef, source_id)` with no separate algo
parameter, plus the mixed-pool crux test §9.3 which goes red on any such hardcode), and (b) stale
cached `PoolMeta.algos_used` fail-closing the fold on a legitimately new algo (consult question).
Two-consult review of the schema-3 task before implementation (Phase 2 T4 discipline).

## 12. Adversarial consult record (2026-07-11, codex xhigh) {#consult}

Full reply: job scratch `consult_phase3_reply.md`. Verdict: ordering and "no algo loop in settlement"
claims SOUND; rev.2/3 NOT ready unchanged — `algos_used` is mutable durable state validated through an
immutable local snapshot. Findings folded into §5/§6/§9/§11 above:

1. **Total order proof confirmed** (algo byte decides before widths can interact; BE digest ==
   magnitude; sentinel-first; writer already enforces non-decreasing appends). Duplicate-sentinel
   guard must be `BlobRef`-keyed.
2. **Run-file format already variable-key-capable** — no format change; blob-prefix seek correct.
   Found a PRE-EXISTING, schema-independent `RunFileReader::seek` contract bug (an exact full key
   duplicated across a block boundary can be skipped: seek picks the LAST block with `min_key <=
   target`; fix = FIRST block with `max_key >= target`). Latent today (all current seeks are
   blob-prefix). Fix independently of Phase 3; micro-test §9.10. Footer-size budgeting commentary
   needs updating for larger keys.
3. **Stale `algos_used` race confirmed** (the biggest hole) → §5 register-before-write +
   monotone-cache + refresh-on-miss at the central manifest-read boundary; test §9.8.
4. **PoolMeta concurrency**: CAS-union loop is lost-update-free, no ABA (union-only); creation-race
   loser unions; `write_algo` must NOT be durable pool state (node-local only);
   `min_reader_generation` bump on first schema-3 registration.
5. **Pair-keying enumeration** (all become `BlobRef`): ManifestEntry, PendingBlob/findPendingBlob/
   referenced_hashes/carry-forward; Build DepKey + recordPendingBlobDep/depIsTokened/
   isCopyForwardableTokenless/keyFor + promote/revalidation/meta chain; Layout/blobKey/blobMetaKey/
   objectKey/locate (Layout can no longer capture one pool-wide algo; DigestCodec selected from
   `ref.algo`); BlobDelta/BlobCandidate/RetiredEntry/ReplacedEntry/preview/head-callbacks/
   inDegreeInGeneration/deletes/.meta jobs/outcomes; rebuild `edge_bearing` + `condemn_seeded`; fsck
   sets + path parse -> BlobRef; dedup cache; inspect/event log render `algo:hex`. `sourceEdgeId`
   stays algo-free (it names the source occurrence; the edge key already carries `BlobRef`).
   **Highest-risk site = the writer W-DEP-SET** (dangle-class), NOT the dedup cache (leak-class).
   NOTE (rev.5, user decree §2): this enumeration is not an opt-in migration checklist — it is the
   mechanical CONSEQUENCE of deleting digest-only blob identity; the compiler enforces totality once
   the digest-only overloads/containers are gone.
6. **TLA**: no full re-gate; re-run with two distinct atoms; race covered by gtest §9.8.
