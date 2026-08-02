---
description: 'Reference map of CAS persisted formats and codecs, with producers, consumers, storage paths, versioning notes, and standardization proposals.'
sidebar_label: 'CAS codecs'
sidebar_position: 10
slug: /superpowers/cas/codecs
title: 'CAS Codec Reference'
doc_type: 'reference'
---

# CAS Codec Reference {#cas-codec-reference}

**Status:** audit/reference for
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.

This document maps every persisted CAS format used by the Core layer: where objects are stored, what
bytes they contain, which code creates them, which code consumes them, why the format exists, and
what standardization work remains. The last sections collect cross-format observations and concrete
proposals.

Notation:

- `<prefix>` is `PoolConfig.pool_prefix`.
- `<ns>` is a `RootNamespace`.
- `<srid>` is a configured `server_root_id`.
- `<gen>` and `<attempt>` are GC generation and adopted attempt.
- `CasHeader` means protobuf field 1 containing `magic`, `writer_version`, and
  `compatibility_version`.

## Inventory {#inventory}

| Format | Stored at | Body family | Producer | Consumer | Purpose |
|---|---|---|---|---|---|
| Blob object, magic `CABL` | `<prefix>/blobs/<shard>/<blob-id>` | Fixed binary `CasEnvelope` header plus raw payload | `Build::putBlob`, `Build::uploadFromSource` | read path, GC, `fsck` | Deduplicated content-addressed bytes for part files. |
| Pool metadata, magic `CAPM` | `<prefix>/_pool_meta` | `PoolMetaProto` protobuf | `PoolMeta::createOrValidate` | pool open path | Pool identity and creation-time constants. |
| Root shard, magic `CARS` | `<prefix>/cas/refs/<ns>/<shard>` | `RootShardManifest` protobuf | `Store::mutateShard` through `encodeRootShard` | `Store::resolveRef`, GC fold, `fsck`, orphan sweep | Namespace ref state and journal; the commit point. |
| Part manifest, magic `CAPT` | `<prefix>/cas/manifests/<ns>/<writer_epoch>/<build_sequence>/<ordinal>.proto` | Custom binary; entries currently reuse the `CARN` block-framed record format | `Build::stageManifest` path through `encodePartManifest` | `Store::readManifest`, read path, GC, `fsck`, orphan sweep | Immutable description of one part's files. |
| Block-framed record stream, magic `CARN` | Embedded in `PartManifest`; also under GC attempt prefixes | Custom sorted-record binary, implemented as `RunFile` | `RunFileWriter` callers | `RunFileReader`, `RunMerger` | Ordered record streams for manifests and GC data-plane data. |
| GC state, magic `CAGT` | `<prefix>/gc/state` | `GcStateProto` protobuf | `Gc` lease/round code | `Gc`, writer publish gate through `RetireView`, store status helpers | Global GC round, lease, snap generation, and current retired-set refs. |
| GC heartbeat | `<prefix>/gc/hb` | Fixed 24-byte binary | `Gc` heartbeat code | GC lease stealing code | Advisory liveness pulse independent of round progress. |
| Fold seal, type `cas_fold_seal` | `<prefix>/gc/gen/<gen>/attempt/<attempt>/fold_seal` | Strict tagged text with `rfl`, `btr`, and `cnd` rows | GC fold through `encodeFoldSeal` and `putDeterministicArtifact` | later GC rounds, rebuild, `fsck`, orphan sweep | Write-once life coverage, run references, and condemned summaries for a generation. |
| Blob target run, magic `CARN` | `<prefix>/gc/gen/<gen>/attempt/<attempt>/blob_target/<shard>/<seq>` | `RunFile` | `CasBlobInDegree` and GC rebuild/fold paths | GC in-degree merge, `fsck` | Blob in-degree/source-edge data for one GC shard. |
| Retired set, magic `CART` | `<prefix>/gc/gen/<gen>/attempt/<attempt>/retired/<round>/<shard>` | `RetiredSetProto` protobuf | GC retirement publication | writer publish gate through `RetireView`, GC deletion, `fsck` | Current condemned object incarnations per GC shard. |
| Outcome log, magic `CAGO` | `<prefix>/gc/gen/<gen>/attempt/<attempt>/outcomes/<round>/<shard>` | `GcOutcomeLogProto` protobuf | GC delete/recheck path | GC replay/adoption path, observability | Idempotent record of delete/recheck outcomes. |
| Owner anchor, magic `CAOW` | `<prefix>/gc/server-roots/<srid>/owner` | `OwnerProto` protobuf | `claimOwnerOrThrow` | mount/open path | Sticky binding from `server_root_id` to server UUID. |
| Writer epoch, magic `CAEP` | `<prefix>/gc/server-roots/<srid>/epoch` | `ServerEpochProto` protobuf | `allocateWriterEpoch` | mount/open path | Durable monotone writer epoch allocation. |
| Mount lease, magic `CAML` | `<prefix>/gc/server-roots/<srid>/mount` | `MountLeaseProto` protobuf | `claimMount`, `MountLeaseKeeper` | local write fence, GC heartbeat floor | Live writer lease plus GC acknowledgement fields. |
| Namespace verbatim file | `<prefix>/roots/<ns>/_files/<relative-path>` | Raw bytes | writer/ref-payload path | read path | Mutable or verbatim files that are not content-addressed. |
| Mountpoint object | `<prefix>/roots/<relative-path>` | Raw bytes | mountpoint loose-file path | read path | Loose non-content-addressed files mirrored by path. |

