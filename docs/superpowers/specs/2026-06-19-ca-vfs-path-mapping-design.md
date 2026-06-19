# CAS Disk — Path-Mirroring Layout & Browsable Introspection (Design) {#title}

- **Status:** Draft for review
- **Date:** 2026-06-19
- **Branch:** `cas-mergetree-poc`
- **Author:** design dialogue (operator + assistant)
- **Reviewers:** intended for independent review in a fresh session / by another model

> **Reviewer orientation.** This document is self-contained. It describes a *bounded* refactor of
> the content-addressed (CA) MergeTree storage that changes **only how ClickHouse paths map to S3
> keys** and **where per-server control objects live**, so that the pool mirrors the ClickHouse
> directory tree (browsable with ordinary tools, navigable with the stock `clickhouse-disks` verbs)
> and a server's data is one self-contained subtree. It is designed to **not touch** the
> content-addressed object store, the garbage collector's algorithm, the manifest/journal format,
> the fence/retire protocol, or any TLA+ model. §4 (Invariants Preserved) is the safety contract;
> the proposal is valid only insofar as it respects it.

---

## 1. Background {#background}

The CA disk is a `DiskObjectStorage` whose metadata layer (`ContentAddressedMetadataStorage` +
`ContentAddressedTransaction`, "the wiring") maps ClickHouse disk paths onto a content-addressed
object pool in S3. The pool has two parts:

- **Immutable, content-addressed objects** (global, deduplicated): `blobs/` (file bytes), `trees/`
  (directory listings), `packs/` (packed small files). Addressed by a 128-bit content hash.
- **Mutable state**, organized under **namespaces**. A `RootNamespace` is an *opaque string* — the
  core never interprets it. Under each namespace live:
  - **root-shard manifests** `roots/<ns>/<N>` — protobuf objects (`N` numeric), each holding many
    **refs** (`ref_name → {tree_id, mutable_files}`) plus a journal. A **ref** is a mutable named
    pointer to an immutable tree; `mutable_files` is a tiny inline overlay of per-ref mutable files.
  - **verbatim files** `roots/<ns>/_files/<name>` — plain, name-keyed, mutable objects.

The **durability core** — GC, the manifest/journal CAS protocol, the per-server watermark, the
build-root precommit machinery, the in-degree snapshots, the namespace registry, and the
fence/retire ordering — has been modeled in TLA+ and validated under chaos soak. **Its algorithms
and formats are out of scope to change here.**

### 1.1 Current layout {#current-layout}

```
POOL/
  blobs/<2hex>/<id>   trees/<2hex>/<id>   packs/<2hex>/<id>   ← content (global, deduplicated)
  roots/
    _registry                          ← authoritative namespace list (GC discovery; CAS-appended)
    <server>/<uuid>/<N>                ← live table manifests (ns = "<server>/<uuid>", ref = part)
    <server>/<uuid>/_files/<name>      ← table verbatim files (format_version.txt, mutation_*, …)
    <server>/detached/<uuid>/<N>       ← detached parts (separate namespace; see P-3)
    <server>/_disk/_files/<name>       ← loose disk files (the startup write probe)
    shadow/<backup>/store/<u3>/<uuid>/<N>   ← FREEZE snapshots (path-shaped namespace already)
  servers/<2hex>/<server_hex>          ← per-server watermark (one object, read by GC by key)
  _builds/<server_hex>/<N>             ← per-server build-root precommits (a namespace)
  gc/state   gc/snap/<gen>/<shard>   gc/hb                   ← GC's own state
  _pool_meta
```

The wiring maps a ClickHouse path to a `(namespace, ref, file)` triple via `parsePartFilePath` +
`route`, special-casing shadow / detached / projection / table-uuid / generic forms.

### 1.2 Worked example (current) {#current-example}

Table `mydb.events`, uuid `3f2a…0001` (Atomic layout), part `all_1_1_0`:

| ClickHouse path | Current S3 location |
|---|---|
| `store/3f2/3f2a…0001/all_1_1_0/data.bin` | bytes at `blobs/9a/9a3f…`; named in part tree `trees/c1/c1d0…`; tree bound to ref `all_1_1_0` in manifest `roots/srv1/3f2a…0001/<N>` |
| `store/3f2/3f2a…0001/all_1_1_0/txn_version.txt` | inline in that manifest, ref `all_1_1_0`'s `mutable_files` |
| `store/3f2/3f2a…0001/format_version.txt` | `roots/srv1/3f2a…0001/_files/format_version.txt` |

