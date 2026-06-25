# CAS schema-evolution framework + format freeze — design

**Status:** design (operator-approved direction, 2026-06-24; ready for an implementation plan)
**Date:** 2026-06-24
**Branch:** `cas-vfs-path-mapping`
**Supersedes:** the format fragments at the tail of
`docs/superpowers/deferred_backlog/cas-mergetree-integration.md` — the `### B13 — pool-format
versioning design` block (incl. the `B200` row) and the `### Layout-gate decisions` block. Those are
removed from the backlog and folded here.
**Related backlog gates:** B13, B176, B92, B97/B10/B96, B8/B64/B1. Cost items B164b and B147 are
**out of scope** (they touch bytes only incidentally and are tracked separately).

## Goal

Give the content-addressed (CA) MergeTree disk a **clean, uniform, understandable framework for
schema evolution** so that, after the on-disk format is frozen for the first release, we can change
it later **without ever misreading old data and without a forced migration**. Concretely:

1. Settle **one** serialization taxonomy (which object is binary / protobuf / JSON) and stop the
   current drift (see Background).
2. Make object identity (the content-address hash) **a Merkle function of logical inputs**, decoupled
   from serialization — so dedup is robust and the on-disk encoding can evolve freely.
3. Replace the monotone `checkVersion` gate with a **`writer_version` + `min_reader_version`** model
   that distinguishes additive from breaking changes.
4. **Freeze**, at generation 1, every byte-level decision that is irreversible after release
   (the self-describing header, the hash domain/Merkle rule, reserved slots, `blob_header_len`).
5. **Design — but defer** the cross-server rollout machinery (the pool-format setting, the durable
   roster, deliberate decommission); none of it lives in the frozen bytes and none is needed until the
   first *breaking* change ships into a mixed-version cluster.

Guiding principle for what we build *now*: pay only for the **irreversible** byte-level decisions;
defer all consensus machinery until there is something to migrate *to*.

## Background — the objects and the current drift

CA persists these object classes:

| Class | Objects | Mutable? | Content-addressed? | Today |
|---|---|---|---|---|
| **hashed** | blob, tree | no | **yes** | binary; `"CHCA"` 96 B header (blob padded to `blob_header_len`); tree has a *second* `"CATR"` payload header; `treeId = CityHash128(encoded catalog)` |
| **mutable, large/complex** | root-shard manifest + journal, gc-snap | yes | no | manifest **protobuf** (`CODEC_VERSION=1`); gc-snap custom binary (`GC_SNAP_VERSION`) |
| **mutable, tiny/flat** | pool-meta, watermark, gc-state, retired-set | yes | no | strict JSON |

Two drifts to fix. (1) `CasCodecUtil.h` documents a *two-class* split (spec §4, 2026-06-11) — binary
for hashed, strict JSON for non-hashed — but the manifest is in fact **protobuf**, and §4 found JSON to
be a hot-path bottleneck that gets ugly as an object grows. So there are really three encodings; this
spec makes that official with a sharp rule. (2) A hashed object today has **two headers** (`CHCA` +
`CATR`) and computes `treeId` by re-hashing the serialized catalog — fragile. This spec collapses to
**one header** and a **Merkle identity**.

There is **no released CA data** yet (PoC/soak stores are throwaway). Everything below *defines*
generation 1; it is not a migration of existing bytes.

---

# Part I — The encoding taxonomy

## The discriminator

```
Is the object content-addressed (its key is its content hash)?
  ├─ yes →  canonical BINARY  (blob, tree)
  └─ no  →  PROTOBUF          (EVERY mutable object: manifest + journal, gc-snap, gc-state,
                               retired-set, watermark, pool-meta, roster)
```

