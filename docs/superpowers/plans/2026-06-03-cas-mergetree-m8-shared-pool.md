---
description: 'M8 — bucket-native safe shared content-addressed pool: create-if-absent CAS primitive, write-session pins, a fenced GC-leader lock with re-validate-under-lock, a multi-mounter _pool_meta registry, re-enabled coordinated GC, and a two-server MinIO acceptance test.'
sidebar_label: 'CAS MergeTree M8 shared pool'
sidebar_position: 12
slug: /superpowers/plans/cas-mergetree-m8
title: 'Content-Addressed MergeTree — M8 Plan (bucket-native safe shared pool, B32 + B51)'
doc_type: 'guide'
---

# CAS MergeTree — M8: bucket-native safe shared pool (B32 + B51) {#m8}

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`. Steps use
> `- [ ]`. The bucket is the single source of truth; Keeper is deferred (B11). Spec:
> `docs/superpowers/specs/2026-06-03-cas-mergetree-shared-pool-design.md`.

**Goal:** multiple servers mount the same content-addressed pool and concurrently
INSERT/merge/mutate/DROP with a safe background GC — no data loss, no leftovers — coordinated entirely
through bucket objects (create-if-absent CAS).

**Architecture:** one primitive (`condCreateIfAbsent` over `WriteSettings::object_storage_write_if_none_match`
on S3/Azure, `O_EXCL` on local, probe-and-fail-closed otherwise). On it: write-session PIN objects
(`pool/sessions/<id>`), a fenced GC-leader lock (`pool/gc.lock`), a fencing counter
(`pool/fence/<n>`), and a CAS-claimed `_pool_meta` mounter registry. GC: roots = refs ∪ live sessions'
pending lists; re-validate-under-lock before delete; stamp fence.

**Tech Stack:** `ContentAddressed/` units, `WriteSettings`/`WriteBufferFromS3` conditional writes,
`Codec.h`/`FormatHeader` for object encodings, the `clickhouse-praktika-tests` skill, the
`test_content_addressed_s3` integration harness (extended to two instances).

## Build & test {#build}
- Build: `cmake --build build --target clickhouse unit_tests_dbms > build/cas_m8_build.log 2>&1`
  (redirect; subagent summarizes). No `-j`/`nproc`.
- gtests: `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`.
- Stateless/integration via the `clickhouse-praktika-tests` skill, **foreground**, binary at
  `ci/tmp/clickhouse`. Never `clickhouse local` (B48).
- Allman; `DB::Exception`+`ErrorCodes`; no `<...>` in `///`; full-path includes.

---

## Phase A — the CAS primitive + multi-mounter `_pool_meta` (B51) {#phase-a}

