---
id: DRAFT-53
title: Decide whether blob upload re-hashes the local scratch bytes it sends
status: Draft
assignee: []
created_date: '2026-09-26 12:54'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2031'
documentation:
  - docs/superpowers/cas/2031-triage.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
With local staging, a blob's digest is computed while it is written to the scratch file. The upload then reads that file back and sends it under the digest key. "The core otherwise never re-hashes payloads" (`R/Pool/CasPartWriteTxn.cpp:101-105`).
A length-preserving divergence of the scratch file between write and upload (a local-disk bit flip or a foreign writer in `scratch_path`) would publish wrong bytes under the right key. Every later dedup would then adopt that body.
The neighbouring guards the 2026-08-21 triage relied on are now in place: size check at adoption (`R/Pool/CasPartWriteTxn.cpp:408-422`), exact blob HEAD instead of the presence cache (`907c3b5ce7d`), and atomic emulated-backend install (`emuPublishBlobAtomically`). They catch length changes and non-atomic writes, not same-length corruption.
Candidate fix: hash the bytes as the upload source reads them and refuse the publish on mismatch. That costs one extra CityHash128/XXH3 pass over bytes already read, and no extra I/O.
Rejected alternative: re-hash on read (CAS-008, by design). Deciding factors: CPU cost on large merges versus the local-disk failure rate.

Provenance: docs/superpowers/cas/2031-triage.md#cas-009 (2031-triage CAS-009). The ledger's cited tracking items are implemented (u09 size guard, u14 [disk-error-audit], [dedup-presence-only-window-recheck]), leaving this residual untracked. Draft: no observation, value undecided. Verified 2026-09-26 against cae9288ee65 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records whether upload-time verification is added, with its measured CPU cost on a large merge
- [ ] #2 If added: a gtest that corrupts one byte of a scratch file between write and upload sees the publish refused with `CORRUPTED_DATA` and nothing published
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
