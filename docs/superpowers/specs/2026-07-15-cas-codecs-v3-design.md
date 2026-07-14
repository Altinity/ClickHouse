---
description: 'Design spec for the CAS codecs v3 cutover: every persisted metadata format becomes human-readable text (JSON / NDJSON / banner payload zones), codecs move into Core/Formats/ with a physical layering rule and a README registry, protobuf leaves the subsystem.'
sidebar_label: 'CAS codecs v3 design'
sidebar_position: 60
slug: /superpowers/specs/2026-07-15-cas-codecs-v3-design
title: 'CAS Codecs V3 Design'
doc_type: 'reference'
---

# CAS Codecs V3 Design {#cas-codecs-v3-design}

**Status:** approved design, 2026-07-15. The normative format reference is
`docs/superpowers/cas/codecs_proposal_v3.md` (file shape, conventions, integrity model, evolution
doctrine, alternatives); this spec is the actionable delta on top of it: the corrected object
inventory, the code placement, the registry, and the migration order. Supersedes
`codecs_proposal_v2.md`; `codecs.md` becomes a historical audit once the `Formats/README.md`
registry exists.

**Decisions fixed during brainstorm (2026-07-15):**

1. **Full sweep.** Every persisted CAS format converts, including the freshly-stabilized
   refsnaplog objects (`ref_log` / `ref_snap`) and the `blob_meta` sidecar. No binary exceptions
   except raw payload bytes.
2. **Conventions-only depth.** This spec fixes the file shape, naming policy, and per-object
   dispositions; per-object JSON key mappings (field tables) are implementation-plan work, executed
   under the naming policy (keys 2–5 chars, full-word values, units in the registry).
3. **Placement approach A.** All codecs move to `Core/Formats/` with a physical include-layering
   rule.
4. **`Formats/README.md` is the living registry** (bucket map + codec table + evolution rules),
   updated in the same commit as any format change.

## Container Design (By Reference) {#container-design}

Fixed in `codecs_proposal_v3.md` and already agreed; summarized for self-containment:

- **One file shape:** header line `{"type":"cas_<object>","v":N}` (type = magic, `v` = the only
  version field, gated before the body); body = one JSON object (control) / sorted NDJSON records
  (streams) / raw payload zone (manifest, blob); trailer line where the body is line-structured.
- **Parsing/writing:** the `ReadHelpers`/`WriteHelpers` JSON primitives (the `JSONEachRow` code
  path) — streaming pull parsing over `ReadBuffer`, hand-rolled deterministic writers. No DOM
  library.
- **Compression:** optional whole-object single-frame `zstd` (magic-sniffed, XXH64 on, declared
  content size checked against the per-type cap before allocation). Deterministic types pinned raw.
- **Integrity:** every guard names its consumer; storage-level corruption detection delegated
  (S3 checksums, zstd frame, MergeTree `checksums.txt`, `fsck`). Runs are guarded by the seal-held
  whole-file CityHash128 verified at the end of every full read. Padding zones ("no smuggling") are
  regenerated and verified.
- **Evolution:** additive = new tolerant key (best-effort on mutable objects until the pool floor
  rises); breaking = `v` bump + `G_BUILD`/`changePoints` + write-down-to-floor; deterministic
  formats are strict-keys with the **adoption pin** (re-encode at the `v` of the existing object on
  the `putDeterministicArtifact` conflict path). Mount gates (`min_reader_generation` forward,
  pool-meta `v` backward) unchanged.
- **Runs:** sorted NDJSON, no blocks, no footer, **no `seek`** — `RunFileReader::seek`,
  `inDegreeInGeneration`, and `SourceEdgeKeyCodec::seekPrefix` are deleted (production consumers
  are all sequential whole-reads). Accepted cost: ≈2× bytes on deterministic runs; fallback is
  re-binarizing `cas_run` alone.
- **Optional provider-metadata mirror:** `Content-Type` per family + `x-amz-meta-cas` = copy of the
  header line; the protocol never reads it.

## Corrected Object Inventory {#corrected-object-inventory}

Verified against the code on `cas-gc-rebuild` 2026-07-15. The v2/audit tables (and the first cut of
the v3 dispositions) were stale: root shard `CARS` no longer exists (ref state is refsnaplog),
retired set `CART` was removed 2026-07-10, and the blob-meta sidecar was missing entirely.

