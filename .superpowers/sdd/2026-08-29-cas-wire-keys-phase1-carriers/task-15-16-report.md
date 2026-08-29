# CAS wire-keys phase 1 carriers: Tasks 15–16

## Task 15 — `cas_part_manifest`

- Status: complete.
- Files: `Formats/CasPartManifestFormat.h`, `Formats/CasPartManifestFormat.cpp`,
  `Formats/CasTextFormat.h`, `Formats/CasTextFormat.cpp`, `Formats/CasWireVocab.cpp`, and this report.
- Gate: `build/build_wirekeys_task15.log` PASS; `build/test_cas_task15.log` PASS — 2199/2199
  `CAS*` tests passed, 0 failed.
- Updated expectations (old → new): none. The adopted blob-reference group validation changes its
  diagnostic through `BlobRefFields::build`, but no existing test asserted the old text.
- Deviations: none. The `isLowercaseHexChar` extraction preserves lowercase-hex acceptance exactly.

## Task 16 — `cas_run`, `RunMarker`, and `RunRef::key_generation`

- Status: complete.
- Files: `Formats/CasRecordStreamFormat.h`, `Formats/CasRecordStreamFormat.cpp`,
  `Formats/CasFoldSealFormat.h`, `Formats/CasFoldSealFormat.cpp`,
  `Formats/CasRefCatalogFormat.cpp`, `Gc/CasBlobInDegree.h`,
  `Gc/CasBlobInDegree.cpp`, `Gc/CasGc.cpp`, `Tools/CasFsck.cpp`,
  `Tools/CasInspect.cpp`, the CAS test helpers and affected CAS test suites, and this report.
- Gate: `build/build_wirekeys_task16.log` PASS; `build/test_cas_task16.log` PASS — 2200/2200
  `CAS*` tests passed, 0 failed.
- Marker-byte boundary conversions: `CasBlobInDegree.cpp` now uses `runMarkerFromByte` for
  prior-run rows, condemned-row decoding, and zero-marker scanning; it uses `runMarkerByte` for
  condemned-row encoding and edge/zero payload emission. `CasGc.cpp`, `CasFsck.cpp`, and
  `CasInspect.cpp` use `runMarkerFromByte` for persisted payload reads. The test helpers and
  sealed-run test readers use `runMarkerFromByte`; the condemned-row byte assertion uses
  `runMarkerByte`. No bare `static_cast<RunMarker>` remains outside the checked helper.
- Updated expectations (old → new): source-edge marker assertions and fixtures use
  `kEdgeActive`/`kZeroMarker`/`kCondemned` → `RunMarker::Edge`/`RunMarker::Zero`/
  `RunMarker::Condemned`; raw payload comparisons use the checked byte helpers. Run-reference
  assertions and fixtures use `RunRef::generation` → `RunRef::key_generation`. Added
  `CASCondemnedRow.UnknownMarkerByteFailsClosedWithCorruptedData`: payload byte `0x03` →
  `CORRUPTED_DATA`. The persisted fold-seal key remains `"gen"`.
- Removed the unreachable `default` branch from the exhaustive `RunMarker` payload-encoding
  switch; all values admitted from persisted bytes first pass `runMarkerFromByte`.
- Deviations: none. `CASEncodingPins.SourceEdgeRunLines` remains byte-identical.
