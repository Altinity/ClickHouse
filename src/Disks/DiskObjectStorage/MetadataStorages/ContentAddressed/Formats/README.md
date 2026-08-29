# CAS persisted formats — the living registry

Every persisted CAS object is a text file: header line `{"type":"cas_<object>","v":N}`, body
(one JSON object / sorted NDJSON records / raw payload zone), optional `{"n":…}` trailer.
Can-grow-large types are stored under a **`.zst` key suffix** and are ALWAYS one zstd frame
(checksum on; declared content size checked against the cap before allocation); always-small and
deterministic types are raw. `CasTextFormat.{h,cpp}` is the only code that knows this shape.

The object inventory is text end to end — there are no binary CAS formats and no protobuf
dependency. The GC source-edge data plane (`cas_run`) is sorted NDJSON written and read as a
stream (no seek); its integrity check is the whole-file seal checksum. The part manifest is the
one `PayloadHybrid` object: text header + descriptor meta + sorted entry records + `{"n":…}`
trailer, followed by a banner-framed raw payload zone for inline file bytes.

**Rule:** any change to a persisted format lands in the SAME commit as its row here.

## Bucket map

| Key (under the pool prefix) | Object | Codec | Writer |
|---|---|---|---|
| `_pool_meta` | pool identity + floors (`pool_id`, `blob_header_len`, `gc_shards`, `min_reader_generation`, `algos_used` array) | `CasPoolMetaFormat` | pool create/admit |
| `cas/ns/stream/<life_id>/_log/…​.zst` | ref transaction log | `CasRefLogFormat` (`.zst`) | writer commit path |
| `cas/ns/stream/<life_id>/_snap/…​.zst` | complete ref table | `CasRefSnapshotFormat` (`.zst`) | writer/GC fold |
| `cas/ns/state/<life_id>/_ckpt` | mutable life checkpoint | `CasRefCkptFormat` | writer/GC fold |
| `cas/ns/state/<life_id>/_files/…​` | namespace-owned raw files | — | upper layers |
| `cas/manifests/<ns>/<epoch-hex>-<seq-hex>/<ordinal>.zst` | part manifest | `CasPartManifestFormat` | part build |
| blob keys (`CasLayout::blobKey`) | blob envelope + payload | `CasBlobEnvelopeFormat` | uploads |
| blob-meta keys (`CasLayout::blobMetaKey`) | freshness sidecar (`state`, `condemn_round`, `size`) | `CasBlobMetaFormat` | dedup/GC |
| `gc/state`, `gc/hb` | GC state (`round`, `gc_shards`, `snap_generation`, `snap_pruned_through`, `snap_attempt`, `manifest_sweep_cursor`, `lease_owner`, `lease_seq`) / heartbeat (`owner`, `hb_seq`) | `CasGcStateFormat` | GC |
| `gc/maintenance_state` | leak-only namespace-janitor cursor (`janitor_cursor`) | `CasGcMaintenanceStateFormat` | future janitor |
| `gc/gen/<g>/attempt/<a>/outcomes/…​.zst` | outcome log | `CasGcOutcomesFormat` (`.zst`) | GC |
| `gc/gen/<g>/attempt/<a>/fold_seal` | fold seal (deterministic) | `CasFoldSealFormat` | GC |
| `gc/gen/<g>/…​/runs` | GC source-edge record-stream runs | `CasRecordStreamFormat` | GC |
| `gc/server-roots/<srid>/{owner,epoch,mount}` | server-root singletons (`server_uuid`, optional `retired_at_ms`; `next_writer_epoch`; `server_uuid`, `writer_epoch`, `hostname`, `pid`, `started_at_ms`, `seq`, `expires_at_ms`, `min_active_build_sequence`, `gc_fenced`, `write_attempt_id`) | `CasServerRootFormats` | mount |
| `roots/…` | raw passthrough (verbatim) | — (never interpreted) | upper layers |

## Codec table

Authoritative per-format traits (type string, family, strictness, compression policy, caps) live
in `CasFormat.cpp` (`TRAITS`), asserted complete by `gtest_cas_text_format.cpp`. Key naming: keys
2–5 chars; fixed-width `UInt128` identities = 32-char lowercase hex strings; blob digests =
algo-width hex (two chars per digest byte), rendered with their algo name (`sha256:ab12…`) wherever
a bare hex would be ambiguous; unbounded u64 = decimal strings; bounded counts/lengths/ms-timestamps
= numbers; units documented here per object as codecs land.

`CasWireVocab.{h,cpp}` owns repeated value fields: `BlobRef` uses `algo`/`digest`, `Token` uses
`token_type`/`token`, `ManifestRef` uses `epoch`/`build`/`ord`, and owner-transition bindings use
the corresponding `old_*` and `new_*` key bundles.

## Evolution rules (one screen)

- `v` (header line) is the ONLY version field; reader gate: `v > G_BUILD` →
  `UNKNOWN_FORMAT_VERSION`, checked before the body.
- Additive change = new tolerant key, no `v` bump; on MUTABLE objects the field is best-effort
  until the pool floor rises (an old writer's fresh re-encode drops it).
- Breaking change = `v` bump + `changePoints` + write-down-to-floor; the floor raise is what
  fences old builds out (mount gates: `min_reader_generation` forward, pool-meta `v` backward).
- Deterministic formats (`cas_fold_seal`, `cas_run`): strict keys, pinned raw, and the adoption
  pin — on a `putDeterministicArtifact` conflict, re-encode at the `v` of the EXISTING object.
- A key prefixed `!` is critical: a reader that does not understand it fails closed.
- Padding zones (blob header pad, manifest banners) are deterministic and verified — no
  unaccounted bytes in any object.
- `openObject` policy asymmetry: a compressed body under a raw-compression policy is rejected
  (`CORRUPTED_DATA`), but a NON-zstd body under an `Always` policy is passed through verbatim as
  canonical text — an intentional uncompressed-repair path, not an "`Always` ⇒ must be compressed"
  enforcement. In practice the mismatch never arises: `Always` objects are read via a constructed
  `.zst`-suffixed key, so a raw body is not GETtable at that key.

## Generation history {#generation-history}

The format's generation history was reset to a flat `{1, 1}` baseline (`G_BUILD == 1`): CAS is
pre-release, carries no persisted data, and pays no compatibility cost for starting the count over.
Every class's `changePoints` begins at generation 1; a future breaking change appends a real entry to
that class's own array and bumps `G_BUILD`, the same way it always has.
