# Content-Addressed metadata storage — source layout

This directory implements the content-addressed (CAS) object-storage metadata
storage: a "git for `MergeTree`" content-addressed pool over S3-family object
storage. The tree is **layered**: the entry points sit at the top level and the
implementation lives in per-subsystem subdirectories with a strict one-direction
include rule.

## Layer map

```
Primitives → Formats → Backend → Pool → Gc → Tools ≈ Parts → facade (top level)
```

- **`Primitives/`** — the vocabulary, zero outward dependencies: `CasTypes.h`
  (identity types: `RootNamespace`, `Token`, `BlobDigest`, `BlobRef`,
  `ManifestId`, `RefTxnId`), `CasBlobHasher`, `CasXXH3`, `CasCodecUtil`
  (identifier/varint/hex codec helpers), `CasEvent` (audit-event POD + sink),
  `CasSourceEdgeMarkers`.
- **`Formats/`** — everything persisted: bytes **and** keys. The per-object
  text/format files (`CasFormat`, `CasTextFormat`, `CasPartManifestFormat`,
  `CasRefLogFormat`, …) plus `CasLayout` (the object-key schema). See
  `Formats/README.md` for the format registry.
- **`Backend/`** — the token-aware storage seam: `CasBackend` (the contract),
  `CasObjectStorageBackend`, `CasInMemoryBackend`, `CasInstrumentedBackend`,
  `CasRequestControl`, `CasProbe`.
- **`Pool/`** — the pool engine: `CasStore` (composition root), `CasBuild`
  (one-part write transaction), `CasRefProtocol` (ref-table replay + intake),
  `CasServerRoot` (mount-claim protocol + single-writer slot + staging sweeper),
  `CasPoolMeta`, `CasBlobMeta`.
- **`Gc/`** — garbage collection: `CasGcScheduler` (pacing thread), `CasGc` (the
  round engine), `CasGcShardPlan` (sharding math), `CasBlobInDegree`,
  `CasOrphanManifestSweep`.
- **`Tools/`** — operator verbs (`clickhouse-disks`): `CasFsck`,
  `CasDecommission`, `CasInspect`.
- **`Parts/`** — part semantics over the pool: `PartPathParser` (the
  ClickHouse-path classifier), `PartFolderAccess` (`PartRefKey` + `Freshness`
  + `PartFolderValidate` + `PartFolderView` + `CachedPartFolderAccess`).
- **Top level (facade)** — the entry points: `ContentAddressedMetadataStorage`
  (the `IMetadataStorage` facade), `ContentAddressedTransaction` (the
  `IMetadataTransaction`, including the write buffers), `ContentAddressedExchange`
  (the replication seam).

## Include-direction rule

A file may include only its **own layer** and layers to its **left** in the
order above. `Tools` and `Parts` are siblings with no edges between them. This
is enforced by convention (README rule) — there is no CI check.

**Named exceptions** (deliberate):

- The staging sweeper (in `Pool/CasServerRoot`) and `probeConditionalCopy`
  bypass `Backend` and reach straight into `IObjectStorage`.
- `Backend` may read `Formats` traits (the provider-metadata mirror).

## Reading order

To understand a request end to end, read in this order:

1. `ContentAddressedMetadataStorage` — the facade / routing.
2. `Parts/PartFolderAccess` (`PartRefKey` → the folder view / cache).
3. `Pool/CasStore` — the pool composition root and `open` protocol.
4. `Pool/CasBuild` — one-part write transaction.
5. `Gc/CasGc` — the GC round engine.

> Note: `Pool/CasStore` and `Pool/CasBuild` are renamed to `CasPool` and
> `CasPartWriteTxn` in the renames phase of the source-layout refactoring; this
> reading order will be updated to the new names at that point.
