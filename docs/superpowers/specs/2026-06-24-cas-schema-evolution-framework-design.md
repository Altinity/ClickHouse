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
2. Replace the monotone `checkVersion` gate with a **`writer_version` + `min_reader_version`** model
   that distinguishes additive from breaking changes.
3. **Freeze**, at generation 1, every byte-level decision that is irreversible after release
   (self-describing headers, reserved extension slots, the hash domain, `blob_header_len`).
4. **Design — but defer** the cross-server rollout machinery (the pool-format setting, the durable
   roster, deliberate decommission), because none of it lives in the frozen bytes and none of it is
   needed until the first *breaking* change ships into a mixed-version cluster.

The guiding principle for what we build *now*: pay only for the **irreversible** byte-level
decisions; defer all consensus machinery until there is something to migrate *to*.

## Background — the objects and the current drift

CA persists these object classes:

| Object | Mutable? | Hashed (content-addressed)? | Today |
|---|---|---|---|
| blob payload (wrapped by an envelope) | no | **yes** — key *is* the payload hash | binary envelope + raw payload |
| tree (`"CATR"`) | no | **yes** — `treeId = CityHash128(encoded)` | custom binary |
| envelope (`"CHCA"`) | no | wraps a hashed object | binary: 96 B core + TLV, padded to `blob_header_len` |
| root-shard manifest + journal | yes | no | **protobuf** (`CODEC_VERSION=1`) |
| gc-snap | yes | no | custom binary (`GC_SNAP_VERSION`) |
| gc-state / cursor / retired-set | yes | no | strict JSON |
| pool-meta | yes (create-once fields) | no | strict JSON |
| watermark | yes | no | strict JSON |