## Format Families {#format-families}

The current system has six families:

- Protobuf metadata with `CasHeader`: the default for mutable/control/write-once metadata.
- Fixed binary envelope: currently only `CABL` blobs.
- Block-framed record format: `CARN`, implemented by `RunFile`.
- Binary part manifest: `CAPT` `PartManifest`.
- Strict raw tagged text control objects: `cas_fold_seal`.
- Raw passthrough bytes: namespace verbatim files and mountpoint objects.

There is also one exceptional fixed binary control record: `GcHeartbeat`. It is not raw user data,
but it also does not carry magic or versioning.

The family count is reasonable. The main inconsistency is not "too many proto messages"; it is that
the manual binary formats and `GcHeartbeat` do not follow the same naming, versioning, and validation
contract as the protobuf formats.

## Blob Object `CABL` {#blob-object-cabl}

**Storage path:** `<prefix>/blobs/<first-two-hex-chars>/<32-hex-blob-id>`, produced by
`Layout::blobKey`.

**Body:** fixed-size `CasEnvelope` header followed by raw file bytes. The pool currently uses
`blob_header_len = 256`, stored in `PoolMeta`. The envelope core is 94 bytes and contains:

- `magic = CABL`.
- `writer_version` and `compatibility_version`.
- `hash_algo`, currently `1` for `CityHash128`.
- `flags` and `header_len`.
- `logical_size`.
- `logical_hash`.
- `domain_id`, normally the pool id.
- `incarnation_tag`.
- `build_id`.
- `header_hash`, a `CityHash64` guard for the core header.

The header may also carry diagnostic TLVs such as `Provenance` and `intended_ref`. The payload begins
at `header_len`, normally 256.

**Producer:** `Build::putBlob` computes or receives the blob content hash, derives the key through
`Layout::blobKey`, and calls `Build::uploadFromSource`. The upload path writes the envelope and the
payload as one object. It uses `putIfAbsentStream` for fresh objects and exact-token overwrite when a
condemned incarnation must be revived.

**Consumers:** the read path resolves a `PartManifest` entry with `EntryPlacement::Blob` into a blob
location and reads payload ranges after the header. GC lists and reclaims blob objects by exact token.
`fsck` also enumerates blob keys and validates references.

**Purpose:** store part-file bytes once by content hash while allowing multiple physical
incarnations of the same logical payload. The incarnation zone is excluded from `logical_hash`, so a
new physical object can have a different token without changing the blob id.

**Versioning and integrity:** `decodeEnvelopeHeader` checks magic, `compatibility_version`, critical
extensions, header length, object-size arithmetic, and `header_hash`. Content identity is the pool
content hash over payload bytes, not a hash of the whole object.

**Notes:**

- `hash_algo` is stored, but the current decode path should explicitly reject anything except the
  supported value.
- `header_hash` is only a header corruption guard. It is not the blob identity.
- `ObjectKind::Blob` is the only live object kind. Future kinds should not be predeclared without a
  producer and reader.

## Pool Metadata `CAPM` {#pool-metadata-capm}

**Storage path:** `<prefix>/_pool_meta`.

**Body:** `PoolMetaProto` protobuf with `CasHeader` magic `CAPM`. It stores:

- `pool_id`: raw 16-byte pool identity.
- `root_shards`: root-shard count.
- `blob_header_len`: fixed blob payload offset, normally 256.
- `min_reader_generation`: pool-level startup gate.

**Producer:** `PoolMeta::createOrValidate`. On first creation it mints `pool_id` and writes with a
CAS create. On reopen it reads the object and treats the pool values as authoritative.

**Consumers:** store open path, blob-envelope writing, namespace layout decisions, and any code that
needs pool constants.

**Purpose:** establish pool identity and creation-time constants. The pool id doubles as the blob
envelope `domain_id`.

**Versioning and integrity:** protobuf with `CasHeader`; decode checks magic and
`compatibility_version`. It also validates constant invariants such as non-zero shard counts and a
valid blob header length.

**Notes:**

- The C++ header comment still describes strict JSON. The implementation is protobuf and the comment
  should be updated.
