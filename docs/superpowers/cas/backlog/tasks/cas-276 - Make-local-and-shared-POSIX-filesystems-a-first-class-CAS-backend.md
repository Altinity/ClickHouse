---
id: CAS-276
title: Make local and shared POSIX filesystems a first-class CAS backend
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:backend'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
milestone: m-6
dependencies: []
documentation:
  - docs/superpowers/specs/2026-09-08-cas-posix-shared-backend-design.md
priority: medium
type: feature
ordinal: 341000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Today `object_storage_type = local` routes to `ObjectStorageBackend::Mode::EmulatedSingleProcess`: correct for one process,
racy for several writers on local or NFS storage (put-if-absent is not atomic across processes), and control objects are
rewritten in place with `O_TRUNC`. Roadmap §1 lists "CAS on POSIX-compatible network disks".
The coordinator-free design `docs/superpowers/specs/2026-09-08-cas-posix-shared-backend-design.md` (revision 10) implements the
full backend contract from `link`/`rename`/`unlink` and replaces `EmulatedSingleProcess` with `Mode::Posix`. It has no
implementation plan, and no operator doc states today's limits.

Provenance: BACKLOG/formats-and-storage.md [B26 / B135]; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Both subtasks are done
- [ ] #2 The emulated-mode defects (`emulated-mutable-atomic-install`, `emu-token-expiry-monotonic`, `local-mountpoint-eisdir`) are closed or retired by `Mode::Posix`
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'B26 / B135')
<!-- SECTION:NOTES:END -->
