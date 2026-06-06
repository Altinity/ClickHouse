---
description: Design spec for getting the full stateless suite green on content-addressed (CA) MergeTree over both local and S3/minio — B85 (wire the 404→repair fallback into the blob data GET via a pre-resolve HEAD), B87 (CA moveFile delegates a part-directory rollback rename to moveDirectory instead of LOGICAL_ERROR), B86 (route system logs off the CA-S3 disk), plus triage of the non-CA noise.
sidebar_label: 'CA all-green (B85/B87/B86)'
sidebar_position: 16
slug: /superpowers/specs/ca-all-green
title: 'Content-Addressed MergeTree — Path to All-Green (B85/B87/B86)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Path to All-Green {#ca-all-green}

**Status:** design spec (authored unattended; decision-maker = the implementing model). **Date:** 2026-06-06. **Branch:** `cas-mergetree-poc`. **Goal:** the full stateless suite passes on CA-over-local AND CA-over-S3(minio) with **no CA-caused failures** (the realistic bar: same pass set as a plain-disk run, modulo legitimately by-definition-gated tests + genuine environment failures that also fail on plain disk). Closes B85, B87, B86; triages the non-CA noise.

## 1. B85 — read-path 404→repair safety net (blob data GET) {#b85}

**Problem.** `getStorageObjects` resolves a part-file read to `blobs/<H>/<g>` via `resolveBlobGenKeyForRead` (which reads the best-effort `active` hint), then hands a bare `StoredObject` to a generic `ReadBufferFromS3`/local read. `active` can be stale (a failed best-effort PUT, or the GC swept the generation after caching), so the GET 404s (`S3_ERROR`/`FILE_DOESNT_EXIST`) with no recovery. The repair helper `repairBlobGenOn404` (LIST `blobs/<H>/`, pick the highest present generation, repair `active`, cache) exists and is correct but is only wired to the manifest read (`loadPartManifestOrThrow`), not the blob data read.

**Design (chosen: pre-resolve HEAD).** Add a private helper in `ContentAddressedMetadataStorage` and use it at the two content-blob return sites in `getStorageObjects` (the live ref branch and the shadow branch):
```
ContentAddressed::BlobObjectKey resolveBlobGenKeyChecked(const ContentAddressed::BlobHash & h) const
{
    auto key = resolveBlobGenKeyForRead(h);
    if (object_storage->tryGetObjectMetadata(key.string(), /*with_tags=*/false))   // HEAD; nullopt on miss (S3 AND local)
        return key;
    return repairBlobGenOn404(h);   // LIST highest-present gen, repair active, cache
}
```
- `tryGetObjectMetadata` returns `nullopt` on a missing object for **both** backends, so we never have to discriminate `S3_ERROR`(NoSuchKey, which has no dedicated ErrorCode) from `FILE_DOESNT_EXIST` deep in the gather path. The failure is caught BEFORE the key reaches the read buffer.
- Common case (never-resurrected g0): `resolveBlobGenKeyForRead` returns the cached/`active`-derived g0 key, the HEAD succeeds, return — one extra HEAD per content object (the repair LIST runs ONLY when `active` was stale). Acceptable for correctness; the same `getStorageObjects` call already HEAD+GETs the manifest. (A future perf refinement — skip the HEAD for a cache-confirmed generation — can be backlogged; it is not needed for correctness or to pass the suite.)
- Rejected alternatives: carry `BlobHash` on `StoredObject` (no attributes field; serialization fallout) and a lazy catch-in-the-read-buffer wrapper (must string-match `S3_ERROR`, no existing CA seam) — both larger/riskier.

**Bonus:** this ALSO fixes the B87 trigger. `tryLoadPartsToAttach` loads a part via the ordinary `readFile`→`getStorageObjects` path; a part wrongly seen as "broken" during ATTACH because a blob generation was missing-at-the-resolved-key is exactly this stale-`active` 404, now repaired.

## 2. B87 — CA moveFile delegates a part-directory rollback rename {#b87}