**Two encodings only — JSON is abandoned (2026-06-24).** An earlier draft kept JSON for "rarely-touched
control-plane" objects, but once the hot-path rule moved `gc-state`/`retired-set`/`watermark` to
protobuf, the only JSON object left in the release was `pool-meta` (the future `roster` is deferred to
Part IV). Carrying the whole JSON codec family (`JsonObjectWriter`, the `require*`/`checkNoUnknownKeys`
helpers, `parseJsonDocument`, the version-aware unknown-key rule, `tolerateUnknownKeys`) for one tiny
create-once object is not worth the special-case surface. The "human-inspectable with plain S3 tools"
benefit had already evaporated — every other mutable object needs decode tooling — so `pool-meta`
joins the rest as protobuf. If a plain-readable touch is ever wanted, that is **B180** (a tiny static
human-readable pool *breadcrumb*), not a versioned JSON codec.

## Why each branch

- **Hashed ⇒ binary.** Dedup correctness does **not** force this (identity is now Merkle over logical
  inputs — Part III — so it is independent of serialization). The reasons are: (1) the hashed objects
  are the **hottest read path** (a tree is read on every part open) and binary parses fastest; (2) the
  tree object carries **raw inline payload** (small part files) in a catalog-first / data-last layout
  that is a natural custom-binary structure; (3) a blob is raw file bytes anyway. Determinism is a
  property we get for free from the Merkle rule, not a reason to pick binary.
- **Mutable ⇒ protobuf (all of them).** Never hashed, so non-canonical wire output is harmless.
  Protobuf gives free **skip-unknown** (the engine of additive evolution) and avoids the two problems
  §4 found with JSON: parse/serialize cost on the hot path, and unreadable codec code as the schema
  grows. Even the tiny create-once objects (`pool-meta`) use protobuf — one mechanism for every mutable
  object beats a special-case codec for one or two of them (see the "JSON abandoned" note above).

## Drop packs — inline the eager part files into the tree

The "open a part in ≈1 GET" goal (B10) and "pack small files" (B97) are **both** achieved by
**inlining**, with strictly less machinery than a `pack` object type (no new object kind, producer,
slice locator, or third GC edge).

The tree codec already supports `Placement::Inline`. Today the part writer does **not** use it — every
part file is staged as `Placement::Blob` (`ContentAddressedTransaction.cpp:560`), so a part with N
files = N blobs = N GETs to open. The fix:

- **Eager part-load files** — `checksums.txt`, `columns.txt`, `count.txt`, `serialization.json`,
  `metadata_version.txt`, `partition.dat`, `minmax_*.idx`, codec/TTL files, small `primary.idx`:
  **inline into the tree**. Small and always read at part open, so they ride the single tree GET. This
  *is* B10.
- **Lazy per-column data** — `.bin`, `.mrk`: **stay `Blob`**. ClickHouse loads marks lazily per column
  via `MarkCache`; a query touching 3 of 200 columns must fetch 3 blobs, not all 200. Packing these
  would destroy column selectivity — which is why inline is *better* than packs here, not just simpler.

A size threshold drives inline-vs-blob (a large `primary.idx` may stay a blob); the threshold and file
list are tuning details, not freeze decisions. **Placement does not affect identity** (Part III), so
inline↔blob is a free storage choice.

**Freeze consequence — remove packs from the code entirely.** Packs were never produced (the writer
only ever stages `Blob`), so there is **no on-disk data** with `placement = 3` and nothing to stay
compatible with. We therefore delete all pack code — the `Placement::PackSlice` value, the
`ObjectKind::Pack` envelope kind, the pack `index_len`/`payloadOffset` logic, and the dead `PackSlice`
branches in `CasFsck`, `CasGc`, `CasStore`, `CasBuild`, `CasRootShardCodec`, `CasClosureWalk`,
`CasPlacement`, `CasTreeCodec`. **No reserved-but-unused slot** (YAGNI; a future use of value 3 would
be a new tree format version anyway, and no released bytes ever held it). B97, B10, and the B96
"snap_shards>1" tangent leave the release scope.

---

# Part II — The compatibility primitive

