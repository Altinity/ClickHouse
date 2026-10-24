---
id: CAS-280
title: Check the pinned RustFS image against the upstream fix for rustfs#3231
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 14:38'
labels:
  - 'area:soak'
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:soak'
dependencies: []
references:
  - utils/ca-soak
  - 'https://github.com/rustfs/rustfs/issues/3231'
priority: low
type: task
ordinal: 347000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
RustFS returned false 404s under load and kept overwritten versions (rustfs#3231), which capped merge-heavy full-scale runs
and the 4 h chaos soak. CAS is safe against it (clamp plus destruction suppression). Upstream reports it fixed, but the ca-soak
configs still pin `rustfs/rustfs:1.0.0-rc.3` (ten pins under `utils/ca-soak`), never checked against the fix.
The ~400x physical-footprint figure blamed on it holds only for many-tiny-object soaks; do not generalise it.

Provenance: BACKLOG/formats-and-storage.md [F2 / rustfs#3231]; absorbs u06's dropped [physical-footprint amplification]. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The pinned version is compared with the version that carries the fix, and the result is recorded in `RUN_HISTORY.md`
- [ ] #2 If a fixed image exists, the pins move to it and one full-scale merge-heavy soak runs clean
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'F2 / rustfs#3231')

Merged from u19b-soak (merge into CAS-280): While the pinned RustFS still retains overwritten versions, long chaos soaks need the orphan reaper. Launched as `docker exec … orphan_reaper.sh` it dies on the first chaos RustFS restart, after which the leak grows until the disk watchdog stops the run. The fix recorded on 2026-07-09 (a host-side loop calling `orphan_reaper.sh <dir> --once` every 120 s) was applied by hand once and never wired: `utils/ca-soak/scripts/run_24h.sh` does not launch the reaper at all. If a fixed RustFS image removes the leak, retire the reaper instead.
<!-- SECTION:NOTES:END -->
