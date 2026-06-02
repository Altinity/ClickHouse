---
description: Post-review structural hardening for the content-addressed MergeTree PoC — typed identities, honest capabilities, mutable-state split, versioned formats, whole-part commit, and the safe (pin+lease+fence) GC protocol.
sidebar_label: 'CAS MergeTree M5 hardening'
sidebar_position: 5
slug: /superpowers/plans/cas-mergetree-m5-hardening
title: 'Content-Addressed MergeTree — M5 Hardening Plan (post-review)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — M5 Hardening Plan (post-review) {#cas-mergetree-m5}

> **For agentic workers:** each step below is its own sub-project; write a detailed bite-sized plan (`superpowers:writing-plans`) for a step before executing it. Steps are ordered: earlier ones de-risk later ones.

**Context.** Phases 1–4 produced a `content_addressed` disk that writes/reads/dedups/merges/drops on local + S3 and passes stateless + integration tests. A 2026-06-02 design review then found a **live data-loss bug** (GC blob key-space mismatch), a **corruption bug** (partition-cloning ALTERs), and the deeper structural fragilities below. Immediate fixes (data-loss GC key + regression test; background sweep **off by default**; partition-clone ALTERs gated off) are handled separately (see backlog Phase-4 entry + B21). **This plan is the structural sequence** that takes the PoC from "runs" to "robust", in dependency order.

**Guiding principles from the review.**
- *Time may protect only failure detection, never live work* (kills `grace`-for-safety → B32).
- *Make in-flight work visible to the GC* (pins), don't infer safety from age.
- *A content-addressed part is an atomic unit*; the file-by-file `IMetadataStorage` seam is the wrong granularity (→ B30).
- *Be honest about capabilities*; fail closed on everything unproven (→ B31).
- *POCs become production data*; formalize on-disk formats before that happens (→ versioned formats).

---

## Step 1 — Typed identities (B29) {#step-1}
**Goal:** make the data-loss bug class a compile error. **Why first:** cheapest, and it hardens every later step.
- Introduce wrappers (no implicit string conversion): `BlobHash` (content hash), `BlobObjectKey` (`<prefix>/blobs/<fanout>/<hash>`), `PartId`, `PartObjectKey` (`parts/…`), `RefObjectKey` (`store/…/refs/…`).
- `PartManifest::BlobEntry` stores `BlobHash`. `PoolPaths` builders take typed inputs and return typed keys (`blobKey(prefix, BlobHash) -> BlobObjectKey`). The GC reachable set is `std::set<BlobObjectKey>`; the listing returns `BlobObjectKey` — so they can only be compared in the same space.
- Object-storage calls (`writeObject`/`listObjects`/`removeObjectsIfExist`) take the typed key's `.string()` only at the boundary.
- **Done when:** the GC reachable/candidate comparison cannot be expressed with mismatched key kinds; all CA code + tests compile against the typed API; 32+ unit tests + stateless green. (Fold the still-pending file-consolidation pass in here — it touches the same files.)

## Step 2 — Explicit capability set + reject-unsupported (B31; the Phase-5 gate) {#step-2}
**Goal:** the disk advertises exactly what it supports and fails closed on the rest, so unsupported features cannot silently corrupt.
- Replace the optimistic `supportsHardLinks`/`supportZeroCopyReplication` booleans with an explicit capability advertisement for `content_addressed`: **no** zero-copy replication, projections, partition clone (B21), external `ATTACH … FROM`, patch parts (lightweight delete), or multi-mounter pool.
- Reject unsupported table metadata at `CREATE`/`ATTACH` (projections, nested/unknown part metadata — see B5/B7 manifest-hierarchy) with clear errors.
- Add the **`_pool_meta` self-check** (B11): on mount, validate config + detect a second/concurrent mounter → **fail closed** (so background GC can never run un-coordinated; gates B32 re-enable).
- **Done when:** a CREATE with a projection / on a shared pool / etc. throws a clear error; a stateless test asserts each rejection.

