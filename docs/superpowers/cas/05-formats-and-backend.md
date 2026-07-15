---
description: 'Object kinds, the one-header envelope format, codecs, deterministic-artifact upload, layout keys, the Cas::Backend abstraction with exact-token deletes, the rustfs testbed, AWS/GCS/Azure support status, and schema-evolution stance for the CAS MergeTree feature.'
sidebar_label: 'Formats and backend'
sidebar_position: 5
slug: /superpowers/cas/formats-and-backend
title: 'CAS MergeTree — Formats and Backend'
doc_type: 'reference'
---

# CAS MergeTree — Formats and Backend {#title}

**Status:** covers generation-1 decisions: the envelope format (`DONE`, blob only — see below), the
encoding taxonomy and proto rename (`DONE`), `putDeterministicArtifact` (`DONE`, GC-internal artifacts
only), the `Backend` seam (`DONE`), layout keys (`DONE`); the standalone `Tree` object kind, its
inline-data section, and the Merkle `treeId` rule (**removed 2026-07-03** — superseded by
the rev. 15 `PartManifest` redesign, then excised from the code entirely), schema-evolution rollout
machinery (`TODO`/deferred — Part IV); GCS/Azure binding (`TODO`); Merkle `treeId` tree-layer
intermediate object (`REJECTED`).

Sources: `specs/2026-06-07-ca-merkle-store-design.md`, `specs/2026-06-08-ca-merkle-store-requirements.md`,
`plans/2026-06-24-cas-2b-envelope-one-header.md`, `plans/2026-06-24-cas-2a-merkle-tree-id.md`,
`specs/2026-06-26-cas-proto-rename-design.md`, `specs/2026-06-24-cas-schema-evolution-framework-design.md`,
`specs/2026-06-15-ca-rustfs-overwrite-leak-mitigation-design.md`,
`specs/2026-06-15-ca-head-after-put-etag-design.md`, `specs/2026-06-19-ca-vfs-contract.md`.
Grounded against `Core/CasEnvelope.{h,cpp}`, `Core/CasBackend.h`, `Core/CasLayout.h`,
`Core/CasToken.h`, `Core/CasObjectStorageBackend.{h,cpp}`.

---

## Object kinds {#object-kinds}

**Status: DONE, but see removed-Tree note below**

Three object kinds were designed; a fourth (`Pack`) was removed (see Rejected section). **As of the
rev. 15 root-local part-manifest redesign, `Tree` was no longer produced on the write path**, and it
was excised from the code entirely on 2026-07-03: `CasManifestCodec.h` states explicitly "No
`Subtree`: there are no nested tree objects in this design; a directory is a path prefix, not a
placement." Part contents are described by a `PartManifest` (protobuf; see `Core/CasManifestCodec.h`)
whose entries are either `EntryPlacement::Inline` (bytes embedded in the manifest body) or
`EntryPlacement::Blob` (content-addressed blob). `ObjectKind::Tree` and the `CATR` magic are **removed
2026-07-03** from `CasEnvelope.h`/`CasFormat.h` — `ObjectKind` now has `Blob` as its sole enumerator.
`Cas::Layout` correspondingly has no `treeKey` builder; `objectKey` only ever addresses `Blob` (the
`ObjectKind::Tree` LOGICAL_ERROR thrower it used to carry was removed along with the enumerator).

| Kind | Mutable? | Content-addressed? | Encoding |
|------|----------|--------------------|----------|
| **Blob** | no | yes | 256-B header (`CABL`) + raw file bytes |
| **Tree** (removed 2026-07-03 — see above) | no | yes | 256-B header (`CATR`) + catalog + inline-data section |
| **Part manifest** (`PartManifest`, part of a root-shard manifest body) | no | no (referenced by ref, not by content hash) | protobuf — `clickhouse.cas.format` package |
| **Root shard** (manifest/journal) | yes | no | protobuf — `clickhouse.cas.format` package |
| **GC/control objects** (`gc/state`, mount/heartbeat, `pool-meta`, retired list, etc.) | yes | no | protobuf |

The decisive split: **content-addressed ⇒ binary; mutable ⇒ protobuf.** JSON was used for small
control objects in earlier iterations and is now fully abandoned (`specs/2026-06-24-cas-schema-evolution-framework-design.md`
§ "JSON abandoned"). The JSON codec family (`JsonObjectWriter`, `require*`, `parseJsonDocument`,
`tolerateUnknownKeys`) is deleted; every mutable object uses the protobuf path.

Rationale for binary on the hashed path: blobs are raw file bytes (cannot be protobuf's 2 GB
limit or overhead); trees are the hot read path and embed raw inline bytes; canonical serialization
is required for reproducible identity. Protobuf's skip-unknown is the additive-evolution engine on
the mutable side, where identity-stability is not required.

---

## Encoding taxonomy and the proto rename {#encoding-taxonomy}

**Status: DONE**

