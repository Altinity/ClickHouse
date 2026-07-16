---
description: 'Proposal v3 for CAS persisted formats: every metadata byte is human-readable text — JSON control plane, NDJSON record streams, a head -v-style manifest payload zone, a JSON blob-envelope header — with integrity delegated to storage, zstd, and the consuming layer; protobuf and all custom binary framing removed.'
sidebar_label: 'CAS codecs proposal v3'
sidebar_position: 12
slug: /superpowers/cas/codecs-proposal-v3
title: 'CAS Codecs Proposal V3'
doc_type: 'reference'
---

# CAS Codecs Proposal V3 {#cas-codecs-proposal-v3}

**Status:** approved design (brainstorm 2026-07-15; actionable spec:
`specs/2026-07-15-cas-codecs-v3-design.md`). Supersedes `codecs_proposal_v2.md` **in full**; the
audit in `codecs.md` maps the pre-v3 on-disk reality, and the living registry moves to
`Core/Formats/README.md` with this design. Where v2 and v3 disagree, v3 wins.
Structural facts quoted here (envelope core size, seek consumers, inline read path) were verified
against the code as of 2026-07-15; several numbered docs (`05-formats-and-backend.md` envelope
section) lag the code and are updated in the last migration step.

**Scope:** every persisted format under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`. Pre-release rules apply: no
persisted data exists in the wild, so there is no dual-read compatibility scaffolding — one cutover
per format, one new baseline.

## Grounding {#grounding}

v3 is the product of a survey of three serialization traditions, run 2026-07-14:

- **ClickHouse itself.** The control plane of a MergeTree part is plain text, one concern per file
  (`columns.txt`, `count.txt`, `metadata_version.txt`); structured control files are a text version
  line plus a body (`ttl.txt` = `ttl format version: 1` + JSON; `checksums.txt` = text version line
  + compressed binary table; mutation entries and replication log entries = `format version: N` +
  text). `DiskObjectStorageMetadata` is line-oriented text with a leading integer version. Binary
  survives only where data or Raft lives: `.bin` frame streams with an external `.mrk` index, Keeper
  changelog/snapshot.
- **S3-heavy table formats.** Iceberg: plain-JSON root metadata + Avro manifests + Puffin (a binary
  format whose self-description is still JSON-in-a-footer). Delta: NDJSON commit log + Parquet
  checkpoints + a `.crc` sidecar that is actually a JSON state summary. Hudi: HFile (an SSTable)
  for its metadata table — chosen for point lookups, a need CAS runs do not have. Counterexamples
  (Lance protobuf manifests, SlateDB FlatBuffers) optimize a hot CAS-swap window that our
  kilobyte-scale, per-namespace ref objects do not sit on.
- **S3 integrity primitives, 2024+.** S3 computes and stores a full-object CRC64NVME on every PUT
  (even from old clients), SDKs send trailing checksums and validate GETs by default, and
  conditional writes (`If-None-Match`, `If-Match`) are native. At-rest and in-transit corruption on
  the S3 backend is covered end-to-end by the provider.

And of two in-repo findings:

- **Ranged `seek` over record streams is dead code in production.** The only caller of
  `RunFileReader::seek` is `inDegreeInGeneration`, which itself has no production callers — every
  live consumer (fold merge, `zeroInDegree`, orphan scan, `fsck`) reads runs whole, sequentially.
- **Inline manifest bytes are served from memory** (`PartFolderView`,
  `ReadBufferFromOwnMemoryFile`), not by ranged reads into the manifest object — so the manifest
  encoding is free to choose any container; only the blob envelope needs constant payload offsets.

## Design Goals {#design-goals}

1. **Every metadata byte is human-readable.** Any CAS object except raw blob payload is inspectable
   with `head`, `zstdcat`, `jq`, `less`, and `diff` — no bespoke tooling, no `protoc`, no
   `CasInspect`. Debuggability of the GC protocol is the top design pressure at this stage of the
   project's life.
2. **One file shape.** Header line, body, optional trailer line. A reader of any CAS object follows
   the same steps: sniff line 1, gate on `v`, decode the body by family.
3. **No guard without a consumer.** Every integrity mechanism names the decision it protects.
   Truncation guards exist because decode is a consumer; a whole-stream checksum exists because the
   GC fold decides deletions; per-header hashes and per-block CRCs whose failure changes no decision
   are deleted, and bit-flip detection is explicitly delegated (storage provider, zstd frame,
   MergeTree's own `checksums.txt`, `fsck`).
4. **Streaming where necessary, materialized where sufficient.** Unbounded-cardinality data is
   NDJSON read line-by-line with O(one line) resident memory. Bounded metadata is materialized
   whole behind fail-closed size caps checked before allocation.
5. **Determinism by construction where consumed.** Objects compared byte-for-byte at adoption are
   written by our own writers with fixed field order and no compression — byte-stable without any
   canonicalization machinery.
6. **Zero new dependencies; two removed.** Parsing and writing use the `ReadHelpers` /
   `WriteHelpers` JSON primitives (`readJSONString`, `skipJSONField`, `writeJSONString` — the
   `JSONEachRow` code path): streaming, schema-driven, fail-closed. protobuf (currently the full
   runtime plus `libprotoc`) and the generated-code build step leave the subsystem entirely.

Non-goals: changing protocol semantics (journals, tokens, ack-floor); introducing a DOM JSON
library (Poco, simdjson, rapidjson are all rejected — materializing parsers with allocation churn,
no streaming); re-keying any object.

## The V3 Model {#the-v3-model}

### One File Shape {#one-file-shape}

```text
line 1   header   {"type":"cas_<object>","v":N}     — identity + version gate
body     one JSON object (control), NDJSON records (streams, manifest entries),
         or a raw payload zone (manifest, blob)