### Task A1: `condCreateIfAbsent` CAS helper (TDD) {#a1}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.{h,cpp}`
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp`

A free function `bool condCreateIfAbsent(IObjectStorage &, const std::string & key, const std::string & bytes)`:
write `bytes` to `key` with `WriteSettings::object_storage_write_if_none_match = "*"`; return `true`
if created, `false` if the object already existed (an `S3Exception` whose `getMessage()`/exception
name is `PreconditionFailed`), rethrow on any other error. For a backend that does not support
conditional writes, a one-time **capability probe** (attempt a cond-create on a throwaway key twice;
the second MUST report a conflict) gates it: if unsupported, throw `NOT_IMPLEMENTED` (fail closed) —
do NOT silently fall back to read-then-write. For `LocalObjectStorage`, implement via `O_EXCL`
create (a separate small backend branch keyed off the object-storage type).

- [ ] **Step 1: Write failing gtest** `CondCreateIfAbsentIsAtomic` — using the gtest's
  `LocalObjectStorage` harness (mirror `getObjectStorage` in the test file): two `condCreateIfAbsent`
  calls to the same key; the first returns `true`, the second returns `false`; the stored bytes are
  the first writer's. (If `LocalObjectStorage` cannot do conditional create, the probe must make the
  helper throw `NOT_IMPLEMENTED` — assert that instead, and wire the `O_EXCL` path so it CAN.)
- [ ] **Step 2: Run** `--gtest_filter='*CondCreateIfAbsent*'` → fail.
- [ ] **Step 3: Implement** `condCreateIfAbsent` + the capability probe.
- [ ] **Step 4: Run** → pass.
- [ ] **Step 5: Commit** `CAS M8: condCreateIfAbsent CAS primitive (if_none_match / O_EXCL) + probe`.

### Task A2: `_pool_meta` CAS claim + mounter registry (B51) {#a2}

**Files:**
- Modify: `…/ContentAddressed/PoolMeta.cpp` (`claimPoolOwnership`)
- Modify: `…/ContentAddressed/PoolPaths.{h,cpp}` (per-mounter registry key
  `_pool_meta.mounters/<server_id>`)
- Test: gtest

Today `claimPoolOwnership` is read-then-write (B51: fails open on a concurrent first mount). Make the
first-mount claim a `condCreateIfAbsent` of `_pool_meta`; on conflict, READ the existing marker and
validate compatibility (existing same-owner re-mount OK; different owner without `allow_shared` →
fail closed). Add a per-mounter registry: each mounter `condCreateIfAbsent`s
`_pool_meta.mounters/<server_id>` so the set of live mounters is listable from the bucket.

- [ ] **Step 1: Write failing gtest** `PoolMetaConcurrentClaimResolvesViaCAS` — simulate two distinct
  `server_id`s claiming a fresh pool without `allow_shared`: exactly one succeeds, the other fails
  closed (mirror the existing `PoolMetaSecondMounterFailsClosed` test); with `allow_shared`, both
  register in the mounter registry and listing returns both.
- [ ] **Step 2: Run** → fail (current read-then-write lets both through / races).
- [ ] **Step 3: Implement** the CAS claim + mounter registry.
- [ ] **Step 4: Run** → pass; existing `PoolMeta*` gtests stay green.
- [ ] **Step 5: Commit** `CAS M8: _pool_meta CAS claim + mounter registry (B51)`.

---

## Phase B — write-session pins (the multi-process PIN) {#phase-b}

### Task B1: `WriteSession` object + codec (TDD) {#b1}

**Files:**
- Create: `…/ContentAddressed/WriteSession.{h,cpp}` (struct `{server_id, lease_deadline_unix,
  fence_token, std::vector<BlobHash> pending, PartId part_id}` + versioned LE serialize/deserialize on
  `Codec.h`/`FormatHeader`, mirroring `RefSidecar`/`PoolMeta`)
- Test: gtest

- [ ] **Step 1: Failing gtest** `WriteSessionRoundTripsAndRejectsBadVersion` (mirror the `PoolMeta` /
  `RefSidecar` codec tests).
- [ ] **Step 2–4: Implement, run, green.**
- [ ] **Step 5: Commit** `CAS M8: WriteSession pin object + versioned codec`.

### Task B2: transaction opens/pins/closes a write-session (TDD) {#b2}

**Files:**
- Modify: `…/ContentAddressed/ContentAddressedTransaction.cpp` (write path)
- Modify: `…/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` (session key helpers; owns the
  session id for this mounter)
- Modify: `…/ContentAddressed/PoolPaths.{h,cpp}` (`sessionKey(prefix, id)`, `sessionsPrefix`)
- Test: gtest

In `writeFile`/`finalizeImpl`+`commit`: before uploading a part's blobs, write/refresh this
transaction's session object listing the part's `pending` blob hashes + `part_id` (the PIN). After
the ref is published in `commit`, remove the part from the session's pending list. Reuse the existing
in-process pin (B52) as the in-mounter fast path; the session object is the cross-mounter pin.

- [ ] **Step 1: Failing gtest** `SessionPinListsPendingBlobsBeforeUpload` — drive a part write through
  the transaction; assert a `pool/sessions/<id>` object exists listing the part's blob hashes during
  the write and that the entry is cleared after `commit` publishes the ref. (Mirror
  `WritePartThenReadBackAndDedup` for the build; read the session object via `readObject`.)
- [ ] **Step 2–4: Implement, run, green.**
- [ ] **Step 5: Commit** `CAS M8: transaction writes a cross-mounter session pin before upload`.

---

## Phase C — fenced GC-leader lock + coordinated sweep {#phase-c}

### Task C1: fencing counter + `GcLock` acquire/renew/steal (TDD) {#c1}

**Files:**
- Modify: `…/ContentAddressed/PoolCoordination.{h,cpp}` — `allocateFenceToken(os, prefix) -> uint64`
  (monotonic via `condCreateIfAbsent` on `pool/fence/<n>` scanning upward, or a CAS-bumped counter);
  `GcLock` acquire (`condCreateIfAbsent` of `pool/gc.lock`; if present + past `lease_deadline`, steal
  by taking a higher fence token and overwriting), renew, release.
- Test: gtest

- [ ] **Step 1: Failing gtest** `GcLockGrantsOneHolderAndStealsAfterLease` — holder A acquires; B
  fails while A's lease is live; after A's deadline, B steals with a strictly higher fence token; A's
  token is now stale.
- [ ] **Step 2–4: Implement, run, green.**
- [ ] **Step 5: Commit** `CAS M8: fenced GC-leader lock (acquire/renew/steal) over conditional writes`.

### Task C2: GC reads sessions as roots + re-validate-under-lock + fence-stamped delete (TDD) {#c2}

**Files:**
- Modify: `…/ContentAddressed/ContentAddressedGC.cpp` (`runSweepOnce`)
- Test: gtest

`runSweepOnce` now: acquire `GcLock` → roots = live refs ∪ **every live session's pending list**
(read from `sessionsPrefix`; a session past its lease is NOT a root and is itself reclaimable) → mark
→ candidates = listed − reachable → **for each candidate, re-read the refs/sessions that could
reference it and skip if now reachable** (re-validate-under-lock) → delete only while still holding
the lock with the current fence token.

- [ ] **Step 1: Failing gtests** — (a) `SweepTreatsLiveSessionPinAsRoot`: a session pins a blob with
  no ref yet → sweep keeps it; after the session is removed → sweep reclaims it. (b)
  `SweepRevalidatesBeforeDelete`: a blob is unreachable at snapshot but a ref is added before delete →
  re-validate cancels its deletion. (Mirror the existing `Sweep*` gtests.)
- [ ] **Step 2–4: Implement, run, green** (existing `Sweep*` gtests stay green).
- [ ] **Step 5: Commit** `CAS M8: coordinated GC — session-pin roots + re-validate-under-lock + fence`.

---

## Phase D — lift the single-owner gate + re-enable coordinated background GC {#phase-d}

### Task D1: multi-mount allowed; background sweep re-enabled under the lock {#d1}

**Files:**
- Modify: `…/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`startup` — register in the
  mounter registry instead of single-owner-gating)