The drift to fix: `CasCodecUtil.h` documents a *two-class* split (spec §4, 2026-06-11): **binary** for
hashed/identity objects, **strict JSON** for everything non-hashed. But the root-shard manifest is in
fact **protobuf** already (the `u128ToBytesBE` helpers exist *for* "the root-shard manifest's protobuf
`bytes` fields"), and the §4 experience was that **JSON is a hot-path bottleneck and gets ugly as the
object grows**. So in practice there are three encodings; this spec makes that official and gives a
sharp rule for which is which.

There is **no released CA data** yet (the PoC/soak stores are throwaway). Everything below *defines*
generation 1; it is not a migration of existing bytes.

---

# Part I — The encoding taxonomy

## The discriminator

```
Is the object content-addressed (its key is a hash of its own bytes)?
  ├─ yes →  canonical BINARY        (blob, tree, envelope)
  └─ no  →  large or structurally complex (nested/repeated, grows with the pool)?
              ├─ yes →  PROTOBUF    (root-shard manifest + journal, gc-snap)
              └─ no  →  JSON        (pool-meta, watermark, roster, gc-state, retired-set*)
```

`*` retired-set: JSON while it is a small bounded list; promote to protobuf only if profiling shows it
growing large (YAGNI — keep JSON now).

## Why each branch

- **Hashed ⇒ binary, and binary only.** A hashed object's *bytes are its identity* and its dedup
  key. Two servers that build the same logical tree (e.g. two replicas independently merging the same
  parts — the mechanism that *replaces* zero-copy replication) must emit **byte-identical** output, or
  they mint two objects for identical content and dedup silently misses. Canonical binary
  (name-sorted entries, explicit little-endian byte order) is reproducible by construction. Protobuf's
  wire output is **not** canonically guaranteed (field ordering, varint, `map<>` iteration,
  default/unknown handling), so it is a liability precisely on the hashed objects. Hashed objects are
  also the hottest read path.
- **Mutable + large/complex ⇒ protobuf.** These are never hashed, so non-canonical output is
  harmless. Protobuf gives free **skip-unknown** (the engine of additive evolution) and avoids the two
  problems §4 found with JSON on these objects: parse/serialize cost on the hot path, and unreadable
  encoder/decoder code as the schema grows.
- **Mutable + tiny/flat/rare ⇒ JSON.** These are the operator's incident surface — inspectable with
  plain S3 tooling. They are small and simple enough that JSON's cost and ugliness never materialize.

## Drop packs — inline the eager part files into the tree

The "open a part in ≈1 GET" goal (B10) and the "pack small files" item (B97) are **both** achieved by
**inlining**, with strictly less machinery than a `pack` object type would need (a new object kind, a
producer, a slice locator, a third GC edge kind).

The tree codec already supports `Placement::Inline` (file bytes embedded, entries name-sorted →
deterministic). Today the part writer does **not** use it — every part file is staged as
`Placement::Blob` (`ContentAddressedTransaction.cpp:560`), so a part with N files = N blobs = N GETs
to open. The fix:

- **Eager part-load files** — `checksums.txt`, `columns.txt`, `count.txt`, `serialization.json`,
  `metadata_version.txt`, `partition.dat`, `minmax_*.idx`, codec/TTL files, and small `primary.idx`:
  **inline into the tree**. They are small and always read at part open, so they ride the single tree
  GET. This *is* B10.
- **Lazy per-column data** — `.bin` and `.mrk`: **stay `Blob`**. ClickHouse loads marks lazily per
  column via `MarkCache`; a query touching 3 of 200 columns must fetch 3 blobs, not all 200. Packing
  these would destroy column selectivity — which is why inline is *better* than packs here, not merely
  simpler.

A size threshold drives inline-vs-blob (a large `primary.idx` may stay a blob); the exact threshold
and file list are a tuning detail, not a freeze decision.

**Freeze consequence:** `Placement` enum value `3` (`PackSlice`) is **permanently reserved and
unused** — never reassign the number. The dead `PackSlice` branches in `CasFsck`, `CasGc`, `CasStore`,
`CasBuild`, `CasRootShardCodec`, `CasClosureWalk`, `CasPlacement` are removed (a `PackSlice` value seen
on read is `CORRUPTED_DATA` — it can never legitimately exist). B97, B10 and the B96 "snap_shards>1"
tangent leave the release scope.

Trees now carry payload bytes (the inlined eager files), which *reinforces* the "binary, like blobs"
choice and its determinism requirement.

---

# Part II — The compatibility primitive

## Three numbers

- **`writer_version`** — the global format generation the object was written at. "I was produced by
  generation-N code."
- **`min_reader_version`** — the minimum generation a reader must understand to read this object
  **correctly**. A writer's promise: "any build knowing generation ≥ this reads me right."
- **`G_build`** — a single compile-time constant per ClickHouse build: the highest generation it
  knows. A build keeps **every** decoder for generations `1 … G_build` (new code always reads old).

## The one reader rule

```
if  min_reader_version > G_build   →   fail-closed  (UNKNOWN_FORMAT_VERSION)
else                               →   read; ignore anything not recognized
```

This yields the contract:

- **New always reads old** — all past decoders are kept forever.
- **Old usually reads new** — additive changes leave `min_reader_version` unchanged, so the old
  reader skips what is new.
- **Old fails-closed on breaking** — a breaking change bumps `min_reader_version` past the old build;
  it refuses rather than misreading.

## Global generation numbering

One monotonic generation counter for the whole pool. When **any** object's format changes, the new
format takes `number = current_global_max + 1` and the global max bumps; unchanged objects keep their
old number. Consequences:

- **No dense `level → {per-class version}` map to maintain.** Each object class carries an
  append-only list of *its own* change-points on the global timeline (e.g. `tree: gen 1, gen 5`). To
  write at floor `G`, a class picks "my newest format with generation ≤ `G`". Cost is `O(total
  changes ever)`, not `O(levels × classes)`.
- **A build is one number** `G_build`. An object is readable iff `min_reader ≤ G_build`.
- **`min_reader` is unchanged by this** — it remains the per-object additive/breaking axis. Global
  numbering only governs how the *writer* version is allocated.
- **Version field is 2 bytes** (`uint16`, little-endian) everywhere — it is part of the hashed bytes
  for binary objects, hence frozen; the extra byte is cheap insurance against the 256 ceiling.

## Additive vs breaking — worked examples

**Additive — bump `writer_version` only.** Suppose a future release adds an optional field (say a
`compaction_hint`) to the manifest (protobuf). The new build knows generation 2 and writes the
manifest as `(writer=2, min_reader=1)`: a generation-1 reader has `min_reader(1) ≤ G_build(1)` → reads
it and silently skips the unknown field number. "Old reads new."

**Breaking — bump both.** Change the meaning/layout of an existing tree field so the old decoder would
read garbage. New build knows generation 3 and writes the tree as `(writer=3, min_reader=3)`: a
generation-2 reader has `min_reader(3) > G_build(2)` → fail-closes with `UNKNOWN_FORMAT_VERSION`
instead of corrupting. "Old refuses new."

The writer derives `min_reader` from a compiled per-class table of change-points:

```
tree:      gen 1 → min_reader 1
manifest:  gen 1 → min_reader 1,  gen 2 → min_reader 1 (additive),  gen 4 → min_reader 4 (breaking)
gc_snap:   gen 1 → min_reader 1,  gen 3 → min_reader 1 (additive)
```

This table **replaces** the monotone `checkVersion(seen>current→fail)` in `CasCodecUtil.h:337`, which
conflates the two axes and wrongly rejects additive changes.

## How the one rule lands in each encoding

The concept (`format_id` + `writer_version` + `min_reader_version`) is uniform; the bytes differ:

- **Hashed binary (tree, envelope-wrapped blob).** Magic + 2-byte version inside the hashed bytes.
  Raw binary cannot skip-unknown, so the **default is breaking** → `min_reader == writer`; a new
  version = a new decoder = a new hash, old objects immutable and still readable. The **envelope is
  the additive axis for the whole hashed family**: its TLV **critical bit** *is* a per-extension
  `min_reader`. A non-critical TLV an old reader doesn't know → skip (additive). A critical TLV it
  doesn't know → fail-closed (breaking). So a hashed object evolves either breaking-by-version (rare,
  rollout-gated) or additive-by-non-critical-TLV (the envelope's common case: `provenance`,
  `intended_ref`).