| Object | Key (`CasLayout`) | Today | v3 `type` | Family | Compression | Deterministic |
|---|---|---|---|---|---|---|
| Pool meta | `poolMetaKey` | proto | `cas_pool_meta` | Control | below threshold | no |
| Ref log txn | `refLogKey` | custom binary (`kRefLogTxnFormatVersion` + len-prefixed) | `cas_ref_log` | Control | optional | no |
| Ref snapshot | `refSnapshotKey` | custom binary | `cas_ref_snap` | Control | **yes** | no |
| Ref cleanup marker | `refCleanupMarkerKey` | empty body (key-only presence marker) | — | non-family (documented in registry) | — | — |
| Part manifest | `manifestKey` | `CAPT` hybrid (binary header + embedded `CARN`) | `cas_part_manifest` | Payload hybrid (JSON descriptor + banner payload zone) | yes | no |
| GC run | `blobTargetRunKey` | `CARN` blocks + footer | `cas_run` | Record stream | no | **yes** |
| Part-manifest cleanup run | `partManifestCleanupKey` | `CARN` | — | **deleted** (sealed but never read) | — | — |
| Fold seal | `foldSealKey` | proto (`CasGenerationSeal`) | `cas_fold_seal` | Control | no | **yes** |
| GC state | `gcStateKey` | proto | `cas_gc_state` | Control | below threshold | no |
| GC heartbeat | `gcHbKey` | 24-byte raw, unversioned | `cas_gc_hb` | Control | no | no |
| GC outcomes | `outcomesKey` | proto | `cas_gc_outcomes` | Control | yes | no |
| Owner anchor | `ownerKey` | proto | `cas_owner` | Control | no | no |
| Server epoch | `epochKey` | proto | `cas_epoch` | Control | no | no |
| Mount lease | `mountKey` | proto | `cas_mount_lease` | Control | no | no |
| Blob envelope | `blobKey` | 70-byte binary core + TLV, pad 256 | `cas_blob` | Payload (256-byte JSON header line + payload) | payload: none | header: n/a (tag fresh per incarnation) |
| Blob meta sidecar | `blobMetaKey` | fixed 22-byte binary (state/condemn_round/size) | `cas_blob_meta` | Control (one line) | no | no — CAS-swapped; token semantics unchanged |
| Roster | — | reserved `FormatId`, unbuilt | reserved | — | — | — |
| Namespace verbatim / mountpoint files | `namespaceFileKey` / `mountpointObjectKey` | raw | — | raw passthrough | — | — |

Envelope header fields (settled): `type`, `v`, `tag`, `bld`, `ts` (unix ms, number), `by`, `op`
(full words), `ch` (`VERSION_INTEGER`), `ref` (truncated to fit line ≤ 255 + `\n`); dropped:
`hash_algo` (identity lives in the key/refs), `domain_id` (written, never validated — YAGNI),
`header_hash` (no consumer; tag comparisons are storage-vs-storage), `writer_version`. Pad = spaces
to byte 255 + `\n`; `blob_header_len` stays 256.

Carried-over invariants that survive re-encoding: refsnaplog's key↔body binding check (body `ns` /
`txn_id` must match the key it was read from) stays and extends to the manifest; refsnaplog byte
budgets (`ref_txn_max_bytes`, removal class) are re-derived for JSON inflation at plan time.

## Code Placement: `Core/Formats/` {#code-placement}

```text
Core/Formats/
  README.md                  bucket map + codec registry + evolution rules (see below)
  CasFormat.{h,cpp}          registry: FormatId, type strings, G_BUILD, changePoints,
                             per-format {family, caps, compression, strictness} table (moves)
  CasTextFormat.{h,cpp}      the file shape: header/trailer lines, zstd arm, padding, banners,
                             error taxonomy — the only code that knows the shape (new)
  CasPoolMetaFormat.*        cas_pool_meta
  CasRefLogFormat.*          cas_ref_log (+ cleanup-marker key documentation)
  CasRefSnapshotFormat.*     cas_ref_snap
  CasPartManifestFormat.*    cas_part_manifest (descriptor + banner payload zone)
  CasRecordStreamFormat.*    cas_run: writer / reader / k-way merger + typed opens (was CasRunFile)
  CasFoldSealFormat.*        cas_fold_seal
  CasGcStateFormat.*         cas_gc_state + cas_gc_hb
  CasGcOutcomesFormat.*      cas_gc_outcomes
  CasServerRootFormats.*     cas_owner, cas_epoch, cas_mount_lease
  CasBlobEnvelopeFormat.*    cas_blob 256-byte header
  CasBlobMetaFormat.*        cas_blob_meta
```

**The layering rule is physical, not conventional:** `Formats/` may include only IO primitives and
the identifier vocabulary (`CasIds`, `CasToken`, `CasBlobRef`, `CasManifestId`, `CasRefIds`,
`CasBlobDigest`) — never `CasBackend`, `CasStore`, or subsystem headers. A codec that wants a
backend does not compile. Consequences:

- Wire structs (`BlobMeta`, `RefLogTxn`, `FoldSeal`, …) move **with** their codecs — they are the
  protocol vocabulary.
- Mixed files split: `CasBlobMeta` keeps its CAS lifecycle helpers in `Core/` while the struct +
  `encodeBlobMeta`/`decodeBlobMeta` move to `Formats/CasBlobMetaFormat`; `CasServerRoot` (932
  lines) sheds its wire code to `Formats/CasServerRootFormats` and keeps `claimMount`/renew
  protocol logic.