One normative proto file: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto`,
package `clickhouse.cas.format` (renamed from `cas_root_shard.proto` / `DB.Cas.Proto` — see
`specs/2026-06-26-cas-proto-rename-design.md`). The rename is behavior-preserving: protobuf wire
format is independent of package/message names (field numbers + wire types only). C++ consumers
alias `namespace Proto = ::clickhouse::cas::format;` to leave call sites unchanged.

The CMake target name `clickhouse_cas_proto` is unchanged.

Every object in the pool carries a self-describing header with three invariants:

- **magic** — 4-byte ASCII type tag (`CABL` blob, `CARS` root-shard, `CAPM` pool-meta,
  `CAGT` gc-state, `CAHB` heartbeat/mount, `CAGO` gc-outcomes, `CAMT` per-hash
  freshness meta — see [§per-hash freshness meta](#per-hash-meta)). Sanity check only — no CRC;
  integrity comes from S3 ETag and content-addressing for hashed objects.
  (`CATR` (tree) was removed 2026-07-03 — see the removed-`Tree` note above. The standalone `CAWM`
  watermark object was removed when the build watermark merged into the mount-lease heartbeat — see
  `03 §merged-heartbeat`; its `min_active` field now lives in the `CAHB` mount body — the `observed_gc_round`
  ack field was later removed in v3, `cas_format.proto` reserved 10.)
- **`writer_version`** — forensic: which build wrote this object.
- **`compatibility_version`** (`min_reader_version` in earlier plans) — functional write-down-to-floor:
  a reader must fail-closed (`UNKNOWN_FORMAT_VERSION`) if `compatibility_version > G_BUILD`.

For **hashed objects** (blob/tree) the trio lives in the binary envelope header described below.
For **mutable objects** a `CasHeader` protobuf message (field 1 of each object message) carries the
same three things with no binary prefix, making the objects fully `protoc`-decodable.

---

## The one-header envelope format {#envelope}

**Status: DONE** (`plans/2026-06-24-cas-2b-envelope-one-header.md`)

Every blob and tree object begins with the same **256-byte fixed-length header** (`blob_header_len = 256`),
padded uniformly for both kinds. A constant payload offset means every blob in the pool starts at
the same offset — a constant shift to locate content, no per-object header parse on the read path.

### Binary core layout (94 bytes, hole-free) {#envelope-core}

```
offset  size  field
[0,4)   4     magic            "CABL" (blob) or "CATR" (tree)   — encodes kind; no separate kind byte
[4,6)   2     writer_version   u16 LE
[6,8)   2     compatibility_version  u16 LE  (formerly min_reader_version)
[8,9)   1     hash_algo        u8 (1 = cityHash128)
[9,10)  1     flags            u8 (FLAG_HAS_CRITICAL_EXTENSION = 0x01)
[10,14) 4     header_len       u32 LE
[14,22) 8     logical_size     u64 LE
[22,38) 16    logical_hash     u128 LE
[38,54) 16    domain_id        u128 LE
[54,70) 16    incarnation_tag  u128 LE
[70,86) 16    build_id         u128 LE
[86,94) 8     header_hash      u64 LE  (CityHash64 over [0,94) with this field zeroed)
```

`CORE_HEADER_LEN = 94`, `HEADER_HASH_OFFSET = 86`. The core is **never grown**: new fields go into
the `[94, header_len)` TLV extension area. `header_len` is padded to an 8-byte multiple;
`pad_to_header_len = 256` for every object in the pool (set by `CasBuild::uploadFromSource`).

The **incarnation zone** (`domain_id`, `incarnation_tag`, `build_id`) is **excluded from
`logical_hash`**: the header may differ between incarnations of the same logical object, and a test
asserts that `logical_hash` is unchanged when the incarnation zone varies. `logical_hash` is
`cityHash128` of the **payload** for blobs, or the Merkle tree id for trees (see §5 below).

Decode validation: bad magic → `CORRUPTED_DATA`; wrong kind for the expected magic → `CORRUPTED_DATA`;
`compatibility_version > G_BUILD` → `UNKNOWN_FORMAT_VERSION`; `header_hash` mismatch → `CORRUPTED_DATA`.
An unknown non-critical TLV is skipped; an unknown critical TLV → `UNKNOWN_FORMAT_VERSION`.

The prior format had a `CHCA` magic with a separate `kind` byte and a zero-pad word at `[12,16)`.
Both were reclaimed by shifting fields left to form the 94-byte hole-free core. The old `FORMAT_VERSION`
byte is replaced by the `writer_version`/`compatibility_version` pair.

Code: `Core/CasEnvelope.h`, `Core/CasEnvelope.cpp`.

### Why the incarnation tag lives in the body, not provider metadata {#incarnation-in-body}

The `incarnation_tag` (and the rest of the incarnation zone) is carried **in the object body**, not
in provider metadata, and the choice is not close for a CAS meant to run across object stores
(source `incarnation-tagged-cas.md`). On S3, user-defined metadata is **write-once per upload** and
changing it **requires copying the object**; S3 documents an **8 KB PUT-header cap** and that the
**ETag reflects the object contents, not its metadata**. A body-carried incarnation tag therefore
gives a backend-independent way to force a new physical incarnation — a genuinely different backend
token (ETag) — while keeping the logical content address stable. Putting the tag in metadata would
change no bytes, would not reliably change the S3 overwrite/ETag token, would require a copy to
modify, and would be subject to provider header limits. GCS `generation`/`metageneration` and Azure
ETag concurrency are the per-provider alternatives; the body-carried tag is the portable primitive
that works uniformly. The incarnation zone is excluded from `logical_hash` (see above) precisely so
that minting a fresh tag re-incarnates the physical object without rekeying it.

---

## Codecs and determinism {#codecs-and-determinism}

**Status: DONE**

### `putDeterministicArtifact` — byte-reproducible sealed artifacts {#deterministic-artifact}

A blob or tree produced by a given build is **deterministic**: same logical content → identical
payload bytes → identical `logical_hash` key. This is the core dedup property. Determinism
requirements:

- **Blobs** — raw file bytes; the hash is the pool-wide streaming content-hash convention: chunked
  `CityHash128` chained via `HashingWriteBuffer`/`HashingReadBuffer` per `DBMS_DEFAULT_HASHING_BLOCK_SIZE`
  = 2048-byte blocks (`Core/CasBuild.cpp`, `poolContentHash`), hex-formatted via `getHexUIntLowercase` —
  **not** a one-shot `CityHash128` of the whole payload; a one-shot hash diverges from the streaming hash
  for any payload larger than one hash block (this caused a live false `CORRUPTED_DATA` in the 2026-07-03
  soak). Identity is independent of the header incarnation zone.
- **Trees** — the catalog is sorted by entry name (byte-wise) and uses a fixed field layout with no
  nondeterministic fields (`mtime`, `uid`, and any host-dependent attribute are excluded). Two trees
  with the same logical content therefore hash identically, enabling recursive directory dedup.
- **Canonical serialization** — same rule as Git tree objects: entries sorted by name, fixed field
  order, explicit `kind` byte for domain separation, no optional fields in the identity-sensitive
  input.

Blob upload (`Build::putBlob`, `Core/CasBuild.cpp`) derives the content hash **before** serializing/
uploading the payload (the caller supplies the already-hashed `BlobId`) and uses it as the object key.
On `putIfAbsent`/`putIfAbsentStream` the object-storage backend returns `PreconditionFailed` if the key
already exists — a dedup hit — and no bytes are transferred. This is why the write path is
`O(new-content)`, not `O(all-content)`.

`putDeterministicArtifact(Backend&, key, bytes)` (`Core/CasBlobInDegree.cpp`) is a distinct, narrower
helper: it is used only for **GC-internal** deterministic artifacts (fold seals, run files) where a
replay is expected to reproduce byte-identical output. It calls `putIfAbsent`, and on
`PreconditionFailed` re-`get`s the existing bytes and throws `CORRUPTED_DATA` if they differ from what
was about to be written (divergent bytes at a supposedly-deterministic key); a byte-equal collision is
treated as an idempotent no-op adoption. It is not part of the blob/tree content-addressed write path.

**Blob-hash-over-payload domain:** the blob hash covers only `[header_len, EOF)` — the raw payload
bytes — not the header. A header TLV or incarnation-zone change never rekeys a blob. A test in the
gtest suite (`CasEnvelope.IncarnationZoneDoesNotAffectPayloadOrId`) asserts this.

### Tree codec and inline-data section {#tree-codec}

**Not implemented as of 2026-07-03:** the standalone `Tree` object kind described in this section was
superseded by the rev. 15 root-local part-manifest redesign. There is no `CasTreeCodec.{h,cpp}` in the
tree; part contents are described by `PartManifest` (`Core/CasManifestCodec.h`), whose entries carry
`EntryPlacement::Inline` or `EntryPlacement::Blob` directly — see `§object-kinds` above.

The tree payload layout is **catalog-first, inline-data-last** (not interleaved):

```
[ catalog: N × (name, kind, content_hash, size, placement, offset, length) ]
[ inline-data: concatenated bytes of Placement::Inline files ]
```

A directory listing reads only the catalog and never fetches the inline-data section. Reading one
inline file is a seek to its `(offset, length)`. `size`/`placement`/`offset`/`length` live in the
catalog but are **outside identity** and may evolve freely.

`Placement::Inline` is used for eager small part files (`checksums.txt`, `columns.txt`, `count.txt`,
`serialization.json`, `metadata_version.txt`, `partition.dat`, small `primary.idx`) — they ride the
single tree GET at part open. `Placement::Blob` is used for lazy per-column data (`.bin`, `.mrk`),
preserving column selectivity.

Code: `Core/CasTreeCodec.{h,cpp}` (not implemented as of 2026-07-03 — no such file exists; superseded by
`Core/CasManifestCodec.{h,cpp}`, see above).

---

## Per-hash freshness meta (`.meta`, `CAMT`) {#per-hash-meta}

**Status: DONE** (v3 freshness-meta, spec `2026-07-09 §meta-protocols`).

Alongside each blob body the pool may carry a small per-hash **freshness meta** object
(`Core/CasBlobMeta.{h,cpp}`), tagged with the on-disk magic `"CAMT"` (fixed-length body). It records a
2-state `MetaState` — `Clean = 0` (referenceable; body present) or `Condemned = 1` (GC marked in-degree
0; body STILL present, a writer may resurrect by CAS) — plus a `condemn_round` (an ABA guard after
spare→re-condemn) and `size`. There is **no** `Tombstone` state (the raw-body/terminal-tombstone and
per-incarnation-key variants were rejected — see the freshness-meta v3 plan).

Role: it is the **writer's freshness point-read**, replacing the old writer-side retire-view. On a
dedup/reuse hit the writer `loadMeta`s the hash and treats `Condemned` as `ABORTED` → re-upload from
source (INV-1); otherwise it adopts the existing blob. GC keeps it current (`writeCondemnedMeta` on
condemn, plus spare/delete transitions) on a bounded pool (`gc_meta_pool_size`). It is a freshness
marker only, **not** the linearization point: delete-exactness stays with the in-body incarnation tag +
exact-token BODY delete (see [§incarnation-in-body](#incarnation-in-body); `CaIncarnationCore`). Ops
surface (`CasBlobMeta.h`): `loadMeta`, `putMetaIfAbsent`, `casMeta`, `deleteMetaExact`.

---

## Merkle `treeId` identity rule {#merkle-tree-id}

**Status: not implemented as of 2026-07-03** — superseded by the rev. 15 root-local part-manifest
redesign. There is no `merkleTreeId` function and no `CasTreeCodec.{h,cpp}` in the tree; a `PartManifest`
is addressed by its journal-assigned `ManifestRef` (`writer_epoch`/`build_sequence`/`manifest_ordinal`),
not by a content hash of its entries. The description below documents a design that was implemented in
an earlier generation and is retained for historical context only.

`treeId = CityHash128( "CAMT" || u8(1) || u32(entry_count) || per-entry[(u16 name_len || name || u8 node_kind || u128 child_hash)] )`

where:
- entries are sorted by name (byte-wise); duplicate names rejected (`BAD_ARGUMENTS`).
- `node_kind`: `0` = file (Inline or Blob), `1` = subtree — RFC 6962-style domain separation (prevents a file hash and a subtree hash under the same name from colliding; Bitcoin's CVE-2012-2459 arose from its absence).
- `child_hash` = `logical_hash` for files, `treeId` of the child tree for subtrees.
- `file_size`, `placement` (Inline vs Blob), and `inline_bytes` are **excluded** — identity is
  independent of storage layout. An inline file and a standalone blob with the same content produce
  the same `treeId`.

The domain tag `"CAMT"` and rule version `u8(1)` live only inside this (historical, never-persisted)
hash input. (Note: the 4-byte tag `CAMT` has since been REUSED as a real on-disk magic for the
unrelated per-hash freshness-meta object — see [§per-hash freshness meta](#per-hash-meta); that reuse
is independent of this treeId hash-domain tag.) The rule is **frozen by convention**: changing it only loses cross-boundary dedup
(logically identical trees get different ids → stored twice — a benign duplicate, never a
correctness problem). Readers never recompute `treeId`; the writer always computes it directly from
the collected entries before serializing the catalog (`CasBuild::stageTree` uses `merkleTreeId(entries)`).

The old rule was `CityHash128(encodeTree(...))` — identity from the serialized catalog bytes. This
was fragile: any catalog encoding change would rekey all trees, breaking dedup. The Merkle rule
decouples identity from serialization entirely.

Code: `Core/CasTreeCodec.{h,cpp}` (`merkleTreeId`); `Core/CasBuild.cpp` (`stageTree`). (Not implemented
as of 2026-07-03 — see status note above; the current write path is `CasBuild::stageManifest`.)

---

## REJECTED: Merkle `treeId` tree-layer intermediate object {#rejected-tree-layer}

**Status: REJECTED** (plan `plans/2026-06-24-cas-2a-merkle-tree-id.md` was superseded by the
identity-rule change in §5 above; the intermediate tree-layer *object* was never proposed or
implemented)

The rejected design that *was* considered and discarded is a separate **tree-layer object** that
would have sat between blobs and the part-level tree — analogous to Git's "tree of trees" nesting
with explicit intermediate objects stored in the pool. The rationale for rejection:

1. **No benefit over inline subtrees.** The CAS tree codec already supports recursive subtrees
   (`Placement::Subtree` entries pointing to child tree ids). A separate `trees/` object layer adds
   key-space complexity without any protocol benefit.
2. **`treeId` decoupling suffices.** The actual requirement was to decouple identity from
   serialization so encoding changes don't rekey objects. That is achieved by the Merkle rule over
   logical inputs (§5) — no intermediate object is needed.
3. **GC complexity.** Every new object kind adds a GC edge type and a retire/delete path. The
   decision was to keep the object model to exactly two content-addressed kinds: blob and tree.

The Merkle `treeId` *rule* (§5) was implemented in an earlier generation but is **not implemented as of
2026-07-03** (superseded by the rev. 15 root-local part-manifest redesign — see `§merkle-tree-id`).
What remains REJECTED regardless is the idea of an additional intermediate tree-layer object as a
distinct `ObjectKind`.

---

## Layout keys {#layout-keys}

**Status: DONE** (`Core/CasLayout.h`)

All keys are built by `Cas::Layout` from a pool prefix. The naming follows the protocol spec §4
layout exactly.

```
<prefix>/
  blobs/<shard2>/<blobId>          content blob (immutable)
  cas/refs/<ns>/<shard_number>     root-shard manifest (Phase 1 hot/cold split)
  cas/manifests/<ns>/<writer_epoch>/<build_seq>/<ordinal>.proto   part-manifest body
  roots/<server_root_id>/          per-server mutable state mirror
      _files/<name>                verbatim namespace files
  gc/
    state                          GC lease/round state (retired-in-snapshot 2026-07-10: no retired_refs)
    hb                             advisory GC-leader heartbeat (B160)
    gen/<gen>/attempt/<attempt>/
      fold_seal                    per-gc-shard coverage + condemned_summary (retired-in-snapshot)
      blob_target/<shard>/<seq>    source-edge run; condemned state rides here as kCondemned rows
      part_manifest_cleanup/<owner_shard>/<seq>
      outcomes/<round>/<shard>     (retired/<round>/<shard> removed 2026-07-10: no separate retired-list run)
    checkpoint/<version>
    server-roots/<server_root_id>/
      owner / epoch / mount        (mount body carries min_active; observed_gc_round removed in v3)
  _pool_meta                       pool identity + format version