- **Protobuf (manifest, gc-snap).** A tiny uniform framing header `[magic][writer:u16][min_reader:u16]`
  precedes the body. The header (not in-message fields) keeps the version check **pre-parse** and
  identical for the single-message manifest *and* the **length-delimited streaming** gc-snap. Additive
  = a new field number (skip-unknown). Discipline: no `map<>`, pinned field order — for **diffability**
  and stable golden tests, not correctness (never hashed).
- **JSON (pool-meta, watermark, roster, gc-state).** Keys `"format"`, `"writer_version"`,
  `"min_reader_version"`. Unknown-key handling is **version-aware**: strict for a same-or-older object
  (`writer_version ≤ G_build` and an unknown key → `CORRUPTED_DATA`, preserving the incident-surface
  safety), but unknown keys are **allowed when `writer_version > G_build`** (those are forward
  additions — ignore them). Since any field addition bumps `writer_version`, the two cases never
  overlap. This replaces the unconditional `checkNoUnknownKeys` + monotone `checkVersion`.

---

# Part III — Concrete freezes (generation 1, irreversible after release)

These are the byte-level decisions that **cannot** change post-release without an old-data read path.
All are defined as generation 1.

1. **Universal self-describing header.** Every object carries `format_id` + 2-byte `writer_version` +
   2-byte `min_reader_version`, stamped **equal** (both = 1) until the first real change. Reserving
   `min_reader` now is the point — adding it to a frozen format later would itself be breaking.
2. **Envelope.** 96-byte fixed LE core (frozen — *never grow the core*), TLV extensions with the
   critical/non-critical bit, padded to **`blob_header_len = 256`** (per-pool, set at creation,
   immutable after) — ~160 B of TLV room plus a constant payload offset. **The blob content hash is
   over the payload, not the envelope** — otherwise any TLV addition rekeys every blob. This is a
   freeze prerequisite; assert it in a test.
3. **Tree.** Canonical binary, magic `"CATR"`, 2-byte version, entries name-sorted byte-wise. Inlines
   the eager part files (Part I). `Placement` value `3` (`PackSlice`) permanently reserved-unused.
4. **Manifest.** Protobuf + the framing header. `ca_mtime` moves from the magic `.ca_mtime` string key
   in `RefPayload.mutable_files` to a **typed `RefPayload.published_at_ms` protobuf field** — done
   **as part of generation 1** (pre-release, so there is no migration; we simply define the gen-1
   manifest with the typed field). Post-release, the same kind of field addition would be the additive
   case from Part II (ship at generation N+1, `min_reader` unchanged, older readers skip it). (B92:
   also carry `tree_size` correctly on the adopt/relink wire so the field stops being written as 0.)
5. **gc-snap → protobuf** (B176): length-delimited **streaming** (never materialize the whole snap —
   B165 OOM), zstd compression (with B149), deterministic record ordering (golden-tested), preserving
   the fold cursor and the `GC_SNAP_VERSION` B140 fix. Measure protobuf overhead on the hot path.
6. **JSON objects** (pool-meta, watermark, roster, gc-state): adopt the version-aware header; otherwise
   unchanged.
7. **S3 object user-metadata** (`ObjectMeta`/`HeadResult.attributes`): usable only for
   **optional/diagnostic** data cheaply read at HEAD (e.g. provenance), **never load-bearing** — it is
   dropped by `LocalObjectStorage` (B167b).

**Pre-freeze checklist (must be locked before the first release):** version field width (2 B);
`blob_header_len` (256); blob hash domain (payload); envelope core size (96 B) and the TLV critical
bit; `PackSlice` reserved; the taxonomy (which object is which encoding); the `format_id` set.

---

# Part IV — Deferred-but-designed: the rollout machinery

None of this lives in a frozen immutable object, so it is added **when the first breaking change ships
into a mixed-version cluster**, additively, touching no frozen bytes. It is specified here so the door
is provably open.

- **Setting `max_content_addressable_pool_format`** (default = current `G_build`): an operator cap on
  the generation the pool may write. Lets a user pin the old write-format after an upgrade
  (mixed-version safety, staged rollout). Parallels ClickHouse `compatibility` / `data_format_version`.
