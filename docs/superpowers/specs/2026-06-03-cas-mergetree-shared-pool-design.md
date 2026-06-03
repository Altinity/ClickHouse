---
description: 'Design for a bucket-native, self-describing, safely-shared content-addressed MergeTree pool — multiple servers mount the same object-store pool, write/merge/mutate/DROP concurrently, and run a safe reachability GC, with the bucket as the single source of truth (Keeper deferred as an optional accelerator). Foundation for B1 replication.'
sidebar_label: 'CAS MergeTree shared-pool design'
sidebar_position: 11
slug: /superpowers/specs/cas-mergetree-shared-pool
title: 'Content-Addressed MergeTree — bucket-native safe shared pool (B32 + B51) design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — bucket-native safe shared pool {#shared-pool}

## Goal {#goal}

Let **multiple servers mount the same content-addressed pool** (one object-store root) and
concurrently `INSERT` / merge / mutate / `DROP`, with a **safe background reachability GC**, and
**no data loss and no leftovers**. The **object-store bucket is the single source of truth**: it
describes the pool's state correctly at every moment and is sufficient for correct operation and GC
on its own. This is the foundation B1 (table replication) rides on; it is *not* table replication.

## Guiding principle (user directive) {#principle}

The **bucket alone is authoritative and self-describing.** Correctness must **survive Keeper loss /
split-brain** — safety comes from *bucket-level* primitives, never from Keeper. **Keeper is an
optional accelerator** layered on later (B11) that makes coordination faster/simpler/cleaner (watches
as a delta feed, leader election, cheap locks) but is **never the sole authority**: even with Keeper,
every state change still lands in the bucket so it stays self-describing, and the bucket-level
mutual-exclusion (lock + fencing token) remains the backstop a Keeper split-brain cannot bypass.

## The one primitive {#primitive}

Everything is built on **create-if-absent compare-and-set**: `WriteSettings::if_none_match` (S3/Azure,
GA — already used by Iceberg; `If-None-Match: *` → HTTP 412 on conflict, already handled by
`PocoHTTPClient`), `O_EXCL` on a local/NFS backend, and a **capability probe that fails closed** on a
backend that supports neither (B7). No other consistency primitive is assumed.

- **Lease** = an object carrying a `deadline`. A lease is a **liveness hint only, never a safety
  mechanism** ("time protects only failure detection"): it lets a peer *consider* taking over, it
  never authorizes mutating live work.
- **Fencing token** = a monotonically increasing integer (allocated by create-if-absent on
  `pool/fence/<n>`, or a CAS-bumped counter). Safety lives here: a holder that paused past its lease
  is **fenced out** at the commit/delete step by a peer that took a higher token.

## Catalog = bucket objects {#catalog}

All under the pool root, all readable by listing (so the bucket is self-describing):

- **Refs** (exists): `store/<server>/<uuid>/refs/<part>` → `part_id`. Already the authoritative active
  set; reachability roots.
- **Write-sessions** (new): `pool/sessions/<session_id>` =
  `{server_id, lease_deadline, fence_token, pending:[blob_hash…, part_id]}`. The **PIN**: written
  *before* uploading blobs; the GC treats every live session's `pending` list as additional roots.
- **GC-leader lock** (new): `pool/gc.lock` = `{server_id, lease_deadline, fence_token}`. Acquired by
  create-if-absent; renewed by the holder; after the deadline a peer may steal it by taking a higher
  fence token.
- **`_pool_meta`** (fix B51): a **mounter registry** (not a single-owner gate). Each mounter records
  itself via create-if-absent under a per-mounter key; a truly-concurrent first mount no longer races
  (the CAS resolves it).

## Write protocol (per part) {#write}

build-local (all blob hashes known up front) → **write/renew the session `pending` list (the PIN),
before upload** → upload blobs (put-if-absent) + manifest → publish ref → clear the part from
`pending`. A crashed/paused writer leaves a session whose lease expires; its `pending` list keeps the
blobs pinned (reachable) until the session is reclaimed, so a concurrent GC never deletes a blob a
live-but-not-yet-referenced part needs (this is the multi-process generalization of M6's in-process
pin B49/B52).