- The format is necessary and not redundant; it is the one authoritative source for constants that
  cannot safely come from local config after pool creation.

## Root Shard Manifest `CARS` {#root-shard-manifest-cars}

**Storage path:** `<prefix>/cas/refs/<ns>/<shard>`, produced by `Layout::rootShardKey`.

**Body:** `RootShardManifest` protobuf with `CasHeader` magic `CARS`. It stores:

- `shard_version`: current version of the root shard.
- `fence_round`: GC fence marker.
- `refs`: map from ref name to `RootRefProto`.
- `journal`: ordered `RootOwnerEventProto` stream.
- `incarnation_writer_epoch` and `incarnation_build_sequence`: shard incarnation stamp.

Nested records carry:

- `RootRefProto`: `ref_name`, `ManifestRefProto`, mutable per-ref files, and publish timestamp.
- `RootOwnerEventProto`: transition version plus optional old/new owner bindings.
- `OwnerBindingProto`: committed or precommit owner plus `ManifestRefProto`.

**Producer:** root mutations through `Store::mutateShard`, encoded by `encodeRootShard`. Examples are
publish, precommit, promote, abandon, drop, trim, fence, and reclaim.

**Consumers:** `Store::resolveRef`, `Store::listRefs`, GC fold, GC trim, orphan manifest sweep, and
`fsck`.

**Purpose:** this is the single commit point for namespace refs. It maps user-visible refs to
immutable part manifests and provides the ordered journal that GC folds into reachability deltas.

**Versioning and determinism:** protobuf with `CasHeader`. `std::map` keeps refs name-sorted; journal
order is insertion order and should match `transition_version` order. Additive fields are safe only
when old readers can ignore them without changing correctness.

**Notes:**

- This format is necessary; it is not replaceable by object listing without losing ordered
  transition semantics.
- Decode already validates several important invariants, including unknown `OwnerKind`, malformed
  16-byte fields, and invalid journal events.
- A useful additional strictness check is that map keys agree with embedded `ref_name`.

## Manifest Reference Subformat {#manifest-reference-subformat}

`ManifestRef` is not a standalone object, but it is a wire contract used by `RootShardManifest` and
`PartManifest`.

**Fields:**

- `writer_epoch`: durable monotone epoch allocated under the mounted server root.
- `build_sequence`: monotone sequence inside one writer incarnation.
- `manifest_ordinal`: per-build ordinal rendered as a six-digit filename.

**Storage:** inside `ManifestRefProto` and inside the `CAPT` part manifest body. A full manifest
identity is `ManifestId = (RootNamespace, ManifestRef)`.

**Purpose:** separate the compact journal reference from the storage key. The journal does not store
the namespace; the owning root context provides it.

**Notes:**

- The rendered filename is currently `000001.proto`, but the body addressed by the key is binary
  `CAPT`, not protobuf.
- `ManifestRef` alone is not globally unique; `ManifestId` is the protocol identity.

## Part Manifest `CAPT` {#part-manifest-capt}

**Storage path:** `<prefix>/cas/manifests/<ns>/<writer_epoch>/<build_sequence>/<ordinal>.proto`,
produced by `Layout::manifestKey`.

**Body:** custom binary format written by `encodePartManifest`. It contains:

- 4-byte magic `CAPT`.
- A 16-bit version field currently named `format_version`.
- `writer_version`.
- `ManifestRef`.
- `root_namespace_id`.
- `payload_digest`.
- Entries encoded as an embedded `CARN` record stream with `RunKind::ManifestEntries`.

Each manifest entry contains:

- `path`: part file path.
- `placement`: `Inline` or `Blob`.
- `blob_hash` and `blob_size` for blob-backed files.
- `inline_bytes` for inline files.

**Producer:** the build path stages one manifest body per part, computes `payload_digest`, encodes the
body, and writes it under `Layout::manifestKey`.

**Consumers:** `Store::readManifest`, the read path, GC cleanup, orphan sweep, and `fsck`. `readManifest`
checks that the journal `ManifestRef` matches the decoded body and that the decoded namespace matches
the owning namespace.

**Purpose:** immutable description of a part's file set. It keeps small files inline and points large
files to `CABL` blobs. It is also the source identity for GC source edges and manifest cleanup.

**Versioning and integrity:** decode checks `CAPT` magic and passes the 16-bit version field to
`checkCompatibility`. `payload_digest` is computed from canonical bytes with the digest field zeroed.

**Notes:**

- The `.proto` suffix is misleading because the body is not protobuf.
- The field named `format_version` is used as a global compatibility gate. It should either be
  renamed to `compatibility_version` or split from a true local `format_version`.
- `payload_digest` is written but not generally validated by `Store::readManifest`. Verify it or drop
  it before treating it as an integrity guarantee.
- The embedded `CARN` stream is convenient reuse of existing block framing, sorted-record encoding, and
  CRC checks. That is the only strong argument for it.