- **Durable roster** — a single pool-global object `{members: {<server_id>: {path, G_build}}}`, **one
  GET** to read all members, CAS to update one's own entry (off the hot path). It is **membership, not
  liveness**: a member stays until *deliberately* removed; a server paused for a day is not gone.
  Co-locate with `RootsRegistry` (already a mutable pool-global CAS object) — this is the right answer
  to "combine with the registry", as opposed to the watermark (which is per-server liveness, GC-facing,
  and would force a fan-out read of every server's hot watermark). Keeper (B101) can host it later.
- **Write rule.** A server may write a format `V` iff `min_reader(V) ≤ floor` **and** `V ≤ setting`,
  where `floor = min(G_build)` over **roster members**. This is the whole point of the roster:
  **additive changes** (low `min_reader`) are safe immediately, even in a mixed cluster, so they need
  no coordination; only **breaking changes** (`min_reader = V`) must wait until every member's
  `G_build ≥ V`. No downgrade through a bump (immutable objects can't be rewritten) — record
  explicitly.
- **B200 — deliberate decommission.** Membership is durable, so removal is explicit: (a) default =
  never auto-remove for inactivity; (b) a deliberate path (`SYSTEM DROP CONTENT ADDRESSED POOL MEMBER
  <server_id>` and/or `clickhouse-disks`) drops a decommissioned server's roster entry; (c) advisory
  surfacing of long-absent members (watermark stale ≫ grace) so an operator can decide — never act
  automatically. A permanently-dead, never-decommissioned member pins the floor — the **safe** default
  (a one-way-stuck upgrade beats locking a returning server out of unreadable data).

**Why this is safe to defer:** in a single-version world every build writes `generation = G_build` and
there is nothing to negotiate; and even across versions, additive changes are always readable. The
roster only ever gates breaking changes, which do not exist until we make one.

---

# Part V — Code shape (the clean framework)

A small, focused module instead of per-codec ad-hoc version handling:

- **`CasFormat.h`** (new): the `format_id` enum; the per-class append-only change-point table
  (`generation → min_reader`); `currentWriterVersion(class, write_floor)`; and the single
  `gateOnRead(format_id, min_reader, G_build)` that replaces `checkVersion`. The framing-header
  read/write helpers (`[magic][writer:u16][min_reader:u16]`) for the protobuf class.
- **`CasCodecUtil.h`**: drop the monotone `checkVersion`; `parseJsonDocument` calls `gateOnRead` and
  applies the version-aware unknown-key rule; the UInt128 LE/BE helpers stay.
- **`CasEnvelope.{h,cpp}`**: formalize the TLV critical bit as the additive axis; assert the
  hash-over-payload invariant; `blob_header_len = 256`.
- **`CasTreeCodec.{h,cpp}`**: 2-byte version; remove `PackSlice` (reserve value 3); the writer chooses
  `Inline` for eager files below the threshold.
- **`CasRootShardCodec.{h,cpp}`**: framing header; `published_at_ms` typed field (gen 2);
  `tree_size` on adopt/relink (B92).
- **`CasGcSnap.{h,cpp}`**: binary → streaming protobuf with the framing header.
- Part-writer (`ContentAddressedTransaction.cpp`): inline-vs-blob placement decision for part files.

Each codec keeps **golden byte tests** (encode-stability) and a **cross-version read test**: write a
generation-1 object, then assert a simulated `G_build = 0` reader fail-closes, and a `G_build ≥ 1`
reader reads it; for the additive `published_at_ms` case, assert a generation-1 reader reads a
generation-2 manifest and ignores the field.

---

# Non-goals / out of scope

- **B164b** (journal-length bound) and **B147** (zstd object compression, decode cache) — cost items,
  tracked separately; they touch bytes only incidentally.
- Building any actual generation-2 format beyond the `published_at_ms` worked example (and `tree_size`
  fix). The framework makes future versions painless; this spec does not invent them.
- Implementing the roster / setting / decommission (Part IV is designed, not built).

# Risks / open items

- **Protobuf determinism discipline** (no `map<>`, fixed field order) for the manifest/gc-snap — needed
  for diffability and golden tests, not correctness; confirm the existing manifest codec already
  satisfies it.
- **Eager-file set + inline threshold** tuning — measure tree size for wide tables; keep large
  `primary.idx` as a blob if it bloats the hot tree GET.
- **gc-snap protobuf overhead** on the hot path — measure before committing (Part III.5).
- **No data migration** is required (pre-release); if any soak/PoC store must be read after the freeze,
  it is simply re-created.

# Verification

- Per-codec golden byte tests + round-trip; the cross-version read tests above.
- Full `Cas*`/`Ca*` gtest sweep stays green.
- A chaos soak after implementation (the standard `utils/ca-soak` 6 h run with periodic reports),
  confirming one-GET part open (inline) and the gc-snap streaming path under load.
