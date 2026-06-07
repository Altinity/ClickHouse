---
description: 'Technical reference for the content-addressed (CA) MergeTree storage backend on the cas-mergetree-poc branch — the on-object-storage layout/protocol, the write protocol, and the lock-free reference-counting GC, with a precise account of how concurrent writers and the background sweep coexist without a shared mutex and why that is race-free.'
sidebar_label: 'CA protocol and lockless GC'
sidebar_position: 20
slug: /superpowers/reports/ca-protocol-and-lockless-gc
title: 'Content-Addressed MergeTree — On-Storage Protocol and the Lock-Free GC'
doc_type: 'reference'
---

# Content-Addressed MergeTree — On-Storage Protocol and the Lock-Free GC {#ca-protocol-and-lockless-gc}

This document describes the content-addressed (CA) MergeTree storage backend on the `cas-mergetree-poc`
branch: how parts and their column files are laid out content-addressed on shared object storage, how a part
is written and committed, and — its main subject — how concurrent writers and a background garbage-collection
(GC) sweep run with **no shared mutex on the hot path** and why that is safe. It is grounded in the source
under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` and the design specs under
`docs/superpowers/specs/`. Where the *implemented* code deliberately differs from the *north-star* design,
this document states the implemented behavior and flags the gap, because the safety argument depends on it.

## Table of contents {#toc}

- [1. Overview and goals](#overview)
- [2. On-object-storage layout / namespace](#layout)
- [3. The write protocol](#write-protocol)
- [4. The GC protocol](#gc-protocol)
- [5. Concurrency without locks](#concurrency)
- [6. Why it is safe / race-free](#safety)
- [7. Failure / crash safety](#failure)
- [8. Implemented-vs-north-star gaps and ambiguities](#gaps)

---

## 1. Overview and goals {#overview}

CA storage is a new `metadata_type = content_addressed` for an object-storage disk (the same extension seam
`plain_rewritable` uses). A normal non-replicated `MergeTree` points at a disk with this metadata type and
needs no engine or DDL change. The on-store model is "Git for MergeTree":

- **Blobs** — each part *file*'s bytes (the `.bin`, marks, `primary.idx`, `columns.txt`, …) are stored once,
  content-addressed by the file's existing `checksums.txt` `cityHash128` (plus size). Identical content
  across parts, merges, and mutations **deduplicates** to the same blob.
- **Manifests** — a per-part descriptor (`PartManifest`) that maps each logical file to its blob hash. The
  manifest is addressed by `part_id`, a content-only `SipHash-128` over the deterministic file subset
  (excluding the mutable per-part files `uuid.txt`, `txn_version.txt`, `metadata_version.txt`).
- **Refs** — the only mutable, listable records; the *commit point*. A ref names a part and points at a
  `part_id`. Refs are the GC roots.

This replaces zero-copy replication: because `parts/` + `blobs/` are a **global per-pool** content pool and a
ref is just a pointer, a second server "fetches" a part by publishing a ref to an already-present `part_id` —
no byte copy. `remove` is a pure pointer-unlink; blobs are reclaimed only by a deferred reachability GC,
never inline.

The design goals that govern the GC (from `2026-06-04-ca-gc-convergence-design.md` §1):

- **G1 — Non-blocking writers.** Writers must not block each other or the sweep on the common commit path. A
  shared in-process `gc_lock` once serialized every commit against the whole sweep; it has been demoted to a
  narrow container guard (see §5).
- **G2 — ABA-safe reclamation.** A GC delete can never kill a concurrent re-use of the same content
  (generations + tombstones).
- **G3 — No bucket-wide scan on the normal path.** GC works from a candidate stream (the compaction's
  count-0 keys + a per-shard sealed index), not a full `LIST` of `parts/` + `blobs/`.
- **G4 — S3 is the source of truth.** All durable state lives in the bucket and is self-describing;
  coordination uses only the object store's create-if-absent primitive. Keeper is an optional accelerator
  holding no durable state (not implemented on this branch — the implemented coordination is bucket-only).

---

## 2. On-object-storage layout / namespace {#layout}

The single source of truth for every object key is `PoolPaths.h`/`.cpp` and `GcLayout.h`/`.cpp`. Every key
builder takes the object-storage common key prefix as its first argument; an empty prefix yields the bare key
shown below. One disk root = one pool = one GC coordinator.

```
<pool root>/
  blobs/<H0>/<H1>/<H>/                         one content hash H = a small keyspace (fan-out on <H>)
    <g>                                          immutable blob bytes at generation g (g=0 common)
    <g>.tombstone                                GC-owned seal/gravestone for generation g
    active                                       best-effort preferred-generation HINT (plain PUT, not CAS)
  parts/<p0>/<p1>/<part_id>/                   one part_id = a small keyspace, symmetric to a blob
    <mg>                                         the manifest body (pins BARE H) at generation mg (mg=0 common)
    <mg>.tombstone                               GC-owned seal/gravestone for manifest generation mg
    active                                       best-effort preferred-generation HINT
  store/<server_id>/                           per server/replica (refs never collide across mounters)
    <uuid[:3]>/<uuid>/refs/
      <part_name>                                LIVE ref → part_id (the commit point; GC root)
      <part_name>.meta                           per-ref sidecar bundle (RefSidecar: mutable per-part files)
      <part_name>.<file>.meta                    per-mutable-file object (verbatim bytes for the read path)
    <uuid[:3]>/<uuid>/refs/detached/<name>       detached ref (GC root, not in the active set)
    <uuid[:3]>/<uuid>/files/<tail>               table-level verbatim files (format_version.txt, …)
  shadow/<backup>/store/<uuid[:3]>/<uuid>/refs/<part>   FROZEN ref — GC root, table-lifetime-independent
  sessions/<session_id>                        in-flight write-session PIN (GC root)
  gc/current_epoch/<shard>                     per-shard open epoch counter (single writer = shard GC leader)
  gc/log/<epoch>.<shard>/<event_id>            coalesced +/- delta objects (pins inlined, (H,g)-resolved)
  gc/snap/<padded-epoch>.<shard>               sorted (kind,identity,generation)→count run (the reverse index)
  gc/sealed/<shard>/<identity>.<generation>.<b|p>   sealed-but-unswept tombstone index (Scan-A replacement)
  _pool_meta                                   PoolMeta record (pool format version / pool uuid)
  _pool_meta.mounters/<server_id>              per-mounter registry (listable mounter set)
  fence/<n>                                     monotonic fence-token allocator (tiny object per token)
  gc.lock                                      GC-leader lock record (server_id, lease_deadline, fence_token)