- The current decoder materializes entry bytes before reading them, so `PartManifest` does not get
  the main benefit of the `CARN` record-stream format: bounded-memory streaming and `seek`. As long as entries
  are read as one in-memory section, a single self-contained `CAPT` entries table would be simpler
  than "custom binary header plus nested record-stream binary".
- Preferred cleanup: make `CAPT` either fully self-contained custom binary, or fully protobuf if
  manifests are expected to stay small. Keep embedded `CARN` only if `PartManifest` grows a real
  streaming reader or shares typed `CARN` validation with GC record streams.

## Block-Framed Record Stream `CARN` (`RunFile`) {#run-file-carn}

**Storage paths:**

- Embedded inside `PartManifest` for manifest entries.
- `<prefix>/gc/gen/<gen>/attempt/<attempt>/blob_target/<shard>/<seq>` for blob-target in-degree runs.

**Naming:** the word `run` is external-sort/LSM jargon and is not self-explanatory here. In design
docs, call the format "`CARN` block-framed record stream" or "`CARN` record format". `RunFile` is the
current C++ implementation name. Avoid names like `RunBinary`: they read like "execute a binary" and
do not explain the data structure.

**Body:** custom sorted block-framed binary. The header is 13 bytes:

- `magic = CARN`.
- `format_version`, currently used as compatibility gate.
- `kind`.
- `key_schema`.
- `codec`, currently only `0` for no compression.
- `block_size`.

The payload is a sequence of CRC-protected blocks. Each block stores sorted records as key/payload
pairs. A sparse footer index stores block offsets and key ranges so `RunFileReader::seek` can skip
blocks. The footer is also CRC-protected.

**Kinds currently modeled:** `BlobDelta`, `SourceEdge`, `ManifestEntries`, and `TargetShardDelta`.
Current live producers use `ManifestEntries` and source/blob-target style GC runs; some kind values
look ahead of current call sites.

**Producer:** any caller of `RunFileWriter`. Important call sites are `CasManifestCodec` for embedded
manifest entries, `CasBlobInDegree` for blob-target/source-edge runs, and GC cleanup bundling for
part-manifest cleanup runs.

**Consumers:** `RunFileReader`, `RunMerger`, GC in-degree merge, cleanup code, and `fsck`.

**Purpose:** compact deterministic data-plane format for large sorted streams. It is justified where
bounded-memory merge, ranged `seek`, and replay/adoption of write-once run objects matter. It is much
less compelling for small embedded sections that are always materialized whole.

**Versioning and integrity:** decode checks magic, compatibility version, codec, block bounds, block
CRC, and footer CRC. The block CRCs are currently necessary because external `RunRef` checksums are
not verified.

**Notes:**

- The reader exposes `kind` and `key_schema`, but callers do not consistently validate expected
  values before interpreting records.
- Unknown `RunKind` should fail closed in the reader or in typed open helpers.
- Record-bound checks after loading a CRC-valid block should be tightened so a malformed but
  self-consistent block cannot yield partial records.
- Unused `RunKind` values should be removed or clearly reserved outside the active runtime enum.
- `format_version` has the same naming problem as `PartManifest`.

## GC State `CAGT` {#gc-state-cagt}

**Storage path:** `<prefix>/gc/state`, produced by `Layout::gcStateKey`.

**Body:** `GcStateProto` protobuf with `CasHeader` magic `CAGT`. It stores:

- `round`: highest GC round with durable retired sets.
- `fence_seq`: leadership epoch counter.
- `snap_shards`: GC shard count.
- `snap_generation`: authoritative generation.
- `snap_pruned_through`: retention cursor.
- `lease`: GC leader owner and sequence.
- `snap_attempt`: adopted attempt id for the current generation.
- `manifest_sweep_cursor`: best-effort orphan manifest sweep cursor.
- `retired_refs`: sorted mapping from GC shard to current retired-set object key.

**Producer:** GC lease acquisition, renewal, stealing, round commit, rebuild, and sweep cursor updates
through `encodeGcState`.

**Consumers:** GC itself, `Store::loadPublishedGcRound`, `RetireView`, writer publish gate, orphan
sweep, and `fsck`.

**Purpose:** global coordination point for GC. It says which generation and retired-set refs are
current, and who currently owns the GC lease.

**Versioning and integrity:** protobuf with `CasHeader`. `retired_refs` is a sorted repeated field,
not a proto map, to keep deterministic byte order.

**Notes:**

- `retired_refs` is documented as additive, but it is semantically load-bearing for the ack-floor
  writer gate. If an old reader can ignore it and make a wrong decision, it needs a
  `compatibility_version` gate or a documented mixed-version rollout rule.
- Comments in `CasGcFormats.h` still describe an older JSON/CAGS design and should be replaced.

