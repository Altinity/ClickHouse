# CAS S3-native staging area — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, off-by-default S3-native staging area for CAS MergeTree blob writes: stream a large blob directly to a per-mountpoint S3 staging key while hashing, then promote to the content-addressed blob key via a **write-once conditional server-side copy**, eliminating the local-disk round-trip.

**Architecture:** A per-disk `cas_staging_backend` toggle (`local` default | `s3`) selects the staging path. In `s3` mode, above `cas_s3_staging_min_bytes` a blob streams to `POOL/staging/<mount_id>/<rnd>.tmp` via the object store; on commit it is promoted to `blobs/<shard>/<hash>` with a conditional (`If-None-Match:*`) server-side copy (`copyS3File`, multipart-safe), capturing the destination ETag as the incarnation token; then the staging object is deleted. Unsupported conditional copy ⇒ **fail-close to local staging**. The existing condemn/resurrect gate is preserved on the promote path (resurrect source = the staging object, never the condemned key).

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, `src/IO/S3/copyS3File.cpp`, `src/Disks/DiskObjectStorage/ObjectStorages/`), gtest (`src/Disks/tests/`), pytest integration (`tests/integration/`, `with_rustfs`), ca-soak.

**Spec:** `docs/superpowers/specs/2026-07-11-cas-s3-native-staging-design.md`. Read it before Task 1.

## Global Constraints

- **OFF BY DEFAULT.** `cas_staging_backend` default = `local` ⇒ the write path is **byte-for-byte** the current local `scratchPath` path. No new code path is taken, no probe runs, when local. (User: "не хочу чтобы s3 staging использовался по умолчанию".)
- **Promote is WRITE-ONCE.** Never overwrite a live blob object. The promote copy is conditional (`If-None-Match:*`) or the disk uses local staging. Unsupported conditional ⇒ **fail-close to local**, never fall to unconditional overwrite (fail-open is forbidden — CLAUDE.md).
- **Resurrect source = the staging object**, never a `GET`/`copyObject` of the condemned `blobKey` (`feedback_ca_resurrect_invariant`).
- **Reads stay content-hash-only** — no read/fsck path may gate on a token/ETag.
- **Edge-before-observe** unchanged: `precommitAdd(edge) → stage → promote(copy)+condemn-check → delete staging → promoteBuild(visible)`.
- C++ style: **Allman braces**; say "exception" not "crash"; wrap SQL/class/function names in inline code in comments; never `sleep` for races.
- Build: redirect `ninja` output to a log in the build dir; use a subagent to summarize the log. Tests: redirect to a uniquely-named log; subagent summarizes.
- Emulated/Local object backend: `supportsConditionalCopy()` = false ⇒ metadata layer uses local staging; the S3 seams are unused there.

---

## File Structure

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h` — add `supportsConditionalCopy()`, `stageStream(key)`, `promoteStaged(staging_key, blob_key)` to the backend interface.
- `.../Core/CasObjectStorageBackend.{h,cpp}` — Native impl of the three seams (conditional copy via `copyS3File`); Emulated impl returns `supportsConditionalCopy()=false`.
- `.../Core/CasProbe.{h,cpp}` (existing capability probe) — add a conditional-copy probe run at mount; store the result.
- `src/IO/S3/copyS3File.{h,cpp}` — thread an optional `If-None-Match` header into `CopyObject` (`fillCopyRequest`) and `CompleteMultipartUpload`; return the destination ETag.
- `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` + `S3/S3ObjectStorage.{h,cpp}` — a `copyObject` overload (or new method) that takes an `If-None-Match` precondition and returns the destination ETag (or throw a typed precondition-failed).
- `.../ContentAddressed/ContentAddressedWriteBuffers.{h,cpp}` — `CaContentWriteBuffer` gains an S3-staging mode (stream to an object-store sink while hashing).
- `.../ContentAddressed/ContentAddressedTransaction.{h,cpp}` — `PendingBlob` gains `backend`/`staging_key`; `stageBlobPartFile`, `publishStaging`, `cleanupPendingTempFiles` handle S3 staging; the promote runs the condemn/resurrect gate via `promoteStaged`.
- `.../ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` — parse `cas_staging_backend`/`cas_s3_staging_min_bytes`; expose staging mode + threshold; wire the capability probe result; choose the write-buffer mode in `writeFile`.
- `.../MetadataStorageFactory.cpp` — read the two config keys.
- Staging sweeper: `.../Core/CasStagingSweeper.{h,cpp}` (new) — mount-start + GC age-sweep of `staging/<mount_id>/`, lease-fenced.
- Tests: `src/Disks/tests/gtest_cas_s3_staging.cpp` (new); `tests/integration/test_cas_s3_staging/` (new); ca-soak variant lane config.

---

## Task 0: Config plumbing + capability plumbing (no behavior change)

**Files:**
- Modify: `.../MetadataStorageFactory.cpp` (near the existing `scratch_path` read, ~229-237)
- Modify: `.../ContentAddressedMetadataStorage.{h,cpp}` (store `staging_backend` enum + `s3_staging_min_bytes` + a `bool conditional_copy_supported` set later by the probe)
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp` (new)