> ## ⭐ CONVERGED HEADER MODEL (2026-06-25) — supersedes the binary-framing-header description in the rest of Part II/III
>
> Every object — hashed or mutable — carries a **self-describing header** with the same three things,
> and every codec follows the same **write-down-to-floor** evolution discipline. Only the *wire
> encoding* of the header differs (because blobs are raw bytes and trees are hot-path binary).
>
> **The header trio (every object):**
> - `magic` — type id + sanity ("am I parsing what I expect"); the `CA__` ASCII family (`CABL` blob,
>   `CATR` tree, `CARS` manifest, `CAPM` pool-meta, `CAWM` watermark, `CAGT` gc-state, `CART`
>   retired-set, `CAHB` heartbeat, `CARR` roots-registry, `CAGO` gc-outcomes). No CRC (integrity comes
>   from S3 ETag + content-addressing for blob/tree; magic is sanity only).
> - `writer_version` — **forensic**: which build/generation wrote it.
> - `compatibility_version` — **functional**: the pool **floor** the writer targeted; the reader
>   dispatches its decode on it and **fail-closes** if `compatibility_version > G_build`.
>
> **Two encodings, one concept:**
> - **mutable objects → pure protobuf.** The trio lives in a `CasHeader` protobuf message embedded as
>   **field 1** of each object message — NO binary prefix, fully `protoc`-decodable (the portability
>   goal). `message CasHeader { fixed32 magic = 1; uint32 writer_version = 2; uint32 compatibility_version = 3; }`
> - **hashed objects (blob, tree) → binary header.** The trio sits in the envelope core as binary
>   fields (the 2b core already has the two u16 version slots — this is a rename of `min_reader_version`
>   → `compatibility_version` + the discipline below). Payload stays raw (blob) / binary catalog+inline
>   (tree); these are NOT protobuf (blob = raw file bytes, can exceed protobuf's 2 GB limit; tree = hot
>   read path + catalog-first random access). `build_id` in the incarnation zone is the forensic writer.
>
> **Write-down-to-floor ser/de discipline (documented in every codec):** the writer targets the
> negotiated `compatibility_version` (the pool floor = min over members) and emits the field-set valid
> at that level, so an old reader is **never handed something it would silently mis-skip**:
> ```
> serialize(compatibility_version):
>   if      (compatibility_version < 10)  emit the <10 field-set
>   else if (compatibility_version < 20)  emit the <20 field-set
>   else                                   emit the freshest
> ```
> - **Safe-additive change → no branch:** just add a new protobuf field number; old readers skip it.
>   This is the common case and needs no ceremony or floor-wait.
> - **Unsafe/replacing change (or new binary format) → a branch:** keep emitting the old-compatible
>   field-set until the floor rises (all members upgraded), then switch. The branch ladder is **bounded**
>   — prune the `<10` arm once the floor is permanently ≥ 10 (after a completed upgrade cycle).
> - For **trees**, the branch is identity-free: `treeId` is Merkle over logical children, so the same
>   logical tree at any `compatibility_version` dedups to one id.
>
> **Reader rule (both encodings):** check `magic` (mismatch → `CORRUPTED_DATA`); then
> `compatibility_version > G_build` ⇒ `UNKNOWN_FORMAT_VERSION` (fail-closed — for protobuf this is a
> cheap post-parse check, safe because parsing has no side effects and we gate *before using* the data);
> then interpret the body per `compatibility_version`.
>
> **Pool-level gate:** `pool-meta` carries `min_reader_generation`; a client **refuses to open the pool
> at startup** if `G_build < min_reader_generation` (clean "you're too old, upgrade" UX). Bumped only on
> a genuine breaking change, after all members upgraded. The per-object `compatibility_version` check is
> the defensive backstop; this startup gate is the primary, friendly one.
>
> **Consequences for what landed tonight (the rework, see the 2026-06-25 plan):** the `CasFormat`
> binary **framing-header** helpers (`writeFramingHeader`/`readFramingHeader`, the 8-byte prefix on the
> protobuf objects from 3a/3c) are **removed** — replaced by the `CasHeader` protobuf field; per-object
> `min_reader_version` is replaced by `compatibility_version`; `gateOnRead` becomes a post-parse
> compat check. The hashed envelope keeps its binary header (rename + discipline). The
> `max_content_addressable_pool_format` write-floor / roster stays **deferred** (pre-release:
> `compatibility_version` = the current generation, no branching yet).
>
> The subsections below (`writer_version`/`min_reader_version`, the framing header) are the prior
> iteration, kept for context; where they conflict, THIS box wins.