```

Roles, authors, and lifecycles:

- **`blobs/<H>/<g>`** — immutable file bytes. Written by a committing writer via `ContentAddressedWriteBuffer`
  (build-local, hash, then upload once with put-if-absent). Reclaimed only by the GC sweep. `g=0` is the
  common case; a `g>0` object appears only after the GC condemned `g` and a contended writer *resurrected*.
- **`blobs/<H>/<g>.tombstone`** — the GC's single-owner seal. Created only by the fenced GC leader via
  `condCreateIfAbsent`. One object, three fates (`ContentAddressedGC::sweepCandidates`): sealed → **swept**
  (kept forever as a gravestone), → **recovered** (deleted/un-sealed when a live ref/session still needs `g`
  and no successor exists), or → **drained** (kept, blob kept, no re-open, a successor already exists).
  Writers never delete a tombstone — on seeing one they resurrect.
- **`blobs/<H>/active`** — a plain best-effort PUT, not a conditional update. Absent in the common `g=0`
  case (readers default to 0). Written lazily on resurrection (advance) and on sweep (reset off the swept
  generation). A stale `active` is repaired by the reader's `404`→`LIST` fallback, never treated as
  corruption (`PoolPaths.h`, I7d).
- **`parts/<part_id>/<mg>`** — the manifest, treated as "just another content-addressed object" with the
  identical generation/tombstone lifecycle as a blob (the convergence spec §9: this closes the
  relink-after-full-drop ABA hole that a fixed `parts/<part_id>` key would have). The manifest *body* still
  pins bare `H`, so `part_id` stays a pure content function and dedup is preserved.
- **`store/<server_id>/<uuid>/refs/<part_name>`** — the live ref, the commit point. Written last in commit;
  unlinked first in drop. Its payload is a `RefPayload` (`part_id` + header fields). The `.meta` sidecar
  (`RefSidecar`) holds the mutable per-part files; the `<file>.meta` per-file objects back the byte reads.
  Both `.meta` variants live under the same `refs/` prefix so a ref-scoped `removeRecursive` reclaims them
  and the reachability sweep (which scans only `blobs/`+`parts/`) can never miss them.
- **`shadow/<backup>/…/refs/<part>`** — a frozen ref. An independent GC root with a table-lifetime-independent
  lifetime (survives `DROP TABLE`, like local `shadow/`).
- **`sessions/<session_id>`** — the in-flight write-session pin (`WriteSession`). Written by a committing
  writer *before* uploading blobs; lists the `pending` bare hashes (and a `part_id`) an uncommitted part
  intends to reference. The GC treats every unexpired live session's pending set as roots. Carries
  `lease_deadline_unix` (a liveness hint), `committed`, `delta_epochs`, and the `#2` sticky-fail fields.