```

The `completion_seal` and the standalone `_watermark` object are **gone**: the ack-floor round has one CAS (no completion phase), and the build watermark merged into the `mount` body (`03 §merged-heartbeat`). Each gc-shard's `retired/<round>/<shard>` run is the **current** outstanding-candidate set for that shard — `RetiredEntry` per entry now carries `condemn_round` (the ack-floor graduation gate, GC-only) and `delete_pending` (the two-phase-graduation flag, `04 §two-phase-graduation`), both additive proto fields. `gc/state` gained `retired_refs` (gc-shard → the current run's object key) and **dropped** the per-round `fence_version` map with the fence phase. The refs and the round number are published in one CAS, so a reader that observes round K can always load the complete retired list of version K (the publish-order invariant).

**Key design choices:**

- `<shard2>` = first two hex characters of the id. `Layout::blobKey` is the canonical builder for
  content blobs. `objectKey(layout, kind, hash)` dispatches on `ObjectKind`, which has `Blob` as its
  sole enumerator — the `ObjectKind::Tree` LOGICAL_ERROR thrower and `Layout::treeKey` were removed
  2026-07-03 along with the enumerator (see `§object-kinds`).
- Namespaces are opaque strings to the core; the wiring composes e.g. `<server_id>/<table_uuid>`.
  `_files`, `_manifests`, `_precommits` are reserved segments. Numeric-only shard keys and `_files`
  cannot collide.
- Phase 1 (hot/cold split) relocated root-shard manifests from `roots/<ns>/<shard>` to
  `cas/refs/<ns>/<shard>`. GC discovery LISTs `casRefsPrefix()` — only ref shards, no verbatim
  files interleaved.
- Part-manifest bodies live under `cas/manifests/` keyed by `(writer_epoch, build_sequence,
  ordinal)`, making them enumerable by namespace without a full-pool LIST.

### CH-path → CAS-namespace mapping (the `@cas@` contract) {#path-mapping}

**Status: DONE** (`PartPathParser.{h,cpp}`, `ContentAddressedMetadataStorage.cpp`, `CasLayout.h`).
Sources: `specs/2026-06-19-ca-vfs-contract.md`, `specs/2026-06-19-ca-vfs-path-mapping-design.md`.
The rules below are grounded live in code.

**The `@cas@` archive-suffix marker.** `kCasArchiveSuffix = "@cas@"` is a **suffix on a table-dir
segment** (`…/<uuid>@cas@`), **never a standalone path segment**. It marks where the mirrored
ClickHouse path ends and the content-addressed archive begins — like a `.zip` extension in
`foo.zip/inner/file`. `@` is S3-safe and never occurs in ClickHouse uuids, part names, detached
prefixes, projection names, or column files, so it cannot collide with real path data. The
**mutability invariant** is exactly this boundary: a node is immutable **iff** it is
content-addressed (has a hash); trees/subtrees/blobs/pack-slices have a hash → immutable; namespaces,
refs, and verbatim/overlay files have none → mutable. `@cas@` is the content/verbatim boundary.

**Atomic vs non-Atomic mirroring** (`mirroredArchiveNamespace`):

| Database engine | Reported path | CAS namespace |
|---|---|---|
| Atomic | bare `<uuid>` | `store/<u3>/<uuid>@cas@` — `<u3>` = first 3 chars of the uuid, matching ClickHouse's `store/` fanout |
| non-Atomic | full `data/<db>/<tbl>` | `data/<db>/<tbl>@cas@` — the `@cas@` suffix appended verbatim (no `store/<u3>` fanout) |

**`@cas@`-scoped shard classification.** Root-shard parsing under `roots/`/`cas/refs/` applies
**only inside a `@cas@` archive** (namespace's last segment ends with `@cas@`, or is the reserved
`_precommits` segment). A numeric-tailed loose file with **no `@cas@` ancestor** (e.g.
`roots/srv1/clickhouse_access_check_123`) is an **opaque ordinary file and is never mis-parsed as a
`(namespace, shard)`**. This closes the old hazard where `roots/srv1/foo/7` would be read as
namespace `srv1/foo`, shard `7`.

**Non-nesting namespace invariant.** No registered namespace may be a path-prefix of another, because
GC enumerates a namespace's shards by prefix-LIST and a nested namespace would let a parent's LIST
sweep a child's keys. Live-vs-shadow diverge at their first segment (`store/…@cas@` vs
`shadow/<backup>/store/…@cas@`) so neither is a prefix of the other. FREEZE publishes **one ref per
frozen part** under `shadow/<backup>/…/refs/<part>` (not a shared container ref) deliberately, to avoid
a shared-`detached`-style read-modify-write on a shared ref under concurrent freeze — the B66a torn-read
hazard (source: `plans/2026-06-04-cas-mergetree-freeze.md §3.2/§7d`; see also `08 §scenario-table` S18). **Detached parts (B181)** are
handled by folding them **into the table's own namespace** as refs keyed by `kDetachedRefPrefix =
"detached/"` (a `detached/PART` ref versus a live `PART` ref) — one namespace per table, no
live-vs-detached name collision (the old sibling `detachedNamespace` is gone). The `TABLE/detached`
container dir surfaces the table's refs filtered to the `detached/` prefix (stripped for display).
The non-nesting property is thus preserved via ref-name prefixing rather than a separate namespace.

**Reserved segments.** `_files`, `_manifests`, `_precommits` are reserved segments under a namespace;
numeric-only shard keys and `_files` cannot collide.

**Logical vs physical view.** `clickhouse-disks` `cd`/`ls`/`read` present the **logical** ClickHouse
view — `@cas@` stripped (`stripCasArchiveSuffix`), files reconstructed from the manifest/trees. Raw
`aws s3 ls` shows the **physical** archive — the same paths with `@cas@` on table dirs. The two
correspond segment-for-segment but are deliberately different renderings (logical vs physical), not
byte-identical listings. A raw subtree `rm roots/<server>/…` is **destructive offline maintenance,
NOT equivalent to `dropNamespace`**: it bypasses the journal and the fenced index prune and may leave
index/GC leftovers that a repair/prune step must reconcile.

---

## `Cas::Backend` abstraction {#cas-backend}

**Status: DONE** (`Core/CasBackend.h`)

`Backend` is a token-aware storage seam with 9 pure-virtual operations: `get`, `getStream`, `head`,
`putIfAbsent`, `putIfAbsentStream`, `putOverwrite`, `casPut`, `deleteExact`, `list` (plus the
capability query `supportsListTokens`). It has three concrete implementations:

- `ObjectStorageBackend` — production, wraps `IObjectStorage` (S3, GCS, Azure).
- `CasInMemoryBackend` — in-process fake for unit tests, mints monotonic tokens.
- `CasInstrumentedBackend` — wraps any backend with operation counting for test assertions.

The token contract is documented in `CasBackend.h`:

> a token must uniquely identify the byte-content of the incarnation it labels — i.e.
> `head(k).token == prior get(k).token` MUST imply the bytes are unchanged.

This is load-bearing for the `readShardDecoded` cache: a token match skips a re-`get`+decode,
so a backend whose token could repeat across different content would serve stale manifests.

### Exact-token deletes {#exact-token-deletes}

`deleteExact(key, token)` removes **only the incarnation whose token matches** and must return
`TokenMismatch` with the object untouched if the token does not match the current incarnation.
Backends that silently ignore the condition are rejected by `Cas::Probe` at pool open.

This is the safety-critical primitive of the ABA-prevention protocol: the GC captures a delete token
by `HEAD`ing the object when the three-cursor merge condemns it (`04 §three-cursor-merge`), then
issues `deleteExact` only after the entry has graduated through the ack floor to `delete_pending` and
that pending state was published by a prior round. A write that arrived after the condemn observation
carries a new token; the exact-token delete mismatches (`TokenMismatch`) and the object is left
alone.

`Cas::Backend` exposes `casPut(key, bytes, expected_token)` for the root-manifest CAS path (commit
a manifest iff the expected current token or expected absence matches — corresponding to model
`WCreate`/`WReuse`).

### `get` NoSuchKey → `std::nullopt` contract {#get-nullopt}

`ObjectStorageBackend::get` wraps its ranged read so that a concurrent-delete `S3Exception`
(`NoSuchKey` / 404) **during the GET** returns `std::nullopt` (the object vanished between HEAD and
GET = absent), matching `CasInMemoryBackend`. This removes the raw-`Code 499`-escape class for all
callers: a legitimate live read in the HEAD→GET delete window (e.g. `Store::readTree` for a SELECT
or merge) surfaces cleanly as absence rather than propagating a raw S3 error. Callers expecting
absence check for `std::nullopt`; callers expecting presence get a handled `FILE_DOESNT_EXIST`
instead of an unhandled `Code 499`. A unit test drives a backend that throws `NoSuchKey` mid-GET and
asserts `get` returns `nullopt`, not an exception. (Source
`specs/2026-06-21-ca-revival-consolidation-design.md`; note this is a read-window contract only —
revival never GETs, per INV-1 in `03-writer-protocol.md`.)

### `supportsListTokens` capability {#list-tokens}

`supportsListTokens()` returns true iff the backend surfaces per-key incarnation tokens through
`list`. When true, GC `discover` may skip reading the body of an unchanged root shard (listed token
== persisted folded token ⇒ no change). When false, GC must read every root-shard body. S3 ETags
are content-derived and returned in LIST responses → true for `ObjectStorageBackend`.

---

## Exact-token semantics: ETag vs Generation tokens {#etag-vs-generation}

**Status: ETag path DONE; GCS generation binding DEFERRED**

`CasToken.h` defines three token types:

```cpp
enum class TokenType : uint8_t
{
    ETag       = 1,  /// S3-family / Azure
    Generation = 2,  /// GCS (binding deferred; fail-closed until probed)
    Emulated   = 3,  /// test backends
};
```

**ETag (S3/Azure):** content-derived. The S3 `PutObject` and `CompleteMultipartUpload` responses
include an ETag in the response body; `WriteBufferFromS3` captures it and exposes it via
`getResultObjectETag()`. This enables eliminating the post-write HEAD that was ~73 % of all S3
HEADs (`specs/2026-06-15-ca-head-after-put-etag-design.md`): the CA backend records the
PUT-response ETag directly as the dependency token (`TokenType::ETag`) — model `WCreate` — instead
of issuing a follow-up `HEAD`.

The dedup-reuse path (`PreconditionFailed` on `putIfAbsent`) still needs an observation token; its
`HEAD` remains (model `WReuse`).

**GCS generation token:** GCS uses a per-object generation number rather than a content-derived
ETag. The generation number is NOT content-based — it increments on every overwrite, including
overwriting with the same bytes. This breaks the `Token ⟹ content precondition` that the
`readShardDecoded` cache depends on. GCS generation tokens are defined in `CasToken.h` but the
binding is deferred and fail-closed until `Cas::Probe` validates the actual semantics at pool open.

**Azure:** uses ETags; follow the same code path as S3.

**Non-content-based tokens — the protocol gap:** the CAS protocol requires that equal token ⟹
equal content (for the decode cache, not just for the delete gate). A backend whose token can repeat
across different content (e.g. a GCS-style generation number that increments on overwrite) must NOT
be used as a CAS pool without a workaround. The current design records this as a `TODO` in the
`Probe` component.

---

## AWS / GCS / Azure support status and gaps {#cloud-support}

**Status: AWS S3 DONE; Azure partial; GCS DEFERRED**

| Provider | Conditional write | Exact-token delete | LIST returns token | `supportsListTokens` | Notes |
|---|---|---|---|---|---|
| AWS S3 | `If-None-Match: *` via `WriteSettings` | `removeObjectIfTokenMatches` (ETag) | yes | `true` | Full production path |
| MinIO / RustFS | same S3 API | same | yes | `true` | Used in soak/test harness |
| Azure Blob | ETags; Azure conditional PUT | yes | yes | `true` | Code path identical to S3 |
| GCS | `x-goog-if-generation-match: 0` (dialect-mapped from `If-None-Match: *`) | generation-exact delete (dialect-mapped `If-Match`) | NO — list tokens disabled (XML LIST bodies carry MD5 ETags) | `false` on generation stores | **Validated live 2026-07-03** via `http_client = gcs_hmac` (GOOG4-HMAC + conditional dialect); versioned buckets refused by the probe; conditional writes single-PUT only (`gcs_max_conditional_put_bytes`) |

**LIST consistency requirement:** the protocol's GC `discover` step LISTs `cas/refs/` and expects
the list to be consistent: an object PUT before the LIST boundary must appear in the list. S3 and
GCS both offer strong LIST consistency (as of 2021). Azure has strong consistency for Blob Storage.
RustFS offers strong consistency in the single-node configuration used in the soak harness.

An object-storage backend with **eventual LIST consistency** (an older S3 model, or some
third-party implementations) would allow a freshly written root shard to be invisible to the GC
discovery scan — causing it to be missed in the fold and potentially retired. The protocol assumes
**strong LIST consistency** as a hard requirement. `Cas::Probe` should verify this at pool open
(tracked as `TODO`).

---

## The rustfs testbed {#rustfs}

**Status: DONE as a soak harness; overwrite-leak workaround DONE**

RustFS is used as the S3-API backend in the `utils/ca-soak` 24-hour soak harness instead of MinIO,
to expose object-storage behavior under realistic CA write patterns.

**Overwrite-leak defect (`specs/2026-06-15-ca-rustfs-overwrite-leak-mitigation-design.md`):**
RustFS `1.0.0-beta.8` does not reclaim the previous data directory on an un-versioned overwrite
(`xl.meta` is repointed, but the old `<uuid>/` data directory leaks). The CA manifest pattern
overwrites `roots/` (root-shard manifests) on every commit; ~74 GB grew in ~1 h from 18 live keys.

Mitigation: a **scope-limited orphan reaper** sidecar (`utils/ca-soak/scripts/orphan_reaper.sh`)
runs every 300 s, scoped to `roots/` only (blobs/trees are immutable and have exactly one data dir).
For each manifest object dir it keeps `xl.meta` and any `<uuid>/` dir whose mtime is within a 120 s
grace window, and the single newest dir; deletes the rest. Blobs/trees cannot be touched because
they are outside `roots/` and have one data dir per key.

This is a **test-harness workaround** for a RustFS defect. The CA server code is unchanged.
Production deployments use real S3 (or a correctly-versioned object store) and have no equivalent
issue.

---

## Schema-evolution stance {#schema-evolution}

**Status: Generation-1 freeze DONE; rollout machinery DEFERRED (pre-release)**

(`specs/2026-06-24-cas-schema-evolution-framework-design.md`, Parts I–V)

### Generation-1 freeze {#gen1-freeze}

The following byte-level decisions are frozen (irreversible after first release):

- Single header for blob (and, until removed 2026-07-03, tree); magic `CABL` (`CATR` removed along
  with `ObjectKind::Tree`); exact core size 94 bytes; `blob_header_len = 256`.
- Version field width: 2 bytes (`uint16` LE) for `writer_version` and `compatibility_version`.
- Merkle `treeId` rule: `CityHash128("CAMT" || u8(1) || sorted(name, node_kind, child_hash))`
  (not implemented as of 2026-07-03 — superseded by the rev. 15 `PartManifest` redesign; see
  `§merkle-tree-id`).
- Blob hash is the streaming chunked convention (`§codecs-and-determinism`), over payload only
  (not the header).
- Catalog-first / inline-data-last tree layout (not implemented as of 2026-07-03 — see `§tree-codec`).
- `Placement::Pack` fully removed (was never produced; no reserved slot — YAGNI).
- Format id set (`CABL`/`CARS`/… — `CATR` removed 2026-07-03).
- Single error code `UNKNOWN_FORMAT_VERSION` for future format and unknown critical TLV.

### Write-down-to-floor discipline {#write-down-to-floor}

The writer targets the pool floor (`min G_build` over members) and emits the field-set valid at
that level, so an old reader is never handed something it would silently mis-skip:

```
serialize(compatibility_version):
  if      (compatibility_version < 10) emit the <10 field-set
  else if (compatibility_version < 20) emit the <20 field-set
  else                                 emit the freshest