## GC Heartbeat Fixed Record {#gc-heartbeat-fixed-record}

**Storage path:** `<prefix>/gc/hb`, produced by `Layout::gcHbKey`.

**Body:** exactly 24 bytes:

- 16-byte big-endian GC owner id.
- 8-byte big-endian heartbeat sequence.

There is no magic and no version field.

**Producer:** GC heartbeat path in `CasGc.cpp` increments and writes it while a leader is alive.

**Consumers:** GC lease-steal path reads it to avoid stealing from a slow but live leader whose
`GcState.lease.seq` may not advance during a long round.

**Purpose:** advisory liveness pulse independent of round progress.

**Versioning and integrity:** length is the only structural guard. There is no `CasHeader`, no magic,
and no future-object gate.

**Notes:**

- This is the strongest convention exception in the current set.
- It is acceptable only if treated as disposable advisory state. If it affects cross-version safety,
  convert it to protobuf with `CasHeader`.
- Do not add more fixed unversioned control records.

## Fold Seal `cas_fold_seal` {#fold-seal-cafs}

**Storage path:** `<prefix>/gc/gen/<gen>/attempt/<attempt>/fold_seal`, produced by
`Layout::foldSealKey`.

**Body:** strict raw text control object. After the versioned `cas_fold_seal` header and the
generation/parent-generation meta line, records appear in fixed tagged order:

- `rfl`: one opaque-life-keyed `RefLifeFoldState` for every catalog-admitted `Live` or `Removing`
  life, containing coverage, an optional hold, and optional terminal cleanup evidence.
- `btr`: sorted blob-target `RunRef` records with checksum, shard, and physical generation.
- `cnd`: shard-sorted condemned-row summaries used by graduation and pure carry.

The object ends with a record-count trailer. There is no name-keyed ref-shard row and no persisted
`part_manifest_cleanup` run: manifest cleanup executes from the round's in-memory cleanup map.
Terminal cleanup evidence in an `rfl` row participates in the proof for later exact catalog-row
removal; the perpetual namespace janitor independently reclaims the dead life's stream, checkpoint,
and namespace-file bytes.

**Producer:** GC fold creates it through `encodeFoldSeal` and stores it with
`putDeterministicArtifact`, so replay of the same attempt must reproduce identical bytes.

**Consumers:** later GC rounds, rebuild baseline, orphan manifest sweep, and `fsck` use the ref-life,
blob-target-run, and condemned-summary records.

**Purpose:** write-once coverage record for a generation. It is the durable link from a generation to
its catalog-admitted ref-life state and blob-target runs.

**Versioning and integrity:** strict tagged text with a generation-gated format header, line and object
byte caps, deterministic ordering, duplicate-key rejection, and pinned raw storage.

**Notes:**

- The coverage classification set `{0, 1, 2, 4}` is closed and validated before narrowing to its byte.
- A classification-4 `rfl` record requires a canonical hold; other classifications forbid one.
- Every `btr` row carries the whole-object checksum that readers verify before acting on the run.

## Run Reference Subformat {#run-reference-subformat}

`RunRef` is not a standalone object. Each value is encoded as one `btr` row inside `cas_fold_seal`.

**Fields:**

- `key`: object key of a write-once `RunFile`.
- `checksum`: the whole-object chained CityHash128 stored in the row's `ck` field.
- `shard`: GC shard, required for `blob_target_runs`.
- `generation`: physical generation namespace that holds the run.

**Producer:** `CasBlobInDegree` creates `RunRef` values when it writes source-edge runs. Parent-carry
code may copy existing refs verbatim into a new seal.

**Consumers:** GC and `fsck` resolve runs through the key, shard, and generation fields. GC streams
each run and verifies its whole-object checksum before acting on its rows.

**Purpose:** decouple the logical generation from the physical key namespace. With parent reference
carry, a generation can refer to a run physically stored under an older generation.

## Blob Target Run `CARN` {#blob-target-run-carn}

**Storage path:** `<prefix>/gc/gen/<gen>/attempt/<attempt>/blob_target/<shard>/<seq>`.

**Body:** `RunFile`, normally keyed for blob/source-edge data.

**Producer:** `CasBlobInDegree` fold and rebuild paths.

**Consumers:** GC in-degree merge, next-generation fold, rebuild, and `fsck`.

**Purpose:** persist large sorted blob in-degree data without keeping all edges resident.

**Notes:**

- This is the strongest justification for `RunFile`.
- Current `gc_shards > 1` support is target-sharded data layout plus a local reducer loop. One GC
  leader partitions `BlobDelta` rows with `blobShard`, then invokes `ShardReducer` for each shard
  inside the same `Gc::fold` call. There is no production scheduler that assigns reducer shards to
  multiple replicas or workers.
- Consumers should open it through a typed helper that verifies expected `RunKind` and `key_schema`.

