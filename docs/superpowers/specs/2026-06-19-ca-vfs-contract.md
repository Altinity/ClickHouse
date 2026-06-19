# CAS Disk — VFS Contract {#title}

- **Status:** Companion to `2026-06-19-ca-vfs-path-mapping-design.md`
- **Date:** 2026-06-19
- **Branch:** `cas-mergetree-poc`

## Entities

- **Pool** — an S3 prefix holding all content + control objects.
- **Content object** — an immutable, content-addressed, globally deduplicated object: a `blob`
  (file bytes), a `tree` (directory listing), or a `pack` (packed small files). Addressed by a
  128-bit content hash.
- **Namespace** — an *opaque string* the core never interprets; the wiring composes it to mirror the
  ClickHouse disk path, marking the content-addressed boundary with the `@cas@` suffix on the
  table-dir segment.
- **Ref** — a mutable named pointer to an immutable tree (a mutable directory handle, git-style),
  plus a small inline overlay of per-ref mutable files (`mutable_files`).
- **Verbatim file** — a plain, name-keyed mutable object. Two locations: loose in the mountpoint
  (`roots/<server>/<path>`) or inside a `@cas@` archive (`…@cas@/_files/<name>`).

## Mutability invariant

A node is **immutable if and only if it is content-addressed** (has a hash). Trees, subtrees, blobs,
and pack-slices have a hash → immutable. Namespaces, refs, and overlay/verbatim files have no hash →
mutable. `@cas@` is exactly the line between deduplicated immutable content and ordinary files.

## Path grammar

```
POOL/
  blobs/ trees/ packs/                              content (immutable, deduplicated)
  roots/                                            DATA ONLY (server mountpoints; CH paths mirrored)
    <server>/store/<u3>/<uuid>@cas@/<N>             a table archive: root-shard manifests
    <server>/store/<u3>/<uuid>@cas@/_files/<name>   that table's verbatim files
    <server>/detached/store/<u3>/<uuid>@cas@/<N>    detached parts (sibling archive)
    <server>/_precommits/<N>                        this server's in-flight precommits (Phase 6)
    <server>/_watermark                             this server's watermark (Phase 6)
    <server>/<plain path>                           loose non-CAS files (e.g. the write probe)
    shadow/<backup>/store/<u3>/<uuid>@cas@/<N>       FREEZE snapshots
  gc/
    registry                                        authoritative namespace list (GC discovery)
    precommits                                      precommit discovery index (Phase 6)
    state snap/<gen>/<shard> hb                      GC's own state
  _pool_meta
```

Reserved folders among data carry a leading underscore (`_files`, `_precommits`, `_watermark`,
`_registry`-style names) so they never collide with a real ClickHouse path segment. `@cas@` is a
suffix on a directory name, never its own folder.

## Listing / merge semantics

- Stock `clickhouse-disks` `cd`/`ls`/`read` present the **logical** ClickHouse view: `@cas@`
  stripped, files reconstructed from the manifest/trees — a normal-feeling MergeTree disk.
- Raw `aws s3 ls` shows the **physical** archive: the same paths with `@cas@` on table dirs and the
  manifest/protobuf objects (`<N>`, `_files/…`) inside. The two correspond segment-for-segment but
  are deliberately different renderings (logical vs physical), not byte-identical listings.
- Browse uses a bounded, occasional scoped LIST (loose) and re-checks `listRefs` before showing an
  entry. GC uses the compact authoritative registry (never a full LIST).

## What is NOT guaranteed

- The logical and physical listings are not byte-identical.
- A loose mountpoint object is not content-addressed (no dedup, no hardlink/rename).
- Raw subtree deletion (`rm roots/<server>/`) is destructive offline maintenance — NOT equivalent to
  `dropNamespace`; it bypasses the journal and the fenced index prune and may leave index/GC
  leftovers that a repair/prune step reconciles.

## Parts, merges, projections live in the wiring

The content-addressed store knows refs, trees, blobs, packs, and namespaces — nothing about
ClickHouse parts, merges, or projections. Part naming, the detached-part sibling-archive split, the
projection `<proj>.proj/` name prefix, and mutable-per-part file classification are all **wiring
policy** in `ContentAddressedMetadataStorage` / `ContentAddressedTransaction` / `PartPathParser`,
never in the `Cas::` core.
