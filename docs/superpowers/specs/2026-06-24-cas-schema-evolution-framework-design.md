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
  └─ no  →  HOT (written/read per-operation or per-GC-round), OR large, OR structurally complex?
              ├─ yes →  PROTOBUF  (manifest + journal, gc-snap, gc-state, retired-set, watermark)
              └─ no  →  JSON      (control-plane only, touched rarely: pool-meta, roster)
```

JSON is reserved for the **rarely-touched control plane** — objects written once at creation or on a
rare control event, never per-operation. Everything on a hot path goes to protobuf even when it is
tiny (e.g. `gc-state`, `watermark`): the per-object protobuf overhead is trivial, and one mechanism
for all hot objects is cleaner than a size cutoff.

## Why each branch

- **Hashed ⇒ binary.** Dedup correctness does **not** force this (identity is now Merkle over logical
  inputs — Part III — so it is independent of serialization). The reasons are: (1) the hashed objects
  are the **hottest read path** (a tree is read on every part open) and binary parses fastest; (2) the
  tree object carries **raw inline payload** (small part files) in a catalog-first / data-last layout
  that is a natural custom-binary structure; (3) a blob is raw file bytes anyway. Determinism is a
  property we get for free from the Merkle rule, not a reason to pick binary.
- **Mutable + hot, large, or complex ⇒ protobuf.** Never hashed, so non-canonical wire output is
  harmless. Protobuf gives free **skip-unknown** (the engine of additive evolution) and avoids the two
  problems §4 found with JSON here: parse/serialize cost on the hot path, and unreadable codec code as
  the schema grows. **"Hot" (per-operation or per-GC-round) is a trigger on its own**, even for tiny
  objects — the §4 lesson was specifically that JSON on a hot path is a bottleneck.
- **Mutable, control-plane, rarely touched ⇒ JSON.** Only objects written at creation or on a rare
  control event (`pool-meta`, the deferred `roster`). They are the operator's incident surface —
  inspectable with plain S3 tooling — and small/simple, so JSON's cost never materializes.

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
- **JSON (pool-meta, roster).** Keys `"format"`, `"writer_version"`,
  `"min_reader_version"`. Unknown-key handling is **version-aware**: strict for a same-or-older object
  (`writer_version ≤ G_build` and an unknown key → `CORRUPTED_DATA`, preserving incident-surface
  safety), but unknown keys are **allowed when `writer_version > G_build`** (forward additions —
  ignore them). Any field addition bumps `writer_version`, so the two cases never overlap. Replaces the
  unconditional `checkNoUnknownKeys` + monotone `checkVersion`.

---

# Part III — Concrete freezes (generation 1, irreversible after release)

## A hashed object = one header + payload

There are exactly **two** hashed object types — **blob** and **tree** — and they share **one** header.
There is no separate "envelope object"; "envelope" was just the name of that header.

```
blob:  [ 256-B header: "CABL" + ver + incarnation-zone ][ payload = raw file bytes ]
tree:  [ 256-B header: "CATR" + ver + incarnation-zone ][ catalog ][ inline-data section ]
```

- **One header for both.** The 96-byte fixed LE core (frozen — *never grow the core*; new fields go in
  TLVs) plus TLV extensions, padded to **`blob_header_len = 256`** for **both** blob and tree (was
  blob-only) — a constant payload offset, no per-object header read. The **magic distinguishes the
  type** (`CABL` = blob, `CATR` = tree), so the old `kind` enum field is dropped. The header is the
  **incarnation zone** — `domain_id`, `incarnation_tag`, `build_id`, optional `provenance`/`intended_ref`
  TLVs — and is **excluded from identity**.
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
3. **gc-snap → protobuf** (B176): length-delimited **streaming** (never materialize the whole snap —
   B165 OOM), zstd compression (with B149), deterministic record ordering (golden-tested), preserving
   the fold cursor and the `GC_SNAP_VERSION` B140 fix. Measure protobuf overhead on the hot path.
4. **Other mutable objects → protobuf** (hot-path rule): `gc-state` and `watermark` (tiny — framing
   header, single message); `retired-set` (length-delimited streaming like gc-snap, since it grows).
   These move off JSON because they are written per-GC-round / per-heartbeat.
5. **JSON objects** (`pool-meta`, and the deferred `roster`): version-aware header; the rarely-touched
   control-plane surface, kept human-inspectable.
6. **S3 object user-metadata** (`ObjectMeta`/`HeadResult.attributes`): only for **optional/diagnostic**
   data cheaply read at HEAD (e.g. provenance), **never load-bearing** — `LocalObjectStorage` drops it
   (B167b).
7. **Error-code unification:** "future format version" and "unknown critical TLV" throw
   **`UNKNOWN_FORMAT_VERSION`** everywhere (the header decode currently throws `NOT_IMPLEMENTED`).

**Pre-freeze checklist (lock before first release):** the single header (magic set `CABL`/`CATR`, 96-B
core, `blob_header_len = 256` for both); version field width (2 B); the Merkle `treeId` rule
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
- **`CasCodecUtil.h`**: drop the monotone `checkVersion`; `parseJsonDocument` calls `gateOnRead` and
  applies the version-aware unknown-key rule; the UInt128 LE/BE helpers stay.
- **`CasEnvelope.{h,cpp}`** (the shared object header): one header for blob and tree; magic
  `CABL`/`CATR` (drop the `kind` enum, incl. `ObjectKind::Pack`); **remove the pack `index_len` and the
  pack branch of `payloadOffset`**; pad **both** to `blob_header_len = 256`; formalize the TLV critical
  bit as the additive axis; unify the future-format error to `UNKNOWN_FORMAT_VERSION`; assert the
  blob-hash-over-payload invariant.
- **`CasTreeCodec.{h,cpp}`**: **`treeId` becomes the Merkle rule** over `(name, kind, child_hash)`
  (replaces `CityHash128(encoded)`); the on-disk payload becomes catalog-first / inline-data-last; drop
  the `CATR` payload header (magic now lives in the shared header) and **delete** the `PackSlice`
  placement entirely; the writer chooses `Inline` for eager files below the threshold.
- **`CasRootShardCodec.{h,cpp}`**: framing header; `published_at_ms` typed field. (B92 `tree_size` fix
  is a separate iteration.)
- **`CasGcSnap.{h,cpp}`**: binary → streaming protobuf with the framing header.
- Part-writer (`ContentAddressedTransaction.cpp`): the inline-vs-blob placement decision for part
  files.
- **`CasBuild.{h,cpp}`** (follow-on, enabled by Merkle): the precommit part-build gets two
  simplifications from the same fold over children. (1) **The `treeId` is computed directly from the
  collected entries — no serialize-the-catalog-then-hash step** (today it hashes `encodeTree(...)`
  output); the address is known before the bytes are laid out. (2) **The dependency closure falls out
  of that same enumeration** — `(name, kind, child_hash)` over the catalog *is* the dep set, so the
  per-placement dep-tracking branches collapse into one walk ("collect each `Blob`/`Subtree`
  `child_hash`"), with packs already removed. Realize this after the tree codec lands.

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
