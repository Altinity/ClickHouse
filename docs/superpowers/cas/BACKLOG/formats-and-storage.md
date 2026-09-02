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
- **[S3-native staging §7] memory fast-path for small blobs** — MINOR (optional) — Buffer sub-single-part blobs in memory and feed the ordinary unconditional `publishBlob` streaming transport directly (no disk staging or native copy). The optimization must not bypass the mandatory blob `HEAD` or the shared `publication_attempted` state.

## Backends — real-store validation, GCS, LIST consistency {#backends}

- **[GATE #1: Azure] real-store GC validation on Azure** — GATE — The earlier AWS + GCS exact-delete/GC
  validation completed on 2026-07-03. Azure has not started and remains the last leg of that reclaim
  gate. The newer unconditional-publication GCS gate is separate and is still blocked, as recorded in
  the [2026-08-23 live results](/superpowers/cas/unconditional-blob-publication-live-results). Before
  implementing Azure CAS, decide whether to introduce the provider-neutral conditional-operations
  layer described in the [draft proposal](/superpowers/specs/cas-object-storage-conditional-operations-proposal).
  The refactor is justified primarily if Azure is the next concrete backend.
- **[GCS] every GCS-specific item lives in [`BACKLOG/gcs.md`](gcs.md)** — the unrun `gcp_oauth` and
  ambiguity arms of the live gate, `[relink-confirm-lane-livelock]`, `[gcs-hot-control-keys-429]`,
  `[gc-run-connect-failure-propagation]`, the environment findings of 2026-09-02 and the fix order.
- **[LIST consistency on real S3] token-diff discovery under eventual consistency** — {#list-consistency-real-s3} — TEST/GATE — S3's LIST may not reflect a just-PUT key; code handles it conservatively but needs real-S3 testing. Add a LIST-consistency probe in `Cas::Probe` before LIST-derived discovery is trusted on a given store. Also load-bearing for the (moot) registry-removal LIST premise.
- **[B196] cap `s3_max_connections` to backend permits** — HARD (cheap) — CONFIRMED still open: no CA code caps `s3_max_connections`; prevents 503 + retry storm under high concurrency.
- **[F2 / rustfs#3231] false-404-under-load + overwrite-leak upstream report + repro** — INFRA — Dominant scale blocker (caps merge-heavy full-scale + 4h chaos soak). Our side is safe (clamp + destruction suppression). Needs a #3231-free/fixed rustfs or the S22 fault-proxy stand; build a repro on the #3231 dir-bloat repro.

## Local / emulated backend {#local-backend}

Collected 2026-07-23 (user direction): every "local backend" story lives HERE, so the class is visible
as one body of work instead of scattered minors. Blob bodies now use `HEAD` followed by unconditional
publication, and `emuPublishBlobAtomically` installs them with a sibling temporary file plus rename.
Ordinary emulated metadata/ref writes still use `LocalObjectStorage`'s direct `O_TRUNC` path and lack
atomic visibility to concurrent readers. Nothing in this section affects S3/GCS production pools.

- **[disk-error-audit] ✅ CLOSED 2026-08-23: temp-file + rename in the local blob write path** —
  `ObjectStorageBackend::emuPublishBlobAtomically` writes a fully validated blob body to a sibling
  `.publish-<uuid>.tmp` object, renames it over the destination under `emu_mutex`, and cleans the
  temporary file on failure. Deterministic tests cover complete old-or-new visibility and failure
  cleanup. The independent peak-memory debt remains under
  [`emulated-resurrect-should-spill-to-disk`](../BACKLOG.md#emulated-resurrect-spill-to-disk).
- **[B26 / B135] local / NFS / shared-fs as a first-class backend** — DESIRABLE — Unit-tested over
  `LocalObjectStorage`; needs server-level doc + the put-if-absent atomicity caveat (racy multi-writer
  on local/NFS) + multi-mount safety notes. (B66a is the concrete instance of the caveat.)
- **[B66a] concurrent-fetch torn read of a shared `detached` ref on local storage** — MINOR — OPEN.
  Ordinary emulated metadata/ref writes still use `emuWrite` and
  `LocalObjectStorage::writeObject` directly on the final key with `O_TRUNC`; the blob-only
  `emuPublishBlobAtomically` helper does not cover them. A concurrent reader/writer of the same ref
  key can therefore observe a half-written object. Safe on S3 (atomic PUT). Freeze dodged this class
  by design (one ref per frozen part, no shared container); the residual case is concurrent writers
  of one `detached/<part>` name. Fix ordinary emulated mutable/ref installation with an atomic sibling
  temporary file plus rename while preserving its lock and token semantics. Deliberately out of the
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

- **[TXN-ONE-PIPELINE follow-up] committed-ref DDL overlay** — HARD — The TXN-ONE-PIPELINE initial landing (decision Tension 1) implements the one-overlay invariant for the part-build write path only. Committed-ref DDL ops (DROP/MOVE/RENAME TABLE via `removeDirectory`/`removeRecursive`/`republishRef`/`dropNamespace`) and verbatim table/mountpoint files stay the immediate class (durable-at-call-time), not overlay-deferred. Follow-up = execute Appendix-A Tasks 1.2/1.3-DDL/1.4-DDL (a `pending_ref_ops` overlay `{Drop,Move,Replace}` keyed by `(ns,ref)`, materialized in `commit` after `publishStaging`, plus the new read surface so transaction reads answer "ref dropped/moved"). Deferred because it fixes no known motivating bug and has the highest regression risk (interacts with the empty-cover `commitTransaction` workaround and DROP/DETACH/ATTACH rollback); gate before doing it — audit that no single CA transaction interleaves an overlay-deferred part op with an immediate DDL/verbatim op order-sensitively. Abandon-path note: the funnel converts durable DDL/verbatim ref-ops from commit-time-queue-drain to call-time (immediate class), applying at call-time and NOT compensated on abort, consistent with spec §Transaction-Model. Interim risk: a DDL/ALTER that applies a durable ref-op then aborts before disk commit leaves the early-applied drop — narrow, not hit by the INSERT-sink abort, and covered by the DDL-exercising stateless/soak gates (a dangling/lost-part there = STOP + pull that op's overlay-deferral forward). (Confirmed 2026-08-04: orphaned-open cluster C-1118 describes this same `pending_ref_ops` overlay mechanism — already tracked, folded in as confirmation.)
- **[ca-scratch-path-docker-entrypoint-permission] a CA disk's default scratch path can be root-locked by the docker entrypoint** — {#ca-scratch-path-docker-entrypoint} — DOC — If a sibling `local`-type disk declares an explicit `<path>` under the same `<data-path>/disks/` tree as a CA disk's default (undeclared) scratch path, the official docker image's entrypoint `chown`s only the leaf it was told about, leaving the shared `disks/` parent root-owned — the CA disk's later scratch-dir creation then throws `Permission denied` and the server exits before listening. Not a code bug (fail-closed is correct); a deployment nuance worth documenting: keep any sibling local disk's declared path outside the CA disk's `disks/<name>/` namespace. Already worked around in the ca-soak harness configs; open ask is to carry the warning into user-facing deployment docs.
- **[SEAL-DECODE-REMAINING-FIELDS] ✅ CLOSED at HEAD by `2bbcbb18683` (verified 2031-triage CAS-038); kept for provenance** — {#seal-decode-remaining-fields} — was SMALL FOLLOW-UP (2026-07-29, T16 concern 2, deliberately left out to keep the F1 diff reviewable) — `btr` missing `key`/`ck` and `cnd` missing `shard` default silently exactly the way `cls` did before T16's fix; same treatment owed (required-field refusal, CORRUPTED_DATA). One small task, same test file, after T16 merges.
- **[cas-format-version-floor] `checkCompatibility` never rejects a version below a type's own birth generation** — {#cas-format-version-floor} — DESIRABLE — `checkCompatibility` (`Formats/CasFormat.cpp`) throws `UNKNOWN_FORMAT_VERSION` above `G_BUILD` but accepts anything below it, including a version under the type's own birth generation (e.g. a header claiming generation 1 for a type born at generation 4 decodes as legal); `decodeRefCatalog` also discards the parsed header once the check passes, so nothing downstream can recover the version for logging. Not required by the empty-universe GC gate fix that surfaced it — closing this floor would only shrink an already-accepted residual, not remove it. Fix: a per-type birth-generation floor enforced centrally in `checkCompatibility`, refusing a version below `changePoints(id).front().generation` as `CORRUPTED_DATA`; blast radius is the shared format layer used by every CAS object type, so needs its own failing-first coverage and a fixture/artifact audit.
- **[codecs.md standardization]** — DOC (proposed) — Complete the magic table (`OwnerProto`/`ServerEpochProto`/`MountLeaseProto`/`FoldSealProto`); decide the `RunRef.checksum` / `PartManifest.payload_digest` CRC-boundary before release; standardize binary version fields (`compatibility_version` vs local `format_version`); type `CARN` record streams at open. A proposed target structure exists but is not adopted; one pre-release cutover.

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[gc-snap-codec-tlv-review] B176 format-freeze review of the `gc/snap` streamed codec's envelope TLV fields** — DESIRABLE — Concrete review action item ahead of the format-freeze gate.
- **[mixed-ca-tiered-topology] mixed CA + non-CA tiered hot/cold storage policy support is unverified** — DESIRABLE — Governs the severity of a relink pool-UUID mis-advertise bug; no test or doc confirms this topology is supported.
- **[runfilereader-seek-duplicate-key-skip] `RunFileReader::seek` block-boundary duplicate-key skip bug** — MINOR — A real correctness edge case with a stated fix direction, found during review.
- **[runfile-block-crc-coverage-gap] run-file block CRC coverage gap (head fields not covered)** — MINOR — Concrete integrity-hardening item: the block CRC does not cover the head fields.
- **[format-version-field-naming-standardization] `format_version`/`compatibility_version` field-naming standardization across `RunFile`/`PartManifest`** — MINOR — Real but purely cosmetic, low urgency.
- **[sec4-decoder-size-bounds] decoders lack an explicit size bound for a valid-CRC oversized shard/manifest** — DESIRABLE (security-relevant, SEC-4) — A pool-write-capable party could place an enormous but validly-framed object; `decodeRootShard`/`decodePartManifest`-class decoders were not confirmed to validate size bounds.

## Copy-object paths out of a CA disk copy envelope bytes (2031-triage CAS-020) {#move-out-copies-envelope-bytes}

`getStorageObjects` cannot express a blob's payload offset — `StoredObject` has no offset field
(`ContentAddressedMetadataStorage.cpp:1859-1865`; the envelope header is ≥225 bytes,
`CasManifestReader.cpp:144-160`), so any consumer that server-side-copies those keys copies the
envelope, not the file. `clonePart` branches on the DESTINATION only
(`DataPartStorageOnDiskBase.cpp:735`), so a MOVE **out** of a CA disk falls into
`DiskObjectStorage::copyFile` (`:300`), and `DataSourceDescription::operator==`
(`DiskType.cpp:35-38`) ignores `metadata_type` — a CA s3 disk and a plain s3 disk on the same
endpoint compare EQUAL, so `copyFileImpl` (`:522`) takes the raw server-side copy. The same class
reaches `BackupWriterS3/Azure/Disk::copyFileFromDisk` through `getBlobPath`. No move-out refusal
exists.

Loudness correction to the audit: every real part has inline files (≤1 MiB) whose storage key is
`""`, so the operation aborts loudly — garbage objects plus a confusing error, not silent
corruption. Hence P2, not a release blocker.

Fix options: (1) refuse move-out/copy-out on a CA source with a clear message (CA-local, preferred
first step), or (2) make `DataSourceDescription` equality account for `metadata_type` — that is
generic disk code, so it needs the upstream-consult step
([[feedback_upstream_code_consult_first]], [[feedback_cas_upstream_coupling_minimization]]) before
anyone touches it.

## Write-once probes and multipart blob publication (2031-triage CAS-031) {#write-once-probe-misses-multipart}

**CLOSED for blobs by the 2026-08-23 protocol rewrite; identifier kept for provenance.** Blob-body
PUT/copy no longer carries a destination precondition and therefore no longer asks a small
write-once probe to certify multipart behavior. Streaming bodies use ordinary multipart; explicit S3
staging requires native same-store copy and fails closed when that capability is absent. The
conditional capability battery remains load-bearing for small mutable metadata/control objects,
native-token `HEAD`, and exact deletion, none of which delegates a blob-body multipart contract to
the probe. Related LIST trust work remains tracked at {#list-consistency-real-s3}.

## `skip_access_check` and the decommission open silently skip the whole probe (2031-triage CAS-030) {#skip-access-check-no-signal}

`skip_access_check` skips the capability probe wholesale, and `clickhouse-disks cas-drop-member`
opens read-only, which never probes at all — so `openForDecommission`'s comment ("the calling disk
validated it") does not hold for capability in that path. Not the "removes every bucket
defense" the audit claims: the single-attempt gate and the residual proof stay (deliberate split in
`fb25e8cd3f6`, pinned by `CASPool.SkipAccessCheckStillEnforcesSingleAttemptGate`). The residue is
observability: nothing tells the operator the probe was skipped, and the probe's own step-8 text
claims the versioning check "has no override", which is false. Fix: log the skip at default level
(with what is consequently unverified) and correct the message. P3.

## Versioning enabled AFTER mount: misclassified error, delete-marker check at 1 of 8 sites (2031-triage CAS-029) {#versioning-enabled-after-mount}

The audit's headline (versioning slips through because the GCS config check fails open) is false: a
mandatory BEHAVIOURAL probe at every writable mount detects it — `CasProbe.cpp:209-224` checks
`created_delete_marker`, tested at `gtest_cas_probe.cpp:61`, and it rejects a versioned bucket on AWS
and on any S3-compatible store regardless of the config check. Three narrow residuals do stand:

- versioning turned ON **after** the mount surfaces as `LOGICAL_ERROR` (`Gc/CasGc.cpp:804`) for a
  state an operator can reach — reclassify (`CORRUPTED_DATA`-class or a named CAS error), per
  [[feedback_logical_error_tests_death_split]];
- `created_delete_marker` is consulted at only ONE of the eight destructive delete sites, so the
  post-mount case is caught late and only on the blob-body path — decide whether the check belongs in
  the shared delete helper;
- the GCS config check fails open, and `skip_access_check` skips the probe entirely (see
  {#skip-access-check-no-signal}). Update 2026-09-02: this is now the decided behaviour, not a
  residual — an unverifiable versioning probe logs a warning and the mount proceeds, because the
  live stand's credential lacked `storage.buckets.get` and the refusal made a missing IAM grant a
  hard outage. A probe that verifies `Enabled` still refuses.

## Optional-field / bound residuals in the control-object decoders {#decoder-optional-field-residuals}

Residuals left after the 2026-08-21 audit-verification pass over the control-object decoders (most of
the reported family turned out to be either already required or a safe default; these two are the
real leftovers).

- **[outcome-log-oc-not-required] `decodeOutcomeLog` does not require `oc` (or `k`)** — {#outcome-log-oc-not-required} — MINOR — `Formats/CasGcOutcomesFormat.cpp:113` requires `ha`/`h`/`tt` but not `oc`, so a record without the outcome word it exists to carry decodes as the struct default `OutcomeKind::Spared` (`CasGcOutcomesFormat.h:37`), and `k` defaults to `ObjectKind::Blob`. The header comment two lines above the decoder claims "required record fields ... are checked", so code and contract disagree. Reachable only through the byte-adopt arm (`Gc/CasGc.cpp:981`) on a foreign/damaged log, and the sole consumer is the round report's tally (`Gc/CasGc.cpp:989-996`) — no deletion or liveness decision reads it — so the impact is a skewed counter, not a safety hole. Owed treatment: the same required-field refusal `cls`/`btr`/`cnd` already got in the fold-seal codec.
- **[gc-state-encode-no-line-cap] `encodeGcState` does not enforce the line cap its own decoder enforces** — {#gc-state-encode-no-line-cap} — MINOR — `decodeGcState` reads the body with the 64 KiB `line_cap` (`Formats/CasGcStateFormat.cpp:43`, `Formats/CasFormat.cpp:168`) while `encodeGcState` checks only `gc_shards >= 1` (`:21`), so unlike `encodeFoldSeal` (which calls `checkLineBytes` per line) nothing stops the writer from persisting a `gc/state` its own reader would refuse. The only variable-length field is `msc`, an object key returned by a LIST page (`Gc/CasOrphanManifestSweep.cpp:910`), bounded by the backend's key length (~1 KiB) — so this is unreachable today and would fail closed (`CORRUPTED_DATA`, GC wedged until repaired) rather than corrupt anything. Owed: the symmetric encode-side cap check, for the same reason the fold-seal codec has one.

Also verified in that pass: {#seal-decode-remaining-fields} is CLOSED — `btr` now requires
`key`/`ck`/`shard`/`gen` and `cnd` requires `shard`/`ct`/`pt`/`ocr` (`Formats/CasFoldSealFormat.cpp:495`,
`:515`), landed in `2bbcbb18683`.

## `gc_shards` has no upper bound anywhere {#gc-shards-no-upper-bound}

- **[gc-shards-no-upper-bound]** — MINOR — `gc_shards` is validated for `!= 0` at every boundary (setting: `ContentAddressedSettings.cpp:178`; mint: `Pool/CasPoolMeta.cpp:116`; `_pool_meta` decode: `Formats/CasPoolMetaFormat.cpp:164`; `gc/state` decode: `Formats/CasGcStateFormat.cpp:66`) and never for an upper bound, while the value sizes per-round vectors (`Gc/CasGc.cpp:1804`, `:3178`, `:3181`, `:4109-4111`). A pool-write-capable party (or a fat-fingered creation-time setting) can therefore make every GC round die in allocation. Fails closed and loudly — no corruption, no out-of-range indexing (`blobShard` is `% gc_shards`; a seal's `run.shard >= gc_shards` is refused at `Formats/CasFoldSealFormat.cpp:113`) — and the durable pair is cross-checked (`Gc/CasGc.cpp:4540`). Owed: a sane ceiling (a small constant, or one derived from the fold-seal reservation) at the setting boundary and in `decodePoolMeta`. Same family as {#sec4-decoder-size-bounds}.
- **[gc-shards-config-override-silent] adopting the pool's `gc_shards` over the node's configured value is silent** — {#gc-shards-config-override-silent} — MINOR (operability) — `Pool::open`/`openForDecommission` overwrite the configured value with the durable one (`Pool/CasPool.cpp:497`, `:842`) and `PoolMeta::createOrValidate` returns the existing `_pool_meta` without ever looking at the passed `gc_shards` (`Pool/CasPoolMeta.cpp:124-128`, same treatment `blob_header_len` gets). Pool-authority is the intended design and the setting is documented creation-time-only, but an operator who edits `<gc_shards>` on an existing pool gets no signal at all. Owed: one `WARNING` naming both values when they differ.

## The 16 MiB per-manifest inline budget has no re-classification path (2031-triage CAS-044) {#manifest-inline-budget-no-spill}

`PartWriteTxn::stageManifest` enforces three fail-closed caps
(`Pool/CasPartWriteTxn.cpp:813-834`): `kMaxManifestEntries`, the per-entry
`kMaxLargestInlineEntryBytes = 1 MiB`, and the aggregate
`kMaxManifestInlineBytesTotal = 16 MiB` (`:55-57`). The per-entry cap has a spill path — an inline
candidate above `INLINE_CAP = 1 MiB` is written to a local temp file and staged as an ordinary blob
(`ContentAddressedTransaction.cpp:932-970`) — but the aggregate cap has none: nothing tracks a
running inline total while files are staged, and `stageManifest` cannot re-place an entry, so a part
whose inline-eligible files sum past 16 MiB throws `LIMIT_EXCEEDED` and the INSERT (or the merge that
would produce that part, or a repoint through the same call at `:356`) fails permanently and
reproducibly for that schema/data shape.

Reachability is wider than the audit's "many projections or skip indexes" framing, because the
inline-candidate set is the complement of the `partFileMustStayBlob` allowlist
(`ContentAddressedTransaction.cpp:65-73`) and that allowlist misses the shipped default names (see
{#part-file-suffix-allowlist-memory}): `primary.cidx`, `.cmrk4` (compact-part marks with substreams),
every skip-index `.idx`, and — because the `primary.idx` branch is an exact-name compare — every
projection's `<proj>.proj/primary.idx`. Projection files live in the PARENT part's manifest (routing
keeps them as `<proj>.proj/<file>` entries under one ref, `ContentAddressedMetadataStorage.h:459-472`),
so ~8-17 near-1-MiB files across a handful of projections reach the aggregate cap while each stays
under the per-entry cap that would have spilled it.

Loudness: fail-closed and loud (`LIMIT_EXCEEDED` before the body PUT and before
`uploadPendingBlobs`, `ContentAddressedTransaction.cpp:410-412`) — no corruption, no partial
publication, only local scratch work wasted. Hence P2, not a release blocker; the failure mode is a
table whose parts can never be written, discovered at the first INSERT.

Owed: track the inline total during staging and demote the largest inline candidates to blobs before
the cap (the machinery already exists on the per-entry path), so the cap becomes a placement decision
instead of a write refusal. Fixing {#part-file-suffix-allowlist-memory} shrinks the exposure but does
not close it (projection metadata and skip-index bodies stay inline-eligible by design).

## Control-object bytes are materialized whole before the format cap is checked (2031-triage CAS-036) {#control-object-read-precap-materialization}

Every control-object read is `Backend::get` → whole body into a `String` → `openObject`, which is
where the per-format `object_cap` first fires (`Backend/CasObjectStorageBackend.cpp:356`
`readStringUntilEOF`, `Formats/CasTextFormat.cpp:388-403`). `get` already HOLDS the authoritative
size — it HEADs the key first and passes `hr->size` down (`:614`) — so nothing stops a pre-read gate
except that `get` is format-agnostic and no caller passes its cap down. Consequence today is a
transient allocation the size of whatever sits at the key before the cap can refuse it; under the
memory tracker that surfaces as `MEMORY_LIMIT_EXCEEDED` on a GC/mount/recovery thread instead of the
`CORRUPTED_DATA` the caps exist to produce. Fail-loud either way, and only a bucket-credential holder
can plant such an object ({#pool-trust-boundary-undocumented} — that credential is the whole trust
boundary), hence P3, not a security defect.

Fix direction: give `get`/`getStream` an optional expected-cap argument (or a thin
`getControlObject(FormatId, key)` wrapper) and refuse `hr->size > object_cap` before the read, so the
cap is enforced at the same place for every format and the error is the pinned one.

Second, independent residue in the same read: `JsonObjectReader::nextKey` de-duplicates keys with a
linear `std::find` over a `std::vector<String>` (`Formats/CasTextFormat.cpp:173-175`), so one line
with k distinct keys costs Θ(k²) comparisons. The line caps bound k, but the `RefLog`/`RefSnapshot`
line cap is 64 MiB (`Formats/CasFormat.cpp:144`) — millions of keys, i.e. a single planted record
that pins one thread for a long time. Cheap fix: a sorted/hashed seen-set, or a key-count cap per
object.

## The `std::stoull` key-number parses accept `-1` and the manifest read window can wrap (2031-triage CAS-037) {#numeric-parse-and-window-wrap}

Three GC key-shape parses use `std::stoull` inside `catch (...)` (`Gc/CasGc.cpp:1488`, `:1619`,
`:4094`). The catch handles junk, but `std::stoull("-1")` does not throw — it returns `UINT64_MAX`
(verified) — so a key named `.../gc/gen/-1/...` parses as a legitimate generation. At `:4094` that
value flows into `const uint64_t generation = max_gen + 1` (`:4102`), which wraps to 0 and hands the
REBUILD path a generation number the very comment above it forbids ("must never collide with debris
of the lost era"). The same lesson was already learned and fixed elsewhere in this tree with
`std::from_chars` (`ContentAddressedMetadataStorage.cpp:332-340`, commit `fc89b827d74`); these three
sites did not get it. Fix: `std::from_chars` at all three, plus a saturating/`checked` successor for
`max_gen + 1`.

Related in the same class: `readBlobPayload` forms the read window as
`location.offset + location.length` twice (`ContentAddressedMetadataStorage.cpp:2004-2006`) where
`length` is the manifest's `sz`, parsed with `readIntText`'s default
`DO_NOT_CHECK_OVERFLOW` (`Formats/CasPartManifestFormat.cpp:222`, `src/IO/readIntText.h:246`). A `sz`
near `UINT64_MAX` wraps the sum below `offset`, and `ReadBufferFromFileView` has no
`left_bound <= right_bound` precondition — the window collapses to an immediate EOF (empty read,
loud failure downstream), so this is robustness, not silent corruption. Fix on the CAS side (validate
`sz` or use a saturating add) rather than in the generic upstream buffer.

## The manifest payload digest is a canonical re-encode, which makes the format's `Tolerant` key policy inert (2031-triage CAS-041) {#manifest-digest-by-reencode}

`decodePartManifest` verifies `payload_digest` by re-encoding the decoded model
(`Formats/CasPartManifestFormat.cpp:297-301`, `computePayloadDigest` at `:306-317` deep-copies the
manifest and calls `encodePartManifest`). Two residuals, neither of them a live bug:

- **Design contradiction, not a reachable failure.** `cas_part_manifest` is registered
  `KeyStrictness::Tolerant` (`Formats/CasFormat.cpp:165`) and the format framework explicitly plans
  for additive changes that keep the old reader floor (`Formats/CasFormat.h`, `FormatChangePoint`
  doc) — but a tolerated unknown key cannot survive a digest computed from what the local struct can
  re-emit, so for this one format the tolerance affordance can never be used. The audit's stated
  consequence ("a manifest written by any other generation reads as `CORRUPTED_DATA`") is NOT
  reachable today: `currentCompatibilityVersion()` always stamps `G_BUILD` and there is no
  write-down-to-floor policy (tracked as B180), so a newer object is refused earlier and louder by
  `checkCompatibility` (`Formats/CasTextFormat.cpp:327`, `UNKNOWN_FORMAT_VERSION`), and every
  generation bump so far is recreate-only. This has to be settled as part of the format-freeze /
  `PartManifest.payload_digest` CRC-boundary decision above ({#codecs-and-protocol}): either digest
  the wire bytes (which makes tolerance real and removes the re-encode) or drop the format to
  `Strict` and say plainly that additive evolution of this object requires a generation bump.
- **Cost, measured.** The recompute is 27–63% of total decode time on this build (60×100 B entries:
  45 µs of which 12 µs; 500×2 KiB: 510 µs of which 200 µs; 200×64 KiB (13 MB): 4.6 ms of which
  2.9 ms), i.e. roughly 1.4×–2.7× the decode work, plus two extra transient copies of the payload
  (the probe manifest and its encoded bytes) on top of the decoded model — with a 256 MiB
  `object_cap` for this format, that peak is worth bounding. Digesting the wire bytes removes both.

## Emulated token-state expiry compares an fs-mtime etag against the local clock (2031-triage CAS-067) {#emu-token-state-clock-skew-leak}

MINOR (emulated mode only — CI, unit tests, local development; no S3/GCS pool is affected).

`emuMintToken` seeds its token from the object's etag, which `LocalObjectStorage` derives from the
file's mtime in nanoseconds (`ObjectStorages/Local/LocalObjectStorage.cpp:391`, `:427`). The bound on
`emu_token_state` (added 2026-07-18, `08ea8d1200e`) expires an entry only when that etag is
"comfortably in the past" relative to the process's own `system_clock`:
`etagComfortablyInThePast` requires `now_ns > etag_ns && now_ns - etag_ns >= 2s`
(`Backend/CasObjectStorageBackend.cpp:447-464`).

Those two clocks are the same clock on a local filesystem, but not necessarily on a shared mount, and
not across an NTP step:

- **mtimes from a clock AHEAD of ours** (NFS/CIFS server ahead): `etag_ns > now_ns` forever, so
  `etagComfortablyInThePast` never returns true. `deleteExact` then always takes the retain branch
  (`:1050-1057`), and the lazy sweep pops the queue record after the age check without erasing the map
  entry (`:513-520`) — so `emu_token_expiry` stays bounded, but each deleted key leaks one
  `emu_token_state` entry for the lifetime of the backend instance.
- **local clock stepped BACK**: while `now_ns <= candidate.queued_at_ns`, the sweep breaks at the
  oldest record (`:512-513`) and prunes nothing at all. This one self-heals once the clock passes the
  queued timestamp — it is not permanent.

No correctness impact: a leaked entry only makes a later same-key recreate mint a disambiguated
`etag#N` token (still unique, still fail-closed against a stale token), and a recreate with an
advanced etag overwrites the entry outright (`:577-579`). The cost is memory: ~100 B per distinct
deleted key in a long-running emulated-mode process under skew.

Fix direction: derive the expiry decision from a monotonic, locally-generated stamp (the
`queued_at_ns` already recorded in `EmuTokenExpiry`) instead of re-reading the storage-supplied etag,
so the guard never compares two unrelated clocks; or cap `emu_token_state` by size as a backstop.
The existing unit coverage
(`gtest_cas_backend.cpp:847-919`, `DeleteExactErasesEmuTokenStateOnlyWhenEtagIsComfortablyOld` /
`EmuTokenStateEventuallyPrunesDistinctShortLivedKeys`) already has the injectable clock/etag doubles
needed for a failing-first test.

## S3 staging debris is reclaimed only at mount start and is invisible to fsck (2031-triage CAS-081) {#s3-staging-reclaim-only-at-mount-start}

An aborted/exception-unwound transaction deliberately leaves its S3 staging objects in place
(`ContentAddressedTransaction.cpp:174-207` — the local temp file is removed unconditionally, the S3
object only `else if (committed)`), because a staging object is a re-readable publication source and
must outlive one failed attempt. Shared monotonic `publication_attempted` state ensures only the first
absent publication may use verbatim native copy; condemned and subsequent publications retag and
stream. That part of the design is settled. What is NOT
covered is the reclaim side:

- `sweepOwnMountStaging` (`Pool/CasServerRoot.cpp:1473`) has exactly ONE call site, at mount start
  (`ContentAddressedMetadataStorage.cpp:824-828`, gated on `staging_backend=s3` + `!read_only` after
  startup has required `ObjectStorageCopyMode::NativeOnly`). There is no periodic in-mount reclaim, so under an opt-in
  `staging_backend=s3` disk the staged bytes of every killed/cancelled INSERT, failed mutation and
  aborted MOVE accumulate for the WHOLE uptime of the mount and are reclaimed only by the next restart.
- GC never lists `staging/` (a top-level prefix disjoint from `blobs/`, pinned by
  `gtest_cas_s3_staging.cpp:900-910`), and `runFsck` inherits the same blob-discovery prefix, so the
  debris does not appear as `unaccounted` — an operator has no shipped way to see how much staging
  residue a LIVE member is holding. (For a DEAD member the drain does exist:
  `SYSTEM CAS DROP POOL MEMBER` removes `staging/<victim_srid>/` and reports
  `staging_objects_removed` — `Tools/CasDecommission.cpp:232-236`.)

Fix direction: run the same prefix sweep periodically (e.g. from the GC scheduler's own pacing loop,
still fenced to THIS mount's `staging/<server_root_id>/` prefix) with an age filter that cannot reach
an in-flight staging object, and surface the staging-object count/bytes in an introspection surface
(fsck report row or `system.content_addressed_mounts`). Low urgency: the whole path is opt-in and OFF
by default, and every restart clears the residue.

## The blob envelope is stamped but never decoded on any production path (2031-triage CAS-089) {#blob-envelope-never-read-back}

Every blob body is `[fixed-length envelope][payload]`, and the read path takes the payload offset from
pool meta rather than from the object: `CasManifestReader::locate` returns
`.offset = meta.blob_header_len` (`Pool/CasManifestReader.cpp:155-159`), consumed by
`getBlobViewPlan`/`readBlobPayload` (`ContentAddressedMetadataStorage.cpp:1987-1989`, `:2002-2004`).
`decodeEnvelopeHeader` has exactly one non-test caller — `SYSTEM CAS INSPECT`
(`Tools/CasInspect.cpp:630`) — so the envelope's `v` (compatibility version) and `tag`/`bld` are
forensic fields, never a gate on a read; `runFsck` never GETs a body either, by design (GET budget).

This is a deliberate design (constant-shift locate, no per-object header read, documented at
`Formats/CasBlobEnvelopeFormat.h:44-58, 76-93` and `Pool/CasManifestReader.cpp:147-153`), and divergence
between an object's real envelope length and the pool's `blob_header_len` is unreachable today:
`blob_header_len` is minted once at pool creation and the pool is authoritative on reopen
(`Pool/CasPoolMeta.cpp:122`, `:150`), a mount whose expected value disagrees is rejected
(`Pool/CasPool.cpp:124-128`), the value is range/multiple-validated on every decode
(`Formats/CasPoolMetaFormat.cpp:36-46`, called `:171`), and cross-generation format change is gated
pool-wide by `min_reader_generation` (`Formats/CasPoolMetaFormat.cpp:174-177`) rather than per blob.

Residual for the format-freeze pass (same gate as `[gc-snap-codec-tlv-review]` and
`[codecs.md standardization]`): decide explicitly whether the envelope stays a write-only forensic
record or gains a read-side use, and record the decision. Two cheap options if it stays write-only:
drop `decodeEnvelopeHeader`'s unused `object_size` parameter
(`Formats/CasBlobEnvelopeFormat.cpp:162` — the parameter is already unnamed) or give it a real
consumer; and have fsck's `detail` mode optionally decode the envelope of a sampled blob so
`header_len == blob_header_len` and `v` are proven somewhere outside `INSPECT`. No integrity item:
a wrong offset would hand MergeTree shifted bytes, which its own compressed-block checksums reject
loudly.

## `Layout::checkNamespace` is the only CAS path validator that admits `.` and `..` segments (2031-triage CAS-091) {#checknamespace-admits-dot-segments}

`Layout::checkNamespace` (`Formats/CasLayout.cpp:319-343`) rejects an empty namespace, an empty
segment (leading/trailing/doubled `/`) and the reserved `_files`/`_manifests` segments — but NOT a
`.` or `..` segment. Every sibling validator in the tree does reject them:

- `validateServerRootId` (`Pool/CasServerRoot.h:199-225`) is otherwise the SAME function and has the
  check at `:216-218` ("uses a relative segment ('.' or '..')");
- `isCanonicalRefName` (`Primitives/CasCodecUtil.h:71-…`, doc `:65-70`);
- `isCleanRelativeNamespaceFileName` (`Formats/CasLayout.h:25-31`);
- the manifest entry-path check (`Formats/CasPartManifestFormat.cpp:197-210`), whose comment even
  claims it enforces "the same path hygiene as `CasLayout::checkNamespace` … no empty/'.'/'..'
  segments" — a parity claim that is false at HEAD. `Pool/CasServerRoot.h:192` carries the mirror
  version of the same false claim.

The mechanism the divergence would enable is real, not theoretical: for `ObjectStorageType::Local`
the backend is auto-selected into `EmulatedSingleProcess`
(`ContentAddressedMetadataStorage.cpp:688-690`) and keys are joined under the disk root via
`getCommonKeyPrefix` (`:727-729`), i.e. they ARE filesystem paths, so a namespace segment `..` would
escape the pool root.

No production reachability today, which is why this is hygiene and not an incident:
- every live namespace is `serverPrefix() + "/" + mirroredArchiveNamespace(table_uuid)`
  (`ContentAddressedMetadataStorage.cpp:1267-1273`), and the prefix is the config value already run
  through `validateServerRootId` (`ContentAddressedSettings.cpp:192`);
- `mirroredArchiveNamespace` builds `store/<u3>/<uuid>@cas@` or `data/<db>/<tbl>@cas@`
  (`Parts/PartPathParser.cpp:376-386`) from escaped identifiers / a hex UUID;
- the one user-supplied string on the shadow path is the FREEZE backup name, and MergeTree escapes it
  before it becomes a directory (`MergeTreeData.cpp:9947` `escapeForFileName(with_name)`, which turns
  `..` into `%2E%2E`).

Fix is one line plus prose: add the `.`/`..` segment rejection to `checkNamespace` (or have it call
the shared helper), which also makes the two "same hygiene as `checkNamespace`" comments true. Worth
a gtest case next to the existing namespace-validation cases, since `validateNamespace`
(`Formats/CasLayout.h:451`) is explicitly the re-validation hook for namespaces reconstructed from
untrusted listed keys on the GC ref-intake path.

## FREEZE reuses a backup name silently on CA: `isDirectoryEmpty`'s part-dir short-circuit disables the `DIRECTORY_ALREADY_EXISTS` guard (2031-triage CAS-086) {#freeze-name-reuse-merges-shadow-ref}

P3, narrow but SILENT. `ContentAddressedMetadataStorage::isDirectoryEmpty` deliberately answers
`true` for any part-shaped directory, including a SHADOW part dir (`route` keeps
`ref = <part>`, `file = ""` for a `shadow/<name>/...` path,
`ContentAddressedMetadataStorage.cpp:1292-1299`, `:1799-1817`) — the short-circuit exists so
`DiskObjectStorage::removeDirectory` reaches the ref-unlink instead of throwing `CANNOT_RMDIR`
(`DiskObjectStorage.cpp:441-443`), which is right for the removal path and test-pinned
(`src/Disks/tests/gtest_ca_wiring.cpp:463`, `:580`).

The side effect is on the FREEZE path: `Backup` refuses a destination that exists and is not empty
(`src/Storages/MergeTree/Backup.cpp:146`), which upstream is what makes a second
`ALTER TABLE ... FREEZE WITH NAME 'x'` fail loudly for an already-frozen part. On CA the guard never
fires, so the second freeze stages into the SAME shadow ref and `publishStaging` takes the
existing-view repoint arm, merging the old frozen manifest's untouched entries with the new ones
(`ContentAddressedTransaction.cpp:338-392`). Re-freezing an unchanged part is idempotent and
harmless; re-freezing a DIFFERENT part that carries the same part name (after `REPLACE PARTITION` /
`ATTACH PARTITION FROM`, which can reuse `all_1_1_0`) silently produces one frozen ref mixing files
from two snapshots, where upstream would have refused the operation.

Cross-disk `ATTACH PARTITION FROM` into a content-addressed disk now works after `cfe9a6a3615`, so
the local→CAS route to this state is reachable instead of failing earlier in `freezeRemote`.
Reconsider this item's priority against that wider reachability rather than inheriting its old rank.

Fix options, both small: teach `isDirectoryEmpty` to keep the listing-based answer for shadow part
dirs (the removal-path rationale only covers live/detached part dirs, and shadow removal goes through
`removeRecursive`, not `removeDirectory`), or reject a shadow ref that already exists on the FREEZE
staging path. A gtest next to the existing shadow-shape cases plus a stateless `FREEZE WITH NAME`
twice case would pin whichever is chosen.

## A non-Atomic database or table literally named `detached`/`moving` is folded as the reserved dir with no refusal (2031-triage CAS-087) {#nonatomic-reserved-name-fold-no-refusal}

P3, accepted-limitation half already pinned, missing half is the fail-closed refusal.
`findPartDirComponent` scans for a `detached`/`moving` component before the right-to-left part-dir
grammar scan (`Parts/PartPathParser.cpp:205-212`); the ordering is load-bearing (without it the inner
part name steals the anchor and the reserved dir becomes part of a table namespace `DROP TABLE` never
cleans). The consequence — a non-Atomic `data/<db>/detached/...` path is indistinguishable from a
table's reserved `detached/` subdir — is a deliberately accepted limitation, documented at the anchor
site (`:214-221`) and pinned by
`CASPartPathParser.DetachedNamedTableIsKnownAmbiguityFoldedAsReservedDir`
(`src/Disks/tests/gtest_ca_wiring.cpp:229-245`).

What is NOT covered: nothing refuses such a table. Under a deprecated `Ordinary` database
(`allow_deprecated_database_ordinary`, default `false`, `src/Core/Settings.cpp:6712`) a table named
`detached` gets `ns = liveNamespace("data/<db>")` and every one of its parts folds onto
`detached/<part>` refs (`ContentAddressedMetadataStorage.cpp:1300-1311`); with the DATABASE named
`detached`, all tables in it collapse onto `detached/<table>` refs, so each table's parts share ONE
ref and concurrent publishes fight over it. Failures there are loud (promote conflicts), but the
config is silently accepted rather than refused. Fix: refuse at table-attach/mount time a non-Atomic
relative data path whose db or table component equals `kDetachedDirName`/`kMovingDirName`, naming the
reserved name in the message. Same class as the DiskEncrypted-over-CA refusal item.

## The per-namespace 64 MiB ref-table budget is a hard per-table ref ceiling with no spill path (2031-triage CAS-111) {#ref-table-64mib-admission-ceiling}

Every state-growing ref op is previewed against two whole-table encodings and refused when either
would pass 64 MiB: `admits` encodes the hypothetical complete snapshot and the hypothetical
whole-namespace removal transaction (`Pool/CasRefProtocol.cpp:718-736`, budgets
`ref_snapshot_max_bytes = ref_removal_max_bytes = 64 MiB`,
`Formats/CasRefSnapshotFormat.h:65`, `Formats/CasRefLogFormat.h:100`, per-table budgets minus wire
overhead at `Pool/CasRefLedger.cpp:1205-1208`), and the refusal is
`LIMIT_EXCEEDED` at `Pool/CasRefLedger.cpp:3021-3025`. One namespace is one table
(`srv1/<table_uuid>`, `Primitives/CasTypes.h:42-44`), one committed row is one part folder
(projection files live in the parent part's manifest), and a snapshot row is
`{"k":"c","rn":…,"me":…,"mb":…,"mo":…,"ts":…}` (`Formats/CasRefSnapshotFormat.cpp:57-70`) — on the
order of 75-95 bytes for realistic part names, i.e. a ceiling around 0.6-0.9 M refs per table.

Failure class: fail-closed and loud, checked before any object is created, no corruption and no
partial publication. But the refusal covers only GROWTH, and a merge grows before it shrinks, so a
table at the ceiling cannot merge its way back down — only removals (`DROP PARTITION`, part removal,
`DROP TABLE`) shrink the table. Reachability is well above MergeTree's own default active-part
ceiling (`max_parts_in_total = 100000`, `src/Storages/MergeTree/MergeTreeSettings.cpp:976`, enforced
at `MergeTreeData.cpp:6191`), so getting there needs that setting raised several-fold plus outdated
parts still holding refs — hence P2, not a release blocker.

Owed: (a) a `CurrentMetrics`/`system.` surface for per-table budget consumption plus a WARNING well
before the wall, so the ceiling is observable instead of arriving as a write refusal; (b) a design
answer for tables that legitimately want more refs — a chunked/multi-object snapshot, or a
partitioned ref table — since today the only way out is deleting parts. Same family as
{#manifest-inline-budget-no-spill} (a fail-closed format cap with no re-placement path).

## The manifest encoded-size cap is checked after the canonical text is materialized (2031-triage CAS-113) {#manifest-encoded-cap-checked-after-materialization}

MINOR (consistency; the failure is loud and fail-closed, and unreachable under the caps that precede it).

`stageManifest` encodes the whole canonical text and only then compares it against the cap
(`Pool/CasPartWriteTxn.cpp:854-858`, `kMaxManifestEncodedBytes = 256 MiB` at `:54`). The ref lane
answers the same question the other way round: `admits` (`Pool/CasRefProtocol.h:571`) decides whether
one MORE row still fits *before* any encoding, off O(1) incremental body-byte counters, against
per-table budgets computed once in recovery (`Pool/CasRefLedger.cpp:1206-1208`); and
`Formats/CasByteBudget.h:12-22` states the rule explicitly ("a producer that must decide whether one
MORE entry still fits cannot encode first and measure afterwards"), with `fitsObjectCap` already used
by `Formats/CasRefCatalogFormat.cpp:283`, `:367-379` and `Formats/CasFoldSealFormat.cpp:166`.

Not a release concern: the entry-count cap (`:816-818`, 1 048 576), the per-entry inline cap
(`:822-827`, 1 MiB) and the total inline cap (`:830-832`, 16 MiB) are all checked before the body is
built, so the 256 MiB text is unreachable for any realistic part folder, and the caller already holds
the whole `entries` vector in memory — the transient text is a constant factor on top of memory that
is already allocated, not a new order of magnitude. The cap also fires before `sealObject` (`:859`)
and before the PUT (`:880`), so it can never name a manifest in an owner transition.

Owed: the same additive pre-encode reservation the other control objects use (an entry-count ×
worst-case-per-entry reservation plus the fixed frame, via `fitsObjectCap`), so this format follows
the one discipline the tree already documents rather than being the exception.

## The staged-Inline arms of `createHardLink` / `moveFile` never establish the destination `PartWriteTxn` (2031-triage CAS-128) {#staged-inline-hardlink-no-dst-buildtxn}

`f7539af045c` ("inline-only part must create a Build") fixed exactly one of the three places that can
push an `EntryPlacement::Inline` entry into a part staging: the inline WRITE callback, which now calls
`buildFor(route, st)` (`ContentAddressedTransaction.cpp:943`). The two transfer paths were not fixed
and still push an Inline entry into a possibly-fresh destination staging without a `PartWriteTxn`:

- `createHardLink`, staged-source arm — the `Blob` case calls `adoptStagedBlob(..., buildFor(*dst, dst_st), ...)`
  (`ContentAddressedTransaction.cpp:1178`), the `Inline` case falls through to the bare
  `dst_st.entries.push_back(...)` at `:1181-1183` with no `buildFor`. (The committed-source arm below
  is fine: `buildFor(*dst, dst_st).adoptEvidence(*src_entry)` at `:1188`.)
- `moveFile`, staged-entry arm — `buildFor(*dst, dst_st)` is inside `if (&src_st != &dst_st && entry.placement == Cas::EntryPlacement::Blob)`
  (`ContentAddressedTransaction.cpp:1508-1522`), so a CROSS-PART move of an Inline entry leaves the
  destination staging with entries and no build (and the source's build stays with the source).

`publishStaging` then throws `LOGICAL_ERROR` "staged entries or removal marks for {}/{} without a
Build" (`ContentAddressedTransaction.cpp:395-397`) — unless the destination ref happens to be already
committed, in which case the repoint branch at `:337-392` tolerates a null build. Fail-closed and
loud: nothing is published, no silent corruption; but under `abort_on_logical_error` it is a server
abort, which is how the write-path instance of this same shape was originally found.

Not reachable from production today: every MergeTree `createHardLink` call site carries forward from a
COMMITTED source part (`MutateTask.cpp:2280,2294,2538,2574,2599` all pass `ctx->source_part`'s
storage; `DataPartStorageOnDiskBase.cpp:478` and `Backup.cpp:71` hardlink from a committed part), and
the part-file `moveFile` caller renames within ONE part directory
(`DataPartStorageOnDiskFull.cpp:335`), so `&src_st == &dst_st` and the build already exists from the
inline write. So this is latent-shape hardening, not a live bug.

Fix: add the idempotent `buildFor(*dst, dst_st)` to both Inline arms (it is idempotent, `:151-155`),
and add the two gtest cases next to `CASWiringWrite.InlineOnlyPartPublishesWithoutBuildCrash`
(`src/Disks/tests/gtest_ca_wiring.cpp:828`): stage an inline file into part A, then hardlink / move
ONLY it into a fresh part B, and commit.