## Historical Part Manifest Cleanup Run `CARN` {#part-manifest-cleanup-run-carn}

This subsection records a removed design and is not part of the current persisted-format inventory.
Older trees stored cleanup bundles at
`<prefix>/gc/gen/<gen>/attempt/<attempt>/part_manifest_cleanup/<owner_shard>/<seq>`.

**Historical body:** `RunFile` rows containing manifest object keys and tokens.

**Historical producer and consumer:** the GC cleanup bundling path wrote these rows and sealed their
references in `FoldSealProto.part_manifest_cleanup`. The audited implementation had no runtime reader
for the bundles.

**Current behavior:** `cas_fold_seal` has no cleanup-run field. Post-CAS manifest deletion uses the
in-memory `folded.mf_cleanup` map, and the orphan sweep scans `cas/manifests/` directly. Namespace-life
bytes are a separate concern owned by the perpetual namespace janitor.

**Historical purpose:** keep manifest cleanup work deterministic and bounded while avoiding a large
array in the fold seal. The design came from the sharded GC plan where cleanup
workers owned disjoint `ManifestId` ranges/namespaces and contributed bundle refs into
`CasFoldSeal.part_manifest_cleanup`.

**Notes:**

- The removed names above are retained only to explain older artifacts and documentation.
- No current producer, consumer, layout key, or fold-seal field implements this design.

## Retired Set `CART` {#retired-set-cart}

**Storage path:** `<prefix>/gc/gen/<gen>/attempt/<attempt>/retired/<round>/<shard>`, produced by
`Layout::retiredKey`.

**Body:** `RetiredSetProto` protobuf with `CasHeader` magic `CART`. Each `RetiredEntryProto` stores:

- `kind`: object kind, currently blob.
- `hash`: raw 16-byte object hash.
- `token_value` and `token_type`: exact incarnation token.
- `size`.
- `condemn_round`.
- `delete_pending`.

**Producer:** GC publishes retired sets with `encodeRetiredSet`. Rebuild can also install retired
sets for the adopted generation/attempt.

**Consumers:** writer publish gate through `RetireView`, GC delete/graduation code, `fsck`, and
observability paths.

**Purpose:** list object incarnations that writers must treat as condemned. Exact tokens make
deletion safe: GC deletes only the incarnation it observed.

**Versioning and determinism:** protobuf with `CasHeader`. Entries are intended to be sorted
deterministically.

**Notes:**

- `kind` and `token_type` should be validated as enum domains.
- `ObjectKind::Tree` comments in proto are stale because tree objects were removed.

## Outcome Log `CAGO` {#outcome-log-cago}

**Storage path:** `<prefix>/gc/gen/<gen>/attempt/<attempt>/outcomes/<round>/<shard>`, produced by
`Layout::outcomesKey`.

**Body:** `GcOutcomeLogProto` protobuf with `CasHeader` magic `CAGO`. Each entry stores object kind,
hash, token, token type, and outcome.

**Producer:** GC delete/recheck path writes outcome logs with `encodeOutcomeLog`. Replays read an
existing object and verify/adopt it instead of blindly overwriting.

**Consumers:** GC replay/adoption path and observability. The code currently has decode support via
`decodeOutcomeLog`.

**Purpose:** make destructive GC decisions idempotent and auditable.

**Versioning and integrity:** protobuf with `CasHeader`; entries preserve insertion order.

**Notes:**

- `CasGcOutcomes.h` still describes an older strict JSON body. The implementation is protobuf.
- `kind`, `token_type`, and `outcome` should be validated as enum domains.

## Owner Anchor `CAOW` {#owner-anchor-caow}

**Storage path:** `<prefix>/gc/server-roots/<srid>/owner`, produced by `Layout::ownerKey`.

**Body:** `OwnerProto` protobuf with `CasHeader` magic `CAOW`. It stores `server_uuid`.

**Producer:** `claimOwnerOrThrow`. It writes the object once when the subtree is empty, or validates
that an existing owner matches the current server UUID.

**Consumers:** mount/open path.

**Purpose:** sticky identity binding for a configured server root. It prevents silently reusing an
existing subtree under a different server UUID.

**Versioning and integrity:** protobuf with `CasHeader`; decode validates the raw UUID size.

**Notes:** necessary and consistent with the current metadata convention.

## Server Epoch `CAEP` {#server-epoch-caep}

**Storage path:** `<prefix>/gc/server-roots/<srid>/epoch`, produced by `Layout::epochKey`.

**Body:** `ServerEpochProto` protobuf with `CasHeader` magic `CAEP`. It stores
`next_writer_epoch`.

**Producer:** `allocateWriterEpoch` reads the current value, CAS-writes `next + 1`, and returns the
allocated writer epoch.

**Consumers:** mount/open path and writer fencing.