---

## 2. Goals {#goals}

The CA disk should behave **(almost) like a normal disk**, with ClickHouse knowledge confined to the
wiring.

- **G1 — Path-mirroring layout.** The S3 key for a table's mutable state mirrors the ClickHouse disk
  path, so the pool is browsable with ordinary tools and legible without a decoder.
- **G2 — Simple, near-identity CH→CAS→S3 mapping.** The wiring's path→namespace mapping becomes a
  thin, mechanical transform rather than a thick classifier; `parsePartFilePath`/`route` shrink.
- **G3 — Self-describing boundary.** From a key alone it is obvious where "ordinary path" ends and
  "content-addressed archive" begins — no object probing, no GC state consulted.
- **G4 — Server-scoped data is one subtree.** All of a server's mutable state lives under a single
  `roots/<server>/` subtree, so "drop a server" is one subtree delete (+ small index prune + GC
  reclaim) rather than collecting data across several global prefixes.
- **G5 — Navigable introspection.** `clickhouse-disks`, pointed at the already-configured CA disk,
  navigates it with the **stock** `cd`/`ls`/`read` verbs (the CA metadata storage already implements
  the `IMetadataStorage` traversal surface those use). No new `ca-*` verbs.
- **G6 — Terminology.** Remove "part" vocabulary from the *generic* CA layer (part-awareness is a
  wiring property, not a store property); name the VFS contract.
- **G7 — Self-describing pool identity** *(acknowledged, deferred — see §6 D7 / backlog B180)*. From
  the pool alone, tell which `database.table` a directory is. Out of scope here (needs the
  name/`CREATE` plumbed from the storage layer); recorded so its absence is intentional.

---

## 3. Problems with the current design {#problems}

- **P-1 (opaque layout).** `roots/<server>/<uuid>/…` does not mirror the ClickHouse path; the pool
  is not browsable and needs manifest decoders to understand. (Blocks G1.)
- **P-2 (thick mapping).** `parsePartFilePath` + `route` carry shadow/detached/projection/table-uuid/
  generic special cases — ClickHouse knowledge spread across the wiring. (Blocks G2.)
- **P-3 (namespace-variant hacks).** With no notion of a *path*, ClickHouse concepts are faked with
  namespace conventions or name tricks: `_disk` (a magic namespace for "the disk root"),
  `detached/<uuid>` (a *parallel namespace* so a detached part can share a name with a live part
  without colliding), and projections (a `<proj>.proj/` **name prefix** in tree entries). Three
  workarounds for one missing feature — directories. (Blocks G1/G2.)
- **P-4 (no boundary marker).** Nothing in a key says "content-addressed content starts here," so
  navigation needs decoders or per-segment probing. (Blocks G3.)
- **P-5 (server data is scattered).** A server's footprint spans `roots/<server>/…` (refs),
  `_builds/<server_hex>/…` (precommits), `servers/<2hex>/<server_hex>` (watermark), plus its entries
  in the shared `roots/_registry`. Dropping a server means handling several global prefixes.
  (Blocks G4.)
- **P-6 (introspection gaps).** `clickhouse-disks` has only `fsck`/`ca-gc-dryrun`; the existing
  `IDisk` traversal presents the part-aware ClickHouse view and the layout it walks is opaque.
  (Blocks G5.)

---

## 4. Invariants preserved (the safety contract) {#invariants}

This refactor **must not** change:

- **N1** — the content-addressed object store: `blobs/`/`trees/`/`packs/` key scheme and hashing.
- **N2** — the GC *algorithm*: reachability, fence rounds, retire/epoch protocol, in-degree
  snapshots, the GC scheduler.
- **N3** — the root-shard **manifest + journal** format and its CAS publish protocol.
- **N4** — the **watermark** semantics (W-ANCHOR: durable before first PUT) and the **precommit /
  build-root** protocol (build-root-before-adopt ordering; the fenced fold + abandoned-precommit
  reclaim). Their *keys may move* (§5.4); their *protocol and ordering may not*.