**Problem.** `PartsTemporaryRename` does its FORWARD attach rename via `disk->moveDirectory(detached/X → detached/attaching_X)` but its ROLLBACK via `disk->moveFile(detached/attaching_X → detached/X)`. Both endpoints are part DIRECTORIES; CA `moveFile` only understands part-FILE renames, so the rollback hits `throw Exception(LOGICAL_ERROR, "moveFile source not recorded")` → under `abort_on_logical_error` (CI) the server aborts (in release it's a recoverable exception, but the rollback still fails).

**Design.** In `ContentAddressedTransaction::moveFile`, before the existing non-empty-`file` part-file guard, detect a both-sides part-DIRECTORY rename and delegate to `moveDirectory` (which already owns the `detached→detached` re-key via `rekeyDetachedPartDir`):
```
auto src = ContentAddressed::parsePartFilePath(from);
auto dst = ContentAddressed::parsePartFilePath(to);
/// A part-DIRECTORY rename reached moveFile (PartsTemporaryRename::rollBackAll undoes an attach with
/// moveFile(detached/attaching_X -> detached/X), whereas the forward tryRenameAll uses moveDirectory).
/// CA re-keys directories; delegate to moveDirectory (it owns the detached<->detached re-key) instead of
/// throwing LOGICAL_ERROR (B87).
if (src && src->file.empty() && dst && dst->file.empty())
{
    moveDirectory(from, to);
    return;
}
```
**Predicate safety:** a part-FILE rename always has a non-empty `file` on both sides (the existing guard relies on this), so a real file move can never be misdetected as a directory move. The non-part-path branch runs first and is unaffected. `moveDirectory` for an unrecognized dir-shape falls through to its own (no-op-safe) re-key logic, so the change only converts a hard `LOGICAL_ERROR` into the already-correct path.

## 3. B86 — route system logs off the CA-S3 disk {#b86}

**Problem.** The CA-S3 test config sets `content_addressed_s3` as the DEFAULT MergeTree policy, so system logs (`query_log`, etc.) write to the cacheless S3 disk → ~84 `Timeout exceeded (180s) while flushing system log … to S3`. (Plain s3 tests avoid this by backing their policy with a `cached_s3` disk.) The ~46 process wall-clock timeouts are downstream of the same synchronous-flush stall.

**Design.** In `tests/config/config.d/content_addressed_s3_storage_policy_for_merge_tree_by_default.xml`, pin every system-log table to the always-present local `default` storage policy (`<query_log><storage_policy>default</storage_policy></query_log>`, … for all system logs). User tables still default to `content_addressed_s3` (the `<merge_tree><storage_policy>` is unchanged), so the suite still tests CA-over-S3 for the data under test; only the log-flush firehose moves to local. CA-LOCAL config is NOT changed (local object store is fast; logs-on-CA there is extra coverage that already passes post-cap-fix). After this, re-measure: if a residual set of genuinely S3-heavy user-table tests still wall-clock-times-out, bump the timeout scoped to the CA-S3 param set (`ci/defs/job_configs.py`) only — do not raise it globally, and only if needed.

## 4. Non-CA noise — triage (not CA bugs) {#noise}
- **Alias-marker WIP** (`03649_alias_marker_distributed.sh`, `03650_..._different_databases.sql`): untracked working-tree leftovers from ANOTHER feature branch (chmod-missing / unregistered `enable_alias_marker` setting). They fail on any disk, not CA. Do NOT adopt/fix another feature's WIP; document as non-CA. (`03649` chmod is harmless to fix if trivial; `03650`'s missing setting is out of scope.)
- **Environment-only** (qemu x86-emulation tests on ARM, IPv6 `::1` not available, MySQL not running): fail identically on a plain-disk run on this host. Not CA. Document; not in the CA bar.
- **Known-gated** (`no-content-addressed-storage`): correctly SKIPPED. **B81** (`04295` GC-leftover flake) and **B65** (`03829`) are tracked.

## 5. Testing / acceptance {#testing}
1. Implement B87 + B85 + B86; build `clickhouse` + `unit_tests_dbms`; **158 `ContentAddressed*` gtests stay green**.
2. Targeted verify: `00753_alter_attach` (B87 — no abort), `03566`/`03711` (B85), a CA-S3 run of a few log-heavy tests (B86 — no flush timeout), plus the GC tests (`04279`/`04292`).
3. **Full reruns:** CA-local full suite + CA-S3 full suite on the fixed binary. Triage each: the ONLY acceptable remaining failures are {legitimately-gated (skipped), genuine environment (qemu/IPv6/MySQL — confirm they fail on plain disk too), the non-CA alias-marker WIP, the B81 04295 flake}. ANY CA-caused failure → fix and re-run (iterate).
4. Acceptance = no CA-caused failure on EITHER kind; the suite's pass set matches a plain-disk run modulo the documented buckets.

## 6. Risks {#risks}
- **B85 per-read HEAD** adds one HEAD per content-blob read (cached generation still HEADed). Correct + safe; a perf refinement (skip HEAD for a verified-cached gen) is a future backlog item, not needed for green.
- **B87 delegation** — confined to the empty-`file` both-dirs case; cannot affect file moves. Verify `moveDirectory` handles the exact rollback from→to (`detached/attaching_X → detached/X`) — it does (the forward uses the same branch).
- **B86** — if a system log a test asserts on is now on `default`, ensure the test doesn't depend on it being on CA (it shouldn't — tests assert log CONTENT, not storage). Watch for any test that checks `system.parts` for a log table on the CA disk.