**Interfaces:**
- Produces: `enum class StagingBackend { Local, S3 }`; `ContentAddressedMetadataStorage::stagingBackend()`, `::s3StagingMinBytes()`, `::conditionalCopySupported()` (default false until probe). `writeFile` will consult these in Task 3/4.

- [ ] **Step 1: Failing test** — parse a disk config with `<cas_staging_backend>s3</cas_staging_backend>` and `<cas_s3_staging_min_bytes>67108864</cas_s3_staging_min_bytes>`; assert the metadata storage reports `StagingBackend::S3` and `67108864`. Also assert the **default** (keys absent) is `StagingBackend::Local` and that no probe is invoked and `writeFile` selects the current local path (guard: a flag/counter that the S3 path was not entered).
- [ ] **Step 2: Run test, verify it fails** (accessor/enum absent).
- [ ] **Step 3: Implement** the enum, the two config reads (default `Local`, default min-bytes e.g. `64 * 1024 * 1024`), accessors. `Local` ⇒ set nothing else, take no probe. Follow the existing `scratch_path` parse for anchoring/validation.
- [ ] **Step 4: Run test, verify pass.**
- [ ] **Step 5: Commit** `feat(cas): parse cas_staging_backend / cas_s3_staging_min_bytes (default local, no behavior change)`.

## Task 1: `copyS3File` — optional `If-None-Match` + return destination ETag

**Files:**
- Modify: `src/IO/S3/copyS3File.{h,cpp}` (`copyS3File` signature; `fillCopyRequest` ~667-687; `performSingleOperationCopy` ~CopyObject; `performMultipartUploadCopy` `CompleteMultipartUpload` ~595)
- Test: `src/IO/tests/gtest_writebuffer_s3.cpp` or a new `gtest_copy_s3_conditional.cpp` if an S3 mock exists; otherwise cover via the integration test in Task 7 and keep this task's test a compile/interface test.

**Interfaces:**
- Produces: `copyS3File(..., std::optional<String> if_none_match = {}, String * out_dest_etag = nullptr)` — when `if_none_match` is set, the header is added to `CopyObject` and to `CompleteMultipartUpload`; on a 412 the function throws `S3Exception` with `Aws::S3::S3Errors::PRECONDITION_FAILED` (or the code the SDK surfaces). `out_dest_etag`, if non-null, is filled from the `CopyObject`/`CompleteMultipartUpload` result ETag (already read at `copyS3File.cpp:595`/`:811`, currently discarded).

- [ ] **Step 1: Failing test** — a unit test asserting the new params compile and default behavior is unchanged (call with no `if_none_match`/`out_dest_etag`). If an S3 mock harness exists in-tree, add a test that a conditional copy over an existing key surfaces `PRECONDITION_FAILED`; else defer enforcement coverage to Task 7 and note it here.
- [ ] **Step 2: Run, verify fail** (signature mismatch).
- [ ] **Step 3: Implement** — add the optional header to `fillCopyRequest` (`request.SetIfNoneMatch(*if_none_match)`) for the single-copy path and to the `CompleteMultipartUpload` request for the multipart path; capture the result ETag into `*out_dest_etag`; map a 412 to a typed throw. Keep both paths' size-based selection (`use_single_operation_copy`) intact.
- [ ] **Step 4: Run, verify pass.**
- [ ] **Step 5: Commit** `feat(s3): copyS3File optional If-None-Match precondition + return dest ETag`.