```

A **safe-additive change** (new protobuf field number) needs no branch: old readers skip the
unknown field. A **breaking change** (semantics of an existing field changes, or a new binary format
section) uses a branch and waits for the floor to rise. The ladder is pruned after a completed
upgrade cycle.

For trees, `treeId` was designed to be Merkle over logical children — so a breaking serialization
change would not rekey objects (not implemented as of 2026-07-03; see `§merkle-tree-id`). Old builds
refuse to parse the new tree (fail-closed); objects dedup correctly across the boundary.

### Deferred rollout machinery (Part IV) {#deferred-rollout}

The following are designed but **not yet built** (pre-release, no breaking change exists yet):

- **`max_content_addressable_pool_format` setting** — per-server cap on write generation; staged rollout.
- **Durable roster** (`pool-meta` extension): `{members: {server_id: {path, G_build}}}` — one GET
  to read all members, CAS to update own entry. Floor = `min(G_build)` over members. Write a format
  `V` iff `min_reader(V) ≤ floor AND V ≤ setting`. Collocated with `RootsRegistry`.
- **B200 — deliberate decommission** — **IMPLEMENTED 2026-07-15, without the roster** (grammar
  landed as `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '<server_id>' FROM DISK '<disk>'`; spec
  `2026-07-13-cas-pool-member-decommission-design.md`). Never auto-removes for inactivity;
  a long-absent member pins shared floors until deliberately dropped (safe default). Roster
  forward-hook stays Part IV: when the durable roster lands, decommission additionally removes
  the member's roster entry.

These are designed so the door is provably open, but nothing in frozen bytes depends on them.

### gc-snap conversion deferred {#gc-snap-defer}

`gc-snap` keeps its existing versioned-binary+zstd codec (not converted to protobuf). Reasoning:
it is GC-internal durable state (not a cross-implementation interchange format), already binary,
already versioned with `GC_SNAP_VERSION` + magic, and the conversion is the highest-risk
lowest-interchange-value change. The B165 OOM concern (not materializing the whole body for large
snaps) is addressable within the binary codec. GC-local objects (`gc-state`, `retired-set`) are
converted off JSON for cleanup but their protobuf is not an interchange contract.

---

## DONE / TODO / REJECTED / DESIRABLE summary {#status-summary}

| Item | Status | Notes |
|---|---|---|
| One 256-B header for blob (`CABL`), 94-byte hole-free core | DONE (envelope format only; `Tree`/`CATR` removed 2026-07-03 — see below) | `CasEnvelope.{h,cpp}` |
| `compatibility_version` (formerly `min_reader_version`) + `gateOnRead` | DONE | `CasFormat.{h,cpp}` |
| Blob hash over payload, not header | DONE (streaming chunked hash, not one-shot — see `§codecs-and-determinism`) | test asserts this |
| Standalone `Tree` object kind / `Layout::treeKey` | **removed 2026-07-03** | superseded by `PartManifest` (`CasManifestCodec.h`), rev. 15 redesign, then excised from the code (`ObjectKind` now has `Blob` as its sole enumerator) |
| Merkle `treeId` rule (2a) | **not implemented as of 2026-07-03** | no `merkleTreeId`/`CasTreeCodec` in tree; superseded by rev. 15 |
| Catalog-first / inline-data-last tree layout | **not implemented as of 2026-07-03** | no `CasTreeCodec`; superseded by rev. 15 |
| `Placement::Pack` removed | DONE | no code, no on-disk data |
| proto rename → `cas_format.proto`, `clickhouse.cas.format` | DONE | `CasObjectStorageBackend` |
| JSON codec family deleted | DONE | `CasCodecUtil.h` |
| PUT-response ETag capture (no post-write HEAD on S3) | DONE | `WriteBufferFromS3`, `CasObjectStorageBackend` |
| `Backend` seam + exact-token `deleteExact` | DONE | `CasBackend.h` |
| `Cas::Layout` key builders | DONE | `CasLayout.h` |
| AWS S3 full support | DONE | production path |
| rustfs soak harness + orphan-reaper workaround | DONE | `utils/ca-soak/scripts/` |
| GCS generation-token binding | **DONE 2026-07-03** | `http_client = gcs_hmac`: GOOG4-HMAC signer + wire-boundary conditional dialect (response `ETag := generation`); full live validation cycle green (spec `2026-07-03-cas-gcs-generation-binding-design`) |
| LIST consistency probe in `Cas::Probe` | TODO | see §10 |
| Durable roster + `max_content_addressable_pool_format` setting | DEFERRED | Part IV, pre-release |
| gc-snap → protobuf | DEFERRED | lowest-priority; keep binary+zstd |
| B200 deliberate decommission | **DONE 2026-07-15** | implemented WITHOUT the roster (`SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` + `ca-drop-member`); roster entry removal = Part IV forward-hook |
| Merkle tree-layer intermediate object | REJECTED | see §6 |
| JSON for control objects | REJECTED | abandoned 2026-06-24 |
| `CHCA` single magic with kind byte | REJECTED | replaced by per-kind `CABL`/`CATR` magic |