## GC protocol (any mounter; mutual exclusion via the bucket lock) {#gc}

acquire `gc.lock` (create-if-absent + lease + **fence**) → snapshot roots = **all refs ∪ all live
sessions' `pending` lists** (read from the bucket) → mark reachable → candidates = listed − reachable
→ **re-validate each candidate's unreachability under the lock immediately before delete** (re-read
the refs/sessions touching that blob; closes the read-refs-*then*-list enumeration race the current
single-owner sweep has) → delete, stamping the holder's fence token. A session whose lease has expired
is itself reclaimable (its pending list stops being a root once reclaimed). A GC holder that paused
past its lease cannot delete: a peer that took over holds a higher fence token, and the paused
holder's delete is rejected (fenced).

## Multi-mounter (lift the single-owner gate, B51) {#multi-mount}

`_pool_meta` stops being a single-owner gate; GC mutual exclusion moves to the `gc.lock`. So N
mounters read/write/merge/mutate/DROP concurrently, any one can contend for the GC-leader lock, and
the background sweep is **re-enabled** under this protocol (it was off-by-default since the M5 review
precisely because un-coordinated background deletion was unsafe — now it is coordinated).

## Components / files {#files}

- New `PoolCoordination` seam (interface) with a **bucket-native backend** in this milestone (Keeper
  backend deferred to B11). Lives under `…/MetadataStorages/ContentAddressed/`.
- A `condPutIfAbsent`/CAS helper over the object storage (wires `WriteSettings::if_none_match`;
  capability probe).
- `WriteSession` (pin) object + `GcLock` (lease+fence) object + their versioned LE codecs (reuse
  `Codec.h`/`FormatHeader`).
- `ContentAddressedTransaction` — open/renew/close a write-session around a part write; PIN before
  upload.
- `ContentAddressedGC` — read sessions as roots; acquire/renew the lock; re-validate-under-lock before
  delete; stamp fence.
- `PoolMeta`/`claimPoolOwnership` — CAS claim; mounter registry.

## Acceptance {#acceptance}

- gtests for: CAS create-if-absent (conflict → fail-closed); session pin keeps a dedup-reused blob
  alive across a concurrent sweep; lock lease steal + fencing rejects the stale holder's delete;
  re-validate-under-lock cancels a delete when a ref appears mid-sweep.
- **Integration: two ClickHouse servers mounting the same MinIO pool**, concurrent
  insert/merge/mutate/DROP from both, background GC on → no data loss, no leftovers, correct reads on
  both; plus fault injection: (a) kill a writer mid-upload → its pinned blobs survive then are
  reclaimed after session expiry; (b) pause the GC leader past its lease → a second mounter takes over
  and fencing blocks the first's stale delete.

## Out of scope / deferred {#deferred}

- **Keeper accelerator (B11)** — faster coordination; must preserve bucket self-description.
- **Table replication (B1)** — replicas referencing shared blobs under the Keeper `/log`; rides on
  this milestone.
- Broader non-AWS conditional-write hardening beyond the probe + `O_EXCL` local path (B7).

## Self-review {#self-review}

- **Coverage:** B32 (pin+lease+fence over conditional writes), B51 (`_pool_meta` CAS / multi-mount),
  re-enabled coordinated GC. Maps to the acceptance items.
- **Principle honored:** every coordination object is in the bucket; no Keeper in this milestone; the
  bucket lock+fence is the only mutual exclusion, so Keeper-loss/split-brain cannot affect correctness.
- **Safety, not time:** leases never authorize mutating live work; fencing + re-validate-under-lock
  are the safety mechanisms.
- **No layout change:** `blobs/`/`parts/`/`refs/` unchanged; adds `pool/sessions/`, `pool/gc.lock`,
  `pool/fence/`, and per-mounter `_pool_meta` registry keys.