- Modify: `…/ContentAddressed/ContentAddressedGCThread.cpp` (the background loop acquires/renews the
  `GcLock`; only the lock holder sweeps)
- Test: gtest + a stateless single-server regression

- [ ] **Step 1:** Allow multiple mounters (CAS registry from A2); the GC thread runs the coordinated
  `runSweepOnce` (Phase C) under the lock. A non-leader mounter's thread renews-or-yields.
- [ ] **Step 2:** gtest `TwoMetadataStoragesShareOnePoolGcIsExclusive` — two
  `ContentAddressedMetadataStorage` over the SAME `LocalObjectStorage` root; both write parts; a sweep
  driven through the lock never deletes a blob the other's live session pins; both can read all parts.
- [ ] **Step 3:** Stateless regression: `04290`/`04295` no-leftovers still `[ OK ]` (single-mounter
  path unaffected; the lock is uncontended).
- [ ] **Step 4: Commit** `CAS M8: allow multi-mount + re-enable coordinated background GC under the lock`.

---

## Phase E — two-server acceptance + fault injection {#phase-e}

### Task E1: two ClickHouse servers, one MinIO pool (integration) {#e1}

**Files:**
- Modify: `tests/integration/test_content_addressed_s3/` — add a second instance over the SAME bucket
  path/policy; a new test module if cleaner.

- [ ] **Step 1:** Add `node2` mounting the same `content_addressed_s3` pool (same bucket + root).
  Test: both create the same-UUID-independent tables (distinct table UUIDs, shared pool), each
  INSERT/merge/mutate/DROP; with background GC on, assert each node reads its own data correctly, no
  data loss, and after both DROP the pool drains (no leftovers).
- [ ] **Step 2:** Run via `python -m ci.praktika run "Integration tests (arm_binary, distributed plan, 1/4)"
  --test test_content_addressed_s3` (foreground). Expected: passed.
- [ ] **Step 3: Commit** `CAS M8: two-server shared-pool integration test (MinIO)`.

### Task E2: fault injection (integration) {#e2}

- [ ] **Step 1:** (a) kill a node mid-write (or simulate a stale session) → assert its pinned blobs
  survive a peer's sweep, then are reclaimed after the session lease expires + a reclaim pass; (b)
  hold/extend one node's `gc.lock` past its lease, let node2 steal it, assert the first node's delete
  is fenced (rejected) — drive via a small test hook or by manipulating the lock object.
- [ ] **Step 2:** Run (foreground); expected passed.
- [ ] **Step 3: Commit** `CAS M8: shared-pool fault injection (pin survival + lock fencing)`.

## Verify — HARD GATE {#verify}
- Build clean; `--gtest_filter='ContentAddressed*'` all green (incl. new CAS/session/lock/sweep tests).
- Single-server stateless no-leftovers (`04290`,`04295`) still `[ OK ]`.
- Two-server MinIO integration passes; fault-injection passes.
- Final fresh-subagent + adversarial review: no BLOCKER/CRITICAL; confirm the bucket alone is
  authoritative (no Keeper anywhere) and the lock+fence is the only mutual exclusion.

## Self-review {#self-review}
- **Coverage:** spec §primitive (A1), §catalog `_pool_meta` (A2) + sessions (B) + lock/fence (C),
  §write (B2), §gc (C2), §multi-mount (D), §acceptance (E).
- **Principle:** every coordination object is in the bucket; no Keeper; lock+fence is the sole mutual
  exclusion → Keeper-loss/split-brain cannot affect correctness.
- **Incremental + demonstrable:** A1+A2 are immediately wired in and testable; each phase builds on the
  prior; E proves the end-to-end shared-pool claim.

## Deferrals likely to surface {#deferrals}
- Keeper accelerator (B11). Non-AWS conditional-write hardening (B7). Clock-skew bounds for lease
  *liveness* (safety is fence-based, so skew costs only throughput). Replication (B1) is the next
  milestone, built on this.
