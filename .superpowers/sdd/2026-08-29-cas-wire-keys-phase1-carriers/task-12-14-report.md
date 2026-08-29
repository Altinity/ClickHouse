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