## Step 3 — Split mutable per-ref state out of the manifest (B23) {#step-3}
**Goal:** the manifest holds only shared, content-identical state; per-part mutable fields live in the ref.
- Move `uuid.txt`, `txn_version.txt`, `metadata_version.txt` out of `PartManifest` into the **ref payload / a per-ref sidecar**; overlay them on the read path. (`part_id` already excludes them.)
- **Done when:** two parts with identical content but different `uuid`/`metadata_version` no longer collide on one manifest; a test with `assign_part_uuids=1` (and a metadata-only `ALTER`, once supported) round-trips correctly.

## Step 4 — Versioned, little-endian on-disk formats + exact parse (B19, B28) {#step-4}
**Goal:** formalize the ref and manifest formats before real data exists.
- `PartManifest` (de)serialization: explicit little-endian (`writeBinaryLittleEndian`/varint), a format **version** byte, strict bounds — replace the host-endian `memcpy` (`PartManifest.cpp`).
- Ref payload: a versioned struct with an **exact** parse (not PoolScan's "first hex run"); one shared `partIdFromRefPayload` used by both the read path and GC (B28). Reserve room for the `ReplicatedMergeTreePartHeader` (B1) + the per-ref mutable fields (Step 3).
- A reader rejects a version it does not understand (fail closed).
- **Done when:** cross-arch determinism is guaranteed; golden-format tests pin the bytes; ref parse is exact and single-sourced.

## Step 5 — Whole-part commit contract (B30) {#step-5}
**Goal:** model a part as the atomic unit, not a bag of files.
- A first-class begin-part / write-files / commit-part contract; route every part-producing path (INSERT, merge, mutate, clone, ATTACH) through exactly one such transaction. **Fail closed** anywhere one whole-part transaction can't be supplied (this is what makes B21's gating principled rather than ad-hoc).
- Revisit the `moveDirectory`/`moveFile` re-pin and `createHardLink` carry-forward as operations on the part unit, not per file.
- **Done when:** no part-producing path autocommits per file; the clone/mutate paths either work atomically or are rejected with a clear error.

## Step 6 — Safe GC protocol: pin + lease + fence (B32) {#step-6}
**Goal:** correct GC of a shared pool with no time-for-safety, then re-enable background sweeping.
- Implement the catalog (active refs + live write-sessions with lease + pending-object lists + single GC-leader lock). Backend: in-process for single-owner; **object-store conditional writes** (`If-None-Match` / `O_EXCL`) for Keeper-free shared; Keeper (B11) as the fast path. Same protocol either way.
- Write side: open session → **pin** (write pending list of hashes before upload) → upload (put-if-absent; deduped blob already pinned) → manifest + ref → drop pending list. **Fencing token** on commit closes the paused-writer hazard.
- GC side (leader, under lock): roots = refs ∪ live sessions' pending lists → mark → candidates → **re-validate under the lock immediately before delete** (fixes the enumeration-order race) → delete; dead-lease sessions' pending lists are reclaimable.
- **Done when:** a test with a concurrent slow writer + an aggressive GC never loses a live blob (the case `grace` could not cover); background sweep re-enabled only with ownership (Step 2) + this protocol.

---

## Sequencing rationale {#sequence}
1 (types) hardens everything after it. 2 (honest capabilities + ownership self-check) makes the disk *safe to expose* and gates 6. 3 + 4 fix the format before data persists. 5 (whole-part contract) is the structural payoff and the precondition for safe clone/mutate. 6 (safe GC) is last because it depends on the ownership self-check (2) and benefits from the part contract (5). Background GC stays **off** until 6 + 2 are done.

## Out of scope here (post-M1, tracked in the backlog)
B1 replication, B9 persisted/sharded delta index, B10 one-GET part open, B13 migration, B14 expedited delete, B15 introspection, B16 BACKUP/RESTORE, B17 encryption, the full Keeper coordination (B11 beyond the self-check).