**Purpose:** durable monotone epoch allocation. A fenced or superseded writer cannot later reuse the
same epoch after reopen.

**Versioning and integrity:** protobuf with `CasHeader`.

**Notes:** necessary and consistent.

## Mount Lease `CAML` {#mount-lease-caml}

**Storage path:** `<prefix>/gc/server-roots/<srid>/mount`, produced by `Layout::mountKey`.

**Body:** `MountLeaseProto` protobuf with `CasHeader` magic `CAML`. It stores:

- `server_uuid`.
- `writer_epoch`.
- `hostname`, `pid`, and `started_at_ms`.
- `seq`.
- `expires_at_ms`.
- `min_active`.
- `observed_gc_round`.
- `gc_fenced`.

**Producer:** `claimMount`, `MountLeaseKeeper`, termination path, and GC fence-out path.

**Consumers:** local write fence, mount open path, GC heartbeat floor, orphan sweep, and GC
fence-out.

**Purpose:** live lease for one writer incarnation plus the ack-floor fields GC needs. It is liveness
state, not identity; identity belongs to `OwnerProto`.

**Versioning and integrity:** protobuf with `CasHeader`.

**Notes:**

- This format usefully replaced separate watermark/heartbeat state.
- `cas_format.proto` top magic table should include `MountLeaseProto`; today the table is incomplete.

## Namespace Verbatim Files {#namespace-verbatim-files}

**Storage path:** `<prefix>/roots/<ns>/_files/<relative-path>`, produced by
`Layout::namespaceFileKey`.

**Body:** raw bytes. These are not CAS self-describing objects.

**Producer:** writer/ref-payload paths for mutable or verbatim files.

**Consumers:** read path for files that are not part of immutable manifest/blob content.

**Purpose:** store namespace-scoped files whose bytes must be preserved by path rather than by content
hash.

**Versioning and integrity:** no CAS header. The format is the file's own external format.

**Notes:** this is acceptable as a raw passthrough family because CAS is not interpreting the body.
The path validator is the important safety boundary.

## Mountpoint Objects {#mountpoint-objects}

**Storage path:** `<prefix>/roots/<relative-path>`, produced by `Layout::mountpointObjectKey`.

**Body:** raw bytes.

**Producer:** loose-file mountpoint path.

**Consumers:** read path for mirrored loose files.

**Purpose:** store non-content-addressed files at their mirrored ClickHouse path.

**Versioning and integrity:** no CAS header because CAS treats the bytes as passthrough content.

**Notes:** keep this separate from CAS metadata formats. Do not store CAS control state in this raw
family.

## Token Subformat {#token-subformat}

`Token` is not a standalone object, but it is a persistent subformat in retired sets, outcome logs,
fold coverage, and backend metadata.

**Fields in persisted protobuf objects:** token value string plus token type. Known token types are
ETag, generation, and emulated token.

**Purpose:** exact-token delete and overwrite safety. CAS must delete only the object incarnation it
observed.

**Notes:**

- Every persisted token type field should validate its enum domain.
- Token values should remain opaque strings. Code should not infer provider semantics from the string
shape once `token_type` is present.

## Global Versioning Model {#global-versioning-model}

The intended convention is:

- `writer_version`: forensic producer generation.
- `compatibility_version`: read gate; readers fail closed when it is greater than `G_BUILD`.
- `format_version`: local codec layout version only, if a codec truly needs one.

Protobuf metadata follows this through `CasHeader`. `CasEnvelope` follows the same rule with binary
slots. `PartManifest` and `RunFile` are less clean because their `format_version` fields are passed
to `checkCompatibility`, so they act as `compatibility_version`.

`changePoints` and `FormatChangePoint` exist as future migration machinery, but today every format
returns the generation-1 baseline. That is not harmful, but it should either gain tests with the
first real generation bump or be removed until needed.

## General Observations {#general-observations}

The design is directionally consistent:

- Mutable/control metadata is protobuf with `CasHeader`.
- Hashed payload bytes are binary and streamable.
- Large GC data-plane streams use `RunFile`.
- Raw passthrough files are kept out of CAS metadata decoding.

The current inconsistencies are concentrated:

- Some comments still describe JSON, CAGS, trees, or removed snap/watermark formats.
- `cas_format.proto` has an incomplete magic table.
- `GcHeartbeat` is unversioned control state.
- `RunFile` and `PartManifest` use `format_version` as a compatibility gate.
- `PartManifest.payload_digest` is persisted but not consistently verified.
- Several enum fields are decoded through casts and need explicit domain validation.
- Some placeholder ids, kinds, and fields look ahead of current call sites.
- `gc_shards > 1` is supported as sharded data layout and local per-shard reducer execution, not as a
  distributed map-reduce-like GC. `CasGcScheduler` runs one `Gc::runRegularRound` behind one GC
  lease; it does not distribute reducer ownership across replicas.