- **`gc/current_epoch/<shard>`, `gc/log/<epoch>.<shard>/`, `gc/snap/<padded-epoch>.<shard>`** — the
  log-structured reverse index (see §4). Authored by writers (`gc/log` deltas) and the fenced GC leader (epoch
  close + snapshots).
- **`gc/sealed/<shard>/…`** — a compact index of open (sealed-but-unswept) tombstones. The seal step adds an
  entry; sweep and recover remove it. The sweep LISTs only this index instead of the whole tree (G3).
- **`_pool_meta`, `_pool_meta.mounters/<server_id>`, `fence/<n>`, `gc.lock`** — pool identity, the mounter
  registry, the fence-token allocator, and the GC-leader lock. All built on `condCreateIfAbsent`
  (`PoolCoordination.*`).

**Locality by author.** Everything about one `H` (its generations, their seals, the `active` hint) is
co-located under `blobs/<H0>/<H1>/<H>/`, so a single `LIST` of that prefix returns existence + tombstone +
active together — serving dedup, generation resolution, and the writer's handshake re-check in one op. The
writer-authored counterpart is `sessions/<id>`. The two-flag handshake (§5) is the protocol *between* these
two authors.

---

## 3. The write protocol {#write-protocol}

A part is built in local scratch (the merge/insert runs entirely locally; the object store sees nothing until
commit). `ContentAddressedWriteBuffer` wraps a `HashingWriteBuffer` that computes each file's `cityHash128`
as bytes stream to a local temp file; on `finalizeImpl` it derives `blobs/<H>/<g>` and uploads exactly once
with put-if-absent (dedup). The commit then runs the §7.1 handshake order from the convergence spec, as
implemented in `ContentAddressedTransaction::commit` / `commitOnePart`:

```
WRITER (ContentAddressedTransaction::commit / commitOnePart)            object affected
─────────────────────────────────────────────────────────────────     ───────────────────────────
1. resolve H→g and part_id→mg per the `active` hints (default 0)
2. raise/renew the durable WriteSession  ── the handshake FLAG ──►      PUT sessions/<id>   (persistSession)
   (already raised per-blob during the build via OnPinBlob; step 2
    re-asserts it is durable BEFORE any upload/recheck/+)
3. upload missing blobs (put-if-absent)                          ──►    PUT blobs/<H>/<g>
   condCreateIfAbsent the manifest at the resolved mg           ──►    PUT parts/<part_id>/<mg>
4. RE-CHECK the .tombstone for every pinned (H,g) and (part_id,mg)      HEAD/GET blobs/<H>/<g>.tombstone …
5. if any tomb present → resurrect to g+1 / mg+1, retry from (3)       (resolveAndResurrectGeneration loop)
6. enqueue the `+` delta AFTER the re-check (resolved (H,g) pins) ──►    PUT gc/log/<epoch>.<shard>/<event_id>
   (appendAndFlushForCommit; records settled (shard,epoch))            (coalesced; +before-ref preserved)
7. publish the LIVE REF  ── the commit point, written LAST ──────►      PUT store/<srv>/<uuid>/refs/<part>
8. mark session committed + record delta_epochs; release it ONLY
   once every `+` epoch is FOLDED into a durable snapshot         ──►    DELETE sessions/<id> (when folded)
```

Key points:

- **Pin-before-upload.** The session pin (step 2) — and, in-process, the `in_flight_pinned_blobs` set taken
  by `ContentAddressedWriteBuffer` *under* the container `gc_lock`, *before* the dedup existence-check — is
  raised before the blob is uploaded or a dedup-skip is decided. This is what makes the blob reachable to a
  concurrent sweep before any ref names it.
- **Ref published last.** The live ref is the commit point and the last write. A crash before it leaves the
  part not-live (see §7).
- **`+` before ref.** The `+` delta is flushed before the ref (`appendAndFlushForCommit`), so *ref exists ⇒
  delta exists* (I1/I6). The implemented commit path flushes synchronously before the ref.