- **N5** — the **registry** mechanism (CAS-append on `W-REGISTER`, GC fencing, authoritative
  discovery). Its *key may move* (`roots/_registry` → `gc/registry`); the mechanism may not.
- **N6** — the mutable-overlay model (`RefPayload.mutable_files`) and the ref/tree/blob data model.
- **N7** — the TLA+ models and their proven invariants: no re-floor, no re-model.

**Why this is achievable.** `RootNamespace` is *opaque to the core* (`CasIds.h`: "the core never
interprets its contents"). `tryParseRootShardKey` classifies a manifest purely by a *numeric tail*,
and `checkNamespace` only forbids empty / reserved segments. So the namespace *string* is free to
encode the ClickHouse path, and GC keeps working unchanged. `shadow/...` namespaces already exercise
path-shaped namespaces in production. The changes below are therefore (a) namespace-string
composition, (b) `Cas::Layout` key construction for a few control objects read *by key*, and (c)
adjusting the wiring's existing methods — never the GC algorithm or the manifest/journal protocol.

---

## 5. Proposal {#proposal}

Target layout:

```
POOL/
  blobs/  trees/  packs/                        ← content (global, immutable, deduplicated)
  roots/                                        ← DATA ONLY (server mountpoints; CH paths mirrored)
    <server>/
      store/<u3>/<uuid>@cas@/<N>                ← a table: a content-addressed archive (@cas@)
      store/<u3>/<uuid>@cas@/_files/<name>      ← that table's verbatim files (format_version, …)
      detached/store/<u3>/<uuid>@cas@/<N>       ← detached parts (sibling archive, no collision)
      _precommits/<N>                           ← this server's in-flight build-root precommits
      _watermark                                ← this server's watermark (read by GC by key)
      <plain path>                              ← loose non-CAS files (e.g. the write probe)
    shadow/<backup>/store/<u3>/<uuid>@cas@/<N>  ← FREEZE snapshots
  gc/                                           ← DISCOVERY + GC STATE
    registry                                    ← authoritative namespace list (was roots/_registry);
                                                  ALSO discovers precommit (build-root) namespaces, as today
    state   snap/<gen>/<shard>   hb             ← GC's own state (unchanged)
  _pool_meta
```

**Naming convention:** reserved/special folders that sit *among* data carry a leading underscore
(`_precommits`, `_watermark`, `_files`) — matching the existing `_files`/`_registry`/`_pool_meta`
convention — so they never collide with a real ClickHouse path segment (`store`/`data`/`shadow`/…).
The `@cas@` token is **not** a folder; it is a *suffix* on a directory name (§5.1).

### 5.1 Path-mirroring namespaces + the `@cas@` boundary {#path-mirroring}

Compose namespace strings so the namespace **is** the ClickHouse disk path of the CA directory, with
the content-addressed boundary marked by a **suffix on the table-dir segment**: `…/<uuid>@cas@`.

Why a suffix, not a `/@cas@` segment: a slash is a *path separator*, so the marker should not
masquerade as its own directory. `@cas@` reads like a **file extension** — the directory is a
content-addressed *archive*, and the part path lives *inside* it, exactly like a path within a `.zip`
(`foo.zip/inner/file`). `@` is S3-safe and never occurs in ClickHouse uuids, part names, detached
prefixes, projection names, or column files, so it cannot collide with real path data.

| Logical area | Namespace string |
|---|---|
| live table | `<server>/store/<u3>/<uuid>@cas@` |
| detached parts | `<server>/detached/store/<u3>/<uuid>@cas@` |
| FREEZE shadow | `shadow/<backup>/store/<u3>/<uuid>@cas@` |

Refs, manifests, `_files`, and the overlay are **unchanged** — only the namespace string they sit
under changes. The worked example becomes:

| ClickHouse path | New S3 location |
|---|---|
| `store/3f2/3f2a…0001/all_1_1_0/data.bin` | `blobs/9a/9a3f…`; part tree `trees/c1/c1d0…`; ref `all_1_1_0` in manifest `roots/srv1/store/3f2/3f2a…0001@cas@/<N>` |
| `…/all_1_1_0/txn_version.txt` | inline in that manifest, ref `all_1_1_0`'s `mutable_files` |
| `store/3f2/3f2a…0001/format_version.txt` | `roots/srv1/store/3f2/3f2a…0001@cas@/_files/format_version.txt` |

The wiring's mapping (G2) collapses to **"prefix the server id, insert `@cas@` at the table-dir
boundary"** on the way down, strip it on the way up. The ref (= part name) and in-tree-file split
remain in the wiring, but the namespace composition is now a mechanical path transform.
`parsePartFilePath` shrinks to "find the table-dir boundary"; detached/shadow stop being special
namespace *shapes* and become ordinary sibling paths.

**Self-describing boundary (G3):** the `@cas@` token is *in the key*, so any consumer can see where
content-addressing begins by inspecting names — no probing. The wiring knows the boundary
structurally (the table-uuid level); a consumer without ClickHouse knowledge discovers it from the
`@cas@` token appearing in a listing.

### 5.2 Eliminate `_disk`: loose files are plain mountpoint objects {#eliminate-disk}

Non-CAS, loose disk files (the startup write probe; anything written outside a table archive) are
stored as **plain objects in the mountpoint at their mirrored path** — `putObject(roots/<server>/<path>)`
— with no namespace and no `_files` wrapper. The `genericNamespace()` → `_disk/_files/…` indirection
is removed.

This is **wiring-only and GC-safe**: GC deletes only content (`blobs/`/`trees/`/`packs/`) and
enumerates only **registered** namespaces (from the registry), so a plain mountpoint object is never
scanned for deletion (it is owned by its path and removed only by `removeFile`). The only code change
is `ContentAddressedTransaction::writeFile`'s non-table branch.

> **Invariant — root-shard parsing is `@cas@`-scoped (must be encoded in the implementation).**
> Root-shard discovery/parsing under `roots/` applies **only inside `@cas@` archive directories**.
> A plain mountpoint object with no `@cas@` ancestor is an **opaque ordinary file** and is **never**
> passed to root-shard classification. This is required because today `tryParseRootShardKey` is
> broader — it treats *any* numeric tail under `roots/` as a shard, so `roots/srv1/foo/7` would be
> mis-read as namespace `srv1/foo`, shard `7`. The implementation must gate shard-key classification
> on the `@cas@` boundary (e.g. only classify keys whose namespace segment ends in `@cas@`), so the
> grammar is correct for *all* consumers — GC, `fsck`, raw tooling, future listing code — not just
> registry-driven GC. With this invariant, loose files at `roots/<server>/<path>` are unambiguous.

After this there are exactly **two verbatim-file locations**, both "plain objects at a path":
(1) **loose in the mountpoint** (was `_disk`), and (2) **`_files` inside a `@cas@` archive** — the
table bookkeeping (`format_version.txt`, `mutation_*.txt`, dedup logs) at `…/<uuid>@cas@/_files/<name>`.

**Caveat (tolerable):** files inside `@cas@` are content-addressed (immutable, deduplicated, shared
— no in-place hardlink/rename); files outside are plain objects (ordinary ops). `@cas@` is exactly
the line between "deduplicated immutable content" and "ordinary file." Loose files need no
hardlinks, so the difference is acceptable.

### 5.3 `roots/` is data; `gc/` is discovery + state {#roots-vs-gc}

`roots/` holds **server data only**. Discovery indexes are infrastructure, not data, so they move
to `gc/`:

- **Registry** `roots/_registry` → **`gc/registry`**. It is read by GC (the authoritative CAS-dir
  universe for the reachability fold) and CAS-appended by writers on first publish (`W-REGISTER`).
  The **browse path no longer needs it**: with path-mirroring, enumerating namespaces is a *scoped
  `LIST`* of the mirrored subtree (the natural "ls" of a mirrored layout) — its authoritative-ness
  matters only to GC (a miss → over-reclaim), and browse tolerates a loose LIST (it re-checks
  `listRefs` before showing an entry). GC keeps reading the compact registry (a full every-round
  LIST would be too costly); browse does a bounded, occasional LIST. The dependency genuinely
  splits, so the registry becomes GC-only infrastructure. Small wiring change: replace the
  `listNamespaces`-based shadow enumeration in `listDirectory` with a scoped LIST.
- **Precommit (build-root) discovery** stays on the relocated **`gc/registry`** — no separate index.
  Build-root namespaces are CAS-registered in the registry today and discovered through it; relocating
  their data (§5.4) does not change that. A dedicated `gc/precommits` index was considered and
  rejected: it is net-new fenced-discovery machinery (an N2/N5 risk) for no functional gain.

This is a **bounded, Layout-only move**: the registry is read/written *by key*
(`Layout::rootsRegistryKey`), GC never LISTs `roots/` to find it, and the CAS-append + fence
**mechanism is unchanged** — N5 respected. Bonus cleanup: `tryParseRootShardKey`/`checkNamespace`
no longer need the `_registry`-under-`roots/` reservation. GC discovery cost is unchanged (the
namespace strings are merely longer; counts and per-namespace listings are identical).

### 5.4 Server-scoped data under the mountpoint {#server-data}

Bring a server's mutable control state under its own `roots/<server>/` subtree so the whole
footprint is one subtree (G4):

**Contract — one canonical `<server>` token.** `<server>` is the single canonical server identity,
used *identically* for the mountpoint (`roots/<server>/…`), the watermark key, and the precommit
owner. Reclaim must derive the **same** server identity it uses to locate the watermark; because the
precommit data and the watermark sit under the *same* `roots/<server>/` segment, that identity is
read directly from the shared `<server>` token (or carried in the precommit-index payload) — there
is **no** dependence on parsing a 32-hex id out of `_builds/<server_hex>`. (Whether `<server>` is the
32-lower-hex `server_id` or another canonical form is an implementation choice; the only requirement
is that watermark and precommit resolve the *same* token.)

- **Watermark** `servers/<2hex>/<server_hex>` → **`roots/<server>/_watermark`**. GC reads it *by
  computed key* (`watermarkOf` → `serverWatermarkKey`), never by listing `servers/`, so this is a
  `Cas::Layout` key change GC follows transparently — no discovery index needed, W-ANCHOR ordering
  is location-independent.
- **Precommits** (build-roots) `_builds/<server_hex>` → **`roots/<server>/_precommits`**, still
  **discovered via the relocated `gc/registry`** (build-root namespaces are CAS-registered there
  today; no separate index — see §5.3). The precommit namespace, its sharded manifests, the fenced
  fold, and the abandoned-precommit reclaim are **unchanged in protocol** — only the namespace string
  moves, and reclaim reads the owner from the shared `<server>` token (per the contract above)
  instead of the old `_builds/<server_hex>` parse.

**Dropping a server.** The operability win (G4) is that *all* of a server's mutable state is the one
subtree `roots/<server>/` (refs + `_precommits` + `_watermark` + loose files); content stays global
and deduplicated (never deleted by prefix — reclaimed by reachability). Two ways to remove it, and
the doc must not conflate them:

- **Proper drop (protocol):** enumerate the server's registered namespaces and `dropNamespace` each
  (which writes the journal `Remove` records the manifest protocol expects), then CAS-prune its
  entries from the registry (`gc/registry`) under the usual fencing. This is the in-protocol
  operation.
- **Raw subtree delete (offline/admin):** `rm roots/<server>/` is a destructive *maintenance*
  shortcut, **not** equivalent to `dropNamespace` — it bypasses the journal and the fenced index
  prune, so it can leave index/GC leftovers (stale registry/precommit entries, now-unreachable
  content). That is tolerable because **GC/introspection reports the leftovers and a repair/prune
  step reconciles them** (stale index entries pointing at vanished subtrees are pruned; orphaned
  content is reclaimed). It must be documented as offline destructive maintenance, not a normal API.

> **Scope note.** The watermark move is trivial (by-key). The precommit move is a **bounded GC-code
> edit**: relocate the build-root namespace string, update the build-root recognizer
> (`isBuildRootNamespace`) and the owning-server derivation in `reclaimAbandonedPrecommit` (read the
> shared `<server>` token), keep discovery on `gc/registry`, and **rename the code's `build`/`_builds`
> noun to `precommit`** for consistency (the verb `precommit` stays; the noun was `build-root`). The
> protocol/fence/reclaim ordering is unchanged, so this is validated by unit tests + a re-soak, not
> a TLA+ re-model. Whether this precommit move lands in *this* refactor or is deferred is the one
> open decision — see §8.

### 5.5 Navigation via the stock `clickhouse-disks` verbs {#navigation}

Use the **stock** verbs — `cd` / `list` (ls) / `read` (cat) — from the start; **no** `ca-*` commands,
**no** separate facade. The CA metadata storage already implements the `IMetadataStorage` surface
those call (`listDirectory`, `iterateDirectory`, `existsFile`/`existsDirectory`, `getFileSize`,
`getStorageObjects`/`readFile`). Once the remap (§5.1) is in place, the work is to **adjust those
existing methods** to the new layout; stock `cd`/`ls`/`read` then navigate the CA disk for free,
because the wiring already sits behind `IMetadataStorage` as the resolver.

- **Two distinct views (not identical).** Stock `clickhouse-disks` `ls`/`cd`/`read` present the
  **logical ClickHouse disk view** — `store/<u3>/<uuid>/<part>/<file>`, `shadow/…`, `detached/…`,
  with `@cas@` stripped and files reconstructed from the manifest/trees — a normal-feeling MergeTree
  disk. Raw `aws s3 ls` shows the **physical archive layout**: the same paths but with the `@cas@`
  suffix on table dirs and the manifest/protobuf objects (`<N>`, `_files/…`) inside, not the
  reconstructed files. Path-mirroring makes the two *correspond segment-for-segment* (you can read a
  raw S3 key and know exactly which logical path it backs), which is the browsability win — but they
  are deliberately different renderings (logical vs physical), not byte-identical listings.
- **Forensic decoders stay dedicated.** `fsck`/`ca-gc-dryrun` and the B169 raw-internals decoders
  (`ca-explain-delete <hash>`, manifest/snap dumps) remain separate — they decode CAS internals
  (hashes, in-degree, manifests), which are not "files."
- **Read-only for inspection** (the startup write-probe mutates otherwise) — as `CommandFsck` already
  enforces.
- **Deferred niceties:** an `[m]/[i]` mutable/immutable column in `ls`, and the CA-native *raw* view
  (servers, `@cas@`, blobs-by-hash, a cross-server `.root`) for raw-S3/FUSE consumers.

> **Mutability invariant (for the contract doc, §5.6):** a node is immutable **iff** it is
> content-addressed (has a hash). Namespaces, refs, and overlay/verbatim files have no hash →
> mutable; trees, subtrees, blobs, pack-slices have a hash → immutable.

### 5.6 Terminology stabilization {#terminology}

Light-touch; no public type renames (`BlobId`/`TreeId`/`RootNamespace`/`TreeEntry`/`Placement` are
already generic). Remove "part" from generic-layer comments/local names (`CasTreeCodec.h`,
`CasBuild.h`, `CasEvent` `ref_name` comment, `CasRootShardCodec` "per-part"); keep `ref` (documented
as "a mutable directory handle, git-style"); and write a short **VFS contract** doc (entities, the
mutability invariant, the path grammar, listing/merge semantics, what is *not* guaranteed, and an
explicit "parts/merges/projections live in the wiring" statement).

### 5.7 Why this does not touch the durability core {#fit-analysis}

- **Namespace-string only.** §5.1 changes *what string* is passed to `Cas::Layout`; the Layout key
  *construction*, the manifest/journal codec, and the GC algorithm are unchanged.
  `tryParseRootShardKey` already accepts any path-shaped namespace with a numeric tail; `@cas@` is a
  valid segment. (Respects N1–N3, N6, N7.)
- **Non-nesting invariant (critical).** Today no registered namespace is a path-prefix of another
  (`<server>/<uuid>` vs `<server>/detached/<uuid>` diverge at `detached`). The new scheme preserves
  this: `…/store/…@cas@` vs `…/detached/store/…@cas@` diverge at `store`/`detached`. This matters
  because GC enumerates a namespace's shards by listing its prefix; a nested namespace would let a
  parent's prefix-LIST sweep a child's keys. **Therefore detached parts stay a sibling namespace
  (as today), not folded under the live table** (full folding is deferred — §6 D4). (Respects N2/N3.)
- **Control-object key moves are by-key.** The registry (§5.3) and watermark (§5.4) are read/written
  *by computed key*, so relocating them is a `Cas::Layout` change the readers follow; mechanism and
  ordering are unchanged. (Respects N4/N5.)
- **Precommit relocation is namespace-string + recognizer.** The precommit namespace is
  registry-discovered; relocating it changes its string, the `_builds/`-prefix recognizer, and the
  owning-server parse — *not* the fenced fold, reclaim, or ordering. Validated by re-soak, not a
  re-model. (Respects N4/N7 — see the §5.4 scope note.)
- **Navigation** reuses the existing read-only `IMetadataStorage` methods, adjusted for the remap.
  No writes, no GC interaction. (Respects all.)
- **GC discovery stays traversal-free.** GC keeps reading the (relocated) registry; it never walks
  the tree. Longer namespace strings do not change namespace/shard counts or per-namespace listing
  cost. (Respects N2/N5.)

---

## 6. Explicitly deferred (a possible future "CAS v4") {#deferred}

Attractive ideas from the dialogue that **touch the durability core** (algorithm, manifest/journal
protocol, fence/reclaim ordering, or the object-store scheme) and so belong to a separate
brainstorm → spec → TLA+ → soak cycle:

- **D1** — Unify `blobs/`/`trees/`/`packs/` into one `objects/` content-addressed store.
- **D2** — A richer `gc/` substrate expressed as content-addressed structures: `point-in-time` roots
  for time-travel (ties to B175) and `deltas/` for map-reduce parallel GC (ties to B178). *(The
  registry stays a single global object — §5.3; this is additive substrate, not a registry rework.)*
- **D3** — Fold detached and projections into the table directory as real sub-paths (needs the GC
  shard-enumeration / nested-namespace analysis flagged in §5.7).
- **D4** — Replace shard manifests with per-path CAS-dir markers (native browsability to the
  part/file level). Re-floors GC ordering; the biggest risk.
- **D5** — Drop the mutable overlay entirely (pure git model: rewrite the tree on every change).
- **D6** — "Attach a table from the pool" with no prior local metadata (relates to BACKUP/RESTORE-on-
  CA, B16/B34); depends on D7.
- **D7** — Self-describing-pool **identity breadcrumb** (G7): an `_identity` object per CA table dir
  via the existing `_files` mechanism (`{database, table, uuid, engine, create_query}`), written on
  create/rename, read by introspection to label namespaces. Storage-side is trivial; the cost is
  plumbing the name/`CREATE` from the storage/attach layer to a `putNamespaceFile`. Tracked as
  backlog **B180**.

The dividing line for "v4": **anything that changes the GC algorithm, the manifest/journal protocol,
the fence/reclaim ordering, or the object-store scheme.** Everything in §5 stays on the safe side —
it relocates opaque namespace strings and by-key control objects, and adjusts the wiring.

---

## 7. Migration {#migration}

**None.** The feature is in active development with **no production users**, so existing POC pools
are wiped and recreated. There is no compatibility burden and no offline rename pass. This is also
why the by-key/control-object relocations in §5.3–§5.4 are affordable: nothing has to migrate.

---

## 8. Open decision {#open-decision}

One scope decision remains; everything else above is settled.

- **Does the precommit relocation (§5.4: `_builds/<server_hex>` → `roots/<server>/_precommits`,
  discovery kept on `gc/registry`, + the `build → precommit` code rename) land in this refactor, or
  is it deferred?** It is the only piece that edits GC code (a bounded recognizer/owner-derivation
  change, no protocol change) and therefore the only piece needing a re-soak. The rest of §5
  (path-mirroring, `@cas@`, eliminating `_disk`, registry → `gc/registry`, watermark → `_watermark`)
  is by-key / wiring-only. (The implementation plan isolates this as a separable final phase —
  Phase 6 — so it can be cut without disturbing the rest.)
  - **Include it now:** the layout is fully consolidated (G4 complete — one subtree per server), and
    migration is free so the re-soak is the only cost.
  - **Defer it:** this refactor then touches *zero* GC code and is provably inert w.r.t. the
    durability core; the watermark still moves (by-key), but precommits stay at `_builds/` until a
    follow-up.

*Recorded design note (not blocking):* the reason the precommit move costs any GC edit at all is
that build-root-ness and the owning-server are encoded **in the namespace string**. Carrying them in
the manifest/index **payload** instead would make any future namespace relocation free (no GC
re-parse), at the cost of a small format change — a candidate for the v4 work.