trailer  {"n":...}-style JSON line where the body is line-structured (streams, manifest)
```

- `type` is the magic: a `cas_`-prefixed string, unique per `FormatId`. `jq -r .type <(head -1 f)`
  identifies any CAS object; a stray file in a bucket self-describes.
- `v` is `compatibility_version`, the **only** version field. The reader gates on line 1 **before**
  touching the body: `v > G_BUILD` → `UNKNOWN_FORMAT_VERSION`. There is no `writer_version` and no
  `format_version`; forensic provenance is carried by `ch` and `bld` where it matters (envelope).
  `currentCompatibilityVersion` and `changePoints` in `CasFormat.h` stay as the stamping machinery.
- **Compression** is whole-object, single-frame `zstd`, and the policy is **per type and
  deterministic** (amended 2026-07-16): `Always` types are stored under a **`.zst` key suffix**
  and compressed regardless of instance size; everything else is raw with no suffix. `Always` is
  assigned by one rule — the object can grow large: `cas_ref_log`, `cas_ref_snap`,
  `cas_part_manifest`, `cas_gc_outcomes`. Always-small types stay bare `cat`-able JSON, and the
  deterministic types (`cas_run`, `cas_fold_seal`) stay raw despite their size — adoption compares
  bytes, and compressed bytes are only as stable as the vendored zstd version. There is no size
  threshold: a threshold would make the key a function of the body, breaking constructed-key
  point-GETs (`manifestKey`, `refSnapshotKey`). The zstd frame checksum (XXH64) is **enabled**,
  the declared content size is mandatory and checked against the per-type cap **before**
  decompressing (cap-before-alloc), and the body magic is **validated against the policy** —
  a compressed body in a raw type or vice versa is `CORRUPTED_DATA`, not a writer's choice.
  A CAS object still begins either with `{` or with the zstd magic (`28 B5 2F FD`), nothing else,
  and `.zst` is the standard extension every tool recognizes (`unzstd`, `zstdcat`).

### Families By Role {#families-by-role}

| Family | Body | Parsing contract | Used for |
|---|---|---|---|
| **Control** | one JSON object | materialized whole, size-capped | pool meta, ref log/snapshot, GC state, heartbeat, owner, epoch, mount lease, fold seal, outcome log, blob meta |
| **Record stream** | sorted NDJSON records + trailer | streamed, O(one line) resident, read whole | GC blob-target / source-edge runs |
| **Payload hybrid** | JSON descriptor lines + raw payload zone | descriptor materialized; payload bytes verbatim | part manifest (inline files), blob envelope (256-byte JSON header + payload) |

Raw passthrough files (namespace verbatim files, mountpoint objects) remain a deliberate
**non-family**: no header line, CAS never interprets the bytes. Unchanged from v2.

This is the industry split by role, not by size: Iceberg's JSON + Avro + Puffin, git's
refs/commits (text) + trees/packs (indexed binary) — with the difference that our "packs" (runs)
lost their index because nothing seeks into them.

## Universal Conventions {#universal-conventions}

- **Key naming:** 2–5 characters, documented per object in the registry (`codecs.md`). Values are
  full words (`"op":"merge"`, not codes). Units live in the registry, not in key names (`ts` is
  documented as milliseconds).
- **Hashes and ids:** lowercase fixed-width hex strings. Lowercase hex preserves unsigned byte
  order under lexicographic comparison (digits sort below `a`–`f`), so sorted-merge over hex keys
  equals today's binary key order.
- **Integers:** fields with a structural bound below 2^53 (lengths, offsets, entry counts,
  timestamps in ms) are JSON numbers. Unbounded `u64` counters (epochs, sequence numbers, blob
  sizes) are decimal **strings**, per the proto3 JSON mapping convention — `jq` and JS tooling stay
  exact.
- **Unknown keys:** a key prefixed `!` is a critical extension — a reader that does not understand
  it fails closed with `UNKNOWN_FORMAT_VERSION` (the TLV critical flag, textualized). Unknown keys
  without `!` are skipped via `skipJSONField` on tolerant (non-deterministic) objects and rejected
  as `CORRUPTED_DATA` on strict (deterministic) ones, where an unknown key would break re-encode
  determinism anyway.
- **Duplicate keys:** `CORRUPTED_DATA`.
- **Writers are hand-rolled** (`WriteBuffer` + `writeJSONString`/`writeIntText`), field order fixed
  and documented, map iteration sorted. This is what makes determinism a property instead of a
  feature. Readers are hand-rolled pull parsers over `ReadBuffer` — the manifest and envelope
  codecs stay "mapping plus invariants", exactly v2's shape, just text.
- **Padding hygiene ("no smuggling"):** every padding zone (envelope header pad, manifest
  inter-file gaps) has deterministic expected content; the decoder regenerates and verifies it.
  There are no unaccounted bytes in any CAS object.
- **Error taxonomy**, enforced in the shared helpers, not per codec: not-JSON / wrong `type` /
  truncation (missing trailer, cut line, bounds violation) / duplicate key / padding or banner
  mismatch / zstd error / missing or over-cap declared content size → `CORRUPTED_DATA`;
  future `v` / unknown `!`-key → `UNKNOWN_FORMAT_VERSION`.

## Control Plane {#control-plane}

One JSON object per file. Example (heartbeat, formerly the 24-byte unversioned exception):

```text
{"type":"cas_gc_hb","v":3,"by":"<32hex>","seq":"1741"}
```

- Tiny singletons (`cas_pool_meta`, `cas_gc_state`, `cas_gc_hb`, `cas_owner`, `cas_epoch`,
  `cas_mount_lease`, `cas_blob_meta`) are written and read whole; RTT dominates parse by orders of
  magnitude, so text costs nothing measurable. They are pinned raw (`Never`) and remain bare
  `cat`-able JSON. `cas_blob_meta` is CAS-swapped — the token semantics of the
  dedup/resurrect gate are untouched by the encoding.
- Can-grow-large control objects (`cas_ref_snap` complete tables, `cas_ref_log` transactions,
  `cas_gc_outcomes`) are `Always`-compressed under `.zst` keys;
  `zstdcat | jq` replaces `CasInspect`. The ref log — the commit point and the single most
  debugging-valuable object in the protocol — becomes a readable transaction list. The refsnaplog
  key↔body binding check (the decoded `ns`/`txn_id` must match the key the object was read from)
  stays as a per-object invariant and extends to the manifest.
- The fold seal is Control-family but **deterministic** (see [Determinism](#determinism)): strict
  keys, pinned raw.
- Per-object field mappings (today's proto fields → short JSON keys) are per-codec work items in
  the migration plan, executed under the naming policy above and recorded in the registry.

The v2 complaint "the version gate runs after the parse" dissolves rather than being fixed: JSON of
any future version is still parseable JSON, so version only gates interpretation, and the gate
reads line 1. A future object always reports `UNKNOWN_FORMAT_VERSION`, never a parse error.

## Record Streams {#record-streams}

The GC data plane: unbounded-cardinality sorted record sets, written once per GC
generation/attempt/shard, always read whole.

```text
{"type":"cas_run","v":3,"kind":"source_edge"}
{"b":"01<digest-hex>","s":"<32hex>","m":"edge"}
{"b":"01<digest-hex>","s":"<32hex>","m":"zero"}
...
{"n":184267}
```

- **Sorted NDJSON.** One record per line, keys in today's sort order (`b` = algo-prefixed blob
  digest, `s` = source id; tuple comparison over fixed-width lowercase hex equals the current
  binary key order). Marker values are words: `edge`, `zero`, `condemned`.
- **No blocks, no footer index, no `seek`.** All production consumers read sequentially, so the
  block/footer machinery of the `CARN` format has no consumer left. Writer memory is O(one line);
  reader memory is O(one line); the k-way merge holds O(inputs) lines. `RunFileReader::seek`,
  `inDegreeInGeneration`, and `SourceEdgeKeyCodec::seekPrefix` are deleted (the test helper
  rewrites as a sequential scan). If a point-lookup consumer ever appears, an indexed format
  returns as a new `kind` — an additive change that does not need to be reserved for.
- **Typed opens stay:** `openSourceEdgeRun` validates `type`, `v`, `kind` from the header line
  before any record is interpreted; unknown `kind` fails closed.
- **Integrity: the read unit is now the whole file**, so the guard is the whole-file checksum that
  already exists — `RunRef.checksum` (CityHash128 over the stored bytes), carried by the referencing
  fold seal. Every consumer streams the full object anyway; it accumulates the hash while reading
  and verifies against the seal **before acting on what it read**. This guard has a real consumer:
  the fold decides deletions. The trailer count `n` additionally catches truncation at a line
  boundary (NDJSON's one blind spot).
- **Deterministic, therefore raw.** Runs go through `putDeterministicArtifact` (byte-compare
  adoption), so they are pinned uncompressed.

**Accepted cost:** hex NDJSON is ≈2× the bytes of the retired binary encoding (a source-edge record
is ~110 bytes vs ~50), and determinism forbids compressing it away. At 10M records that is roughly
1 GB vs 500 MB per full fold artifact set, read 2–3 times over its life, in the background. This is
knowingly the scale at which Iceberg chose Avro; we choose text anyway while the system is
pre-release and GC debuggability is the bottleneck, measure in soak, and keep "re-binarize runs
only" as a localized fallback (one family, one `kind` bump, no other object affected).

## Part Manifest {#part-manifest}

The one object that mixes structure with raw bytes: per-part file list where small sidecar files
travel inline (cap 1 MiB/file, 16 MiB total) and large files are blob references. Inline bytes are
served from the decoded manifest in memory; nothing range-reads into the object. base64 was
rejected — payload does not belong inside the descriptor, and most inline files are themselves
text that base64 would blind.

**Shape: JSON descriptor lines, then a raw payload zone with `head -v`-style banners:**

```text
{"type":"cas_part_manifest","v":3}
{"path":"columns.txt","inline":{"off":0,"len":34}}
{"path":"count.txt","inline":{"off":56,"len":7}}
{"path":"data.bin","blob":{"hash":"<hex>","size":"184549376"}}
{"n":12,"plen":4321}

==> columns.txt (34) <==
2 columns:
`id` UInt64
`s` String

==> count.txt (7) <==
1000000
```

- The descriptor is pure JSON lines: `head -n $((n+2)) | jq` works. Entries carry exact
  `off`/`len` into the payload zone (offsets exclude banners and padding).
- The payload zone starts after one blank line; each inline file is preceded by the banner
  `\n==> <path> (<len>) <==\n`. In `less` the zone reads like `head -v *` over the part directory;
  `/==>` navigates, `grep -a` extracts. Banners are a **deterministic function of the entries**:
  the decoder regenerates each gap and verifies it byte-for-byte — padding stays smuggling-proof
  without any free-form bytes.
- Trailer: `n` guards descriptor truncation, `plen` cross-checks the descriptor against the zone.
  There is **no payload hash**: truncation is caught by `n`/`plen`/bounds checks, bit flips are
  covered by the provider (S3), the zstd frame (when compressed), and one layer up by MergeTree
  itself — the inline files are exactly the sidecars that `checksums.txt` (traveling inline)
  verifies at part load, and the files it does not cover (`columns.txt`,
  `default_compression_codec.txt`, `metadata_version.txt`) are equally uncovered in vanilla
  ClickHouse on local disks, where parsing is the fail-closed guard. CAS makes no decisions from
  inline bytes; it serves them. Guards without consumers are not written.
- The embedded `CARN` stream, `RunKind::ManifestEntries`, and `payload_digest` are deleted with the
  old hybrid codec. Manifests are not byte-adopted (CAS-by-token), so they are free to compress —
  and the banner/padding overhead vanishes under zstd.

## Blob Envelope {#blob-envelope}

The only object with a hot ranged-read path keeps its structural contract — a fixed-length header
and payload at the constant pool-wide offset `blob_header_len` (256, a `PoolMeta` parameter) — but
the header becomes one JSON line padded with whitespace:

```text
{"type":"cas_blob","v":3,"tag":"<32hex>","bld":"<32hex>","ts":1752537600123,"by":"<32hex>","op":"merge","ch":26006001,"ref":"t-.../all_1_2_0"}
```

Layout: bytes `[0, json_len)` are the JSON object, `[json_len, 255)` are spaces, byte 255 is `\n`.
`head -c 256 blob | jq .` decodes it with no tooling (JSON parsers ignore trailing whitespace —
this is why the pad is spaces, not zero bytes). The decoder verifies the pad zone is exactly
spaces-then-newline (no smuggling).

| Key | Was | Meaning |
|---|---|---|
| `type` | magic `CABL` | object kind |
| `v` | `compatibility_version` | read gate |
| `tag` | `incarnation_tag` | fresh random u128 per upload attempt (W-FRESH-TAG); the exact-token delete primitive |
| `bld` | `build_id` | the writer build that uploaded this incarnation; newborn-debris watermark attribution, B170 token-join |
| `ts` | `created_at_ms` | decimal unix ms, number |
| `by` | `creator_server_id` | 32 hex |
| `op` | `ProvenanceOp` | `insert` / `merge` / `mutation` / `attach` / `repack` / `other` |
| `ch` | `ch_version` | `VERSION_INTEGER` (e.g. `26006001`), number |
| `ref` | `intended_ref` | diagnostic; truncated so the header line fits byte 255 |

Everything except `ref` totals ~193 bytes, leaving ~54 characters of `ref` content; 256 holds with
the full diagnostics on board, so `blob_header_len` does not change.

**Dropped fields, each with its reason:**

- `hash_algo` — the identity pair (algo + digest) lives in the object key and in every manifest
  `ref`; a reader always arrives via one of those. Redundant in the header.
- `domain_id` — written by everyone, validated by no one (no production reader exists). YAGNI.
- `header_hash` — no consumer: `tag` comparisons at condemn/delete are storage-vs-storage (both
  sides read the same stored bytes, so corruption is self-consistent and degrades to a spared
  object, never a wrong delete), and provider checksums cover S3. With it dies the last
  zeroed-slot hash recipe in the protocol, and CityHash64 leaves the protocol entirely (v2's
  primitive-unification goal, reached by deletion instead of replacement).
- `writer_version` — forensics are `ch` + `bld`.
- `logical_size` / `logical_hash` — already gone from the code (2026-07-11 S3-staging fix); noted
  here because `05-formats-and-backend.md` still describes the 94-byte core.

The TLV area is replaced by JSON keys under the `!`-prefix critical convention. The header is still
built before the payload streams (S3-native staging compatible). Payload bytes, their constant
offset, and the "no frame-level compression" rule (payload identity is the raw bytes) are
untouched. Envelope byte-determinism is structurally absent by design — `tag` is fresh per
incarnation — so text costs nothing there.

## Raw Passthrough {#raw-passthrough}

Namespace verbatim files and mountpoint objects: no header line, no `type`, bytes preserved
verbatim, integrity belongs to the file's own format. The one deliberately unguarded,
uninterpreted surface — accepted and documented, unchanged from v2.

## Integrity Model {#integrity-model}

> **Every guard names the decision it protects, is verified before that decision, and matches the
> read unit. Detection of storage-level corruption is delegated to the layers that own it; a guard
> whose failure changes no decision is not written.**

| Read unit | Guard | Consumer it protects |
|---|---|---|
| Header line | `type`/`v`/key validation, first | every decode (fail-closed dispatch) |
| Control body | JSON parse + strict/tolerant key rules + caps | every decode |
| Compressed body | zstd magic, declared content size ≤ cap before alloc, XXH64 frame checksum | every decode |
| Stream (whole file) | seal-held `RunRef.checksum` (CityHash128), accumulated during the full read, verified before use; trailer `n` for line-boundary truncation | GC fold / `zeroInDegree` / `fsck` — deletion decisions |
| Manifest descriptor + zone | trailer `n`/`plen`, entry bounds checks, regenerated banners/padding | manifest decode; inline-file content verified one layer up by MergeTree `checksums.txt` at part load |
| Envelope header | JSON parse + pad verification | mount/GC/fsck header reads; `tag` semantics are storage-vs-storage and fail-safe |
| Blob payload | content hash at `fsck`; ClickHouse's own per-frame checksums inside part data files | delegated, as in v2 |
| Uncompressed local files | storage/filesystem + `fsck` | vanilla-ClickHouse parity for text control files |
| Raw passthrough | none at the CAS layer | delegated to the embedded format |

Primitives: **CityHash128 = identity** (blob content hash, run checksums, adoption), verified where
identity is consumed. **zstd XXH64 = corruption guard on compressed bodies.** CRC32C and CityHash64
appear nowhere; the v2 plan to add CRC32C everywhere is replaced by delegation with named owners.

## Determinism {#determinism}

Exactly the objects that pass through `putDeterministicArtifact` (byte-compare adoption) must be
byte-deterministic: the **fold seal** and the **GC runs**. For them: pinned raw (never compressed),
strict keys, fixed field order, sorted iteration — properties of our hand-rolled writers, enforced
by golden tests, with no canonical-JSON machinery (no floats exist in any CAS object). Everything
else follows the per-type compression policy (`Always` under `.zst` keys for can-grow-large types,
raw otherwise); golden tests for `Always` formats assert decoded content plus a pinned-zstd byte
snapshot.

## Schema Evolution And Mixed-Version Mounts {#schema-evolution-and-mixed-version-mounts}

v3 changes the container, not the evolution doctrine
(`specs/2026-06-24-cas-schema-evolution-framework-design.md`, `05-formats-and-backend.md`
§schema-evolution). Three gates, all retained with identical semantics:

1. **Per-object:** `v ≤ G_BUILD` or `UNKNOWN_FORMAT_VERSION` — now gated on line 1, before the
   body. New code always reads old: a build keeps every decoder arm for generations
   `1..G_BUILD` until the ladder is pruned.
2. **Per-pool (mount time):** `PoolMeta.min_reader_generation > G_BUILD` → the binary is too old
   to mount (forward floor); a pool-meta `v` below a removed-format floor → the pool is too old
   for this binary (backward floor, the `kRefSnapshotLogGeneration` precedent). Both fire before
   any other object is touched. Today (pre-release) the floor is raised aggressively —
   `admitOrValidate` CAS-raises it to the admitting build's own `G_BUILD`; the release-grade
   replacement is the roster below.
3. **Fleet rollout (designed, deliberately unbuilt — Part IV):** the durable **roster** in pool
   meta (`{server_id: {path, G_build}}`), floor = `min(G_build)` over members;
   `max_content_addressable_pool_format` as the staged-rollout cap; a writer emits format `V` iff
   `min_reader(V) ≤ floor AND V ≤ setting`. A long-absent member pins the floor until explicitly
   decommissioned (B200) — fail-safe.

Change recipes under v3, per kind of change:

| Change | Recipe |
|---|---|
| Additive field, safe to ignore | new tolerant key; **no `v` bump**; old readers skip it via `skipJSONField`. On **mutable** objects the field is best-effort until the floor rises: an old-build rewrite re-encodes from its own struct and omits it. This is not a JSON regression — protobuf unknown-field preservation only masked the read-modify-write subcase; an old build writing the object fresh dropped the field there too. Design additive fields on mutable objects to be safe-to-lose, or wait for the floor. |
| Breaking / reinterpreting | encoder ladder keyed by the target version (write-down-to-floor: emit the field-set valid at the pool floor); bump `G_BUILD` + `changePoints`; the new field-set is written only once the floor reaches it, and the floor raise is exactly what fences old builds out at mount. Prune the ladder after the upgrade cycle. |
| New stream record `kind` | new `kind` value + typed open; old readers fail closed on the header line. |
| Deterministic formats (fold seal, runs) | additive **is** breaking (strict keys), plus the **adoption pin**: the `putDeterministicArtifact` conflict path already fetches the existing object — the re-deriving writer re-encodes at the `v` read from that object's header line, not at its own freshest. A leader failover that spans an upgrade therefore byte-matches an in-flight generation's artifacts instead of wedging on `CORRUPTED_DATA`. |
| Envelope header | new keys under the `!`-critical convention; `blob_header_len` is a pool-creation parameter, not a format constant. |
| File shape itself (header line / trailer contract) | the one thing that must never change shape; a change here is a new `type`. |

The mixed-fleet read matrix this yields: **new reader + old object** — always fine (decoder arms
kept). **Old reader + new object** — fine for additive changes (skips unknown keys), structurally
impossible for breaking ones (the floor that allows writing the new format has already refused the
old binary at mount). **Too-old binary + pool** — refused at mount by the floor; **binary + too-old
pool** — refused at mount by the backward floor.

Pre-release stance (unchanged, `feedback` 2026-06-24): no ladders and no roster exist yet; until
first release a `G_BUILD` bump may simply fail-close old pools, which are recreated. The recipes
above are what the door stays open for.

## Size Caps {#size-caps}

Checked **before** allocation: the zstd declared content size (compressed arm) or the accumulated
read (raw arm) against the per-type cap; NDJSON and descriptor lines against a per-type line cap.

| Class | Object cap | Line cap |
|---|---|---|
| Singletons (`cas_pool_meta`, `cas_gc_state`, `cas_gc_hb`, `cas_owner`, `cas_epoch`, `cas_mount_lease`, `cas_blob_meta`) | 1 MiB | 64 KiB |
| `cas_ref_log` / `cas_ref_snap` | today's byte budgets (`ref_txn_max_bytes`, removal class), re-derived for JSON at plan time | 64 KiB |
| `cas_part_manifest` | 256 MiB | 64 KiB (descriptor lines; payload governed by inline caps: 1 MiB/entry, 16 MiB total) |
| `cas_gc_outcomes` / `cas_fold_seal` | 256 MiB | 64 KiB |
| `cas_run` | no object cap (streamed) | 4 KiB |
| `cas_blob` header | 256 bytes exactly | — |

A cap hit is `CORRUPTED_DATA` (100–1000× above realistic sizes; hitting one means a corrupt object
or a protocol bug). Caps are constants next to the codec, revisited with soak numbers.

## Provider Metadata Mirror (Optional) {#provider-metadata-mirror}

An optional convenience tier with one hard rule, inherited from the body-carried-tag rationale in
`05-formats-and-backend.md`: **provider metadata is a mirror for eyes and dashboards; the protocol
never reads it** (write-once on S3, 2 KiB cap, dropped by many copy tools, absent on the local
backend — fail-close forbids depending on it).

- `Content-Type` per family: `application/json` (control), `application/x-ndjson` (runs),
  `application/octet-stream` (blobs, manifests).
- `x-amz-meta-cas` = a byte-for-byte copy of the object's header line (≤256 bytes). `aws s3api
  head-object` on any debris key then answers "who, when, which `op`, which `ref`" without a GET —
  the first question of every dangling/unaccounted investigation.
- No `Content-Encoding` on compressed objects (SDKs and proxies may "helpfully" decode; compression
  is sniffed from the body). Raw passthrough files get no mirror; writers pass metadata explicitly,
  the backend sniffs nothing.

## Object Dispositions {#object-dispositions}

Inventory verified against the code on `cas-gc-rebuild` 2026-07-15 (the v2/audit tables were
stale: `CARS` was replaced by the refsnaplog objects, `CART` was removed 2026-07-10, and the
blob-meta sidecar was missing).

| Object | Was | v3 `type` | Family | Compression | Change |
|---|---|---|---|---|---|
| Blob | `CABL` binary core 70 B + TLV | `cas_blob` | Payload hybrid | payload: none (structural) | JSON header line padded to 256; drop `hash_algo`, `domain_id`, `header_hash`, `writer_version`; TLV → `!`-keys |
| Blob meta sidecar | fixed 22-byte binary | `cas_blob_meta` | Control | no (tiny) | one-line JSON; CAS/resurrect token semantics untouched |
| Pool meta | `CAPM` proto | `cas_pool_meta` | Control | no (always small) | JSON |
| Ref log txn | custom binary, versioned, no magic | `cas_ref_log` | Control | **always, `.zst`** | JSON; key↔body binding check stays; byte budgets re-derived |
| Ref snapshot | custom binary | `cas_ref_snap` | Control | **always, `.zst`** | JSON; the complete ref table becomes readable |
| Ref cleanup marker | empty body | — | non-family | — | unchanged: key-only presence marker, documented in the registry |
| Part manifest | `CAPT` hybrid (binary header + embedded `CARN`) | `cas_part_manifest` | Payload hybrid | **always, `.zst`** | JSON descriptor + banner payload zone; embedded stream, `RunKind::ManifestEntries`, `payload_digest` deleted |
| GC runs | `CARN` blocks + footer index | `cas_run` | Record stream | no — deterministic | sorted NDJSON + trailer; blocks/footer/`seek` deleted; seal checksum verified on every read |
| GC state | `CAGT` proto | `cas_gc_state` | Control | no (always small) | JSON |
| GC heartbeat | 24-byte raw, unversioned | `cas_gc_hb` | Control | no (tiny) | JSON; the exception dies |
| Fold seal | proto (`CasGenerationSeal`) | `cas_fold_seal` | Control | **no — deterministic** | JSON, strict keys |
| Outcome log | `CAGO` proto | `cas_gc_outcomes` | Control | **always, `.zst`** | JSON |
| Owner anchor | `CAOW` proto | `cas_owner` | Control | no (tiny) | JSON |
| Server epoch | `CAEP` proto | `cas_epoch` | Control | no (tiny) | JSON |
| Mount lease | `CAML` proto | `cas_mount_lease` | Control | no (tiny) | JSON |
| Part-manifest cleanup run | `CARN` | — | — | — | **deleted** with the fold seal's cleanup field and `partManifestCleanupKey` (sealed but never read, per the audit) |
| Roster | reserved `FormatId`, unbuilt | reserved | — | — | unchanged |
| Namespace verbatim / mountpoint | raw | — | non-family | — | unchanged |

Subformat wire shapes (`ManifestRef`, `RunRef`, `Token`, owner bindings) become JSON sub-objects of
their parents under the same key-naming policy; their decode-side domain validation (audit's
"Tighten Decoder Strictness") lands with each codec's migration.

## Code Layout {#code-layout}

All codecs live in a dedicated **`Core/Formats/`** directory
(`specs/2026-07-15-cas-codecs-v3-design.md` §code-placement): the directory listing is the format
registry, and the layering rule is physical — `Formats/` may include only IO primitives and the
identifier vocabulary (`CasIds`, `CasToken`, `CasBlobRef`, `CasManifestId`, `CasRefIds`), never
`CasBackend`/`CasStore`. A codec that wants a backend does not compile; "mapping-only" is
structure, not review convention. Mixed files split accordingly: wire structs move with their
codecs, protocol lifecycle logic (`claimMount`, resurrect CAS helpers) stays in `Core/`.

```text
Core/Formats/
  README.md                  bucket map + codec registry + evolution rules (the living registry)
  CasFormat.{h,cpp}          FormatId, type strings, G_BUILD, changePoints, checkCompatibility,
                             per-format {family, caps, compression, strictness} table
  CasTextFormat.{h,cpp}      header/trailer line write+sniff, zstd wrap/unwrap with cap-before-alloc,
                             padding/banner helpers, error taxonomy — the only place that knows the
                             file shape
  CasPoolMetaFormat.*        cas_pool_meta
  CasRefLogFormat.*          cas_ref_log
  CasRefSnapshotFormat.*     cas_ref_snap
  CasPartManifestFormat.*    cas_part_manifest (descriptor + banner payload zone)
  CasRecordStreamFormat.*    cas_run: writer / reader / k-way merger + typed opens
  CasFoldSealFormat.*        cas_fold_seal
  CasGcStateFormat.*         cas_gc_state + cas_gc_hb
  CasGcOutcomesFormat.*      cas_gc_outcomes
  CasServerRootFormats.*     cas_owner, cas_epoch, cas_mount_lease
  CasBlobEnvelopeFormat.*    cas_blob 256-byte header (encode/decode + pad verification)
  CasBlobMetaFormat.*        cas_blob_meta
```

The checklist for adding (or finding) a persisted object:

1. **One `FormatId` entry** in `Formats/CasFormat.h`: `type` string, family, caps, compression
   policy, strict/tolerant. A unit test asserts the table is complete.
2. **One `Cas<Object>Format.{h,cpp}`** in `Formats/`: wire struct + `encodeX` / `decodeX` +
   invariants, nothing else. If a codec contains raw header/trailer plumbing, review rejects it —
   that lives in `CasTextFormat` only.
3. **One storage key** in `CasLayout`, documented with owner and lifecycle.
4. **One registration** in the shared test harness.
5. **One row in `Formats/README.md`** — same commit. The registry lives next to the code;
   `codecs.md` is retitled as the pre-v3 historical audit.

**Shared test harness**, one registration line per format: round-trip equality; golden **text**
files (human-diffable; compressed arm pinned against vendored zstd); truncation at every line
boundary → `CORRUPTED_DATA`; `v+1` → `UNKNOWN_FORMAT_VERSION`; wrong `type` (another format's
valid object) → `CORRUPTED_DATA`; unknown plain key → skipped (tolerant) / `CORRUPTED_DATA`
(strict); unknown `!`-key → `UNKNOWN_FORMAT_VERSION`; duplicate key → `CORRUPTED_DATA`; declared
size over cap → `CORRUPTED_DATA` with no allocation; padding/banner mutation → `CORRUPTED_DATA`.
The harness does **not** pretend to catch bit flips in uncompressed bodies — that delegation is
explicit and tested at the layers that own it (`fsck`, MergeTree load checks).

## What Is Deleted {#what-is-deleted}

- `cas_format.proto`, `clickhouse_cas_proto`, `protobuf_generate_cpp`, and the protobuf link
  dependency (currently the **full** runtime plus `libprotoc`).
- The v2 plan's `CasFrame` / 32-byte `CasProtoFrame` / 8-byte binary preamble — never built.
- `CARN` block framing, footer index, `RunFileReader::seek`, streaming ranged-get machinery,
  `inDegreeInGeneration`, `SourceEdgeKeyCodec::seekPrefix`.
- The binary envelope codec, its TLV parser, `header_hash`, `domain_id`, `hash_algo`.
- The refsnaplog custom binary codecs and the 22-byte fixed blob-meta codec (both replaced by
  JSON; key↔body binding and token semantics preserved).
- The heartbeat fixed-record codec (last unversioned object) and the part-manifest cleanup run.
- Most of `CasInspect` (`jq` replaces it; `ca-inspect` may remain as a trivial
  "decompress + print" convenience or be dropped).
- CityHash64 from the protocol; all zeroed-slot hash recipes; the CRC32C adoption plan.

## Accepted Trade-Offs {#accepted-trade-offs}

- **Deterministic runs cost ≈2× bytes** (hex, uncompressed). Measured in soak; localized fallback:
  re-binarize `cas_run` only.
- **Uncompressed local text files carry no CAS-layer corruption guard** — exactly the risk level of
  every vanilla ClickHouse control file on the same disk, with `fsck` and MergeTree's own checks
  above, and provider checksums on S3. Chosen deliberately over guards without consumers.
- **protoc-decodability is gone; `jq`-decodability replaces it.** Third-party reimplementation now
  needs: JSON, zstd, and this registry — strictly more universal primitives than protobuf +
  custom frames.
- **Text parse is slower than binary parse.** Irrelevant everywhere it occurs: control objects are
  RTT-dominated; the manifest is capped and parsed at part load; runs are background GC work
  parsed at `JSONEachRow`-class throughput.

## Alternatives Considered {#alternatives-considered}

- **v2 (universal 8-byte binary preamble + 32-byte framed protobuf).** Rejected: puts the entire
  control plane behind bespoke tooling against both the ClickHouse and the lakehouse tradition;
  reinvents the standard zstd container (magic, length, checksum) while disabling zstd's own
  checksum; adds CRC32C guards whose failure changes no decision; its flagship win (root-shard
  compression) is modest because typical root shards are kilobytes. The genuinely good parts —
  gate-before-parse, typed opens, cap-before-alloc, error taxonomy, the shared harness, killing
  the heartbeat exception — are all retained here in text form.
- **Avro object-container files for runs.** Schema-once-per-file, per-block compression — but no
  key-based seek (irrelevant now), a new dependency in the disk layer with no in-repo precedent
  outside Formats, random sync markers that break byte-determinism, and no `jq`. Rejected.
- **Parquet for runs.** The Delta-checkpoint shape, but drags Arrow into the storage core; our
  records are two hashes and a marker — row-group machinery with nothing to prune. Rejected.
- **MessagePack.** Schema-less binary JSON: loses both human readability and schema enforcement.
  Rejected.
- **protobuf kept for the manifest only.** One big message with `repeated` entries parses whole
  (fine under the cap) but is not incrementally readable by generated code, keeps the protobuf
  dependency alive for one object, and splits the mental model. Rejected with the banner-zone
  design in hand.
- **base64 / hex inline bytes in a pure-JSON manifest.** Payload inside the descriptor: blinds the
  mostly-text sidecars, bloats, and needed a payload hash to feel safe. Rejected for the
  descriptor + payload-zone hybrid.
- **Keeping per-block CRC + footer in runs "just in case".** A guard and an index with no consumer;
  YAGNI by the audit's own rule. The seal checksum covers the actual read unit (the whole file).

## Migration Plan {#migration-plan}

Pre-release, single cutover per format, each step lands green on its own:

1. **Bootstrap `Formats/`** — `CasTextFormat` + shared harness + `CasFormat` moves + README
   skeleton. Pure addition, nothing wired.
2. **Control plane** — pool meta, GC state + heartbeat (`cas_gc_hb` kills the last unversioned
   format), outcomes, fold seal, owner/epoch/lease: per-object key mapping, codecs become
   mapping-only in `Formats/`, register in the per-format table, migrate tests, add the audit's
   decoder-strictness validations while touching each decoder; protobuf messages die per object.
3. **Refsnaplog** — ref log txn + ref snapshot → JSON; byte budgets re-derived for JSON inflation;
   the key↔body binding invariant kept.
4. **Blob meta** — one-line JSON; CAS/resurrect token semantics untouched.
5. **Runs** — NDJSON stream writer/reader/merger, typed opens, seal-checksum verification on every
   full read; delete blocks/footer/`seek`/`inDegreeInGeneration`/`seekPrefix` and the
   part-manifest-cleanup run + the fold seal's cleanup field + `partManifestCleanupKey`.
6. **Part manifest** — descriptor + banner payload zone; delete the embedded stream path and
   `RunKind::ManifestEntries`.
7. **Blob envelope** — JSON header line, pad verification, field drops; golden tests re-pinned;
   `blob_header_len` stays 256.
8. **Finish** — provider-metadata mirror in the backend PUT path (`Content-Type` per family,
   `application/zstd` for `.zst` objects); protobuf build wiring removed; docs updated
   (`05-formats-and-backend.md` envelope + evolution sections, `codecs.md` retitled historical,
   README finalized); `CasInspect` gutted or removed. (Compression needs no separate step: the
   `.zst` suffix and the `Always` policy land with each format's own cutover, since the key
   changes together with the body.)

Steps 1–2 deliver the bulk of the value (readable control plane, uniform shape, protobuf mostly
gone); 3–7 convert the rest and delete the exceptions; 8 is switch-flips and hygiene. Formats are
CAS-by-token or write-once, so a half-migrated codebase is fine as long as each format cuts over
atomically in one commit.

## What This Buys, Concretely {#what-this-buys-concretely}

- **Introspection:** every metadata object in the system readable with `head` / `zstdcat` / `jq` /
  `less` / `diff`; the manifest payload zone reads like `head -v` over a part directory; `HEAD` on
  any object answers who/when/why via the metadata mirror. `CasInspect` and `protoc` leave the
  debugging loop.
- **Dependencies:** protobuf (full runtime + `libprotoc`) and code generation removed from the
  subsystem; zero serialization dependencies added — the JSON path is `ReadHelpers`, already in
  `clickhouse_common_io`.
- **Families:** 5 + 1 exception → 3 + explicit raw passthrough; zero unversioned objects; one file
  shape; one version field, gated on line 1.
- **Formats code:** framing/parse plumbing collapses into one shared helper file; codecs are key
  mapping + invariants; the stream implementation loses blocks, footer, seek, and dual-mode
  complexity.
- **Integrity:** every guard has a named consumer; CityHash64, CRC32C plans, and zeroed-slot
  recipes deleted; corruption detection delegated to the layers that own it (provider checksums,
  zstd frame, MergeTree checks, `fsck`) — with the delegation stated, not accidental.
- **Golden tests become readable text diffs**, and a layout mistake fails pinned tests instead of a
  soak run.