- **Generations and resurrection.** `resolveAndResurrectGeneration` (`ContentAddressedTransaction.cpp:~1380`)
  is the bounded loop: ensure `g` exists, re-check its tombstone; if sealed, `condCreateIfAbsent blobs/H/(g+1)`
  (a different physical key — ABA-proof), best-effort advance `active`, and retry. It settles the moment it
  reaches an unsealed generation. A fresh `g=0` blob has no prior `+`, so the compaction can never have
  emitted it as a candidate, so it cannot be sealed — the re-check is a no-op on the common path.
- **Carry-forward (mutation).** `MutateTask` is unchanged; its `createHardLinkFrom` for unchanged columns
  means, on this metadata type, that the new manifest entry points at the same blob hash — a reference, no
  re-upload. The source part stays live through the mutation, so its carried-forward blobs are continuously
  reachable; the new part commits referencing them before the source goes outdated.

---

## 4. The GC protocol {#gc-protocol}

### 4.1 The log-structured reverse index {#reverse-index}

The reverse index (blob/manifest → referrers) is **never materialized as a refcount**. It lives implicitly
inside a streaming compaction (`GcCompaction`):

- **Snapshot** `gc/snap/<padded-epoch>.<shard>` — a sorted run of `CountKey{kind, identity, generation} →
  count`, the durable folded state. `kind` is `Blob` or `Part`; `identity` is the bare hex digest;
  `generation` is the resolved physical `g`/`mg` (generation-aware since S3, so `(H,g)` and a resurrected
  `(H,g+1)` are counted independently).
- **Delta log** `gc/log/<epoch>.<shard>/<event_id>` — commit appends a `+` and drop appends a `-`
  (`GcDelta`), pins inlined and `(H,g)`-resolved, plus a `(part_id, mg)` manifest edge (§9). Writes are
  **coalesced** (group-commit, `GcLogWriter`): one object per `(shard, window)`, keyed by a stable
  `event_id = SipHash-128(op, generation, part_id)` so a re-append dedups on fold. Deltas are split by
  hash-prefix shard (`shardForHash`); the manifest edge goes to the part's home shard (`shardForPartId`).

### 4.2 The epoch compaction (count / merge-join) {#compaction}

`GcCompaction::compactShard` folds one shard's open epoch:

```
   gc/snap/<E>.<shard>            gc/log/<E>.<shard>/  (LISTed AFTER the close)
   (sorted by CountKey)           (decoded, external-sorted by CountKey, deduped by event_id)
        │                                │
        └──────────  streaming merge-join (walk both in lockstep)  ──────────┘
                                │
                ┌───────────────┴───────────────┐
                │ sum counts per key, dedup +    │
                │ stream gc/snap/<E+1>.<shard>   │
                └───────────────┬───────────────┘
                                │  any key whose RUNNING COUNT hits 0
                                ▼
                          count-0 CANDIDATE  (falls out of the merge — no decrement-to-zero queue)
```

1. **Close the epoch** — a plain fenced PUT `gc/current_epoch/<shard> = E+1` (single writer = the shard's GC
   leader; the fence lease serializes it, so no CAS — §5.1 rule 1). Only after the close is `E` LISTed and
   folded, so the LIST sees a stable, complete epoch.
2. LIST + decode `gc/log/<E>.<shard>/`, external-sort by `CountKey`, dedup by `event_id` while folding.
3. Merge-join against the sorted snapshot, stream the new `gc/snap/<E+1>.<shard>`, emit count-0 candidates.
4. Reclaim the old snapshot + log objects (idempotent under a leader crash).

The compaction reads only `gc/snap` + `gc/log` — never `LIST blobs/` (G3). The full bucket scan survives only
as `runReconciliationScan` (the rare rebuild / abandoned-upload / orphan-drift fallback,
`reconciliation_cadence_rounds`, default 0 = off).

### 4.3 The sweep state machine {#state-machine}

`runSweepOnce` collects candidates from three sources (compaction count-0 stream + `collectSealedTombstoneCandidates`
from the `gc/sealed` index + optionally the reconciliation scan), then `sweepCandidates` runs the
generation-aware tail per candidate:

