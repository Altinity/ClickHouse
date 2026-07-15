# CAS persisted formats — the living registry

Every persisted CAS object is a text file: header line `{"type":"cas_<object>","v":N}`, body
(one JSON object / sorted NDJSON records / raw payload zone), optional `{"n":…}` trailer.
Can-grow-large types are stored under a **`.zst` key suffix** and are ALWAYS one zstd frame
(checksum on; declared content size checked against the cap before allocation); always-small and
deterministic types are raw. `CasTextFormat.{h,cpp}` is the only code that knows this
shape. Design: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md`; reference:
`docs/superpowers/cas/codecs_proposal_v3.md`.

**Rule:** any change to a persisted format lands in the SAME commit as its row here.

## Bucket map

| Key (under the pool prefix) | Object | Codec | Writer |
|---|---|---|---|
| `_pool_meta` | pool identity + floors | `CasPoolMetaFormat` | pool create/admit |
| `cas/refs/<ns>/…_log` | ref transaction log | `CasRefLogFormat`* | writer commit path |
| `cas/refs/<ns>/…_snap` | complete ref table | `CasRefSnapshotFormat`* | writer/GC fold |
| `cas/refs/<ns>/…` cleanup marker | key-only presence marker (empty body) | — | GC |
| `cas/manifests/<ns>/<epoch>/<seq>/<ordinal>` | part manifest | `CasPartManifestFormat`* | part build |
| blob keys (`CasLayout::blobKey`) | blob envelope + payload | `CasBlobEnvelopeFormat`* | uploads |
| blob-meta keys (`CasLayout::blobMetaKey`) | freshness sidecar | `CasBlobMetaFormat`* | dedup/GC |
| `gc/state`, `gc/hb` | GC state / leader heartbeat | `CasGcStateFormat` | GC |
| `gc/gen/<g>/attempt/<a>/outcomes/…​.zst` | outcome log | `CasGcOutcomesFormat` (`.zst`) | GC |
| `gc/gen/<g>/attempt/<a>/fold_seal` | fold seal (deterministic) | `CasFoldSealFormat` | GC |
| `gc/gen/<g>/…​/runs` | GC source-edge record-stream runs | `CasRecordStreamFormat` | GC |
| `gc/server-roots/<srid>/{owner,epoch,mount}` | server-root singletons | `CasServerRootFormats` | mount |
| `roots/…` | raw passthrough (verbatim) | — (never interpreted) | upper layers |

`*` = still the legacy binary codec; the row flips as each phase of the migration lands. **Phase 2
(control plane) is DONE**: `cas_pool_meta`, `cas_gc_state` + `cas_gc_hb`, `cas_gc_outcomes`,
`cas_fold_seal`, and `cas_owner`/`cas_epoch`/`cas_mount_lease` are text, and the protobuf codecs +
the CA protobuf build target are removed. **Phase 5 (runs) is DONE**: the GC source-edge data plane
(`cas_run`) is sorted NDJSON via `CasRecordStreamFormat` (streamed, no seek), integrity is the
whole-file seal-checksum, and the part-manifest cleanup run + `RunFileReader::seek` + `RunMerger` are
gone — `CasRunFile` survives only as the phase-6-owned embedded part-manifest (`ManifestEntries`)
codec. Remaining `*` rows = phases 3/4/6/7 (refsnaplog, blob meta, part manifest, blob envelope).

## Codec table

Authoritative per-format traits (type string, family, strictness, compression policy, caps) live
in `CasFormat.cpp` (`TRAITS`), asserted complete by `gtest_cas_text_format.cpp`. Key naming: keys
2–5 chars; hashes = 32-char lowercase hex strings; unbounded u64 = decimal strings; bounded
counts/lengths/ms-timestamps = numbers; units documented here per object as codecs land.

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
