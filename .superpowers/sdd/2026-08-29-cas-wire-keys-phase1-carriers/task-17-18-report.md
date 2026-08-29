# CAS wire-keys phase 1 carriers: Tasks 17–18

## Unit 0 — Tasks 15–16 review fixes

- Commit: `e8485344551` (`cas: fix wire-keys 15/16 review findings (marker contract test, condemned pin, manifest reader adoption)`).
- Files: `CasRecordStreamFormat.h`, `CasRecordStreamFormat.cpp`, `CasBlobInDegree.cpp`, `CasPartManifestFormat.cpp`, `gtest_cas_blob_indegree.cpp`, `gtest_cas_encoding_pins.cpp`, `gtest_cas_record_stream_format.cpp`.
- Gate: `build/build_wirekeys_fix1516.log` passed; `build/test_cas_fix1516.log` passed, 2202 tests.
- Updated expectations: `SourceEdgeRunLines` grew from header + `edge` + trailer (`n=1`) to header + `edge` + `condemned` + trailer (`n=2`), pinning `pend`/`tt`/`tv`/`sz`/`cr`/`mc`; no persisted spelling changed. The condemned-row marker test comment now names its decoder-only scope. No other error-message expectations surfaced.
- Deviations: none.

## Task 17 — `cas_fold_seal`

- Commit: `b543a035b77` (`cas: fold-seal codec onto carriers and the HoldReason table`).
- Files: `CasFoldSealFormat.cpp`.
- Gate: `build/build_wirekeys_task17.log` passed; `build/test_cas_task17.log` passed, 2202 tests.
- Updated expectations: none; fold-seal goldens and hold-grammar expectations remained byte-identical.
- Deviations: none.

## Task 18 — `cas_gc_outcomes`

- Commit: `e504f4dd530` (`cas: gc-outcomes codec onto carriers; requiredness unchanged`).
- Files: `CasGcOutcomesFormat.cpp`, `CasGcOutcomesFormat.h`.
- Gate: `build/build_wirekeys_task18.log` passed; `build/test_cas_task18.log` passed, 2202 tests.
- Updated expectations: none; outcome-log bytes remained byte-identical. Requiredness remains `ha`/`h`/`tt`, with missing `tv` decoded as an empty value.
- Deviations: none.

## EXTRA — private `RefTxnId` writer

- Commit: `cas: make the string_view RefTxnId writer private`.
- Files: `CasRefWireVocab.h`, `CasRefWireVocab.cpp`.
- Gate: `build/build_wirekeys_task18x.log` passed; `build/test_cas_task18x.log` passed.
- Updated expectations: none.
- Deviations: none. A tree-wide caller check found no codec caller of the removed `string_view` overload.
