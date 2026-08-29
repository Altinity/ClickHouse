# CAS wire-keys phase 1 — carriers report

## Task 12 — `cas_ref_snap`

- Status: PASS
- Files: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp`; this report.
- Gate: `2199 tests from 284 test suites ran`; `2199 tests` passed, `0` failed.
- Updated test expectations (old → new): none. The adopted shared `ManifestRefFields::buildRef` wording changes an unpinned malformed manifest-field-group message only.
- Deviations: none.

## Task 13 — `cas_ref_ckpt`

- Status: PASS
- Files: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.cpp`; `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.h`; `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.cpp`; `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp`; this report.
- Gate: build PASS; `2199 tests from 284 test suites ran`; `2199 tests` passed, `0` failed.
- Updated test expectations (old → new): none.
- Deviations: none.

## Task 14 — `cas_ref_catalog` + `NsState` table

- Status: PASS
- Files: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.cpp`; `src/Disks/tests/gtest_cas_ref_catalog.cpp`; this report.
- Gate: build PASS; `2199 tests from 284 test suites ran`; `2199 tests` passed, `0` failed.
- Updated test expectations (old → new): `CASRefCatalogFormatDeathTest.NsStateToWordRaisesLogicalErrorOnImpossibleValueAborts`: `unknown ns state` → `outside the wire vocabulary` (sanctioned unreachable encoder branch).
- Deviations: controller ruling accepts the existing commit metadata.

## Fix round 1

- Retired the final codec call site using the `string_view` `writeRefTxnIdFields` overload, pinned
  the `st:"removing"` catalog word, and used the shared `NsState` table diagnostic directly.

Fix-round certification note (controller): the fix round was certified by TWO gates — release
`build/test_cas_task12_14_fix1.log` (2199/2199, filter CAS*) and the debug FULL gate
`build_debug/test_cas_task12_14_fix1.log` (2204 tests from 304 suites, PASSED, including
`CASRefCatalogFormatDeathTest.NsStateToWordRaisesLogicalErrorOnImpossibleValueAborts` [OK]) — the
debug gate is the one that exercises the changed death expectation.
