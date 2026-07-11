---
description: 'Design for an opt-in S3-native staging area for CAS MergeTree blob writes: stream the blob to a per-mountpoint S3 staging key while hashing, then promote to the content-addressed key via a WRITE-ONCE conditional server-side copy — eliminating the local-disk round-trip for large blobs. Capability-probed, fail-close to local staging, off by default.'
sidebar_label: 'CAS S3-native staging area'
sidebar_position: 21
slug: /superpowers/specs/cas-s3-native-staging
title: 'CAS S3-native staging area (conditional-copy promote)'
doc_type: 'reference'
---

# CAS S3-native staging area — design {#title}

**Status:** DESIGN (2026-07-11). **Branch:** `cas-gc-rebuild`. **Feature flag:** per-disk,
**OFF BY DEFAULT**. Derived from two independent strong-model consults (fable + adversarial opus) whose
synthesis is recorded below; the adversarial consult refuted the naive unconditional-copy design and is
the reason this spec mandates a **write-once conditional** promote.

## 1. Problem & motivation {#motivation}

A CAS blob's final object key is `blobs/<2-char-shard>/<hex hash of full content>` — content-addressed,
so the key is unknowable until every byte has been written and hashed. Today (`ContentAddressedWriteBuffers.cpp`,
`ContentAddressedTransaction.cpp`) a blob part-file (column data / marks / `primary.idx`, above the 1 MiB
`INLINE_CAP`) is therefore:

1. streamed to a **local temp file** under `scratchPath()` (`<data>/disks/<name>/cas_scratch/`) while a
   `HashingWriteBuffer` computes its `cityHash128` (`CaContentWriteBuffer`);
2. on commit, **read back** from that local file and uploaded to the object store via
   `putBlob → putIfAbsentStream → IObjectStorage::writeObject` (a conditional `If-None-Match:*` create).

For a wide part this means the bytes hit the **local disk** once (write) and are then re-read and pushed to
S3 — a full local-disk round-trip and a second pass over the data, plus the local scratch capacity has to
hold in-flight parts. On a node with small/fast-but-scarce local storage this is the dominant write cost.

**Goal:** for large blobs, stream the bytes **directly to a staging object in the same S3 bucket** while
hashing, then **promote** staging → `blobs/<shard>/<hash>` with a **server-side copy** (no data through the
CH process), then delete staging. This removes the local-disk round-trip and the read-back pass.

**Non-goals:** changing the read path; changing GC settlement semantics; changing the default write path
(this feature is opt-in); supporting server-side copy on non-object backends (Local/Emulated keep local
staging); the pluggable-hash work (separate campaign task 3).

## 2. The hard constraint that shapes everything: promote must be WRITE-ONCE {#write-once}

The tempting design — "stream to staging, then unconditionally `copyObject(staging → blobKey)`" — is
**unsafe**. The adversarial consult established three independent reasons, each grounded in the code:

1. **Silent corruption.** The read path **never re-hashes** a blob body (`CasBuild.cpp:68`: "the core
   otherwise NEVER re-hashes payloads"). An unconditional copy **overwrites** an object that may be
   concurrently open for read. On any store whose overwrite is not guaranteed atomic-replace, a straddling
   `GET` can return a torn/short body that is then accepted as correct data — worse than a 404.
   `putIfAbsentStream` never has this problem because it **never overwrites** (conditional create).
2. **Chronic leak.** For a multipart copy (>5 GB) the destination ETag is `<...>-<partcount>`, **not**
   `MD5(content)`, and depends on part boundaries. Two writers racing to promote identical content produce
   **different** ETags; the incarnation token (= the S3 ETag) recorded by the loser is stale forever, so the
   add-only exact-token GC delete (`DeleteObject If-Match:<etag>`) gets a 412 and **can never reclaim** that
   key — one un-reclaimable orphan per content-key that ever raced (the `resurrect-reupload-orphan` class).
3. **Resurrect-race data loss.** A "HEAD blobKey; if present, adopt token, skip copy" dedup shortcut that
   **omits the condemn check** lets a writer freshly reference a **condemned** blob (pending GC delete)
   without re-establishing edge-before-observe → GC's delete fires → data loss.

**Key reframing (both consults agree):** the exact-token `If-Match` delete is a **leak knob, not a safety
knob**. Because content-addressed blobs of identical content share a single-part ETag `MD5(C)`, a stale
delete already "matches" a fresh identical incarnation today. Data safety comes from **edge-before-observe +
resurrect-from-source**, verified in code: the read side carries no tokens at all (`CasStore.h:266`,
`CasStore.cpp:761`). Server-side copy therefore must not be judged by "does the ETag guard hold" but by
"does edge-before-observe and the resurrect/condemn gate still hold, and do we ever overwrite a live object."

**Conclusion:** the promote must preserve `putIfAbsentStream`'s **write-once** semantics exactly. The only
safe server-side copy is a **conditional** one: `If-None-Match:*` on `CopyObject` and, for the multipart
path, on `CompleteMultipartUpload`. A loser gets `PreconditionFailed` and adopts the winner — identical to
today. No live object is ever overwritten.

## 3. Capability probe + fail-close {#capability}

Conditional copy is **not universally supported**. AWS added `If-None-Match` to `CopyObject`/`CompleteMultipartUpload`
relatively recently, and the codebase already has a **measured** case of an S3-compatible surface (GCS
sigv4, `PocoHTTPClient.h:290`, measured 2026-07-03) that **silently ignores** `If-None-Match`/`If-Match`. A
backend that silently drops the conditional turns "atomic exclusive promote" into an unconditional overwrite
**while the code believes it won exclusively** — reintroducing every §2 hazard with false confidence.

Therefore:

- **Mount-time capability probe** (extend the existing `CasProbe`): PUT a tiny object, attempt a conditional
  copy / conditional CMU-copy over it with `If-None-Match:*`, and **demand a real 412**. RustFS honoring
  `If-None-Match` on `PutObject` (which the soak already relies on) does **not** imply support on the copy
  path — the probe must exercise the copy path specifically.
- **If the probe confirms 412 semantics → S3-native staging is available** for this disk (when opted in).
- **If it does not → fail CLOSE to today's local staging + `putIfAbsentStream`.** Never silently degrade to
  unconditional-overwrite copy — that is the fail-open path CLAUDE.md forbids. The disk logs one line at
  mount that S3 staging was requested but is unavailable on this backend, and uses local staging.

This makes the empirical question **"does RustFS honor conditional copy?"** a **Phase-0 gate** of the
implementation plan: if it does not, the S3-staging path is not exercisable in our test harness (RustFS) and
the feature would ship dark. The plan's first task verifies this against a real RustFS before any code.

## 4. Configuration (off by default) {#config}

Per-disk config under the CAS disk block:

- `cas_staging_backend` = `local` (**default**) | `s3`. `local` ⇒ **byte-for-byte the current path**, zero
  behavior change, no probe, no new code path taken. `s3` ⇒ opt-in to S3-native staging (subject to the
  capability probe; falls back to `local` if unsupported).
- `cas_s3_staging_min_bytes` (default e.g. `64 MiB`; only meaningful when `cas_staging_backend=s3`): blobs
  **below** this size keep the local path (or the memory fast-path, §7); only blobs **≥** this go to S3
  staging + conditional-copy promote. Confines the copy-promote machinery to large blobs (where the
  local round-trip actually hurts) and keeps the high-cardinality small-blob churn on the atomic 1-op
  `putIfAbsentStream`. Inline blobs (≤ `INLINE_CAP` = 1 MiB) are unaffected — they ride the tree object.

When `cas_staging_backend=local`, none of §5–§8 applies.

## 5. Staging object lifecycle {#lifecycle}

Per-mountpoint staging prefix: `POOL/staging/<mount_id>/<random>.tmp`, where `mount_id` is the disk's stable
mount identity (reuse the writer/mount incarnation identity already threaded for GC fencing — the same value
that names a mount's writes; it must be stable per mountpoint and distinct across mounts/servers).

1. **Stream to staging.** `CaContentWriteBuffer` (s3 mode) writes bytes to `writeObject(StoredObject(staging_key))`
   through a `HashingWriteBuffer`, computing `cityHash128` during the upload. On finalize the hash is known.
   `on_finalized(hash_hex, size, staging_key)` records `PendingBlob{hash, staging_key, size, backend=s3}` —
   the `staging_key` replaces `temp_path` in the existing `PendingBlob` record; the rest of the staging
   bookkeeping (`stageBlobPartFile`, dep registration, `ManifestEntry{placement=Blob}`) is unchanged.
2. **Promote (write-once conditional copy).** In `publishStaging`, for each referenced pending blob, run the
   **existing condemn/resurrect gate** with a copy-based upload primitive:
   - Conditional `copyObject(staging_key → blobKey, If-None-Match:*)`:
     - **Created (2xx):** capture the destination ETag **from the copy response** (extend `copyS3File` to
       return the `CompleteMultipartUpload`/`CopyObject` ETag it already reads at `copyS3File.cpp:595`/`:811`
       but currently discards — avoids an extra HEAD on the hot miss path). Token = `{etag, ETag}`.
     - **PreconditionFailed (blob already exists):** HEAD `blobKey` to observe the current token, then run
       the **same** `observeAndAdmit` / condemn-check path `putBlob` uses today (`CasBuild.h:46-49`): if the
       existing object is **condemned**, resurrect by copying **from our own staging object**
       (`copyForwardFromSource`-equivalent), **never** by reading the condemned `blobKey`
       (`feedback_ca_resurrect_invariant`). Fail-closed on ambiguity.
3. **Delete staging.** Only **after** promote + condemn-check completes for that blob (staging is the
   resurrect source and must outlive the gate). `cleanupPendingTempFiles` is extended to delete S3 staging
   objects (via `removeObject`) for `backend=s3` pending blobs, mirroring the local `fs::remove`.

`promoteBuild` (manifest becomes visible) must **strictly follow** copy success — multipart destinations are
invisible until `CompleteMultipartUpload`, so this is naturally atomic provided the code never reorders
visibility ahead of copy completion. Reads remain content-hash-only; **no** read path may gate on a token/ETag.

## 6. Crash & orphan handling {#crash}

Two new crash obligations (both data-safe if honored):

- **Crash between copy-complete and staging-delete** → a leaked staging object. Bounded per mount by the
  `<mount_id>` prefix. Reclaimed by a **mount-lease-scoped staging sweeper**: a mount sweeps its **own**
  `staging/<mount_id>/` on start, and GC age-sweeps staging objects older than a TTL. The sweeper is
  **lease-fenced** — a janitor racing a live slow writer's in-flight staging must not delete it; an expired
  lease means the writer is fenced anyway, and a promote whose `copyObject` then sees source-404 **fails
  closed** (abort the insert, no commit) — the correct outcome.
- **Crash between `precommitAdd(edge)` and copy** → a precommit edge references a `blobKey` whose object does
  not yet exist. Safe **iff** crash recovery **never promotes a precommit whose copy is not verifiably
  complete** — recovery must re-drive the copy from the surviving staging object or abort. This is sharper
  than today because blob existence now depends on a separate copy step; it must be asserted, not assumed.

**GC exclusion:** GC blob discovery LISTs the `blobs/` prefix; `staging/` is a distinct prefix and is
naturally excluded from the fold. The staging sweeper is the **only** reclaimer of `staging/` — GC must never
treat a staging object as an orphan blob to exact-token-delete.

## 7. Optional: memory fast-path for small blobs {#memory-fastpath}

(Consult-recommended, separable — may be a follow-on task.) When `cas_staging_backend=s3`, a blob smaller
than the single-part threshold can be **buffered in memory** while hashing and then written with today's
`putIfAbsentStream` (1 op, atomic dedup, **no disk, no staging, no copy**). This is strictly better than
either local or S3 staging for the dominant small-blob population and confines **every** §2/§5 copy-promote
hazard to genuinely large (multipart) blobs. If included, `cas_s3_staging_min_bytes` is the spill threshold:
below it → memory + `putIfAbsentStream`; at/above → S3 staging + conditional-copy promote.

## 8. Backend abstraction {#backend}

Add to `Cas::Backend` (`Core/CasBackend.h`) a Native-only capability + two seams:

- `bool supportsConditionalCopy()` — set by the mount-time probe (§3).
- `WriteSinkPtr stageStream(const String & staging_key)` — an unconditional streaming write to a staging key
  (staging keys are unique-per-attempt, so no conditional needed on the staging write itself).
- `PutResult promoteStaged(const String & staging_key, const String & blob_key)` — conditional
  (`If-None-Match:*`) server-side copy staging→blob via `copyS3File` (multipart-safe, >5 GB), returning
  `{Done, token=destETag}` or `{PreconditionFailed, {}}`. Emulated/Local backend: `supportsConditionalCopy()`
  = false ⇒ the metadata layer uses local staging, so these seams are unused there.

`copyS3File` gains an out-parameter (or a small result struct) surfacing the destination ETag it already
computes, and threads an optional `If-None-Match` header into `fillCopyRequest` /
`CompleteMultipartUpload` for the conditional promote.

## 9. Invariants (carried forward, must not regress) {#invariants}

- **Never overwrite a live blob object** (§2) — promote is write-once/conditional or it is local.
- **Edge-before-observe** unchanged: `precommitAdd(edge) → stage → promote(copy)+condemn-check → delete
  staging → promoteBuild(visible)`. Manifest never visible before the blob object exists at `blobKey`.
- **Resurrect source is the staging object, never a GET of a condemned `blobKey`**
  (`feedback_ca_resurrect_invariant`). No `copyObject(blobKey→blobKey)` refresh.
- **Reads are content-hash-only** — no read/fsck path gates on a token/ETag (multipart ETags are not
  content-MD5; a token-based read-time integrity check would be a false-positive data-loss generator).
- **Fail-close, never fail-open**: unsupported conditional copy ⇒ local staging, never unconditional copy.
- **Default path byte-for-byte unchanged** when `cas_staging_backend=local`.

## 10. Testing {#testing}

- **Phase 0 (gate):** empirically verify RustFS honors conditional `CopyObject` **and** `CompleteMultipartUpload`
  (`If-None-Match:*` → real 412). Decides whether the primary path is testable in-harness. If RustFS lacks
  it, either add a MinIO-backed test lane that does, or scope the feature to the local-fallback floor and
  surface that to the user.
- Gtest (`src/Disks/tests/`): probe true/false → path selection; conditional-copy Created vs PreconditionFailed;
  promote-over-condemned resurrects from staging (not from blobKey); crash-before-copy recovery re-drives or
  aborts; staging sweeper reclaims orphans and is lease-fenced.
- Integration (`with_rustfs`): opt-in `cas_staging_backend=s3` disk, large-blob INSERT, verify blob lands at
  `blobs/<shard>/<hash>` and `staging/` is empty post-commit; dedup race (two replicas, same content) leaves
  one blob and no chronic leak.
- Soak: an S3-staging variant lane (phase-3 chaos), asserting `dangling==0` and no `staging/` accumulation.

## 11. Consult synthesis (for the record) {#consults}

- fable (`a18e1b0f`): reframed single-part ETag = `MD5(C)` determinism, proposed `D`+deterministic-part-layout,
  flagged the edge-first ordering as the thing most likely gotten wrong, and the GCS silent-ignore measurement.
- opus-adversarial (`aeda856e`): **refuted** unconditional copy — silent-corruption via live-object overwrite
  (read path never re-hashes), chronic non-self-healing multipart leak, resurrect-race data loss if the dedup
  shortcut skips the condemn gate; verdict = **conditional copy (`B`) + return-ETag, capability-probed, fail-
  close to local**, plus the staging sweeper and crash-recovery obligations. This spec adopts the opus verdict.