## Three numbers

- **`writer_version`** — the global format generation the object was written at.
- **`min_reader_version`** — the minimum generation a reader must understand to read this object
  **correctly**. The writer's promise: "any build knowing generation ≥ this reads me right."
- **`G_build`** — one compile-time constant per ClickHouse build: the highest generation it knows. A
  build keeps **every** decoder for `1 … G_build` (new code always reads old).

## The one reader rule

```
if  min_reader_version > G_build   →   fail-closed  (UNKNOWN_FORMAT_VERSION)
else                               →   read; ignore anything not recognized
```

Contract: **new always reads old** (all past decoders kept); **old usually reads new** (additive
changes leave `min_reader_version` untouched → old skips what's new); **old fails-closed on breaking**
(a breaking change bumps `min_reader_version` past the old build).

## Global generation numbering

One monotonic generation counter for the whole pool. When **any** object's format changes, the new
format takes `number = current_global_max + 1`; unchanged objects keep their old number.

- **No dense `level → {per-class version}` map.** Each class carries an append-only list of *its own*
  change-points on the global timeline (e.g. `tree: gen 1, gen 5`). To write at floor `G`, a class
  picks its newest format with generation ≤ `G`. Cost is `O(total changes ever)`, not `O(levels ×
  classes)`.
- **A build is one number** `G_build`. An object is readable iff `min_reader ≤ G_build`.
- **Version field is 2 bytes** (`uint16` LE) everywhere — part of the header bytes for binary objects;
  the extra byte is cheap insurance against the 256 ceiling.

## Additive vs breaking — worked examples

**Additive — bump `writer_version` only.** A future release adds an optional field (say
`compaction_hint`) to the manifest (protobuf). The new build knows generation 2 and writes
`(writer=2, min_reader=1)`: a generation-1 reader has `min_reader(1) ≤ G_build(1)` → reads it and
silently skips the unknown field number. "Old reads new."

**Breaking — bump both.** A future release changes the meaning of an existing tree-catalog field so an
old parser would misread it. The new build knows generation 3 and writes the tree header as
`(writer=3, min_reader=3)`: a generation-2 reader has `min_reader(3) > G_build(2)` → fail-closes with
`UNKNOWN_FORMAT_VERSION` instead of corrupting. "Old refuses new." Note: because tree **identity** is
Merkle (Part III), even this breaking serialization change does **not** rekey trees — `treeId` is
stable; the only effect is that old builds can't *parse* the new tree, which they correctly refuse.

The writer derives `min_reader` from a compiled per-class change-point table:

```
tree:      gen 1 → min_reader 1
manifest:  gen 1 → min_reader 1,  gen 2 → min_reader 1 (additive),  gen 4 → min_reader 4 (breaking)
gc_snap:   gen 1 → min_reader 1,  gen 3 → min_reader 1 (additive)
```

This table **replaces** the monotone `checkVersion(seen>current→fail)` (`CasCodecUtil.h:337`), which
conflates the two axes and wrongly rejects additive changes.

## How the one rule lands in each encoding

The concept (`format_id` + `writer_version` + `min_reader_version`) is uniform; the bytes differ:

- **Hashed binary (blob, tree).** All three live in the single object header (Part III), which is the
  **incarnation zone — outside the identity hash**. The header's version is the breaking axis
  (fail-closed on an unknown version); the TLV **critical bit** is the additive axis (a non-critical
  TLV an old reader doesn't know → skip; a critical one → fail-closed). Crucially, because identity is
  decoupled from serialization, a header/catalog version bump never rekeys the object.
- **Protobuf (manifest, gc-snap).** A tiny uniform framing header `[magic][writer:u16][min_reader:u16]`
  precedes the body, so the version check is **pre-parse** and identical for the single-message
  manifest and the **length-delimited streaming** gc-snap. Additive = a new field number (skip-unknown
  native). Discipline: no `map<>`, pinned field order — for **diffability** and golden tests, not
  correctness (never hashed).
  (There is no JSON branch — JSON is abandoned. `pool-meta` and the future `roster` are protobuf and use
  the same framing header as the manifest and gc-snap.)

---

# Part III — Concrete freezes (generation 1, irreversible after release)

## A hashed object = one header + payload

There are exactly **two** hashed object types — **blob** and **tree** — and they share **one** header.
There is no separate "envelope object"; "envelope" was just the name of that header.

```
blob:  [ 256-B header: "CABL" + ver + incarnation-zone ][ payload = raw file bytes ]
tree:  [ 256-B header: "CATR" + ver + incarnation-zone ][ catalog ][ inline-data section ]
```

- **One header for both.** A **fixed, tightly-packed LE core** (exact size fixed at freeze — *never
  grow it*; new fields go in TLVs) plus TLV extensions, padded to **`blob_header_len = 256`** for
  **both** blob and tree (was blob-only) — a constant payload offset, no per-object header read. The
  **magic distinguishes the type** (`CABL` = blob, `CATR` = tree), so the old `kind` enum byte is
  dropped; that byte and the former pack `index_len` slot are **reclaimed by shifting fields left — no
  vestigial pad**. (The header is stream-parsed via `ReadBuffer`, so serialized-field alignment is
  irrelevant; the interim pack-removal commit kept a byte-preserving zero pad, repacked away here.) The
  header is the **incarnation zone** — `domain_id`, `incarnation_tag`, `build_id`, optional
  `provenance`/`intended_ref` TLVs — and is **excluded from identity**.
- **Identity = content, computed by a frozen rule, independent of serialization:**
  - `blobId = H(payload)` — the raw file content hash (a Merkle leaf). The header also stores this id,
    which doubles as a bit-rot check on read.
  - `treeId = H( canonical( sorted-by-name entries: (name, kind, child_hash) ) )` — a **Merkle** hash
    over logical inputs only. `name` is length-prefixed (unambiguous framing); `kind ∈ {file,
    subtree}` is a domain-separation byte (RFC 6962 leaf/node separation; the absence of which caused
    Bitcoin's CVE-2012-2459). **No `size`** in identity (it is implied by `child_hash`; Git omits it,
    and IPFS's `Tsize` is a known reproducibility wart). **No `placement`** — an inline file and a
    standalone blob with the same content contribute the same `child_hash`, so inline↔blob never
    changes `treeId`. This is exactly Git's tree model (`mode`+`name`+`hash`, sorted, no size); the
    storage layer (inline/blob, like Git loose/packed) lives **below** the identity layer. Because the
    id is a **fold over the children**, a writer computes it **directly from the entries, without
    serializing the catalog** — a part's address (and a dedup/existence check) is known before, and
    independent of, laying out its bytes; serialization is a separate later step.
  - **Merkle rule frozen by convention.** Changing what enters the identity hash only loses dedup
    across the boundary (logically-identical trees get different ids → stored twice — a benign
    duplicate, never a correctness or readability problem). A reader never recomputes `treeId` to use a
    tree (it's an address from the manifest); only the writer computes it, with its current rule. So we
    do **not** stamp an `identity_scheme` field — it is documented as frozen and that is enough.
- **Hash domain freeze:** the blob hash is over the **payload**, not the header — otherwise a header
  TLV change would rekey every blob. Assert this in a test.

## Tree on-disk layout — catalog first, inline data last

The tree payload is `[catalog][inline-data]`, **not** interleaved:

- **catalog** (a compact, contiguous run): per entry — `name`, `kind`, `content_hash`, `size`,
  `placement`, and for `Inline` an `(offset, length)` into the data section. Sorted by name. A
  directory listing (`ls`) reads **only** the catalog and never touches the inline bytes.
- **inline-data section**: the concatenated bytes of the inline files. Reading one inline file is a
  seek to its `(offset, length)`. Per-file alignment is an **optional** nicety (serve as an aligned
  in-memory slice); the essential win is the catalog/data split.

`size`/`placement`/`offset`/`length` live in the catalog (for `ls`/stat without fetching a blob) but
are **outside** identity, so they evolve freely.

## The rest of generation 1

1. **Universal self-describing header** on every object: `format_id` + 2-byte `writer_version` + 2-byte
   `min_reader_version`, stamped **equal** (both = 1) until the first real change. Reserving
   `min_reader` now is the point — adding it to a frozen format later would itself be breaking. For the
   hashed objects this is carried in the 256-B header above; the magic is the `format_id`.
2. **Manifest.** Protobuf + the framing header. `ca_mtime` moves from the magic `.ca_mtime` string key
   in `RefPayload.mutable_files` to a **typed `RefPayload.published_at_ms`** field — done as part of
   generation 1 (pre-release, no migration). (B92 — fixing the adopt/relink `tree_size=0` — is a
   **separate iteration**, not this effort.)
3. **gc-snap → protobuf (B176) — DEFERRED (2026-06-24, unattended-run decision).** On review, gc-snap
   is **GC-internal durable state, not a cross-implementation interchange format** (a third party
   reimplementing CA reads the manifest/tree/blob, never another impl's GC snapshot), and its existing
   codec already satisfies the substantive goals: versioned (`GC_SNAP_VERSION` + magic, fail-closed),
   **zstd-compressed**, **deterministic** (ordered-map/-set iteration), and it carries the fold cursor
   (B140 fix). Converting this large, GC-correctness-critical codec to streaming protobuf is the
   highest-risk, lowest-interchange-value change in Plan 3 (a bug risks GC mis-deletion), and it is
   **already binary** — it is NOT part of the "abandon JSON" cleanup. So gc-snap **keeps its current
   versioned-binary+zstd codec**; the protobuf conversion is a deferred consistency-only follow-up
   (B176 stays open). The only genuine gap — not materializing the whole body for very large snaps
   (B165 OOM) — is a separate memory concern addressable within the binary codec, independent of
   protobuf. The consolidated `cas_format.proto` therefore covers the **interchange** mutable objects
   (manifest, pool-meta, watermark, roster); GC-internal `gc-state`/`retired-set` are converted off
   JSON for the cleanup but their protobuf is GC-local, not an interchange contract.
4. **Other mutable objects → protobuf** (hot-path rule): `gc-state` and `watermark` (tiny — framing
   header, single message); `retired-set` (length-delimited streaming like gc-snap, since it grows).
   These move off JSON because they are written per-GC-round / per-heartbeat.
5. **`pool-meta` → protobuf** (+ the framing header), and **delete the JSON codec family** — the
   `CasCodecUtil` `require*`/`checkNoUnknownKeys`/`parseJsonDocument`/`JsonObjectWriter` helpers and the
   now-dead `tolerateUnknownKeys` (Plan 1). The deferred `roster` will be protobuf when built. No JSON
   objects remain.
6. **S3 object user-metadata** (`ObjectMeta`/`HeadResult.attributes`): only for **optional/diagnostic**
   data cheaply read at HEAD (e.g. provenance), **never load-bearing** — `LocalObjectStorage` drops it
   (B167b).
7. **Error-code unification:** "future format version" and "unknown critical TLV" throw
   **`UNKNOWN_FORMAT_VERSION`** everywhere (the header decode currently throws `NOT_IMPLEMENTED`).

**Pre-freeze checklist (lock before first release):** the single header (magic set `CABL`/`CATR`, the
tightly-packed core size, `blob_header_len = 256` for both); version field width (2 B); the Merkle `treeId` rule
`(name, kind, child_hash)` and the blob-hash-over-payload domain; the catalog-first/inline-last tree
layout; packs fully removed (no reserved slot); the taxonomy; the `format_id` set; the one error code.

---

# Part IV — Deferred-but-designed: the rollout machinery

None of this lives in a frozen immutable object, so it is added **when the first breaking change ships
into a mixed-version cluster**, additively, touching no frozen bytes. Specified here so the door is
provably open.

- **Setting `max_content_addressable_pool_format`** (default = current `G_build`): an operator cap on
  the generation the pool may write — pin the old write-format after an upgrade (mixed-version safety,
  staged rollout). Parallels ClickHouse `compatibility` / `data_format_version`.
- **Durable roster** — one pool-global object `{members: {<server_id>: {path, G_build}}}`, **one GET**
  to read all members, CAS to update one's own entry (off the hot path). It is **membership, not
  liveness**: a member stays until *deliberately* removed; a server paused a day is not gone.
  Co-locate with `RootsRegistry` (already a mutable pool-global CAS object) — the right answer to
  "combine with the registry", vs. the watermark (per-server liveness, GC-facing, would force a
  fan-out read of every server's hot watermark). Keeper (B101) can host it later.
- **Write rule.** A server may write a format `V` iff `min_reader(V) ≤ floor` **and** `V ≤ setting`,
  where `floor = min(G_build)` over **roster members**. This is the whole point of the roster:
  **additive changes** (low `min_reader`) are safe immediately even in a mixed cluster; only **breaking
  changes** (`min_reader = V`) wait until every member's `G_build ≥ V`. No downgrade through a bump
  (immutable objects can't be rewritten) — record explicitly.
- **B200 — deliberate decommission.** Membership is durable, so removal is explicit: (a) default =
  never auto-remove for inactivity; (b) a deliberate path (`SYSTEM DROP CONTENT ADDRESSED POOL MEMBER
  <server_id>` and/or `clickhouse-disks`) drops a decommissioned server's roster entry; (c) advisory
  surfacing of long-absent members (watermark stale ≫ grace) so an operator can decide — never act
  automatically. A permanently-dead, never-decommissioned member pins the floor — the **safe** default
  (a one-way-stuck upgrade beats locking a returning server out of unreadable data).

**Why safe to defer:** in a single-version world every build writes `generation = G_build` and there
is nothing to negotiate; across versions, additive changes are always readable. The roster only ever
gates breaking changes, which do not exist until we make one.

---

# Part V — Code shape (the clean framework)

A small, focused module instead of per-codec ad-hoc version handling:

- **`CasFormat.h`** (new): the `format_id` set; the per-class append-only change-point table
  (`generation → min_reader`); `currentWriterVersion(class, write_floor)`; the single
  `gateOnRead(format_id, min_reader, G_build)` that replaces `checkVersion`; the framing-header
  read/write helpers (`[magic][writer:u16][min_reader:u16]`) for the protobuf class.
- **`CasCodecUtil.h`**: **delete the entire JSON codec family** (`JsonObjectWriter`, the
  `require*`/`requireU64`/`checkNoUnknownKeys`/`parseJsonDocument`/`decodeJsonGuarded` helpers) and the
  monotone `checkVersion` once their last caller is gone; the UInt128 LE/BE binary helpers + the
  `decodeGuarded`/`readFixedBytes` binary helpers stay. Also delete the now-dead `tolerateUnknownKeys`
  from `CasFormat` (Plan 1).
- **`CasEnvelope.{h,cpp}`** (the shared object header): one header for blob and tree; magic
  `CABL`/`CATR` (drop the `kind` enum); **repack the core hole-free** — reclaim the dropped `kind` byte
  and the former `index_len` zero pad by shifting fields left (set the new exact core size /
  `header_hash` offset); pad **both** to `blob_header_len = 256`; formalize the TLV critical bit as the
  additive axis; unify the future-format error to `UNKNOWN_FORMAT_VERSION`; assert the
  blob-hash-over-payload invariant.
- **`CasTreeCodec.{h,cpp}`**: **`treeId` becomes the Merkle rule** over `(name, kind, child_hash)`
  (replaces `CityHash128(encoded)`); the on-disk payload becomes catalog-first / inline-data-last; drop
  the `CATR` payload header (magic now lives in the shared header) and **delete** the `PackSlice`
  placement entirely; the writer chooses `Inline` for eager files below the threshold.
- **`CasRootShardCodec.{h,cpp}`**: framing header; `published_at_ms` typed field. (B92 `tree_size` fix
  is a separate iteration.)
- **`CasGcSnap.{h,cpp}`**: binary → streaming protobuf with the framing header.
- **One normative proto library — `Core/Proto/cas_format.proto`** (Plan 3): a single self-contained
  `proto3` file, one package (`clickhouse.cas.format`), defining **every** mutable-object message as a
  separate type — `RootShard`+`Journal` (merged from `cas_root_shard.proto`), `GcSnap`, `GcState`,
  `RetiredSet`, `Watermark`, `PoolMeta`, (future `Roster`). NOT one union message: each object is
  stored standalone and identified by its key + framing magic, so a `oneof` wrapper would only couple
  unrelated objects. No `import` of other ClickHouse protos, explicit field numbers, no `reserved`
  (pre-release), no `map<>` (deterministic field order — use `repeated` key-value sub-messages). The
  file header documents the two things protobuf can't express: the 8-byte framing header
  `[magic:4][writer:u16][min_reader:u16]` + each object's magic, and the writer/min_reader evolution
  contract. This makes the `.proto` the **portable normative spec** for the mutable side — a third
  party (Python/Java/Go) compiles one file to reimplement CAS reads/writes. (The binary/hashed formats
  — blob/tree envelope + the Merkle `treeId` rule — are normative in THIS design doc, cross-referenced
  from the proto header.)
- Part-writer (`ContentAddressedTransaction.cpp`): the inline-vs-blob placement decision for part
  files.
- **`CasBuild.{h,cpp}`** — two simplifications from the same fold over children. (1) **The `treeId` is
  computed directly from the collected entries — no serialize-the-catalog-then-hash step** (it used to
  hash `encodeTree(...)` output); the address is known before the bytes are laid out. **DONE in
  sub-plan 2a** (`stageTree` now uses `merkleTreeId(entries)`). (2) **The dependency closure falls out
  of that same enumeration** — `(name, kind, child_hash)` over the catalog *is* the dep set, so the
  per-placement dep-tracking branches (the `W-TREE-BUILD` loop + `deps` set) collapse into one walk,
  with packs already removed. This touches precommit-closure correctness (B188/B199), so it is
  **scheduled as sub-plan 2e, after the tree layout (2c) lands** — isolated from the format-freeze
  churn and reviewed on its own.

Each codec keeps **golden byte tests** (encode-stability) and a **cross-version read test**: write a
generation-1 object, assert a simulated `G_build = 0` reader fail-closes and a `G_build ≥ 1` reader
reads it. For the tree, add a **Merkle identity test**: the same logical tree with files inline vs. as
blobs, and with a different serialization, must produce the **same** `treeId`.

---

# Non-goals / out of scope

- **B164b** (journal-length bound) and **B147** (zstd object compression, decode cache) — cost items,
  tracked separately; they touch bytes only incidentally.
- Building any actual future-generation format beyond what this spec freezes. The framework makes
  future versions painless; this spec does not invent them.
- **B92** (adopt/relink `tree_size=0` fix) — a separate iteration, not this effort.
- Implementing the roster / setting / decommission (Part IV is designed, not built).

# Risks / open items

- **Eager-file set + inline threshold** tuning — measure tree size for wide tables; keep a large
  `primary.idx` as a blob if it bloats the hot tree GET.
- **gc-snap protobuf overhead** on the hot path — measure before committing (Part III.3).
- **Manifest protobuf determinism discipline** (no `map<>`, fixed field order) — for diffability and
  golden tests (the manifest is not hashed, so this is not a correctness risk); confirm the existing
  codec already satisfies it.
- **No data migration** is required (pre-release); any soak/PoC store is simply re-created.

# Verification

- Per-codec golden byte tests + round-trip; the cross-version read tests and the tree Merkle-identity
  test above.
- Full `Cas*`/`Ca*` gtest sweep stays green.
- A chaos soak after implementation (the standard `utils/ca-soak` 6 h run with periodic reports),
  confirming one-GET part open (inline) and the gc-snap streaming path under load.