```
   count-0 candidate (H,g)  /  re-presented sealed tombstone
                │
                ▼
        ┌──────────────────┐  condCreateIfAbsent(<g>.tombstone)  + add gc/sealed/<shard> entry
        │      SEAL        │  durable, single-owner, fence-gated. After this, NO new attach may target g
        └────────┬─────────┘  (the writer's tomb re-check routes reuse to g+1 → I4).
                 │
        ┌────────▼─────────┐
        │      GRACE       │  liveness ageing from the seal (NOT a safety fence — first_unreachable timer)
        └────────┬─────────┘
                 │
        ┌────────▼───────────────────────────────────────────────┐
        │  FRESH AUTHORITATIVE RE-CHECK (computeReachability)     │  AFTER the seal: refs → manifests reachable
        │  = live refs → manifests  ∪  live session pins          │  blobs ∪ live sessions' pinned blobs/parts
        └────────┬───────────────────────────────────────────────┘
                 │
   ┌─────────────┼──────────────────────────────┬─────────────────────────────┐
   ▼             ▼                                ▼                             
 SWEEP         RECOVER                          DRAIN
 (id NOT       (id reachable AND no successor    (id reachable BUT a successor
  reachable)    generation exists)                g+1… already exists)
 delete <g>,   delete <g>.tombstone (un-seal),   keep tombstone AND keep <g>
 reset active   re-open g as attachable;          (still referenced); do NOT
 off g, KEEP    drop gc/sealed entry              re-open g; swept later once
 gravestone                                       its pins drain to zero
 forever; drop
 gc/sealed entry
```

Every seal and every delete is fence-gated: a `leadership_lost` lambda re-reads `gc.lock` and confirms it
still carries `held->fence_token` immediately before each removal. The compaction's epoch-close PUT and
reclaim are likewise skipped if `fence_still_mine` returns false. The whole sweep runs **lock-free**: it takes
the container `gc_lock` only briefly to snapshot `in_flight_pinned_blobs` (`snapshotInFlightPinnedBlobs`),
then runs against the copy.

The background driver is `ContentAddressedGCThread`: it acquires/renews the `gc.lock` lease (with a fresh,
strictly-higher fence on a steal), deletes only while it holds the lock, and is **opt-in**
(`content_addressed_gc_enabled`, default off). `reapFoldedSessions` runs each round, deleting a committed
session only once every one of its `delta_epochs` is folded (`GcCompaction::isEpochFolded`).

### 4.4 IMPORTANT: the implemented safety net is Scan B, not the §6.2 count {#scan-b}

The convergence spec's end-state makes the compaction count the *sole* delete authority via the §6.2
"sessions + current compaction" gate. **On this branch that step is NOT yet implemented** (backlog B78,
deferred, DO-NOT-MERGE-UNTIL-PROVEN). What actually gates every delete today is **Scan B** — the fresh
authoritative re-check `computeReachability` → `markReachableBlobs` over `listLivePartIds`, which checks the
candidate's bare identity `blobKey(H)` against the reachable set built from live refs → manifests ∪ live
sessions. This is **generation-blind and over-protective by design**: a candidate at any generation `(H,g)`
is spared if *any* live part still references bare `H`. The compaction count is only the *candidate source*;
Scan B is the load-bearing *delete gate*. The convergence spec S4 framing (§2 of
`2026-06-05-ca-gc-s4-review-remediation-design.md`) is explicit: the lockless machinery is "ready to be
authoritative without yet making it authoritative." This is the single most important fact for reasoning
about the safety below.

---

## 5. Concurrency without locks {#concurrency}

