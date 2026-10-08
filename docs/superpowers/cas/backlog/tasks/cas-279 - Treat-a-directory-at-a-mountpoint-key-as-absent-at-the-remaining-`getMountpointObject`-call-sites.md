---
id: CAS-279
title: >-
  Treat a directory at a mountpoint key as absent at the remaining
  `getMountpointObject` call sites
status: To Do
assignee: []
created_date: '2026-07-23'
updated_date: '2026-09-26 14:38'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - utils/ca-soak/scenarios/BACKLOG.md
priority: low
type: bug
ordinal: 346000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On the local backend `getMountpointObject` opens a pool subdirectory as a file and throws `CANNOT_READ_FROM_FILE_DESCRIPTOR`
("Is a directory"). `99b244a9444` fixed `existsFile` and `getStorageObjects` via `Store::mountpointObjectExists`
(`CA/ContentAddressedMetadataStorage.cpp:1509`, `:1970`). Still unguarded: `CA/ContentAddressedTransaction.cpp:880`, `:1524`,
`:1527`, `:1693` and `CA/ContentAddressedMetadataStorage.cpp:1749`. S3 has no directories, but RustFS on a filesystem may.

Provenance: BACKLOG/formats-and-storage.md [STATELESS-04286 EISDIR]; scenario entry STATELESS-04286-getmountpoint-eisdir. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each remaining call site either treats a directory as absent or fails closed with a named error; the choice is recorded per site
- [ ] #2 `04286_content_addressed_remote_data_paths` and a RustFS run of the same probe pass
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
First recorded: 2026-07-23 (1989155d7e4, by 'STATELESS-04286 EISDIR')

Merged from u19b-soak (merge into CAS-279): Soak ledger entry STATELESS-04286-getmountpoint-eisdir is already carried by CAS-279; no new facts.
<!-- SECTION:NOTES:END -->