## Task 2: `IObjectStorage` conditional-copy method + S3 impl

**Files:**
- Modify: `.../ObjectStorages/IObjectStorage.h` (add `copyObjectIfNoneMatch` or extend `copyObject` — new virtual with a default that throws `NOT_IMPLEMENTED`, fail-closed like `removeObjectIfTokenMatches`)
- Modify: `.../ObjectStorages/S3/S3ObjectStorage.{h,cpp}` (impl routing to Task 1's `copyS3File` with the precondition + ETag out-param)
- Test: extend `gtest_cas_s3_staging.cpp` for the interface; enforcement covered in Task 7.

**Interfaces:**
- Produces: `struct ConditionalCopyResult { bool created; String dest_etag; };` and `IObjectStorage::copyObjectConditional(const StoredObject & from, const StoredObject & to, const ReadSettings &, const WriteSettings &, std::optional<ObjectAttributes> = {}) -> ConditionalCopyResult` — `created=true` + ETag on success; `created=false` on 412 (precondition failed = destination existed). Default impl throws `NOT_IMPLEMENTED` (Emulated/Local do not support it).

- [ ] Steps 1–5 (TDD): interface test (default throws; S3 impl compiles and forwards `If-None-Match:*` + returns ETag/created), implement, commit `feat: IObjectStorage::copyObjectConditional (S3 write-once server-side copy)`.

## Task 3: Capability probe (mount-time) — conditional copy

**Files:**
- Modify: `.../Core/CasProbe.{h,cpp}` (the existing capability probe; add a conditional-copy probe)
- Modify: `.../ContentAddressedMetadataStorage.cpp` (invoke the probe only when `stagingBackend()==S3`; set `conditional_copy_supported`; on false, log one line and fall back to `Local`)
- Test: `gtest_cas_s3_staging.cpp` — the *selection logic* (given probe=true ⇒ S3 path enabled; probe=false ⇒ Local fallback). The live probe is exercised in Task 7.

**Interfaces:**
- Consumes: `IObjectStorage::copyObjectConditional` (Task 2).
- Produces: `CasProbe::probeConditionalCopy(backend) -> bool` (PUT a tiny probe object under `POOL/staging/<mount_id>/probe/`, conditional-copy it to a second key, then conditional-copy AGAIN and demand `created==false`; clean up both). `ContentAddressedMetadataStorage::conditionalCopySupported()` reflects it. **Fail-close:** any probe error or a non-enforcing result (second copy `created==true`) ⇒ `false` ⇒ Local fallback.

- [ ] Steps 1–5 (TDD): selection-logic test with a stub backend (enforcing vs non-enforcing) ⇒ S3-enabled vs Local-fallback; implement the probe (mirror the existing CasProbe pattern; the probe writes only under the mount's own `staging/<mount_id>/probe/`); commit `feat(cas): mount-time conditional-copy capability probe + fail-close to local`.

## Task 4: `CaContentWriteBuffer` S3-staging mode (stream-to-staging while hashing)

**Files:**
- Modify: `.../ContentAddressedWriteBuffers.{h,cpp}` (add an S3 sink mode)
- Modify: `.../ContentAddressedTransaction.cpp` (`writeFile` selects the mode when `stagingBackend()==S3 && conditionalCopySupported() && size-hint ≥ threshold`; `PendingBlob` gains `backend` + `staging_key`)
- Test: `gtest_cas_s3_staging.cpp` with the Emulated backend — but note Emulated has `supportsConditionalCopy()=false`, so pure-gtest coverage of the S3 write path needs a fake object-store sink. Provide a minimal in-test `IObjectStorage` fake that records writes, OR cover the streaming-hash-to-sink behavior with a fake `WriteBuffer`. The end-to-end S3 path is covered in Task 7.

**Interfaces:**
- Consumes: `Backend::stageStream(staging_key)` (Task 5 provides the real seam; for this task the write buffer takes a `WriteBufferFromFileBase` sink + a `staging_key` + the `HashingWriteBuffer`).
- Produces: `CaContentWriteBuffer` in S3 mode computes `cityHash128` while streaming to the sink; `on_finalized(hash_hex, size, staging_key)` (staging_key replaces temp_path). `temp_ownership_transferred` semantics preserved (the transaction owns the staging object post-finalize and deletes it after promote).

- [ ] **Step 1: Failing test** — construct a `CaContentWriteBuffer` in S3 mode over a fake sink; write N bytes; assert (a) the sink received exactly the bytes, (b) `on_finalized` was called with the correct `cityHash128` hex and size and the staging key, (c) on `cancel()` before finalize the staging object is scheduled for removal (fake records a delete), (d) on successful finalize `temp_ownership_transferred==true` and the dtor does NOT delete.
- [ ] **Step 2–4:** implement the mode; run; pass.
- [ ] **Step 5: Commit** `feat(cas): CaContentWriteBuffer S3-staging mode (stream-to-staging while hashing)`.

## Task 5: Backend seams + promote with condemn/resurrect gate

**Files:**
- Modify: `.../Core/CasBackend.h` (`supportsConditionalCopy()`, `stageStream(key)`, `promoteStaged(staging_key, blob_key) -> PutResult`)
- Modify: `.../Core/CasObjectStorageBackend.{h,cpp}` (Native impl: `stageStream` = unconditional streaming write to staging key; `promoteStaged` = `copyObjectConditional(staging→blob, If-None-Match:*)` → `{Done, token=dest_etag}` on created, `{PreconditionFailed, {}}` on 412; Emulated: `supportsConditionalCopy()=false`, seams unused)
- Modify: `.../ContentAddressedTransaction.cpp` (`publishStaging` ~197-266: for `backend=s3` pending blobs, drive the promote through the **same** condemn/resurrect gate `putBlob`/`CasBuild` uses today)
- Test: `gtest_cas_s3_staging.cpp`

**Interfaces:**
- Consumes: `copyObjectConditional` (T2), `PendingBlob{backend,staging_key}` (T4).
- Produces: `Backend::promoteStaged(...) -> PutResult{outcome, token}`. The promote in `publishStaging`:
  1. `promoteStaged(staging_key, blobKey)`:
     - `Done` ⇒ token = dest ETag; record edge `(blobKey, token)`.
     - `PreconditionFailed` ⇒ blob already exists: `head(blobKey)` for the current token, then run the existing **condemn check** (`observeAndAdmit`/`.meta` point-read). If **condemned**, resurrect by `promoteStaged` semantics **from our staging object** to a fresh incarnation — concretely, re-run the conditional copy is not possible (dest exists), so resurrect must overwrite via the sanctioned **copy-forward-from-source** path (`copyForwardFromCondemned`-equivalent) sourced from the **staging object**, matching today's `uploadFromSource` (which re-uploads to change the token). Fail-closed on ambiguity.
  2. Only after promote + condemn-check completes, delete the staging object (T6 cleanup).

**Note to implementer:** read `Core/CasBuild.{h,cpp}` (`putBlob` `CasBuild.h:46-49`, promote `CasBuild.h:90-92`, `copyForwardFromCondemned`, `uploadFromSource`, `observeAndAdmit`) to reproduce the condemn/resurrect gate exactly. The ONLY change vs today is the upload primitive (conditional server-side copy from staging instead of `putIfAbsentStream` from a local file) and the resurrect source (the staging object). Do not weaken the gate.

- [ ] **Step 1: Failing test** — with a fake conditional-enforcing backend: (a) promote to a fresh blobKey ⇒ `Done`, token = the fake's dest ETag, edge recorded; (b) promote when blobKey already exists & Clean ⇒ `PreconditionFailed` ⇒ HEAD-adopt existing token, no overwrite; (c) promote when blobKey exists & **Condemned** ⇒ resurrect **from staging** (fake asserts the copy source was the staging key, NEVER the blobKey), token bumped; (d) assert no code path ever issues an unconditional copy over a live blob.
- [ ] **Step 2–4:** implement; run; pass.
- [ ] **Step 5: Commit** `feat(cas): S3 staging promote via conditional copy, condemn/resurrect gate preserved`.

## Task 6: Staging cleanup + mount-lease sweeper + crash recovery

**Files:**
- Modify: `.../ContentAddressedTransaction.cpp` (`cleanupPendingTempFiles` ~142-153: for `backend=s3`, `object_storage->removeObject(staging_key)` after successful promote; abort path leaves staging for the sweeper)
- Create: `.../Core/CasStagingSweeper.{h,cpp}` (mount-start sweep of `staging/<mount_id>/` + a GC age-sweep hook; lease-fenced)
- Modify: GC discovery to **exclude** the `staging/` prefix (verify it already does — different prefix from `blobs/`); wire the age-sweep into the GC round or mount lifecycle
- Modify: crash-recovery/precommit-replay path — **never promote a precommit whose copy is not verifiably complete** (re-drive `promoteStaged` from the surviving staging object, or abort the recovery of that build)
- Test: `gtest_cas_s3_staging.cpp` + `gtest_cas_gc_*` (staging excluded from fold)

**Interfaces:**
- Produces: `CasStagingSweeper::sweepOwnMountOnStart()`, `::ageSweep(now, ttl)`; both **lease-fenced** (skip if the mount lease is not held / a staging object is younger than TTL and possibly in-flight).

- [ ] **Step 1: Failing tests** — (a) successful commit deletes the S3 staging object; (b) an aborted transaction leaves staging, and `sweepOwnMountOnStart` reclaims only THIS mount's `staging/<mount_id>/`; (c) the age-sweep does not delete a staging object younger than TTL; (d) GC fold does not list/condemn any `staging/` object; (e) recovery of a precommit whose blobKey does not yet exist re-drives the copy from staging (or aborts) and never makes the manifest visible without the blob.
- [ ] **Step 2–4:** implement; run; pass.
- [ ] **Step 5: Commit** `feat(cas): S3 staging cleanup + mount-lease sweeper + crash-recovery copy re-drive`.

## Task 7: Integration + soak (live RustFS, opt-in disk)

**Files:**
- Create: `tests/integration/test_cas_s3_staging/` (a `with_rustfs` CA disk with `cas_staging_backend=s3`, `cas_s3_staging_min_bytes` low enough to force the S3 path for the test data)
- Create/modify: a ca-soak variant config enabling `cas_staging_backend=s3`
- Test assertions: large-blob INSERT lands at `blobs/<shard>/<hash>`; `staging/` empty post-commit; the mount-time probe passes on RustFS (enforcing) and the S3 path activates; a two-replica same-content dedup race leaves exactly one blob and no leak; SELECT returns correct data; the **conditional-copy enforcement** (412 on a second promote) is exercised end-to-end.

- [ ] **Step 1: Failing integration test** — opt-in disk, INSERT a part large enough to exceed the threshold, assert blob at the content key + empty staging + correct SELECT.
- [ ] **Step 2: Run** `python -m ci.praktika run "integration" --test test_cas_s3_staging` (redirect to a build-dir log; subagent summarizes). Verify it fails first (feature not wired end-to-end) then passes.
- [ ] **Step 3: Soak** — run a short phase-3 chaos soak on the S3-staging variant; assert `dangling==0` at every checkpoint and no `staging/` accumulation.
- [ ] **Step 4: Commit** `test(cas): S3-native staging integration + soak lane (with_rustfs, opt-in)`.

---

## Self-Review (spec coverage)

- Off-by-default ✅ (T0 default Local, guard test). Write-once conditional ✅ (T1/T2/T5). Fail-close ✅ (T3 probe → Local). Resurrect-from-staging ✅ (T5c test). Reads content-hash-only ✅ (no read-path change touched). Staging sweeper + GC exclusion + crash recovery ✅ (T6). Capability probe ✅ (T3, live in T7). Memory fast-path (spec §7) is **deferred** — not in this plan (separable optimization; noted for a follow-on).
- Type consistency: `StagingBackend`, `ConditionalCopyResult`, `PutResult{outcome,token}`, `PendingBlob{backend,staging_key}`, `promoteStaged`, `copyObjectConditional`, `stageStream` used consistently across tasks.
- Placeholder scan: the `cas_s3_staging_min_bytes` default (`64 MiB`) and the probe key layout are concrete; where a task requires reading a current signature (CasProbe, CasBuild gate), the task names the exact file + the existing function to mirror — an implementer subagent reads it.