The core claim: concurrent commits and the background sweep coexist with **no shared mutex serializing them**.
The old `gc_lock` that held across the entire sweep (G1's antagonist) is gone; it survives only as a narrow
**container guard** for the in-process `in_flight_pinned_blobs` `std::set` (a data-structure mutex, not a
protocol mutex). What carries safety in its place:

### 5.1 The two-flag handshake {#handshake}

Two parties, two opposite-polarity flags, both store-then-load; only the *order within each side* matters,
not atomicity across sides (convergence spec §7.1):

```
   WRITER                                          GC LEADER
   ──────                                          ─────────
   A. raise session pin (sessions/<id>)            M. seal <g>.tombstone (condCreateIfAbsent)
   B. re-check <g>.tombstone                       L. fresh authoritative ref-check (reads live sessions)
   C. (resurrect if sealed, else) log + , ref      D. delete iff no ref/session AND tomb intact
```

The session pin (A), **not** the `+` delta and **not** the visible ref, is the writer-side flag. Making the
session the flag lets the writer re-check the tombstone (B) *before* logging the `+`, so an abandoned
generation never leaves a stale `+`. The GC seals (M) *before* its reference check (L), and L reads the live
session set. The chain `end(publish) ≤ start(recheck) < end(seal) ≤ start(refcheck) < end(publish)` is
unsatisfiable, so "writer commits to `g`" and "GC deletes `g`" cannot both hold — with no lock, only
read/list-after-write ordering.

### 5.2 The durable session pin read by the sweep's re-check {#session-pin}

`sessionPinnedBlobs` / `sessionPinnedPartKeys` (`ContentAddressedGC.*`) list `sessions/`, deserialize each
`WriteSession`, and for every session whose `lease_deadline_unix >= now` project its `pending` hashes to full
blob object keys (and its `part_id` to a manifest key) in the *same key space* as `markReachableBlobs`.
`computeReachability` unions these into the reachable set. So a blob that has been uploaded — or dedup-decided
— but is not yet named by any ref is still reachable, because its writer's session pins it. The pin is raised
*before* the upload/dedup-skip; the sweep re-reads sessions in its fresh re-check immediately before deleting.

### 5.3 Seal + grace window {#seal-grace}

The seal (`<g>.tombstone`) is the *durable, persistent* condemned-state — not an in-memory timer. A candidate
condemned in round 1 is re-discovered every round via the `gc/sealed/<shard>` index
(`collectSealedTombstoneCandidates`) and re-processed through grace → re-check → branch until it is swept or
recovered. Once sealed, no new attach may target `g`; reuse routes to `g+1`. Grace is liveness-only ageing
from the seal; it never authorizes a delete — the fresh re-check does.

### 5.4 The generation / resurrection mechanism {#generations}

Generations make a lockless *unconditional* delete ABA-safe. Because re-creation after a sweep routes to
`g+1` (a different key), the delete of a sealed `g` cannot collide with a re-use: the re-user is at `g+1`. The
writer's `resolveAndResurrectGeneration` and the GC's seal are the two halves; `successorGenerationExists`
splits DRAIN (a successor exists → keep) from RECOVER (no successor → un-seal). Manifests get the identical
machinery (the relink-after-full-drop ABA hole the bare `parts/<part_id>` key would have had is closed by
`parts/<part_id>/<mg>`).

### 5.5 The fenced GC-leader lock (multiple mounters) {#fence}

For multiple mounters sharing one pool (`PoolCoordination.*`), GC-vs-GC is governed by a **fenced** leader
lock built entirely on `condCreateIfAbsent`:

- `fence/<n>` is a monotonic token allocator: scan `n` upward, `condCreateIfAbsent(fence/<n>)` until one
  succeeds. Only one caller can create `fence/<n>`, so no two callers get the same token, and a later
  allocation always yields a strictly higher token than any earlier-completed one.
- `gc.lock` holds `{server_id, lease_deadline_unix, fence_token}`. `tryAcquireGcLock` cond-creates it (fence
  from 1) or, if expired, STEALS it with a fence from `existing.fence_token + 1`. The lease is a **liveness
  hint only**; the fence is the safety authority.
- Every delete (and seal, and epoch-close) is gated on `leadership_lost` / `fence_still_mine`, which re-reads
  `gc.lock` and confirms it still carries the holder's `fence_token`. A leader that paused past its lease
  cannot delete after a successor took a higher fence: the re-check fails and the sweep stops deleting.

```
   GC leader A (fence=7)                 GC leader B
   ──────────────────────                ───────────
   acquire gc.lock {fence=7}
   ... long GC pause (lease expires) ...
                                         observe expired lease
                                         allocate fence/8, STEAL gc.lock {fence=8}
   resume, about to delete blob
   leadership_lost(): read gc.lock →
     fence on disk = 8 ≠ 7  →  STOP       (deletes safely under fence=8)
```

`renewGcLock` and `releaseGcLock` are equally fence-checked, so A never clobbers B's lock.

---

## 6. Why it is safe / race-free {#safety}

The invariants are I1–I8 from the convergence spec §8 and the G-goals from §1. The implemented safety rests on
**Scan B** (§4.4) as the over-protective net; I1/I2/I6 below are stated as implemented, with the north-star
target noted where it differs.

- **I1 / I6 (log completeness).** `gc/log +` is written before the ref (`appendAndFlushForCommit` then ref),
  so *ref ⇒ delta*. Under concurrent appends, a writer that read epoch `E` re-appends the same `event_id`
  into the now-open epoch if `E` closed (`reappendIfAdvanced`, §5.1 rule 2), and the session is held until
  folded (rule 3). So *(live sessions) ∪ (folded snapshot)* covers every live reference; rebuild can only
  over-count.
- **I2 (delete gating).** A generation is deleted only after a seal, a grace, and a fresh re-check showing no
  reference. *Implemented gate:* `computeReachability` (Scan B), which is generation-blind/over-protective.
  *North-star target:* the §6.2 sessions+compaction count (B78, deferred).
- **I3 (writer handshake).** The writer publishes the ref only after raising the session pin and observing
  the tombstone absent for every pinned `(H,g)`; otherwise it resurrects. `+` after the re-check, ref last.
- **I4 (sealed generation, GC-owned tombstone).** A sealed generation receives no new attach; reuse routes
  to `g+1`; only the GC leader removes a tombstone (RECOVER, no successor); the delete of a sealed generation
  is unconditional-safe.
- **I5 (uniqueness).** At most one object per `(H,g)` — concurrent creators collapse via `condCreateIfAbsent`.
- **I7 (generation semantics).** I7a single *attachable* generation; I7b content availability (a ref to bare
  `H` ⇒ at least one generation present); I7c read any present generation (byte-identical); I7d `active` is a
  best-effort hint repaired by reader fallback.
- **I8 (ordering biases to over-count).** Commit: session before reuse; `+` after re-check; ref before
  session removal. Drop: live ref removal before `-`. A crash over-counts (safe), never under-counts.

### 6.1 Race windows {#races}

**(a) Dedup-reuse vs concurrent sweep.** Writer reuses an existing `blobs/H/0` (does not re-upload).

```
   WRITER                                  SWEEP
   ──────                                  ─────
   pin H in session/in_flight (under guard)
   decide dedup-skip (no upload)
                                           computeReachability reads live sessions → H pinned
                                           → H is REACHABLE → not swept
   publish ref (now H reachable via ref too)
```
The pin precedes the skip; the sweep re-reads sessions in its fresh re-check before deleting. Safe.

**(b) Seal vs resurrect.** GC seals `H/g` while a contended writer wants the same content.

```
   GC                          WRITER
   ──                          ──────
   seal H/g.tombstone (M)
                               re-check H/g.tombstone (B) → PRESENT
                               condCreateIfAbsent H/(g+1), pin (H,g+1)
   fresh ref-check (L):
     g not referenced (writer
     went to g+1) → SWEEP g    publish ref naming bare H; reads resolve any present gen (g+1)
```
The writer never commits to a sealed `g`; the GC sweeps a `g` no live reference names. The reverse order
(writer re-checked *before* the seal and saw it absent) means the writer already raised its session pin
covering `(H,g)`, which L observes → GC does not delete. The chain is unsatisfiable either way.

**(c) The `active` hint is best-effort with a LIST fallback.** `active` is a plain PUT, never a CAS. A stale
`active` (pointing at a swept generation, or lagging a resurrection) is not corruption: the reader `GET`s the
hinted generation, and on `404` LISTs `blobs/H/` and reads any present generation (`repairPartGenOn404`,
mirrored by the GC's `resolveManifestAtAnyGeneration`). A `<g>.tombstone` gates *attachment*, not *reads* — a
reader that successfully `GET`s a sealed-but-present blob uses it regardless of the tombstone. So a lost or
stale hint costs at most one extra LIST, never a wrong answer.

**(d) Leader hand-off under the fence.** Covered in §5.5: a paused leader's delete is rejected because the
on-disk `gc.lock` fence is higher than the one it holds. Two stealers both allocate distinct, higher tokens;
the fence (not the lock object) is the authority, so neither can delete under the other's token.

### 6.2 Why no orphan leaks indefinitely {#no-leak}

Genuine orphans (merged-away, dropped, replaced) net to count 0 in the compaction → become candidates → are
sealed, age through grace, fail the re-check, and are swept. Abandoned uploads (crash before ref) and
over-counts are bounded by `runReconciliationScan` under the `reconciliation_cadence_rounds` policy. A crashed
writer's session pins blobs only until its lease expires, after which `sessionPinnedBlobs` ignores it and the
blob becomes sweepable. Permanent gravestones accrue per sealed generation (a known liveness cost — backlog
B83/B84 — not a safety issue).

---

## 7. Failure / crash safety {#failure}

- **Crash after upload, before ref publish.** The ref is written last, so the part is not-live and never
  rediscovered (`iterateDirectory` lists refs). The orphan blob is reachable via the still-present session
  until its lease expires, then swept (compaction count-0 / reconciliation). Benign.
- **Crash mid-build.** Local scratch is discarded; `ContentAddressedWriteBuffer::cancelImpl` removes the temp
  file; any partial blob upload ages out. No ref, no dangling pointer.
- **Crash mid-commit, `+`-flush threw.** Implemented as a fail-closed *sticky* session (`#2`): the ref is
  published but the `+` is not durable, so the session is marked `deltas_failed`, retained, and exempt from
  lease-reaping; the GC reaper re-logs `pending_add_delta` (idempotent by `event_id`) and clears sticky only
  once the re-logged `+` is folded (`reapFoldedSessions`). The session is never released in this state.
- **GC leader crash mid-sweep.** The lease expires; a successor takes a higher fence. Seals are idempotent
  (`condCreateIfAbsent`); deletes are fence-gated, so a stale leader cannot delete after losing leadership.
  A half-advanced epoch re-runs cleanly (idempotent PUTs; missing old objects are no-ops).
- **Drop ordering.** The live ref is removed before the `-` delta (I8), so a crash between them over-counts
  (safe), never strands a delete.
- **Fail-closed reads/deserialization.** Every codec (`WriteSession`, `GcLock`, `GcLogBatch`, manifest,
  sidecar, pool meta) rejects bad magic / unknown version. `condCreateIfAbsent` throws `NOT_IMPLEMENTED`
  rather than a racy read-then-write on a backend that cannot prove conditional create. A live ref whose
  manifest is missing at *every* generation throws `CORRUPTED_DATA` and aborts the sweep without deleting
  (`resolveManifestAtAnyGeneration`, B18). Any list/read error in a scan propagates → the sweep aborts with
  the pool intact (a partial scan must never drive deletion).

---

## 8. Implemented-vs-north-star gaps and ambiguities {#gaps}

These are places where the sources were ambiguous or where the implemented code deliberately differs from the
north-star design — flagged so they can be verified rather than taken on faith:

1. **Spec filenames differ from the task brief.** The brief referenced `2026-06-02-cas-mergetree-m1.md`,
   `2026-06-03-cas-mergetree-m8-shared-pool.md`, `2026-06-05-ca-gc-s1-reverse-index.md`, and
   `2026-06-04-cas-mergetree-transactions.md`. The repo has `2026-06-02-cas-mergetree-integration-design.md`,
   `2026-06-03-cas-mergetree-shared-pool-design.md`, `2026-06-04-cas-mergetree-transactions-design.md`, and a
   single GC umbrella `2026-06-04-ca-gc-convergence-design.md` plus the S4 remediation
   `2026-06-05-ca-gc-s4-review-remediation-design.md`. There is **no standalone `s1-reverse-index` spec** — S1
   is described inside the convergence spec §10 and the S4 remediation. I grounded this document in the files
   that exist.

2. **Scan B is the load-bearing delete gate, NOT the §6.2 count (most important).** The convergence spec §6.2
   describes the sessions+compaction authoritative gate as the end-state, but on this branch
   `sweepCandidates` still gates every delete on `computeReachability` / `markReachableBlobs` (generation-blind
   reachability over live refs ∪ sessions). Replacing it (B78) is explicitly deferred and DO-NOT-MERGE. I
   wrote §4.4 / §6 to reflect the *implemented* gate. Verify against `ContentAddressedGC::sweepCandidates`
   and backlog rows B70/B78.

3. **Object-key shapes differ from the shared-pool spec text.** `2026-06-03-cas-mergetree-shared-pool-design.md`
   writes `pool/sessions/<id>`, `pool/gc.lock`, `pool/fence/<n>`. The *implemented* keys in `PoolPaths.cpp` are
   `sessions/<id>`, `gc.lock`, `fence/<n>` (no `pool/` prefix). I used the implemented shapes. The
   `gc/current_epoch`, `gc/log`, `gc/snap`, `gc/sealed` shapes match `GcLayout.h`.

4. **Keeper acceleration is unimplemented.** The convergence spec §12 details a Keeper accelerator
   (ephemeral sessions, epoch mirror, negative tombstone cache). I found no Keeper code in the CA directory;
   the implemented coordination is bucket-only (`PoolCoordination` on `condCreateIfAbsent`). I stated Keeper as
   optional/not-on-this-branch in §1.

5. **`reconciliation_cadence_rounds` default and the manifest-pin race (B83).** The reconciliation scan is off
   by default (cadence 0). The known `resolveAndResurrectGeneration exceeded N iterations` pathology (the
   write-path session pins the part *name*, not the content `part_id`, so the manifest object is unprotected
   during the `condCreate`→ref-publish window) is mitigated by a cap bump (8→256), not cured (B83/B84). I
   mentioned the gravestone-accrual liveness cost in §6.2 but did not claim it is fully resolved.

6. **`active` write timing.** I described `active` as written best-effort on resurrection (advance) and on
   sweep (`resetActiveOffGeneration`). The exact set of call sites that write `active` on the *first* g=0
   commit (the common case writes none) is asserted from `PoolPaths.h` comments and the GC sweep; I did not
   exhaustively trace every writer of `active` in the commit path.
