---
description: 'Live backlog — formats and storage backends: staging/adoption, real-store backends (S3/GCS/Azure), local/emulated backend, and codec/format items.'
sidebar_label: 'Formats & storage'
sidebar_position: 4
slug: /superpowers/cas/backlog/formats-and-storage
title: 'CAS Backlog — Formats and storage backends'
doc_type: 'guide'
---

# CAS Backlog — Formats and storage backends {#formats-and-storage}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for staging/adoption, real-store
backend validation, the local/emulated backend, and codec/format items.

## Staging / adoption {#staging}

- **[out-of-band staging adoption] adopt bulk-load/backup/external-tooling uploads via verified copy-forward** — HARD (needs spec) — Distinct from the landed (opt-in) S3-native writer staging. Objects uploaded out-of-band land under a staging prefix and are ADOPTED into the pool via the verified hash-then-publish copy-forward path instead of being trusted in place. Scope/semantics to be specced.
- **[S3-native staging §7] memory fast-path for small blobs** — MINOR (optional) — Buffer sub-single-part blobs in memory and `putIfAbsentStream` (no disk/staging/copy).

## Backends — real-store validation, GCS, LIST consistency {#backends}

- **[GATE #1: Azure] real-store GC validation on Azure** — GATE — AWS + GCS DONE (2026-07-03, live-validated). Azure not started — the last leg of the real-S3 reclaim release gate.
- **[GCS production-grade follow-ups]** — DESIRABLE — Compose-based conditional finalize for blobs above `gcs_max_conditional_put_bytes` (multipart silently ignores the precondition on GCS → currently throws `NOT_IMPLEMENTED`); `gcp_oauth` dialect probe validation against live GCS (ADC creds); generation-aware LIST discovery (GC re-reads every shard on GCS since list tokens are disabled — cost only); signed `x-goog-*` `extra_headers` on `gcs_hmac` (currently unsigned).
- **[LIST consistency on real S3] token-diff discovery under eventual consistency** — {#list-consistency-real-s3} — TEST/GATE — S3's LIST may not reflect a just-PUT key; code handles it conservatively but needs real-S3 testing. Add a LIST-consistency probe in `Cas::Probe` before LIST-derived discovery is trusted on a given store. Also load-bearing for the (moot) registry-removal LIST premise.
- **[B196] cap `s3_max_connections` to backend permits** — HARD (cheap) — CONFIRMED still open: no CA code caps `s3_max_connections`; prevents 503 + retry storm under high concurrency.
- **[F2 / rustfs#3231] false-404-under-load + overwrite-leak upstream report + repro** — INFRA — Dominant scale blocker (caps merge-heavy full-scale + 4h chaos soak). Our side is safe (clamp + destruction suppression). Needs a #3231-free/fixed rustfs or the S22 fault-proxy stand; build a repro on the #3231 dir-bloat repro.

## Local / emulated backend {#local-backend}

Collected 2026-07-23 (user direction): every "local backend" story lives HERE, so the class is visible
as one body of work instead of scattered minors. Common root: `LocalObjectStorage` writes are plain
`O_TRUNC` file writes — no atomic PUT, no conditional-write enforcement, no torn-read protection —
while the CAS protocol is designed against S3 atomic/conditional semantics. Nothing in this section
affects S3/GCS production pools.

- **[disk-error-audit] temp-file + rename in the local blob write path** — HARD — (moved from the
  2026-07-21 disk-error audit) `emuWrite` (`CasObjectStorageBackend.cpp:546-557`) streams through
  `LocalObjectStorage::writeObject`, which opens a plain `WriteBufferFromFile` directly on the final
  key with `O_TRUNC` (`LocalObjectStorage.cpp:250-277`). ENOSPC or a kill mid-write leaves a partial
  file at the final content-addressed key; the next `putIfAbsent` sees `emuExists == true` and returns
  `PreconditionFailed` = "already present" (`:776-780`), so the writer dedups against the truncated
  body. Presence-only admission + non-atomic local write is the ONLY corruption window the disk-error
  audit found (see also `operability-and-introspection.md`'s disk-error follow-ups). Native/S3 mode is
  not affected (`If-None-Match` + atomic completion). Fix: write to a sibling temp name and `rename`
  into place (or fix it inside `LocalObjectStorage`), paired with the dedup-admit size guard as
  defense-in-depth. Fixing this also closes B66a's torn-read mechanism below.
- **[B26 / B135] local / NFS / shared-fs as a first-class backend** — DESIRABLE — Unit-tested over
  `LocalObjectStorage`; needs server-level doc + the put-if-absent atomicity caveat (racy multi-writer
  on local/NFS) + multi-mount safety notes. (B66a is the concrete instance of the caveat.)
- **[B66a] concurrent-fetch torn read of a shared `detached` ref on local storage** — MINOR —
  `LocalObjectStorage` write is not atomic; a concurrent reader/writer of the SAME ref key can observe
  a half-written object. Safe on S3 (atomic PUT). Freeze dodged this class by design (one ref per
  frozen part, no shared container by design); the residual case is concurrent
  writers of one `detached/<part>` name. Mechanism is closed by the temp-file+rename item above;
  until then racy multi-writer on local/NFS stays documented-unsafe. Deliberately OUT of the
  2026-07-23 reserved-precommit iteration (orthogonal to the handoff protocol).
- **[emulated list-token contract]** — MINOR / VERIFY — `ObjectStorageBackend::list`
  in `EmulatedSingleProcess` mode may still return a different token kind than
  `head`/`get`/`put`/`delete` (child etag vs `emuObserveToken`), a Liskov gap vs `supportsListTokens`.
  The token-policy centralization (C1, landed) added `tokenForHead/tokenForList/tokenMatches`; verify
  the emulated `list` path now agrees or make `supportsListTokens()==false`.
- **[STATELESS-04286 EISDIR]** — pointer — `existsFile` mountpoint probe throws "Is a directory"
  (EISDIR) on the LOCAL CA backend; tracked in `utils/ca-soak/scenarios/BACKLOG.md`
  (STATELESS-04286-getmountpoint-eisdir, 2026-07-08); needs a fail-closed-semantics decision +
  re-check on RustFS/S3.
- Related, landed: `LocalObjectStorage` TOCTOU walk fix (B38) — upstream carve-out tracked in
  docs-and-cleanup.md, Group G.

## Codecs, formats, and disk-transaction protocol {#codecs-and-protocol}

- **[TXN-ONE-PIPELINE follow-up] committed-ref DDL overlay** — HARD — The TXN-ONE-PIPELINE initial landing (plan `docs/superpowers/plans/2026-07-16-cas-txn-one-pipeline.md`, decision Tension 1) implements the one-overlay invariant for the part-build write path only. Committed-ref DDL ops (DROP/MOVE/RENAME TABLE via `removeDirectory`/`removeRecursive`/`republishRef`/`dropNamespace`) and verbatim table/mountpoint files stay the immediate class (durable-at-call-time), not overlay-deferred. Follow-up = execute Appendix-A Tasks 1.2/1.3-DDL/1.4-DDL (a `pending_ref_ops` overlay `{Drop,Move,Replace}` keyed by `(ns,ref)`, materialized in `commit` after `publishStaging`, plus the new read surface so transaction reads answer "ref dropped/moved"). Deferred because it fixes no known motivating bug and has the highest regression risk (interacts with the empty-cover `commitTransaction` workaround and DROP/DETACH/ATTACH rollback); gate before doing it — audit that no single CA transaction interleaves an overlay-deferred part op with an immediate DDL/verbatim op order-sensitively. Abandon-path note: the funnel converts durable DDL/verbatim ref-ops from commit-time-queue-drain to call-time (immediate class), applying at call-time and NOT compensated on abort, consistent with spec §Transaction-Model. Interim risk: a DDL/ALTER that applies a durable ref-op then aborts before disk commit leaves the early-applied drop — narrow, not hit by the INSERT-sink abort, and covered by the DDL-exercising stateless/soak gates (a dangling/lost-part there = STOP + pull that op's overlay-deferral forward).
- **[ca-scratch-path-docker-entrypoint-permission] a CA disk's default scratch path can be root-locked by the docker entrypoint** — {#ca-scratch-path-docker-entrypoint} — DOC — If a sibling `local`-type disk declares an explicit `<path>` under the same `<data-path>/disks/` tree as a CA disk's default (undeclared) scratch path, the official docker image's entrypoint `chown`s only the leaf it was told about, leaving the shared `disks/` parent root-owned — the CA disk's later scratch-dir creation then throws `Permission denied` and the server exits before listening. Not a code bug (fail-closed is correct); a deployment nuance worth documenting: keep any sibling local disk's declared path outside the CA disk's `disks/<name>/` namespace. Already worked around in the ca-soak harness configs; open ask is to carry the warning into user-facing deployment docs.
- **[SEAL-DECODE-REMAINING-FIELDS] the rest of the silently-defaulting-field family in the fold-seal codec** — {#seal-decode-remaining-fields} — SMALL FOLLOW-UP (2026-07-29, T16 concern 2, deliberately left out to keep the F1 diff reviewable) — `btr` missing `key`/`ck` and `cnd` missing `shard` default silently exactly the way `cls` did before T16's fix; same treatment owed (required-field refusal, CORRUPTED_DATA). One small task, same test file, after T16 merges.
- **[cas-format-version-floor] `checkCompatibility` never rejects a version below a type's own birth generation** — {#cas-format-version-floor} — DESIRABLE — `checkCompatibility` (`Formats/CasFormat.cpp`) throws `UNKNOWN_FORMAT_VERSION` above `G_BUILD` but accepts anything below it, including a version under the type's own birth generation (e.g. a header claiming generation 1 for a type born at generation 4 decodes as legal); `decodeRefCatalog` also discards the parsed header once the check passes, so nothing downstream can recover the version for logging. Not required by the empty-universe GC gate fix that surfaced it — closing this floor would only shrink an already-accepted residual, not remove it. Fix: a per-type birth-generation floor enforced centrally in `checkCompatibility`, refusing a version below `changePoints(id).front().generation` as `CORRUPTED_DATA`; blast radius is the shared format layer used by every CAS object type, so needs its own failing-first coverage and a fixture/artifact audit.
- **[codecs.md standardization]** — DOC (proposed) — Complete the magic table (`OwnerProto`/`ServerEpochProto`/`MountLeaseProto`/`FoldSealProto`); decide the `RunRef.checksum` / `PartManifest.payload_digest` CRC-boundary before release; standardize binary version fields (`compatibility_version` vs local `format_version`); type `CARN` record streams at open. A proposed target structure exists but is not adopted; one pre-release cutover.