## Standardization Proposals {#standardization-proposals}

### Make The Format Contract Explicit {#make-the-format-contract-explicit}

Every persisted CAS object should document:

- Storage key and owner.
- Body family.
- Magic or explicit reason for no magic.
- Producer and consumers.
- Determinism requirement.
- Versioning rule.
- Integrity rule.
- Decoder invariants.
- Test coverage.

If a new object cannot fill this checklist, it should stay in memory or be folded into an existing
format.

### Keep One Proto File {#keep-one-proto-file}

Keep `cas_format.proto` as the single normative protobuf file. The proto set is not excessive; it is
one storage protocol with shared headers and shared subformats. Splitting it would make the protocol
harder to audit.

Cleanups:

- Complete the magic table with `OwnerProto`, `ServerEpochProto`, and `MountLeaseProto`.
- Remove stale references to JSON, tree objects, and old snap/watermark formats except in explicit
  historical notes.
- Reserve removed fields and names in proto when appropriate; do not keep removed runtime concepts
  alive in examples.

### Standardize Binary Version Fields {#standardize-binary-version-fields}

For `RunFile` and `PartManifest`, choose one:

- Rename the on-disk field to `compatibility_version` on the next incompatible binary-format break.
- Or add a separate local `format_version` and keep `compatibility_version` for `G_BUILD` gating.

Do not keep a field named `format_version` if the reader treats it as global compatibility.

### Decide The CRC And Checksum Boundary {#decide-the-crc-and-checksum-boundary}

Do not remove `RunFile` block/footer CRC32C yet. It is currently the only integrity check applied
near ranged-read use, and `RunRef` checksums are not verified.

Standard boundary:

- CRC32C: local block/footer corruption guard for `RunFile`.
- Content hash: blob payload identity.
- Header hash: `CasEnvelope` header corruption guard.
- Artifact hash: full write-once object adoption check, if retained.

Then decide `RunRef`:

- Either verify `RunRef.checksum` as a full-run artifact hash before trusting a run.
- Or remove/demote it before release.

Also decide `PartManifest.payload_digest`:

- Verify it during manifest read.
- Or remove/demote it before release.

### Make `CARN` Record Streams Typed At Open {#make-runfile-typed-at-open}

Add typed reader helpers or expected-kind parameters:

- `openManifestEntriesRun`.
- `openBlobTargetRun`.
- `openPartManifestCleanupRun`.

These helpers should validate `RunKind`, `key_schema`, `codec`, and block-size invariants before
callers interpret record payloads. In docs and new APIs, prefer `CARN` record stream or block-framed
record format over jargon-heavy or command-like names such as `sorted run` or `RunBinary`.

### Convert Or Document `GcHeartbeat` {#convert-or-document-gcheartbeat}

Preferred: convert `GcHeartbeat` to protobuf with `CasHeader` if it participates in any
cross-version safety decision.

Acceptable alternative: explicitly document it as disposable advisory state, keep exact length
validation, and state that a future incompatible reader may ignore or recreate it.

### Tighten Decoder Strictness {#tighten-decoder-strictness}

All decoders should follow the same error split:

- Bad magic, truncation, duplicate key, wrong owner, invalid enum, invalid length:
  `CORRUPTED_DATA`.
- Future `compatibility_version` or unknown critical extension: `UNKNOWN_FORMAT_VERSION`.

Concrete checks to add:

- Validate `CasEnvelope.hash_algo`.
- Validate `RunKind` and `key_schema`.
- Validate enum fields in retired entries, outcome entries, and fold coverage.
- Check `RunFileReader::next` record bounds after block installation.
- Check `RootShardManifest.refs` map key against embedded `ref_name`.

### Remove YAGNI Placeholders {#remove-yagni-placeholders}

Remove or demote these unless a current producer and consumer exists:

- `FormatId::Roster`.
- Unused `RunKind` values.
- `TreeId` if no live code still needs it as a type boundary.
- `changePoints` until the first real format generation test, or keep it but add a migration test.
- Distributed sharded-GC scaffolding comments/classes such as cleanup-worker ownership language,
  `CoordinatorPlan`, and `manifestCleanupShard` unless the current code grows an actual reducer
  scheduler and replay path.
- Stale comments for removed `Tree`, `GcSnap`, JSON, watermark, and completion-seal formats.

### Migration Order {#migration-order}

Recommended sequence:

1. Fix comments and magic tables.
2. Add enum, kind, schema, and `hash_algo` validation.
3. Add missing `RunFile` record-bound tests and checks.
4. Decide and implement verification or removal for `RunRef.checksum` and `payload_digest`.
5. Decide `GcHeartbeat`.
6. Standardize binary version field names on the next format break.
7. Remove unused placeholders.

This preserves the current architecture while making the protocol easier to reason about and harder
to misread.