- `CasCodecUtil.h` contents fold into `CasTextFormat` or die with the binary codecs; `CasInspect`
  is gutted to a thin "decompress + print" convenience or deleted.
- Keep the `Cas` file-name prefix (tree-wide grep-ability).

**Checklist for adding a persisted object** (replaces the v2 6-step list):

1. One `FormatId` + type string + row in the `CasFormat` per-format table (unit test asserts
   completeness).
2. One `Cas<Object>Format.{h,cpp}` in `Formats/`: wire struct + `encodeX`/`decodeX` + invariants,
   nothing else.
3. One key builder in `CasLayout`, documented with owner and lifecycle.
4. One registration in the shared test harness.
5. One row in `Formats/README.md` — same commit.

## `Formats/README.md` — The Living Registry {#formats-readme}

Small, three parts:

1. **Bucket map:** the key tree under the pool prefix (`_pool_meta`, `cas/refs/<ns>/…` `_log` /
   `_snap` / cleanup markers, `cas/manifests/…`, blob + blob-meta keys, `gc/…`, `roots/…`,
   staging), one line each: what lives there, which codec reads it, who writes it.
2. **Codec table:** key pattern → `type` → `Formats/` file → family → compression → cap →
   deterministic?
3. **Evolution rules on one screen:** additive = new tolerant key (best-effort on mutable objects
   until the floor rises); breaking = `v` bump + floor; deterministic = strict keys + adoption pin;
   `!`-prefixed keys are critical; mount gates. Links to `codecs_proposal_v3.md` and
   `05-formats-and-backend.md` §schema-evolution.

The registry moves from `codecs.md` to this README so it lives next to the code and is reviewed
with it; `codecs.md` is retitled as the pre-v3 historical audit in the final migration step.

## Testing {#testing}

One parameterized harness (`src/Disks/tests/gtest_cas_formats.cpp`), one registration line per
format: round-trip; golden **text** files (compressed arm pinned against vendored zstd);
truncation at every line boundary → `CORRUPTED_DATA`; `v+1` → `UNKNOWN_FORMAT_VERSION`; wrong
`type` → `CORRUPTED_DATA`; unknown plain key → skipped (tolerant) / `CORRUPTED_DATA` (strict);
unknown `!`-key → `UNKNOWN_FORMAT_VERSION`; duplicate key → `CORRUPTED_DATA`; declared size over
cap → `CORRUPTED_DATA` with no allocation; padding/banner mutation → `CORRUPTED_DATA`. Existing
format tests migrate into harness registrations; subsystem tests (GC, store, e2e) are the
behavioral safety net per migration step. The harness explicitly does not claim bit-flip detection
on uncompressed bodies — that delegation is documented and tested where it lives (`fsck`, MergeTree
load checks).

## Migration Order {#migration-order}

Eight steps, each independently green, each format cutting over atomically in one commit
(pre-release: no dual-read arms):

1. **Bootstrap `Formats/`:** `CasTextFormat` + shared harness + `CasFormat` moves + README skeleton.
   Pure addition.
2. **Control plane:** pool meta, GC state + heartbeat (`cas_gc_hb` kills the unversioned
   exception), outcomes, fold seal, owner/epoch/lease — codecs rewritten as mapping-only in
   `Formats/`, proto messages deleted per object, decoder-strictness validations added while
   touching each.
3. **Refsnaplog:** ref log txn + ref snapshot → JSON; byte budgets re-derived; key↔body binding
   invariant kept.
4. **Blob meta** → one-line JSON; CAS/resurrect token semantics untouched.
5. **Runs** → sorted NDJSON + trailer + seal-checksum verification on every full read; delete
   blocks/footer/`seek`/`inDegreeInGeneration`/`seekPrefix` and the part-manifest-cleanup run
   (+ its fold-seal field and `partManifestCleanupKey`); merger rewritten line-based.
6. **Part manifest** → JSON descriptor + `head -v`-banner payload zone; delete the embedded stream
   path and `RunKind::ManifestEntries`.
7. **Blob envelope** → 256-byte JSON header line; drop `hash_algo`/`domain_id`/`header_hash`;
   golden tests re-pinned; `blob_header_len` stays 256.
8. **Finish:** compression flip per policy table; provider-metadata mirror in the backend PUT
   path; protobuf build wiring removed (`clickhouse_cas_proto`, `protobuf_generate_cpp`, the
   `libprotoc` link); docs updated (`codecs_proposal_v3.md` dispositions, `05-formats-and-backend.md`
   envelope + evolution sections, `codecs.md` retitled historical, README finalized); `CasInspect`
   gutted or removed.

## Deferred To Plans {#deferred-to-plans}

- Per-object JSON key mappings (field tables) under the naming policy.
- Refsnaplog byte-budget values for JSON inflation.
- Exact per-type caps and compression thresholds (constants next to `CasFormat` table).
- README initial content (step 1 deliverable).
- Soak measurement of the deterministic-run 2× cost; the re-binarize fallback triggers only on
  evidence.
